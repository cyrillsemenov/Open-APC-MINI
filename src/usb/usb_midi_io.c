#include "usb_lib.h"
#include "usb_pwr.h"
#include "apc_mini.h"

const uint8_t kUpdaterUnlockReq[12] = {0x04, 0xF0, 0x5A, 0x57, 0x04, 0x6E,
                                       0x28, 0x3C, 0x07, 0x4E, 0x3E, 0xF7};

const uint8_t kBootloaderMagic[8] = {0x55, 0x5A, 0xA5, 0xAA, 0x59, 0x95, 0x3A, 0xA3};

// Global USB-MIDI TX/RX Buffers & State
volatile uint8_t usb_tx_busy = 0;
static uint8_t rx_sysex_state = 0;
volatile uint8_t sysex_Msg_id = 0;
static uint16_t midi_msg_len = 0;
static uint8_t midi_msg_ready = 0;
volatile uint8_t sof_flag = 0;

// Dual 256-byte TX ring queues
static uint8_t *tx_q0_write_ptr;
static uint8_t *tx_q0_read_ptr;
static uint16_t tx_q0_count = 0;
static uint8_t *tx_q1_write_ptr;
static uint8_t *tx_q1_read_ptr;
static uint16_t tx_q1_count = 0;

// 512-byte RX ring buffer indices
static int32_t rx_ring_head = 0;
static int32_t rx_ring_tail = 0;

uint8_t usb_ep1_rx_buf[64];
static uint8_t tx_q0_buf[256];
static uint8_t tx_q1_buf[256];
static uint8_t tx_ep1_pkt[64];
static uint8_t serial_buf[16];
static uint8_t midi_msg_buf[512];
uint8_t flash_page_buf[256];
static uint8_t rx_ring_buf[512];

void usb_tx_q_init(void) {
    tx_q0_write_ptr = tx_q0_buf;
    tx_q0_read_ptr = tx_q0_buf;
    tx_q0_count = 0;

    tx_q1_write_ptr = tx_q1_buf;
    tx_q1_read_ptr = tx_q1_buf;
    tx_q1_count = 0;
}

void usb_tx_q0_write(const uint8_t *src, uint8_t len) {
    uint8_t i;
    for (i = 0; i < len; i++) {
        if (tx_q0_count > 255) {
            break;
        }
        *tx_q0_write_ptr = *src++;
        tx_q0_count++;
        tx_q0_write_ptr++;
        if (tx_q0_write_ptr == (tx_q0_buf + 256)) {
            tx_q0_write_ptr -= 256;
        }
    }
}

uint8_t usb_tx_q0_read(uint8_t *dst, uint8_t len) {
    uint8_t i;
    for (i = 0; i < len; i++) {
        if (tx_q0_count == 0) {
            break;
        }
        *dst++ = *tx_q0_read_ptr;
        tx_q0_count--;
        tx_q0_read_ptr++;
        if (tx_q0_read_ptr == (tx_q0_buf + 256)) {
            tx_q0_read_ptr -= 256;
        }
    }
    return i;
}

uint8_t usb_tx_q1_read(uint8_t *dst, uint8_t len) {
    uint8_t i;
    if (tx_q1_count == 0) {
        return 0;
    }
    if (tx_q1_count < 64) {
        tx_q1_count = 0;
    } else {
        tx_q1_count -= 64;
    }

    for (i = 0; i < len; i++) {
        *dst++ = *tx_q1_read_ptr;
        tx_q1_read_ptr++;
        if (tx_q1_read_ptr == (tx_q1_buf + 256)) {
            tx_q1_read_ptr -= 256;
        }
    }
    return i;
}

uint32_t USB_SIL_Write_EP1(uint8_t *pBufferPointer, uint32_t wBufferSize) {
    UserToPMABufferCopy(pBufferPointer, GetEPTxAddr(ENDP1), (uint16_t)wBufferSize);
    SetEPTxCount(ENDP1, (uint16_t)wBufferSize);
    SetEPTxValid(ENDP1);
    return 0;
}

void usb_midi_flush_tx(void) {
    uint8_t n;

    if (Device_Info.Current_Configuration == 0) {
        return;
    }
    if (usb_tx_busy != 0) {
        return;
    }

    // Check EP1 STAT_TX == NAK before transmitting
    if ((_GetENDPOINT(ENDP1) & EPTX_STAT) == EP_TX_VALID) {
        return;
    }

    if (tx_q1_count >= 64) {
        n = usb_tx_q1_read(tx_ep1_pkt, 64);
        (void)n;
        USB_SIL_Write_EP1(tx_ep1_pkt, 64);
    } else if (tx_q0_count >= 3) {
        n = usb_tx_q0_read(tx_ep1_pkt, 64);
        while (n < 64) {
            tx_ep1_pkt[n++] = 0x00;
        }
        USB_SIL_Write_EP1(tx_ep1_pkt, 64);
    }
}

static uint8_t rx_ring_is_empty(void) {
    return (rx_ring_head == rx_ring_tail) ? 1 : 0;
}

static uint8_t rx_ring_is_full(void) {
    if (rx_ring_tail > 0) {
        return ((rx_ring_tail - 1) == rx_ring_head) ? 1 : 0;
    } else {
        return (rx_ring_head == 511) ? 1 : 0;
    }
}

static uint8_t rx_ring_pop(void) {
    uint8_t val = 0xFF;
    if (!rx_ring_is_empty()) {
        int32_t idx = rx_ring_tail;
        rx_ring_tail = idx + 1;
        val = rx_ring_buf[idx];
        rx_ring_tail = rx_ring_tail % 512;
    }
    return val;
}

static uint8_t rx_ring_push(uint8_t val) {
    if (!rx_ring_is_full()) {
        int32_t idx = rx_ring_head;
        rx_ring_head = idx + 1;
        rx_ring_buf[idx] = val;
        rx_ring_head = rx_ring_head % 512;
        return 1;
    }
    return 0;
}

void usb_rx_parse_packet(void) {
    uint8_t i;
    uint8_t valid_end = 0;
    uint8_t stopped = 0;

    for (i = 0; i < 64; i += 4) {
        uint8_t cin = usb_ep1_rx_buf[i];
        if (cin == 0x09 || cin == 0x08 || cin == 0x0E || cin == 0x0B || cin == 0x03) {
            valid_end = (uint8_t)(valid_end + 4);
        } else if (cin == 0x04 || cin == 0x0F) {
            // SysEx 3-byte continuation or 1-byte message: keep scanning
        } else {
            stopped = 1;
            if (cin == 0x07 || cin == 0x06 || cin == 0x05) {
                valid_end = (uint8_t)(i + (cin - 4) + 1);
            } else if (cin == 0x00) {
                valid_end = i;
            } else {
                valid_end = 0;
            }
            break;
        }
    }

    if (!stopped) {
        valid_end = 64;
    }

    for (i = 0; i < valid_end; i++) {
        uint8_t b = usb_ep1_rx_buf[i];
        if (b == 0xF0 || b == 0x90 || b == 0x80 || b == 0xB0) {
            rx_sysex_state = 1;
        } else if (b >= 0xF8 && b <= 0xFC) {
            rx_sysex_state = 1;
        } else if (b == 0xF7) {
            if (rx_sysex_state == 1) {
                rx_sysex_state = 2;
            } else {
                rx_sysex_state = 0;
                return;
            }
        }
    }

    if (rx_sysex_state == 0) {
        return;
    }

    uint8_t pushed = 0;
    for (i = 0; i < valid_end; i++) {
        if ((i & 3) != 0) {
            rx_ring_push(usb_ep1_rx_buf[i]);
            pushed++;
        } else if (usb_ep1_rx_buf[i] == 0x0F) {
            uint8_t b1 = usb_ep1_rx_buf[i + 1];
            if (b1 < 0x80 || b1 >= 0xF8) {
                rx_ring_push(b1);
                pushed++;
            }
            i += 4;
        }
    }

    if (pushed == 0) {
        return;
    }

    if (rx_sysex_state == 2) {
        rx_sysex_state = 0;
        midi_msg_ready = 1;
    } else if (rx_sysex_state == 1) {
        midi_msg_ready = 1;
    }
}

void send_identity_reply(void) {
    uint8_t pkt[48];
    uint8_t *p = pkt;

    *p++ = 0x04;
    *p++ = 0xF0;
    *p++ = 0x7E;
    *p++ = midi_msg_buf[2];
    *p++ = 0x04;
    *p++ = 0x06;
    *p++ = 0x02;
    *p++ = 0x47;
    *p++ = 0x04;
    *p++ = 0x28;
    *p++ = 0x00;
    *p++ = 0x19;
    *p++ = 0x04;
    *p++ = 0x01;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x04;
    *p++ = 0x00;
    *p++ = sysex_Msg_id;
    *p++ = 0x00;
    *p++ = 0x04;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x04;
    *p++ = serial_buf[0];
    *p++ = serial_buf[1];
    *p++ = serial_buf[2];
    *p++ = 0x04;
    *p++ = serial_buf[3];
    *p++ = serial_buf[4];
    *p++ = serial_buf[5];
    *p++ = 0x04;
    *p++ = serial_buf[6];
    *p++ = serial_buf[7];
    *p++ = serial_buf[8];
    *p++ = 0x04;
    *p++ = serial_buf[9];
    *p++ = serial_buf[10];
    *p++ = serial_buf[11];
    *p++ = 0x04;
    *p++ = serial_buf[12];
    *p++ = serial_buf[13];
    *p++ = serial_buf[14];
    *p++ = 0x06;
    *p++ = serial_buf[15];
    *p++ = 0xF7;
    *p++ = 0x00;

    usb_tx_q0_write(pkt, 48);
}

void send_ableton_fader_dump(void) {
    uint8_t pkt[24];
    uint8_t *p = pkt;

    *p++ = 0x04;
    *p++ = 0xF0;
    *p++ = 0x47;
    *p++ = sysex_Msg_id;
    *p++ = 0x04;
    *p++ = 0x28;
    *p++ = 0x61;
    *p++ = 0x00;
    *p++ = 0x04;
    *p++ = 0x09;
    *p++ = faders[0].cc_val;
    *p++ = faders[1].cc_val;
    *p++ = 0x04;
    *p++ = faders[2].cc_val;
    *p++ = faders[3].cc_val;
    *p++ = faders[4].cc_val;
    *p++ = 0x04;
    *p++ = faders[5].cc_val;
    *p++ = faders[6].cc_val;
    *p++ = faders[7].cc_val;
    *p++ = 0x07;
    *p++ = faders[8].cc_val;
    *p++ = 0x00;
    *p++ = 0xF7;

    usb_tx_q0_write(pkt, 24);
}

uint8_t flash_write_page_256(uint32_t page_addr) {
    uint32_t offset;
    volatile uint32_t d;

    if ((page_addr & 0x3FFUL) != 0) {
        return 1;
    }

    // Check if the 256-byte region already matches flash_page_buf
    uint8_t diff = 0;
    for (offset = 0; offset < 256; offset += 4) {
        uint32_t flash_w = *(volatile uint32_t *)(page_addr + offset);
        uint32_t ram_w = *(uint32_t *)(flash_page_buf + offset);
        if (flash_w != ram_w) {
            diff = 1;
            break;
        }
    }

    if (!diff) {
        return 1;
    }

    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);

    if (FLASH_ErasePage(page_addr) != FLASH_COMPLETE) {
        FLASH_Lock();
        return 0;
    }

    for (offset = 0; offset < 256; offset += 4) {
        uint32_t ram_w = *(uint32_t *)(flash_page_buf + offset);
        FLASH_ProgramWord(page_addr + offset, ram_w);
        for (d = 0; d < 20; d++) {}
        if (*(volatile uint32_t *)(page_addr + offset) != ram_w) {
            FLASH_Lock();
            return 0;
        }
    }

    FLASH_Lock();
    return 1;
}

void handle_updater_unlock_and_reset(void) {
    uint8_t i;
    uint8_t ack[64];
    volatile uint32_t d; // !

    if (usb_ep1_rx_buf[1] != 0xF0) {
        return;
    }

    for (i = 0; i < 12; i++) {
        if (usb_ep1_rx_buf[i] != kUpdaterUnlockReq[i]) {
            return;
        }
    }

    // Copy 8-byte bootloader magic into flash_page_buf and program
    for (i = 0; i < 8; i++) {
        flash_page_buf[i] = kBootloaderMagic[i];
    }
    flash_write_page_256((uint32_t)_boot_magic_page);

    for (i = 0; i < 64; i++) {
        ack[i] = 0x00;
    }
    ack[0] = 0x07;
    ack[1] = 0xF0;
    ack[2] = 0x15;
    ack[3] = 0xF7;
    USB_SIL_Write_EP1(ack, 64);

    // Wait for the USB host to read the EP1 IN packet (STAT_TX leaves EP_TX_VALID)
    for (d = 0; d < 400000; d++) {
        if ((_GetENDPOINT(ENDP1) & EPTX_STAT) != EP_TX_VALID) {
            break;
        }
    }

    // Small settle delay (~2 ms at 48 MHz) to ensure the USB ACK handshake completes on the wire
    for (d = 0; d < 20000; d++) {}

    // Pull PA8 (USB D+ pull-up) LOW so the host OS registers a physical USB disconnect
    USB_Cable_Config(DISABLE);
    for (d = 0; d < 400000; d++) {}

    // Trigger ARM Cortex-M3 System Reset (SCB->AIRCR = VECTKEY (0x05FA) | SYSRESETREQ (bit 2))
    __DSB();
    SCB->AIRCR = (0x5FAUL << 16) | (SCB->AIRCR & 0x700UL) | (1UL << 2);
    __DSB();
    while (1) {}
}

void usb_midi_poll_rx(void) {
    while (!rx_ring_is_empty() && midi_msg_len < 512) {
        midi_msg_buf[midi_msg_len++] = rx_ring_pop();
    }

    if (midi_msg_len == 0) {
        return;
    }

    while (midi_msg_len != 0) {
        uint16_t consumed = 0;
        uint8_t b0 = midi_msg_buf[0];

        if (b0 == 0xF0 && midi_msg_buf[1] == 0xF7) {
            consumed = 2;
        } else if (b0 == 0xF0) {
            // Universal Device Inquiry: F0 7E <ch> 06 01 F7
            if (midi_msg_buf[1] == 0x7E && (midi_msg_buf[2] == 0x7F || midi_msg_buf[2] <= 0x0F) &&
                midi_msg_buf[3] == 0x06 && midi_msg_buf[4] == 0x01 && midi_msg_buf[5] == 0xF7) {
                consumed = 6;
                send_identity_reply();
            }
            // Akai Serial Write: F0 47 <dev> 28 79 00 10 <16B> F7
            else if (midi_msg_buf[1] == 0x47 && midi_msg_buf[2] <= 0x7F &&
                     midi_msg_buf[3] == 0x28 && midi_msg_buf[4] == 0x79 &&
                     midi_msg_buf[5] == 0x00 && midi_msg_buf[6] == 0x10 &&
                     midi_msg_buf[23] == 0xF7) {
                uint8_t k;
                consumed = 24;
                sysex_Msg_id = midi_msg_buf[2];
                for (k = 7; k < 23; k++) {
                    serial_buf[k - 7] = midi_msg_buf[k];
                }
            }
            // Ableton Live Intro / Fader Dump Request: F0 47 <dev> 28 60 00 04 00 <v1> <v2> <v3> F7
            else if (midi_msg_buf[1] == 0x47 && midi_msg_buf[2] <= 0x7F &&
                     midi_msg_buf[3] == 0x28 && midi_msg_buf[4] == 0x60 &&
                     midi_msg_buf[5] == 0x00 && midi_msg_buf[6] == 0x04 &&
                     midi_msg_buf[7] == 0x00 && midi_msg_buf[11] == 0xF7) {
                consumed = 12;
                sysex_Msg_id = midi_msg_buf[2];
                send_ableton_fader_dump();
            } else {
                goto reset_msg_buf;
            }
        } else if (b0 >= 0x90 && b0 <= 0x9F) {
            // MIDI Note-On: 9n <note> <vel>
            uint8_t note = midi_msg_buf[1];
            uint8_t vel = midi_msg_buf[2] & 0x7FU;
            uint8_t kind = 0;

            consumed = 3;
            if (note < 0x40) {
                kind = 3;
            } else if (note >= 0x40 && note <= 0x47) {
                kind = 1;
            } else if (note >= 0x52 && note <= 0x59) {
                kind = 2;
            }

            if (kind == 1) {
                uint8_t bit_idx = (uint8_t)(7 - (note - 0x40));
                if (vel == 0) {
                    round_blink_mask_0 &= (uint8_t)~(1U << bit_idx);
                    set_round_led(0, bit_idx, 0);
                } else if (vel == 2) {
                    round_blink_mask_0 |= (uint8_t)(1U << bit_idx);
                } else {
                    round_blink_mask_0 &= (uint8_t)~(1U << bit_idx);
                    set_round_led(0, bit_idx, 1);
                }
            } else if (kind == 2) {
                uint8_t bit_idx = (uint8_t)(7 - (note - 0x52));
                if (vel == 0) {
                    round_blink_mask_1 &= (uint8_t)~(1U << bit_idx);
                    set_round_led(1, bit_idx, 0);
                } else if (vel == 2) {
                    round_blink_mask_1 |= (uint8_t)(1U << bit_idx);
                } else {
                    round_blink_mask_1 &= (uint8_t)~(1U << bit_idx);
                    set_round_led(1, bit_idx, 1);
                }
            } else if (kind == 3) {
                uint8_t row = (uint8_t)(7 - (note / 8));
                uint8_t col = (uint8_t)(note % 8);
                uint32_t immediate = 1;
                led_event_t ev;

                ev.active = 1;
                ev.note = note;
                if (vel < 8) {
                    ev.color = vel;
                    if (vel == 2 || vel == 4 || vel == 6) {
                        immediate = 0;
                    }
                } else {
                    ev.color = 7;
                }

                pad_mode[row][col] = ev.color;

                if (immediate != 0) {
                    if (led_flush_gate == 0) {
                        led_flush_gate = 50;
                    } else {
                        led_flush_gate = 20;
                    }
                    enqueue_led_q(&ev);
                }
            } else {
                goto reset_msg_buf;
            }
        } else if (b0 >= 0x80 && b0 <= 0x8F) {
            // MIDI Note-Off: 8n <note> <vel>
            uint8_t note = midi_msg_buf[1];
            uint8_t kind = 0;

            consumed = 3;
            if (note < 0x40) {
                kind = 3;
            } else if (note >= 0x40 && note <= 0x47) {
                kind = 1;
            } else if (note >= 0x52 && note <= 0x59) {
                kind = 2;
            }

            if (kind == 1) {
                uint8_t bit_idx = (uint8_t)(7 - (note - 0x40));
                round_blink_mask_0 &= (uint8_t)~(1U << bit_idx);
                set_round_led(0, bit_idx, 0);
            } else if (kind == 2) {
                uint8_t bit_idx = (uint8_t)(7 - (note - 0x52));
                round_blink_mask_1 &= (uint8_t)~(1U << bit_idx);
                set_round_led(1, bit_idx, 0);
            } else if (kind == 3) {
                uint8_t row = (uint8_t)(7 - (note / 8));
                uint8_t col = (uint8_t)(note % 8);
                led_event_t ev;

                ev.active = 1;
                ev.note = note;
                ev.color = 0;
                pad_mode[row][col] = 0;
                enqueue_led_q(&ev);
            } else {
                goto reset_msg_buf;
            }
        } else if ((b0 >= 0xA0 && b0 <= 0xAF) || (b0 >= 0xB0 && b0 <= 0xBF) ||
                   ((b0 & 0xE0) == 0xE0)) {
            consumed = 3;
        } else if (((b0 & 0xC0) == 0xC0) || ((b0 & 0xD0) == 0xD0)) {
            consumed = 2;
        } else {
            goto reset_msg_buf;
        }

        if (midi_msg_len >= consumed) {
            midi_msg_len = (uint16_t)(midi_msg_len - consumed);
            if (midi_msg_len != 0) {
                uint16_t j;
                for (j = 0; j < midi_msg_len; j++) {
                    midi_msg_buf[j] = midi_msg_buf[j + consumed];
                }
            } else {
                goto reset_msg_buf;
            }
        } else {
            goto reset_msg_buf;
        }
    }

reset_msg_buf:
    midi_msg_buf[0] = 0;
    midi_msg_len = 0;
    midi_msg_ready = 0;
}
