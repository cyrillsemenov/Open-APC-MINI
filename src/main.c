#include <libopencm3/stm32/iwdg.h>
#include "apc_mode.h"
#include "board.h"
#include "bootloader.h"
#include "buttons.h"
#include "faders.h"
#include "leds.h"
#include "lua_vm.h"
#include "monome_mode.h"
#include "usb_midi.h"

static uint32_t s_loop_now = 0;

static void app_on_button(apc_btn_group_t group, uint8_t index, bool pressed) {
    if (lua_vm_on_button(group, index, pressed, s_loop_now)) {
        return;
    }

    uint8_t flags = apc_mode_get_flags();
    if ((flags & APC_MODE_FLAG_SERIALOSC) != 0U) {
        monome_mode_on_button(group, index, pressed, s_loop_now);
    }
    if ((flags & APC_MODE_FLAG_MIDI) != 0U) {
        apc_mode_on_button(group, index, pressed);
    }
}

static void app_on_fader(uint8_t fader_idx, uint8_t cc_val, uint16_t hires_val, bool cc_changed) {
    lua_vm_on_fader(fader_idx, cc_val, hires_val, s_loop_now);
    uint8_t flags = apc_mode_get_flags();
    if ((flags & APC_MODE_FLAG_SERIALOSC) != 0U) {
        monome_mode_on_fader(fader_idx, hires_val);
    }
    if (cc_changed && (flags & APC_MODE_FLAG_MIDI) != 0U) {
        apc_mode_on_fader(fader_idx, cc_val);
    }
}

int main(void) {
    board_init();
    leds_init();
    buttons_init();

    (void)board_consume_post_dfu_monome();

    usb_midi_init();
    apc_mode_init();
    monome_mode_init(board_ticks());
    faders_init();
    lua_vm_init(board_ticks());
    board_watchdog_init();

    while (1) {
        iwdg_reset();

        uint32_t now = board_ticks();
        s_loop_now = now;
        buttons_task(now, app_on_button);
        faders_task(app_on_fader);
        monome_mode_poll_rx(now);
        apc_mode_poll_rx(now);
        lua_vm_task(board_ticks());
        leds_task(board_ticks());
        usb_midi_flush_tx();
        board_record_loop_turn(board_ticks() - now);
    }
}
