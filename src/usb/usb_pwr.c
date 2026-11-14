#include "stm32f10x.h"
#include "usb_lib.h"
#include "usb_conf.h"
#include "usb_pwr.h"
#include "hw_config.h"

__IO uint32_t bDeviceState = UNCONNECTED;
__IO bool fSuspendEnabled = TRUE;

struct {
    __IO RESUME_STATE eState;
    __IO uint8_t bESOFcnt;
} ResumeS;

RESULT PowerOn(void) {
    uint16_t wRegVal;

    USB_Cable_Config(ENABLE);

    wRegVal = CNTR_FRES;
    _SetCNTR(wRegVal);

    wInterrupt_Mask = 0;
    _SetCNTR(wInterrupt_Mask);
    _SetISTR(0);

    wInterrupt_Mask = CNTR_RESETM | CNTR_SUSPM | CNTR_WKUPM;
    _SetCNTR(wInterrupt_Mask);

    return USB_SUCCESS;
}

RESULT PowerOff(void) {
    _SetCNTR(CNTR_FRES);
    _SetISTR(0);
    USB_Cable_Config(DISABLE);
    _SetCNTR(CNTR_FRES + CNTR_PDWN);

    return USB_SUCCESS;
}

void Suspend(void) {
    uint16_t wCNTR;

    wCNTR = _GetCNTR();
    wCNTR |= CNTR_FSUSP;
    _SetCNTR(wCNTR);

    wCNTR = _GetCNTR();
    wCNTR |= CNTR_LPMODE;
    _SetCNTR(wCNTR);

    Enter_LowPowerMode();
}

void Resume_Init(void) {
    uint16_t wCNTR;

    wCNTR = _GetCNTR();
    wCNTR &= (~CNTR_LPMODE);
    _SetCNTR(wCNTR);

    Leave_LowPowerMode();
    _SetCNTR(IMR_MSK);
}

void Resume(RESUME_STATE eResumeSetVal) {
    uint16_t wCNTR;

    if (eResumeSetVal != RESUME_ESOF) {
        ResumeS.eState = eResumeSetVal;
    }

    switch (ResumeS.eState) {
    case RESUME_EXTERNAL:
        Resume_Init();
        ResumeS.eState = RESUME_OFF;
        break;
    case RESUME_INTERNAL:
        Resume_Init();
        ResumeS.eState = RESUME_START;
        break;
    case RESUME_LATER:
        ResumeS.bESOFcnt = 2;
        ResumeS.eState = RESUME_WAIT;
        break;
    case RESUME_WAIT:
        ResumeS.bESOFcnt--;
        if (ResumeS.bESOFcnt == 0) {
            ResumeS.eState = RESUME_START;
        }
        break;
    case RESUME_START:
        wCNTR = _GetCNTR();
        wCNTR |= CNTR_RESUME;
        _SetCNTR(wCNTR);
        ResumeS.eState = RESUME_ON;
        ResumeS.bESOFcnt = 10;
        break;
    case RESUME_ON:
        ResumeS.bESOFcnt--;
        if (ResumeS.bESOFcnt == 0) {
            wCNTR = _GetCNTR();
            wCNTR &= (~CNTR_RESUME);
            _SetCNTR(wCNTR);
            ResumeS.eState = RESUME_OFF;
        }
        break;
    case RESUME_OFF:
    case RESUME_ESOF:
    default:
        ResumeS.eState = RESUME_OFF;
        break;
    }
}
