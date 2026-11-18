#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "buttons.h"
#include "leds.h"

#define APC_MODE_FLAG_MIDI      0x01U
#define APC_MODE_FLAG_SERIALOSC 0x02U
#define APC_MODE_FLAG_LUA       0x04U
#define APC_MODE_FLAG_ABLETON   0x08U
#define APC_MODE_FLAGS_DEFAULT  (APC_MODE_FLAG_MIDI | APC_MODE_FLAG_SERIALOSC | APC_MODE_FLAG_ABLETON)

void apc_mode_init(void);
uint8_t apc_mode_get_flags(void);
void apc_mode_set_flags(uint8_t flags, uint32_t now_ticks);
void apc_mode_on_button(apc_btn_group_t group, uint8_t index, bool pressed);
void apc_mode_on_fader(uint8_t fader_idx, uint8_t value);
void apc_mode_poll_rx(uint32_t now_ticks);
apc_pad_led_mode_t apc_mode_get_pad_mode(uint8_t x, uint8_t y);
void apc_mode_clear_pad_mode(uint8_t x, uint8_t y);

// Lua Co-Processor overlay hooks for APC MIDI mode
void apc_mode_set_overlay_led(uint8_t x, uint8_t y, uint8_t level, uint32_t now_ticks);
void apc_mode_clear_overlay(uint32_t now_ticks);
uint8_t apc_mode_get_host_led(uint8_t x, uint8_t y);
