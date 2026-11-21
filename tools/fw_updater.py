"""Flash a stamped .bin or pre-encoded .syx firmware image to APC Mini over USB-MIDI."""

import argparse
import glob
import os
import subprocess
import sys
import time
from pathlib import Path

import mido

from tools.bin_to_sysex import (
    ACK_APP_UNLOCK,
    ACK_BL_UNLOCK,
    APP_TOTAL_SIZE,
    UNLOCK_SYSEX,
    SysexStep,
    decode_syx_to_steps,
    encode_firmware_steps,
)
from tools.serialosc_osc import serialosc_trigger_dfu


def raw_sysex_to_mido(sysex: bytes) -> mido.Message:
    if len(sysex) < 2 or sysex[0] != 0xF0 or sysex[-1] != 0xF7:
        raise ValueError("Invalid raw MIDI SysEx message framing")
    return mido.Message("sysex", data=list(sysex[1:-1]))


def load_firmware_steps(path: Path) -> list[SysexStep]:
    data = path.read_bytes()
    if path.suffix.lower() == ".syx" or (len(data) != APP_TOTAL_SIZE and data.startswith(b"\xF0")):
        return decode_syx_to_steps(data)
    return encode_firmware_steps(data)


def _match_midi_port(names: list[str], port_filter: str, prefer_bootloader: bool = False) -> str | None:
    filt = port_filter.lower()
    if prefer_bootloader:
        for kw in ("updater", filt, "apc"):
            m = next((p for p in names if kw in p.lower()), None)
            if m:
                return m
        return None
    for kw in (filt, "monome", "updater"):
        m = next((p for p in names if kw in p.lower()), None)
        if m:
            return m
    return None


def open_apc_midi_ports(port_filter: str, timeout_s: float = 8.0, prefer_bootloader: bool = False):
    """Waits for and opens matching system MIDI IN and OUT ports."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        out_names = mido.get_output_names()
        in_names = mido.get_input_names()
        out_match = _match_midi_port(out_names, port_filter, prefer_bootloader)
        in_match = _match_midi_port(in_names, port_filter, prefer_bootloader)
        if out_match and in_match:
            out_port = mido.open_output(out_match)
            in_port = mido.open_input(in_match)
            for _ in in_port.iter_pending():
                pass
            return out_port, in_port
        time.sleep(0.25)

    raise RuntimeError(
        f"MIDI ports matching '{port_filter}' not found.\n"
        f"Available OUT: {mido.get_output_names()}\n"
        f"Available IN:  {mido.get_input_names()}"
    )


def send_and_wait_ack(
    out_port,
    in_port,
    msg: mido.Message,
    timeout_s: float = 3.0,
    ignore_unlock_ack: bool = False,
) -> int:
    """
    Sends a SysEx message via system MIDI and waits for the 1-byte status SysEx reply
    (F0 <status_code> F7). Ignores any non-SysEx bytes leaked from uninitialized stack.
    """
    for _ in in_port.iter_pending():
        pass

    out_port.send(msg)

    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for rx_msg in in_port.iter_pending():
            if rx_msg.type == "sysex" and len(rx_msg.data) >= 1:
                code = rx_msg.data[0]
                if ignore_unlock_ack and code == ACK_BL_UNLOCK:
                    continue
                return code
        time.sleep(0.001)

    raise TimeoutError("Timed out waiting for SysEx ACK from APC Mini.")


def try_serial_dfu_unlock() -> bool:
    """If the APC Mini is currently in Monome FTDI mode, send DFU unlock via serialoscd OSC or raw serial."""
    if serialosc_trigger_dfu():
        return True
    serial_ports = sorted(glob.glob("/dev/cu.usbserial-m*") + glob.glob("/dev/ttyUSB*"))
    for sp in serial_ports:
        try:
            fd = os.open(sp, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            try:
                os.write(fd, UNLOCK_SYSEX)
                print(f"Sent raw DFU unlock over Monome serial port {sp}; waiting for USB-MIDI bootloader...")
            finally:
                os.close(fd)
            time.sleep(1.5)
            return True
        except OSError:
            pass
    return False


def wait_for_bootloader_unlock(port_filter: str):
    """Polls until the USB-MIDI bootloader enumerates and answers UNLOCK with 0x3E."""
    unlock_msg = raw_sysex_to_mido(UNLOCK_SYSEX)
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        try:
            out_port, in_port = open_apc_midi_ports(
                port_filter, timeout_s=2.0, prefer_bootloader=True
            )
            code = send_and_wait_ack(out_port, in_port, unlock_msg, timeout_s=1.5)
            if code == ACK_BL_UNLOCK:
                print("Bootloader unlocked (ACK 0x3E).")
                time.sleep(0.25)
                return out_port, in_port
            out_port.close()
            in_port.close()
        except Exception:
            pass
        time.sleep(0.5)

    raise RuntimeError(
        "Timed out waiting for APC Mini to enter bootloader mode (0x3E)."
    )


def enter_and_unlock_bootloader(port_filter: str):
    """Unlocks via system MIDI from either the running App (0x15), Monome FTDI/OSC, or Bootloader (0x3E)."""
    try:
        out_port, in_port = open_apc_midi_ports(port_filter, timeout_s=1.5)
    except RuntimeError:
        try_serial_dfu_unlock()
        return wait_for_bootloader_unlock(port_filter)

    unlock_msg = raw_sysex_to_mido(UNLOCK_SYSEX)
    code = send_and_wait_ack(out_port, in_port, unlock_msg)

    if code == ACK_BL_UNLOCK:
        print("Bootloader already active and unlocked (ACK 0x3E).")
        time.sleep(0.1)
        return out_port, in_port

    if code == ACK_APP_UNLOCK:
        print(
            "Application accepted unlock (ACK 0x15). Waiting for USB-MIDI bootloader re-enumeration..."
        )
        out_port.close()
        in_port.close()
        time.sleep(2.5)
        return wait_for_bootloader_unlock(port_filter)

    raise RuntimeError(f"Unexpected unlock ACK code: 0x{code:02X}")


def _send_step_with_retry(
    out_port, in_port, step: SysexStep, port_filter: str
) -> tuple[int, object, object]:
    timeout_s = 5.0 if step.kind in ("flash_write", "finalize") else 1.5
    max_attempts = 5 if step.kind in ("ram_load", "flash_write") else 1
    msg = raw_sysex_to_mido(step.sysex)
    unlock_msg = raw_sysex_to_mido(UNLOCK_SYSEX)
    for attempt in range(max_attempts):
        try:
            ack = send_and_wait_ack(
                out_port, in_port, msg, timeout_s=timeout_s, ignore_unlock_ack=True
            )
            if ack == step.expected_ack or step.kind != "ram_load" or attempt + 1 >= max_attempts:
                return ack, out_port, in_port
        except TimeoutError:
            if attempt + 1 >= max_attempts:
                raise

        # Background DAW probes (e.g. Ableton Live / REAPER) can collide right after USB
        # enumeration; let the probe burst settle, reopen ports if needed, and re-sync 0x3E.
        time.sleep(0.4)
        for _ in in_port.iter_pending():
            pass
        if attempt >= 2:
            out_port.close()
            in_port.close()
            out_port, in_port = open_apc_midi_ports(
                port_filter, timeout_s=4.0, prefer_bootloader=True
            )
        try:
            send_and_wait_ack(out_port, in_port, unlock_msg, timeout_s=1.5)
        except Exception:
            pass
        time.sleep(0.1)
    raise AssertionError("unreachable")


def flash_steps_on_ports(
    steps: list[SysexStep], out_port, in_port, port_filter: str
) -> None:
    num_pages = sum(1 for s in steps if s.kind == "flash_write")
    try:
        for step in steps:
            if step.kind == "finalize":
                print("\nVerifying checksum & rebooting into application...")

            ack, out_port, in_port = _send_step_with_retry(
                out_port, in_port, step, port_filter
            )
            if ack != step.expected_ack:
                if step.kind == "ram_load":
                    raise RuntimeError(
                        f"RAM load failed at page 0x{(step.flash_offset or 0):04X}"
                        f"+0x{(step.chunk_offset or 0):03X}: ACK 0x{ack:02X}"
                    )
                if step.kind == "flash_write":
                    raise RuntimeError(
                        f"Flash write failed at 0x0800{(step.flash_offset or 0):04X}: ACK 0x{ack:02X}"
                    )
                raise RuntimeError(
                    f"Finalization failed: ACK 0x{ack:02X} (expected 0x{step.expected_ack:02X})"
                )

            if step.kind == "flash_write" and step.page_idx is not None and num_pages:
                pct = ((step.page_idx + 1) * 100) // num_pages
                print(
                    f"\rFlashing: Page {step.page_idx + 1}/{num_pages} "
                    f"(0x0800{(step.flash_offset or 0):04X}) [{pct}%]",
                    end="",
                    flush=True,
                )

        print(
            "Success! Bootloader verified checksum (ACK 0x77) and launched application."
        )
    finally:
        out_port.close()
        in_port.close()


def flash_steps(steps: list[SysexStep], port_filter: str) -> None:
    out_port, in_port = enter_and_unlock_bootloader(port_filter)
    flash_steps_on_ports(steps, out_port, in_port, port_filter)


def flash_firmware(fw_bytes: bytes, port_filter: str) -> None:
    flash_steps(encode_firmware_steps(fw_bytes), port_filter)


def _spawn_bootloader_flasher(firmware: Path, port_filter: str) -> None:
    # Spawn a fresh Python process so macOS CoreMIDI/python-rtmidi creates a clean
    # client after USB disconnect/re-enumeration instead of holding stale endpoints.
    res = subprocess.run(
        [
            sys.executable,
            "-m",
            "tools.fw_updater",
            "--wait-bootloader",
            str(firmware),
            "-p",
            port_filter,
        ],
        check=False,
    )
    if res.returncode != 0:
        raise SystemExit(res.returncode)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Flash stamped .bin or .syx firmware image to APC Mini via MIDI"
    )
    parser.add_argument(
        "firmware",
        type=Path,
        default=Path("build/apc_mini_app.syx"),
        nargs="?",
        help="Path to .syx or 22,528-byte stamped .bin (default: build/apc_mini_app.syx)",
    )
    parser.add_argument(
        "-p", "--port", default="APC", help="MIDI port name substring (default: 'APC')"
    )
    parser.add_argument(
        "--wait-bootloader",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    args = parser.parse_args()

    steps = load_firmware_steps(args.firmware)
    if args.wait_bootloader:
        out_port, in_port = wait_for_bootloader_unlock(args.port)
        flash_steps_on_ports(steps, out_port, in_port, args.port)
        return

    try:
        out_port, in_port = open_apc_midi_ports(args.port, timeout_s=1.5)
    except RuntimeError:
        try_serial_dfu_unlock()
        time.sleep(1.5)
        _spawn_bootloader_flasher(args.firmware, args.port)
        return

    unlock_msg = raw_sysex_to_mido(UNLOCK_SYSEX)
    code = send_and_wait_ack(out_port, in_port, unlock_msg)

    if code == ACK_BL_UNLOCK:
        print("Bootloader already active and unlocked (ACK 0x3E).")
        time.sleep(0.1)
        flash_steps_on_ports(steps, out_port, in_port, args.port)
        return

    if code == ACK_APP_UNLOCK:
        print(
            "Application accepted unlock (ACK 0x15). Waiting for USB-MIDI bootloader re-enumeration..."
        )
        out_port.close()
        in_port.close()
        time.sleep(2.5)
        _spawn_bootloader_flasher(args.firmware, args.port)
        return

    out_port.close()
    in_port.close()
    raise RuntimeError(f"Unexpected unlock ACK code: 0x{code:02X}")


if __name__ == "__main__":
    main()
