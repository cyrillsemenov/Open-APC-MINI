#pragma once

#include <stdbool.h>

void relocate_vtor(void);
void usb_dp_pullup(bool enable);
void iwdg_init(void);

void hw_init(void);
void periph_app_init(void);
