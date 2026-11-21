#include <stddef.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/st_usbfs.h>
#include <libopencm3/usb/usbd.h>
#include "apc_protocol.h"
#include "board.h"
#include "bootloader.h"
#include "usb_desc.h"
#include "usb_midi.h"

#define USB_MIDI_TX_RING_LEN 128U // 128 x 4B = 512 B
#define USB_MIDI_RX_RING_LEN 128U // 128 x 4B = 512 B
#define USB_MIDI_SYSEX_MAX   64U  // Max assembled SysEx length

#define USB_SER_TX_RING_LEN 256U // 256 B FTDI TX byte ring
#define USB_SER_RX_RING_LEN 256U // 256 B FTDI RX byte ring

#define FTDI_SIO_RESET             0x00U
#define FTDI_SIO_MODEM_CTRL        0x01U
#define FTDI_SIO_SET_FLOW_CTRL     0x02U
#define FTDI_SIO_SET_BAUD_RATE     0x03U
#define FTDI_SIO_SET_DATA          0x04U
#define FTDI_SIO_GET_MODEM_STATUS  0x05U
#define FTDI_SIO_SET_EVENT_CHAR    0x06U
#define FTDI_SIO_SET_ERROR_CHAR    0x07U
#define FTDI_SIO_SET_LATENCY_TIMER 0x09U
#define FTDI_SIO_GET_LATENCY_TIMER 0x0AU
#define FTDI_SIO_READ_EEPROM       0x90U

#define FTDI_MODEM_STATUS_B0 0x01U
#define FTDI_MODEM_STATUS_B1 0x60U

static usbd_device *s_usbd_dev = NULL;
static uint8_t s_usbd_ctrl_buf[192];
static volatile bool s_usb_configured = false;
static uint8_t s_ftdi_latency = 2U;

static uint8_t s_tx_ring[USB_MIDI_TX_RING_LEN][4];
static uint8_t s_tx_head = 0;
static uint8_t s_tx_tail = 0;

static uint8_t s_rx_ring[USB_MIDI_RX_RING_LEN][4];
static volatile uint8_t s_rx_head = 0;
static uint8_t s_rx_tail = 0;

static uint8_t s_sysex_buf[USB_MIDI_SYSEX_MAX];
static uint16_t s_sysex_len = 0;
static bool s_sysex_active = false;

static uint8_t s_ser_tx_ring[USB_SER_TX_RING_LEN];
static uint8_t s_ser_tx_head = 0;
static uint8_t s_ser_tx_tail = 0;

static uint8_t s_ser_rx_ring[USB_SER_RX_RING_LEN];
static volatile uint8_t s_ser_rx_head = 0;
static uint8_t s_ser_rx_tail = 0;

static void rx_ring_push_isr(const uint8_t ev[4]) {
    uint8_t next = (uint8_t)((s_rx_head + 1U) & (USB_MIDI_RX_RING_LEN - 1U));
    if (next == s_rx_tail) {
        return;
    }
    s_rx_ring[s_rx_head][0] = ev[0];
    s_rx_ring[s_rx_head][1] = ev[1];
    s_rx_ring[s_rx_head][2] = ev[2];
    s_rx_ring[s_rx_head][3] = ev[3];
    s_rx_head = next;
}

static void usb_ep1_out_cb(usbd_device *usbd_dev, uint8_t ep) {
    uint8_t pkt[64];
    uint16_t len = usbd_ep_read_packet(usbd_dev, ep, pkt, sizeof(pkt));

    for (uint16_t i = len; i < sizeof(pkt); i++) {
        pkt[i] = 0;
    }

    bootloader_check_unlock_packet(pkt);

    for (uint16_t off = 0; (off + 4U) <= len; off += 4U) {
        uint8_t cin = (uint8_t)(pkt[off] & 0x0FU);
        if (cin == USB_MIDI_CIN_MISC) {
            break;
        }
        rx_ring_push_isr(&pkt[off]);
    }
}

static void usb_ep1_in_cb(usbd_device *usbd_dev, uint8_t ep) {
    (void)usbd_dev;
    (void)ep;
}

static void usb_ftdi_out_cb(usbd_device *usbd_dev, uint8_t ep) {
    uint8_t pkt[64];
    uint16_t len = usbd_ep_read_packet(usbd_dev, ep, pkt, sizeof(pkt));

    for (uint16_t i = len; i < sizeof(pkt); i++) {
        pkt[i] = 0;
    }

    bootloader_check_unlock_packet(pkt);

    for (uint16_t i = 0; i < len; i++) {
        uint8_t next = (uint8_t)((s_ser_rx_head + 1U) & (USB_SER_RX_RING_LEN - 1U));
        if (next == s_ser_rx_tail) {
            break;
        }
        s_ser_rx_ring[s_ser_rx_head] = pkt[i];
        s_ser_rx_head = next;
    }
}

static enum usbd_request_return_codes
usb_ftdi_control_cb(usbd_device *usbd_dev, struct usb_setup_data *req, uint8_t **buf, uint16_t *len,
                    usbd_control_complete_callback *complete) {
    (void)usbd_dev;
    (void)complete;

    if ((req->bmRequestType & USB_REQ_TYPE_TYPE) != USB_REQ_TYPE_VENDOR) {
        return USBD_REQ_NEXT_CALLBACK;
    }

    if ((req->bmRequestType & USB_REQ_TYPE_DIRECTION) == USB_REQ_TYPE_IN) {
        switch (req->bRequest) {
        case FTDI_SIO_GET_MODEM_STATUS:
            (*buf)[0] = FTDI_MODEM_STATUS_B0;
            (*buf)[1] = FTDI_MODEM_STATUS_B1;
            *len = 2U;
            return USBD_REQ_HANDLED;

        case FTDI_SIO_GET_LATENCY_TIMER:
            (*buf)[0] = s_ftdi_latency;
            *len = 1U;
            return USBD_REQ_HANDLED;

        case FTDI_SIO_READ_EEPROM:
            (*buf)[0] = 0xFFU;
            (*buf)[1] = 0xFFU;
            *len = 2U;
            return USBD_REQ_HANDLED;

        default:
            *len = 0U;
            return USBD_REQ_HANDLED;
        }
    }

    // Vendor OUT requests (0x40)
    switch (req->bRequest) {
    case FTDI_SIO_RESET:
        if (req->wValue == 1U) {
            s_ser_rx_head = 0;
            s_ser_rx_tail = 0;
        } else if (req->wValue == 2U) {
            s_ser_tx_head = 0;
            s_ser_tx_tail = 0;
        }
        *len = 0U;
        return USBD_REQ_HANDLED;

    case FTDI_SIO_SET_LATENCY_TIMER:
        s_ftdi_latency = (uint8_t)(req->wValue & 0xFFU);
        *len = 0U;
        return USBD_REQ_HANDLED;

    case FTDI_SIO_MODEM_CTRL:
    case FTDI_SIO_SET_FLOW_CTRL:
    case FTDI_SIO_SET_BAUD_RATE:
    case FTDI_SIO_SET_DATA:
    case FTDI_SIO_SET_EVENT_CHAR:
    case FTDI_SIO_SET_ERROR_CHAR:
    default:
        *len = 0U;
        return USBD_REQ_HANDLED;
    }
}

static void usb_set_config_cb(usbd_device *usbd_dev, uint16_t wValue) {
    if (wValue == 0) {
        s_usb_configured = false;
        return;
    }

    usbd_ep_setup(usbd_dev, 0x01, USB_ENDPOINT_ATTR_BULK, 64, usb_ep1_out_cb);
    usbd_ep_setup(usbd_dev, 0x81, USB_ENDPOINT_ATTR_BULK, 64, usb_ep1_in_cb);
    usbd_ep_setup(usbd_dev, 0x02, USB_ENDPOINT_ATTR_BULK, 64, usb_ftdi_out_cb);
    usbd_ep_setup(usbd_dev, 0x03, USB_ENDPOINT_ATTR_BULK, 64, usb_ep1_out_cb);
    usbd_ep_setup(usbd_dev, 0x83, USB_ENDPOINT_ATTR_BULK, 64, usb_ep1_in_cb);
    usbd_register_control_callback(usbd_dev, USB_REQ_TYPE_VENDOR, USB_REQ_TYPE_TYPE,
                                   usb_ftdi_control_cb);
    s_usb_configured = true;
}

static void usb_reset_cb(void) {
    s_usb_configured = false;
    if (s_usbd_dev != NULL) {
        usbd_register_control_callback(s_usbd_dev, USB_REQ_TYPE_VENDOR, USB_REQ_TYPE_TYPE,
                                       usb_ftdi_control_cb);
    }
}

static void sysex_append_bytes(const uint8_t *bytes, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        uint8_t b = bytes[i];
        if (b == MIDI_STATUS_SYSEX_START) {
            s_sysex_len = 0;
            s_sysex_active = true;
        }
        if (!s_sysex_active) {
            continue;
        }
        if (s_sysex_len >= USB_MIDI_SYSEX_MAX) {
            s_sysex_active = false;
            s_sysex_len = 0;
            return;
        }
        s_sysex_buf[s_sysex_len++] = b;
    }
}

usb_profile_t usb_get_profile(void) {
    return USB_PROFILE_MONOME_FTDI;
}

void usb_midi_init(void) {
    s_usb_configured = false;
    s_tx_head = 0;
    s_tx_tail = 0;
    s_rx_head = 0;
    s_rx_tail = 0;
    s_sysex_len = 0;
    s_sysex_active = false;
    s_ser_tx_head = 0;
    s_ser_tx_tail = 0;
    s_ser_rx_head = 0;
    s_ser_rx_tail = 0;

    rcc_periph_clock_enable(RCC_USB);
    nvic_enable_irq(NVIC_USB_LP_CAN_RX0_IRQ);

    s_usbd_dev =
        usbd_init(&st_usbfs_v1_usb_driver, &usb_ftdi_dev_desc, &usb_ftdi_config_desc,
                  usb_ftdi_strings, USB_NUM_STRINGS, s_usbd_ctrl_buf, sizeof(s_usbd_ctrl_buf));
    usbd_register_control_callback(s_usbd_dev, USB_REQ_TYPE_VENDOR, USB_REQ_TYPE_TYPE,
                                   usb_ftdi_control_cb);

    usbd_register_set_config_callback(s_usbd_dev, usb_set_config_cb);
    usbd_register_reset_callback(s_usbd_dev, usb_reset_cb);

    board_usb_dp_pullup(true);
}

void usb_midi_poll_isr(void) {
    if (s_usbd_dev != NULL) {
        usbd_poll(s_usbd_dev);
    }
}

bool usb_midi_is_configured(void) {
    return s_usb_configured;
}

uint16_t usb_midi_ep1_write(const uint8_t *buf, uint16_t len) {
    if (s_usbd_dev == NULL) {
        return 0;
    }
    usbd_ep_write_packet(s_usbd_dev, 0x83, buf, len);
    return usbd_ep_write_packet(s_usbd_dev, 0x81, buf, len);
}

bool usb_midi_send_event(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3) {
    uint8_t next = (uint8_t)((s_tx_head + 1U) & (USB_MIDI_TX_RING_LEN - 1U));
    if (next == s_tx_tail) {
        return false;
    }
    s_tx_ring[s_tx_head][0] = (uint8_t)(cin & 0x0FU);
    s_tx_ring[s_tx_head][1] = b1;
    s_tx_ring[s_tx_head][2] = b2;
    s_tx_ring[s_tx_head][3] = b3;
    s_tx_head = next;
    return true;
}

bool usb_midi_send_sysex(const uint8_t *data, uint16_t len) {
    if (data == NULL || len < 2U) {
        return false;
    }

    uint16_t off = 0;
    while ((off + 3U) < len) {
        if (!usb_midi_send_event(USB_MIDI_CIN_SYSEX_START, data[off], data[off + 1U],
                                 data[off + 2U])) {
            return false;
        }
        off = (uint16_t)(off + 3U);
    }

    uint16_t rem = (uint16_t)(len - off);
    if (rem == 1U) {
        return usb_midi_send_event(USB_MIDI_CIN_SYSEX_END_1B, data[off], 0, 0);
    }
    if (rem == 2U) {
        return usb_midi_send_event(USB_MIDI_CIN_SYSEX_END_2B, data[off], data[off + 1U], 0);
    }
    return usb_midi_send_event(USB_MIDI_CIN_SYSEX_END_3B, data[off], data[off + 1U],
                               data[off + 2U]);
}

bool usb_serial_write(const uint8_t *data, uint16_t len) {
    if (data == NULL) {
        return false;
    }
    for (uint16_t i = 0; i < len; i++) {
        uint8_t next = (uint8_t)((s_ser_tx_head + 1U) & (USB_SER_TX_RING_LEN - 1U));
        if (next == s_ser_tx_tail) {
            return false;
        }
        s_ser_tx_ring[s_ser_tx_head] = data[i];
        s_ser_tx_head = next;
    }
    return true;
}

bool usb_serial_read_byte(uint8_t *out_byte) {
    if (s_ser_rx_tail == s_ser_rx_head) {
        return false;
    }
    *out_byte = s_ser_rx_ring[s_ser_rx_tail];
    s_ser_rx_tail = (uint8_t)((s_ser_rx_tail + 1U) & (USB_SER_RX_RING_LEN - 1U));
    return true;
}

void usb_midi_flush_tx(void) {
    if (!s_usb_configured) {
        return;
    }

    if (s_ser_tx_tail != s_ser_tx_head &&
        ((*USB_EP_REG(USB_EP1) & USB_EP_TX_STAT) != USB_EP_TX_STAT_VALID)) {
        uint8_t pkt[64];
        pkt[0] = FTDI_MODEM_STATUS_B0;
        pkt[1] = FTDI_MODEM_STATUS_B1;
        uint16_t offset = 2U;

        while (s_ser_tx_tail != s_ser_tx_head && offset < 64U) {
            pkt[offset++] = s_ser_tx_ring[s_ser_tx_tail];
            s_ser_tx_tail = (uint8_t)((s_ser_tx_tail + 1U) & (USB_SER_TX_RING_LEN - 1U));
        }

        usbd_ep_write_packet(s_usbd_dev, 0x81, pkt, offset);
    }

    if (s_tx_tail == s_tx_head) {
        return;
    }

    if ((*USB_EP_REG(USB_EP3) & USB_EP_TX_STAT) == USB_EP_TX_STAT_VALID) {
        return;
    }

    uint8_t pkt[64];
    uint8_t offset = 0;

    while (s_tx_tail != s_tx_head && offset < 64U) {
        pkt[offset + 0U] = s_tx_ring[s_tx_tail][0];
        pkt[offset + 1U] = s_tx_ring[s_tx_tail][1];
        pkt[offset + 2U] = s_tx_ring[s_tx_tail][2];
        pkt[offset + 3U] = s_tx_ring[s_tx_tail][3];
        s_tx_tail = (uint8_t)((s_tx_tail + 1U) & (USB_MIDI_TX_RING_LEN - 1U));
        offset = (uint8_t)(offset + 4U);
    }

    while (offset < 64U) {
        pkt[offset++] = 0;
    }

    usbd_ep_write_packet(s_usbd_dev, 0x83U, pkt, sizeof(pkt));
}

void usb_midi_poll_rx(usb_midi_channel_cb_t on_channel_msg, usb_midi_sysex_cb_t on_sysex_msg) {
    while (s_rx_tail != s_rx_head) {
        uint8_t ev[4];
        ev[0] = s_rx_ring[s_rx_tail][0];
        ev[1] = s_rx_ring[s_rx_tail][1];
        ev[2] = s_rx_ring[s_rx_tail][2];
        ev[3] = s_rx_ring[s_rx_tail][3];
        s_rx_tail = (uint8_t)((s_rx_tail + 1U) & (USB_MIDI_RX_RING_LEN - 1U));

        uint8_t cin = (uint8_t)(ev[0] & 0x0FU);
        switch (cin) {
        case USB_MIDI_CIN_SYSEX_START:
            sysex_append_bytes(&ev[1], 3U);
            break;

        case USB_MIDI_CIN_SYSEX_END_1B:
        case USB_MIDI_CIN_SYSEX_END_2B:
        case USB_MIDI_CIN_SYSEX_END_3B: {
            uint8_t n = (uint8_t)(cin - 4U);
            sysex_append_bytes(&ev[1], n);
            if (s_sysex_active && s_sysex_len >= 2U &&
                s_sysex_buf[s_sysex_len - 1U] == MIDI_STATUS_SYSEX_END) {
                if (on_sysex_msg != NULL) {
                    on_sysex_msg(s_sysex_buf, s_sysex_len);
                }
            }
            s_sysex_active = false;
            s_sysex_len = 0;
            break;
        }

        case USB_MIDI_CIN_NOTE_OFF:
        case USB_MIDI_CIN_NOTE_ON:
        case USB_MIDI_CIN_POLY_KEYPRESS:
        case USB_MIDI_CIN_CONTROL_CHANGE:
        case USB_MIDI_CIN_PITCH_BEND:
            s_sysex_active = false;
            s_sysex_len = 0;
            if (on_channel_msg != NULL) {
                on_channel_msg(ev[1], ev[2], ev[3]);
            }
            break;

        case USB_MIDI_CIN_PROGRAM_CHANGE:
        case USB_MIDI_CIN_CHAN_PRESSURE:
            s_sysex_active = false;
            s_sysex_len = 0;
            if (on_channel_msg != NULL) {
                on_channel_msg(ev[1], ev[2], 0);
            }
            break;

        case USB_MIDI_CIN_SINGLE_BYTE:
            if (ev[1] < MIDI_STATUS_REALTIME_MIN) {
                sysex_append_bytes(&ev[1], 1U);
                if (s_sysex_active && ev[1] == MIDI_STATUS_SYSEX_END) {
                    if (on_sysex_msg != NULL) {
                        on_sysex_msg(s_sysex_buf, s_sysex_len);
                    }
                    s_sysex_active = false;
                    s_sysex_len = 0;
                }
            }
            break;

        default:
            break;
        }
    }
}
