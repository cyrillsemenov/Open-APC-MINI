#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/vector.h>
#include "hw_config.h"
#include "apc_mini.h"

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
    if (scan_gate != 0) {
        scan_gate--;
    }
    if (led_flush_gate != 0) {
        led_flush_gate--;
    }

    if (sys_tick_div2++ >= 1) {
        sys_tick_div2 = 0;
        if (round_gate != 0) {
            round_gate--;
        }
    }

    if (blink_timer != 0) {
        blink_timer--;
    }

    if (adc_tick_div10++ >= 9) {
        adc_tick_div10 = 0;
    }
}

void usb_lp_can_rx0_isr(void) {
    usb_midi_poll_isr();
}
