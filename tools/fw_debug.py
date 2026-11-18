#!/usr/bin/env python3
"""
SWD-Less Live Telemetry, Memory Peek & Post-Mortem Black-Box Crash Analyzer (tools/fw_debug.py).

Communicates with the Open APC Mini over non-commercial USB-MIDI SysEx
(F0 7D 'A' 'P' 'C' <subcmd> ... F7) to inspect:
  1. Silicon signature (DBGMCU_IDCODE, DESIG_FLASH_SIZE at 0x1FFFF7E0)
  2. Last reset cause from RCC_CSR (POR, NRST pin, Software, or IWDG Watchdog timeout)
  3. Live main-loop performance (uptime, loop frequency, worst-case loop stall in ms)
  4. Persistent Post-Mortem Black-Box Crash Log at Flash Page 30 (0x08007800),
     including Cortex-M3 CFSR/HFSR/BFAR/MMFAR decoding and PC/LR symbol resolution
     via arm-none-eabi-addr2line.
  5. Arbitrary 32-bit address memory peek (--peek <addr> [len]).
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

DIAG_SUB_TELEMETRY = 0x01
DIAG_SUB_CRASH_GET = 0x02
DIAG_SUB_CRASH_CLR = 0x03
DIAG_SUB_PEEK_MEM = 0x04
DIAG_SUB_TEST_FAULT = 0x05
DIAG_SUB_SET_MODES = 0x06

APC_MODE_FLAG_MIDI = 0x01
APC_MODE_FLAG_SERIALOSC = 0x02
APC_MODE_FLAG_LUA = 0x04
APC_MODE_FLAG_ABLETON = 0x08

APC_CRASH_MAGIC = 0x43525348  # "CRSH"

IPSR_NAMES = {
    2: "NMI (Non-Maskable Interrupt / HSE Clock Security)",
    3: "HardFault",
    4: "MemManage Fault",
    5: "BusFault",
    6: "UsageFault",
}

CFSR_BITS = [
    (0, "IACCVIOL (Instruction access violation)"),
    (1, "DACCVIOL (Data access violation)"),
    (3, "MUNSTKERR (MemManage fault on unstacking)"),
    (4, "MSTKERR (MemManage fault on stacking)"),
    (7, "MMARVALID (MMFAR holds valid fault address)"),
    (8, "IBUSERR (Instruction bus error)"),
    (9, "PRECISERR (Precise data bus error)"),
    (10, "IMPRECISERR (Imprecise data bus error)"),
    (11, "UNSTKERR (BusFault on exception unstacking)"),
    (12, "STKERR (BusFault on exception stacking)"),
    (15, "BFARVALID (BFAR holds valid fault address)"),
    (16, "UNDEFINSTR (Undefined instruction)"),
    (17, "INVSTATE (Invalid EPSR/Thumb state)"),
    (18, "INVPC (Invalid EXC_RETURN integrity check)"),
    (19, "NOCP (No coprocessor)"),
    (24, "UNALIGNED (Unaligned memory access)"),
    (25, "DIVBYZERO (Divide by zero)"),
]

HFSR_BITS = [
    (1, "VECTTBL (BusFault on vector table read)"),
    (30, "FORCED (Escalated configurable fault)"),
    (31, "DEBUGEVT (Debug event)"),
]


def decode_diag_nibbles(sysex_bytes: bytes) -> tuple[int, bytes]:
    """Validates and decodes F0 7D 'A' 'P' 'C' <subcmd> <nibbles...> F7."""
    if (
        len(sysex_bytes) < 7
        or sysex_bytes[0] != 0xF0
        or sysex_bytes[-1] != 0xF7
        or sysex_bytes[1:5] != b"\x7dAPC"
    ):
        raise ValueError(f"Invalid diagnostic SysEx frame: {sysex_bytes.hex(' ')}")
    subcmd = sysex_bytes[5]
    nibbles = sysex_bytes[6:-1]
    if len(nibbles) % 2 != 0:
        raise ValueError(f"Odd nibble count ({len(nibbles)}) in diagnostic reply")
    out = bytearray(len(nibbles) // 2)
    for i in range(len(out)):
        out[i] = ((nibbles[2 * i] & 0x0F) << 4) | (nibbles[2 * i + 1] & 0x0F)
    return subcmd, bytes(out)


def encode_addr_nibbles(addr: int) -> list[int]:
    le4 = struct.pack("<I", addr & 0xFFFFFFFF)
    nibs: list[int] = []
    for b in le4:
        nibs.append((b >> 4) & 0x0F)
        nibs.append(b & 0x0F)
    return nibs


def decode_reset_flags(flags: int) -> list[str]:
    causes: list[str] = []
    if flags & 0x04:
        causes.append("IWDGRST (Independent Watchdog Timeout!)")
    if flags & 0x08:
        causes.append("WWDGRST (Window Watchdog Timeout)")
    if flags & 0x10:
        causes.append("SFTRST (Software Reset / DFU Reboot)")
    if flags & 0x20:
        causes.append("PORRST (Power-On / Power-Down Reset)")
    if flags & 0x40:
        causes.append("PINRST (NRST Pin Reset)")
    if flags & 0x80:
        causes.append("LPWRRST (Low-Power Reset)")
    return causes or [f"NONE (0x{flags:02X})"]


def resolve_symbol(elf_path: Path, addr: int) -> str:
    if addr == 0 or not elf_path.is_file():
        return ""
    addr2line = shutil.which("arm-none-eabi-addr2line")
    if not addr2line:
        return ""
    try:
        res = subprocess.run(
            [addr2line, "-f", "-p", "-e", str(elf_path), f"0x{addr & ~1:08X}"],
            capture_output=True,
            text=True,
            check=False,
        )
        out = res.stdout.strip()
        if out and not out.startswith("??"):
            return f" ({out})"
    except OSError:
        pass
    return ""


def format_crash_report(raw: bytes, elf_path: Path) -> str:
    if len(raw) < 48:
        return f"Incomplete crash log ({len(raw)} B)"
    (
        magic,
        ipsr,
        fault_msp,
        cfsr,
        hfsr,
        far,
        r0,
        r1,
        r2,
        lr,
        pc,
        uptime_ticks,
    ) = struct.unpack("<12I", raw[:48])

    if magic != APC_CRASH_MAGIC:
        return "No post-mortem crash log recorded in Flash (0x08007800 is clean)."

    exc_name = IPSR_NAMES.get(ipsr & 0x1FF, f"Exception #{ipsr & 0x1FF}")
    cfsr_desc = [desc for bit, desc in CFSR_BITS if cfsr & (1 << bit)] or ["None"]
    hfsr_desc = [desc for bit, desc in HFSR_BITS if hfsr & (1 << bit)] or ["None"]

    lines = [
        "=== POST-MORTEM BLACK-BOX CRASH LOG (0x08007800) ===",
        f"  Exception  : {exc_name} (IPSR=0x{ipsr:08X})",
        f"  Uptime     : {uptime_ticks * 0.5:.1f} ms ({uptime_ticks} ticks)",
        f"  Fault PC   : 0x{pc:08X}{resolve_symbol(elf_path, pc)}",
        f"  Caller LR  : 0x{lr:08X}{resolve_symbol(elf_path, lr)}",
        f"  Fault MSP  : 0x{fault_msp:08X}",
        f"  SCB->CFSR  : 0x{cfsr:08X} ({', '.join(cfsr_desc)})",
        f"  SCB->HFSR  : 0x{hfsr:08X} ({', '.join(hfsr_desc)})",
        f"  Fault Addr : 0x{far:08X} (BFAR/MMFAR)",
        f"  Registers  : R0=0x{r0:08X}  R1=0x{r1:08X}  R2=0x{r2:08X}",
    ]
    return "\n".join(lines)


def send_and_recv(outport, inport, subcmd: int, extra: list[int] | None = None, timeout: float = 1.0) -> bytes:
    import mido

    for _ in inport.iter_pending():
        pass
    body = [0x7D, ord("A"), ord("P"), ord("C"), subcmd]
    if extra:
        body.extend(extra)
    outport.send(mido.Message("sysex", data=body))

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for msg in inport.iter_pending():
            if msg.type == "sysex" and len(msg.data) >= 5 and list(msg.data[:5]) == [0x7D, ord("A"), ord("P"), ord("C"), subcmd]:
                raw = bytes([0xF0, *msg.data, 0xF7])
                _, decoded = decode_diag_nibbles(raw)
                return decoded
        time.sleep(0.002)
    raise TimeoutError(f"Timed out waiting for diagnostic SysEx reply (subcmd=0x{subcmd:02X})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, default=Path("build/apc_mini_app.elf"), help="ELF file for PC/LR symbol lookup")
    parser.add_argument("--clear-crash", action="store_true", help="Erase the persistent crash log at 0x08007800")
    parser.add_argument("--peek", nargs="+", metavar=("ADDR", "LEN"), help="Read memory over SysEx (e.g. --peek 0x1FFFF7E0 16)")
    parser.add_argument("--test-fault", action="store_true", help="Deliberately trigger UDF #0 fault to test crash log -> DFU")
    parser.add_argument("--midi", type=int, choices=[0, 1], help="Enable (1) or disable (0) MIDI Note/CC mode")
    parser.add_argument("--serialosc", type=int, choices=[0, 1], help="Enable (1) or disable (0) Monome serialosc mext mode")
    parser.add_argument("--lua", type=int, choices=[0, 1], help="Enable (1) or disable (0) On-Chip Lua Co-Processor")
    parser.add_argument("--ableton", type=int, choices=[0, 1], help="Enable (1) or disable (0) Ableton Live Intro (0x60) handshake")
    parser.add_argument("--palette", type=int, choices=[0, 1, 2, 3], help="Set Monome grid color palette (0..3)")
    parser.add_argument("--brightness", type=int, choices=[0, 1, 2, 3], help="Set LED brightness preset (0..3)")
    args = parser.parse_args()

    import mido

    in_names = mido.get_input_names()
    out_names = mido.get_output_names()
    in_name = next(
        (n for n in in_names if "APC MINI" in n.upper() or "MONOME" in n.upper()),
        None,
    )
    out_name = next(
        (n for n in out_names if "APC MINI" in n.upper() or "MONOME" in n.upper()),
        None,
    )
    if not in_name or not out_name:
        print(f"error: APC MINI / monome MIDI port not found (inputs={in_names}, outputs={out_names})", file=sys.stderr)
        return 1

    with mido.open_input(in_name) as inport, mido.open_output(out_name) as outport:
        if any(
            v is not None
            for v in (
                args.midi,
                args.serialosc,
                args.lua,
                args.ableton,
                args.palette,
                args.brightness,
            )
        ):
            telem_cur = send_and_recv(outport, inport, DIAG_SUB_TELEMETRY)
            flags = telem_cur[28] if len(telem_cur) >= 30 else 0x0B
            pal = (telem_cur[29] & 0x03) if len(telem_cur) >= 30 else 0
            bri = ((telem_cur[29] >> 4) & 0x03) if len(telem_cur) >= 30 else 3
            for bit, opt in (
                (APC_MODE_FLAG_MIDI, args.midi),
                (APC_MODE_FLAG_SERIALOSC, args.serialosc),
                (APC_MODE_FLAG_LUA, args.lua),
                (APC_MODE_FLAG_ABLETON, args.ableton),
            ):
                if opt is not None:
                    flags = (flags | bit) if opt else (flags & ~bit)
            if args.palette is not None:
                pal = args.palette & 0x03
            if args.brightness is not None:
                bri = args.brightness & 0x03
            rep = send_and_recv(
                outport, inport, DIAG_SUB_SET_MODES, [flags & 0x0F, pal, bri]
            )
            nf = rep[0] if len(rep) >= 2 else flags
            np = (rep[1] & 0x03) if len(rep) >= 2 else pal
            nb = ((rep[1] >> 4) & 0x03) if len(rep) >= 2 else bri
            print(
                f"Updated Modes: MIDI={'ON' if nf & 1 else 'OFF'} | "
                f"serialosc={'ON' if nf & 2 else 'OFF'} | "
                f"Lua={'ON' if nf & 4 else 'OFF'} | "
                f"Ableton={'ON' if nf & 8 else 'OFF'} | "
                f"Palette={np} | Brightness={nb}"
            )
            return 0

        if args.test_fault:
            print("Triggering deliberate UDF #0 fault (device will write 0x08007800 crash log and reboot into DFU)...")
            outport.send(mido.Message("sysex", data=[0x7D, ord("A"), ord("P"), ord("C"), DIAG_SUB_TEST_FAULT, 0x5A]))
            return 0

        if args.clear_crash:
            res = send_and_recv(outport, inport, DIAG_SUB_CRASH_CLR)
            print(f"Cleared crash log at 0x08007800 (status={res[0] if res else 0})")
            return 0

        if args.peek:
            base_addr = int(args.peek[0], 0)
            total_len = int(args.peek[1], 0) if len(args.peek) > 1 else 16
            for off in range(0, total_len, 16):
                cur = base_addr + off
                chunk = send_and_recv(outport, inport, DIAG_SUB_PEEK_MEM, encode_addr_nibbles(cur))
                hex_part = " ".join(f"{b:02X}" for b in chunk)
                ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
                print(f"0x{cur:08X}: {hex_part:<47}  |{ascii_part}|")
            return 0

        telem = send_and_recv(outport, inport, DIAG_SUB_TELEMETRY)
        rst_flags, crash_present, flash_kb, uid0, uptime_ticks, loop_cnt, max_loop_ticks = struct.unpack(
            "<BBHIIIH", telem[:18]
        )
        faders = list(telem[18:27])
        shift = telem[27]
        mode_flags = telem[28] if len(telem) >= 30 else 0x0B
        pal_bri = telem[29] if len(telem) >= 30 else 0x30

        uptime_s = uptime_ticks * 0.0005
        avg_hz = (loop_cnt / uptime_s) if uptime_s > 0 else 0.0

        print("=== OPEN APC MINI LIVE TELEMETRY ===")
        print(f"  MCU Silicon   : STM32F102 | Flash={flash_kb} KB | UID[0]=0x{uid0:08X}")
        print(f"  Last Reset    : {', '.join(decode_reset_flags(rst_flags))}")
        print(f"  Uptime        : {uptime_s:.2f} s ({uptime_ticks} ticks)")
        print(f"  Main Loop     : {loop_cnt} turns (~{avg_hz:.0f} Hz) | Max turn={max_loop_ticks * 0.5:.1f} ms ({max_loop_ticks} ticks)")
        print(
            f"  Active Modes  : MIDI={'ON' if mode_flags & 1 else 'OFF'} | "
            f"serialosc={'ON' if mode_flags & 2 else 'OFF'} | "
            f"Lua={'ON' if mode_flags & 4 else 'OFF'} | "
            f"Ableton={'ON' if mode_flags & 8 else 'OFF'} | "
            f"Palette={pal_bri & 3} | Brightness={(pal_bri >> 4) & 3}"
        )
        print(f"  Faders (1..9) : {faders} | Shift={shift}")
        print()

        crash_raw = send_and_recv(outport, inport, DIAG_SUB_CRASH_GET)
        print(format_crash_report(crash_raw, args.elf))

    return 0


if __name__ == "__main__":
    sys.exit(main())
