import argparse
import struct
import time
from pathlib import Path

import mido

APP_BASE_OFFSET = 0x2000
APP_TOTAL_SIZE = 0x5800  # 22,528 bytes (22 x 1 KB pages: 0x2000..0x77FF)
FLASH_PAGE_SIZE = 0x0400  # 1,024 bytes

# 8 payload bytes + 15 frame overhead bytes = 23 binary bytes
# -> 46 ASCII hex chars + F0 + F7 = 48 MIDI bytes = 16 USB-MIDI packets (64 bytes max)
RAM_CHUNK_SIZE = 8

# Inner bytes of F0 5A 57 6E 28 3C 4E 3E F7 (mido adds F0 and F7 automatically)
UNLOCK_SYSEX_DATA = [0x5A, 0x57, 0x6E, 0x28, 0x3C, 0x4E, 0x3E]


def build_bootloader_sysex_msg(records: bytes) -> mido.Message:
    """
    Wraps command records in the bootloader frame (0x08000DC8..0x08000EE8)
    and returns a standard mido SysEx Message containing uppercase ASCII hex bytes.
    """
    body = bytes([0x6A, 0x04, 0xDA, 0x33]) + records
    r6_len = len(body) + 2
    length_be = struct.pack(">H", r6_len)
    chk = (0 - sum(body)) & 0xFFFF
    chk_le = struct.pack("<H", chk)

    raw_frame = length_be + body + chk_le
    ascii_hex_bytes = raw_frame.hex().upper().encode("ascii")
    return mido.Message("sysex", data=list(ascii_hex_bytes))


def make_record(opcode: int, addr24: int, length16: int, payload: bytes = b"") -> bytes:
    """Creates a single bootloader command record (0x08000EFE)."""
    return (
        bytes(
            [
                opcode,
                (addr24 >> 16) & 0xFF,
                (addr24 >> 8) & 0xFF,
                addr24 & 0xFF,
                (length16 >> 8) & 0xFF,
                length16 & 0xFF,
            ]
        )
        + payload
    )


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
            # Drain any pending messages
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
    unlock_msg = mido.Message("sysex", data=UNLOCK_SYSEX_DATA)
    code = send_and_wait_ack(out_port, in_port, unlock_msg)

    if code == 0x3E:
        print("Bootloader already active and unlocked (ACK 0x3E).")
        return out_port, in_port

    if code == 0x15:
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
                if code == 0x3E:
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


def flash_firmware(fw_bytes: bytes, port_filter: str):
    if len(fw_bytes) != APP_TOTAL_SIZE:
        raise ValueError(f"Expected {APP_TOTAL_SIZE} bytes, got {len(fw_bytes)} bytes")

    out_port, in_port = enter_and_unlock_bootloader(port_filter)
    num_pages = APP_TOTAL_SIZE // FLASH_PAGE_SIZE  # 22 pages (1 KB each)

    try:
        for page_idx in range(num_pages):
            flash_offset = APP_BASE_OFFSET + (page_idx * FLASH_PAGE_SIZE)
            page_data = fw_bytes[
                page_idx * FLASH_PAGE_SIZE : (page_idx + 1) * FLASH_PAGE_SIZE
            ]

            # 1. Load 1 KB page into bootloader RAM buffer (0x200006B4) in 8-byte chunks (Opcode 0x0A)
            for chunk_off in range(0, FLASH_PAGE_SIZE, RAM_CHUNK_SIZE):
                chunk = page_data[chunk_off : chunk_off + RAM_CHUNK_SIZE]
                rec = make_record(0x0A, chunk_off, len(chunk), chunk) + b"\x00"
                msg = build_bootloader_sysex_msg(rec)
                ack = send_and_wait_ack(out_port, in_port, msg)
                if ack != 0x1A:
                    raise RuntimeError(
                        f"RAM load failed at page 0x{flash_offset:04X}+0x{chunk_off:03X}: ACK 0x{ack:02X}"
                    )

            # 2. Erase & program 1 KB Flash page from RAM buffer (Opcode 0x0B)
            rec = make_record(0x0B, flash_offset, FLASH_PAGE_SIZE) + b"\x00"
            msg = build_bootloader_sysex_msg(rec)
            ack = send_and_wait_ack(out_port, in_port, msg, timeout_s=5.0)
            if ack != 0x1A:
                raise RuntimeError(
                    f"Flash write failed at 0x0800{flash_offset:04X}: ACK 0x{ack:02X}"
                )

            pct = ((page_idx + 1) * 100) // num_pages
            print(
                f"\rFlashing: Page {page_idx + 1}/{num_pages} "
                f"(0x0800{flash_offset:04X}) [{pct}%]",
                end="",
                flush=True,
            )

        print("\nVerifying checksum & rebooting into application...")
        finish_rec = make_record(0x0E, 0x000000, 0x0000) + b"\x00"
        finish_msg = build_bootloader_sysex_msg(finish_rec)
        ack = send_and_wait_ack(out_port, in_port, finish_msg, timeout_s=5.0)
        if ack != 0x77:
            raise RuntimeError(f"Finalization failed: ACK 0x{ack:02X} (expected 0x77)")

        print(
            "Success! Bootloader verified checksum (ACK 0x77) and launched application."
        )
    finally:
        out_port.close()
        in_port.close()


def main():
    parser = argparse.ArgumentParser(
        description="Flash stamped app binary to APC Mini via MIDI"
    )
    parser.add_argument(
        "firmware",
        type=Path,
        default=Path("build/apc_mini_app.bin"),
        nargs="?",
        help="Path to 22,528-byte stamped app binary (default: build/apc_mini_app.bin)",
    )
    parser.add_argument(
        "-p", "--port", default="APC", help="MIDI port name substring (default: 'APC')"
    )
    args = parser.parse_args()

    fw_bytes = args.firmware.read_bytes()
    flash_firmware(fw_bytes, args.port)


if __name__ == "__main__":
    main()
