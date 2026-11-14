"""Convert a stamped 22,528-byte APC Mini firmware (.bin) into bootloader MIDI SysEx (.syx)."""

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

APP_BASE_OFFSET = 0x2000
APP_CHECKSUM_OFF = 0x57FE
APP_TOTAL_SIZE = 0x5800  # 22,528 bytes (22 x 1 KB pages: 0x2000..0x77FF)
FLASH_PAGE_SIZE = 0x0400  # 1,024 bytes

# 8 payload bytes + 15 frame overhead bytes = 23 binary bytes
# -> 46 ASCII hex chars + F0 + F7 = 48 MIDI bytes = 12 USB-MIDI packets (<= 64 B bulk packet)
RAM_CHUNK_SIZE = 8

# Raw MIDI SysEx message that unlocks DFU from App (ACK 0x15) or Bootloader (ACK 0x3E)
UNLOCK_SYSEX = bytes([0xF0, 0x5A, 0x57, 0x6E, 0x28, 0x3C, 0x4E, 0x3E, 0xF7])

OPCODE_RAM_LOAD = 0x0A
OPCODE_FLASH_WRITE = 0x0B
OPCODE_FINALIZE = 0x0E

ACK_APP_UNLOCK = 0x15
ACK_BL_UNLOCK = 0x3E
ACK_OK = 0x1A
ACK_DONE = 0x77

StepKind = Literal["ram_load", "flash_write", "finalize"]


@dataclass(frozen=True)
class SysexStep:
    """Single bootloader SysEx command message and its expected ACK status byte."""

    kind: StepKind
    sysex: bytes  # Complete raw MIDI SysEx message (F0 ... F7)
    expected_ack: int
    page_idx: int | None = None
    flash_offset: int | None = None
    chunk_offset: int | None = None


def validate_stamped_bin(fw_bytes: bytes) -> None:
    if len(fw_bytes) != APP_TOTAL_SIZE:
        raise ValueError(f"Expected {APP_TOTAL_SIZE} B stamped binary, got {len(fw_bytes)} B")
    sum16 = sum(fw_bytes[:APP_CHECKSUM_OFF]) & 0xFFFF
    chk = struct.unpack_from("<H", fw_bytes, APP_CHECKSUM_OFF)[0]
    if ((sum16 + chk) & 0xFFFF) != 0:
        raise ValueError(
            f"Invalid bootloader checksum in binary: (0x{sum16:04X} + 0x{chk:04X}) & 0xFFFF != 0"
        )


def make_record(opcode: int, addr24: int, length16: int, payload: bytes = b"") -> bytes:
    """Builds a single bootloader command record (0x08000EFE)."""
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


def build_bootloader_sysex(records: bytes) -> bytes:
    """
    Wraps command records in the bootloader frame (0x08000DC8..0x08000EE8)
    and returns a complete raw MIDI SysEx message (F0 <ASCII_HEX> F7).
    """
    body = bytes([0x6A, 0x04, 0xDA, 0x33]) + records
    r6_len = len(body) + 2
    length_be = struct.pack(">H", r6_len)
    chk_le = struct.pack("<H", (-sum(body)) & 0xFFFF)

    raw_frame = length_be + body + chk_le
    return b"\xF0" + raw_frame.hex().upper().encode("ascii") + b"\xF7"


def encode_firmware_steps(fw_bytes: bytes) -> list[SysexStep]:
    """Converts a 22,528-byte stamped firmware binary into ordered bootloader SysEx steps."""
    validate_stamped_bin(fw_bytes)
    num_pages = APP_TOTAL_SIZE // FLASH_PAGE_SIZE
    steps: list[SysexStep] = []

    for page_idx in range(num_pages):
        flash_offset = APP_BASE_OFFSET + (page_idx * FLASH_PAGE_SIZE)
        page_data = fw_bytes[page_idx * FLASH_PAGE_SIZE : (page_idx + 1) * FLASH_PAGE_SIZE]

        # 1. Load 1 KB page into bootloader RAM buffer (0x200006B4) in 8-byte chunks (0x0A)
        for chunk_off in range(0, FLASH_PAGE_SIZE, RAM_CHUNK_SIZE):
            chunk = page_data[chunk_off : chunk_off + RAM_CHUNK_SIZE]
            rec = make_record(OPCODE_RAM_LOAD, chunk_off, len(chunk), chunk) + b"\x00"
            steps.append(
                SysexStep(
                    kind="ram_load",
                    sysex=build_bootloader_sysex(rec),
                    expected_ack=ACK_OK,
                    page_idx=page_idx,
                    flash_offset=flash_offset,
                    chunk_offset=chunk_off,
                )
            )

        # 2. Erase & program 1 KB Flash page from RAM buffer (0x0B)
        rec = make_record(OPCODE_FLASH_WRITE, flash_offset, FLASH_PAGE_SIZE) + b"\x00"
        steps.append(
            SysexStep(
                kind="flash_write",
                sysex=build_bootloader_sysex(rec),
                expected_ack=ACK_OK,
                page_idx=page_idx,
                flash_offset=flash_offset,
            )
        )

    # 3. Verify application checksum & reboot into application (0x0E)
    finish_rec = make_record(OPCODE_FINALIZE, 0x000000, 0x0000) + b"\x00"
    steps.append(
        SysexStep(
            kind="finalize",
            sysex=build_bootloader_sysex(finish_rec),
            expected_ack=ACK_DONE,
        )
    )
    return steps


def encode_syx_stream(fw_bytes: bytes, *, include_unlock: bool = False) -> bytes:
    """Encodes firmware into a standard concatenated MIDI .syx byte stream."""
    parts = [UNLOCK_SYSEX] if include_unlock else []
    parts.extend(step.sysex for step in encode_firmware_steps(fw_bytes))
    return b"".join(parts)


def split_syx_messages(syx_bytes: bytes) -> list[bytes]:
    """Splits a .syx stream into individual F0..F7 MIDI SysEx messages."""
    msgs: list[bytes] = []
    idx = 0
    n = len(syx_bytes)
    while idx < n:
        if syx_bytes[idx] != 0xF0:
            raise ValueError(f"Expected 0xF0 at byte offset {idx}, got 0x{syx_bytes[idx]:02X}")
        end = syx_bytes.find(0xF7, idx + 1)
        if end < 0:
            raise ValueError(f"Unterminated SysEx message starting at offset {idx}")
        msgs.append(syx_bytes[idx : end + 1])
        idx = end + 1
    if not msgs:
        raise ValueError("Empty .syx stream")
    return msgs


def decode_syx_to_steps(syx_bytes: bytes) -> list[SysexStep]:
    """Parses a bootloader .syx file back into validated SysexStep records."""
    steps: list[SysexStep] = []
    page_idx = 0
    for msg in split_syx_messages(syx_bytes):
        if msg == UNLOCK_SYSEX:
            continue
        raw = bytes.fromhex(msg[1:-1].decode("ascii"))
        r6_len = struct.unpack_from(">H", raw, 0)[0]
        body = raw[2:-2]
        chk = struct.unpack_from("<H", raw, len(raw) - 2)[0]
        if r6_len != len(body) + 2 or ((sum(body) + chk) & 0xFFFF) != 0:
            raise ValueError("Corrupted bootloader SysEx frame length or checksum")
        if body[:4] != bytes([0x6A, 0x04, 0xDA, 0x33]):
            raise ValueError("Invalid bootloader frame magic header")

        rec = body[4:]
        opcode = rec[0]
        addr24 = (rec[1] << 16) | (rec[2] << 8) | rec[3]
        if opcode == OPCODE_RAM_LOAD:
            steps.append(
                SysexStep(
                    "ram_load",
                    msg,
                    ACK_OK,
                    page_idx=page_idx,
                    flash_offset=APP_BASE_OFFSET + page_idx * FLASH_PAGE_SIZE,
                    chunk_offset=addr24,
                )
            )
        elif opcode == OPCODE_FLASH_WRITE:
            steps.append(
                SysexStep("flash_write", msg, ACK_OK, page_idx=page_idx, flash_offset=addr24)
            )
            page_idx += 1
        elif opcode == OPCODE_FINALIZE:
            steps.append(SysexStep("finalize", msg, ACK_DONE))
        else:
            raise ValueError(f"Unknown bootloader SysEx opcode: 0x{opcode:02X}")
    return steps


def decode_syx_to_bin(syx_bytes: bytes) -> bytes:
    """Reconstructs and validates the 22,528-byte firmware image from a bootloader .syx stream."""
    flash_image = bytearray(b"\xFF" * APP_TOTAL_SIZE)
    ram_page = bytearray(b"\xFF" * FLASH_PAGE_SIZE)
    finalized = False

    for step in decode_syx_to_steps(syx_bytes):
        raw = bytes.fromhex(step.sysex[1:-1].decode("ascii"))
        rec = raw[6:-2]
        length16 = (rec[4] << 8) | rec[5]
        if step.kind == "ram_load":
            assert step.chunk_offset is not None
            payload = rec[6 : 6 + length16]
            ram_page[step.chunk_offset : step.chunk_offset + length16] = payload
        elif step.kind == "flash_write":
            assert step.flash_offset is not None
            dst = step.flash_offset - APP_BASE_OFFSET
            if dst < 0 or dst + length16 > APP_TOTAL_SIZE or length16 != FLASH_PAGE_SIZE:
                raise ValueError(f"Invalid flash_write range: 0x{step.flash_offset:04X}+{length16}")
            flash_image[dst : dst + length16] = ram_page
            ram_page[:] = b"\xFF" * FLASH_PAGE_SIZE
        elif step.kind == "finalize":
            finalized = True

    if not finalized:
        raise ValueError("Missing bootloader finalize (0x0E) command in .syx stream")
    result = bytes(flash_image)
    validate_stamped_bin(result)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input",
        type=Path,
        nargs="?",
        default=Path("build/apc_mini_app.bin"),
        help="Input 22,528-byte stamped .bin file (default: build/apc_mini_app.bin)",
    )
    parser.add_argument(
        "output",
        type=Path,
        nargs="?",
        default=None,
        help="Output .syx path (default: input path with .syx extension)",
    )
    parser.add_argument(
        "--include-unlock",
        action="store_true",
        help="Prepend the 9-byte bootloader unlock SysEx (F0 5A 57 6E 28 3C 4E 3E F7)",
    )
    args = parser.parse_args()

    out_path = args.output if args.output is not None else args.input.with_suffix(".syx")
    try:
        fw_bytes = args.input.read_bytes()
        steps = encode_firmware_steps(fw_bytes)
        syx_data = (UNLOCK_SYSEX if args.include_unlock else b"") + b"".join(
            s.sysex for s in steps
        )
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(syx_data)
    msg_count = len(steps) + (1 if args.include_unlock else 0)
    print(f"[SYSEX] {args.input} -> {out_path} ({msg_count} messages, {len(syx_data)} B)")


if __name__ == "__main__":
    main()
