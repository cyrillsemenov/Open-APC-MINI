#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "apc_config.h"

typedef void (*faders_change_fn)(uint8_t fader_idx, uint8_t cc_val, uint16_t hires_val,
                                 bool cc_changed);

void faders_init(void);
void faders_task(faders_change_fn on_change);
void faders_get_values(uint8_t out_values[APC_FADER_COUNT]);
uint16_t faders_get_hires(uint8_t fader_idx);
