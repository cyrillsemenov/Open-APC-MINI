#pragma once

#include <stdint.h>

typedef struct {
    uint32_t active; // 1 = pending, 0 = empty
    uint8_t note;    // MIDI note 0x00..0x3F (grid)
    uint8_t pad[3];  // 3-byte alignment padding
    uint32_t color;  // MIDI velocity / LED mode
} led_event_t;

void tm_send_byte(uint8_t data);
void tm1629_clear_init(void);
void round_led_init(void);
void set_round_led(uint8_t group, uint8_t bit_idx, uint8_t state);
void mux_round_leds(void);
void enqueue_led_q(const led_event_t *ev);
void process_led_q(void);
void blink_refresh(void);
