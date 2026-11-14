#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "buttons.h"

void apc_mode_init(void);
void apc_mode_on_button(apc_btn_group_t group, uint8_t index, bool pressed);
void apc_mode_on_fader(uint8_t fader_idx, uint8_t value);
void apc_mode_poll_rx(uint32_t now_ticks);
