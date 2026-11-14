#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/iwdg.h>
#include <libopencm3/stm32/rcc.h>
#include "hw_config.h"
#include "apc_mini.h"

// 8 MHz HSE * 6 = 48 MHz SYSCLK, 48 MHz USBCLK (/1), 24 MHz APB1 (/2), 8 MHz ADCCLK (/6)
static const struct rcc_clock_scale k_clock_hse8_48mhz = {
    .pll_mul = RCC_CFGR_PLLMUL_PLL_CLK_MUL6,
    .pll_source = RCC_CFGR_PLLSRC_HSE_CLK,
    .hpre = RCC_CFGR_HPRE_NODIV,
    .ppre1 = RCC_CFGR_PPRE_DIV2,
    .ppre2 = RCC_CFGR_PPRE_NODIV,
    .adcpre = RCC_CFGR_ADCPRE_DIV6,
    .flash_waitstates = FLASH_ACR_LATENCY_2WS,
    .prediv1 = RCC_CFGR_PLLXTPRE_HSE_CLK,
    .usbpre = RCC_CFGR_USBPRE_PLL_CLK_NODIV,
    .ahb_frequency = 48000000UL,
    .apb1_frequency = 24000000UL,
    .apb2_frequency = 48000000UL,
};

static void clock_init(void) {
    // Safely step SYSCLK down to HSI first if the bootloader left PLL running
    if (((RCC_CFGR >> RCC_CFGR_SWS_SHIFT) & 0x3U) == RCC_CFGR_SWS_SYSCLKSEL_PLLCLK) {
        rcc_osc_on(RCC_HSI);
        rcc_wait_for_osc_ready(RCC_HSI);
        rcc_set_sysclk_source(RCC_CFGR_SW_SYSCLKSEL_HSICLK);
        while (((RCC_CFGR >> RCC_CFGR_SWS_SHIFT) & 0x3U) != RCC_CFGR_SWS_SYSCLKSEL_HSICLK) {}
    }
    rcc_osc_off(RCC_PLL);

    flash_prefetch_enable();
    rcc_clock_setup_pll(&k_clock_hse8_48mhz);
}

static void gpio_init(void) {
    // Enable AFIO and disable JTAG (keep SWD on PA13/PA14, free PA15/PB3/PB4)
    rcc_periph_clock_enable(RCC_AFIO);
    gpio_primary_remap(AFIO_MAPR_SWJ_CFG_JTAG_OFF_SW_ON, 0);

    // Enable GPIOA, GPIOB, GPIOC, GPIOD clocks
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
    rcc_periph_clock_enable(RCC_GPIOD);

    // PB8..PB11: Round LED column sinks -> Out_PP 2 MHz (idle HIGH = LEDs off)
    gpio_set(GPIOB, GPIO8 | GPIO9 | GPIO10 | GPIO11);
    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL,
                  GPIO8 | GPIO9 | GPIO10 | GPIO11);

    // PB12..PB14: TM1629 STB, CLK, DIO -> Out_OD 2 MHz (idle HIGH)
    gpio_set(GPIOB, GPIO12 | GPIO13 | GPIO14);
    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_OPENDRAIN,
                  GPIO12 | GPIO13 | GPIO14);

    // PC0..PC3 (round LED row anodes LOW) + PC5..PC15 (button column strobes HIGH) -> Out_PP 2 MHz
    gpio_clear(GPIOC, GPIO0 | GPIO1 | GPIO2 | GPIO3);
    gpio_set(GPIOC, 0xFFE0U);
    gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, 0xFFEFU);

    // PB0..PB7, PB15 (0x80FF): Button matrix row inputs with internal pull-ups (ODR bit = 1)
    gpio_set_mode(GPIOB, GPIO_MODE_INPUT, GPIO_CNF_INPUT_PULL_UPDOWN, 0x80FFU);
    gpio_set(GPIOB, 0x80FFU);

    // PA9, PA10, PA15: Out_PP 2 MHz, set HIGH
    gpio_set(GPIOA, GPIO9 | GPIO10 | GPIO15);
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO9 | GPIO10 | GPIO15);

    // PD2: Out_PP 2 MHz, set HIGH
    gpio_set(GPIOD, GPIO2);
    gpio_set_mode(GPIOD, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO2);
}

static void adc_dma_init(void) {
    static const uint8_t k_adc_channels[9] = {0, 1, 2, 3, 4, 5, 6, 7, 14};

    rcc_periph_clock_enable(RCC_DMA1);
    rcc_periph_clock_enable(RCC_ADC1);
    rcc_periph_clock_enable(RCC_ADC2);

    // PA0..PA7: Faders 1..8 analog inputs
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_ANALOG, 0x00FFU);

    // PC4 (ADC1_IN14): Master fader 9 analog input
    gpio_set_mode(GPIOC, GPIO_MODE_INPUT, GPIO_CNF_INPUT_ANALOG, GPIO4);

    // DMA1_Channel1: circular 9 half-words from ADC1_DR (0x4001244C) to adc_raw[9]
    dma_channel_reset(DMA1, DMA_CHANNEL1);
    dma_set_peripheral_address(DMA1, DMA_CHANNEL1, (uint32_t)&ADC_DR(ADC1));
    dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)adc_raw);
    dma_set_read_from_peripheral(DMA1, DMA_CHANNEL1);
    dma_set_number_of_data(DMA1, DMA_CHANNEL1, 9);
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
    adc_set_regular_sequence(ADC1, 9, (uint8_t *)k_adc_channels);
    adc_enable_dma(ADC1);
    adc_power_on(ADC1);
    adc_reset_calibration(ADC1);
    adc_calibrate(ADC1);

    // Enable and calibrate ADC2 (matches stock board initialization)
    adc_power_off(ADC2);
    adc_power_on(ADC2);
    adc_reset_calibration(ADC2);
    adc_calibrate(ADC2);

    dma_clear_interrupt_flags(DMA1, DMA_CHANNEL1, DMA_GIF);
    adc_start_conversion_regular(ADC1);
}

static void systick_init(void) {
    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB_DIV8);
    systick_set_reload((rcc_ahb_frequency / 8UL) / 2000UL);
    systick_clear();
    systick_interrupt_enable();
    systick_counter_enable();
}

void usb_dp_pullup(bool enable) {
    // PA8 drives external USB D+ 1.5k pull-up
    if (enable) {
        gpio_set(GPIOA, GPIO8);
    } else {
        gpio_clear(GPIOA, GPIO8);
    }
}

static void usb_hw_init(void) {
    rcc_periph_clock_enable(RCC_USB);
    nvic_enable_irq(NVIC_USB_LP_CAN_RX0_IRQ);

    rcc_periph_clock_enable(RCC_GPIOA);
    gpio_clear(GPIOA, GPIO8);
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO8);

    usb_midi_device_init();
}

void iwdg_init(void) {
    IWDG_KR = IWDG_KR_UNLOCK;
    IWDG_PR = IWDG_PR_DIV32;
    IWDG_RLR = 0x04E2U; // 1250 -> ~1.0 s timeout
    iwdg_reset();
    iwdg_start();
}

void hw_init(void) {
    clock_init();
    relocate_vtor();
    gpio_init();
    usb_tx_q_init();
    usb_hw_init();
    systick_init();
}

void periph_app_init(void) {
    tm1629_clear_init();
    adc_dma_init();
    fader_init();
    round_led_init();
}
