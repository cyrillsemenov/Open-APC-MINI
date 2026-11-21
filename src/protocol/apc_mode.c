#include "apc_config.h"
#include "apc_protocol.h"
#include "apc_mode.h"
#include "board.h"
#include "bootloader.h"
#include "leds.h"
#include "lua_vm.h"
#include "monome_mode.h"
#include "sysex.h"
#include "usb_midi.h"

#define APC_CONFIG_MAGIC 0x41504331UL // "APC1"

typedef struct {
    uint32_t magic;
    uint8_t mode_flags;
    uint8_t palette;
    uint8_t brightness;
    uint8_t checksum;
} apc_persist_cfg_t;

static uint32_t s_rx_now_ticks = 0;
static uint8_t s_mode_flags = APC_MODE_FLAGS_DEFAULT;
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
    monome_mode_refresh_pad(x, y, now_ticks);
}

apc_pad_led_mode_t apc_mode_get_pad_mode(uint8_t x, uint8_t y) {
    if (x >= 8U || y >= 8U) {
        return APC_PAD_LED_OFF;
    }
    return s_host_pad_mode[y][x];
}

void apc_mode_clear_pad_mode(uint8_t x, uint8_t y) {
    if (x < 8U && y < 8U) {
        s_host_pad_mode[y][x] = APC_PAD_LED_OFF;
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
        monome_mode_clear_grid_pad(col, row);
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
    if ((s_mode_flags & APC_MODE_FLAG_MIDI) == 0U) {
        return;
    }
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
    s_mode_flags = APC_MODE_FLAGS_DEFAULT;
    for (uint8_t y = 0; y < 8U; y++) {
        for (uint8_t x = 0; x < 8U; x++) {
            s_host_pad_mode[y][x] = APC_PAD_LED_OFF;
            s_overlay_level[y][x] = 0U;
        }
    }
    sysex_init();
}

void apc_mode_load_config(uint32_t now_ticks) {
    const volatile apc_persist_cfg_t *cfg =
        (const volatile apc_persist_cfg_t *)(uintptr_t)_config_flash_page;
    uint8_t flags = cfg->mode_flags;
    uint8_t pal = cfg->palette;
    uint8_t bri = cfg->brightness;
    if (cfg->magic != APC_CONFIG_MAGIC || flags > 0x0FU || pal >= 4U || bri >= 4U ||
        cfg->checksum != (uint8_t)~(flags ^ pal ^ bri)) {
        return;
    }

    monome_mode_set_palette(pal, now_ticks);
    monome_mode_set_brightness(bri, now_ticks);
    if ((board_reset_flags() & 0x20U) != 0U) {
        // Safe mode after watchdog reset: keep saved config in Flash, skip Lua auto-start
        flags &= (uint8_t)~APC_MODE_FLAG_LUA;
    }
    apc_mode_set_flags(flags, now_ticks);
}

void apc_mode_save_config(void) {
    uint8_t flags = apc_mode_get_flags();
    uint8_t pal = monome_mode_get_palette();
    uint8_t bri = monome_mode_get_brightness();
    uint8_t csum = (uint8_t)~(flags ^ pal ^ bri);
    const volatile apc_persist_cfg_t *cur =
        (const volatile apc_persist_cfg_t *)(uintptr_t)_config_flash_page;
    if (cur->magic == APC_CONFIG_MAGIC && cur->mode_flags == flags && cur->palette == pal &&
        cur->brightness == bri && cur->checksum == csum) {
        return;
    }

    apc_persist_cfg_t next = {
        .magic = APC_CONFIG_MAGIC,
        .mode_flags = flags,
        .palette = pal,
        .brightness = bri,
        .checksum = csum,
    };
    bootloader_write_flash_page((uint32_t)(uintptr_t)_config_flash_page, (const uint8_t *)&next,
                                (uint16_t)sizeof(next));
}

uint8_t apc_mode_get_flags(void) {
    if (lua_vm_is_enabled()) {
        return (uint8_t)(s_mode_flags | APC_MODE_FLAG_LUA);
    }
    return (uint8_t)(s_mode_flags & (uint8_t)~APC_MODE_FLAG_LUA);
}

void apc_mode_set_flags(uint8_t flags, uint32_t now_ticks) {
    uint8_t next = (uint8_t)(flags & 0x0FU);
    uint8_t prev = apc_mode_get_flags();
    s_mode_flags = next;

    if (((prev & APC_MODE_FLAG_MIDI) != 0U) && ((next & APC_MODE_FLAG_MIDI) == 0U)) {
        for (uint8_t y = 0; y < 8U; y++) {
            for (uint8_t x = 0; x < 8U; x++) {
                s_host_pad_mode[y][x] = APC_PAD_LED_OFF;
                refresh_apc_pad(x, y, now_ticks);
            }
            leds_set_round(APC_ROUND_GROUP_TRACK, y, APC_ROUND_LED_OFF);
            leds_set_round(APC_ROUND_GROUP_SCENE, y, APC_ROUND_LED_OFF);
        }
    }
    if (((prev & APC_MODE_FLAG_SERIALOSC) != 0U) && ((next & APC_MODE_FLAG_SERIALOSC) == 0U)) {
        for (uint8_t y = 0; y < 8U; y++) {
            for (uint8_t x = 0; x < 8U; x++) {
                monome_mode_clear_grid_pad(x, y);
                refresh_apc_pad(x, y, now_ticks);
            }
        }
    }
    if (((prev ^ next) & APC_MODE_FLAG_LUA) != 0U) {
        lua_vm_toggle(now_ticks);
    }
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
