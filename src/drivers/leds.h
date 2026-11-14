#pragma once

#include <stdint.h>
#include "apc_protocol.h"

typedef enum {
    APC_ROUND_GROUP_TRACK = 0, // Horizontal buttons 1..8 (notes 0x40..0x47)
    APC_ROUND_GROUP_SCENE = 1, // Vertical buttons 1..8 (notes 0x52..0x59)
} apc_round_led_group_t;

void leds_init(void);
void leds_set_pad(uint8_t row, uint8_t col, apc_pad_led_mode_t mode, uint32_t now_ticks);
void leds_set_brightness(uint8_t level, uint32_t now_ticks);
void leds_set_round(apc_round_led_group_t group, uint8_t index, apc_round_led_mode_t mode);
void leds_all_off(uint32_t now_ticks);
void leds_task(uint32_t now_ticks);
