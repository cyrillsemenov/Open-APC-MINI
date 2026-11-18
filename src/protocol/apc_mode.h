#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "buttons.h"

void apc_mode_init(void);
void apc_mode_on_button(apc_btn_group_t group, uint8_t index, bool pressed);
void apc_mode_on_fader(uint8_t fader_idx, uint8_t value);
void apc_mode_poll_rx(uint32_t now_ticks);

// Lua Co-Processor overlay hooks for APC MIDI mode
void apc_mode_set_overlay_led(uint8_t x, uint8_t y, uint8_t level, uint32_t now_ticks);
void apc_mode_clear_overlay(uint32_t now_ticks);
uint8_t apc_mode_get_host_led(uint8_t x, uint8_t y);
void apc_mode_refresh_status_leds(void);
