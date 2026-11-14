#pragma once

#include <stdint.h>

// Provided by the linker script at 0x08007C00 (1 KB bootloader flag page).
extern uint8_t _boot_magic_page[];

// Inspects a 64-byte EP1 OUT packet for the stock Akai DFU unlock sequence.
// On match, programs the bootloader magic page, sends ACK 0x15, detaches USB, and resets.
void bootloader_check_unlock_packet(const uint8_t pkt[64]);
