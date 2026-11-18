#!/usr/bin/env python3
"""
Lua-to-APC Bytecode Compiler & Live Uploader (tools/lua_compiler.py).

Uses `luaparser` to parse standard Lua 5.3/5.4 AST (functions init, on_tick,
on_grid / g.key, on_fader, locals, globals, mem[] arrays, if/elseif/else,
while, numeric for, expressions, and APC/Monome hardware intrinsics) into a
compact "LUA1" stack bytecode image (< 768 B) for the STM32F102RBT6 on-chip
Lua co-processor.

Supports live upload (< 50 ms) over either:
  - Monome FTDI Serial (/dev/cu.usbserial-m* or /dev/ttyUSB*)
  - Akai APC Mini USB-MIDI SysEx (09E8:0028)
"""

from __future__ import annotations

import argparse
import glob
import os
import struct
import sys
import time
from pathlib import Path
from typing import Optional

from luaparser import ast, astnodes

# Bytecode Header Constants
LUA_HEADER_SIZE = 16
LUA_MAX_IMAGE_LEN = 768
LUA_ENTRY_NONE = 0xFFFF

# Opcodes (must match src/protocol/lua_vm.h)
LUA_OP_HALT = 0x00
LUA_OP_RET = 0x01
LUA_OP_PUSH_I8 = 0x02
LUA_OP_PUSH_I16 = 0x03
LUA_OP_GET_LOCAL = 0x04
LUA_OP_SET_LOCAL = 0x05
LUA_OP_GET_GLOBAL = 0x06
LUA_OP_SET_GLOBAL = 0x07
LUA_OP_GET_MEM = 0x08
LUA_OP_SET_MEM = 0x09
LUA_OP_POP = 0x0A

LUA_OP_ADD = 0x10
LUA_OP_SUB = 0x11
LUA_OP_MUL = 0x12
LUA_OP_DIV = 0x13
LUA_OP_MOD = 0x14
LUA_OP_NEG = 0x15
LUA_OP_NOT = 0x16
LUA_OP_BAND = 0x17
LUA_OP_BOR = 0x18
LUA_OP_BXOR = 0x19
LUA_OP_SHL = 0x1A
LUA_OP_SHR = 0x1B

LUA_OP_EQ = 0x20
LUA_OP_NE = 0x21
LUA_OP_LT = 0x22
LUA_OP_LE = 0x23
LUA_OP_GT = 0x24
LUA_OP_GE = 0x25

LUA_OP_JMP = 0x30
LUA_OP_JZ = 0x31
LUA_OP_JNZ = 0x32

LUA_OP_CALL_BUILTIN = 0x40

# Builtin Function IDs (must match src/protocol/lua_vm.h)
BUILTINS: dict[str, tuple[int, int]] = {
    "led": (0x00, 3),
    "g:led": (0x00, 3),
    "g.led": (0x00, 3),
    "led_all": (0x01, 1),
    "g:all": (0x01, 1),
    "g.all": (0x01, 1),
    "grid_key": (0x02, 3),
    "trigger_key": (0x02, 3),
    "mext_key": (0x02, 3),
    "tilt": (0x03, 4),
    "mext_tilt": (0x03, 4),
    "note_on": (0x04, 3),
    "note_off": (0x05, 2),
    "cc": (0x06, 3),
    "fader": (0x07, 1),
    "shift": (0x08, 0),
    "set_tempo": (0x09, 1),
    "rand": (0x0A, 1),
    "math.random": (0x0A, 1),
    "abs": (0x0B, 1),
    "math.abs": (0x0B, 1),
    "min": (0x0C, 2),
    "math.min": (0x0C, 2),
    "max": (0x0D, 2),
    "math.max": (0x0D, 2),
    "host_led": (0x0E, 2),
    "life_step": (0x0F, 0),
    "yield": (0x10, 0),
    "sleep_ms": (0x11, 1),
    "key_pulse": (0x12, 3),
    "fader_hires": (0x13, 1),
    "fader14": (0x13, 1),
    "pitch_bend": (0x14, 2),
    "cc14": (0x15, 3),
}

# SysEx Subcommands
LUA_SYSEX_CMD_BEGIN = 0x01
LUA_SYSEX_CMD_CHUNK = 0x02
LUA_SYSEX_CMD_COMMIT = 0x03
LUA_SYSEX_CMD_SAVE = 0x04
LUA_SYSEX_CMD_RESET = 0x05
LUA_SYSEX_CMD_ENABLE = 0x06


def _resolve_dotted_name(node: astnodes.Node) -> str:
    if isinstance(node, astnodes.Name):
        return node.id
    if isinstance(node, astnodes.Index) and isinstance(node.idx, astnodes.Name):
        return f"{_resolve_dotted_name(node.value)}.{node.idx.id}"
    raise SyntaxError(f"Unsupported name node: {type(node).__name__}")


class LuaASTCompiler:
    def __init__(self) -> None:
        self.code = bytearray()
        self.globals: dict[str, int] = {}
        self.locals: dict[str, int] = {}
        self.next_local_slot = 0
        self.entries: dict[str, int] = {
            "init": LUA_ENTRY_NONE,
            "on_tick": LUA_ENTRY_NONE,
            "on_grid": LUA_ENTRY_NONE,
            "on_fader": LUA_ENTRY_NONE,
        }

    def emit(self, *bs: int) -> int:
        pos = len(self.code)
        for b in bs:
            self.code.append(b & 0xFF)
        return pos

    def emit_push_int(self, val: int) -> None:
        if -128 <= val <= 127:
            self.emit(LUA_OP_PUSH_I8, val & 0xFF)
        else:
            clamped = max(-32768, min(32767, val))
            u16 = clamped & 0xFFFF
            self.emit(LUA_OP_PUSH_I16, u16 & 0xFF, (u16 >> 8) & 0xFF)

    def emit_jump(self, op: int, target: int = 0) -> int:
        pos = len(self.code)
        self.emit(op, target & 0xFF, (target >> 8) & 0xFF)
        return pos

    def patch_jump(self, jmp_pos: int, target: Optional[int] = None) -> None:
        if target is None:
            target = len(self.code)
        self.code[jmp_pos + 1] = target & 0xFF
        self.code[jmp_pos + 2] = (target >> 8) & 0xFF

    def get_global_slot(self, name: str) -> int:
        if name not in self.globals:
            if len(self.globals) >= 64:
                raise SyntaxError(f"Too many global variables (max 64) at {name!r}")
            self.globals[name] = len(self.globals)
        return self.globals[name]

    def alloc_local_slot(self, name: str) -> int:
        if name in self.locals:
            return self.locals[name]
        if self.next_local_slot >= 32:
            raise SyntaxError(f"Too many local variables in function (max 32) at {name!r}")
        slot = self.next_local_slot
        self.locals[name] = slot
        self.next_local_slot += 1
        return slot

    def compile_chunk(self, chunk: astnodes.Chunk) -> bytes:
        stmts = chunk.body.body
        for stmt in stmts:
            if isinstance(stmt, astnodes.SemiColon):
                continue
            if isinstance(stmt, astnodes.Function):
                self.compile_function(stmt)
            else:
                if self.entries["init"] == LUA_ENTRY_NONE:
                    self.entries["init"] = len(self.code)
                self.compile_stmt(stmt)

        if len(self.code) == 0 or self.code[-1] not in (LUA_OP_HALT, LUA_OP_RET):
            self.emit(LUA_OP_HALT)

        header = struct.pack(
            "<4sHHHHHH",
            b"LUA1",
            len(self.code),
            self.entries["init"],
            self.entries["on_tick"],
            self.entries["on_grid"],
            self.entries["on_fader"],
            0,
        )
        image = header + bytes(self.code)
        if len(image) > LUA_MAX_IMAGE_LEN:
            raise ValueError(
                f"Compiled bytecode image ({len(image)} B) exceeds limit ({LUA_MAX_IMAGE_LEN} B)"
            )
        return image

    def compile_function(self, fn: astnodes.Function) -> None:
        raw_name = _resolve_dotted_name(fn.name)
        norm_name = {
            "init": "init",
            "on_tick": "on_tick",
            "tick": "on_tick",
            "on_grid": "on_grid",
            "g.key": "on_grid",
            "grid_key": "on_grid",
            "on_fader": "on_fader",
            "fader_move": "on_fader",
        }.get(raw_name)
        if norm_name is None:
            raise SyntaxError(
                f"Unsupported entry function {raw_name!r}; expected init, on_tick, on_grid, or on_fader"
            )

        self.locals.clear()
        self.next_local_slot = 0
        for arg in fn.args:
            if isinstance(arg, astnodes.Name):
                self.alloc_local_slot(arg.id)

        self.entries[norm_name] = len(self.code)
        self.compile_block(fn.body)
        self.emit(LUA_OP_HALT)

    def compile_block(self, block: astnodes.Block) -> None:
        for stmt in block.body:
            if not isinstance(stmt, astnodes.SemiColon):
                self.compile_stmt(stmt)

    def compile_stmt(self, stmt: astnodes.Node) -> None:
        if isinstance(stmt, astnodes.LocalAssign):
            for i, target in enumerate(stmt.targets):
                if not isinstance(target, astnodes.Name):
                    raise SyntaxError("Local assignment target must be variable name")
                if i < len(stmt.values):
                    self.compile_expr(stmt.values[i])
                else:
                    self.emit_push_int(0)
                slot = self.alloc_local_slot(target.id)
                self.emit(LUA_OP_SET_LOCAL, slot)
            return

        if isinstance(stmt, astnodes.Assign):
            for i, target in enumerate(stmt.targets):
                val_node = stmt.values[i] if i < len(stmt.values) else astnodes.Number(0)
                if isinstance(target, astnodes.Name):
                    self.compile_expr(val_node)
                    if target.id in self.locals:
                        self.emit(LUA_OP_SET_LOCAL, self.locals[target.id])
                    else:
                        self.emit(LUA_OP_SET_GLOBAL, self.get_global_slot(target.id))
                elif isinstance(target, astnodes.Index):
                    self._compile_mem_index(target)
                    self.compile_expr(val_node)
                    self.emit(LUA_OP_SET_MEM)
                else:
                    raise SyntaxError(f"Unsupported assignment target: {type(target).__name__}")
            return

        if isinstance(stmt, astnodes.If):
            self.compile_if(stmt)
            return

        if isinstance(stmt, astnodes.While):
            loop_start = len(self.code)
            self.compile_expr(stmt.test)
            jz_exit = self.emit_jump(LUA_OP_JZ)
            self.compile_block(stmt.body)
            self.emit_jump(LUA_OP_JMP, loop_start)
            self.patch_jump(jz_exit)
            return

        if isinstance(stmt, astnodes.Fornum):
            var_name = stmt.target.id
            var_slot = self.alloc_local_slot(var_name)
            limit_slot = self.alloc_local_slot(f"__lim_{var_name}")
            self.compile_expr(stmt.start)
            self.emit(LUA_OP_SET_LOCAL, var_slot)
            self.compile_expr(stmt.stop)
            self.emit(LUA_OP_SET_LOCAL, limit_slot)

            step_val = 1
            if stmt.step is not None:
                if isinstance(stmt.step, int):
                    step_val = stmt.step
                elif isinstance(stmt.step, astnodes.Number):
                    step_val = int(stmt.step.n)
                elif isinstance(stmt.step, astnodes.UMinusOp) and isinstance(
                    stmt.step.operand, astnodes.Number
                ):
                    step_val = -int(stmt.step.operand.n)
                else:
                    raise SyntaxError("For-loop step must be an integer constant")

            loop_start = len(self.code)
            self.emit(LUA_OP_GET_LOCAL, var_slot)
            self.emit(LUA_OP_GET_LOCAL, limit_slot)
            self.emit(LUA_OP_LE if step_val >= 0 else LUA_OP_GE)
            jz_exit = self.emit_jump(LUA_OP_JZ)

            self.compile_block(stmt.body)

            self.emit(LUA_OP_GET_LOCAL, var_slot)
            self.emit_push_int(step_val)
            self.emit(LUA_OP_ADD)
            self.emit(LUA_OP_SET_LOCAL, var_slot)
            self.emit_jump(LUA_OP_JMP, loop_start)
            self.patch_jump(jz_exit)
            return

        if isinstance(stmt, astnodes.Return):
            if stmt.values:
                self.compile_expr(stmt.values[0])
            else:
                self.emit_push_int(0)
            self.emit(LUA_OP_RET)
            return

        if isinstance(stmt, (astnodes.Call, astnodes.Invoke)):
            if self._is_noop_refresh(stmt):
                return
            self.compile_call(stmt)
            self.emit(LUA_OP_POP)
            return

        raise SyntaxError(f"Unsupported statement node: {type(stmt).__name__}")

    def compile_if(self, node: astnodes.If) -> None:
        self.compile_expr(node.test)
        jz_next = self.emit_jump(LUA_OP_JZ)
        end_jumps: list[int] = []

        self.compile_block(node.body)

        orelse = node.orelse
        while isinstance(orelse, astnodes.ElseIf):
            end_jumps.append(self.emit_jump(LUA_OP_JMP))
            self.patch_jump(jz_next)
            self.compile_expr(orelse.test)
            jz_next = self.emit_jump(LUA_OP_JZ)
            self.compile_block(orelse.body)
            orelse = orelse.orelse

        if isinstance(orelse, astnodes.Block):
            end_jumps.append(self.emit_jump(LUA_OP_JMP))
            self.patch_jump(jz_next)
            jz_next = -1
            self.compile_block(orelse)
        elif isinstance(orelse, list):
            end_jumps.append(self.emit_jump(LUA_OP_JMP))
            self.patch_jump(jz_next)
            jz_next = -1
            for s in orelse:
                if not isinstance(s, astnodes.SemiColon):
                    self.compile_stmt(s)

        if jz_next >= 0:
            self.patch_jump(jz_next)
        for j in end_jumps:
            self.patch_jump(j)

    def _compile_mem_index(self, idx_node: astnodes.Index) -> None:
        if isinstance(idx_node.value, astnodes.Index):
            # 2D sugar: grid[y][x] -> (y) * 8 + (x)
            self.compile_expr(idx_node.value.idx)
            self.emit_push_int(8)
            self.emit(LUA_OP_MUL)
            self.compile_expr(idx_node.idx)
            self.emit(LUA_OP_ADD)
        else:
            self.compile_expr(idx_node.idx)

    def _is_noop_refresh(self, node: astnodes.Node) -> bool:
        if isinstance(node, astnodes.Invoke):
            name = f"{_resolve_dotted_name(node.source)}:{node.func.id}"
            return name == "g:refresh"
        if isinstance(node, astnodes.Call):
            name = _resolve_dotted_name(node.func)
            return name in ("g.refresh", "refresh")
        return False

    def compile_call(self, node: astnodes.Call | astnodes.Invoke) -> None:
        if isinstance(node, astnodes.Invoke):
            fn_name = f"{_resolve_dotted_name(node.source)}:{node.func.id}"
        else:
            fn_name = _resolve_dotted_name(node.func)

        if fn_name not in BUILTINS:
            raise SyntaxError(f"Unknown built-in function {fn_name!r}")
        fn_id, expected_argc = BUILTINS[fn_name]
        if len(node.args) != expected_argc:
            raise SyntaxError(
                f"Function {fn_name}() expects {expected_argc} arguments, got {len(node.args)}"
            )
        for arg in node.args:
            self.compile_expr(arg)
        self.emit(LUA_OP_CALL_BUILTIN, fn_id, expected_argc)

    def compile_expr(self, expr: astnodes.Node) -> None:
        if isinstance(expr, astnodes.Number):
            self.emit_push_int(int(expr.n))
            return

        if isinstance(expr, astnodes.TrueExpr):
            self.emit_push_int(1)
            return

        if isinstance(expr, (astnodes.FalseExpr, astnodes.Nil)):
            self.emit_push_int(0)
            return

        if isinstance(expr, astnodes.Name):
            if expr.id in self.locals:
                self.emit(LUA_OP_GET_LOCAL, self.locals[expr.id])
            else:
                self.emit(LUA_OP_GET_GLOBAL, self.get_global_slot(expr.id))
            return

        if isinstance(expr, astnodes.Index):
            self._compile_mem_index(expr)
            self.emit(LUA_OP_GET_MEM)
            return

        if isinstance(expr, (astnodes.Call, astnodes.Invoke)):
            self.compile_call(expr)
            return

        if isinstance(expr, astnodes.UMinusOp):
            if isinstance(expr.operand, astnodes.Number):
                self.emit_push_int(-int(expr.operand.n))
            else:
                self.compile_expr(expr.operand)
                self.emit(LUA_OP_NEG)
            return

        if isinstance(expr, astnodes.ULNotOp):
            self.compile_expr(expr.operand)
            self.emit(LUA_OP_NOT)
            return

        if isinstance(expr, astnodes.UBNotOp):
            self.compile_expr(expr.operand)
            self.emit_push_int(-1)
            self.emit(LUA_OP_BXOR)
            return

        if isinstance(expr, astnodes.AndLoOp):
            self.compile_expr(expr.left)
            self.compile_expr(expr.right)
            self.emit_push_int(0)
            self.emit(LUA_OP_NE)
            self.emit(LUA_OP_BAND)
            return

        if isinstance(expr, astnodes.OrLoOp):
            self.compile_expr(expr.left)
            self.compile_expr(expr.right)
            self.emit(LUA_OP_BOR)
            return

        bin_ops: dict[type, int] = {
            astnodes.AddOp: LUA_OP_ADD,
            astnodes.SubOp: LUA_OP_SUB,
            astnodes.MultOp: LUA_OP_MUL,
            astnodes.FloatDivOp: LUA_OP_DIV,
            astnodes.FloorDivOp: LUA_OP_DIV,
            astnodes.ModOp: LUA_OP_MOD,
            astnodes.BAndOp: LUA_OP_BAND,
            astnodes.BOrOp: LUA_OP_BOR,
            astnodes.BXorOp: LUA_OP_BXOR,
            astnodes.BShiftLOp: LUA_OP_SHL,
            astnodes.BShiftROp: LUA_OP_SHR,
            astnodes.EqToOp: LUA_OP_EQ,
            astnodes.NotEqToOp: LUA_OP_NE,
            astnodes.LessThanOp: LUA_OP_LT,
            astnodes.LessOrEqThanOp: LUA_OP_LE,
            astnodes.GreaterThanOp: LUA_OP_GT,
            astnodes.GreaterOrEqThanOp: LUA_OP_GE,
        }
        for cls, opcode in bin_ops.items():
            if isinstance(expr, cls):
                self.compile_expr(expr.left)
                self.compile_expr(expr.right)
                self.emit(opcode)
                return

        raise SyntaxError(f"Unsupported expression node: {type(expr).__name__}")


def compile_lua_source(src: str) -> bytes:
    chunk = ast.parse(src)
    compiler = LuaASTCompiler()
    return compiler.compile_chunk(chunk)


def build_upload_sysex_packets(
    image: bytes, save_to_flash: bool = False
) -> list[list[int]]:
    """Encodes a compiled LUA1 bytecode image into < 48-byte SysEx frames."""
    packets: list[list[int]] = []
    total_len = len(image)
    packets.append(
        [
            0xF0,
            0x7D,
            0x4C,
            0x55,
            0x41,
            LUA_SYSEX_CMD_BEGIN,
            total_len & 0x7F,
            (total_len >> 7) & 0x7F,
            0xF7,
        ]
    )

    chunk_size = 16
    for off in range(0, total_len, chunk_size):
        chunk = image[off : off + chunk_size]
        nibbles: list[int] = []
        for b in chunk:
            nibbles.append((b >> 4) & 0x0F)
            nibbles.append(b & 0x0F)
        packets.append(
            [
                0xF0,
                0x7D,
                0x4C,
                0x55,
                0x41,
                LUA_SYSEX_CMD_CHUNK,
                off & 0x7F,
                (off >> 7) & 0x7F,
                *nibbles,
                0xF7,
            ]
        )

    xor_csum = 0
    for b in image:
        xor_csum ^= b
    packets.append(
        [
            0xF0,
            0x7D,
            0x4C,
            0x55,
            0x41,
            LUA_SYSEX_CMD_COMMIT,
            (xor_csum >> 4) & 0x0F,
            xor_csum & 0x0F,
            0xF7,
        ]
    )

    if save_to_flash:
        packets.append([0xF0, 0x7D, 0x4C, 0x55, 0x41, LUA_SYSEX_CMD_SAVE, 0xF7])

    return packets


def send_packets_to_hardware(packets: list[list[int]]) -> str:
    """Sends Lua SysEx frames over APC/monome USB-MIDI, Monome FTDI serial, or serialoscd UDP OSC."""
    try:
        import mido

        out_names = mido.get_output_names()
        target = next(
            (n for n in out_names if "APC MINI" in n.upper() or "MONOME" in n.upper()),
            None,
        )
        if target is not None:
            with mido.open_output(target) as outport:
                for pkt in packets:
                    outport.send(mido.Message("sysex", data=pkt[1:-1]))
                    time.sleep(0.004)
            return f"USB-MIDI ({target})"
    except Exception:
        pass

    serial_devs = sorted(
        glob.glob("/dev/cu.usbserial-m*")
        + glob.glob("/dev/tty.usbserial-m*")
        + glob.glob("/dev/ttyUSB*")
    )
    for dev in serial_devs:
        try:
            fd = os.open(dev, os.O_WRONLY | os.O_NOCTTY | os.O_NONBLOCK)
            try:
                for pkt in packets:
                    os.write(fd, bytes(pkt))
                    time.sleep(0.004)
            finally:
                os.close(fd)
            return f"Monome FTDI serial ({dev})"
        except OSError:
            pass

    from tools.serialosc_osc import serialosc_send_sysex

    osc_via = serialosc_send_sysex(packets)
    if osc_via is not None:
        return osc_via

    raise RuntimeError(
        "No APC Mini found in USB-MIDI ('APC MINI' / 'monome 64'), "
        "Monome FTDI serial (/dev/cu.usbserial-m*), or serialoscd UDP OSC (127.0.0.1:12002)"
    )


def format_c_array(image: bytes, var_name: str = "k_rom_script") -> str:
    lines = [f"static const uint8_t {var_name}[{len(image)}] = {{"]
    for off in range(0, len(image), 12):
        chunk = ", ".join(f"0x{b:02X}U" for b in image[off : off + 12])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Compile & upload Lua scripts to Open APC Mini")
    parser.add_argument("script", nargs="?", type=Path, help="Path to .lua script file")
    parser.add_argument("-o", "--output", type=Path, help="Write compiled .luac binary to file")
    parser.add_argument("--emit-c", action="store_true", help="Print C byte array for ROM embedding")
    parser.add_argument("--upload", action="store_true", help="Upload live to connected APC Mini")
    parser.add_argument("--save", action="store_true", help="Persist uploaded script to Flash 0x08008000")
    parser.add_argument("--reset", action="store_true", help="Reset to built-in ROM script and erase Flash")
    parser.add_argument("--enable", type=int, choices=[0, 1], help="Enable (1) or disable (0) Lua Co-Processor")
    args = parser.parse_args()

    if args.reset:
        pkts = [[0xF0, 0x7D, 0x4C, 0x55, 0x41, LUA_SYSEX_CMD_RESET, 0xF7]]
        via = send_packets_to_hardware(pkts)
        print(f"Reset Lua Co-Processor to built-in ROM script via {via}")
        return 0

    if args.enable is not None and args.script is None:
        pkts = [[0xF0, 0x7D, 0x4C, 0x55, 0x41, LUA_SYSEX_CMD_ENABLE, args.enable & 1, 0xF7]]
        via = send_packets_to_hardware(pkts)
        print(f"Set Lua Co-Processor enabled={args.enable} via {via}")
        return 0

    if args.script is None:
        parser.error("Provide a .lua script path (or --reset / --enable)")

    src = args.script.read_text(encoding="utf-8")
    image = compile_lua_source(src)
    _, code_len, e_init, e_tick, e_grid, e_fader, _ = struct.unpack("<4sHHHHHH", image[:16])

    if args.emit_c:
        print(format_c_array(image))
        return 0

    print(
        f"Compiled {args.script.name}: {len(image)} B total ({code_len} B bytecode) | "
        f"entries: init=0x{e_init:04X} tick=0x{e_tick:04X} grid=0x{e_grid:04X} fader=0x{e_fader:04X}"
    )

    if args.output:
        args.output.write_bytes(image)
        print(f"Wrote {args.output}")

    if args.upload or args.save:
        pkts = build_upload_sysex_packets(image, save_to_flash=args.save)
        via = send_packets_to_hardware(pkts)
        mode_str = "RAM + Flash (0x08008000)" if args.save else "RAM (live)"
        print(f"Uploaded {len(image)} B ({len(pkts)} SysEx frames) to {mode_str} via {via}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
