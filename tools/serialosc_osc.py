"""Minimal zero-dependency UDP OSC client for tunneling DFU & SysEx through serialoscd."""

from __future__ import annotations

import socket
import struct
import time


def _osc_str(s: str) -> bytes:
    raw = s.encode("utf-8") + b"\x00"
    pad = (4 - (len(raw) % 4)) % 4
    return raw + (b"\x00" * pad)


def _read_osc_str(buf: bytes, off: int) -> tuple[str, int]:
    end = buf.index(0, off)
    s = buf[off:end].decode("utf-8", errors="replace")
    next_off = (end + 4) & ~3
    return s, next_off


def pack_osc_msg(address: str, *args: str | int) -> bytes:
    tags = [","]
    payload = bytearray()
    for a in args:
        if isinstance(a, str):
            tags.append("s")
            payload.extend(_osc_str(a))
        elif isinstance(a, int):
            tags.append("i")
            payload.extend(struct.pack(">i", a))
        else:
            raise TypeError(f"Unsupported OSC arg type: {type(a)}")
    return _osc_str(address) + _osc_str("".join(tags)) + bytes(payload)


def unpack_osc_msg(buf: bytes) -> tuple[str, list[str | int]]:
    addr, off = _read_osc_str(buf, 0)
    tags, off = _read_osc_str(buf, off)
    args: list[str | int] = []
    for t in tags.lstrip(","):
        if t == "s":
            s, off = _read_osc_str(buf, off)
            args.append(s)
        elif t == "i":
            val = struct.unpack_from(">i", buf, off)[0]
            off += 4
            args.append(val)
    return addr, args


def discover_monome_osc(timeout_s: float = 0.25) -> tuple[int, str] | None:
    """Queries serialoscd on 127.0.0.1:12002 and returns (device_udp_port, osc_prefix)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("127.0.0.1", 0))
        local_port = sock.getsockname()[1]
        sock.settimeout(timeout_s)

        try:
            sock.sendto(
                pack_osc_msg("/serialosc/list", "127.0.0.1", local_port),
                ("127.0.0.1", 12002),
            )
        except OSError:
            return None

        dev_port: int | None = None
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            try:
                data, _ = sock.recvfrom(2048)
                addr, args = unpack_osc_msg(data)
                if addr == "/serialosc/device" and len(args) >= 3 and isinstance(args[2], int):
                    dev_id = str(args[0]).lower()
                    dev_type = str(args[1]).lower()
                    if "m1" in dev_id or "apc" in dev_type or "monome" in dev_type:
                        dev_port = int(args[2])
                        break
            except (TimeoutError, OSError, ValueError):
                break

        if dev_port is None:
            return None

        prefix = "/monome"
        try:
            sock.sendto(
                pack_osc_msg("/sys/info", "127.0.0.1", local_port),
                ("127.0.0.1", dev_port),
            )
            info_deadline = time.monotonic() + 0.12
            while time.monotonic() < info_deadline:
                data, _ = sock.recvfrom(2048)
                addr, args = unpack_osc_msg(data)
                if addr == "/sys/prefix" and args and isinstance(args[0], str):
                    p = args[0].strip()
                    if p:
                        prefix = p if p.startswith("/") else f"/{p}"
                    break
        except (TimeoutError, OSError, ValueError):
            pass

        return dev_port, prefix
    finally:
        sock.close()


def serialosc_trigger_dfu() -> bool:
    """Tunnels DFU bootloader entry ([0x81, 0xDF]) through serialoscd via <prefix>/tilt/set."""
    found = discover_monome_osc()
    if found is None:
        return False
    dev_port, prefix = found
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.sendto(
            pack_osc_msg(f"{prefix}/tilt/set", 0xDF, 1),
            ("127.0.0.1", dev_port),
        )
        print(
            f"Sent DFU unlock via serialoscd UDP OSC (127.0.0.1:{dev_port} {prefix}/tilt/set 223 1); "
            "waiting for USB-MIDI bootloader..."
        )
        time.sleep(1.6)
        return True
    except OSError:
        return False
    finally:
        sock.close()


def serialosc_send_sysex(packets: list[list[int]]) -> str | None:
    """Tunnels 7-bit SysEx frames through serialoscd via <prefix>/tilt/set <byte> <0|1>."""
    found = discover_monome_osc()
    if found is None:
        return None
    dev_port, prefix = found
    tilt_path = f"{prefix}/tilt/set"
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for pkt in packets:
            if len(pkt) < 2 or pkt[0] != 0xF0 or pkt[-1] != 0xF7:
                continue
            sock.sendto(pack_osc_msg(tilt_path, 0xF0, 1), ("127.0.0.1", dev_port))
            for b in pkt[1:-1]:
                sock.sendto(pack_osc_msg(tilt_path, b & 0x7F, 0), ("127.0.0.1", dev_port))
            sock.sendto(pack_osc_msg(tilt_path, 0xF7, 1), ("127.0.0.1", dev_port))
            time.sleep(0.006)
        return f"serialoscd UDP OSC (127.0.0.1:{dev_port} {tilt_path})"
    except OSError:
        return None
    finally:
        sock.close()
