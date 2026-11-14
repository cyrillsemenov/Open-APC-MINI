#include <stdint.h>
#include "usb_lib.h"
#include "usb_desc.h"

#define USB_VID_LSB ((uint8_t)((USB_VID) & 0xFFu))
#define USB_VID_MSB ((uint8_t)(((USB_VID) >> 8) & 0xFFu))
#define USB_PID_LSB ((uint8_t)((USB_PID) & 0xFFu))
#define USB_PID_MSB ((uint8_t)(((USB_PID) >> 8) & 0xFFu))

// clang-format off
const uint8_t APC_DeviceDescriptor[APC_SIZ_DEVICE_DESC] = {
    0x12,                       // bLength
    USB_DEVICE_DESCRIPTOR_TYPE, // bDescriptorType
    0x10, 0x01,                 // bcdUSB = 1.10
    0x00,                       // bDeviceClass (defined at interface level)
    0x00,                       // bDeviceSubClass
    0x00,                       // bDeviceProtocol
    0x40,                       // bMaxPacketSize0 = 64
    USB_VID_LSB, USB_VID_MSB,   // idVendor
    USB_PID_LSB, USB_PID_MSB,   // idProduct
    0x00, 0x01,                 // bcdDevice = 1.00
    0x01,                       // iManufacturer
    0x02,                       // iProduct
    0x03,                       // iSerialNumber
    0x01                        // bNumConfigurations
};

// 101-byte USB Configuration Descriptor (Audio Control + MIDI Streaming)
const uint8_t APC_ConfigDescriptor[APC_SIZ_CONFIG_DESC] = {
    // Configuration Descriptor (9 B)
    0x09, 0x02, 0x65, 0x00, 0x02, 0x01, 0x00, 0x80, 0xFA,
    // Audio Control Interface Descriptor (9 B)
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    // Class-Specific AC Interface Descriptor (9 B)
    0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x01,
    // MIDI Streaming Interface Descriptor (9 B)
    0x09, 0x04, 0x01, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,
    // Class-Specific MS Interface Header Descriptor (7 B)
    0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00,
    // MIDI IN Jack 1 (Embedded) (6 B)
    0x06, 0x24, 0x02, 0x01, 0x01, 0x00,
    // MIDI IN Jack 2 (External) (6 B)
    0x06, 0x24, 0x02, 0x02, 0x02, 0x00,
    // MIDI OUT Jack 3 (Embedded) (6 B)
    0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00,
    // MIDI OUT Jack 4 (External) (9 B)
    0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x00,
    // Endpoint 1 OUT Descriptor (9 B, Bulk, 64 B)
    0x09, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
    // Class-Specific MS Bulk OUT Endpoint Descriptor (5 B, Assoc Jack 1)
    0x05, 0x25, 0x01, 0x01, 0x01,
    // Endpoint 1 IN Descriptor (9 B, Bulk, 64 B)
    0x09, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
    // Class-Specific MS Bulk IN Endpoint Descriptor (5 B, Assoc Jack 3)
    0x05, 0x25, 0x01, 0x01, 0x03
};

const uint8_t APC_StringLangID[APC_SIZ_STRING_LANGID] = {
    APC_SIZ_STRING_LANGID,
    USB_STRING_DESCRIPTOR_TYPE,
    0x09, 0x04
};
// clang-format on

#define USB_CONCAT_(a, b) a##b
#define USB_CONCAT(a, b)  USB_CONCAT_(a, b)

#define DEFINE_USB_STRING(name, str)                                                               \
    static const struct __attribute__((packed)) {                                                  \
        uint8_t bLength;                                                                           \
        uint8_t bDescriptorType;                                                                   \
        __CHAR16_TYPE__ bString[sizeof(str) - 1];                                                  \
    } name##_raw = {                                                                               \
        .bLength = USB_STRING_DESC_SIZE(str),                                                      \
        .bDescriptorType = USB_STRING_DESCRIPTOR_TYPE,                                             \
        .bString = USB_CONCAT(u, str),                                                             \
    };                                                                                             \
    extern const uint8_t name[USB_STRING_DESC_SIZE(str)] __attribute__((alias(#name "_raw")))

DEFINE_USB_STRING(APC_StringVendor, USB_STRING_VENDOR);
DEFINE_USB_STRING(APC_StringProduct, USB_STRING_PRODUCT);
DEFINE_USB_STRING(APC_StringSerial, USB_STRING_SERIAL);
