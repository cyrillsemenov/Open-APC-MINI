#pragma once

#include <stdbool.h>
#include <stdint.h>

void tm1629_init(void);
void tm1629_set_pad_rgb(uint8_t row, uint8_t col, bool green, bool red);
void tm1629_set_brightness(uint8_t level);
void tm1629_schedule_flush(uint32_t now_ticks, bool coalesce);
void tm1629_task(uint32_t now_ticks);
