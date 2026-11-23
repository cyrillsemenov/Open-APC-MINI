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
        wrap_symbol="relocate_vtor",
        c_stub="void __wrap_relocate_vtor(void) {}",
        expected_err="SCB->VTOR not relocated",
    ),
    BrokenCase(
        name="wrong_sysclk_no_pll",
        wrap_symbol="rcc_clock_setup_pll",
        c_stub="void __wrap_rcc_clock_setup_pll(const void *c) { (void)c; }",
        expected_err="SYSCLK not switched to PLL",
    ),
    BrokenCase(
        name="no_usb_pullup_pa8",
        wrap_symbol="board_usb_dp_pullup",
        c_stub="void __wrap_board_usb_dp_pullup(_Bool e) { (void)e; }",
        expected_err="PA8 (USB D+ pull-up) not driven HIGH",
    ),
    BrokenCase(
        name="watchdog_not_reloaded",
        wrap_symbol="iwdg_reset",
        c_stub=(
            "extern void __real_iwdg_reset(void);\n"
            "void __wrap_iwdg_reset(void) {\n"
            "    static int once = 0;\n"
            "    if (!once++) __real_iwdg_reset();\n"
            "}\n"
        ),
        expected_err="IWDG watchdog is not reloaded in main loop",
    ),
    BrokenCase(
        name="cpu_fault_on_init",
        wrap_symbol="faders_init",
        c_stub="extern void hard_fault_handler(void); void __wrap_faders_init(void) { hard_fault_handler(); }",
        expected_err="CPU entered fault handler",
    ),
    BrokenCase(
        name="infinite_hang_on_boot",
        wrap_symbol="faders_init",
        c_stub="void __wrap_faders_init(void) { while (1) {} }",
        expected_err="IWDG watchdog was not started",
    ),
    BrokenCase(
        name="broken_midi_rx_sysex",
        wrap_symbol="sysex_handle_msg",
        c_stub="void __wrap_sysex_handle_msg(const void *m, unsigned short n) { (void)m; (void)n; }",
        expected_err="Bad SysEx Identity Reply",
    ),
    BrokenCase(
        name="broken_button_scan",
        wrap_symbol="buttons_task",
        c_stub="void __wrap_buttons_task(unsigned int t, void *cb) { (void)t; (void)cb; }",
        expected_err="Missing Pad Note-On",
    ),
    BrokenCase(
        name="broken_fader_adc",
        wrap_symbol="faders_task",
        c_stub="void __wrap_faders_task(void *cb) { (void)cb; }",
        expected_err="Expected Fader 1 CC 0x30=127",
    ),
    BrokenCase(
        name="broken_tm1629_leds",
        wrap_symbol="tm1629_task",
        c_stub="void __wrap_tm1629_task(unsigned int t) { (void)t; }",
        expected_err="TM1629 missing 0x40 data command",
    ),
    BrokenCase(
        name="broken_dfu_unlock",
        wrap_symbol="bootloader_check_unlock_packet",
        c_stub="void __wrap_bootloader_check_unlock_packet(const void *p) { (void)p; }",
        expected_err="Bootloader magic not programmed at 0x08007C00",
    ),
    BrokenCase(
        name="broken_fault_recovery",
        wrap_symbol="bootloader_fault_dfu",
        c_stub="void __wrap_bootloader_fault_dfu(unsigned int msp, unsigned int ipsr) { (void)msp; (void)ipsr; }",
        expected_err="Black-box crash log at 0x08007800 did not capture exception frame",
    ),
    BrokenCase(
        name="broken_monome_mext",
        wrap_symbol="monome_mode_poll_rx",
        c_stub="void __wrap_monome_mode_poll_rx(unsigned int t) { (void)t; }",
        expected_err="Bad mext /sys reply",
    ),
    BrokenCase(
        name="broken_lua_vm",
        wrap_symbol="lua_vm_toggle",
        c_stub="void __wrap_lua_vm_toggle(unsigned int t) { (void)t; }",
        expected_err="Lua Co-Processor did not emit pass-through",
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
            "-Llib/libopencm3/lib",
            "-Tld/stm32f102rb_app.ld",
            "-nostartfiles",
            "-Wl,--gc-sections",
            f"-Wl,--wrap={case.wrap_symbol}",
            "--specs=nano.specs",
            "--specs=nosys.specs",
            "-o",
            str(elf_path),
            "-Wl,--start-group",
            "-lopencm3_stm32f1",
            "-lc",
            "-lgcc",
            "-lnosys",
            "-Wl,--end-group",
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

    objs = sorted(str(p) for p in BUILD_DIR.glob("obj/**/*.o"))
    if not objs:
        print(
            "error: missing object files in build/obj/; run 'make all' first",
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
