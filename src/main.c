#include <libopencm3/stm32/iwdg.h>
#include "apc_mode.h"
#include "board.h"
#include "buttons.h"
#include "faders.h"
#include "leds.h"
#include "usb_midi.h"

int main(void) {
    board_init();
    usb_midi_init();
    apc_mode_init();
    leds_init();
    buttons_init();
    faders_init();
    board_watchdog_init();

    while (1) {
        iwdg_reset();

        uint32_t now = board_ticks();
        buttons_task(now, apc_mode_on_button);
        faders_task(apc_mode_on_fader);
        apc_mode_poll_rx(now);
        leds_task(now);
        usb_midi_flush_tx();
    }
}
