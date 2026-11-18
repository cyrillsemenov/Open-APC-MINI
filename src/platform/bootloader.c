#include <stdbool.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/st_usbfs.h>
#include "apc_protocol.h"
#include "board.h"
#include "bootloader.h"
#include "usb_midi.h"

// USB-MIDI packetized DFU unlock request: F0 5A 57 6E 28 3C 4E 3E F7
static const uint8_t k_updater_unlock_req[12] = {
    USB_MIDI_CIN_SYSEX_START,
    MIDI_STATUS_SYSEX_START,
    0x5AU,
    0x57U,
    USB_MIDI_CIN_SYSEX_START,
    0x6EU,
    0x28U,
    0x3CU,
    USB_MIDI_CIN_SYSEX_END_3B,
    0x4EU,
    0x3EU,
    MIDI_STATUS_SYSEX_END,
};

// 8-byte bootloader flag expected at 0x08007C00: 55 5A A5 AA 59 95 3A A3 (little-endian words)
#define BOOT_MAGIC_WORD0 0xAAA55A55UL
#define BOOT_MAGIC_WORD1 0xA33A9559UL

static bool write_bootloader_magic_page(uint32_t page_addr) {
    if ((page_addr & 0x3FFUL) != 0) {
        return false;
    }

    const volatile uint32_t *flash_words = (const volatile uint32_t *)page_addr;
    if (flash_words[0] == BOOT_MAGIC_WORD0 && flash_words[1] == BOOT_MAGIC_WORD1) {
        return true;
    }

    flash_unlock();
    flash_clear_status_flags();
    flash_erase_page(page_addr);

    if ((FLASH_SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) != 0) {
        flash_lock();
        return false;
    }

    for (uint32_t i = 0; i < 64; i++) {
        uint32_t word = 0x00000000UL;
        if (i == 0) {
            word = BOOT_MAGIC_WORD0;
        } else if (i == 1) {
            word = BOOT_MAGIC_WORD1;
        }

        uint32_t addr = page_addr + (i * 4UL);
        flash_program_word(addr, word);
        for (volatile uint32_t d = 0; d < 20; d++) {}
        if (*(const volatile uint32_t *)addr != word) {
            flash_lock();
            return false;
        }
    }

    flash_lock();
    return true;
}

void bootloader_enter_dfu(void) {
    write_bootloader_magic_page((uint32_t)(uintptr_t)_boot_magic_page);

    // Pull PA8 LOW so the host OS detects a clean USB disconnect before DFU re-enumeration
    board_usb_dp_pullup(false);
    for (volatile uint32_t d = 0; d < 400000; d++) {}

    __asm__ volatile("dsb 0xF" ::: "memory");
    scb_reset_system();
    while (1) {}
}

void bootloader_check_unlock_packet(const uint8_t pkt[64]) {
    if (pkt[1] != MIDI_STATUS_SYSEX_START) {
        return;
    }

    for (uint8_t i = 0; i < sizeof(k_updater_unlock_req); i++) {
        if (pkt[i] != k_updater_unlock_req[i]) {
            return;
        }
    }

    write_bootloader_magic_page((uint32_t)(uintptr_t)_boot_magic_page);

    uint8_t ack[64] = {0};
    ack[0] = USB_MIDI_CIN_SYSEX_END_3B;
    ack[1] = MIDI_STATUS_SYSEX_START;
    ack[2] = AKAI_DFU_UNLOCK_ACK_CMD;
    ack[3] = MIDI_STATUS_SYSEX_END;
    usb_midi_ep1_write(ack, sizeof(ack));

    // Wait for USB host to read EP1 IN ACK (STAT_TX leaves USB_EP_TX_STAT_VALID)
    for (volatile uint32_t d = 0; d < 400000; d++) {
        if ((*USB_EP_REG(USB_EP1) & USB_EP_TX_STAT) != USB_EP_TX_STAT_VALID) {
            break;
        }
    }

    // Settle ~2 ms at 48 MHz so the USB ACK completes on the wire
    for (volatile uint32_t d = 0; d < 20000; d++) {}

    bootloader_enter_dfu();
}
