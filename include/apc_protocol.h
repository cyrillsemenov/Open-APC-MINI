#pragma once

#include <stdint.h>

/*---------------------------------------------------------------------------
 * USB-MIDI 1.0 Code Index Numbers (CIN, Cable 0)
 *---------------------------------------------------------------------------*/
typedef enum {
    USB_MIDI_CIN_MISC = 0x00,
    USB_MIDI_CIN_CABLE_EVENT = 0x01,
    USB_MIDI_CIN_SYSCOMMON_2B = 0x02,
    USB_MIDI_CIN_SYSCOMMON_3B = 0x03,
    USB_MIDI_CIN_SYSEX_START = 0x04,  // SysEx starts or continues (3 bytes)
    USB_MIDI_CIN_SYSEX_END_1B = 0x05, // SysEx ends with 1 byte (or 1B System Common)
    USB_MIDI_CIN_SYSEX_END_2B = 0x06, // SysEx ends with 2 bytes
    USB_MIDI_CIN_SYSEX_END_3B = 0x07, // SysEx ends with 3 bytes
    USB_MIDI_CIN_NOTE_OFF = 0x08,
    USB_MIDI_CIN_NOTE_ON = 0x09,
    USB_MIDI_CIN_POLY_KEYPRESS = 0x0A,
    USB_MIDI_CIN_CONTROL_CHANGE = 0x0B,
    USB_MIDI_CIN_PROGRAM_CHANGE = 0x0C,
    USB_MIDI_CIN_CHAN_PRESSURE = 0x0D,
    USB_MIDI_CIN_PITCH_BEND = 0x0E,
    USB_MIDI_CIN_SINGLE_BYTE = 0x0F,
} usb_midi_cin_t;

/*---------------------------------------------------------------------------
 * MIDI 1.0 Status Bytes
 *---------------------------------------------------------------------------*/
#define MIDI_STATUS_NOTE_OFF       0x80U
#define MIDI_STATUS_NOTE_ON        0x90U
#define MIDI_STATUS_POLY_AFTER     0xA0U
#define MIDI_STATUS_CONTROL_CHANGE 0xB0U
#define MIDI_STATUS_PROGRAM_CHANGE 0xC0U
#define MIDI_STATUS_CHAN_AFTER     0xD0U
#define MIDI_STATUS_PITCH_BEND     0xE0U
#define MIDI_STATUS_SYSEX_START    0xF0U
#define MIDI_STATUS_SYSEX_END      0xF7U
#define MIDI_STATUS_REALTIME_MIN   0xF8U

/*---------------------------------------------------------------------------
 * Akai APC Mini Note & CC Map
 *---------------------------------------------------------------------------*/
#define APC_NOTE_GRID_BASE   0x00U // 8x8 clip grid (bottom-left = 0x00, top-right = 0x3F)
#define APC_NOTE_GRID_END    0x3FU
#define APC_NOTE_TRACK_BASE  0x40U // Horizontal track buttons 1..8
#define APC_NOTE_TRACK_END   0x47U
#define APC_NOTE_SCENE_BASE  0x52U // Vertical scene launch buttons 1..8
#define APC_NOTE_SCENE_END   0x59U
#define APC_NOTE_SHIFT       0x62U // Shift button
#define APC_DEFAULT_VELOCITY 0x7FU

#define APC_CC_FADER_BASE   0x30U // Faders 1..8 = 0x30..0x37
#define APC_CC_FADER_MASTER 0x38U // Master fader 9 = 0x38

/*---------------------------------------------------------------------------
 * APC Mini LED Velocity Modes
 *---------------------------------------------------------------------------*/
typedef enum {
    APC_PAD_LED_OFF = 0,
    APC_PAD_LED_GREEN = 1,
    APC_PAD_LED_GREEN_BLINK = 2,
    APC_PAD_LED_RED = 3,
    APC_PAD_LED_RED_BLINK = 4,
    APC_PAD_LED_YELLOW = 5,
    APC_PAD_LED_YELLOW_BLINK = 6,
    APC_PAD_LED_GREEN_SOLID = 7, // Velocities 7..127 light solid green
} apc_pad_led_mode_t;

typedef enum {
    APC_ROUND_LED_OFF = 0,
    APC_ROUND_LED_ON = 1,
    APC_ROUND_LED_BLINK = 2,
} apc_round_led_mode_t;

/*---------------------------------------------------------------------------
 * SysEx Identifiers & Commands
 *---------------------------------------------------------------------------*/
#define SYSEX_UNIVERSAL_NON_REALTIME 0x7EU
#define SYSEX_DEVICE_BROADCAST       0x7FU
#define SYSEX_SUB_GENERAL_INFO       0x06U
#define SYSEX_SUB_IDENTITY_REQUEST   0x01U
#define SYSEX_SUB_IDENTITY_REPLY     0x02U

#define AKAI_MANUFACTURER_ID    0x47U
#define AKAI_APC_MINI_MODEL_ID  0x28U
#define AKAI_CMD_ABLETON_INTRO  0x60U
#define AKAI_CMD_FADER_DUMP     0x61U
#define AKAI_CMD_SERIAL_WRITE   0x79U
#define AKAI_DFU_UNLOCK_ACK_CMD 0x15U
