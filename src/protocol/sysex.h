#pragma once

#include <stdint.h>

void sysex_init(void);
void sysex_handle_msg(const uint8_t *msg, uint16_t len);
