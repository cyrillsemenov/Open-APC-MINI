#pragma once

#include <libopencm3/usb/usbd.h>

#ifndef USB_VID
    #define USB_VID 0x09E8
#endif
#ifndef USB_PID
    #define USB_PID 0x0028
#endif
#ifndef USB_STRING_VENDOR
    #define USB_STRING_VENDOR "monome"
#endif
#ifndef USB_STRING_PRODUCT
    #define USB_STRING_PRODUCT "monome APC MINI"
#endif
#ifndef USB_STRING_SERIAL
    #define USB_STRING_SERIAL "dev"
#endif
#define USB_STRING_MONOME_SERIAL "m1000001"

#define USB_NUM_STRINGS 3

typedef enum {
    USB_PROFILE_APC_MIDI = 0,
    USB_PROFILE_COMPOSITE_CDC_MIDI = 1,
} usb_profile_t;

extern const struct usb_device_descriptor usb_dev_desc;
extern const struct usb_config_descriptor usb_config_desc;
extern const char *const usb_strings[USB_NUM_STRINGS];
