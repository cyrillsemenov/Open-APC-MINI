#include <stddef.h>
#include <libopencm3/usb/audio.h>
#include <libopencm3/usb/midi.h>
#include <libopencm3/usb/usbd.h>
#include "usb_desc.h"

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

/*---------------------------------------------------------------------------
 * Composite Monome FTDI FT232R (0403:6001) + USB-MIDI Descriptor Set
 *---------------------------------------------------------------------------*/
const struct usb_device_descriptor usb_ftdi_dev_desc = {
    .bLength = USB_DT_DEVICE_SIZE,
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x0403,
    .idProduct = 0x6001,
    .bcdDevice = 0x0600,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const struct usb_endpoint_descriptor ftdi_bulk_endp[] = {
    {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x81,
        .bmAttributes = USB_ENDPOINT_ATTR_BULK,
        .wMaxPacketSize = 64,
        .bInterval = 0x00,
    },
    {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x02,
        .bmAttributes = USB_ENDPOINT_ATTR_BULK,
        .wMaxPacketSize = 64,
        .bInterval = 0x00,
    },
};

static const struct usb_interface_descriptor ftdi_iface_desc[] = {{
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = USB_CLASS_VENDOR,
    .bInterfaceSubClass = 0xFF,
    .bInterfaceProtocol = 0xFF,
    .iInterface = 2,
    .endpoint = ftdi_bulk_endp,
    .extra = NULL,
    .extralen = 0,
}};

static const struct {
    struct usb_audio_header_descriptor_head header_head;
    struct usb_audio_header_descriptor_body header_body;
} __attribute__((packed)) ftdi_audio_control_functional_descriptors = {
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
            .baInterfaceNr = 0x02,
        },
};

static const struct usb_interface_descriptor ftdi_audio_control_iface[] = {{
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 1,
    .bAlternateSetting = 0,
    .bNumEndpoints = 0,
    .bInterfaceClass = USB_CLASS_AUDIO,
    .bInterfaceSubClass = USB_AUDIO_SUBCLASS_CONTROL,
    .bInterfaceProtocol = 0,
    .iInterface = 0,
    .extra = &ftdi_audio_control_functional_descriptors,
    .extralen = sizeof(ftdi_audio_control_functional_descriptors),
}};

static const struct usb_endpoint_descriptor ftdi_midi_bulk_endp[] = {
    {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x03,
        .bmAttributes = USB_ENDPOINT_ATTR_BULK,
        .wMaxPacketSize = 64,
        .bInterval = 0x00,
        .extra = &midi_bulk_endp[0],
        .extralen = sizeof(midi_bulk_endp[0]),
    },
    {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x83,
        .bmAttributes = USB_ENDPOINT_ATTR_BULK,
        .wMaxPacketSize = 64,
        .bInterval = 0x00,
        .extra = &midi_bulk_endp[1],
        .extralen = sizeof(midi_bulk_endp[1]),
    },
};

static const struct usb_interface_descriptor ftdi_midi_streaming_iface[] = {{
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 2,
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = USB_CLASS_AUDIO,
    .bInterfaceSubClass = USB_AUDIO_SUBCLASS_MIDISTREAMING,
    .bInterfaceProtocol = 0,
    .iInterface = 0,
    .endpoint = ftdi_midi_bulk_endp,
    .extra = &midi_streaming_functional_descriptors,
    .extralen = sizeof(midi_streaming_functional_descriptors),
}};

static const struct usb_interface ftdi_ifaces[] = {
    {
        .num_altsetting = 1,
        .altsetting = ftdi_iface_desc,
    },
    {
        .num_altsetting = 1,
        .altsetting = ftdi_audio_control_iface,
    },
    {
        .num_altsetting = 1,
        .altsetting = ftdi_midi_streaming_iface,
    },
};

const struct usb_config_descriptor usb_ftdi_config_desc = {
    .bLength = USB_DT_CONFIGURATION_SIZE,
    .bDescriptorType = USB_DT_CONFIGURATION,
    .wTotalLength = 0,
    .bNumInterfaces = 3,
    .bConfigurationValue = 1,
    .iConfiguration = 0,
    .bmAttributes = USB_CONFIG_ATTR_DEFAULT,
    .bMaxPower = 0xFA, // 500 mA
    .interface = ftdi_ifaces,
};

const char *const usb_ftdi_strings[USB_NUM_STRINGS] = {
    USB_STRING_VENDOR,
    USB_STRING_PRODUCT,
    "m1000001",
};
