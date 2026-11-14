#pragma once

#include <stdint.h>
#include "apc_config.h"

typedef void (*faders_change_fn)(uint8_t fader_idx, uint8_t value);

void faders_init(void);
void faders_task(faders_change_fn on_change);
void faders_get_values(uint8_t out_values[APC_FADER_COUNT]);
