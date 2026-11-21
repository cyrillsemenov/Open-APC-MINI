#pragma once

#include <stdbool.h>
#include <stdint.h>

// Provided by the linker script at 0x08007C00 (1 KB bootloader flag page).
extern uint8_t _boot_magic_page[];

// Provided by the linker script at 0x08007800 (1 KB persistent post-mortem crash log page).
extern uint8_t _crash_log_page[];

// Provided by the linker script at 0x08008000 (4 KB persistent Lua bytecode region).
extern uint8_t _lua_flash_page[];

// Provided by the linker script at 0x08009000 (1 KB persistent config page).
extern uint8_t _config_flash_page[];

// Dedicated 256-byte fault recovery stack in low SRAM (.bss) so resetting MSP
// in a fault handler never overwrites the pre-fault exception frame on the main stack.
extern uint32_t g_fault_stack[64];

#define APC_CRASH_MAGIC 0x43525348UL // "CRSH"

typedef struct {
    uint32_t magic;        // [0]  0x43525348 ("CRSH")
    uint32_t ipsr;         // [1]  Exception number (2=NMI, 3=HardFault, 4=MemManage, 5=BusFault, 6=UsageFault)
    uint32_t fault_msp;    // [2]  Pre-fault MSP
    uint32_t cfsr;         // [3]  SCB->CFSR (0xE000ED28)
    uint32_t hfsr;         // [4]  SCB->HFSR (0xE000ED2C)
    uint32_t far;          // [5]  SCB->BFAR or SCB->MMFAR
    uint32_t r0;           // [6]  Stacked R0
    uint32_t r1;           // [7]  Stacked R1
    uint32_t r2;           // [8]  Stacked R2
    uint32_t lr;           // [9]  Stacked LR (caller return address)
    uint32_t pc;           // [10] Stacked PC (faulting instruction address)
    uint32_t uptime_ticks; // [11] board_ticks() at fault
} apc_crash_log_t;

// Inspects a 64-byte EP1/EP2 OUT packet for the stock Akai DFU unlock sequence.
// On match, programs the bootloader magic page, sends ACK 0x15, detaches USB, and resets.
void bootloader_check_unlock_packet(const uint8_t pkt[64]);

// Programs the 0x08007C00 bootloader magic page, detaches PA8 USB pull-up, and resets into DFU.
void bootloader_enter_dfu(void);

// Captures a post-mortem crash dump into 0x08007800 and reboots into the DFU bootloader.
void bootloader_fault_dfu(uint32_t fault_msp, uint32_t ipsr);

// Erases the persistent post-mortem crash dump page at 0x08007800.
bool bootloader_clear_crash_log(void);

// Erases and writes up to 1024 bytes to a non-app 1 KB Flash page (>= 0x08007800).
bool bootloader_write_flash_page(uint32_t page_addr, const uint8_t *data, uint16_t len);
