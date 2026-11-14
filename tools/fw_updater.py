"""Flash a stamped .bin or pre-encoded .syx firmware image to APC Mini over USB-MIDI."""

import argparse
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


def raw_sysex_to_mido(sysex: bytes) -> mido.Message:
    if len(sysex) < 2 or sysex[0] != 0xF0 or sysex[-1] != 0xF7:
        raise ValueError("Invalid raw MIDI SysEx message framing")
    return mido.Message("sysex", data=list(sysex[1:-1]))


def load_firmware_steps(path: Path) -> list[SysexStep]:
    data = path.read_bytes()
    if path.suffix.lower() == ".syx" or (len(data) != APP_TOTAL_SIZE and data.startswith(b"\xF0")):
        return decode_syx_to_steps(data)
    return encode_firmware_steps(data)


def open_apc_midi_ports(port_filter: str, timeout_s: float = 8.0):
    """Waits for and opens matching system MIDI IN and OUT ports."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        out_names = mido.get_output_names()
        in_names = mido.get_input_names()
        out_match = next(
            (p for p in out_names if port_filter.lower() in p.lower()), None
        )
        in_match = next((p for p in in_names if port_filter.lower() in p.lower()), None)
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
    out_port, in_port, msg: mido.Message, timeout_s: float = 3.0
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
                return rx_msg.data[0]
        time.sleep(0.001)

    raise TimeoutError("Timed out waiting for SysEx ACK from APC Mini.")


def enter_and_unlock_bootloader(port_filter: str):
    """Unlocks via system MIDI from either the running App (0x15) or Bootloader (0x3E)."""
    out_port, in_port = open_apc_midi_ports(port_filter)
    unlock_msg = raw_sysex_to_mido(UNLOCK_SYSEX)
    code = send_and_wait_ack(out_port, in_port, unlock_msg)

    if code == ACK_BL_UNLOCK:
        print("Bootloader already active and unlocked (ACK 0x3E).")
        return out_port, in_port

    if code == ACK_APP_UNLOCK:
        print(
            "Application accepted unlock (ACK 0x15). Please power-cycle USB if it doesn't auto-reset..."
        )
        out_port.close()
        in_port.close()
        time.sleep(2.0)

        # Poll until device comes back in bootloader mode and replies 0x3E
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            try:
                out_port, in_port = open_apc_midi_ports(port_filter, timeout_s=2.0)
                code = send_and_wait_ack(out_port, in_port, unlock_msg, timeout_s=1.5)
                if code == ACK_BL_UNLOCK:
                    print("Bootloader unlocked (ACK 0x3E).")
                    return out_port, in_port
                out_port.close()
                in_port.close()
            except Exception:
                pass
            time.sleep(0.5)

        raise RuntimeError(
            "Timed out waiting for APC Mini to enter bootloader mode (0x3E)."
        )

    raise RuntimeError(f"Unexpected unlock ACK code: 0x{code:02X}")


def flash_steps(steps: list[SysexStep], port_filter: str) -> None:
    num_pages = sum(1 for s in steps if s.kind == "flash_write")
    out_port, in_port = enter_and_unlock_bootloader(port_filter)

    try:
        for step in steps:
            if step.kind == "finalize":
                print("\nVerifying checksum & rebooting into application...")

            timeout_s = 5.0 if step.kind in ("flash_write", "finalize") else 3.0
            ack = send_and_wait_ack(
                out_port, in_port, raw_sysex_to_mido(step.sysex), timeout_s=timeout_s
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


def flash_firmware(fw_bytes: bytes, port_filter: str) -> None:
    flash_steps(encode_firmware_steps(fw_bytes), port_filter)


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
    args = parser.parse_args()

    steps = load_firmware_steps(args.firmware)
    flash_steps(steps, args.port)


if __name__ == "__main__":
    main()
