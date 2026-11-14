#include "usb_lib.h"
#include "usb_conf.h"
#include "usb_prop.h"
#include "usb_desc.h"
#include "usb_pwr.h"
#include "hw_config.h"

DEVICE Device_Table = {EP_NUM, 1};

DEVICE_PROP Device_Property = {
    APC_init,
    APC_Reset,
    APC_Status_In,
    APC_Status_Out,
    APC_Data_Setup,
    APC_NoData_Setup,
    APC_Get_Interface_Setting,
    APC_GetDeviceDescriptor,
    APC_GetConfigDescriptor,
    APC_GetStringDescriptor,
    0,
    0x40, // MaxPacketSize = 64
};

USER_STANDARD_REQUESTS User_Standard_Requests = {
    APC_GetConfiguration,   APC_SetConfiguration, APC_GetInterface,
    APC_SetInterface,       APC_GetStatus,        APC_ClearFeature,
    APC_SetEndPointFeature, APC_SetDeviceFeature, APC_SetDeviceAddress};

ONE_DESCRIPTOR Device_Descriptor = {(uint8_t *)APC_DeviceDescriptor, APC_SIZ_DEVICE_DESC};

ONE_DESCRIPTOR Config_Descriptor = {(uint8_t *)APC_ConfigDescriptor, APC_SIZ_CONFIG_DESC};

ONE_DESCRIPTOR String_Descriptor[4] = {
    {(uint8_t *)APC_StringLangID, APC_SIZ_STRING_LANGID},
    {(uint8_t *)APC_StringVendor, APC_SIZ_STRING_VENDOR},
    {(uint8_t *)APC_StringProduct, APC_SIZ_STRING_PRODUCT},
    {(uint8_t *)APC_StringSerial, APC_SIZ_STRING_SERIAL},
};

void APC_init(void) {
    pInformation->Current_Configuration = 0;
    PowerOn();
    USB_SIL_Init();
    bDeviceState = UNCONNECTED;
}

void APC_Reset(void) {
    pInformation->Current_Configuration = 0;
    pInformation->Current_Feature = APC_ConfigDescriptor[7];
    pInformation->Current_Interface = 0;

    SetBTABLE(BTABLE_ADDRESS);

    // Initialize Endpoint 0 (Control)
    SetEPType(ENDP0, EP_CONTROL);
    SetEPTxStatus(ENDP0, EP_TX_STALL);
    SetEPRxAddr(ENDP0, ENDP0_RXADDR);
    SetEPTxAddr(ENDP0, ENDP0_TXADDR);
    Clear_Status_Out(ENDP0);
    SetEPRxCount(ENDP0, Device_Property.MaxPacketSize);
    SetEPRxValid(ENDP0);

    // Initialize Endpoint 1 (Bulk IN + Bulk OUT, 64 bytes)
    SetEPType(ENDP1, EP_BULK);
    SetEPTxAddr(ENDP1, ENDP1_TXADDR);
    SetEPTxStatus(ENDP1, EP_TX_NAK);
    SetEPRxAddr(ENDP1, ENDP1_RXADDR);
    SetEPRxCount(ENDP1, 0x40);
    SetEPRxStatus(ENDP1, EP_RX_VALID);

    SetDeviceAddress(0);
    bDeviceState = ATTACHED;
}

void APC_Status_In(void) {}

void APC_Status_Out(void) {}

RESULT APC_Data_Setup(uint8_t RequestNo) {
    (void)RequestNo;
    return USB_UNSUPPORT;
}

RESULT APC_NoData_Setup(uint8_t RequestNo) {
    (void)RequestNo;
    return USB_UNSUPPORT;
}

uint8_t *APC_GetDeviceDescriptor(uint16_t Length) {
    return Standard_GetDescriptorData(Length, &Device_Descriptor);
}

uint8_t *APC_GetConfigDescriptor(uint16_t Length) {
    return Standard_GetDescriptorData(Length, &Config_Descriptor);
}

uint8_t *APC_GetStringDescriptor(uint16_t Length) {
    uint8_t wValue0 = pInformation->USBwValue0;
    if (wValue0 > 4) {
        return NULL;
    } else {
        return Standard_GetDescriptorData(Length, &String_Descriptor[wValue0]);
    }
}

RESULT APC_Get_Interface_Setting(uint8_t Interface, uint8_t AlternateSetting) {
    if (AlternateSetting > 0) {
        return USB_UNSUPPORT;
    } else if (Interface > 1) {
        return USB_UNSUPPORT;
    }
    return USB_SUCCESS;
}
