#include "usb_lib.h"
#include "usb_istr.h"
#include "usb_pwr.h"
#include "apc_mini.h"

static uint8_t usb_ep1_rx_len = 0;

void EP1_OUT_Callback(void) {
    usb_ep1_rx_len = (uint8_t)GetEPRxCount(ENDP1);
    if (usb_ep1_rx_len > 0) {
        PMAToUserBufferCopy(usb_ep1_rx_buf, GetEPRxAddr(ENDP1), usb_ep1_rx_len);
        handle_updater_unlock_and_reset();
        usb_rx_parse_packet();
        usb_ep1_rx_len = 0;
    }
    SetEPRxValid(ENDP1);
}

void EP1_IN_Callback(void) {
    usb_tx_busy = 0;
}

void SOF_Callback(void) {
    if (bDeviceState == CONFIGURED) {
        sof_flag = 1;
    }
}
