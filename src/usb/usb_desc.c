#include <stddef.h>
#include <libopencm3/usb/audio.h>
#include <libopencm3/usb/cdc.h>
#include <libopencm3/usb/midi.h>
#include <libopencm3/usb/usbd.h>
#include "usb_desc.h"

/*---------------------------------------------------------------------------
 * Composite USB-MIDI + Monome CDC-ACM (mext) Descriptor Set
 *---------------------------------------------------------------------------*/
const struct usb_device_descriptor usb_dev_desc = {
    .bLength = USB_DT_DEVICE_SIZE,
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0xEF,
    .bDeviceSubClass = 0x02,
    .bDeviceProtocol = 0x01,
    .bMaxPacketSize0 = 64,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0200,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

/*---------------------------------------------------------------------------
 * Function 1: Class-Compliant USB-MIDI (Interfaces 0 & 1, EP 0x01 / 0x81)
 *---------------------------------------------------------------------------*/
static const struct usb_iface_assoc_descriptor midi_iad = {
    .bLength = USB_DT_INTERFACE_ASSOCIATION_SIZE,
    .bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,
    .bFirstInterface = 0,
    .bInterfaceCount = 2,
    .bFunctionClass = USB_CLASS_AUDIO,
    .bFunctionSubClass = USB_AUDIO_SUBCLASS_CONTROL,
    .bFunctionProtocol = 0,
    .iFunction = 0,
};

static const struct {
    struct usb_audio_header_descriptor_head header_head;
    struct usb_audio_header_descriptor_body header_body;
} __attribute__((packed)) audio_control_functional_descriptors = {
    .header_head =
        {
            .bLength = sizeof(struct usb_audio_header_descriptor_head) +
                       sizeof(struct usb_audio_header_descriptor_body),
            .bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
            .bDescriptorSubtype = USB_AUDIO_TYPE_HEADER,
            .bcdADC = 0x0100,
            .wTotalLength = sizeof(struct usb_audio_header_descriptor_head) +
                            sizeof(struct usb_audio_header_descriptor_body),
            .binCollection = 1,
        },
    .header_body =
        {
            .baInterfaceNr = 0x01,
        },
};

static const struct usb_interface_descriptor audio_control_iface[] = {{
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 0,
    .bInterfaceClass = USB_CLASS_AUDIO,
    .bInterfaceSubClass = USB_AUDIO_SUBCLASS_CONTROL,
    .bInterfaceProtocol = 0,
    .iInterface = 0,
    .extra = &audio_control_functional_descriptors,
    .extralen = sizeof(audio_control_functional_descriptors),
}};

static const struct usb_midi_endpoint_descriptor midi_bulk_endp[] = {
    {
        // EP 1 OUT -> Embedded MIDI IN Jack 1
        .head =
            {
                .bLength = sizeof(struct usb_midi_endpoint_descriptor),
                .bDescriptorType = USB_AUDIO_DT_CS_ENDPOINT,
                .bDescriptorSubType = USB_MIDI_SUBTYPE_MS_GENERAL,
                .bNumEmbMIDIJack = 1,
            },
        .jack[0] =
            {
                .baAssocJackID = 0x01,
            },
    },
    {
        // EP 1 IN <- Embedded MIDI OUT Jack 3
        .head =
            {
                .bLength = sizeof(struct usb_midi_endpoint_descriptor),
                .bDescriptorType = USB_AUDIO_DT_CS_ENDPOINT,
                .bDescriptorSubType = USB_MIDI_SUBTYPE_MS_GENERAL,
                .bNumEmbMIDIJack = 1,
            },
        .jack[0] =
            {
                .baAssocJackID = 0x03,
            },
    },
};

static const struct usb_endpoint_descriptor midi_endp[] = {
    {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x01,
        .bmAttributes = USB_ENDPOINT_ATTR_BULK,
        .wMaxPacketSize = 64,
        .bInterval = 0x00,
        .extra = &midi_bulk_endp[0],
        .extralen = sizeof(midi_bulk_endp[0]),
    },
    {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x81,
        .bmAttributes = USB_ENDPOINT_ATTR_BULK,
        .wMaxPacketSize = 64,
        .bInterval = 0x00,
        .extra = &midi_bulk_endp[1],
        .extralen = sizeof(midi_bulk_endp[1]),
    },
};

static const struct {
    struct usb_midi_header_descriptor header;
    struct usb_midi_in_jack_descriptor in_embedded;
    struct usb_midi_in_jack_descriptor in_external;
    struct usb_midi_out_jack_descriptor out_embedded;
    struct usb_midi_out_jack_descriptor out_external;
} __attribute__((packed)) midi_streaming_functional_descriptors = {
    .header =
        {
            .bLength = sizeof(struct usb_midi_header_descriptor),
            .bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
            .bDescriptorSubtype = USB_MIDI_SUBTYPE_MS_HEADER,
            .bcdMSC = 0x0100,
            .wTotalLength =
                sizeof(midi_streaming_functional_descriptors) +
                2 * (USB_DT_ENDPOINT_SIZE + sizeof(struct usb_midi_endpoint_descriptor)),
        },
    .in_embedded =
        {
            .bLength = sizeof(struct usb_midi_in_jack_descriptor),
            .bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
            .bDescriptorSubtype = USB_MIDI_SUBTYPE_MIDI_IN_JACK,
            .bJackType = USB_MIDI_JACK_TYPE_EMBEDDED,
            .bJackID = 0x01,
            .iJack = 0x00,
        },
    .in_external =
        {
            .bLength = sizeof(struct usb_midi_in_jack_descriptor),
            .bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
            .bDescriptorSubtype = USB_MIDI_SUBTYPE_MIDI_IN_JACK,
            .bJackType = USB_MIDI_JACK_TYPE_EXTERNAL,
            .bJackID = 0x02,
            .iJack = 0x00,
        },
    .out_embedded =
        {
            .head =
                {
                    .bLength = sizeof(struct usb_midi_out_jack_descriptor),
                    .bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
                    .bDescriptorSubtype = USB_MIDI_SUBTYPE_MIDI_OUT_JACK,
                    .bJackType = USB_MIDI_JACK_TYPE_EMBEDDED,
                    .bJackID = 0x03,
                    .bNrInputPins = 1,
                },
            .source[0] =
                {
                    .baSourceID = 0x02,
                    .baSourcePin = 1,
                },
            .tail =
                {
                    .iJack = 0x00,
                },
        },
    .out_external =
        {
            .head =
                {
                    .bLength = sizeof(struct usb_midi_out_jack_descriptor),
                    .bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
                    .bDescriptorSubtype = USB_MIDI_SUBTYPE_MIDI_OUT_JACK,
                    .bJackType = USB_MIDI_JACK_TYPE_EXTERNAL,
                    .bJackID = 0x04,
                    .bNrInputPins = 1,
                },
            .source[0] =
                {
                    .baSourceID = 0x01,
                    .baSourcePin = 1,
                },
            .tail =
                {
                    .iJack = 0x00,
                },
        },
};

static const struct usb_interface_descriptor midi_streaming_iface[] = {{
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 1,
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = USB_CLASS_AUDIO,
    .bInterfaceSubClass = USB_AUDIO_SUBCLASS_MIDISTREAMING,
    .bInterfaceProtocol = 0,
    .iInterface = 0,
    .endpoint = midi_endp,
    .extra = &midi_streaming_functional_descriptors,
    .extralen = sizeof(midi_streaming_functional_descriptors),
}};

/*---------------------------------------------------------------------------
 * Function 2: USB CDC-ACM Serial (Interfaces 2 & 3, EP 0x83, EP 0x02 / 0x82)
 *---------------------------------------------------------------------------*/
static const struct usb_iface_assoc_descriptor cdc_iad = {
    .bLength = USB_DT_INTERFACE_ASSOCIATION_SIZE,
    .bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,
    .bFirstInterface = 2,
    .bInterfaceCount = 2,
    .bFunctionClass = USB_CLASS_CDC,
    .bFunctionSubClass = USB_CDC_SUBCLASS_ACM,
    .bFunctionProtocol = USB_CDC_PROTOCOL_NONE,
    .iFunction = 0,
};

static const struct {
    struct usb_cdc_header_descriptor header;
    struct usb_cdc_call_management_descriptor call_mgmt;
    struct usb_cdc_acm_descriptor acm;
    struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) cdc_functional_descriptors = {
    .header =
        {
            .bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
            .bDescriptorType = CS_INTERFACE,
            .bDescriptorSubtype = USB_CDC_TYPE_HEADER,
            .bcdCDC = 0x0110,
        },
    .call_mgmt =
        {
            .bFunctionLength = sizeof(struct usb_cdc_call_management_descriptor),
            .bDescriptorType = CS_INTERFACE,
            .bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
            .bmCapabilities = 0,
            .bDataInterface = 3,
        },
    .acm =
        {
            .bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
            .bDescriptorType = CS_INTERFACE,
            .bDescriptorSubtype = USB_CDC_TYPE_ACM,
            .bmCapabilities = 0x02,
        },
    .cdc_union =
        {
            .bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
            .bDescriptorType = CS_INTERFACE,
            .bDescriptorSubtype = USB_CDC_TYPE_UNION,
            .bControlInterface = 2,
            .bSubordinateInterface0 = 3,
        },
};

static const struct usb_endpoint_descriptor cdc_comm_endp[] = {{
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = 0x83,
    .bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
    .wMaxPacketSize = 16,
    .bInterval = 255,
}};

static const struct usb_interface_descriptor cdc_comm_iface[] = {{
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 2,
    .bAlternateSetting = 0,
    .bNumEndpoints = 1,
    .bInterfaceClass = USB_CLASS_CDC,
    .bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
    .bInterfaceProtocol = USB_CDC_PROTOCOL_NONE,
    .iInterface = 0,
    .endpoint = cdc_comm_endp,
    .extra = &cdc_functional_descriptors,
    .extralen = sizeof(cdc_functional_descriptors),
}};

static const struct usb_endpoint_descriptor cdc_data_endp[] = {
    {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x02,
        .bmAttributes = USB_ENDPOINT_ATTR_BULK,
        .wMaxPacketSize = 64,
        .bInterval = 0x00,
    },
    {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x82,
        .bmAttributes = USB_ENDPOINT_ATTR_BULK,
        .wMaxPacketSize = 64,
        .bInterval = 0x00,
    },
};

static const struct usb_interface_descriptor cdc_data_iface[] = {{
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 3,
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = USB_CLASS_DATA,
    .bInterfaceSubClass = 0,
    .bInterfaceProtocol = 0,
    .iInterface = 0,
    .endpoint = cdc_data_endp,
    .extra = NULL,
    .extralen = 0,
}};

static const struct usb_interface usb_ifaces[] = {
    {
        .iface_assoc = &midi_iad,
        .num_altsetting = 1,
        .altsetting = audio_control_iface,
    },
    {
        .iface_assoc = NULL,
        .num_altsetting = 1,
        .altsetting = midi_streaming_iface,
    },
    {
        .iface_assoc = &cdc_iad,
        .num_altsetting = 1,
        .altsetting = cdc_comm_iface,
    },
    {
        .iface_assoc = NULL,
        .num_altsetting = 1,
        .altsetting = cdc_data_iface,
    },
};

const struct usb_config_descriptor usb_config_desc = {
    .bLength = USB_DT_CONFIGURATION_SIZE,
    .bDescriptorType = USB_DT_CONFIGURATION,
    .wTotalLength = 0,
    .bNumInterfaces = 4,
    .bConfigurationValue = 1,
    .iConfiguration = 0,
    .bmAttributes = USB_CONFIG_ATTR_DEFAULT,
    .bMaxPower = 0xFA, // 500 mA
    .interface = usb_ifaces,
};

const char *const usb_strings[USB_NUM_STRINGS] = {
    USB_STRING_VENDOR,
    USB_STRING_PRODUCT,
    USB_STRING_MONOME_SERIAL,
};
