# Open APC Mini Firmware

I have my APC Mini lying around which I've been using on and off for years. It's a nice control panel: it has 9 faders, sometimes I used it as a MIDI keyboard with a ridiculously wide range, I've connected it to Pure Data (PD) — I barely use it with Ableton though, don't know why. And during all this time I wanted to get inside its brains and find out what it is capable of.

One project inspired me: [craigsblackie/MPK-Mini-open](https://github.com/craigsblackie/MPK-Mini-open). The MPK uses almost the same MCU as the APC (`STM32F102RBT6`), so it was almost definitely vulnerable to power-glitch readout via [CTXz/stm32f1-picopwner](https://github.com/CTXz/stm32f1-picopwner). I connected all needed wires to an RPi Pico and dumped the firmware. Why do we need a dump? First of all, it is a save point; second, we might find something interesting in there. And boy there was — a hidden bootloader mode, activated only by a special SysEx command! Even more counterintuitive is that on stock firmware, after you send this command, you have to physically power-cycle the APC to enter Updater mode before you can flash your own firmware onto it.

The circuit of the APC Mini is remarkably simple: just an STM32F102RBT6 MCU and a TM1629 for the LEDs. So my first goal was to recreate the stock firmware and apply a couple of fixes, initially reconstructing it against the ST Standard Peripheral SDK before migrating the peripheral and USB-MIDI stack to [`libopencm3`](https://github.com/libopencm3/libopencm3).

I wanted to fix two things right away:

- **Implement soft reboot after updater handshake**, so no physical power-cycle is needed when updating (the MCU pulls the USB D+ pull-up low, re-enumerates USB, and resets into the bootloader itself).
- **Fix clunky slider behaviour** — on stock firmware it jumps multiple values and it is hard to dial in the exact value you need even within the restricted 0–127 MIDI CC range.

As a result, we now have a nice working firmware, a **WebMIDI Firmware Updater**, a Unicorn Cortex-M3 hardware emulator test suite, and a lot of plans:

- [x] Migrate to [libopencm3/libopencm3](https://github.com/libopencm3/libopencm3)
- [ ] Implement Mackie Control Universal (MCU)
- [ ] [monome/serialosc](https://github.com/monome/serialosc) compatibility
- [ ] Poor man's monome grid (run Lua scripts inside your APC)
- [ ] Improve visual feedback

---

## Building, Flashing & Web Updater

### Prerequisites

- `arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, `arm-none-eabi-size`
- [`uv`](https://docs.astral.sh/uv/) (`uv sync` installs `unicorn`, `mido`, `python-rtmidi`)

### Commands

```sh
# Build .bin and bootloader MIDI SysEx .syx (default BOOTLOADER=1 @ 0x08002000)
make all

# Optional: build a standalone SWD image at 0x08000000 (without factory bootloader)
make BOOTLOADER=0 all

# Flash directly via command-line
make flash

# Or stage and run the browser WebMIDI updater at http://localhost:8000
make serve-web

# Emulate STM32F102RB hardware in Unicorn and validate firmware behavior
make verify

# Compile fault-injected broken firmwares and verify the validator catches all of them
make test-validator
```

---

## Important Notice for Non-Akai / Custom Hardware

The default USB `VID:PID` (`0x09E8:0x0028`), manufacturer string (`"AKAI PROFESSIONAL,LP"`), and product string (`"APC MINI"`) are configured **strictly for interoperability on genuine retail Akai APC Mini hardware** (which already carries this USB identity in its factory bootloader and hardware).

If you build, adapt, or port this firmware for **any custom PCB, clone, or non-Akai hardware target**, you **must change the USB VID/PID and vendor strings** — do not use Akai's USB Vendor/Product IDs (`0x09E8:0x0028`) or corporate vendor name. Instead, use:

1. **[pid.codes](https://pid.codes) (`VID 0x1209`):** Request a free, dedicated Product ID for your open-source hardware project.
2. **Objective Development Shared USB-MIDI ID (`VID 0x16C0`, `PID 0x05E4`):**
   ```sh
   make USB_VID=0x16C0 USB_PID=0x05E4 \
        USB_STRING_VENDOR="your-domain.org" \
        USB_STRING_PRODUCT="Custom Grid Controller"
   ```

---

## License

Released under the [MIT License](LICENSE) (linking against [`libopencm3`](https://github.com/libopencm3/libopencm3) under `LGPL-3.0-or-later`; see third-party notices in [LICENSE](LICENSE)).
