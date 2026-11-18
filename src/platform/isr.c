#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/vector.h>
#include "board.h"
#include "bootloader.h"
#include "usb_midi.h"

// Top of the base 6 KB SRAM region (0x20000000..0x200017FF), guaranteed valid
// across every STM32F102 density (x6 = 6 KB, x8 = 10 KB, xB = 16 KB) even if
// the main stack pointer (MSP) overflowed or was corrupted prior to the fault.
#define SAFE_FAULT_MSP 0x20001800UL

#define NAKED_FAULT_TO_DFU()                                                                       \
    __asm__ volatile("cpsid i\n\t"                                                                 \
                     "ldr r0, =%0\n\t"                                                             \
                     "msr msp, r0\n\t"                                                             \
                     "b bootloader_enter_dfu\n\t"                                                  \
                     :                                                                             \
                     : "i"(SAFE_FAULT_MSP))

void relocate_vtor(void) {
    SCB_VTOR = (uint32_t)(uintptr_t)&vector_table;
}

void __attribute__((naked)) nmi_handler(void) {
    NAKED_FAULT_TO_DFU();
}

void __attribute__((naked)) hard_fault_handler(void) {
    NAKED_FAULT_TO_DFU();
}

void __attribute__((naked)) mem_manage_handler(void) {
    NAKED_FAULT_TO_DFU();
}

void __attribute__((naked)) bus_fault_handler(void) {
    NAKED_FAULT_TO_DFU();
}

void __attribute__((naked)) usage_fault_handler(void) {
    NAKED_FAULT_TO_DFU();
}

void sv_call_handler(void) {}

void debug_monitor_handler(void) {}

void pend_sv_handler(void) {}

void sys_tick_handler(void) {
    board_tick_inc();
}

void usb_lp_can_rx0_isr(void) {
    usb_midi_poll_isr();
}
