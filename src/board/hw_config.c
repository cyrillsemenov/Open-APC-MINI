#include "hw_config.h"
#include "apc_mini.h"
#include "usb_lib.h"
#include "usb_pwr.h"

// Legacy StdPeriph V2.0 SysTick helpers present in stock binary
#define SysTick_Counter_Disable ((uint32_t)0xFFFFFFFE)
#define SysTick_Counter_Enable  ((uint32_t)0x00000001)
#define SysTick_Counter_Clear   ((uint32_t)0x00000000)

static void SysTick_SetReload(uint32_t Reload) {
    SysTick->LOAD = Reload;
}

static void SysTick_ITConfig(FunctionalState NewState) {
    if (NewState != DISABLE) {
        SysTick->CTRL |= (1UL << 1);
    } else {
        SysTick->CTRL &= ~(1UL << 1);
    }
}

static void SysTick_CounterCmd(uint32_t SysTick_Counter) {
    if (SysTick_Counter == SysTick_Counter_Enable) {
        SysTick->CTRL |= SysTick_Counter_Enable;
    } else if (SysTick_Counter == SysTick_Counter_Disable) {
        SysTick->CTRL &= SysTick_Counter_Disable;
    } else {
        SysTick->VAL = SysTick_Counter_Clear;
    }
}

void RCC_Configuration(void) {
    ErrorStatus HSEStartUpStatus;

    RCC_DeInit();
    RCC_HSEConfig(RCC_HSE_ON);
    HSEStartUpStatus = RCC_WaitForHSEStartUp();

    if (HSEStartUpStatus == SUCCESS) {
        FLASH_PrefetchBufferCmd(FLASH_PrefetchBuffer_Enable);
        FLASH_SetLatency(FLASH_Latency_2);

        RCC_HCLKConfig(RCC_SYSCLK_Div1);
        RCC_PCLK2Config(RCC_HCLK_Div1);
        RCC_PCLK1Config(RCC_HCLK_Div2);
        RCC_ADCCLKConfig(RCC_PCLK2_Div6);

        // 8 MHz HSE * 6 = 48 MHz SYSCLK
        RCC_PLLConfig(RCC_PLLSource_HSE_Div1, RCC_PLLMul_6);
        RCC_PLLCmd(ENABLE);

        while (RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET) {}

        RCC_SYSCLKConfig(RCC_SYSCLKSource_PLLCLK);

        while (RCC_GetSYSCLKSource() != 0x08) {}
    }
}

void GPIO_Configuration(void) {
    GPIO_InitTypeDef GPIO_InitStructure;

    // Enable AFIO and disable JTAG (keep SWD on PA13/PA14, free PA15/PB3/PB4)
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);

    // Enable GPIOA, GPIOB, GPIOC, GPIOD clocks
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD, ENABLE);

    // PB8..PB11: Round LED column sinks -> Out_PP 2 MHz
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // PB12..PB14: TM1629 DIO, CLK, STB -> Out_OD 2 MHz
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // PC0..PC3, PC5..PC15 (0xFFEF): Round LED row anodes + button column strobes -> Out_PP 2 MHz
    GPIO_InitStructure.GPIO_Pin = 0xFFEF;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    // PB0..PB7, PB15 (0x80FF): Button matrix row inputs -> IPU
    GPIO_InitStructure.GPIO_Pin = 0x80FF;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // PA9, PA10, PA15 (0x8600): Out_PP 2 MHz, set HIGH
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_SetBits(GPIOA, GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_15);

    // PD2: Out_PP 2 MHz, set HIGH
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOD, &GPIO_InitStructure);
    GPIO_SetBits(GPIOD, GPIO_Pin_2);
}

void ADC_Configuration(void) {
    GPIO_InitTypeDef GPIO_InitStructure;
    DMA_InitTypeDef DMA_InitStructure;
    ADC_InitTypeDef ADC_InitStructure;

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

    // PA0..PA7: Faders 1..8 analog inputs
    GPIO_InitStructure.GPIO_Pin = 0x00FF;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    // PC4 (ADC1_IN14): Master fader 9 analog input
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    // DMA1_Channel1: circular 9 half-words from ADC1->DR (0x4001244C) to adc_raw[9]
    DMA_DeInit(DMA1_Channel1);
    DMA_InitStructure.DMA_PeripheralBaseAddr = 0x4001244CUL;
    DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)adc_raw;
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
    DMA_InitStructure.DMA_BufferSize = 9;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
    DMA_InitStructure.DMA_Priority = DMA_Priority_High;
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel1, &DMA_InitStructure);
    DMA_Cmd(DMA1_Channel1, ENABLE);

    // ADC1: scan, continuous, SWSTART, right-aligned, 9 regular channels @ 71.5 cycles
    ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;
    ADC_InitStructure.ADC_ScanConvMode = ENABLE;
    ADC_InitStructure.ADC_ContinuousConvMode = DISABLE;
    ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
    ADC_InitStructure.ADC_NbrOfChannel = 9;
    ADC_Init(ADC1, &ADC_InitStructure);

    ADC_RegularChannelConfig(ADC1, ADC_Channel_0, 1, ADC_SampleTime_71Cycles5);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_1, 2, ADC_SampleTime_71Cycles5);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_2, 3, ADC_SampleTime_71Cycles5);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_3, 4, ADC_SampleTime_71Cycles5);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_4, 5, ADC_SampleTime_71Cycles5);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_5, 6, ADC_SampleTime_71Cycles5);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_6, 7, ADC_SampleTime_71Cycles5);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_7, 8, ADC_SampleTime_71Cycles5);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_14, 9, ADC_SampleTime_71Cycles5);

    ADC_DMACmd(ADC1, ENABLE);
    ADC_Cmd(ADC1, ENABLE);

    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1) != RESET) {}
    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1) != RESET) {}

    // Enable and calibrate ADC2
    ADC_Cmd(ADC2, ENABLE);
    ADC_ResetCalibration(ADC2);
    while (ADC_GetResetCalibrationStatus(ADC2) != RESET) {}
    ADC_StartCalibration(ADC2);
    while (ADC_GetCalibrationStatus(ADC2) != RESET) {}

    DMA_ClearFlag(DMA1_FLAG_GL1);
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
}

void IWDG_Configuration(void) {
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_32);
    IWDG_SetReload(0x04E2); // 1250 -> ~1.0 s timeout
    IWDG_ReloadCounter();
    IWDG_Enable();
}

void SysTick_Configuration(void) {
    RCC_ClocksTypeDef RCC_Clocks;

    RCC_GetClocksFreq(&RCC_Clocks);
    SysTick_SetReload((RCC_Clocks.HCLK_Frequency / 8) / 2000);
    SysTick_ITConfig(ENABLE);
    SysTick_CounterCmd(SysTick_Counter_Enable);
}

void Set_USBClock(void) {
    // 48 MHz PLLCLK / 1 = 48 MHz USBCLK
    RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div1);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);
}

void Enter_LowPowerMode(void) {
    bDeviceState = SUSPENDED;
}

void Leave_LowPowerMode(void) {
    DEVICE_INFO *pInfo = &Device_Info;
    if (pInfo->Current_Configuration != 0) {
        bDeviceState = CONFIGURED;
    } else {
        bDeviceState = ATTACHED;
    }
}

void USB_Interrupts_Config(void) {
    NVIC_InitTypeDef NVIC_InitStructure;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_0);

    NVIC_InitStructure.NVIC_IRQChannel = USB_LP_CAN1_RX0_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
}

void USB_Cable_Config(FunctionalState NewState) {
    // PA8 drives D+ pull-up
    if (NewState != DISABLE) {
        GPIO_SetBits(GPIOA, GPIO_Pin_8);
    } else {
        GPIO_ResetBits(GPIOA, GPIO_Pin_8);
    }
}

void usb_midi_init(void) {
    GPIO_InitTypeDef GPIO_InitStructure;

    Set_USBClock();
    USB_Interrupts_Config();

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    USB_Init();
}

void hw_init(void) {
    RCC_Configuration();
    NVIC_SetVectorTable(NVIC_VectTab_FLASH, 0x2000);
    GPIO_Configuration();
    usb_tx_q_init();
    usb_midi_init();
    SysTick_Configuration();
}

void periph_app_init(void) {
    tm1629_clear_init();
    ADC_Configuration();
    fader_init();
    round_led_init();
}
