#include <stdbool.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include "faders.h"

static volatile uint16_t s_adc_dma_buf[APC_FADER_COUNT];
static uint16_t s_hist[APC_FADER_COUNT][APC_ADC_WINDOW_SAMPLES];
static int32_t s_ema_q4[APC_FADER_COUNT];
static uint16_t s_hires_val[APC_FADER_COUNT];
static uint8_t s_cc_val[APC_FADER_COUNT];
static uint8_t s_ring_idx = 0;
static uint8_t s_warm_samples = 0;
static uint16_t s_stall_guard = 0;

static void adc_trigger_swstart(void) {
    ADC_CR2(ADC1) |= ADC_CR2_SWSTART;
    while ((ADC_CR2(ADC1) & ADC_CR2_SWSTART) != 0UL) {}
}

static void adc_dma_hw_init(void) {
    rcc_periph_clock_enable(RCC_DMA1);
    rcc_periph_clock_enable(RCC_ADC1);

    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_ANALOG, APC_FADER_GPIOA_MASK);
    gpio_set_mode(GPIOC, GPIO_MODE_INPUT, GPIO_CNF_INPUT_ANALOG, APC_FADER_MASTER_PIN);

    // DMA1_Channel1: circular 9 half-words from ADC1_DR to s_adc_dma_buf[9]
    DMA_CCR(DMA1, DMA_CHANNEL1) = 0UL;
    DMA_IFCR(DMA1) = DMA_IFCR_CGIF1;
    DMA_CNDTR(DMA1, DMA_CHANNEL1) = APC_FADER_COUNT;
    DMA_CPAR(DMA1, DMA_CHANNEL1) = (uint32_t)&ADC_DR(ADC1);
    DMA_CMAR(DMA1, DMA_CHANNEL1) = (uint32_t)s_adc_dma_buf;
    DMA_CCR(DMA1, DMA_CHANNEL1) = DMA_CCR_PL_HIGH | DMA_CCR_MSIZE_16BIT | DMA_CCR_PSIZE_16BIT |
                                  DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_EN;

    // ADC1: independent, scan, single-shot SWSTART, right-aligned, 9 regular channels @ 71.5 cycles
    ADC_CR2(ADC1) = 0UL;
    ADC_CR1(ADC1) = ADC_CR1_SCAN;
    ADC_SMPR1(ADC1) = 0x00DB6DB6UL; // Ch 10..17 @ 71.5 cycles (0b110 per 3-bit slot)
    ADC_SMPR2(ADC1) = 0x36DB6DB6UL; // Ch 0..9 @ 71.5 cycles
    ADC_SQR3(ADC1) = (0UL << 0U) | (1UL << 5U) | (2UL << 10U) | (3UL << 15U) | (4UL << 20U) |
                     (5UL << 25U);
    ADC_SQR2(ADC1) = (6UL << 0U) | (7UL << 5U) | (14UL << 10U);
    ADC_SQR1(ADC1) = ((uint32_t)(APC_FADER_COUNT - 1U) << ADC_SQR1_L_LSB);
    ADC_CR2(ADC1) = ADC_CR2_EXTTRIG | ADC_CR2_EXTSEL_SWSTART | ADC_CR2_DMA;
    ADC_CR2(ADC1) |= ADC_CR2_ADON;

    // Wait >= t_STAB (~3 us at 48 MHz, >= 2 ADCCLK cycles) before starting ADC calibration
    for (volatile uint16_t d = 0; d < 160U; d++) {}

    ADC_CR2(ADC1) |= ADC_CR2_RSTCAL;
    while ((ADC_CR2(ADC1) & ADC_CR2_RSTCAL) != 0UL) {}
    ADC_CR2(ADC1) |= ADC_CR2_CAL;
    while ((ADC_CR2(ADC1) & ADC_CR2_CAL) != 0UL) {}

    DMA_IFCR(DMA1) = DMA_IFCR_CGIF1;
    adc_trigger_swstart();
}

static uint16_t adc_scale(uint16_t avg, uint16_t max_out) {
    if (avg <= APC_ADC_ZERO_MAX) {
        return 0U;
    }
    if (avg >= APC_ADC_FULL_MIN) {
        return max_out;
    }
    uint32_t num = ((uint32_t)(avg - APC_ADC_ZERO_MAX) * (uint32_t)max_out) +
                   ((uint32_t)APC_ADC_SCALE_SPAN / 2UL);
    return (uint16_t)(num / (uint32_t)APC_ADC_SCALE_SPAN);
}

static uint16_t adc_to_hires_14bit(uint16_t avg) {
    return adc_scale(avg, APC_FADER_HIRES_MAX);
}

static uint8_t adc_to_cc_nearest(uint16_t avg) {
    return (uint8_t)adc_scale(avg, 127U);
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
    if ((DMA_ISR(DMA1) & DMA_ISR_TCIF1) == 0UL) {
        if (++s_stall_guard > 5000U) {
            s_stall_guard = 0;
            DMA_CCR(DMA1, DMA_CHANNEL1) &= ~DMA_CCR_EN;
            DMA_CNDTR(DMA1, DMA_CHANNEL1) = APC_FADER_COUNT;
            DMA_IFCR(DMA1) = DMA_IFCR_CGIF1;
            DMA_CCR(DMA1, DMA_CHANNEL1) |= DMA_CCR_EN;
            adc_trigger_swstart();
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

    DMA_IFCR(DMA1) = DMA_IFCR_CTCIF1;
    adc_trigger_swstart();
    return (s_warm_samples >= APC_ADC_WINDOW_SAMPLES);
}

void faders_init(void) {
    for (uint8_t i = 0; i < APC_FADER_COUNT; i++) {
        s_adc_dma_buf[i] = 0;
        s_ema_q4[i] = 0;
        s_hires_val[i] = 0;
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

        uint16_t cur_hires = s_hires_val[i];
        uint16_t raw_hires = adc_to_hires_14bit(avg);
        if (cur_hires == APC_FADER_HIRES_MAX && avg >= APC_ADC_FULL_HOLD_MIN) {
            raw_hires = APC_FADER_HIRES_MAX;
        }
        int32_t target_q4 = (int32_t)raw_hires << 4;
        if (raw_hires == 0U || raw_hires == APC_FADER_HIRES_MAX) {
            s_ema_q4[i] = target_q4;
        } else {
            s_ema_q4[i] += (target_q4 - s_ema_q4[i]) >> 3;
        }
        uint16_t filt_hires = (uint16_t)((s_ema_q4[i] + 8) >> 4);
        uint16_t diff_hires =
            (filt_hires >= cur_hires) ? (uint16_t)(filt_hires - cur_hires)
                                      : (uint16_t)(cur_hires - filt_hires);
        bool hires_changed = (diff_hires >= APC_ADC_HIRES_HYST) ||
                             (filt_hires == 0U && cur_hires != 0U) ||
                             (filt_hires == APC_FADER_HIRES_MAX && cur_hires != APC_FADER_HIRES_MAX);
        if (hires_changed) {
            s_hires_val[i] = filt_hires;
        }

        uint8_t new_cc = quantize_cc_hysteresis(avg, s_cc_val[i]);
        bool cc_changed = (new_cc != s_cc_val[i]);
        if (cc_changed) {
            s_cc_val[i] = new_cc;
        }

        if ((hires_changed || cc_changed) && on_change != 0) {
            on_change(i, s_cc_val[i], s_hires_val[i], cc_changed);
        }
    }
}

void faders_get_values(uint8_t out_values[APC_FADER_COUNT]) {
    for (uint8_t i = 0; i < APC_FADER_COUNT; i++) {
        out_values[i] = s_cc_val[i];
    }
}

uint16_t faders_get_hires(uint8_t fader_idx) {
    if (fader_idx >= APC_FADER_COUNT) {
        return 0U;
    }
    return s_hires_val[fader_idx];
}
