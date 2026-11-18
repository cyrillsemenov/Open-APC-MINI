#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "apc_config.h"
#include "apc_mode.h"
#include "apc_protocol.h"
#include "board.h"
#include "bootloader.h"
#include "buttons.h"
#include "faders.h"
#include "leds.h"
#include "lua_vm.h"
#include "monome_mode.h"
#include "usb_desc.h"
#include "usb_midi.h"

#define LUA_MAX_STEPS   32768U
#define LUA_SLICE_STEPS 128U
#define LUA_STACK_SIZE  64U
#define LUA_NUM_LOCALS  32U
#define LUA_NUM_GLOBALS 64U
#define LUA_MEM_SIZE    256U
#define LUA_MAX_GATES   8U

typedef struct {
    bool active;
    uint16_t pc;
    uint8_t sp;
    uint16_t total_steps;
    uint32_t wake_tick;
    int32_t ret_val;
    int32_t stack[LUA_STACK_SIZE];
    int32_t locals[LUA_NUM_LOCALS];
} lua_coro_t;

typedef struct {
    bool active;
    uint8_t x;
    uint8_t y;
    uint32_t off_tick;
} lua_gate_t;

// Built-in default ROM script (compiled from scripts/bouncy_echo.lua)
static const uint8_t k_rom_script[385] = {
    0x4CU, 0x55U, 0x41U, 0x31U, 0x71U, 0x01U, 0x00U, 0x00U, 0x6DU, 0x00U, 0x0BU, 0x00U,
    0x55U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x07U, 0x00U, 0x02U, 0x37U, 0x40U, 0x09U,
    0x01U, 0x0AU, 0x00U, 0x04U, 0x02U, 0x02U, 0x01U, 0x20U, 0x31U, 0x51U, 0x00U, 0x06U,
    0x00U, 0x02U, 0x04U, 0x12U, 0x05U, 0x03U, 0x04U, 0x03U, 0x04U, 0x00U, 0x09U, 0x04U,
    0x03U, 0x02U, 0x01U, 0x10U, 0x04U, 0x01U, 0x09U, 0x04U, 0x03U, 0x02U, 0x02U, 0x10U,
    0x02U, 0x01U, 0x09U, 0x04U, 0x03U, 0x02U, 0x03U, 0x10U, 0x02U, 0x07U, 0x40U, 0x07U,
    0x01U, 0x02U, 0x05U, 0x1BU, 0x09U, 0x06U, 0x00U, 0x02U, 0x01U, 0x10U, 0x02U, 0x03U,
    0x17U, 0x07U, 0x00U, 0x04U, 0x00U, 0x04U, 0x01U, 0x02U, 0x0FU, 0x40U, 0x00U, 0x03U,
    0x0AU, 0x02U, 0x00U, 0x01U, 0x00U, 0x04U, 0x00U, 0x02U, 0x08U, 0x20U, 0x31U, 0x6CU,
    0x00U, 0x02U, 0x19U, 0x02U, 0x7FU, 0x04U, 0x01U, 0x11U, 0x02U, 0x01U, 0x1BU, 0x10U,
    0x40U, 0x09U, 0x01U, 0x0AU, 0x00U, 0x02U, 0x00U, 0x40U, 0x01U, 0x01U, 0x0AU, 0x02U,
    0x00U, 0x05U, 0x01U, 0x02U, 0x03U, 0x05U, 0x02U, 0x04U, 0x01U, 0x04U, 0x02U, 0x23U,
    0x31U, 0x70U, 0x01U, 0x04U, 0x01U, 0x02U, 0x04U, 0x12U, 0x05U, 0x03U, 0x04U, 0x03U,
    0x02U, 0x02U, 0x10U, 0x08U, 0x05U, 0x04U, 0x04U, 0x04U, 0x02U, 0x00U, 0x24U, 0x31U,
    0x66U, 0x01U, 0x04U, 0x03U, 0x08U, 0x05U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U, 0x10U,
    0x08U, 0x05U, 0x06U, 0x04U, 0x03U, 0x02U, 0x03U, 0x10U, 0x08U, 0x05U, 0x07U, 0x02U,
    0x0FU, 0x04U, 0x04U, 0x02U, 0x04U, 0x12U, 0x11U, 0x05U, 0x08U, 0x04U, 0x08U, 0x02U,
    0x00U, 0x24U, 0x31U, 0x5EU, 0x01U, 0x04U, 0x05U, 0x04U, 0x04U, 0x11U, 0x02U, 0x00U,
    0x25U, 0x31U, 0xD9U, 0x00U, 0x04U, 0x05U, 0x04U, 0x04U, 0x11U, 0x04U, 0x06U, 0x04U,
    0x08U, 0x40U, 0x00U, 0x03U, 0x0AU, 0x04U, 0x05U, 0x04U, 0x04U, 0x10U, 0x02U, 0x07U,
    0x23U, 0x31U, 0xF1U, 0x00U, 0x04U, 0x05U, 0x04U, 0x04U, 0x10U, 0x04U, 0x06U, 0x04U,
    0x08U, 0x40U, 0x00U, 0x03U, 0x0AU, 0x04U, 0x06U, 0x04U, 0x04U, 0x11U, 0x02U, 0x00U,
    0x25U, 0x31U, 0x09U, 0x01U, 0x04U, 0x05U, 0x04U, 0x06U, 0x04U, 0x04U, 0x11U, 0x04U,
    0x08U, 0x40U, 0x00U, 0x03U, 0x0AU, 0x04U, 0x06U, 0x04U, 0x04U, 0x10U, 0x02U, 0x07U,
    0x23U, 0x31U, 0x21U, 0x01U, 0x04U, 0x05U, 0x04U, 0x06U, 0x04U, 0x04U, 0x10U, 0x04U,
    0x08U, 0x40U, 0x00U, 0x03U, 0x0AU, 0x04U, 0x07U, 0x02U, 0x00U, 0x24U, 0x04U, 0x04U,
    0x04U, 0x07U, 0x23U, 0x02U, 0x00U, 0x21U, 0x17U, 0x31U, 0x50U, 0x01U, 0x04U, 0x05U,
    0x04U, 0x04U, 0x10U, 0x02U, 0x07U, 0x17U, 0x05U, 0x09U, 0x04U, 0x09U, 0x04U, 0x06U,
    0x02U, 0x01U, 0x40U, 0x02U, 0x03U, 0x0AU, 0x04U, 0x09U, 0x04U, 0x06U, 0x02U, 0x00U,
    0x40U, 0x02U, 0x03U, 0x0AU, 0x04U, 0x03U, 0x02U, 0x02U, 0x10U, 0x04U, 0x04U, 0x02U,
    0x01U, 0x10U, 0x09U, 0x30U, 0x66U, 0x01U, 0x04U, 0x03U, 0x02U, 0x02U, 0x10U, 0x02U,
    0x00U, 0x09U, 0x04U, 0x01U, 0x02U, 0x01U, 0x10U, 0x05U, 0x01U, 0x30U, 0x7BU, 0x00U,
    0x00U,
};

static bool s_enabled = false;
static const uint8_t *s_active_image = k_rom_script;
static uint8_t s_ram_image[LUA_MAX_IMAGE_LEN];
static uint16_t s_upload_expected = 0U;
static uint16_t s_upload_received = 0U;

static int16_t s_globals[LUA_NUM_GLOBALS];
static int16_t s_mem[LUA_MEM_SIZE];
static uint8_t s_fader_vals[APC_FADER_COUNT];
static lua_coro_t s_tick_coro;
static lua_gate_t s_gates[LUA_MAX_GATES];

static uint32_t s_last_tick = 0U;
static uint32_t s_tick_interval = 110U; // 55 ms in 0.5 ms SysTick units
static int32_t s_step_counter = 0;
static uint32_t s_prng = 0xA55A3C69UL;

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static bool is_valid_lua_image(const uint8_t *img, uint16_t max_len) {
    if (img == NULL || max_len < LUA_HEADER_SIZE) {
        return false;
    }
    if (img[0] != 'L' || img[1] != 'U' || img[2] != 'A' || img[3] != '1') {
        return false;
    }
    uint16_t code_len = read_u16(&img[4]);
    return (code_len > 0U) && ((uint32_t)LUA_HEADER_SIZE + (uint32_t)code_len <= (uint32_t)max_len);
}

static void refresh_mode_status_leds(void) {
    if (usb_get_profile() == USB_PROFILE_MONOME_FTDI) {
        monome_mode_refresh_status_leds();
    } else {
        apc_mode_refresh_status_leds();
    }
}

static void set_overlay_led(uint8_t x, uint8_t y, uint8_t level, uint32_t now_ticks) {
    if (usb_get_profile() == USB_PROFILE_MONOME_FTDI) {
        monome_mode_set_overlay_led(x, y, level, now_ticks);
    } else {
        apc_mode_set_overlay_led(x, y, level, now_ticks);
    }
}

static void clear_overlay_leds(uint32_t now_ticks) {
    monome_mode_clear_overlay(now_ticks);
    apc_mode_clear_overlay(now_ticks);
}

static void emit_grid_key_event(uint8_t x, uint8_t y, bool pressed) {
    if (x >= 8U || y >= 8U) {
        return;
    }
    if (usb_get_profile() == USB_PROFILE_MONOME_FTDI) {
        monome_mode_send_key(x, y, pressed);
    }
    uint8_t note = (uint8_t)((7U - y) * 8U + x);
    uint8_t cin = pressed ? USB_MIDI_CIN_NOTE_ON : USB_MIDI_CIN_NOTE_OFF;
    uint8_t st = pressed ? MIDI_STATUS_NOTE_ON : MIDI_STATUS_NOTE_OFF;
    usb_midi_send_event(cin, st, note, pressed ? APC_DEFAULT_VELOCITY : 0U);
}

static void flush_all_gates(void) {
    for (uint8_t i = 0U; i < LUA_MAX_GATES; i++) {
        if (s_gates[i].active) {
            s_gates[i].active = false;
            emit_grid_key_event(s_gates[i].x, s_gates[i].y, false);
        }
    }
}

static void service_expired_gates(uint32_t now_ticks) {
    for (uint8_t i = 0U; i < LUA_MAX_GATES; i++) {
        if (s_gates[i].active && (int32_t)(now_ticks - s_gates[i].off_tick) >= 0) {
            s_gates[i].active = false;
            emit_grid_key_event(s_gates[i].x, s_gates[i].y, false);
        }
    }
}

static void schedule_key_pulse(uint8_t x, uint8_t y, int32_t ms, uint32_t now_ticks) {
    if (x >= 8U || y >= 8U) {
        return;
    }
    emit_grid_key_event(x, y, true);
    if (ms <= 0) {
        emit_grid_key_event(x, y, false);
        return;
    }
    if (ms > 4000) {
        ms = 4000;
    }
    uint32_t off_tick = now_ticks + ((uint32_t)ms * 2U);

    // Reuse existing gate slot for the same pad or first free slot
    uint8_t victim = 0U;
    for (uint8_t i = 0U; i < LUA_MAX_GATES; i++) {
        if (s_gates[i].active && s_gates[i].x == x && s_gates[i].y == y) {
            s_gates[i].off_tick = off_tick;
            return;
        }
        if (!s_gates[i].active) {
            victim = i;
            break;
        }
    }
    if (s_gates[victim].active) {
        emit_grid_key_event(s_gates[victim].x, s_gates[victim].y, false);
    }
    s_gates[victim].active = true;
    s_gates[victim].x = x;
    s_gates[victim].y = y;
    s_gates[victim].off_tick = off_tick;
}

static int32_t call_builtin(lua_coro_t *c, uint8_t fn_id, uint8_t argc, const int32_t *args,
                            uint32_t now_ticks, bool *out_yielded) {
    (void)argc;
    switch (fn_id) {
    case LUA_FN_LED: {
        int32_t x = args[0];
        int32_t y = args[1];
        int32_t v = args[2];
        if (x >= 0 && x < 8 && y >= 0 && y < 8) {
            if (v < 0) {
                v = 0;
            } else if (v > 16) {
                v = 16;
            }
            set_overlay_led((uint8_t)x, (uint8_t)y, (uint8_t)v, now_ticks);
        }
        return 0;
    }

    case LUA_FN_LED_ALL: {
        int32_t v = args[0];
        if (v < 0) {
            v = 0;
        } else if (v > 16) {
            v = 16;
        }
        for (uint8_t y = 0; y < 8U; y++) {
            for (uint8_t x = 0; x < 8U; x++) {
                set_overlay_led(x, y, (uint8_t)v, now_ticks);
            }
        }
        return 0;
    }

    case LUA_FN_GRID_KEY: {
        int32_t x = args[0];
        int32_t y = args[1];
        bool pressed = (args[2] != 0);
        if (x >= 0 && x < 8 && y >= 0 && y < 8) {
            emit_grid_key_event((uint8_t)x, (uint8_t)y, pressed);
        }
        return 0;
    }

    case LUA_FN_TILT:
        monome_mode_send_tilt((uint8_t)(args[0] & 3), (int16_t)args[1], (int16_t)args[2],
                              (int16_t)args[3]);
        return 0;

    case LUA_FN_NOTE_ON: {
        uint8_t ch = (uint8_t)(args[0] & 0x0FU);
        uint8_t note = (uint8_t)(args[1] & 0x7FU);
        uint8_t vel = (uint8_t)(args[2] & 0x7FU);
        if (usb_get_profile() == USB_PROFILE_MONOME_FTDI && note < 64U) {
            monome_mode_send_key((uint8_t)(note & 7U), (uint8_t)(7U - (note >> 3U)), vel > 0U);
        }
        usb_midi_send_event(USB_MIDI_CIN_NOTE_ON, (uint8_t)(MIDI_STATUS_NOTE_ON | ch), note, vel);
        return 0;
    }

    case LUA_FN_NOTE_OFF: {
        uint8_t ch = (uint8_t)(args[0] & 0x0FU);
        uint8_t note = (uint8_t)(args[1] & 0x7FU);
        if (usb_get_profile() == USB_PROFILE_MONOME_FTDI && note < 64U) {
            monome_mode_send_key((uint8_t)(note & 7U), (uint8_t)(7U - (note >> 3U)), false);
        }
        usb_midi_send_event(USB_MIDI_CIN_NOTE_OFF, (uint8_t)(MIDI_STATUS_NOTE_OFF | ch), note, 0U);
        return 0;
    }

    case LUA_FN_CC: {
        uint8_t ch = (uint8_t)(args[0] & 0x0FU);
        uint8_t cc = (uint8_t)(args[1] & 0x7FU);
        uint8_t val = (uint8_t)(args[2] & 0x7FU);
        usb_midi_send_event(USB_MIDI_CIN_CONTROL_CHANGE,
                            (uint8_t)(MIDI_STATUS_CONTROL_CHANGE | ch), cc, val);
        return 0;
    }

    case LUA_FN_FADER: {
        int32_t idx = args[0];
        if (idx >= 0 && idx < (int32_t)APC_FADER_COUNT) {
            return (int32_t)s_fader_vals[idx];
        }
        return 0;
    }

    case LUA_FN_SHIFT:
        return buttons_shift_pressed() ? 1 : 0;

    case LUA_FN_SET_TEMPO: {
        int32_t ms = args[0];
        if (ms < 10) {
            ms = 10;
        } else if (ms > 2000) {
            ms = 2000;
        }
        s_tick_interval = (uint32_t)ms * 2U;
        return 0;
    }

    case LUA_FN_RAND: {
        int32_t limit = args[0];
        s_prng ^= s_prng << 13U;
        s_prng ^= s_prng >> 17U;
        s_prng ^= s_prng << 5U;
        if (limit <= 0) {
            return 0;
        }
        return (int32_t)(s_prng % (uint32_t)limit);
    }

    case LUA_FN_ABS:
        return (args[0] < 0) ? -args[0] : args[0];

    case LUA_FN_MIN:
        return (args[0] < args[1]) ? args[0] : args[1];

    case LUA_FN_MAX:
        return (args[0] > args[1]) ? args[0] : args[1];

    case LUA_FN_HOST_LED: {
        int32_t x = args[0];
        int32_t y = args[1];
        if (x < 0 || x >= 8 || y < 0 || y >= 8) {
            return 0;
        }
        if (usb_get_profile() == USB_PROFILE_MONOME_FTDI) {
            return (int32_t)monome_mode_get_host_led((uint8_t)x, (uint8_t)y);
        }
        return (int32_t)apc_mode_get_host_led((uint8_t)x, (uint8_t)y);
    }

    case LUA_FN_LIFE_STEP: {
        int32_t alive_cnt = 0;
        bool is_monome = (usb_get_profile() == USB_PROFILE_MONOME_FTDI);
        for (uint8_t y = 0U; y < 8U; y++) {
            uint8_t ym = (uint8_t)(((y - 1U) & 7U) << 3U);
            uint8_t y0 = (uint8_t)(y << 3U);
            uint8_t yp = (uint8_t)(((y + 1U) & 7U) << 3U);
            for (uint8_t x = 0U; x < 8U; x++) {
                uint8_t xm = (uint8_t)((x - 1U) & 7U);
                uint8_t xp = (uint8_t)((x + 1U) & 7U);
                uint8_t n = 0U;
                if (s_mem[ym + xm] > 0) n++;
                if (s_mem[ym + x] > 0)  n++;
                if (s_mem[ym + xp] > 0) n++;
                if (s_mem[y0 + xm] > 0) n++;
                if (s_mem[y0 + xp] > 0) n++;
                if (s_mem[yp + xm] > 0) n++;
                if (s_mem[yp + x] > 0)  n++;
                if (s_mem[yp + xp] > 0) n++;
                uint8_t hled = is_monome ? monome_mode_get_host_led(x, y)
                                         : apc_mode_get_host_led(x, y);
                if (hled > 0U) {
                    n++;
                }
                uint8_t idx = (uint8_t)(y0 + x);
                int16_t next_st = 0;
                if (s_mem[idx] > 0) {
                    if (n == 2U || n == 3U) {
                        next_st = 1;
                        alive_cnt++;
                    }
                } else if (n == 3U) {
                    next_st = 2;
                    alive_cnt++;
                }
                s_mem[64U + idx] = next_st;
            }
        }
        return alive_cnt;
    }

    case LUA_FN_YIELD:
        if (out_yielded != NULL) {
            *out_yielded = true;
        }
        return 0;

    case LUA_FN_SLEEP_MS: {
        int32_t ms = args[0];
        if (ms > 0) {
            if (ms > 4000) {
                ms = 4000;
            }
            c->wake_tick = now_ticks + ((uint32_t)ms * 2U);
            if (out_yielded != NULL) {
                *out_yielded = true;
            }
        }
        return 0;
    }

    case LUA_FN_KEY_PULSE: {
        int32_t x = args[0];
        int32_t y = args[1];
        int32_t ms = args[2];
        if (x >= 0 && x < 8 && y >= 0 && y < 8) {
            schedule_key_pulse((uint8_t)x, (uint8_t)y, ms, now_ticks);
        }
        return 0;
    }

    default:
        return 0;
    }
}

static bool init_coro(lua_coro_t *c, uint8_t header_entry_off, int32_t a0, int32_t a1, int32_t a2,
                      uint32_t now_ticks) {
    if (s_active_image == NULL) {
        c->active = false;
        return false;
    }
    uint16_t code_len = read_u16(&s_active_image[4]);
    uint16_t pc = read_u16(&s_active_image[header_entry_off]);
    if (pc == LUA_ENTRY_NONE || pc >= code_len) {
        c->active = false;
        return false;
    }
    c->active = true;
    c->pc = pc;
    c->sp = 0U;
    c->total_steps = 0U;
    c->wake_tick = now_ticks;
    c->ret_val = 0;
    for (uint8_t i = 0; i < LUA_NUM_LOCALS; i++) {
        c->locals[i] = 0;
    }
    c->locals[0] = a0;
    c->locals[1] = a1;
    c->locals[2] = a2;
    return true;
}

static void step_coro(lua_coro_t *c, uint16_t slice_budget, uint32_t now_ticks) {
    if (!c->active || s_active_image == NULL) {
        return;
    }
    if ((int32_t)(now_ticks - c->wake_tick) < 0) {
        return;
    }

    uint16_t code_len = read_u16(&s_active_image[4]);
    const uint8_t *code = &s_active_image[LUA_HEADER_SIZE];
    uint16_t pc = c->pc;
    uint8_t sp = c->sp;
    uint16_t slice_steps = 0U;

    while (pc < code_len && slice_steps < slice_budget && c->total_steps < LUA_MAX_STEPS) {
        slice_steps++;
        c->total_steps++;
        uint8_t op = code[pc++];

        switch (op) {
        case LUA_OP_HALT:
            c->active = false;
            c->ret_val = 0;
            return;

        case LUA_OP_RET:
            c->active = false;
            c->ret_val = (sp > 0U) ? c->stack[--sp] : 0;
            return;

        case LUA_OP_PUSH_I8:
            if (pc < code_len && sp < LUA_STACK_SIZE) {
                c->stack[sp++] = (int32_t)(int8_t)code[pc++];
            }
            break;

        case LUA_OP_PUSH_I16:
            if ((pc + 1U) < code_len && sp < LUA_STACK_SIZE) {
                int16_t v = (int16_t)read_u16(&code[pc]);
                pc = (uint16_t)(pc + 2U);
                c->stack[sp++] = (int32_t)v;
            }
            break;

        case LUA_OP_GET_LOCAL:
            if (pc < code_len && sp < LUA_STACK_SIZE) {
                uint8_t idx = (uint8_t)(code[pc++] & (LUA_NUM_LOCALS - 1U));
                c->stack[sp++] = c->locals[idx];
            }
            break;

        case LUA_OP_SET_LOCAL:
            if (pc < code_len && sp > 0U) {
                uint8_t idx = (uint8_t)(code[pc++] & (LUA_NUM_LOCALS - 1U));
                c->locals[idx] = c->stack[--sp];
            }
            break;

        case LUA_OP_GET_GLOBAL:
            if (pc < code_len && sp < LUA_STACK_SIZE) {
                uint8_t idx = (uint8_t)(code[pc++] & (LUA_NUM_GLOBALS - 1U));
                c->stack[sp++] = (int32_t)s_globals[idx];
            }
            break;

        case LUA_OP_SET_GLOBAL:
            if (pc < code_len && sp > 0U) {
                uint8_t idx = (uint8_t)(code[pc++] & (LUA_NUM_GLOBALS - 1U));
                s_globals[idx] = (int16_t)c->stack[--sp];
            }
            break;

        case LUA_OP_GET_MEM:
            if (sp > 0U) {
                uint8_t idx = (uint8_t)(c->stack[sp - 1U] & (LUA_MEM_SIZE - 1U));
                c->stack[sp - 1U] = (int32_t)s_mem[idx];
            }
            break;

        case LUA_OP_SET_MEM:
            if (sp >= 2U) {
                int32_t val = c->stack[--sp];
                uint8_t idx = (uint8_t)(c->stack[--sp] & (LUA_MEM_SIZE - 1U));
                s_mem[idx] = (int16_t)val;
            }
            break;

        case LUA_OP_POP:
            if (sp > 0U) {
                sp--;
            }
            break;

        case LUA_OP_NEG:
            if (sp > 0U) {
                c->stack[sp - 1U] = -c->stack[sp - 1U];
            }
            break;

        case LUA_OP_NOT:
            if (sp > 0U) {
                c->stack[sp - 1U] = (c->stack[sp - 1U] == 0) ? 1 : 0;
            }
            break;

        case LUA_OP_ADD:
        case LUA_OP_SUB:
        case LUA_OP_MUL:
        case LUA_OP_DIV:
        case LUA_OP_MOD:
        case LUA_OP_BAND:
        case LUA_OP_BOR:
        case LUA_OP_BXOR:
        case LUA_OP_SHL:
        case LUA_OP_SHR:
        case LUA_OP_EQ:
        case LUA_OP_NE:
        case LUA_OP_LT:
        case LUA_OP_LE:
        case LUA_OP_GT:
        case LUA_OP_GE:
            if (sp >= 2U) {
                int32_t b = c->stack[--sp];
                int32_t a = c->stack[sp - 1U];
                int32_t r = 0;
                switch (op) {
                case LUA_OP_ADD:
                    r = a + b;
                    break;
                case LUA_OP_SUB:
                    r = a - b;
                    break;
                case LUA_OP_MUL:
                    r = a * b;
                    break;
                case LUA_OP_DIV:
                    r = (b != 0) ? (a / b) : 0;
                    break;
                case LUA_OP_MOD:
                    if (b != 0) {
                        r = a % b;
                        if (r < 0) {
                            r += (b > 0) ? b : -b;
                        }
                    }
                    break;
                case LUA_OP_BAND:
                    r = a & b;
                    break;
                case LUA_OP_BOR:
                    r = a | b;
                    break;
                case LUA_OP_BXOR:
                    r = a ^ b;
                    break;
                case LUA_OP_SHL:
                    r = a << (b & 15);
                    break;
                case LUA_OP_SHR:
                    r = a >> (b & 15);
                    break;
                case LUA_OP_EQ:
                    r = (a == b) ? 1 : 0;
                    break;
                case LUA_OP_NE:
                    r = (a != b) ? 1 : 0;
                    break;
                case LUA_OP_LT:
                    r = (a < b) ? 1 : 0;
                    break;
                case LUA_OP_LE:
                    r = (a <= b) ? 1 : 0;
                    break;
                case LUA_OP_GT:
                    r = (a > b) ? 1 : 0;
                    break;
                case LUA_OP_GE:
                    r = (a >= b) ? 1 : 0;
                    break;
                default:
                    break;
                }
                c->stack[sp - 1U] = r;
            }
            break;

        case LUA_OP_JMP:
            if ((pc + 1U) < code_len) {
                pc = read_u16(&code[pc]);
            }
            break;

        case LUA_OP_JZ:
            if ((pc + 1U) < code_len && sp > 0U) {
                uint16_t target = read_u16(&code[pc]);
                pc = (uint16_t)(pc + 2U);
                if (c->stack[--sp] == 0) {
                    pc = target;
                }
            }
            break;

        case LUA_OP_JNZ:
            if ((pc + 1U) < code_len && sp > 0U) {
                uint16_t target = read_u16(&code[pc]);
                pc = (uint16_t)(pc + 2U);
                if (c->stack[--sp] != 0) {
                    pc = target;
                }
            }
            break;

        case LUA_OP_CALL_BUILTIN:
            if ((pc + 1U) < code_len) {
                uint8_t fn_id = code[pc++];
                uint8_t argc = code[pc++];
                int32_t args[4] = {0, 0, 0, 0};
                for (int8_t i = (int8_t)argc - 1; i >= 0; i--) {
                    int32_t v = (sp > 0U) ? c->stack[--sp] : 0;
                    if (i < 4) {
                        args[i] = v;
                    }
                }
                bool yielded = false;
                int32_t res = call_builtin(c, fn_id, argc, args, now_ticks, &yielded);
                if (sp < LUA_STACK_SIZE) {
                    c->stack[sp++] = res;
                }
                if (yielded) {
                    c->pc = pc;
                    c->sp = sp;
                    return;
                }
            }
            break;

        default:
            c->active = false;
            return;
        }
    }

    if (pc >= code_len || c->total_steps >= LUA_MAX_STEPS) {
        c->active = false;
    } else {
        c->pc = pc;
        c->sp = sp;
    }
}

static int32_t run_sync_entry(uint8_t header_entry_off, int32_t a0, int32_t a1, int32_t a2,
                              uint32_t now_ticks) {
    lua_coro_t sync_coro;
    if (!init_coro(&sync_coro, header_entry_off, a0, a1, a2, now_ticks)) {
        return 0;
    }
    step_coro(&sync_coro, 2048U, now_ticks);
    return sync_coro.ret_val;
}

static void reset_vm_state(uint32_t now_ticks) {
    s_tick_coro.active = false;
    flush_all_gates();
    for (uint8_t i = 0; i < LUA_NUM_GLOBALS; i++) {
        s_globals[i] = 0;
    }
    for (uint16_t i = 0; i < LUA_MEM_SIZE; i++) {
        s_mem[i] = 0;
    }
    faders_get_values(s_fader_vals);
    if (s_fader_vals[7] == 0U) {
        s_fader_vals[7] = 64U; // Default 2 echo repeats until Fader 8 moves
    }
    s_tick_interval = 110U;
    s_step_counter = 0;
    s_last_tick = now_ticks;
    run_sync_entry(6U, 0, 0, 0, now_ticks);
}

static void send_lua_ack(uint8_t status) {
    uint8_t ack[8] = {
        MIDI_STATUS_SYSEX_START, 0x7DU, 'L', 'U', 'A', LUA_SYSEX_CMD_ACK, status & 0x7FU,
        MIDI_STATUS_SYSEX_END,
    };
    if (usb_get_profile() == USB_PROFILE_MONOME_FTDI) {
        usb_serial_write(ack, sizeof(ack));
    } else {
        usb_midi_send_sysex(ack, sizeof(ack));
    }
}

void lua_vm_init(uint32_t now_ticks) {
    s_enabled = false;
    for (uint8_t i = 0U; i < LUA_MAX_GATES; i++) {
        s_gates[i].active = false;
    }
    if (is_valid_lua_image(_lua_flash_page, LUA_MAX_IMAGE_LEN)) {
        s_active_image = _lua_flash_page;
    } else {
        s_active_image = k_rom_script;
    }
    reset_vm_state(now_ticks);
}

bool lua_vm_is_enabled(void) {
    return s_enabled;
}

void lua_vm_set_enabled(bool enabled, uint32_t now_ticks) {
    if (s_enabled == enabled) {
        return;
    }
    s_enabled = enabled;
    s_tick_coro.active = false;
    flush_all_gates();
    clear_overlay_leds(now_ticks);
    if (s_enabled) {
        reset_vm_state(now_ticks);
    }
    refresh_mode_status_leds();
}

void lua_vm_toggle(uint32_t now_ticks) {
    lua_vm_set_enabled(!s_enabled, now_ticks);
}

void lua_vm_task(uint32_t now_ticks) {
    if (!s_enabled) {
        return;
    }

    // 1. Drain expired async note/key gates
    service_expired_gates(now_ticks);

    // 2. Start a new on_tick coroutine frame when the tempo interval elapses
    if (!s_tick_coro.active && (uint32_t)(now_ticks - s_last_tick) >= s_tick_interval) {
        s_last_tick = now_ticks;
        init_coro(&s_tick_coro, 8U, s_step_counter++, 0, 0, now_ticks);
    }

    // 3. Execute at most LUA_SLICE_STEPS (128 ops, ~70 us) per main-loop turn
    if (s_tick_coro.active) {
        step_coro(&s_tick_coro, LUA_SLICE_STEPS, now_ticks);
    }
}

bool lua_vm_on_button(apc_btn_group_t group, uint8_t index, bool pressed, uint32_t now_ticks) {
    if (!s_enabled || group != APC_BTN_GROUP_GRID || index >= 64U) {
        return false;
    }
    int32_t x = (int32_t)(index & 7U);
    int32_t y = (int32_t)(7U - (index >> 3U));
    int32_t z = pressed ? 1 : 0;
    int32_t consumed = run_sync_entry(10U, x, y, z, now_ticks);
    return (consumed != 0);
}

void lua_vm_on_fader(uint8_t fader_idx, uint8_t value, uint32_t now_ticks) {
    if (fader_idx >= APC_FADER_COUNT) {
        return;
    }
    s_fader_vals[fader_idx] = value;
    if (s_enabled) {
        run_sync_entry(12U, (int32_t)fader_idx, (int32_t)value, 0, now_ticks);
    }
}

void lua_vm_handle_sysex(const uint8_t *msg, uint16_t len) {
    if (len < 7U) {
        return;
    }
    uint32_t now = board_ticks();
    uint8_t cmd = msg[5];

    switch (cmd) {
    case LUA_SYSEX_CMD_BEGIN:
        if (len >= 9U) {
            uint16_t total = (uint16_t)((uint16_t)(msg[6] & 0x7FU) |
                                        ((uint16_t)(msg[7] & 0x7FU) << 7U));
            if (total <= LUA_MAX_IMAGE_LEN) {
                s_upload_expected = total;
                s_upload_received = 0U;
                for (uint16_t i = 0; i < LUA_MAX_IMAGE_LEN; i++) {
                    s_ram_image[i] = 0U;
                }
                send_lua_ack(0x01U);
            } else {
                s_upload_expected = 0U;
                send_lua_ack(0x00U);
            }
        }
        break;

    case LUA_SYSEX_CMD_CHUNK:
        if (len >= 9U && s_upload_expected > 0U) {
            uint16_t off = (uint16_t)((uint16_t)(msg[6] & 0x7FU) |
                                      ((uint16_t)(msg[7] & 0x7FU) << 7U));
            uint16_t nibbles = (uint16_t)(len - 9U);
            uint16_t nbytes = (uint16_t)(nibbles >> 1U);
            for (uint16_t i = 0; i < nbytes; i++) {
                if ((uint16_t)(off + i) < s_upload_expected) {
                    uint8_t hi = (uint8_t)(msg[8U + (i << 1U)] & 0x0FU);
                    uint8_t lo = (uint8_t)(msg[9U + (i << 1U)] & 0x0FU);
                    s_ram_image[off + i] = (uint8_t)((hi << 4U) | lo);
                    s_upload_received++;
                }
            }
        }
        break;

    case LUA_SYSEX_CMD_COMMIT:
        if (len >= 9U && s_upload_expected >= LUA_HEADER_SIZE) {
            uint8_t expected_xor =
                (uint8_t)(((msg[6] & 0x0FU) << 4U) | (msg[7] & 0x0FU));
            uint8_t actual_xor = 0U;
            for (uint16_t i = 0; i < s_upload_expected; i++) {
                actual_xor ^= s_ram_image[i];
            }
            if (actual_xor == expected_xor &&
                is_valid_lua_image(s_ram_image, s_upload_expected)) {
                s_active_image = s_ram_image;
                s_enabled = true;
                clear_overlay_leds(now);
                reset_vm_state(now);
                refresh_mode_status_leds();
                send_lua_ack(0x01U);
            } else {
                send_lua_ack(0x00U);
            }
        }
        break;

    case LUA_SYSEX_CMD_SAVE:
        if (is_valid_lua_image(s_active_image, LUA_MAX_IMAGE_LEN)) {
            uint16_t total = (uint16_t)(LUA_HEADER_SIZE + read_u16(&s_active_image[4]));
            bool ok = bootloader_write_flash_page((uint32_t)(uintptr_t)_lua_flash_page,
                                                  s_active_image, total);
            if (ok) {
                s_active_image = _lua_flash_page;
            }
            send_lua_ack(ok ? 0x01U : 0x00U);
        }
        break;

    case LUA_SYSEX_CMD_RESET:
        bootloader_write_flash_page((uint32_t)(uintptr_t)_lua_flash_page, NULL, 0U);
        s_active_image = k_rom_script;
        clear_overlay_leds(now);
        reset_vm_state(now);
        refresh_mode_status_leds();
        send_lua_ack(0x01U);
        break;

    case LUA_SYSEX_CMD_ENABLE:
        if (len >= 8U) {
            lua_vm_set_enabled(msg[6] != 0U, now);
            send_lua_ack(0x01U);
        }
        break;

    default:
        break;
    }
}
