#include "stm32f10x.h"
#include "usb_istr.h"
#include "apc_mini.h"

void NMI_Handler(void) {}

void HardFault_Handler(void) {
    while (1) {}
}

void MemManage_Handler(void) {
    while (1) {}
}

void BusFault_Handler(void) {
    while (1) {}
}

void UsageFault_Handler(void) {
    while (1) {}
}

void SVC_Handler(void) {}

void DebugMon_Handler(void) {}

void PendSV_Handler(void) {}

void SysTick_Handler(void) {
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

void USB_LP_CAN1_RX0_IRQHandler(void) {
    USB_Istr();
}
