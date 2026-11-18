#pragma once

#include <stdbool.h>
#include <stdint.h>

// Relocates SCB_VTOR to the active image's vector table (defined in isr.c).
void relocate_vtor(void);

// Initializes 48 MHz PLL clock tree, VTOR, GPIOs, and 2 kHz SysTick timer.
void board_init(void);

// Starts the ~1.0 s Independent Watchdog (IWDG).
void board_watchdog_init(void);

// Reloads the Independent Watchdog counter.
void board_watchdog_kick(void);

// Increments the 0.5 ms monotonic tick counter (called from sys_tick_handler).
void board_tick_inc(void);

// Returns monotonic 0.5 ms ticks elapsed since SysTick start.
uint32_t board_ticks(void);

// Drives PA8 USB D+ 1.5 kOhm pull-up HIGH (attach) or LOW (detach).
void board_usb_dp_pullup(bool enable);

// Returns RCC_CSR[31:24] reset-cause flags captured at boot before clearing RMVF.
uint8_t board_reset_flags(void);

// Records a completed main-loop iteration and its duration in 0.5 ms SysTick units.
void board_record_loop_turn(uint32_t elapsed_ticks);

// Returns total main-loop iterations since boot.
uint32_t board_loop_count(void);

// Returns worst-case main-loop turnaround time in 0.5 ms SysTick units.
uint16_t board_max_loop_ticks(void);

// Saves or clears the post-DFU Monome serialosc resume flag in STM32 BKP_DR1 (0x40006C04).
void board_set_post_dfu_monome(bool enable);

// Returns true (and clears BKP_DR1) if the previous session entered DFU from Monome mode.
bool board_consume_post_dfu_monome(void);
