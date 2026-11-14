"""Compile broken ARM Cortex-M3 firmware variants and verify validate_fw catches every fault."""

import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from tools.bin_to_sysex import (
    decode_syx_to_bin,
    decode_syx_to_steps,
    encode_firmware_steps,
    encode_syx_stream,
)
from tools.fw_updater import load_firmware_steps
from tools.stamp_fw import stamp_image
from tools.validate_fw import ValidationError, validate_firmware

BUILD_DIR = Path("build")
BROKEN_DIR = BUILD_DIR / "broken"
GOOD_BIN = BUILD_DIR / "apc_mini_app.bin"
GOOD_SYX = BUILD_DIR / "apc_mini_app.syx"


@dataclass(frozen=True)
class BrokenCase:
    name: str
    wrap_symbol: str
    c_stub: str
    expected_err: str


BROKEN_CASES: list[BrokenCase] = [
    BrokenCase(
        name="missing_vtor_reloc",
        wrap_symbol="NVIC_SetVectorTable",
        c_stub="void __wrap_NVIC_SetVectorTable(unsigned a, unsigned b) { (void)a; (void)b; }",
        expected_err="SCB->VTOR not relocated",
    ),
    BrokenCase(
        name="wrong_sysclk_no_pll",
        wrap_symbol="RCC_SYSCLKConfig",
        c_stub="void __wrap_RCC_SYSCLKConfig(unsigned s) { (void)s; }",
        expected_err="SYSCLK not switched to PLL",
    ),
    BrokenCase(
        name="no_usb_pullup_pa8",
        wrap_symbol="PowerOn",
        c_stub="int __wrap_PowerOn(void) { return 0; }",
        expected_err="PA8 (USB D+ pull-up) not driven HIGH",
    ),
    BrokenCase(
        name="watchdog_not_reloaded",
        wrap_symbol="IWDG_ReloadCounter",
        c_stub=(
            "extern void __real_IWDG_ReloadCounter(void);\n"
            "void __wrap_IWDG_ReloadCounter(void) {\n"
            "    static int once = 0;\n"
            "    if (!once++) __real_IWDG_ReloadCounter();\n"
            "}\n"
        ),
        expected_err="IWDG watchdog is not reloaded in main loop",
    ),
    BrokenCase(
        name="cpu_fault_on_init",
        wrap_symbol="periph_app_init",
        c_stub="extern void HardFault_Handler(void); void __wrap_periph_app_init(void) { HardFault_Handler(); }",
        expected_err="CPU entered fault handler",
    ),
    BrokenCase(
        name="infinite_hang_on_boot",
        wrap_symbol="periph_app_init",
        c_stub="void __wrap_periph_app_init(void) { while (1) {} }",
        expected_err="IWDG watchdog was not started",
    ),
    BrokenCase(
        name="broken_midi_rx_sysex",
        wrap_symbol="usb_midi_poll_rx",
        c_stub="void __wrap_usb_midi_poll_rx(void) {}",
        expected_err="Bad SysEx Identity Reply",
    ),
    BrokenCase(
        name="broken_button_scan",
        wrap_symbol="scan_buttons",
        c_stub="void __wrap_scan_buttons(void) {}",
        expected_err="Missing Pad Note-On",
    ),
    BrokenCase(
        name="broken_fader_adc",
        wrap_symbol="report_faders",
        c_stub="void __wrap_report_faders(void) {}",
        expected_err="Expected Fader 1 CC 0x30=127",
    ),
    BrokenCase(
        name="broken_tm1629_leds",
        wrap_symbol="process_led_q",
        c_stub="void __wrap_process_led_q(void) {}",
        expected_err="TM1629 missing 0x40 data command",
    ),
    BrokenCase(
        name="broken_dfu_unlock",
        wrap_symbol="handle_updater_unlock_and_reset",
        c_stub="void __wrap_handle_updater_unlock_and_reset(void) {}",
        expected_err="Bootloader magic not programmed at 0x08007C00",
    ),
]


def build_broken_variant(
    case: BrokenCase, objs: list[str], cc: str, objcopy: str
) -> Path:
    BROKEN_DIR.mkdir(parents=True, exist_ok=True)
    stub_c = BROKEN_DIR / f"{case.name}.c"
    stub_o = BROKEN_DIR / f"{case.name}.o"
    elf_path = BROKEN_DIR / f"{case.name}.elf"
    raw_path = BROKEN_DIR / f"{case.name}_raw.bin"
    bin_path = BROKEN_DIR / f"{case.name}.bin"

    stub_c.write_text(case.c_stub + "\n", encoding="utf-8")
    subprocess.run(
        [
            cc,
            "-mcpu=cortex-m3",
            "-mthumb",
            "-Os",
            "-std=c11",
            "-c",
            str(stub_c),
            "-o",
            str(stub_o),
        ],
        check=True,
    )
    subprocess.run(
        [
            cc,
            *objs,
            str(stub_o),
            "-mcpu=cortex-m3",
            "-mthumb",
            "-Tld/stm32f102rb_app.ld",
            "-nostartfiles",
            "-Wl,--gc-sections",
            f"-Wl,--wrap={case.wrap_symbol}",
            "--specs=nano.specs",
            "--specs=nosys.specs",
            "-o",
            str(elf_path),
        ],
        check=True,
    )
    subprocess.run([objcopy, "-O", "binary", str(elf_path), str(raw_path)], check=True)
    bin_path.write_bytes(stamp_image(raw_path.read_bytes()))
    return bin_path


def main() -> None:
    cross = os.environ.get("CROSS_COMPILE", "arm-none-eabi-")
    cc = f"{cross}gcc"
    objcopy = f"{cross}objcopy"

    if not GOOD_BIN.is_file() or not GOOD_SYX.is_file():
        subprocess.run(["make", "all"], check=True)

    objs = (
        sorted(str(p) for p in BUILD_DIR.glob("app/**/*.o"))
        + sorted(str(p) for p in BUILD_DIR.glob("stdperiph/*.o"))
        + sorted(str(p) for p in BUILD_DIR.glob("usb/*.o"))
    )
    if not objs:
        print(
            "error: missing object files in build/; run 'make all' first",
            file=sys.stderr,
        )
        sys.exit(1)

    print(
        "Testing validator & bin_to_sysex against valid & fault-injected ARM firmwares:"
    )

    # 0. Good firmware & .syx round-trip must pass
    validate_firmware(GOOD_BIN, verbose=False)
    print(f"  [PASS] valid_firmware             -> accepted ({GOOD_BIN})")

    good_bytes = GOOD_BIN.read_bytes()
    syx_bytes = GOOD_SYX.read_bytes()
    assert syx_bytes == encode_syx_stream(good_bytes), (
        "Makefile .syx differs from encode_syx_stream()"
    )
    assert decode_syx_to_bin(syx_bytes) == good_bytes, (
        ".syx -> .bin round-trip mismatch"
    )
    assert (
        decode_syx_to_bin(encode_syx_stream(good_bytes, include_unlock=True))
        == good_bytes
    )
    assert (
        load_firmware_steps(GOOD_BIN)
        == load_firmware_steps(GOOD_SYX)
        == encode_firmware_steps(good_bytes)
    )
    print(
        f"  [PASS] sysex_roundtrip            -> verified ({GOOD_SYX}, {len(syx_bytes)} B)"
    )

    failures = 0

    # 1. Corrupted checksum binary must fail both validate_fw and bin_to_sysex
    BROKEN_DIR.mkdir(parents=True, exist_ok=True)
    bad_chk_bin = BROKEN_DIR / "corrupted_checksum.bin"
    corrupted = bytearray(good_bytes)
    corrupted[0x100] ^= 0x01
    bad_chk_bin.write_bytes(corrupted)
    try:
        validate_firmware(bad_chk_bin, verbose=False)
        print(
            "  [FAIL] corrupted_checksum         -> unexpectedly PASSED!",
            file=sys.stderr,
        )
        failures += 1
    except ValidationError as exc:
        if "Bootloader checksum mismatch" in str(exc):
            print(f"  [PASS] {'corrupted_checksum':<26} -> caught: {exc}")
        else:
            print(
                f"  [FAIL] corrupted_checksum         -> wrong error: {exc}",
                file=sys.stderr,
            )
            failures += 1

    try:
        encode_syx_stream(bytes(corrupted))
        print(
            "  [FAIL] sysex_rejects_bad_bin      -> unexpectedly PASSED!",
            file=sys.stderr,
        )
        failures += 1
    except ValueError as exc:
        print(f"  [PASS] {'sysex_rejects_bad_bin':<26} -> caught: {exc}")

    bad_syx = bytearray(syx_bytes)
    bad_syx[20] = ord("0") if bad_syx[20] != ord("0") else ord("1")
    try:
        decode_syx_to_steps(bytes(bad_syx))
        print(
            "  [FAIL] sysex_rejects_bad_frame    -> unexpectedly PASSED!",
            file=sys.stderr,
        )
        failures += 1
    except ValueError as exc:
        print(f"  [PASS] {'sysex_rejects_bad_frame':<26} -> caught: {exc}")

    # 2..12. Compiled C fault-injected firmware variants
    for case in BROKEN_CASES:
        broken_bin = build_broken_variant(case, objs, cc, objcopy)
        try:
            validate_firmware(broken_bin, verbose=False)
            print(f"  [FAIL] {case.name:<26} -> unexpectedly PASSED!", file=sys.stderr)
            failures += 1
        except ValidationError as exc:
            msg = str(exc)
            if case.expected_err in msg:
                print(f"  [PASS] {case.name:<26} -> caught: {msg}")
            else:
                print(
                    f"  [FAIL] {case.name:<26} -> expected '{case.expected_err}', got '{msg}'",
                    file=sys.stderr,
                )
                failures += 1

    if failures:
        print(f"\n{failures} validator test(s) failed!", file=sys.stderr)
        sys.exit(1)
    print(
        f"\nAll {1 + len(BROKEN_CASES)} broken firmwares caught and valid firmware accepted."
    )


if __name__ == "__main__":
    main()
