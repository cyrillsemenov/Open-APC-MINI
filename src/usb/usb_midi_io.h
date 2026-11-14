#pragma once

#include <stdint.h>

void usb_tx_q_init(void);
void usb_tx_q0_write(const uint8_t *src, uint8_t len);
uint8_t usb_tx_q0_read(uint8_t *dst, uint8_t len);
uint8_t usb_tx_q1_read(uint8_t *dst, uint8_t len);
uint32_t USB_SIL_Write_EP1(uint8_t *pBufferPointer, uint32_t wBufferSize);
void usb_midi_flush_tx(void);
void usb_rx_parse_packet(void);
void usb_midi_poll_rx(void);
void send_identity_reply(void);
void send_ableton_fader_dump(void);
void handle_updater_unlock_and_reset(void);
uint8_t flash_write_page_256(uint32_t page_addr);
