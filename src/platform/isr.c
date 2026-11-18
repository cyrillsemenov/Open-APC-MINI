#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/vector.h>
#include "board.h"
#include "bootloader.h"
#include "usb_midi.h"

#define NAKED_FAULT_TO_DFU()                                                                       \
    __asm__ volatile("cpsid i\n\t"                                                                 \
                     "mrs r0, msp\n\t"                                                             \
                     "mrs r1, ipsr\n\t"                                                            \
                     "ldr r2, =%0\n\t"                                                             \
                     "msr msp, r2\n\t"                                                             \
                     "b bootloader_fault_dfu\n\t"                                                  \
                     :                                                                             \
                     : "i"(&g_fault_stack[64]))

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
