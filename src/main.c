#include <libopencm3/stm32/iwdg.h>
#include "apc_mode.h"
#include "board.h"
#include "bootloader.h"
#include "buttons.h"
#include "faders.h"
#include "leds.h"
#include "monome_mode.h"
#include "usb_midi.h"

static uint32_t s_loop_now = 0;

static void app_on_button(apc_btn_group_t group, uint8_t index, bool pressed) {
    if (buttons_shift_pressed() && group == APC_BTN_GROUP_SCENE && pressed) {
        if (index == 0U && usb_get_profile() != USB_PROFILE_APC_MIDI) {
            leds_all_off(s_loop_now);
            leds_set_brightness(7U, s_loop_now);
            apc_mode_init();
            usb_switch_profile(USB_PROFILE_APC_MIDI);
            return;
        }
        if (index == 1U && usb_get_profile() != USB_PROFILE_MONOME_FTDI) {
            monome_mode_init(s_loop_now);
            usb_switch_profile(USB_PROFILE_MONOME_FTDI);
            return;
        }
        if (index == 7U) {
            bootloader_enter_dfu();
            return;
        }
    }

    if (usb_get_profile() == USB_PROFILE_MONOME_FTDI) {
        monome_mode_on_button(group, index, pressed, s_loop_now);
    } else {
        apc_mode_on_button(group, index, pressed);
    }
}

static void app_on_fader(uint8_t fader_idx, uint8_t value) {
    if (usb_get_profile() == USB_PROFILE_MONOME_FTDI) {
        monome_mode_on_fader(fader_idx, value);
    } else {
        apc_mode_on_fader(fader_idx, value);
    }
}

int main(void) {
    board_init();
    leds_init();
    buttons_init();

    bool boot_monome = false;
    bool boot_dfu = false;
    buttons_scan_boot_chord(&boot_monome, &boot_dfu);
    if (boot_dfu) {
        bootloader_enter_dfu();
    }
    if (boot_monome) {
        usb_select_boot_profile(USB_PROFILE_MONOME_FTDI);
    }

    usb_midi_init();
    apc_mode_init();
    if (boot_monome) {
        monome_mode_init(board_ticks());
    }
    faders_init();
    board_watchdog_init();

    while (1) {
        iwdg_reset();

        uint32_t now = board_ticks();
        s_loop_now = now;
        buttons_task(now, app_on_button);
        faders_task(app_on_fader);
        if (usb_get_profile() == USB_PROFILE_MONOME_FTDI) {
            monome_mode_poll_rx(now);
        } else {
            apc_mode_poll_rx(now);
        }
        leds_task(now);
        usb_midi_flush_tx();
        board_record_loop_turn(board_ticks() - now);
    }
}
