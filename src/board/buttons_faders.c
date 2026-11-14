#include "apc_mini.h"
#include "buttons_faders.h"

volatile uint8_t scan_gate = 0;
volatile uint8_t adc_tick_div10 = 0;
volatile uint8_t sys_tick_div2 = 0;

static uint16_t btn_dirty_mask = 0;
static uint32_t shift_held_state = 0;
static uint8_t btn_prev_col8[8];
static uint8_t btn_prev_col9[8];
static uint8_t btn_prev_shift = 0;
static uint16_t btn_debounce_val[11];
static uint8_t btn_debounce_cnt[11];
static uint8_t btn_prev_grid[64];
static uint8_t btn_sent_state[8][8];
static uint8_t btn_stable[11];

static uint8_t fader_sample_idx = 0;
static uint16_t fader_dirty_mask = 0;
static uint16_t adc_prev[9];
static uint8_t fader_prev0[9];
static uint8_t fader_prev1[9];
static uint16_t fader_hist[9][3];
fader_state_t faders[9];
volatile uint16_t adc_raw[9];
static uint16_t fader_avg[9];

void scan_buttons(void) {
    uint8_t col;
    volatile uint8_t d;

    if (scan_gate != 0) {
        return;
    }
    scan_gate = 8;

    for (col = 0; col < 11; col++) {
        // Set PC5..PC15 HIGH, then pull active column (col + 5) LOW
        GPIOC->ODR |= 0xFFE0U;
        GPIOC->ODR &= (uint16_t)~(0x0020U << col);

        for (d = 0; d < 30; d++) {}

        uint8_t raw = (uint8_t)(GPIOB->IDR & 0x00FFU);

        if (btn_debounce_val[col] == raw) {
            if (btn_debounce_cnt[col] < 15) {
                btn_debounce_cnt[col]++;
            }
            if (btn_debounce_cnt[col] == 4) {
                btn_stable[col] = (uint8_t)~raw;
                btn_dirty_mask |= (uint16_t)(1U << col);
            }
        } else {
            btn_debounce_val[col] = raw;
            btn_debounce_cnt[col] = 0;
        }
    }
}

void report_buttons(void) {
    uint16_t mask;
    uint8_t col;
    uint8_t row;
    uint8_t pkt[4];

    if (btn_dirty_mask == 0) {
        return;
    }
    mask = btn_dirty_mask;
    btn_dirty_mask = 0;

    for (col = 0; col < 11; col++) {
        if ((mask & (1U << col)) == 0) {
            continue;
        }

        if (col < 8) {
            // 8x8 Grid Pads (MIDI notes 0x00..0x3F)
            for (row = 0; row < 8; row++) {
                uint8_t bit_val = (uint8_t)(btn_stable[col] & (1U << row));
                uint8_t idx = (uint8_t)((col * 8) + row);

                if (btn_prev_grid[idx] != bit_val) {
                    btn_prev_grid[idx] = bit_val;
                    if (bit_val != 0) {
                        if (btn_sent_state[col][row] == 0) {
                            btn_sent_state[col][row] = 1;
                            pkt[0] = 0x09;
                            pkt[1] = 0x90;
                            pkt[2] = (uint8_t)(((7 - row) * 8) + col);
                            pkt[3] = 0x7F;
                            usb_tx_q0_write(pkt, 4);
                        }
                    } else {
                        if (btn_sent_state[col][row] != 0) {
                            btn_sent_state[col][row] = 0;
                            pkt[0] = 0x08;
                            pkt[1] = 0x80;
                            pkt[2] = (uint8_t)(((7 - row) * 8) + col);
                            pkt[3] = 0x7F;
                            usb_tx_q0_write(pkt, 4);
                        }
                    }
                }
            }
        } else if (col == 8) {
            // Track Buttons (MIDI notes 0x40..0x47)
            for (row = 0; row < 8; row++) {
                uint8_t bit_val = (uint8_t)(btn_stable[col] & (1U << row));
                if (btn_prev_col8[row] != bit_val) {
                    btn_prev_col8[row] = bit_val;
                    pkt[0] = (bit_val != 0) ? 0x09 : 0x08;
                    pkt[1] = (bit_val != 0) ? 0x90 : 0x80;
                    pkt[2] = (uint8_t)(0x40 + row);
                    pkt[3] = 0x7F;
                    usb_tx_q0_write(pkt, 4);
                }
            }
        } else if (col == 9) {
            // Scene Launch Buttons (MIDI notes 0x52..0x59)
            for (row = 0; row < 8; row++) {
                uint8_t bit_val = (uint8_t)(btn_stable[col] & (1U << row));
                if (btn_prev_col9[row] != bit_val) {
                    btn_prev_col9[row] = bit_val;
                    pkt[0] = (bit_val != 0) ? 0x09 : 0x08;
                    pkt[1] = (bit_val != 0) ? 0x90 : 0x80;
                    pkt[2] = (uint8_t)(0x52 + row);
                    pkt[3] = 0x7F;
                    usb_tx_q0_write(pkt, 4);
                }
            }
        } else if (col == 10) {
            // Shift Button (MIDI note 0x62)
            uint8_t bit_val = (uint8_t)(btn_stable[col] & 0x01U);
            if (btn_prev_shift != bit_val) {
                btn_prev_shift = bit_val;
                shift_held_state = (bit_val != 0) ? 1 : 0;
                pkt[0] = (bit_val != 0) ? 0x09 : 0x08;
                pkt[1] = (bit_val != 0) ? 0x90 : 0x80;
                pkt[2] = 0x62;
                pkt[3] = 0x7F;
                usb_tx_q0_write(pkt, 4);
            }
        }
    }
}

void fader_init(void) {
    uint8_t i;
    for (i = 0; i < 9; i++) {
        faders[i].cc_num = (uint8_t)(0x30 + i);
        faders[i].cc_val = 0;
        faders[i].raw = 0;
    }
}

void kick_adc(void) {
    static uint16_t dma_stall_guard = 0;
    uint8_t i;

    if (DMA_GetFlagStatus(DMA1_FLAG_TC1) == RESET) {
        if (++dma_stall_guard > 5000) {
            dma_stall_guard = 0;
            DMA_Cmd(DMA1_Channel1, DISABLE);
            DMA1_Channel1->CNDTR = 9;
            DMA_ClearFlag(DMA1_FLAG_GL1);
            DMA_Cmd(DMA1_Channel1, ENABLE);
            ADC_SoftwareStartConvCmd(ADC1, ENABLE);
        }
        return;
    }
    dma_stall_guard = 0;

    for (i = 0; i < 9; i++) {
        faders[i].raw = adc_raw[i];
    }

    fader_dirty_mask = 0;
    for (i = 0; i < 9; i++) {
        uint16_t diff = (adc_prev[i] >= faders[i].raw) ? (uint16_t)(adc_prev[i] - faders[i].raw)
                                                       : (uint16_t)(faders[i].raw - adc_prev[i]);
        if (diff >= 26) {
            fader_dirty_mask |= (uint16_t)(1U << i);
            // Do NOT update adc_prev[i] here; report_faders() updates it once 3 back-to-back
            // samples are collected.
        }
    }

    DMA_ClearFlag(DMA1_FLAG_TC1);
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
}

void report_faders(void) {
    uint16_t d01[9];
    uint16_t d12[9];
    uint16_t d02[9];
    uint8_t new_cc[9];
    uint8_t pkt[4];
    uint16_t outlier_mask = 0;
    uint8_t i;

    if (fader_dirty_mask == 0) {
        return;
    }
    fader_dirty_mask = 0;

    for (i = 0; i < 9; i++) {
        fader_hist[i][fader_sample_idx] = faders[i].raw;
    }

    fader_sample_idx++;
    if (fader_sample_idx < 3) {
        return;
    }
    fader_sample_idx = 0;

    // Compute pairwise differences across the 3 samples for each fader
    for (i = 0; i < 9; i++) {
        uint16_t s0 = fader_hist[i][0];
        uint16_t s1 = fader_hist[i][1];
        uint16_t s2 = fader_hist[i][2];

        d01[i] = (s0 >= s1) ? (uint16_t)(s0 - s1) : (uint16_t)(s1 - s0);
        if (d01[i] >= 0xFF) {
            outlier_mask |= (uint16_t)(1U << i);
            continue;
        }

        d12[i] = (s1 >= s2) ? (uint16_t)(s1 - s2) : (uint16_t)(s2 - s1);
        if (d12[i] >= 0xFF) {
            outlier_mask |= (uint16_t)(1U << i);
            continue;
        }

        d02[i] = (s0 >= s2) ? (uint16_t)(s0 - s2) : (uint16_t)(s2 - s0);
        if (d02[i] >= 0xFF) {
            outlier_mask |= (uint16_t)(1U << i);
            continue;
        }
    }

    for (i = 0; i < 9; i++) {
        if (outlier_mask & (1U << i)) {
            continue;
        }

        uint16_t s0 = fader_hist[i][0];
        uint16_t s1 = fader_hist[i][1];
        uint16_t s2 = fader_hist[i][2];

        // Average the two closest samples
        if (d01[i] > d12[i]) {
            if (d12[i] > d02[i]) {
                fader_avg[i] = (uint16_t)((s0 + s2) / 2);
            } else {
                fader_avg[i] = (uint16_t)((s1 + s2) / 2);
            }
        } else {
            if (d01[i] > d02[i]) {
                fader_avg[i] = (uint16_t)((s0 + s2) / 2);
            } else {
                fader_avg[i] = (uint16_t)((s0 + s1) / 2);
            }
        }

        // Anchor adc_prev[i] to the filtered 3-sample average so the next 26-count
        // movement triggers 3 back-to-back hardware samples (~0.8 CC step resolution)
        adc_prev[i] = fader_avg[i];

        // Scale 12-bit average (deadzones <= 3 -> 0, >= 4093 -> 127, linear / 4090)
        if (fader_avg[i] >= 0x0FFD) {
            new_cc[i] = 127;
        } else if (fader_avg[i] <= 3) {
            new_cc[i] = 0;
        } else {
            int32_t scaled = (((int32_t)fader_avg[i] - 3) << 7) / 0x0FFA;
            if (scaled > 127) {
                scaled = 127;
            }
            new_cc[i] = (uint8_t)scaled;
        }

        // Prevent duplicate CC and 1-LSB back-and-forth bounce (fader_prev1)
        if (faders[i].cc_val == new_cc[i]) {
            continue;
        }
        if (new_cc[i] == fader_prev1[i]) {
            continue;
        }

        fader_prev1[i] = fader_prev0[i];
        fader_prev0[i] = new_cc[i];
        faders[i].cc_val = new_cc[i];

        pkt[0] = 0x0B;
        pkt[1] = 0xB0;
        pkt[2] = faders[i].cc_num;
        pkt[3] = new_cc[i];
        usb_tx_q0_write(pkt, 4);
    }
}
