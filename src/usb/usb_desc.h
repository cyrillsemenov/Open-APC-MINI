#pragma once

#include "stm32f10x.h"

#define USB_DEVICE_DESCRIPTOR_TYPE        0x01
#define USB_CONFIGURATION_DESCRIPTOR_TYPE 0x02
#define USB_STRING_DESCRIPTOR_TYPE        0x03
#define USB_INTERFACE_DESCRIPTOR_TYPE     0x04
#define USB_ENDPOINT_DESCRIPTOR_TYPE      0x05

#ifndef USB_VID
    #define USB_VID 0x09E8
#endif
#ifndef USB_PID
    #define USB_PID 0x0028
#endif
#ifndef USB_STRING_VENDOR
    #define USB_STRING_VENDOR "AKAI PROFESSIONAL,LP"
#endif
#ifndef USB_STRING_PRODUCT
    #define USB_STRING_PRODUCT "APC MINI"
#endif
#ifndef USB_STRING_SERIAL
    #define USB_STRING_SERIAL "dev"
#endif

#define USB_STRING_DESC_SIZE(str) (2u + (sizeof(str) - 1u) * 2u)

#define APC_SIZ_DEVICE_DESC    18
#define APC_SIZ_CONFIG_DESC    101
#define APC_SIZ_STRING_LANGID  4
#define APC_SIZ_STRING_VENDOR  USB_STRING_DESC_SIZE(USB_STRING_VENDOR)
#define APC_SIZ_STRING_PRODUCT USB_STRING_DESC_SIZE(USB_STRING_PRODUCT)
#define APC_SIZ_STRING_SERIAL  USB_STRING_DESC_SIZE(USB_STRING_SERIAL)

extern const uint8_t APC_DeviceDescriptor[APC_SIZ_DEVICE_DESC];
extern const uint8_t APC_ConfigDescriptor[APC_SIZ_CONFIG_DESC];
extern const uint8_t APC_StringLangID[APC_SIZ_STRING_LANGID];
extern const uint8_t APC_StringVendor[APC_SIZ_STRING_VENDOR];
extern const uint8_t APC_StringProduct[APC_SIZ_STRING_PRODUCT];
extern const uint8_t APC_StringSerial[APC_SIZ_STRING_SERIAL];
