#include "apc_config.h"
#include "apc_protocol.h"
#include "board.h"
#include "bootloader.h"
#include "buttons.h"
#include "faders.h"
#include "sysex.h"
#include "usb_midi.h"

#define DIAG_SYSEX_SUB_TELEMETRY  0x01U
#define DIAG_SYSEX_SUB_CRASH_GET  0x02U
#define DIAG_SYSEX_SUB_CRASH_CLR  0x03U
#define DIAG_SYSEX_SUB_PEEK_MEM   0x04U
#define DIAG_SYSEX_SUB_TEST_FAULT 0x05U

static uint8_t s_sysex_dev_id = 0;
static uint8_t s_serial_buf[16];

static void put_u16_le(uint8_t *dst, uint16_t val) {
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
}

static void put_u32_le(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((val >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((val >> 24U) & 0xFFU);
}

static void send_diag_nibbles(uint8_t subcmd, const uint8_t *raw, uint8_t raw_len) {
    uint8_t reply[104];
    if (((uint16_t)raw_len * 2U + 7U) > sizeof(reply)) {
        return;
    }
    reply[0] = MIDI_STATUS_SYSEX_START;
    reply[1] = 0x7DU;
    reply[2] = 'A';
    reply[3] = 'P';
    reply[4] = 'C';
    reply[5] = subcmd;

    uint8_t pos = 6U;
    for (uint8_t i = 0U; i < raw_len; i++) {
        reply[pos++] = (uint8_t)((raw[i] >> 4U) & 0x0FU);
        reply[pos++] = (uint8_t)(raw[i] & 0x0FU);
    }
    reply[pos++] = MIDI_STATUS_SYSEX_END;
    usb_midi_send_sysex(reply, pos);
}

static void handle_diag_sysex(const uint8_t *msg, uint16_t len) {
    uint8_t subcmd = msg[5];

    if (subcmd == DIAG_SYSEX_SUB_TELEMETRY) {
        uint8_t payload[28];
        const volatile apc_crash_log_t *crash =
            (const volatile apc_crash_log_t *)(uintptr_t)_crash_log_page;

        payload[0] = board_reset_flags();
        payload[1] = (crash->magic == APC_CRASH_MAGIC) ? 1U : 0U;
        put_u16_le(&payload[2], *(const volatile uint16_t *)0x1FFFF7E0UL);
        put_u32_le(&payload[4], *(const volatile uint32_t *)0x1FFFF7E8UL);
        put_u32_le(&payload[8], board_ticks());
        put_u32_le(&payload[12], board_loop_count());
        put_u16_le(&payload[16], board_max_loop_ticks());
        faders_get_values(&payload[18]);
        payload[27] = buttons_shift_pressed() ? 1U : 0U;

        send_diag_nibbles(DIAG_SYSEX_SUB_TELEMETRY, payload, sizeof(payload));
        return;
    }

    if (subcmd == DIAG_SYSEX_SUB_CRASH_GET) {
        send_diag_nibbles(DIAG_SYSEX_SUB_CRASH_GET, (const uint8_t *)(uintptr_t)_crash_log_page,
                          (uint8_t)sizeof(apc_crash_log_t));
        return;
    }

    if (subcmd == DIAG_SYSEX_SUB_CRASH_CLR) {
        uint8_t ok = bootloader_clear_crash_log() ? 1U : 0U;
        send_diag_nibbles(DIAG_SYSEX_SUB_CRASH_CLR, &ok, 1U);
        return;
    }

    if (subcmd == DIAG_SYSEX_SUB_PEEK_MEM && len == 15U) {
        uint32_t addr = 0UL;
        for (uint8_t i = 0U; i < 4U; i++) {
            uint8_t hi = (uint8_t)(msg[6U + (i * 2U)] & 0x0FU);
            uint8_t lo = (uint8_t)(msg[7U + (i * 2U)] & 0x0FU);
            uint8_t b = (uint8_t)((hi << 4U) | lo);
            addr |= ((uint32_t)b << (i * 8U));
        }
        uint8_t buf[16];
        const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)addr;
        for (uint8_t i = 0U; i < 16U; i++) {
            buf[i] = src[i];
        }
        send_diag_nibbles(DIAG_SYSEX_SUB_PEEK_MEM, buf, sizeof(buf));
        return;
    }

    if (subcmd == DIAG_SYSEX_SUB_TEST_FAULT && len == 8U && msg[6] == 0x5AU) {
        // Deliberately execute an undefined Thumb instruction (UDF #0) to test
        // hardware exception stacking -> 0x08007800 crash log -> DFU recovery.
        __asm__ volatile(".short 0xde00");
    }
}

static void send_identity_reply(uint8_t req_channel) {
    uint8_t reply[35] = {
        MIDI_STATUS_SYSEX_START,
        SYSEX_UNIVERSAL_NON_REALTIME,
        req_channel,
        SYSEX_SUB_GENERAL_INFO,
        SYSEX_SUB_IDENTITY_REPLY,
        AKAI_MANUFACTURER_ID,
        AKAI_APC_MINI_MODEL_ID,
        0x00U,
        0x19U,
        0x01U,
        0x00U,
        0x00U,
        0x00U,
        s_sysex_dev_id,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
    };

    for (uint8_t i = 0; i < 16U; i++) {
        reply[18U + i] = s_serial_buf[i];
    }
    reply[34] = MIDI_STATUS_SYSEX_END;

    usb_midi_send_sysex(reply, sizeof(reply));
}

static void send_ableton_fader_dump(void) {
    uint8_t fader_vals[APC_FADER_COUNT];
    faders_get_values(fader_vals);

    uint8_t reply[18];
    reply[0] = MIDI_STATUS_SYSEX_START;
    reply[1] = AKAI_MANUFACTURER_ID;
    reply[2] = s_sysex_dev_id;
    reply[3] = AKAI_APC_MINI_MODEL_ID;
    reply[4] = AKAI_CMD_FADER_DUMP;
    reply[5] = 0x00U;
    reply[6] = APC_FADER_COUNT;
    for (uint8_t i = 0; i < APC_FADER_COUNT; i++) {
        reply[7U + i] = fader_vals[i];
    }
    reply[16] = 0x00U;
    reply[17] = MIDI_STATUS_SYSEX_END;

    usb_midi_send_sysex(reply, sizeof(reply));
}

void sysex_init(void) {
    s_sysex_dev_id = 0;
    for (uint8_t i = 0; i < sizeof(s_serial_buf); i++) {
        s_serial_buf[i] = 0;
    }
}

void sysex_handle_msg(const uint8_t *msg, uint16_t len) {
    if (len < 2U || msg[0] != MIDI_STATUS_SYSEX_START || msg[len - 1U] != MIDI_STATUS_SYSEX_END) {
        return;
    }

    // Universal Device Inquiry: F0 7E <ch> 06 01 F7
    if (len == 6U && msg[1] == SYSEX_UNIVERSAL_NON_REALTIME &&
        (msg[2] == SYSEX_DEVICE_BROADCAST || msg[2] <= 0x0FU) && msg[3] == SYSEX_SUB_GENERAL_INFO &&
        msg[4] == SYSEX_SUB_IDENTITY_REQUEST) {
        send_identity_reply(msg[2]);
        return;
    }

    // Open APC Mini Diagnostic & Post-Mortem Telemetry SysEx: F0 7D 'A' 'P' 'C' <subcmd> ... F7
    if (len >= 7U && msg[1] == 0x7DU && msg[2] == 'A' && msg[3] == 'P' && msg[4] == 'C') {
        handle_diag_sysex(msg, len);
        return;
    }

    // Bootloader DFU entry SysEx: F0 75 7F 75 54 00 00 F7
    if (len == 8U && msg[1] == 0x75U && msg[2] == 0x7FU && msg[3] == 0x75U && msg[4] == 0x54U &&
        msg[5] == 0x00U && msg[6] == 0x00U) {
        bootloader_enter_dfu();
        return;
    }

    // Akai APC Mini vendor messages: F0 47 <dev> 28 <cmd> ...
    if (len >= 8U && msg[1] == AKAI_MANUFACTURER_ID && msg[2] <= 0x7FU &&
        msg[3] == AKAI_APC_MINI_MODEL_ID) {
        uint8_t cmd = msg[4];

        // Akai Serial Write: F0 47 <dev> 28 79 00 10 <16B> F7
        if (cmd == AKAI_CMD_SERIAL_WRITE && len == 24U && msg[5] == 0x00U && msg[6] == 0x10U) {
            s_sysex_dev_id = msg[2];
            for (uint8_t i = 0; i < 16U; i++) {
                s_serial_buf[i] = msg[7U + i];
            }
            return;
        }

        // Ableton Live Intro / Fader Dump Request: F0 47 <dev> 28 60 00 04 00 <v1> <v2> <v3> F7
        if (cmd == AKAI_CMD_ABLETON_INTRO && len == 12U && msg[5] == 0x00U && msg[6] == 0x04U &&
            msg[7] == 0x00U) {
            s_sysex_dev_id = msg[2];
            send_ableton_fader_dump();
            return;
        }
    }
}
