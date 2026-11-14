#include "hw_config.h"
#include "apc_mini.h"

int main(void) {
    hw_init();
    periph_app_init();
    IWDG_Configuration();

    while (1) {
        IWDG_ReloadCounter();
        mux_round_leds();
        scan_buttons();
        kick_adc();
        report_buttons();
        report_faders();
        usb_midi_poll_rx();
        blink_refresh();
        process_led_q();
        usb_midi_flush_tx();
    }
}
