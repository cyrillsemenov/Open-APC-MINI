#include "apc_config.h"
#include "apc_protocol.h"
#include "apc_mode.h"
#include "leds.h"
#include "lua_vm.h"
#include "monome_mode.h"
#include "sysex.h"
#include "usb_midi.h"

static uint32_t s_rx_now_ticks = 0;
static apc_pad_led_mode_t s_host_pad_mode[8][8];
static uint8_t s_overlay_level[8][8];

static apc_round_led_mode_t velocity_to_round_mode(uint8_t vel) {
    if (vel == 0) {
        return APC_ROUND_LED_OFF;
    }
    if (vel == 2U) {
        return APC_ROUND_LED_BLINK;
    }
    return APC_ROUND_LED_ON;
}

static void refresh_apc_pad(uint8_t x, uint8_t y, uint32_t now_ticks) {
    if (x >= 8U || y >= 8U) {
        return;
    }
    uint8_t ovl = s_overlay_level[y][x];
    if (ovl != 0U) {
        leds_set_pad(y, x, monome_map_level_to_pad_mode(ovl, 0U), now_ticks);
    } else {
        leds_set_pad(y, x, s_host_pad_mode[y][x], now_ticks);
    }
}

static void handle_note_led(uint8_t note, uint8_t vel, bool note_on) {
    if (note <= APC_NOTE_GRID_END) {
        uint8_t row = (uint8_t)(7U - (note / 8U));
        uint8_t col = (uint8_t)(note % 8U);
        apc_pad_led_mode_t mode = APC_PAD_LED_OFF;
        if (note_on) {
            mode = (vel < 8U) ? (apc_pad_led_mode_t)vel : APC_PAD_LED_GREEN_SOLID;
        }
        s_host_pad_mode[row][col] = mode;
        refresh_apc_pad(col, row, s_rx_now_ticks);
    } else if (note >= APC_NOTE_TRACK_BASE && note <= APC_NOTE_TRACK_END) {
        uint8_t idx = (uint8_t)(note - APC_NOTE_TRACK_BASE);
        apc_round_led_mode_t rnd = note_on ? velocity_to_round_mode(vel) : APC_ROUND_LED_OFF;
        leds_set_round(APC_ROUND_GROUP_TRACK, idx, rnd);
    } else if (note >= APC_NOTE_SCENE_BASE && note <= APC_NOTE_SCENE_END) {
        uint8_t idx = (uint8_t)(note - APC_NOTE_SCENE_BASE);
        apc_round_led_mode_t rnd = note_on ? velocity_to_round_mode(vel) : APC_ROUND_LED_OFF;
        leds_set_round(APC_ROUND_GROUP_SCENE, idx, rnd);
    }
}

static void handle_midi_channel_msg(uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t type = (uint8_t)(status & 0xF0U);
    uint8_t note = (uint8_t)(data1 & 0x7FU);
    uint8_t vel = (uint8_t)(data2 & 0x7FU);

    if (type == MIDI_STATUS_NOTE_ON) {
        handle_note_led(note, vel, true);
    } else if (type == MIDI_STATUS_NOTE_OFF) {
        handle_note_led(note, 0, false);
    }
}

void apc_mode_init(void) {
    for (uint8_t y = 0; y < 8U; y++) {
        for (uint8_t x = 0; x < 8U; x++) {
            s_host_pad_mode[y][x] = APC_PAD_LED_OFF;
            s_overlay_level[y][x] = 0U;
        }
    }
    sysex_init();
    apc_mode_refresh_status_leds();
}

void apc_mode_set_overlay_led(uint8_t x, uint8_t y, uint8_t level, uint32_t now_ticks) {
    if (x >= 8U || y >= 8U) {
        return;
    }
    s_overlay_level[y][x] = level;
    refresh_apc_pad(x, y, now_ticks);
}

void apc_mode_clear_overlay(uint32_t now_ticks) {
    for (uint8_t y = 0; y < 8U; y++) {
        for (uint8_t x = 0; x < 8U; x++) {
            s_overlay_level[y][x] = 0U;
            refresh_apc_pad(x, y, now_ticks);
        }
    }
}

uint8_t apc_mode_get_host_led(uint8_t x, uint8_t y) {
    if (x >= 8U || y >= 8U) {
        return 0U;
    }
    return (s_host_pad_mode[y][x] != APC_PAD_LED_OFF) ? 15U : 0U;
}

void apc_mode_refresh_status_leds(void) {
    leds_set_round(APC_ROUND_GROUP_SCENE, 2U,
                   lua_vm_is_enabled() ? APC_ROUND_LED_ON : APC_ROUND_LED_OFF);
}

void apc_mode_on_button(apc_btn_group_t group, uint8_t index, bool pressed) {
    uint8_t note = 0;

    switch (group) {
    case APC_BTN_GROUP_GRID:
        note = (uint8_t)(APC_NOTE_GRID_BASE + index);
        break;
    case APC_BTN_GROUP_TRACK:
        note = (uint8_t)(APC_NOTE_TRACK_BASE + index);
        break;
    case APC_BTN_GROUP_SCENE:
        note = (uint8_t)(APC_NOTE_SCENE_BASE + index);
        break;
    case APC_BTN_GROUP_SHIFT:
        note = APC_NOTE_SHIFT;
        break;
    }

    uint8_t cin = pressed ? USB_MIDI_CIN_NOTE_ON : USB_MIDI_CIN_NOTE_OFF;
    uint8_t status = pressed ? MIDI_STATUS_NOTE_ON : MIDI_STATUS_NOTE_OFF;
    usb_midi_send_event(cin, status, note, APC_DEFAULT_VELOCITY);
}

void apc_mode_on_fader(uint8_t fader_idx, uint8_t value) {
    if (fader_idx >= APC_FADER_COUNT) {
        return;
    }
    uint8_t cc = (uint8_t)(APC_CC_FADER_BASE + fader_idx);
    usb_midi_send_event(USB_MIDI_CIN_CONTROL_CHANGE, MIDI_STATUS_CONTROL_CHANGE, cc, value);
}

void apc_mode_poll_rx(uint32_t now_ticks) {
    s_rx_now_ticks = now_ticks;
    usb_midi_poll_rx(handle_midi_channel_msg, sysex_handle_msg);
}
