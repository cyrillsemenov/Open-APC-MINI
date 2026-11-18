#include <stdbool.h>
#include <stdint.h>
#include "apc_config.h"
#include "apc_mode.h"
#include "apc_protocol.h"
#include "bootloader.h"
#include "buttons.h"
#include "leds.h"
#include "lua_vm.h"
#include "monome_mode.h"
#include "sysex.h"
#include "usb_midi.h"

/*---------------------------------------------------------------------------
 * Monome mext (v4) Serial Protocol Opcodes
 *---------------------------------------------------------------------------*/
#define MEXT_CMD_SYS_QUERY     0x00U
#define MEXT_CMD_SYS_GET_ID    0x01U
#define MEXT_CMD_SYS_SET_ID    0x02U
#define MEXT_CMD_SYS_GET_SIZE  0x03U
#define MEXT_CMD_SYS_SET_SIZE  0x04U
#define MEXT_CMD_SYS_GET_ADDR  0x05U
#define MEXT_CMD_SYS_GET_VER   0x08U

#define MEXT_REP_SYS_QUERY     0x00U
#define MEXT_REP_SYS_ID        0x01U
#define MEXT_REP_SYS_SIZE      0x02U
#define MEXT_REP_SYS_ADDR      0x03U
#define MEXT_REP_SYS_VER       0x0FU

#define MEXT_CMD_LED_OFF       0x10U
#define MEXT_CMD_LED_ON        0x11U
#define MEXT_CMD_LED_ALL_OFF   0x12U
#define MEXT_CMD_LED_ALL_ON    0x13U
#define MEXT_CMD_LED_MAP       0x14U
#define MEXT_CMD_LED_ROW       0x15U
#define MEXT_CMD_LED_COL       0x16U
#define MEXT_CMD_LED_INTENSITY 0x17U
#define MEXT_CMD_LED_LEVEL_SET 0x18U
#define MEXT_CMD_LED_LEVEL_ALL 0x19U
#define MEXT_CMD_LED_LEVEL_MAP 0x1AU
#define MEXT_CMD_LED_LEVEL_ROW 0x1BU
#define MEXT_CMD_LED_LEVEL_COL 0x1CU

#define MEXT_EV_KEY_UP         0x20U
#define MEXT_EV_KEY_DOWN       0x21U

#define MEXT_CMD_TILT_DISABLE  0x80U
#define MEXT_CMD_TILT_ENABLE   0x81U
#define MEXT_EV_TILT           0x81U

#define MONOME_LVL_1BIT_ON     16U

static uint8_t s_device_id[32];
static uint8_t s_grid_level[8][8];    // 0 = off, 1..15 = varibright, 16 = 1-bit ON
static uint8_t s_overlay_level[8][8]; // Lua Co-Processor overlay (0 = transparent)
static uint8_t s_palette = 0;         // 0..3 color palettes
static uint8_t s_bright_idx = 3;      // 0..3 -> TM1629 brightness 0, 1, 2, 7
static uint8_t s_tilt_mask = 0x01U;
static uint8_t s_fader_vals[APC_FADER_COUNT];

static uint8_t s_rx_cmd = 0;
static uint8_t s_rx_need = 0;
static uint8_t s_rx_pos = 0;
static uint8_t s_rx_buf[36];
static uint8_t s_sysex_buf[64];
static uint8_t s_sysex_pos = 0;

// TM1629 duty cycle is non-linear: 0=1/16, 1=2/16, 2=4/16, 3..7=10/16..14/16.
// Use exponential steps {0 (1/16), 1 (2/16), 2 (4/16), 7 (14/16)} for Track 5..8 presets.
static const uint8_t k_bright_levels[4] = {0U, 1U, 2U, 7U};
static const uint8_t k_intensity_to_tm[16] = {
    0U, 0U, 0U, 1U, 1U, 1U, 2U, 2U, 2U, 2U, 3U, 4U, 5U, 6U, 7U, 7U,
};

apc_pad_led_mode_t monome_map_level_to_pad_mode(uint8_t level, uint8_t palette) {
    if (level == 0U) {
        return APC_PAD_LED_OFF;
    }

    switch (palette & 0x03U) {
    case 0U: // Default: 1..5 Green, 6..10 Yellow, 11..15 Red; 1-bit ON = Yellow
        if (level == MONOME_LVL_1BIT_ON) {
            return APC_PAD_LED_YELLOW;
        }
        if (level <= 5U) {
            return APC_PAD_LED_GREEN;
        }
        if (level <= 10U) {
            return APC_PAD_LED_YELLOW;
        }
        return APC_PAD_LED_RED;

    case 1U: // Reverse: 1..5 Red, 6..10 Yellow, 11..15 Green; 1-bit ON = Green
        if (level == MONOME_LVL_1BIT_ON) {
            return APC_PAD_LED_GREEN;
        }
        if (level <= 5U) {
            return APC_PAD_LED_RED;
        }
        if (level <= 10U) {
            return APC_PAD_LED_YELLOW;
        }
        return APC_PAD_LED_GREEN;

    case 2U: // Warm Green/Yellow: 1..7 Green, 8..15 Yellow; 1-bit ON = Yellow
        if (level == MONOME_LVL_1BIT_ON || level >= 8U) {
            return APC_PAD_LED_YELLOW;
        }
        return APC_PAD_LED_GREEN;

    default: // Warm Red/Yellow: 1..7 Red, 8..15 Yellow; 1-bit ON = Red
        if (level == MONOME_LVL_1BIT_ON || level <= 7U) {
            return APC_PAD_LED_RED;
        }
        return APC_PAD_LED_YELLOW;
    }
}

void monome_mode_refresh_pad(uint8_t x, uint8_t y, uint32_t now_ticks) {
    if (x >= 8U || y >= 8U) {
        return;
    }
    uint8_t lvl = s_overlay_level[y][x];
    if (lvl == 0U) {
        lvl = s_grid_level[y][x];
    }
    apc_pad_led_mode_t mode;
    if (lvl != 0U) {
        mode = monome_map_level_to_pad_mode(lvl, s_palette);
    } else {
        mode = apc_mode_get_pad_mode(x, y);
    }
    leds_set_pad(y, x, mode, now_ticks);
}

void monome_mode_clear_grid_pad(uint8_t x, uint8_t y) {
    if (x < 8U && y < 8U) {
        s_grid_level[y][x] = 0U;
    }
}

static void refresh_all_pad_leds(uint32_t now_ticks) {
    for (uint8_t y = 0; y < 8U; y++) {
        for (uint8_t x = 0; x < 8U; x++) {
            monome_mode_refresh_pad(x, y, now_ticks);
        }
    }
}

static void set_grid_pad_level(uint8_t x, uint8_t y, uint8_t level, uint32_t now_ticks) {
    if (x >= 8U || y >= 8U || (apc_mode_get_flags() & APC_MODE_FLAG_SERIALOSC) == 0U) {
        return;
    }
    s_grid_level[y][x] = level;
    apc_mode_clear_pad_mode(x, y);
    monome_mode_refresh_pad(x, y, now_ticks);
}

uint8_t monome_mode_get_palette(void) {
    return s_palette;
}

void monome_mode_set_palette(uint8_t palette, uint32_t now_ticks) {
    s_palette = (uint8_t)(palette & 0x03U);
    refresh_all_pad_leds(now_ticks);
}

uint8_t monome_mode_get_brightness(void) {
    return s_bright_idx;
}

void monome_mode_set_brightness(uint8_t bright_idx, uint32_t now_ticks) {
    s_bright_idx = (uint8_t)(bright_idx & 0x03U);
    leds_set_brightness(k_bright_levels[s_bright_idx], now_ticks);
}

static uint8_t cmd_payload_len(uint8_t cmd) {
    switch (cmd) {
    case MEXT_CMD_SYS_QUERY:
    case MEXT_CMD_SYS_GET_ID:
    case MEXT_CMD_SYS_GET_SIZE:
    case MEXT_CMD_SYS_GET_ADDR:
    case MEXT_CMD_SYS_GET_VER:
    case MEXT_CMD_LED_ALL_OFF:
    case MEXT_CMD_LED_ALL_ON:
        return 0U;
    case MEXT_CMD_LED_INTENSITY:
    case MEXT_CMD_LED_LEVEL_ALL:
    case MEXT_CMD_TILT_DISABLE:
    case MEXT_CMD_TILT_ENABLE:
        return 1U;
    case MEXT_CMD_SYS_SET_SIZE:
    case MEXT_CMD_LED_OFF:
    case MEXT_CMD_LED_ON:
        return 2U;
    case MEXT_CMD_LED_ROW:
    case MEXT_CMD_LED_COL:
    case MEXT_CMD_LED_LEVEL_SET:
        return 3U;
    case MEXT_CMD_LED_LEVEL_ROW:
    case MEXT_CMD_LED_LEVEL_COL:
        return 5U;
    case MEXT_CMD_LED_MAP:
        return 10U;
    case MEXT_CMD_SYS_SET_ID:
        return 32U;
    case MEXT_CMD_LED_LEVEL_MAP:
        return 34U;
    default:
        return 0xFFU;
    }
}

static void send_sys_id_reply(void) {
    uint8_t rep[33];
    rep[0] = MEXT_REP_SYS_ID;
    for (uint8_t i = 0; i < 32U; i++) {
        rep[1U + i] = s_device_id[i];
    }
    usb_serial_write(rep, sizeof(rep));
}

static void execute_mext_cmd(uint8_t cmd, const uint8_t *p, uint32_t now_ticks) {
    switch (cmd) {
    case MEXT_CMD_SYS_QUERY: {
        // Section 1 = led-grid (1), Section 2 = key-grid (1)
        static const uint8_t k_query_rep[6] = {
            MEXT_REP_SYS_QUERY, 1U, 1U, MEXT_REP_SYS_QUERY, 2U, 1U,
        };
        usb_serial_write(k_query_rep, sizeof(k_query_rep));
        break;
    }

    case MEXT_CMD_SYS_GET_ID:
        send_sys_id_reply();
        break;

    case MEXT_CMD_SYS_SET_ID:
        for (uint8_t i = 0; i < 32U; i++) {
            s_device_id[i] = p[i];
        }
        send_sys_id_reply();
        break;

    case MEXT_CMD_SYS_GET_SIZE: {
        static const uint8_t k_size_rep[3] = {MEXT_REP_SYS_SIZE, 8U, 8U};
        usb_serial_write(k_size_rep, sizeof(k_size_rep));
        break;
    }

    case MEXT_CMD_SYS_GET_ADDR: {
        static const uint8_t k_addr_rep[3] = {MEXT_REP_SYS_ADDR, 8U, 8U};
        usb_serial_write(k_addr_rep, sizeof(k_addr_rep));
        break;
    }

    case MEXT_CMD_SYS_GET_VER: {
        static const uint8_t k_ver_rep[9] = {
            MEXT_REP_SYS_VER, 'v', '1', '.', '0', '.', '0', 0U, 0U,
        };
        usb_serial_write(k_ver_rep, sizeof(k_ver_rep));
        break;
    }

    case MEXT_CMD_LED_OFF:
        set_grid_pad_level(p[0], p[1], 0U, now_ticks);
        break;

    case MEXT_CMD_LED_ON:
        set_grid_pad_level(p[0], p[1], MONOME_LVL_1BIT_ON, now_ticks);
        break;

    case MEXT_CMD_LED_ALL_OFF:
        for (uint8_t y = 0; y < 8U; y++) {
            for (uint8_t x = 0; x < 8U; x++) {
                set_grid_pad_level(x, y, 0U, now_ticks);
            }
        }
        break;

    case MEXT_CMD_LED_ALL_ON:
        for (uint8_t y = 0; y < 8U; y++) {
            for (uint8_t x = 0; x < 8U; x++) {
                set_grid_pad_level(x, y, MONOME_LVL_1BIT_ON, now_ticks);
            }
        }
        break;

    case MEXT_CMD_LED_MAP:
        if (p[0] == 0U && p[1] == 0U) {
            for (uint8_t y = 0; y < 8U; y++) {
                uint8_t row_mask = p[2U + y];
                for (uint8_t x = 0; x < 8U; x++) {
                    uint8_t lvl = ((row_mask & (1U << x)) != 0U) ? MONOME_LVL_1BIT_ON : 0U;
                    set_grid_pad_level(x, y, lvl, now_ticks);
                }
            }
        }
        break;

    case MEXT_CMD_LED_ROW:
        if (p[0] == 0U && p[1] < 8U) {
            uint8_t y = p[1];
            uint8_t mask = p[2];
            for (uint8_t x = 0; x < 8U; x++) {
                uint8_t lvl = ((mask & (1U << x)) != 0U) ? MONOME_LVL_1BIT_ON : 0U;
                set_grid_pad_level(x, y, lvl, now_ticks);
            }
        }
        break;

    case MEXT_CMD_LED_COL:
        if (p[0] < 8U && p[1] == 0U) {
            uint8_t x = p[0];
            uint8_t mask = p[2];
            for (uint8_t y = 0; y < 8U; y++) {
                uint8_t lvl = ((mask & (1U << y)) != 0U) ? MONOME_LVL_1BIT_ON : 0U;
                set_grid_pad_level(x, y, lvl, now_ticks);
            }
        }
        break;

    case MEXT_CMD_LED_INTENSITY:
        leds_set_brightness(k_intensity_to_tm[p[0] & 0x0FU], now_ticks);
        break;

    case MEXT_CMD_LED_LEVEL_SET:
        set_grid_pad_level(p[0], p[1], (uint8_t)(p[2] & 0x0FU), now_ticks);
        break;

    case MEXT_CMD_LED_LEVEL_ALL: {
        uint8_t lvl = (uint8_t)(p[0] & 0x0FU);
        for (uint8_t y = 0; y < 8U; y++) {
            for (uint8_t x = 0; x < 8U; x++) {
                set_grid_pad_level(x, y, lvl, now_ticks);
            }
        }
        break;
    }

    case MEXT_CMD_LED_LEVEL_MAP:
        if (p[0] == 0U && p[1] == 0U) {
            for (uint8_t i = 0; i < 32U; i++) {
                uint8_t y = (uint8_t)(i >> 2U);
                uint8_t x0 = (uint8_t)((i & 3U) << 1U);
                uint8_t byte = p[2U + i];
                set_grid_pad_level(x0, y, (uint8_t)((byte >> 4U) & 0x0FU), now_ticks);
                set_grid_pad_level((uint8_t)(x0 + 1U), y, (uint8_t)(byte & 0x0FU), now_ticks);
            }
        }
        break;

    case MEXT_CMD_LED_LEVEL_ROW:
        if (p[0] == 0U && p[1] < 8U) {
            uint8_t y = p[1];
            for (uint8_t i = 0; i < 4U; i++) {
                uint8_t x0 = (uint8_t)(i << 1U);
                uint8_t byte = p[2U + i];
                set_grid_pad_level(x0, y, (uint8_t)((byte >> 4U) & 0x0FU), now_ticks);
                set_grid_pad_level((uint8_t)(x0 + 1U), y, (uint8_t)(byte & 0x0FU), now_ticks);
            }
        }
        break;

    case MEXT_CMD_LED_LEVEL_COL:
        if (p[0] < 8U && p[1] == 0U) {
            uint8_t x = p[0];
            for (uint8_t i = 0; i < 4U; i++) {
                uint8_t y0 = (uint8_t)(i << 1U);
                uint8_t byte = p[2U + i];
                set_grid_pad_level(x, y0, (uint8_t)((byte >> 4U) & 0x0FU), now_ticks);
                set_grid_pad_level(x, (uint8_t)(y0 + 1U), (uint8_t)(byte & 0x0FU), now_ticks);
            }
        }
        break;

    case MEXT_CMD_TILT_DISABLE:
    case MEXT_CMD_TILT_ENABLE: {
        uint8_t val = p[0];
        if (cmd == MEXT_CMD_TILT_ENABLE) {
            if (val == 0xDFU) {
                bootloader_enter_dfu();
                break;
            }
            if (val == MIDI_STATUS_SYSEX_START) {
                s_sysex_buf[0] = MIDI_STATUS_SYSEX_START;
                s_sysex_pos = 1U;
                break;
            }
            if (val == MIDI_STATUS_SYSEX_END && s_sysex_pos > 0U) {
                if (s_sysex_pos < sizeof(s_sysex_buf)) {
                    s_sysex_buf[s_sysex_pos++] = MIDI_STATUS_SYSEX_END;
                    sysex_handle_msg(s_sysex_buf, s_sysex_pos);
                }
                s_sysex_pos = 0U;
                break;
            }
            if (val < 4U) {
                s_tilt_mask |= (uint8_t)(1U << val);
            }
        } else {
            if (s_sysex_pos > 0U) {
                if (s_sysex_pos < (sizeof(s_sysex_buf) - 1U)) {
                    s_sysex_buf[s_sysex_pos++] = (uint8_t)(val & 0x7FU);
                } else {
                    s_sysex_pos = 0U;
                }
                break;
            }
            if (val < 4U) {
                s_tilt_mask &= (uint8_t)~(1U << val);
            }
        }
        break;
    }

    default:
        break;
    }
}

void monome_mode_init(uint32_t now_ticks) {
    static const char k_default_id[8] = {'A', 'P', 'C', ' ', 'M', 'I', 'N', 'I'};
    for (uint8_t i = 0; i < 32U; i++) {
        s_device_id[i] = (i < 8U) ? (uint8_t)k_default_id[i] : 0U;
    }
    for (uint8_t y = 0; y < 8U; y++) {
        for (uint8_t x = 0; x < 8U; x++) {
            s_grid_level[y][x] = 0U;
            s_overlay_level[y][x] = 0U;
        }
    }
    for (uint8_t i = 0; i < APC_FADER_COUNT; i++) {
        s_fader_vals[i] = 0U;
    }
    s_palette = 0U;
    s_bright_idx = 3U;
    s_tilt_mask = 0x01U;
    s_rx_cmd = 0U;
    s_rx_need = 0U;
    s_rx_pos = 0U;
    s_sysex_pos = 0U;

    leds_all_off(now_ticks);
    leds_set_brightness(k_bright_levels[s_bright_idx], now_ticks);
}

void monome_mode_set_overlay_led(uint8_t x, uint8_t y, uint8_t level, uint32_t now_ticks) {
    if (x >= 8U || y >= 8U) {
        return;
    }
    s_overlay_level[y][x] = level;
    monome_mode_refresh_pad(x, y, now_ticks);
}

void monome_mode_clear_overlay(uint32_t now_ticks) {
    for (uint8_t y = 0; y < 8U; y++) {
        for (uint8_t x = 0; x < 8U; x++) {
            s_overlay_level[y][x] = 0U;
            monome_mode_refresh_pad(x, y, now_ticks);
        }
    }
}

uint8_t monome_mode_get_host_led(uint8_t x, uint8_t y) {
    if (x >= 8U || y >= 8U) {
        return 0U;
    }
    if (s_grid_level[y][x] != 0U) {
        return s_grid_level[y][x];
    }
    return apc_mode_get_host_led(x, y);
}

void monome_mode_send_key(uint8_t x, uint8_t y, bool pressed) {
    if (x >= 8U || y >= 8U) {
        return;
    }
    uint8_t ev[3] = {
        pressed ? MEXT_EV_KEY_DOWN : MEXT_EV_KEY_UP,
        x,
        y,
    };
    usb_serial_write(ev, sizeof(ev));
}

void monome_mode_send_tilt(uint8_t sensor, int16_t x, int16_t y, int16_t z) {
    uint16_t ux = (uint16_t)x;
    uint16_t uy = (uint16_t)y;
    uint16_t uz = (uint16_t)z;
    uint8_t pkt[8] = {
        MEXT_EV_TILT,
        (uint8_t)(sensor & 0x03U),
        (uint8_t)(ux & 0xFFU),
        (uint8_t)((ux >> 8U) & 0xFFU),
        (uint8_t)(uy & 0xFFU),
        (uint8_t)((uy >> 8U) & 0xFFU),
        (uint8_t)(uz & 0xFFU),
        (uint8_t)((uz >> 8U) & 0xFFU),
    };
    usb_serial_write(pkt, sizeof(pkt));
}

void monome_mode_on_button(apc_btn_group_t group, uint8_t index, bool pressed, uint32_t now_ticks) {
    (void)now_ticks;
    if (group == APC_BTN_GROUP_GRID && index < 64U) {
        // APC grid index 0 is bottom-left (row=7, col=0); Monome (0,0) is top-left
        uint8_t x = (uint8_t)(index & 7U);
        uint8_t y = (uint8_t)(7U - (index >> 3U));
        monome_mode_send_key(x, y, pressed);
    }
}

void monome_mode_on_fader(uint8_t fader_idx, uint8_t value) {
    if (fader_idx >= APC_FADER_COUNT) {
        return;
    }
    s_fader_vals[fader_idx] = value;

    uint8_t sensor = (uint8_t)(fader_idx / 3U);
    if ((s_tilt_mask & (1U << sensor)) == 0U) {
        return;
    }

    uint8_t base = (uint8_t)(sensor * 3U);
    int16_t x = (int16_t)((uint16_t)s_fader_vals[base + 0U] << 7U);
    int16_t y = (int16_t)((uint16_t)s_fader_vals[base + 1U] << 7U);
    int16_t z = (int16_t)((uint16_t)s_fader_vals[base + 2U] << 7U);
    monome_mode_send_tilt(sensor, x, y, z);
}

void monome_mode_poll_rx(uint32_t now_ticks) {
    uint8_t b = 0;
    while (usb_serial_read_byte(&b)) {
        if (s_sysex_pos > 0U) {
            if (s_sysex_pos < sizeof(s_sysex_buf)) {
                s_sysex_buf[s_sysex_pos++] = b;
                if (b == MIDI_STATUS_SYSEX_END) {
                    sysex_handle_msg(s_sysex_buf, s_sysex_pos);
                    s_sysex_pos = 0U;
                }
            } else {
                s_sysex_pos = 0U;
            }
            continue;
        }

        if (s_rx_need == 0U) {
            if (b == MIDI_STATUS_SYSEX_START) {
                s_sysex_buf[0] = b;
                s_sysex_pos = 1U;
                continue;
            }
            uint8_t need = cmd_payload_len(b);
            if (need == 0xFFU) {
                continue;
            }
            if (need == 0U) {
                execute_mext_cmd(b, s_rx_buf, now_ticks);
            } else {
                s_rx_cmd = b;
                s_rx_need = need;
                s_rx_pos = 0U;
            }
        } else {
            s_rx_buf[s_rx_pos++] = b;
            if (s_rx_pos >= s_rx_need) {
                execute_mext_cmd(s_rx_cmd, s_rx_buf, now_ticks);
                s_rx_need = 0U;
                s_rx_pos = 0U;
            }
        }
    }
}
