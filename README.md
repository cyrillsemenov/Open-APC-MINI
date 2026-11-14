# Akai APC Mini Firmware (`v0.1.0` / Stock `Ver00.10` Reconstruction)

Faithful, pure-C11 reconstruction of the factory **Akai Professional APC Mini (`Ver00.10`)** firmware for the **STM32F102RBT6** (Cortex-M3, 48 MHz, 128 KB Flash, 16 KB SRAM), built against the exact ST peripheral libraries used in the factory binary:

- **ST `STM32F10x_StdPeriph_Driver` (V3.5.0)** + CMSIS (`lib/STM32F10x_StdPeriph_Lib`)
- **ST `STM32_USB-FS-Device_Driver` (V4.1.0)** (`lib/STM32_USB-FS-Device_Driver`)

> **Note:** An alternative standalone implementation built on `libopencm3` is preserved on the `libopencm3` branch.

---

## Flash Memory Map & Bootloader Integration

| Address Range | Offset in Dump | Size | Region | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0x08000000..0x08001FFF` | `0x00000..0x01FFF` | 8 KB (`8,192` B) | **APC MINI Updater** | Factory USB-MIDI SysEx bootloader (`PID 0x0028`) |
| `0x08002000..0x080077FD` | `0x02000..0x077FD` | `22,526` B | **Application Code & Data** | Main APC Mini firmware (`Ver00.10`, vector table at `0x08002000`) |
| `0x080077FE..0x080077FF` | `0x077FE..0x077FF` | 2 B | **App Checksum Word** | 16-bit little-endian two's-complement checksum (`0xAB4B`, bytes `4B AB`) |
| `0x08007800..0x08007BFF` | `0x07800..0x07BFF` | 1 KB (`1,024` B) | **Reserved / Erased** | Erased Flash (`0xFF`) |
| `0x08007C00..0x08007FFF` | `0x07C00..0x07FFF` | 1 KB (`1,024` B) | **Updater Magic Page** | Bootloader magic (`55 5A A5 AA 59 95 3A A3`); erased (`0xFF`) when entering DFU |
| `0x08008000..0x0801FFFF` | `0x08000..0x1FFFF` | 96 KB (`98,304` B) | **Unused Flash** | Erased Flash (`0xFF`) |

---

## Directory Structure

```text
├── Makefile                     # Pure-C build + checksum stamping + verification
├── include/
│   ├── apc_mini.h               # Exact stock data structures, queues, and prototypes
│   ├── core_cm3.h               # Wrapper for CMSIS CoreSupport header in submodule
│   ├── hw_config.h              # Clock, GPIO, NVIC, ADC+DMA, IWDG, and SysTick setup
│   ├── stm32f10x_conf.h         # StdPeriph driver configuration
│   ├── usb_conf.h               # USB-FS-Device PMA layout and endpoint configuration
│   ├── usb_desc.h               # Exact 18B Device, 101B Config, and String descriptors
│   ├── usb_istr.h               # USB interrupt dispatcher declarations
│   ├── usb_prop.h               # USB standard/class request hooks
│   └── usb_pwr.h                # USB power state machine
├── ld/
│   └── stm32f102rb_app.ld       # Linker script for application @ 0x08002000
├── lib/
│   ├── STM32F10x_StdPeriph_Lib/ # Git submodule: ST StdPeriph V3.5.0 + CMSIS
│   └── STM32_USB-FS-Device_Driver/ # Git submodule: ST USB-FS-Device V4.1.0
├── src/
│   ├── startup_stm32f10x_md.c   # Cortex-M3 vector table & Reset_Handler
│   ├── stm32f10x_it.c           # SysTick (0.5 ms), DMA1_Channel1, USB_LP_CAN1_RX0 ISRs
│   ├── hw_config.c              # SystemInit (48 MHz PLL), GPIO, ADC1+ADC2, IWDG
│   ├── usb_desc.c               # Byte-identical USB Audio/MIDI descriptors & strings
│   ├── usb_prop.c               # USB property table & endpoint init (EP0/EP1 OUT/EP1 IN)
│   ├── usb_pwr.c                # PowerOn / Suspend / Resume state machine
│   ├── usb_istr.c               # USB_Istr interrupt handler
│   ├── usb_endp.c               # EP1_OUT_Callback & EP1_IN_Callback
│   ├── leds_tm1629.c            # TM1629 driver (PB12 STB, PB13 CLK, PB14 DIO), 128-entry queue, blink
│   ├── buttons_faders.c         # 9x2 button matrix scan (20 ticks) + 9-fader ADC filter & hysteresis
│   ├── usb_midi_io.c            # Dual 256B TX queues, 512B RX ring, MIDI/SysEx parser, DFU unlock
│   └── main.c                   # Stock main loop & startup sequence
└── tools/
    └── firmware_tool.c          # Pure-C11 host tool for checksum stamping, combining & verification
```

---

## Building & Verifying

### Prerequisites
- `arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, `arm-none-eabi-size`
- Host C11 compiler (`cc` / `clang` / `gcc`)

### Commands
```sh
# Build application binary, combined 30 KB image, and full 128 KB SWD image:
make all

# Run side-by-side verification against reverse_engineering/AkaiAPC_1.bin:
make verify
```

### USB Identity & Legal Disclaimer
By default, `USB_STRING_SERIAL` (used by the stock firmware for the version string) is set automatically from the exact Git tag on release builds (`git describe --tags --exact-match`), falling back to the short commit hash (`git rev-parse --short HEAD`) or `"dev"` when building from an uncommitted tree.

The default USB `VID:PID` (`0x09E8:0x0028`), manufacturer string (`"AKAI PROFESSIONAL,LP"`), and product string (`"APC MINI"`) are configured **strictly for interoperability on genuine retail Akai APC Mini hardware** (which already carries this USB identity in its factory bootloader and hardware).

> **Disclaimer for Custom / Non-Akai Hardware Targets:**  
> This project is an independent open-source firmware reconstruction and is not affiliated with, endorsed by, or sponsored by Akai Professional / inMusic Brands. If you build, adapt, or port this firmware for **any custom PCB, clone, or non-Akai hardware target**, you **must not** use Akai's USB Vendor/Product IDs (`0x09E8:0x0028`) or corporate vendor name (`"AKAI PROFESSIONAL,LP"`).
>
> Instead, use one of the following options when building for custom hardware:
> 1. **[pid.codes](https://pid.codes) (`VID 0x1209`):** Request a free, dedicated Product ID for your open-source hardware project.
> 2. **Objective Development Shared USB-MIDI ID (`VID 0x16C0`, `PID 0x05E4`):** Available for open-source/hobbyist USB-MIDI class devices (requires setting `USB_STRING_VENDOR` to your own domain name or e-mail address per Objective Development's license rules):
>    ```sh
>    make USB_VID=0x16C0 USB_PID=0x05E4 \
>         USB_STRING_VENDOR="your-domain.org" \
>         USB_STRING_PRODUCT="Custom Grid Controller"
>    ```

### Build Outputs
- `build/apc_mini_app.elf` — Linked ELF at `0x08002000` (`12,520` B `.text`, `196` B `.data`, `4,248` B `.bss`).
- `build/apc_mini_app.bin` — Stamped `22,528`-byte application image (`0x08002000..0x080077FF`) ready for the `"APC MINI Updater"` bootloader, with checksum `0xAB4B` at `+0x57FE` (`0x080077FE`).
- `build/apc_mini_full_30k.bin` — Combined `30,720`-byte image (`0x08000000..0x080077FF`, trailing `0xFF` stripped) containing the 8 KB `"APC MINI Updater"` bootloader + 22.5 KB stamped application.
- `build/apc_mini_full.bin` — Full `131,072`-byte (128 KB) SWD Flash image (`0x08000000..0x0801FFFF`) matching the layout, size, and checksums of `reverse_engineering/AkaiAPC_1.bin`.
