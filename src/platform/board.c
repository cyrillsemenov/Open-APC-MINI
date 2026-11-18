#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/iwdg.h>
#include <libopencm3/stm32/rcc.h>
#include "apc_config.h"
#include "board.h"

static volatile uint32_t s_systick_ticks = 0;
static uint8_t s_boot_reset_flags = 0;
static uint32_t s_loop_count = 0;
static uint16_t s_max_loop_ticks = 0;

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
    .ahb_frequency = APC_AHB_HZ,
    .apb1_frequency = APC_APB1_HZ,
    .apb2_frequency = APC_APB2_HZ,
};

static void clock_init(void) {
    // Step SYSCLK down to HSI first if the factory bootloader left PLL active
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
    // Enable AFIO and release JTAG pins (keep SWD on PA13/PA14, free PA15/PB3/PB4)
    rcc_periph_clock_enable(RCC_AFIO);
    gpio_primary_remap(AFIO_MAPR_SWJ_CFG_JTAG_OFF_SW_ON, 0);

    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
    rcc_periph_clock_enable(RCC_GPIOD);

    // PA8: USB D+ pull-up -> Out_PP 50 MHz, initially detached (LOW)
    gpio_clear(APC_USB_DP_PU_PORT, APC_USB_DP_PU_PIN);
    gpio_set_mode(APC_USB_DP_PU_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL,
                  APC_USB_DP_PU_PIN);

    // PB8..PB11: Round LED column sinks -> Out_PP 2 MHz (idle HIGH = LEDs off)
    gpio_set(APC_ROUND_SINK_PORT, APC_ROUND_SINK_MASK);
    gpio_set_mode(APC_ROUND_SINK_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL,
                  APC_ROUND_SINK_MASK);

    // PB12..PB14: TM1629 STB, CLK, DIO -> Out_OD 2 MHz (idle HIGH)
    gpio_set(APC_TM1629_PORT, APC_TM1629_PINS);
    gpio_set_mode(APC_TM1629_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_OPENDRAIN,
                  APC_TM1629_PINS);

    // PC0..PC3 (round LED anodes LOW) + PC5..PC15 (button strobes HIGH) -> Out_PP 2 MHz
    gpio_clear(APC_ROUND_ANODE_PORT, APC_ROUND_ANODE_MASK);
    gpio_set(APC_BTN_COL_PORT, APC_BTN_COL_MASK);
    gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL,
                  APC_ROUND_ANODE_MASK | APC_BTN_COL_MASK);

    // PB0..PB7, PB15: Button matrix row sense inputs with internal pull-ups
    gpio_set_mode(APC_BTN_ROW_PORT, GPIO_MODE_INPUT, GPIO_CNF_INPUT_PULL_UPDOWN,
                  APC_BTN_ROW_IPU_MASK);
    gpio_set(APC_BTN_ROW_PORT, APC_BTN_ROW_IPU_MASK);

    // Park unused outputs HIGH
    gpio_set(GPIOA, APC_PARK_GPIOA_PINS);
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, APC_PARK_GPIOA_PINS);

    gpio_set(GPIOD, APC_PARK_GPIOD_PINS);
    gpio_set_mode(GPIOD, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, APC_PARK_GPIOD_PINS);
}

static void systick_init(void) {
    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB_DIV8);
    systick_set_reload(((APC_AHB_HZ / 8UL) / APC_SYSTICK_HZ) - 1UL);
    systick_clear();
    systick_interrupt_enable();
    systick_counter_enable();
}

void board_init(void) {
    s_boot_reset_flags = (uint8_t)((RCC_CSR >> 24U) & 0xFFU);
    RCC_CSR |= RCC_CSR_RMVF;

    clock_init();
    relocate_vtor();
    gpio_init();
    systick_init();
}

void board_watchdog_init(void) {
    IWDG_KR = IWDG_KR_UNLOCK;
    IWDG_PR = IWDG_PR_DIV32;
    IWDG_RLR = APC_IWDG_RELOAD;
    iwdg_reset();
    iwdg_start();
}

void board_watchdog_kick(void) {
    iwdg_reset();
}

void board_tick_inc(void) {
    s_systick_ticks++;
}

uint32_t board_ticks(void) {
    return s_systick_ticks;
}

void board_usb_dp_pullup(bool enable) {
    if (enable) {
        gpio_set(APC_USB_DP_PU_PORT, APC_USB_DP_PU_PIN);
    } else {
        gpio_clear(APC_USB_DP_PU_PORT, APC_USB_DP_PU_PIN);
    }
}

uint8_t board_reset_flags(void) {
    return s_boot_reset_flags;
}

void board_record_loop_turn(uint32_t elapsed_ticks) {
    s_loop_count++;
    if (elapsed_ticks > (uint32_t)s_max_loop_ticks) {
        s_max_loop_ticks = (elapsed_ticks > 0xFFFFUL) ? 0xFFFFU : (uint16_t)elapsed_ticks;
    }
}

uint32_t board_loop_count(void) {
    return s_loop_count;
}

uint16_t board_max_loop_ticks(void) {
    return s_max_loop_ticks;
}
