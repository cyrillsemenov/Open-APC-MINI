#pragma once

#include <stdbool.h>
#include <stdint.h>

void usb_midi_device_init(void);
void usb_midi_poll_isr(void);
bool usb_midi_is_configured(void);

void usb_tx_q_init(void);
void usb_tx_q0_write(const uint8_t *src, uint8_t len);
uint8_t usb_tx_q0_read(uint8_t *dst, uint8_t len);
uint8_t usb_tx_q1_read(uint8_t *dst, uint8_t len);
uint16_t usb_ep1_write(const uint8_t *buf, uint16_t len);
void usb_midi_flush_tx(void);
void usb_rx_parse_packet(void);
void usb_midi_poll_rx(void);
void send_identity_reply(void);
void send_ableton_fader_dump(void);
void handle_updater_unlock_and_reset(void);
uint8_t flash_write_page_256(uint32_t page_addr);
