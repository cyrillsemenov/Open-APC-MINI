# Open APC Mini Firmware

I have my APC Mini lying around which I've been using on and off for years. It's a nice control panel: it has 9 faders, sometimes I used it as a MIDI keyboard with a ridiculously wide range, I've connected it to Pure Data (PD) — I barely use it with Ableton though, don't know why. And during all this time I wanted to get inside its brains and find out what it is capable of.

One project inspired me: [craigsblackie/MPK-Mini-open](https://github.com/craigsblackie/MPK-Mini-open). The MPK uses almost the same MCU as the APC (`STM32F102RBT6`), so it was almost definitely vulnerable to power-glitch readout via [CTXz/stm32f1-picopwner](https://github.com/CTXz/stm32f1-picopwner). I connected all needed wires to an RPi Pico and dumped the firmware. Why do we need a dump? First of all, it is a save point; second, we might find something interesting in there. And boy there was — a hidden bootloader mode, activated only by a special SysEx command! Even more counterintuitive is that on factory firmware, after you send this command, you have to physically power-cycle the APC to enter Updater mode before you can flash your own firmware onto it.

The circuit of the APC Mini is remarkably simple: just an STM32F102RBT6 MCU and a TM1629 for the LEDs. So my first goal was to recreate the stock firmware and apply a couple of fixes, initially reconstructing it against the ST Standard Peripheral SDK before migrating the peripheral and USB-MIDI stack to [`libopencm3`](https://github.com/libopencm3/libopencm3).

I wanted to fix two things right away:

- **Implement soft reboot after updater handshake**, so no physical power-cycle is needed when updating (the MCU pulls the USB D+ pull-up low, re-enumerates USB, and resets into the bootloader itself).
- **Fix clunky slider behaviour** — on stock firmware it jumps multiple values and it is hard to dial in the exact value you need even within the restricted 0–127 MIDI CC range. Now faders track single-step 7-bit CC and 14-bit high-resolution (`0–16383`) values smoothly.

Since then, the firmware has grown to support simultaneous composite USB-MIDI + Monome `serialosc` (`mext` v4 with 9 faders mapped as 3 tilt banks), an on-chip Lua co-processor compiled right in the browser via Rust/WASM, SWD-less SysEx telemetry & black-box crash logging (`0x08007800`), a Unicorn Cortex-M3 hardware emulator test suite, and a 4-page WebMIDI app:

- [x] Migrate to [libopencm3/libopencm3](https://github.com/libopencm3/libopencm3)
- [x] [monome/serialosc](https://github.com/monome/serialosc) &amp; [monome/norns](https://github.com/monome/norns) compatibility (simultaneous composite USB-MIDI + USB CDC-ACM `mext` v4)
- [x] Poor man's monome grid (on-chip Lua co-processor + browser Rust/WASM compiler)
- [x] Improve visual feedback (varibright palettes, non-destructive LED overlay)

---

## Web Interface (WebMIDI)

Run `make serve-web` (or open the GitHub Pages site in Chrome, Edge, Brave, or Opera) to use the browser interface across four focused pages:

- **Uploader (`index.html`)** — Connect via WebMIDI, select a release/local `.syx` or `.bin` image, and flash over USB-MIDI SysEx with automatic soft-reboot into the bootloader.
- **Config / Debug (`config.html`)** — Toggle active modes (`MIDI`, `serialosc`, `Lua VM`, `Ableton Intro 0x60`), choose LED palette & TM1629 brightness, run LED test sweeps, inspect live telemetry & post-mortem crash logs (`0x08007800`), and peek arbitrary 32-bit MCU memory.
- **Script (`script.html`)** — Edit, live-compile (via `apc_lua_compiler.wasm`), and upload Lua scripts to RAM or Flash (`0x08008000`), toggle the Lua VM, or browse the built-in scripting reference.
- **About (`about.html`)** — Background story, feature summary, roadmap, and repository links.

### Troubleshooting & Fallback

If something goes wrong, you can flash **`v0.2.1`** from the **Uploader** page (or GitHub Releases) — it is just the fixed USB-MIDI firmware (soft reboot + smooth faders) without `serialosc` or the Lua VM. Please also grab your telemetry and crash log from **Config / Debug** (_Read Telemetry & Crash Log_ → _Copy Markdown Report_, or `make debug` / `uv run fw_debug` from the CLI) and [open an issue on GitHub](https://github.com/cyrillsemenov/Open-APC-MINI/issues).

---

## Scripting Guide (On-Chip Lua)

The firmware embeds a compact integer stack VM (`LUA1` bytecode, max `4,096` bytes) running directly on the STM32F102. Write scripts in the browser **Script** page (compiled client-side by Rust + `full_moon` WASM) or from the terminal with `uv run lua_compiler scripts/bouncing_balls.lua --save`.

### Minimal Example

```lua
function init()
    pos = 0
    set_tempo(120)
end

function on_grid(x, y, z)
    if z == 1 then
        note_on(0, 48 + x + (7 - y) * 5, 100)
        led(x, y, 15)
    else
        note_off(0, 48 + x + (7 - y) * 5)
        led(x, y, 0)
    end
    return 1 -- consume press (return 0 to pass through)
end

function on_fader(idx, val, hires)
    if idx == 8 then
        set_tempo(30 + ((16383 - hires) >> 7))
    end
end

function on_tick(step)
    led(pos, 8, 0)
    pos = (pos + 1) & 7
    led(pos, 8, 1)
end
```

### Callbacks

Scripts define up to five top-level callback functions (custom helper functions are not supported):

| Callback                    | Description                                                                                                                                                                 |
| :-------------------------- | :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `init()`                    | Runs once on upload, reset, or VM enable (top-level statements also act as init if `init()` is omitted).                                                                    |
| `on_tick(step)`             | Cooperative coroutine called every tempo tick (`step` increments `0, 1, 2...`). Alias: `tick`.                                                                              |
| `on_grid(x, y, z)`          | Pad press (`z = 1`) / release (`z = 0`), `x, y = 0..7`. Return `1` to consume the event, or `0` to pass through to MIDI / `serialosc`. Aliases: `g.key`, `grid_key`.        |
| `on_fader(idx, val, hires)` | Fader moved: `idx = 0..8` (`8` = Master), `val = 0..127` (7-bit), `hires = 0..16383` (14-bit). Alias: `fader_move`.                                                         |
| `on_button(group, idx, z)`  | Round & Shift buttons: `group = 1` Track 1..8 (`idx = 0..7`), `group = 2` Scene 1..8 (`idx = 0..7`), `group = 3` SHIFT (`idx = 0`). Return `1` to consume. Alias: `button`. |

### Coordinates & LEDs

- **Grid Coordinates:** `x = 0..7` (left to right), `y = 0..7` (top to bottom — Monome convention).
- **Non-Destructive Overlay:** Grid LEDs sit on an overlay above Ableton / `serialosc`. Calling `led(x, y, 0)` clears the overlay and restores whatever host LED is underneath.
- **Varibright Levels:** `1..15` maps through the active palette (Default Palette 0: `1..5` Green, `6..10` Yellow, `11..15` Red; `16` = 1-bit ON).
- **Round LEDs:** `led(x, 8, state)` controls Track LEDs 1..8 and `led(8, y, state)` controls Scene LEDs 1..8 (`0` = off, `1` = solid, `2` = blink).

### Built-in Functions

| Function                           | Description                                                                                                                |
| :--------------------------------- | :------------------------------------------------------------------------------------------------------------------------- |
| `led(x, y, val)`                   | Set grid pad overlay (`0..16`) or round LED (`y=8` Track, `x=8` Scene). Aliases: `g:led`, `g.led`.                         |
| `led_all(val)`                     | Fill all 64 pad overlays with `val` (`0..16`). Aliases: `g:all`, `g.all`.                                                  |
| `host_led(x, y)`                   | Read host LED level (`0..16`) underneath from `serialosc` or MIDI.                                                         |
| `refresh()`                        | No-op accepted for Monome grid script compatibility (`g:refresh`, `g.refresh`).                                            |
| `note_on(ch, note, vel)`           | Send USB-MIDI Note On (`ch 0..15`, `note/vel 0..127`); also emits `serialosc` key if `note < 64`.                          |
| `note_off(ch, note)`               | Send USB-MIDI Note Off (and `serialosc` key-up if `note < 64`).                                                            |
| `cc(ch, num, val)`                 | Send 7-bit USB-MIDI Control Change (`0..127`).                                                                             |
| `cc14(ch, cc, val14)`              | Send 14-bit MSB+LSB MIDI CC pair (`cc = 0..31`, `val14 = 0..16383`).                                                       |
| `pitch_bend(ch, val14)`            | Send 14-bit USB-MIDI Pitch Bend (`0..16383`).                                                                              |
| `grid_key(x, y, z)`                | Emit synthetic key down (`z=1`) / up (`z=0`) to MIDI and `serialosc`. Aliases: `trigger_key`, `mext_key`.                  |
| `key_pulse(x, y, ms)`              | Non-blocking key down + automatic key up after `ms` (up to `4000`, 8 concurrent gates).                                    |
| `tilt(sensor, x, y, z)`            | Send Monome `mext` tilt packet (`sensor = 0..3`). Alias: `mext_tilt`.                                                      |
| `fader(idx)` / `fader_hires(idx)`  | Read fader `0..8` as 7-bit (`0..127`) or 14-bit (`0..16383`; alias `fader14`).                                             |
| `shift()`                          | Returns `1` while SHIFT is held, else `0`.                                                                                 |
| `set_tempo(ms)`                    | Set `on_tick` period in ms (clamped `10..2000`, default `55`).                                                             |
| `yield()` / `sleep_ms(ms)`         | Suspend `on_tick` coroutine until next loop turn or for `ms` (up to `4000`).                                               |
| `rand(max)`                        | Pseudo-random integer in `0 .. max - 1`. Alias: `math.random`.                                                             |
| `abs(x)`, `min(a, b)`, `max(a, b)` | Integer math helpers (also under `math.*`).                                                                                |
| `life_step()`                      | Fast C-accelerated 8×8 toroidal Game of Life step from `mem[0..63]` + `host_led` into `mem[64..127]`; returns alive count. |

### Language Subset & VM Limits

- **Integer-Only VM:** Locals and stack values are signed 32-bit integers; globals and shared memory `mem[0..511]` store signed 16-bit integers. 2D syntax `grid[y][x]` maps automatically to `mem[y * 8 + x]`.
- **Supported Constructs:** `if/elseif/else`, `while`, numeric `for` (constant step), `do..end`, `return`, arithmetic (`+ - * / // %`), bitwise (`& | ~ << >>`), and comparisons (`== ~= < <= > >=`). Note that `and` / `or` evaluate both sides eagerly.
- **Not Supported:** Floating-point math, strings, dynamic tables/closures, `break`, `repeat..until`, and custom user helper functions.
- **Resource Limits:** Max 64 globals, 64 locals per callback, `4,096` B bytecode image. `on_tick` yields cooperatively every 256 opcodes (up to `131,072` ops/tick) so USB-MIDI and LED scanning never stall.

### Bundled Scripts (`scripts/`)

- `scripts/bouncy_echo.lua` — Built-in ROM varibright diamond ripple & echo (Fader 8 = echoes, Master = tempo).
- `scripts/bouncing_balls.lua` — Polyrhythmic bouncing ball sequencer reacting to host LEDs (Master = 14-bit tempo).
- `scripts/game_of_life.lua` — Conway's Game of Life generative sequencer using `life_step()` and `key_pulse()`.
- `scripts/ripple_sequencer.lua` — 2D wave-physics & scale sequencer using Track/Scene buttons (`on_button`) and per-pad fader locks.

---

## Building, Flashing & Tools

### Prerequisites

- `arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, `arm-none-eabi-size`
- [`uv`](https://docs.astral.sh/uv/) (`uv sync` installs `unicorn`, `mido`, `python-rtmidi`, `luaparser`)
- Optional (for rebuilding the browser Lua compiler): Rust with `wasm32-unknown-unknown` target

### Commands

```sh
# Build .bin and bootloader MIDI SysEx .syx (default BOOTLOADER=1 @ 0x08002000)
make all

# Optional: build a standalone SWD image at 0x08000000 (without factory bootloader)
make BOOTLOADER=0 all

# Flash directly via command-line
make flash

# Or stage and run the 4-page WebMIDI app at http://localhost:8000
make serve-web

# Rebuild the Rust/WASM Lua compiler (falls back to prebuilt web/apc_lua_compiler.wasm if absent)
make lua-wasm

# Compile & upload a Lua script over USB-MIDI / serialosc (pass --save to persist in Flash)
uv run lua_compiler scripts/bouncing_balls.lua --save

# Inspect live telemetry, crash log (0x08007800), or memory from the CLI
make debug

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
