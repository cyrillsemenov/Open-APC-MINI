#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "buttons.h"
#include "leds.h"

void monome_mode_init(uint32_t now_ticks);
void monome_mode_on_button(apc_btn_group_t group, uint8_t index, bool pressed, uint32_t now_ticks);
void monome_mode_on_fader(uint8_t fader_idx, uint8_t value);
void monome_mode_poll_rx(uint32_t now_ticks);

// Lua Co-Processor overlay & unified pad compositor hooks
apc_pad_led_mode_t monome_map_level_to_pad_mode(uint8_t level, uint8_t palette);
void monome_mode_refresh_pad(uint8_t x, uint8_t y, uint32_t now_ticks);
void monome_mode_clear_grid_pad(uint8_t x, uint8_t y);
void monome_mode_set_overlay_led(uint8_t x, uint8_t y, uint8_t level, uint32_t now_ticks);
void monome_mode_clear_overlay(uint32_t now_ticks);
uint8_t monome_mode_get_host_led(uint8_t x, uint8_t y);
void monome_mode_send_key(uint8_t x, uint8_t y, bool pressed);
void monome_mode_send_tilt(uint8_t sensor, int16_t x, int16_t y, int16_t z);
void monome_mode_refresh_status_leds(void);
