#pragma once

#include "stm32f10x.h"
#include "usb_type.h"

void Set_System(void);
void RCC_Configuration(void);
void GPIO_Configuration(void);
void ADC_Configuration(void);
void IWDG_Configuration(void);
void SysTick_Configuration(void);
void Set_USBClock(void);
void Enter_LowPowerMode(void);
void Leave_LowPowerMode(void);
void USB_Interrupts_Config(void);
void USB_Cable_Config(FunctionalState NewState);

void hw_init(void);
void usb_midi_init(void);
void periph_app_init(void);
