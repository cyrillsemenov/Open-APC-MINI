#include <stdbool.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include "faders.h"

static volatile uint16_t s_adc_dma_buf[APC_FADER_COUNT];
static uint16_t s_hist[APC_FADER_COUNT][APC_ADC_WINDOW_SAMPLES];
static uint8_t s_cc_val[APC_FADER_COUNT];
static uint8_t s_ring_idx = 0;
static uint8_t s_warm_samples = 0;
static uint16_t s_stall_guard = 0;

static void adc_dma_hw_init(void) {
    static const uint8_t k_adc_channels[APC_FADER_COUNT] = {0, 1, 2, 3, 4, 5, 6, 7, 14};

    rcc_periph_clock_enable(RCC_DMA1);
    rcc_periph_clock_enable(RCC_ADC1);

    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_ANALOG, APC_FADER_GPIOA_MASK);
    gpio_set_mode(GPIOC, GPIO_MODE_INPUT, GPIO_CNF_INPUT_ANALOG, APC_FADER_MASTER_PIN);

    // DMA1_Channel1: circular 9 half-words from ADC1_DR to s_adc_dma_buf[9]
    dma_channel_reset(DMA1, DMA_CHANNEL1);
    dma_set_peripheral_address(DMA1, DMA_CHANNEL1, (uint32_t)&ADC_DR(ADC1));
    dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)s_adc_dma_buf);
    dma_set_read_from_peripheral(DMA1, DMA_CHANNEL1);
    dma_set_number_of_data(DMA1, DMA_CHANNEL1, APC_FADER_COUNT);
    dma_disable_peripheral_increment_mode(DMA1, DMA_CHANNEL1);
    dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL1);
    dma_set_peripheral_size(DMA1, DMA_CHANNEL1, DMA_CCR_PSIZE_16BIT);
    dma_set_memory_size(DMA1, DMA_CHANNEL1, DMA_CCR_MSIZE_16BIT);
    dma_enable_circular_mode(DMA1, DMA_CHANNEL1);
    dma_set_priority(DMA1, DMA_CHANNEL1, DMA_CCR_PL_HIGH);
    dma_enable_channel(DMA1, DMA_CHANNEL1);

    // ADC1: independent, scan, single-shot SWSTART, right-aligned, 9 regular channels @ 71.5 cycles
    adc_power_off(ADC1);
    adc_set_dual_mode(ADC_CR1_DUALMOD_IND);
    adc_enable_scan_mode(ADC1);
    adc_set_single_conversion_mode(ADC1);
    adc_set_right_aligned(ADC1);
    adc_enable_external_trigger_regular(ADC1, ADC_CR2_EXTSEL_SWSTART);
    adc_set_sample_time_on_all_channels(ADC1, ADC_SMPR_SMP_71DOT5CYC);
    adc_set_regular_sequence(ADC1, APC_FADER_COUNT, (uint8_t *)k_adc_channels);
    adc_enable_dma(ADC1);
    adc_power_on(ADC1);

    // Wait >= t_STAB (~3 us at 48 MHz, >= 2 ADCCLK cycles) before starting ADC calibration
    for (volatile uint16_t d = 0; d < 160U; d++) {}

    adc_reset_calibration(ADC1);
    adc_calibrate(ADC1);

    dma_clear_interrupt_flags(DMA1, DMA_CHANNEL1, DMA_GIF);
    adc_start_conversion_regular(ADC1);
}

static uint8_t adc_to_cc_nearest(uint16_t avg) {
    if (avg <= APC_ADC_ZERO_MAX) {
        return 0;
    }
    if (avg >= APC_ADC_FULL_MIN) {
        return 127U;
    }
    uint32_t num =
        ((uint32_t)(avg - APC_ADC_ZERO_MAX) * 127UL) + ((uint32_t)APC_ADC_SCALE_SPAN / 2UL);
    return (uint8_t)(num / (uint32_t)APC_ADC_SCALE_SPAN);
}

static uint8_t quantize_cc_hysteresis(uint16_t avg, uint8_t cur_cc) {
    uint8_t raw_cc = adc_to_cc_nearest(avg);
    if (raw_cc == cur_cc) {
        return cur_cc;
    }

    if (raw_cc == (uint8_t)(cur_cc + 1U)) {
        if (avg >= APC_ADC_FULL_MIN) {
            return 127U;
        }
        uint16_t shifted = (avg > APC_ADC_HYSTERESIS) ? (uint16_t)(avg - APC_ADC_HYSTERESIS) : 0U;
        return (adc_to_cc_nearest(shifted) > cur_cc) ? raw_cc : cur_cc;
    }

    if ((uint8_t)(raw_cc + 1U) == cur_cc) {
        if (avg <= APC_ADC_ZERO_MAX) {
            return 0;
        }
        uint16_t shifted = (uint16_t)(avg + APC_ADC_HYSTERESIS);
        return (adc_to_cc_nearest(shifted) < cur_cc) ? raw_cc : cur_cc;
    }

    return raw_cc;
}

static bool harvest_dma_transfer(void) {
    if (!dma_get_interrupt_flag(DMA1, DMA_CHANNEL1, DMA_TCIF)) {
        if (++s_stall_guard > 5000U) {
            s_stall_guard = 0;
            dma_disable_channel(DMA1, DMA_CHANNEL1);
            dma_set_number_of_data(DMA1, DMA_CHANNEL1, APC_FADER_COUNT);
            dma_clear_interrupt_flags(DMA1, DMA_CHANNEL1, DMA_GIF);
            dma_enable_channel(DMA1, DMA_CHANNEL1);
            adc_start_conversion_regular(ADC1);
        }
        return false;
    }
    s_stall_guard = 0;

    for (uint8_t i = 0; i < APC_FADER_COUNT; i++) {
        s_hist[i][s_ring_idx] = s_adc_dma_buf[i];
    }

    s_ring_idx++;
    if (s_ring_idx >= APC_ADC_WINDOW_SAMPLES) {
        s_ring_idx = 0;
    }
    if (s_warm_samples < APC_ADC_WINDOW_SAMPLES) {
        s_warm_samples++;
    }

    dma_clear_interrupt_flags(DMA1, DMA_CHANNEL1, DMA_TCIF);
    adc_start_conversion_regular(ADC1);
    return (s_warm_samples >= APC_ADC_WINDOW_SAMPLES);
}

void faders_init(void) {
    for (uint8_t i = 0; i < APC_FADER_COUNT; i++) {
        s_adc_dma_buf[i] = 0;
        s_cc_val[i] = 0;
        for (uint8_t s = 0; s < APC_ADC_WINDOW_SAMPLES; s++) {
            s_hist[i][s] = 0;
        }
    }
    s_ring_idx = 0;
    s_warm_samples = 0;
    s_stall_guard = 0;

    adc_dma_hw_init();
}

void faders_task(faders_change_fn on_change) {
    if (!harvest_dma_transfer()) {
        return;
    }

    for (uint8_t i = 0; i < APC_FADER_COUNT; i++) {
        uint16_t s0 = s_hist[i][0];
        uint16_t s1 = s_hist[i][1];
        uint16_t s2 = s_hist[i][2];

        uint16_t d01 = (s0 >= s1) ? (uint16_t)(s0 - s1) : (uint16_t)(s1 - s0);
        if (d01 >= APC_ADC_OUTLIER_DELTA) {
            continue;
        }

        uint16_t d12 = (s1 >= s2) ? (uint16_t)(s1 - s2) : (uint16_t)(s2 - s1);
        if (d12 >= APC_ADC_OUTLIER_DELTA) {
            continue;
        }

        uint16_t d02 = (s0 >= s2) ? (uint16_t)(s0 - s2) : (uint16_t)(s2 - s0);
        if (d02 >= APC_ADC_OUTLIER_DELTA) {
            continue;
        }

        uint16_t avg;
        if (d01 > d12) {
            avg = (d12 > d02) ? (uint16_t)((s0 + s2) / 2U) : (uint16_t)((s1 + s2) / 2U);
        } else {
            avg = (d01 > d02) ? (uint16_t)((s0 + s2) / 2U) : (uint16_t)((s0 + s1) / 2U);
        }

        uint8_t new_cc = quantize_cc_hysteresis(avg, s_cc_val[i]);
        if (new_cc == s_cc_val[i]) {
            continue;
        }

        s_cc_val[i] = new_cc;
        if (on_change != 0) {
            on_change(i, new_cc);
        }
    }
}

void faders_get_values(uint8_t out_values[APC_FADER_COUNT]) {
    for (uint8_t i = 0; i < APC_FADER_COUNT; i++) {
        out_values[i] = s_cc_val[i];
    }
}
