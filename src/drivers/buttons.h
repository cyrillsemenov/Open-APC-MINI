#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APC_BTN_GROUP_GRID = 0,  // index 0..63 (bottom-left = 0, top-right = 63)
    APC_BTN_GROUP_TRACK = 1, // index 0..7  (horizontal track buttons 1..8)
    APC_BTN_GROUP_SCENE = 2, // index 0..7  (vertical scene buttons 1..8)
    APC_BTN_GROUP_SHIFT = 3, // index 0     (Shift button)
} apc_btn_group_t;

typedef void (*buttons_event_fn)(apc_btn_group_t group, uint8_t index, bool pressed);

void buttons_init(void);
void buttons_task(uint32_t now_ticks, buttons_event_fn on_event);
bool buttons_shift_pressed(void);
