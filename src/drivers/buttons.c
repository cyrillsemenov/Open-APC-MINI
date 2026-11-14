#include <libopencm3/stm32/gpio.h>
#include "apc_config.h"
#include "buttons.h"

static uint8_t s_debounce_raw[APC_BTN_NUM_COLS];
static uint8_t s_debounce_cnt[APC_BTN_NUM_COLS];
static uint8_t s_stable[APC_BTN_NUM_COLS];
static uint8_t s_reported[APC_BTN_NUM_COLS];
static uint16_t s_dirty_mask = 0;
static bool s_shift_pressed = false;
static bool s_scan_initialized = false;
static uint32_t s_last_scan = 0;

static void scan_matrix_columns(void) {
    for (uint8_t col = 0; col < APC_BTN_NUM_COLS; col++) {
        // Drive PC5..PC15 HIGH and pull active column (col + 5) LOW atomically via BSRR
        uint32_t col_bit = (uint32_t)(APC_BTN_COL0_PIN << col);
        GPIO_BSRR(APC_BTN_COL_PORT) = (col_bit << 16) | ((uint32_t)APC_BTN_COL_MASK & ~col_bit);

        for (volatile uint8_t d = 0; d < 30; d++) {}

        uint8_t raw = (uint8_t)(GPIO_IDR(APC_BTN_ROW_PORT) & APC_BTN_ROW_MASK);
        if (s_debounce_raw[col] == raw) {
            if (s_debounce_cnt[col] < 15U) {
                s_debounce_cnt[col]++;
            }
            if (s_debounce_cnt[col] == APC_BTN_DEBOUNCE_SCANS) {
                s_stable[col] = (uint8_t)~raw;
                s_dirty_mask |= (uint16_t)(1U << col);
            }
        } else {
            s_debounce_raw[col] = raw;
            s_debounce_cnt[col] = 0;
        }
    }

    // Release all button strobes HIGH after scan pass
    GPIO_BSRR(APC_BTN_COL_PORT) = APC_BTN_COL_MASK;
}

static void dispatch_button_changes(buttons_event_fn on_event) {
    if (s_dirty_mask == 0) {
        return;
    }

    uint16_t mask = s_dirty_mask;
    s_dirty_mask = 0;
    s_shift_pressed = (s_stable[10] & 0x01U) != 0;

    for (uint8_t col = 0; col < APC_BTN_NUM_COLS; col++) {
        if ((mask & (1U << col)) == 0) {
            continue;
        }

        uint8_t max_rows = (col == 10U) ? 1U : APC_BTN_NUM_ROWS;
        uint8_t valid_msk = (col == 10U) ? 0x01U : 0xFFU;
        uint8_t cur = (uint8_t)(s_stable[col] & valid_msk);
        uint8_t changed = (uint8_t)(cur ^ s_reported[col]);
        if (changed == 0) {
            continue;
        }
        s_reported[col] = cur;

        for (uint8_t row = 0; row < max_rows; row++) {
            if ((changed & (1U << row)) == 0) {
                continue;
            }
            bool pressed = (cur & (1U << row)) != 0;

            if (col < 8U) {
                uint8_t grid_idx = (uint8_t)(((7U - row) * 8U) + col);
                if (on_event != 0) {
                    on_event(APC_BTN_GROUP_GRID, grid_idx, pressed);
                }
            } else if (col == 8U) {
                if (on_event != 0) {
                    on_event(APC_BTN_GROUP_TRACK, row, pressed);
                }
            } else if (col == 9U) {
                if (on_event != 0) {
                    on_event(APC_BTN_GROUP_SCENE, row, pressed);
                }
            } else {
                s_shift_pressed = pressed;
                if (on_event != 0) {
                    on_event(APC_BTN_GROUP_SHIFT, 0, pressed);
                }
            }
        }
    }
}

void buttons_init(void) {
    for (uint8_t i = 0; i < APC_BTN_NUM_COLS; i++) {
        s_debounce_raw[i] = 0;
        s_debounce_cnt[i] = 0;
        s_stable[i] = 0;
        s_reported[i] = 0;
    }
    s_dirty_mask = 0;
    s_shift_pressed = false;
    s_scan_initialized = false;
    s_last_scan = 0;
}

void buttons_task(uint32_t now_ticks, buttons_event_fn on_event) {
    if (!s_scan_initialized) {
        s_scan_initialized = true;
        s_last_scan = now_ticks;
        scan_matrix_columns();
    } else if ((uint32_t)(now_ticks - s_last_scan) >= APC_BTN_SCAN_TICKS) {
        s_last_scan = now_ticks;
        scan_matrix_columns();
    }

    dispatch_button_changes(on_event);
}

bool buttons_shift_pressed(void) {
    return s_shift_pressed;
}

void buttons_scan_boot_chord(bool *out_monome_mode, bool *out_dfu_mode) {
    // Scan Column 9 (Scene 1..8)
    uint32_t col9_bit = (uint32_t)(APC_BTN_COL0_PIN << 9U);
    GPIO_BSRR(APC_BTN_COL_PORT) = (col9_bit << 16) | ((uint32_t)APC_BTN_COL_MASK & ~col9_bit);
    for (volatile uint8_t d = 0; d < 30; d++) {}
    uint8_t scene_pressed = (uint8_t)(~GPIO_IDR(APC_BTN_ROW_PORT) & APC_BTN_ROW_MASK);

    // Scan Column 10 (Shift)
    uint32_t col10_bit = (uint32_t)(APC_BTN_COL0_PIN << 10U);
    GPIO_BSRR(APC_BTN_COL_PORT) = (col10_bit << 16) | ((uint32_t)APC_BTN_COL_MASK & ~col10_bit);
    for (volatile uint8_t d = 0; d < 30; d++) {}
    uint8_t shift_pressed = (uint8_t)(~GPIO_IDR(APC_BTN_ROW_PORT) & 0x01U);

    // Release all button strobes HIGH
    GPIO_BSRR(APC_BTN_COL_PORT) = APC_BTN_COL_MASK;

    if (out_dfu_mode != 0) {
        *out_dfu_mode = (scene_pressed & (1U << 7U)) != 0;
    }
    if (out_monome_mode != 0) {
        *out_monome_mode = (shift_pressed != 0) || ((scene_pressed & (1U << 1U)) != 0);
    }
}
