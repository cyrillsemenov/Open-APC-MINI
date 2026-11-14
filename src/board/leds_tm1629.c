#include "apc_mini.h"
#include "leds_tm1629.h"

const uint8_t kTmRedRow[8] = {0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x0C, 0x0D};
const uint8_t kTmGreenRow[8] = {0x02, 0x03, 0x06, 0x07, 0x0A, 0x0B, 0x0E, 0x0F};

volatile uint8_t round_mux_idx = 0;
volatile uint8_t round_gate = 0;
uint8_t round_led[4] = {0x0F, 0x0F, 0x0F, 0x0F};
volatile uint8_t round_blink_phase_0 = 0;
volatile uint8_t round_blink_phase_1 = 0;
volatile uint8_t pad_blink_phase = 0;
volatile uint16_t blink_timer = 0;
volatile uint8_t round_blink_mask_0 = 0;
volatile uint8_t round_blink_mask_1 = 0;
volatile uint8_t led_flush_gate = 0;

uint8_t tm_live[16];
uint8_t tm_shadow[16];
uint8_t led_q_count = 0;
led_event_t led_q[128];
uint32_t pad_mode[8][8];

void tm1629_clear_init(void) {}

void tm_send_byte(uint8_t data) {
    uint8_t bit;
    volatile uint8_t d;

    for (bit = 0; bit < 8; bit++) {
        GPIOB->ODR &= ~GPIO_Pin_13;
        for (d = 0; d < 10; d++) {}

        if (data & 0x01) {
            GPIOB->ODR |= GPIO_Pin_14;
        } else {
            GPIOB->ODR &= ~GPIO_Pin_14;
        }
        data >>= 1;

        GPIOB->ODR |= GPIO_Pin_13;
        for (d = 0; d < 10; d++) {}
    }
}

void round_led_init(void) {
    uint8_t i;
    for (i = 0; i < 4; i++) {
        round_led[i] = 0x0F;
    }
}

/*---------------------------------------------------------------------------
 * group: 0 = track buttons (0x40..0x47), 1 = scene buttons (0x52..0x59)
 * bit_idx: 0..7 (reversed button index: 7 - (note - base))
 * state: 0 = OFF, 1 = ON
 *---------------------------------------------------------------------------*/
void set_round_led(uint8_t group, uint8_t bit_idx, uint8_t state) {
    uint8_t col_sink_bit = 0;
    uint8_t row_anode_idx;

    if (bit_idx > 7) {
        return;
    }

    if (group == 1) {
        col_sink_bit = (bit_idx >= 4) ? 3 : 2;
    } else if (group == 0) {
        col_sink_bit = (bit_idx >= 4) ? 1 : 0;
    }

    row_anode_idx = (uint8_t)((7 - bit_idx) & 3);

    if (state == 1) {
        round_led[row_anode_idx] &= (uint8_t)~(1U << col_sink_bit);
    } else if (state == 0) {
        round_led[row_anode_idx] |= (uint8_t)(1U << col_sink_bit);
    }
}

void mux_round_leds(void) {
    volatile uint8_t d;

    if (round_gate != 0) {
        return;
    }
    round_gate = 4;

    GPIOC->ODR &= 0xFFF0U;
    GPIOC->ODR = (1U << round_mux_idx);

    GPIOB->ODR = (GPIOB->ODR & 0xF0FFU) | ((uint16_t)round_led[round_mux_idx] << 8);

    for (d = 0; d < 20; d++) {}

    round_mux_idx++;
    if (round_mux_idx == 4) {
        round_mux_idx = 0;
    }
}

void enqueue_led_q(const led_event_t *ev) {
    if (ev->active == 0) {
        return;
    }
    if (led_q_count >= 128) {
        return;
    }

    led_q[led_q_count].active = ev->active;
    led_q[led_q_count].note = ev->note;
    led_q[led_q_count].color = ev->color;
    led_q_count++;
}

void process_led_q(void) {
    uint8_t idx = 0;
    uint8_t dirty = 0;
    volatile uint8_t i;

    if (led_q_count == 0) {
        return;
    }
    if (led_flush_gate != 0) {
        return;
    }

    do {
        if (led_q[idx].active == 1) {
            uint8_t note = led_q[idx].note;
            uint8_t row = (uint8_t)(7 - (note / 8));
            uint8_t col = (uint8_t)(note % 8);
            uint8_t grn_idx = kTmGreenRow[row];
            uint8_t red_idx = kTmRedRow[row];
            uint32_t color = led_q[idx].color;
            uint8_t mask = (uint8_t)(1U << col);

            if (color == 5) {
                tm_shadow[red_idx] |= mask;
                tm_shadow[grn_idx] |= mask;
            } else if (color == 0) {
                tm_shadow[grn_idx] &= (uint8_t)~mask;
                tm_shadow[red_idx] &= (uint8_t)~mask;
            } else if (color == 1 || color == 7) {
                tm_shadow[red_idx] &= (uint8_t)~mask;
                tm_shadow[grn_idx] |= mask;
            } else if (color == 3) {
                tm_shadow[grn_idx] &= (uint8_t)~mask;
                tm_shadow[red_idx] |= mask;
            }

            led_q[idx].active = 0;
        }
        idx++;
        led_q_count--;
    } while (led_q_count != 0);

    for (i = 0; i < 16; i++) {
        if (tm_shadow[i] != tm_live[i]) {
            if (dirty == 0) {
                dirty = 1;
            }
        }
        tm_live[i] = tm_shadow[i];
    }

    if (dirty != 0) {
        // Command 1: 0x40 (Data setting: write display, auto-increment address)
        GPIOB->ODR &= ~GPIO_Pin_12;
        tm_send_byte(0x40);
        GPIOB->ODR |= GPIO_Pin_12;
        for (i = 0; i < 16; i++) {}

        // Command 2: 0xC0 (Address 0x00) + 16 display bytes
        GPIOB->ODR &= ~GPIO_Pin_12;
        tm_send_byte(0xC0);
        for (i = 0; i < 16; i++) {}
        for (i = 0; i < 16; i++) {
            tm_send_byte(tm_live[i]);
        }
        GPIOB->ODR |= GPIO_Pin_12;
        for (i = 0; i < 16; i++) {}

        // Command 3: 0x8F (Display ON, max pulse width 14/16)
        GPIOB->ODR &= ~GPIO_Pin_12;
        tm_send_byte(0x8F);
    }
}

void blink_refresh(void) {
    uint8_t i;
    uint8_t row;
    uint8_t col;
    led_event_t ev;

    if (blink_timer != 0) {
        return;
    }
    blink_timer = 750; // 0x02EE * 0.5 ms = 375 ms blink half-period

    // Group 0 round LEDs (Track buttons 0x40..0x47)
    if (round_blink_mask_0 != 0) {
        for (i = 0; i < 8; i++) {
            uint8_t bit_idx = (uint8_t)(7 - i);
            if (round_blink_mask_0 & (1U << bit_idx)) {
                set_round_led(0, bit_idx, round_blink_phase_0 ? 1 : 0);
            }
        }
        round_blink_phase_0 = round_blink_phase_0 ? 0 : 1;
    }

    // Group 1 round LEDs (Scene buttons 0x52..0x59)
    if (round_blink_mask_1 != 0) {
        for (i = 0; i < 8; i++) {
            uint8_t bit_idx = (uint8_t)(7 - i);
            if (round_blink_mask_1 & (1U << bit_idx)) {
                set_round_led(1, bit_idx, round_blink_phase_1 ? 1 : 0);
            }
        }
        round_blink_phase_1 = round_blink_phase_1 ? 0 : 1;
    }

    // 8x8 RGB/Bi-color Grid Pads
    for (row = 0; row < 8; row++) {
        for (col = 0; col < 8; col++) {
            uint8_t note = (uint8_t)((56 - (row * 8)) + col);
            uint32_t mode = pad_mode[row][col];

            if (mode == 4) {
                ev.active = 1;
                ev.note = note;
                ev.color = pad_blink_phase ? 3 : 0;
                enqueue_led_q(&ev);
            } else if (mode == 2) {
                ev.active = 1;
                ev.note = note;
                ev.color = pad_blink_phase ? 1 : 0;
                enqueue_led_q(&ev);
            } else if (mode == 6) {
                ev.active = 1;
                ev.note = note;
                ev.color = pad_blink_phase ? 5 : 0;
                enqueue_led_q(&ev);
            }
        }
    }

    pad_blink_phase = pad_blink_phase ? 0 : 1;
}
