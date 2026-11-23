#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "usb_desc.h"

typedef void (*usb_midi_channel_cb_t)(uint8_t status, uint8_t data1, uint8_t data2);
typedef void (*usb_midi_sysex_cb_t)(const uint8_t *msg, uint16_t len);

// Returns the currently active USB profile.
usb_profile_t usb_get_profile(void);

// Initializes the active USB Full-Speed device profile, enables USB IRQ, and attaches PA8 pull-up.
void usb_midi_init(void);

// Services the libopencm3 USB device state machine (called from usb_lp_can_rx0_isr).
void usb_midi_poll_isr(void);

// Returns true once the USB host completes SET_CONFIGURATION.
bool usb_midi_is_configured(void);

// Raw 64-byte packet write to EP1 IN (used by usb_midi_flush_tx and DFU unlock ACK).
uint16_t usb_midi_ep1_write(const uint8_t *buf, uint16_t len);

// Enqueues a single 4-byte USB-MIDI 1.0 Event Packet for bulk IN transmission.
bool usb_midi_send_event(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3);

// Packetizes a complete SysEx message (F0 ... F7) into CIN 0x04..0x07 Event Packets.
bool usb_midi_send_sysex(const uint8_t *data, uint16_t len);

// Enqueues raw serial bytes for CDC-ACM bulk IN transmission on EP2 IN.
bool usb_serial_write(const uint8_t *data, uint16_t len);

// Pops one received byte from the CDC-ACM bulk OUT ring buffer.
bool usb_serial_read_byte(uint8_t *out_byte);

// Packs pending TX data (USB-MIDI 4B packets on EP1 IN, CDC-ACM stream on EP2 IN) when ready.
void usb_midi_flush_tx(void);

// Drains queued RX Event Packets in main-loop context and invokes channel/SysEx callbacks.
void usb_midi_poll_rx(usb_midi_channel_cb_t on_channel_msg, usb_midi_sysex_cb_t on_sysex_msg);
