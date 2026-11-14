#pragma once

#include "stm32f10x.h"
#include "buttons_faders.h"
#include "leds_tm1629.h"
#include "usb_midi_io.h"

/*---------------------------------------------------------------------------
 * Global Timing Gates & Flags (updated in SysTick_Handler / USB IRQs)
 *---------------------------------------------------------------------------*/
extern volatile uint8_t usb_tx_busy;
extern volatile uint8_t adc_tick_div10;
extern volatile uint8_t sys_tick_div2;
extern volatile uint8_t sysex_Msg_id;
extern volatile uint8_t scan_gate;
extern volatile uint8_t round_mux_idx;
extern volatile uint8_t round_gate;
extern uint8_t round_led[4];
extern volatile uint8_t sof_flag;
extern volatile uint8_t round_blink_phase_0;
extern volatile uint8_t round_blink_phase_1;
extern volatile uint8_t pad_blink_phase;
extern volatile uint16_t blink_timer;
extern volatile uint8_t round_blink_mask_0;
extern volatile uint8_t round_blink_mask_1;
extern volatile uint8_t led_flush_gate;

/*---------------------------------------------------------------------------
 * Global Peripheral & Protocol State
 *---------------------------------------------------------------------------*/
extern volatile uint16_t adc_raw[9];
extern fader_state_t faders[9];
extern uint32_t pad_mode[8][8];
extern uint8_t led_q_count;
extern led_event_t led_q[128];
extern uint8_t tm_live[16];
extern uint8_t tm_shadow[16];

/*---------------------------------------------------------------------------
 * Constant Tables
 *---------------------------------------------------------------------------*/
extern const uint8_t kUpdaterUnlockReq[12];
extern const uint8_t kBootloaderMagic[8];
extern const uint8_t kTmRedRow[8];
extern const uint8_t kTmGreenRow[8];

extern uint8_t usb_ep1_rx_buf[64];
extern uint8_t flash_page_buf[256];
extern uint8_t _boot_magic_page[];
