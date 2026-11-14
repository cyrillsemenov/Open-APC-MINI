#pragma once

#include "usb_lib.h"

#define APC_GetConfiguration NOP_Process
#define APC_SetConfiguration NOP_Process
#define APC_GetInterface NOP_Process
#define APC_SetInterface NOP_Process
#define APC_GetStatus NOP_Process
#define APC_ClearFeature NOP_Process
#define APC_SetEndPointFeature NOP_Process
#define APC_SetDeviceFeature NOP_Process
#define APC_SetDeviceAddress NOP_Process

void APC_init(void);
void APC_Reset(void);
void APC_Status_In(void);
void APC_Status_Out(void);
RESULT APC_Data_Setup(uint8_t RequestNo);
RESULT APC_NoData_Setup(uint8_t RequestNo);
RESULT APC_Get_Interface_Setting(uint8_t Interface, uint8_t AlternateSetting);
uint8_t *APC_GetDeviceDescriptor(uint16_t Length);
uint8_t *APC_GetConfigDescriptor(uint16_t Length);
uint8_t *APC_GetStringDescriptor(uint16_t Length);
