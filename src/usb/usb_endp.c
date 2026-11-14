#include <stddef.h>
#include <libopencm3/usb/usbd.h>
#include "hw_config.h"
#include "apc_mini.h"
#include "usb_desc.h"

// libopencm3 USBD handle & EP0 control buffer (>= 128 B to fit serialized config descriptor)
static usbd_device *g_usbd_dev = NULL;
static uint8_t g_usbd_ctrl_buf[128];
static volatile bool g_usb_configured = false;

static void usb_ep1_out_cb(usbd_device *usbd_dev, uint8_t ep) {
    uint16_t len = usbd_ep_read_packet(usbd_dev, ep, usb_ep1_rx_buf, sizeof(usb_ep1_rx_buf));
    uint16_t i;

    for (i = len; i < sizeof(usb_ep1_rx_buf); i++) {
        usb_ep1_rx_buf[i] = 0;
    }

    handle_updater_unlock_and_reset();
    usb_rx_parse_packet();
}

static void usb_ep1_in_cb(usbd_device *usbd_dev, uint8_t ep) {
    (void)usbd_dev;
    (void)ep;
    usb_tx_busy = 0;
}

static void usb_set_config_cb(usbd_device *usbd_dev, uint16_t wValue) {
    if (wValue == 0) {
        g_usb_configured = false;
        usb_tx_busy = 0;
        return;
    }

    usbd_ep_setup(usbd_dev, 0x01, USB_ENDPOINT_ATTR_BULK, 64, usb_ep1_out_cb);
    usbd_ep_setup(usbd_dev, 0x81, USB_ENDPOINT_ATTR_BULK, 64, usb_ep1_in_cb);
    usb_tx_busy = 0;
    g_usb_configured = true;
}

static void usb_reset_cb(void) {
    g_usb_configured = false;
    usb_tx_busy = 0;
}

static void usb_sof_cb(void) {
    sof_flag = 1;
}

void usb_midi_device_init(void) {
    g_usb_configured = false;
    usb_tx_busy = 0;

    g_usbd_dev = usbd_init(&st_usbfs_v1_usb_driver, &usb_dev_desc, &usb_config_desc, usb_strings,
                           USB_NUM_STRINGS, g_usbd_ctrl_buf, sizeof(g_usbd_ctrl_buf));

    usbd_register_set_config_callback(g_usbd_dev, usb_set_config_cb);
    usbd_register_reset_callback(g_usbd_dev, usb_reset_cb);
    usbd_register_sof_callback(g_usbd_dev, usb_sof_cb);

    usb_dp_pullup(true);
}

void usb_midi_poll_isr(void) {
    if (g_usbd_dev != NULL) {
        usbd_poll(g_usbd_dev);
    }
}

bool usb_midi_is_configured(void) {
    return g_usb_configured;
}

uint16_t usb_ep1_write(const uint8_t *buf, uint16_t len) {
    if (g_usbd_dev == NULL) {
        return 0;
    }
    return usbd_ep_write_packet(g_usbd_dev, 0x81, buf, len);
}
