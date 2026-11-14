#pragma once

// EP_NUM: defines how many endpoints are used by the device
#define EP_NUM (2)

// buffer table base address
#define BTABLE_ADDRESS (0x00)

// EP0 Rx/Tx buffer base addresses
#define ENDP0_RXADDR (0x40)
#define ENDP0_TXADDR (0x80)

// EP1 Tx/Rx buffer base addresses
#define ENDP1_TXADDR (0xC0)
#define ENDP1_RXADDR (0x100)

// IMR_MSK: mask defining which events will be handled
#define IMR_MSK                                                                                    \
    (CNTR_CTRM | CNTR_WKUPM | CNTR_SUSPM | CNTR_ERRM | CNTR_SOFM | CNTR_ESOFM | CNTR_RESETM)

// #define CTR_CALLBACK
// #define DOVR_CALLBACK
// #define ERR_CALLBACK
// #define WKUP_CALLBACK
// #define SUSP_CALLBACK
// #define RESET_CALLBACK
#define SOF_CALLBACK
// #define ESOF_CALLBACK

// CTR service routines associated to defined endpoints
// EP1_IN_Callback and EP1_OUT_Callback are defined
#define EP2_IN_Callback NOP_Process
#define EP3_IN_Callback NOP_Process
#define EP4_IN_Callback NOP_Process
#define EP5_IN_Callback NOP_Process
#define EP6_IN_Callback NOP_Process
#define EP7_IN_Callback NOP_Process

#define EP2_OUT_Callback NOP_Process
#define EP3_OUT_Callback NOP_Process
#define EP4_OUT_Callback NOP_Process
#define EP5_OUT_Callback NOP_Process
#define EP6_OUT_Callback NOP_Process
#define EP7_OUT_Callback NOP_Process
