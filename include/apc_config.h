#pragma once

#include <stdint.h>

/*---------------------------------------------------------------------------
 * Clock & Timer Configuration (STM32F102RBT6 @ 48 MHz from 8 MHz HSE)
 *---------------------------------------------------------------------------*/
#define APC_HSE_HZ     8000000UL
#define APC_SYSCLK_HZ  48000000UL
#define APC_AHB_HZ     48000000UL
#define APC_APB1_HZ    24000000UL
#define APC_APB2_HZ    48000000UL
#define APC_SYSTICK_HZ 2000UL // 0.5 ms per tick

// Periodic task intervals in 0.5 ms SysTick units
#define APC_BTN_SCAN_TICKS             8U   // 4.0 ms per full 11-column scan
#define APC_BTN_DEBOUNCE_SCANS         4U   // 4 consecutive scans = 16 ms debounce
#define APC_ROUND_MUX_TICKS            8U   // 4.0 ms per round-LED anode slot (62.5 Hz refresh)
#define APC_BLINK_HALF_PERIOD_TICKS    750U // 375 ms blink half-period (1.33 Hz)
#define APC_TM1629_COOLDOWN_IDLE_TICKS 50U  // 25 ms initial coalescing window
#define APC_TM1629_COOLDOWN_BUSY_TICKS 20U  // 10 ms burst coalescing window

// Independent Watchdog (40 kHz LSI / 32 = 1.25 kHz -> 1250 counts ~= 1.0 s)
#define APC_IWDG_RELOAD 0x04E2U

/*---------------------------------------------------------------------------
 * Board Pinout Masks
 *---------------------------------------------------------------------------*/
// USB Full-Speed D+ 1.5 kOhm pull-up on PA8 (HIGH = attached, LOW = detached)
#define APC_USB_DP_PU_PORT GPIOA
#define APC_USB_DP_PU_PIN  GPIO8

// Analog faders: PA0..PA7 (ADC1_IN0..7) + PC4 (ADC1_IN14)
#define APC_FADER_GPIOA_MASK 0x00FFU
#define APC_FADER_MASTER_PIN GPIO4

// 11x8 Button Matrix:
//   Columns 0..10: PC5..PC15 active-low push-pull strobes (0xFFE0)
//   Rows 0..7:     PB0..PB7 active-low inputs with internal pull-ups (+ PB15 parked IPU = 0x80FF)
#define APC_BTN_COL_PORT     GPIOC
#define APC_BTN_COL_MASK     0xFFE0U
#define APC_BTN_COL0_PIN     0x0020U
#define APC_BTN_ROW_PORT     GPIOB
#define APC_BTN_ROW_MASK     0x00FFU
#define APC_BTN_ROW_IPU_MASK 0x80FFU
#define APC_BTN_NUM_COLS     11U
#define APC_BTN_NUM_ROWS     8U

// 4x4 Multiplexed Round LEDs (Track 1..8 + Scene 1..8):
//   Row anodes 0..3:   PC0..PC3 active-high push-pull (0x000F)
//   Column sinks 0..3: PB8..PB11 active-low push-pull (0x0F00)
#define APC_ROUND_ANODE_PORT GPIOC
#define APC_ROUND_ANODE_MASK 0x000FU
#define APC_ROUND_SINK_PORT  GPIOB
#define APC_ROUND_SINK_MASK  (GPIO8 | GPIO9 | GPIO10 | GPIO11)
#define APC_ROUND_NUM_SLOTS  4U

// Titan Micro TM1629 16x8 LED Controller (2 MHz open-drain on PB12..PB14)
#define APC_TM1629_PORT    GPIOB
#define APC_TM1629_STB_PIN GPIO12
#define APC_TM1629_CLK_PIN GPIO13
#define APC_TM1629_DIO_PIN GPIO14
#define APC_TM1629_PINS    (APC_TM1629_STB_PIN | APC_TM1629_CLK_PIN | APC_TM1629_DIO_PIN)

// Unused GPIO outputs parked HIGH
#define APC_PARK_GPIOA_PINS (GPIO9 | GPIO10 | GPIO15)
#define APC_PARK_GPIOD_PINS GPIO2

/*---------------------------------------------------------------------------
 * Fader ADC Filter Tuning
 *---------------------------------------------------------------------------*/
#define APC_FADER_COUNT        9U
#define APC_ADC_WINDOW_SAMPLES 3U
#define APC_ADC_OUTLIER_DELTA  0x00FFU
#define APC_ADC_ZERO_MAX       24U
#define APC_ADC_FULL_MIN       4071U
#define APC_ADC_SCALE_SPAN     (APC_ADC_FULL_MIN - APC_ADC_ZERO_MAX)
#define APC_ADC_HYSTERESIS     12U
