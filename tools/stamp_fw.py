"""Pad raw APC Mini firmware binary to 22,528 bytes and stamp bootloader checksum."""

import argparse
import struct
import sys
from pathlib import Path

APP_BASE_ADDR = 0x08002000
APP_CHECKSUM_OFF = 0x57FE  # 22,526 bytes (0x08002000..0x080077FD)
APP_TOTAL_SIZE = 0x5800  # 22,528 bytes (0x08002000..0x080077FF)


def stamp_image(raw: bytes) -> bytes:
    if len(raw) > APP_CHECKSUM_OFF:
        raise ValueError(
            f"Raw firmware ({len(raw)} B) exceeds max {APP_CHECKSUM_OFF} B"
        )
    buf = bytearray(b"\xFF" * APP_TOTAL_SIZE)
    buf[: len(raw)] = raw
    sum16 = sum(buf[:APP_CHECKSUM_OFF]) & 0xFFFF
    chk = (-sum16) & 0xFFFF
    struct.pack_into("<H", buf, APP_CHECKSUM_OFF, chk)
    return bytes(buf)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Raw objcopy binary")
    parser.add_argument("output", type=Path, help="Stamped 22,528-byte app binary")
    args = parser.parse_args()

    raw = args.input.read_bytes()
    try:
        stamped = stamp_image(raw)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(stamped)
    chk = struct.unpack_from("<H", stamped, APP_CHECKSUM_OFF)[0]
    end_addr = APP_BASE_ADDR + max(len(raw) - 1, 0)
    print(
        f"[STAMP] {args.input} -> {args.output} ({len(stamped)} B, "
        f"code 0x{APP_BASE_ADDR:08X}..0x{end_addr:08X}, chk=0x{chk:04X})"
    )


if __name__ == "__main__":
    main()
