#include "apc_config.h"
#include "apc_protocol.h"
#include "faders.h"
#include "sysex.h"
#include "usb_midi.h"

static uint8_t s_sysex_dev_id = 0;
static uint8_t s_serial_buf[16];

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
