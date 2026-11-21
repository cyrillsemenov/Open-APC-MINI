#include <stdbool.h>
#include <string.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/st_usbfs.h>
#include "apc_protocol.h"
#include "board.h"
#include "bootloader.h"
#include "leds.h"
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

// Raw 9-byte DFU unlock sequence (used over serial/FTDI): F0 5A 57 6E 28 3C 4E 3E F7
static const uint8_t k_raw_unlock_req[9] = {
    MIDI_STATUS_SYSEX_START, 0x5AU, 0x57U, 0x6EU, 0x28U, 0x3CU, 0x4EU, 0x3EU, MIDI_STATUS_SYSEX_END,
};

// 8-byte bootloader flag expected at 0x08007C00: 55 5A A5 AA 59 95 3A A3 (little-endian words)
#define BOOT_MAGIC_WORD0 0xAAA55A55UL
#define BOOT_MAGIC_WORD1 0xA33A9559UL

static const uint32_t k_boot_magic[2] = {BOOT_MAGIC_WORD0, BOOT_MAGIC_WORD1};

uint32_t g_fault_stack[64];

static bool flash_write_raw(uint32_t page_addr, const uint8_t *data, uint16_t len, uint32_t pad_word,
                            uint16_t total_words) {
    flash_unlock();
    flash_clear_status_flags();
    uint16_t erase_len = (len > 1024U) ? len : 1024U;
    for (uint16_t off = 0U; off < erase_len; off = (uint16_t)(off + 1024U)) {
        flash_erase_page(page_addr + (uint32_t)off);
        if ((FLASH_SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) != 0) {
            flash_lock();
            return false;
        }
    }

    for (uint16_t i = 0; i < total_words; i++) {
        uint16_t base = (uint16_t)(i << 2U);
        uint32_t word = pad_word;
        uint8_t *wb = (uint8_t *)&word;
        for (uint8_t b = 0; b < 4U; b++) {
            if (data != 0 && (uint16_t)(base + b) < len) {
                wb[b] = data[base + b];
            }
        }
        uint32_t addr = page_addr + (uint32_t)base;
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

static bool write_bootloader_magic_page(uint32_t page_addr) {
    if ((page_addr & 0x3FFUL) != 0) {
        return false;
    }

    const volatile uint32_t *flash_words = (const volatile uint32_t *)page_addr;
    if (flash_words[0] == BOOT_MAGIC_WORD0 && flash_words[1] == BOOT_MAGIC_WORD1) {
        return true;
    }

    return flash_write_raw(page_addr, (const uint8_t *)k_boot_magic, 8U, 0x00000000UL, 64U);
}

bool bootloader_write_flash_page(uint32_t page_addr, const uint8_t *data, uint16_t len) {
    uint16_t max_len = (page_addr >= 0x08008000UL) ? 4096U : 1024U;
    if (page_addr < 0x08007800UL || page_addr == (uint32_t)(uintptr_t)_boot_magic_page ||
        (page_addr & 0x3FFUL) != 0 || len > max_len) {
        return false;
    }

    uint16_t raw_len = (data != 0) ? len : 0U;
    uint16_t words = (raw_len > 0U) ? (uint16_t)((raw_len + 3U) >> 2U) : 0U;
    return flash_write_raw(page_addr, data, raw_len, 0xFFFFFFFFUL, words);
}

bool bootloader_clear_crash_log(void) {
    return bootloader_write_flash_page((uint32_t)(uintptr_t)_crash_log_page, 0, 0U);
}

void bootloader_enter_dfu(void) {
    board_set_post_dfu_monome(true);

    // Blank all 8x8 TM1629 pad LEDs and 16 round LEDs before rebooting into DFU
    leds_init();

    write_bootloader_magic_page((uint32_t)(uintptr_t)_boot_magic_page);

    // Pull PA8 LOW so the host OS detects a clean USB disconnect before DFU re-enumeration
    board_usb_dp_pullup(false);
    for (volatile uint32_t d = 0; d < 400000; d++) {}

    __asm__ volatile("dsb 0xF" ::: "memory");
    scb_reset_system();
    while (1) {}
}

void bootloader_fault_dfu(uint32_t fault_msp, uint32_t ipsr) {
    apc_crash_log_t log;
    log.magic = APC_CRASH_MAGIC;
    log.ipsr = ipsr;
    log.fault_msp = fault_msp;
    log.cfsr = SCB_CFSR;
    log.hfsr = SCB_HFSR;
    // BFARVALID is bit 15 of SCB_CFSR (bit 7 of BFSR)
    log.far = ((log.cfsr & 0x00008000UL) != 0UL) ? SCB_BFAR : SCB_MMFAR;
    log.r0 = 0UL;
    log.r1 = 0UL;
    log.r2 = 0UL;
    log.lr = 0UL;
    log.pc = 0UL;
    log.uptime_ticks = board_ticks();

    // Extract hardware-pushed exception frame if pre-fault MSP points to valid 6..16 KB SRAM
    if (fault_msp >= 0x20000000UL && fault_msp <= 0x20003FE0UL && (fault_msp & 3UL) == 0UL) {
        const volatile uint32_t *frame = (const volatile uint32_t *)fault_msp;
        log.r0 = frame[0];
        log.r1 = frame[1];
        log.r2 = frame[2];
        log.lr = frame[5];
        log.pc = frame[6];
    }

    bootloader_write_flash_page((uint32_t)(uintptr_t)_crash_log_page, (const uint8_t *)&log,
                                (uint16_t)sizeof(apc_crash_log_t));
    bootloader_enter_dfu();
}

void bootloader_check_unlock_packet(const uint8_t pkt[64]) {
    if (memcmp(pkt, k_updater_unlock_req, sizeof(k_updater_unlock_req)) != 0 &&
        memcmp(pkt, k_raw_unlock_req, sizeof(k_raw_unlock_req)) != 0) {
        return;
    }

    write_bootloader_magic_page((uint32_t)(uintptr_t)_boot_magic_page);

    uint8_t ack[64] = {0};
    ack[0] = USB_MIDI_CIN_SYSEX_END_3B;
    ack[1] = MIDI_STATUS_SYSEX_START;
    ack[2] = AKAI_DFU_UNLOCK_ACK_CMD;
    ack[3] = MIDI_STATUS_SYSEX_END;
    usb_midi_ep1_write(ack, sizeof(ack));

    // Wait for USB host to read EP1 IN (or EP3 IN in composite Monome mode) ACK
    for (volatile uint32_t d = 0; d < 400000; d++) {
        if ((*USB_EP_REG(USB_EP1) & USB_EP_TX_STAT) != USB_EP_TX_STAT_VALID) {
            break;
        }
        if ((*USB_EP_REG(USB_EP3) & USB_EP_TX_STAT) != USB_EP_TX_STAT_VALID) {
            break;
        }
    }

    // Settle ~2 ms at 48 MHz so the USB ACK completes on the wire
    for (volatile uint32_t d = 0; d < 20000; d++) {}

    bootloader_enter_dfu();
}
