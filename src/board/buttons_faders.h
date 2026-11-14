#pragma once

#include <stdint.h>

typedef struct {
    uint8_t cc_num; // 0x30..0x38 (MIDI CC 48..56)
    uint8_t cc_val; // 0..127 current reported MIDI CC value
    uint16_t raw;   // Last accepted 12-bit ADC sample
} fader_state_t;

void scan_buttons(void);
void report_buttons(void);
void fader_init(void);
void kick_adc(void);
void report_faders(void);
