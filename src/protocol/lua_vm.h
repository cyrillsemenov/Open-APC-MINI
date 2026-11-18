#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "buttons.h"

/*---------------------------------------------------------------------------
 * "LUA1" Bytecode Header (16 bytes)
 *   [0..3]   Magic: 'L', 'U', 'A', '1' (0x4C, 0x55, 0x41, 0x31)
 *   [4..5]   uint16_t code_len     (bytes following this 16-byte header)
 *   [6..7]   uint16_t entry_init   (byte offset or 0xFFFF)
 *   [8..9]   uint16_t entry_tick   (byte offset or 0xFFFF)
 *   [10..11] uint16_t entry_grid   (byte offset or 0xFFFF)
 *   [12..13] uint16_t entry_fader  (byte offset or 0xFFFF)
 *   [14..15] uint16_t reserved     (0x0000)
 *---------------------------------------------------------------------------*/
#define LUA_HEADER_SIZE   16U
#define LUA_MAX_IMAGE_LEN 768U
#define LUA_ENTRY_NONE    0xFFFFU

/*---------------------------------------------------------------------------
 * Stack Bytecode Opcodes
 *---------------------------------------------------------------------------*/
#define LUA_OP_HALT         0x00U
#define LUA_OP_RET          0x01U
#define LUA_OP_PUSH_I8      0x02U
#define LUA_OP_PUSH_I16     0x03U
#define LUA_OP_GET_LOCAL    0x04U
#define LUA_OP_SET_LOCAL    0x05U
#define LUA_OP_GET_GLOBAL   0x06U
#define LUA_OP_SET_GLOBAL   0x07U
#define LUA_OP_GET_MEM      0x08U
#define LUA_OP_SET_MEM      0x09U
#define LUA_OP_POP          0x0AU

#define LUA_OP_ADD          0x10U
#define LUA_OP_SUB          0x11U
#define LUA_OP_MUL          0x12U
#define LUA_OP_DIV          0x13U
#define LUA_OP_MOD          0x14U
#define LUA_OP_NEG          0x15U
#define LUA_OP_NOT          0x16U
#define LUA_OP_BAND         0x17U
#define LUA_OP_BOR          0x18U
#define LUA_OP_BXOR         0x19U
#define LUA_OP_SHL          0x1AU
#define LUA_OP_SHR          0x1BU

#define LUA_OP_EQ           0x20U
#define LUA_OP_NE           0x21U
#define LUA_OP_LT           0x22U
#define LUA_OP_LE           0x23U
#define LUA_OP_GT           0x24U
#define LUA_OP_GE           0x25U

#define LUA_OP_JMP          0x30U
#define LUA_OP_JZ           0x31U
#define LUA_OP_JNZ          0x32U

#define LUA_OP_CALL_BUILTIN 0x40U

/*---------------------------------------------------------------------------
 * Built-in Intrinsics
 *---------------------------------------------------------------------------*/
#define LUA_FN_LED          0x00U // led(x, y, val)
#define LUA_FN_LED_ALL      0x01U // led_all(val)
#define LUA_FN_GRID_KEY     0x02U // grid_key(x, y, z) / trigger_key(x, y, z)
#define LUA_FN_TILT         0x03U // tilt(sensor, x, y, z)
#define LUA_FN_NOTE_ON      0x04U // note_on(ch, note, vel)
#define LUA_FN_NOTE_OFF     0x05U // note_off(ch, note)
#define LUA_FN_CC           0x06U // cc(ch, num, val)
#define LUA_FN_FADER        0x07U // fader(idx) -> 0..127
#define LUA_FN_SHIFT        0x08U // shift() -> 0|1
#define LUA_FN_SET_TEMPO    0x09U // set_tempo(ms)
#define LUA_FN_RAND         0x0AU // rand(max) -> 0..max-1
#define LUA_FN_ABS          0x0BU // abs(x)
#define LUA_FN_MIN          0x0CU // min(a, b)
#define LUA_FN_MAX          0x0DU // max(a, b)
#define LUA_FN_HOST_LED     0x0EU // host_led(x, y) -> 0..16
#define LUA_FN_LIFE_STEP    0x0FU // life_step() -> alive_count (computes mem[0..63] -> mem[64..127])
#define LUA_FN_YIELD        0x10U // yield() -> suspends current coroutine slice
#define LUA_FN_SLEEP_MS     0x11U // sleep_ms(ms) -> suspends coroutine for ms milliseconds
#define LUA_FN_KEY_PULSE    0x12U // key_pulse(x, y, ms) -> fires key-down + async key-up after ms

/*---------------------------------------------------------------------------
 * SysEx Upload Subcommands: F0 7D 'L' 'U' 'A' <cmd> ... F7
 *---------------------------------------------------------------------------*/
#define LUA_SYSEX_CMD_BEGIN  0x01U
#define LUA_SYSEX_CMD_CHUNK  0x02U
#define LUA_SYSEX_CMD_COMMIT 0x03U
#define LUA_SYSEX_CMD_SAVE   0x04U
#define LUA_SYSEX_CMD_RESET  0x05U
#define LUA_SYSEX_CMD_ENABLE 0x06U
#define LUA_SYSEX_CMD_ACK    0x7FU

void lua_vm_init(uint32_t now_ticks);
bool lua_vm_is_enabled(void);
void lua_vm_set_enabled(bool enabled, uint32_t now_ticks);
void lua_vm_toggle(uint32_t now_ticks);
void lua_vm_task(uint32_t now_ticks);
bool lua_vm_on_button(apc_btn_group_t group, uint8_t index, bool pressed, uint32_t now_ticks);
void lua_vm_on_fader(uint8_t fader_idx, uint8_t value, uint32_t now_ticks);
void lua_vm_handle_sysex(const uint8_t *msg, uint16_t len);
