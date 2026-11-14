#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/vector.h>
#include "board.h"
#include "usb_midi.h"

void relocate_vtor(void) {
    SCB_VTOR = (uint32_t)(uintptr_t)&vector_table;
}

void nmi_handler(void) {}

void hard_fault_handler(void) {
    while (1) {}
}

void mem_manage_handler(void) {
    while (1) {}
}

void bus_fault_handler(void) {
    while (1) {}
}

void usage_fault_handler(void) {
    while (1) {}
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
