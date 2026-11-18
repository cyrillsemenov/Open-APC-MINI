#include <stdbool.h>
#include <libopencm3/stm32/gpio.h>
#include "apc_config.h"
#include "leds.h"
#include "tm1629.h"

static uint8_t s_pad_mode[8][8];
static uint8_t s_round_cathodes[APC_ROUND_NUM_SLOTS] = {0x0FU, 0x0FU, 0x0FU, 0x0FU};
static uint8_t s_round_blink_mask[2] = {0, 0};
static bool s_round_blink_phase[2] = {false, false};
static bool s_pad_blink_phase = false;

static uint8_t s_mux_slot = 0;
static bool s_timers_initialized = false;
static uint32_t s_last_mux_tick = 0;
static uint32_t s_last_blink_tick = 0;

static void write_round_hw_bit(apc_round_led_group_t group, uint8_t bit_idx, bool lit) {
    if (bit_idx > 7) {
        return;
    }

    uint8_t col_sink_bit = 0;
    if (group == APC_ROUND_GROUP_SCENE) {
        col_sink_bit = (bit_idx >= 4) ? 3U : 2U;
    } else {
        col_sink_bit = (bit_idx >= 4) ? 1U : 0U;
    }

    uint8_t row_anode_idx = (uint8_t)((7U - bit_idx) & 3U);
    if (lit) {
        s_round_cathodes[row_anode_idx] &= (uint8_t)~(1U << col_sink_bit);
    } else {
        s_round_cathodes[row_anode_idx] |= (uint8_t)(1U << col_sink_bit);
    }
}

static void mux_round_leds_step(void) {
    // 1. Blank all PC0..PC3 row anodes first (leaves PC5..PC15 button strobes untouched)
    GPIO_BSRR(APC_ROUND_ANODE_PORT) = ((uint32_t)APC_ROUND_ANODE_MASK << 16);

    // 2. Drive PB8..PB11 column sinks for the next slot (leaves PB0..PB7/PB12..PB15 untouched)
    uint32_t nib = (uint32_t)(s_round_cathodes[s_mux_slot] & 0x0FU);
    GPIO_BSRR(APC_ROUND_SINK_PORT) = ((~nib & 0x0FUL) << 24) | (nib << 8);

    // 3. Assert active PC0..PC3 row anode HIGH
    GPIO_BSRR(APC_ROUND_ANODE_PORT) = (1UL << s_mux_slot);

    for (volatile uint8_t d = 0; d < 20; d++) {}

    s_mux_slot = (uint8_t)((s_mux_slot + 1U) & 3U);
}

static void refresh_blink_phase(uint32_t now_ticks) {
    for (uint8_t g = 0; g < 2; g++) {
        if (s_round_blink_mask[g] != 0) {
            for (uint8_t i = 0; i < 8; i++) {
                uint8_t bit_idx = (uint8_t)(7U - i);
                if ((s_round_blink_mask[g] & (1U << bit_idx)) != 0) {
                    write_round_hw_bit((apc_round_led_group_t)g, bit_idx, s_round_blink_phase[g]);
                }
            }
            s_round_blink_phase[g] = !s_round_blink_phase[g];
        }
    }

    bool any_pad_blink = false;
    for (uint8_t row = 0; row < 8; row++) {
        for (uint8_t col = 0; col < 8; col++) {
            uint8_t mode = s_pad_mode[row][col];
            if (mode == APC_PAD_LED_GREEN_BLINK) {
                tm1629_set_pad_rgb(row, col, s_pad_blink_phase, false);
                any_pad_blink = true;
            } else if (mode == APC_PAD_LED_RED_BLINK) {
                tm1629_set_pad_rgb(row, col, false, s_pad_blink_phase);
                any_pad_blink = true;
            } else if (mode == APC_PAD_LED_YELLOW_BLINK) {
                tm1629_set_pad_rgb(row, col, s_pad_blink_phase, s_pad_blink_phase);
                any_pad_blink = true;
            }
        }
    }

    if (any_pad_blink) {
        tm1629_schedule_flush(now_ticks, false);
    }
    s_pad_blink_phase = !s_pad_blink_phase;
}

void leds_init(void) {
    tm1629_init();

    for (uint8_t r = 0; r < 8; r++) {
        for (uint8_t c = 0; c < 8; c++) {
            s_pad_mode[r][c] = APC_PAD_LED_OFF;
        }
    }
    for (uint8_t i = 0; i < APC_ROUND_NUM_SLOTS; i++) {
        s_round_cathodes[i] = 0x0FU;
    }
    s_round_blink_mask[0] = 0;
    s_round_blink_mask[1] = 0;
    s_round_blink_phase[0] = false;
    s_round_blink_phase[1] = false;
    s_pad_blink_phase = false;
    s_mux_slot = 0;
    s_timers_initialized = false;

    GPIO_BSRR(APC_ROUND_ANODE_PORT) = ((uint32_t)APC_ROUND_ANODE_MASK << 16);
    GPIO_BSRR(APC_ROUND_SINK_PORT) = (uint32_t)APC_ROUND_SINK_MASK;
}

void leds_set_pad(uint8_t row, uint8_t col, apc_pad_led_mode_t mode, uint32_t now_ticks) {
    if (row >= 8 || col >= 8) {
        return;
    }
    if (mode > APC_PAD_LED_GREEN_SOLID) {
        mode = APC_PAD_LED_GREEN_SOLID;
    }

    s_pad_mode[row][col] = (uint8_t)mode;

    switch (mode) {
    case APC_PAD_LED_OFF:
        tm1629_set_pad_rgb(row, col, false, false);
        tm1629_schedule_flush(now_ticks, false);
        break;
    case APC_PAD_LED_GREEN:
    case APC_PAD_LED_GREEN_SOLID:
        tm1629_set_pad_rgb(row, col, true, false);
        tm1629_schedule_flush(now_ticks, true);
        break;
    case APC_PAD_LED_RED:
        tm1629_set_pad_rgb(row, col, false, true);
        tm1629_schedule_flush(now_ticks, true);
        break;
    case APC_PAD_LED_YELLOW:
        tm1629_set_pad_rgb(row, col, true, true);
        tm1629_schedule_flush(now_ticks, true);
        break;
    case APC_PAD_LED_GREEN_BLINK:
    case APC_PAD_LED_RED_BLINK:
    case APC_PAD_LED_YELLOW_BLINK:
        // Deferred to 375 ms blink phase generator
        break;
    }
}

void leds_set_round(apc_round_led_group_t group, uint8_t index, apc_round_led_mode_t mode) {
    if (index >= 8 || group > APC_ROUND_GROUP_SCENE) {
        return;
    }

    uint8_t bit_idx = (uint8_t)(7U - index);
    uint8_t bit = (uint8_t)(1U << bit_idx);

    if (mode == APC_ROUND_LED_OFF) {
        s_round_blink_mask[group] &= (uint8_t)~bit;
        write_round_hw_bit(group, bit_idx, false);
    } else if (mode == APC_ROUND_LED_BLINK) {
        s_round_blink_mask[group] |= bit;
    } else {
        s_round_blink_mask[group] &= (uint8_t)~bit;
        write_round_hw_bit(group, bit_idx, true);
    }
}

void leds_all_off(uint32_t now_ticks) {
    for (uint8_t r = 0; r < 8; r++) {
        for (uint8_t c = 0; c < 8; c++) {
            s_pad_mode[r][c] = APC_PAD_LED_OFF;
            tm1629_set_pad_rgb(r, c, false, false);
        }
    }
    s_round_blink_mask[0] = 0;
    s_round_blink_mask[1] = 0;
    for (uint8_t i = 0; i < APC_ROUND_NUM_SLOTS; i++) {
        s_round_cathodes[i] = 0x0FU;
    }
    tm1629_schedule_flush(now_ticks, false);
}

void leds_task(uint32_t now_ticks) {
    if (!s_timers_initialized) {
        s_timers_initialized = true;
        s_last_mux_tick = now_ticks;
        s_last_blink_tick = now_ticks;
        mux_round_leds_step();
        refresh_blink_phase(now_ticks);
    } else {
        if ((uint32_t)(now_ticks - s_last_mux_tick) >= APC_ROUND_MUX_TICKS) {
            s_last_mux_tick = now_ticks;
            mux_round_leds_step();
        }
        if ((uint32_t)(now_ticks - s_last_blink_tick) >= APC_BLINK_HALF_PERIOD_TICKS) {
            s_last_blink_tick = now_ticks;
            refresh_blink_phase(now_ticks);
        }
    }

    tm1629_task(now_ticks);
}
