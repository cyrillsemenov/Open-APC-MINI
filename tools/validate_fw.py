"""Unicorn Cortex-M3 hardware validator for Akai APC Mini (STM32F102RBT6) firmware."""

import argparse
import struct
import sys
from pathlib import Path

from unicorn import (
    UC_ARCH_ARM,
    UC_HOOK_CODE,
    UC_HOOK_MEM_WRITE,
    UC_MODE_MCLASS,
    UC_MODE_THUMB,
)
from unicorn import unicorn as u
from unicorn.arm_const import (
    UC_ARM_REG_APSR,
    UC_ARM_REG_LR,
    UC_ARM_REG_MSP,
    UC_ARM_REG_PC,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R2,
    UC_ARM_REG_R3,
    UC_ARM_REG_R12,
    UC_ARM_REG_SP,
    UC_ARM_REG_XPSR,
    UC_CPU_ARM_CORTEX_M3,
)

# STM32F102RBT6 Memory Map
FLASH_BASE = 0x08000000
FLASH_SIZE = 0x00020000  # 128 KB
APP_BASE = 0x08002000
APP_SIZE = 0x00005800  # 22,528 B (0x08002000..0x080077FF)
APP_CHK_OFF = 0x000057FE
BOOT_MAGIC_ADDR = 0x08007C00
BOOT_MAGIC = bytes([0x55, 0x5A, 0xA5, 0xAA, 0x59, 0x95, 0x3A, 0xA3])

SRAM_BASE = 0x20000000
SRAM_SIZE = 0x00004000  # 16 KB
PMA_BASE = 0x40006000
IRQ_RET_ADDR = 0x0801FFF0

# Exception / IRQ vector indices
VEC_SYSTICK = 15
VEC_USB_LP_IRQ = 36  # 16 + USB_LP_CAN1_RX0_IRQn (20)


class ValidationError(Exception):
    """Raised when a firmware hardware or protocol check fails."""


class STM32F102Harness:
    """Minimal STM32F102RBT6 + APC Mini board peripheral model on Unicorn."""

    def __init__(self, app_bytes: bytes) -> None:
        self.app_bytes = app_bytes
        self.uc = u.Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M3)

        # Peripheral register backing store & hardware state
        self.regs: dict[int, int] = {
            0x40021000: 0x00000083,  # RCC_CR (HSION | HSIRDY)
            0x40022010: 0x00000080,  # FLASH_CR (LOCK)
            0x40005C40: 0x00000003,  # USB_CNTR (FRES | PDWN)
        }
        for port in range(4):
            base = 0x40010800 + port * 0x400
            self.regs[base + 0x00] = 0x44444444  # CRL
            self.regs[base + 0x04] = 0x44444444  # CRH

        self.gpio_odr = [0, 0, 0, 0]  # GPIOA..GPIOD
        self.pressed_buttons: set[tuple[int, int]] = set()  # (col 0..10, row 0..7)
        self.fader_adc = [0] * 9
        self.usb_epr = [0] * 8
        self.usb_istr = 0
        self.ep1_tx_polls = 0
        self.ep1_in_packets: list[bytes] = []
        self.pa8_falls = 0

        # Watchdog, Flash, Reset & LED tracking
        self.iwdg_unlocked = False
        self.iwdg_started = False
        self.iwdg_reloads = 0
        self.flash_key_step = 0
        self.flash_unlocked = False
        self.illegal_flash_write: int | None = None
        self.fault_pc: int | None = None
        self.allow_fault_recovery = False
        self.reset_requested = False

        # TM1629 serial bit-bang state (PB12=STB, PB13=CLK, PB14=DIO)
        self._tm_cur = bytearray()
        self._tm_byte = 0
        self._tm_bits = 0
        self.tm_frames: list[bytes] = []
        self.tm_display: bytes | None = None
        self.round_led_sinks = [0x0F, 0x0F, 0x0F, 0x0F]

        self._map_memory()

    def _map_memory(self) -> None:
        # Flash (128 KB, erased to 0xFF) + SRAM (16 KB) + USB PMA (4 KB page)
        self.uc.mem_map(FLASH_BASE, FLASH_SIZE)
        self.uc.mem_write(FLASH_BASE, b"\xff" * FLASH_SIZE)
        self.uc.mem_write(APP_BASE, self.app_bytes)
        self.uc.mem_write(IRQ_RET_ADDR, b"\x00\xbf\xfe\xe7")  # nop; b .

        self.uc.mem_map(SRAM_BASE, SRAM_SIZE)
        self.uc.mem_map(PMA_BASE, 0x1000)

        # STM32 System Memory / Electronic Signature (DESIG_FLASH_SIZE = 128 KB at 0x1FFFF7E0, UID0 at 0x1FFFF7E8)
        self.uc.mem_map(0x1FFFF000, 0x1000)
        self.uc.mem_write(0x1FFFF7E0, struct.pack("<H", 128))
        self.uc.mem_write(0x1FFFF7E8, struct.pack("<I", 0x10016410))

        # APB1 (before PMA), APB1/APB2/AHB (after PMA), Bit-Band alias, and Cortex-M3 PPB
        self.uc.mmio_map(
            0x40000000,
            0x6000,
            lambda uc, off, sz, d: self._periph_read(0x40000000 + off),
            None,
            lambda uc, off, sz, val, d: self._periph_write(0x40000000 + off, val),
            None,
        )
        self.uc.mmio_map(
            0x40007000,
            0x1D000,
            lambda uc, off, sz, d: self._periph_read(0x40007000 + off),
            None,
            lambda uc, off, sz, val, d: self._periph_write(0x40007000 + off, val),
            None,
        )
        self.uc.mmio_map(
            0x42000000, 0x480000, self._bitband_read, None, self._bitband_write, None
        )
        self.uc.mmio_map(
            0xE000E000, 0x1000, self._ppb_read, None, self._ppb_write, None
        )

        # Guard Flash against unlocked/unprogrammed writes
        self.uc.hook_add(
            UC_HOOK_MEM_WRITE,
            self._on_flash_write,
            begin=FLASH_BASE,
            end=FLASH_BASE + FLASH_SIZE - 1,
        )

        # Catch jumps into HardFault / MemManage / BusFault / UsageFault vectors (3..6)
        fault_addrs = {
            struct.unpack_from("<I", self.app_bytes, v * 4)[0] & ~1
            for v in (3, 4, 5, 6)
        }
        fault_addrs.discard(0)
        for addr in fault_addrs:
            if FLASH_BASE <= addr < FLASH_BASE + FLASH_SIZE:
                self.uc.hook_add(UC_HOOK_CODE, self._on_fault, begin=addr, end=addr + 2)

    def _on_flash_write(
        self, uc: u.Uc, access: int, addr: int, size: int, val: int, data: object
    ) -> None:
        cr_pg = (self.regs.get(0x40022010, 0x80) & 0x01) != 0
        if not (self.flash_unlocked and cr_pg):
            self.illegal_flash_write = addr
            uc.emu_stop()
        else:
            self.regs[0x4002200C] = self.regs.get(0x4002200C, 0) | 0x20  # FLASH_SR_EOP

    def _on_fault(self, uc: u.Uc, addr: int, size: int, data: object) -> None:
        if self.allow_fault_recovery:
            return
        self.fault_pc = addr
        uc.emu_stop()

    def _periph_read(self, addr: int) -> int:
        # USB Endpoint registers EP0R..EP7R
        if 0x40005C00 <= addr <= 0x40005C1C:
            ep = (addr - 0x40005C00) // 4
            if ep == 1 and (self.usb_epr[1] & 0x0030) == 0x0030:
                self.ep1_tx_polls += 1
                if self.ep1_tx_polls >= 3:
                    pkt = self._harvest_ep_tx(1)
                    if pkt is not None:
                        self.ep1_in_packets.append(pkt)
            return self.usb_epr[ep]

        # USB ISTR (priority-encoded CTR/DIR/EP_ID from EPnR)
        if addr == 0x40005C44:
            istr = self.usb_istr & 0x7F00
            for ep in range(8):
                epr = self.usb_epr[ep]
                if epr & 0x8000:  # EP_CTR_RX
                    return istr | 0x8010 | ep
                if epr & 0x0080:  # EP_CTR_TX
                    return istr | 0x8000 | ep
            return istr

        # GPIOA..GPIOD IDR / ODR
        if 0x40010800 <= addr < 0x40011800:
            port = (addr - 0x40010800) // 0x400
            reg = addr & 0x3FF
            if reg == 0x08:  # IDR
                if port == 1:  # GPIOB button matrix rows (PB0..PB7)
                    idr = 0xFFFF
                    pc_odr = self.gpio_odr[2]
                    for col, row in self.pressed_buttons:
                        if (pc_odr & (1 << (col + 5))) == 0:
                            idr &= ~(1 << row)
                    return idr
                return self.gpio_odr[port]
            if reg == 0x0C:  # ODR
                return self.gpio_odr[port]

        return self.regs.get(addr, 0)

    def _periph_write(self, addr: int, val: int) -> None:
        # IWDG
        if addr == 0x40003000:
            if val == 0x5555:
                self.iwdg_unlocked = True
            elif val == 0xCCCC:
                self.iwdg_started = True
            elif val == 0xAAAA:
                self.iwdg_reloads += 1
            return

        # USB Endpoint registers EP0R..EP7R (STM32 toggle / rc_w0 hardware rules)
        if 0x40005C00 <= addr <= 0x40005C1C:
            ep = (addr - 0x40005C00) // 4
            cur = self.usb_epr[ep]
            nxt = ((cur & ~0x070F) | (val & 0x070F)) ^ (val & 0x7070)
            nxt &= val | ~0x8080
            self.usb_epr[ep] = nxt & 0xFFFF
            if ep == 1:
                self.ep1_tx_polls = 0
            return

        # USB ISTR (rc_w0)
        if addr == 0x40005C44:
            self.usb_istr &= val | 0x001F
            return

        # GPIOA..GPIOD ODR / BSRR / BRR
        if 0x40010800 <= addr < 0x40011800:
            port = (addr - 0x40010800) // 0x400
            reg = addr & 0x3FF
            if reg == 0x0C:  # ODR
                self._update_gpio_odr(port, val & 0xFFFF)
                return
            if reg == 0x10:  # BSRR
                cur = self.gpio_odr[port]
                self._update_gpio_odr(
                    port, (cur | (val & 0xFFFF)) & ~((val >> 16) & 0xFFFF)
                )
                return
            if reg == 0x14:  # BRR
                self._update_gpio_odr(port, self.gpio_odr[port] & ~(val & 0xFFFF))
                return

        # ADC1 / ADC2 CR2 (auto-complete calibration; trigger DMA1 on SWSTART)
        if addr in (0x40012408, 0x40012808):
            clean = val & ~(
                (1 << 3) | (1 << 2) | (1 << 22)
            )  # clear RSTCAL, CAL, SWSTART
            self.regs[addr] = clean
            if addr == 0x40012408 and (val & (1 << 22)) and (val & 1):
                self._run_adc1_dma()
            return

        # DMA1 IFCR (write-1-to-clear DMA1 ISR)
        if addr == 0x40020004:
            isr = self.regs.get(0x40020000, 0)
            if val & 0x01:
                isr &= ~0x0F
            isr &= ~val
            self.regs[0x40020000] = isr
            return

        # RCC CR & CFGR (instant HSI/HSE/PLL lock + SWS mirror)
        if addr == 0x40021000:
            if val & 0x01:
                val |= 0x02  # HSIRDY
            if val & (1 << 16):
                val |= 1 << 17  # HSERDY
            if val & (1 << 24):
                val |= 1 << 25  # PLLRDY
            self.regs[addr] = val
            return
        if addr == 0x40021004:
            val = (val & ~0x0C) | ((val & 0x03) << 2)  # SWS = SW
            self.regs[addr] = val
            return

        # FLASH KEYR, SR, CR
        if addr == 0x40022004:
            if val == 0x45670123:
                self.flash_key_step = 1
            elif val == 0xCDEF89AB and self.flash_key_step == 1:
                self.flash_unlocked = True
                self.regs[0x40022010] = self.regs.get(0x40022010, 0) & ~0x80
            else:
                self.flash_key_step = 0
            return
        if addr == 0x4002200C:  # FLASH_SR (rc_w1)
            self.regs[addr] = self.regs.get(addr, 0) & ~val
            return
        if addr == 0x40022010:  # FLASH_CR
            if val & 0x80:
                self.flash_unlocked = False
                self.regs[addr] = val
                return
            if self.flash_unlocked:
                if (val & 0x42) == 0x42:  # PER | STRT -> erase 1 KB page
                    page = self.regs.get(0x40022014, 0) & ~0x3FF
                    if FLASH_BASE <= page <= FLASH_BASE + FLASH_SIZE - 0x400:
                        self.uc.mem_write(page, b"\xff" * 0x400)
                        self.regs[0x4002200C] = self.regs.get(0x4002200C, 0) | 0x20
                    val &= ~0x40
                self.regs[addr] = val
            return

        self.regs[addr] = val

    def _bitband_read(self, uc: u.Uc, offset: int, size: int, data: object) -> int:
        byte_off = offset >> 5
        bit = ((byte_off & 3) * 8) + ((offset >> 2) & 7)
        return (self._periph_read(0x40000000 + (byte_off & ~3)) >> bit) & 1

    def _bitband_write(
        self, uc: u.Uc, offset: int, size: int, val: int, data: object
    ) -> None:
        byte_off = offset >> 5
        bit = ((byte_off & 3) * 8) + ((offset >> 2) & 7)
        word_addr = 0x40000000 + (byte_off & ~3)
        cur = self._periph_read(word_addr)
        nxt = (cur | (1 << bit)) if (val & 1) else (cur & ~(1 << bit))
        self._periph_write(word_addr, nxt)

    def _ppb_read(self, uc: u.Uc, offset: int, size: int, data: object) -> int:
        return self.regs.get(0xE000E000 + offset, 0)

    def _ppb_write(
        self, uc: u.Uc, offset: int, size: int, val: int, data: object
    ) -> None:
        addr = 0xE000E000 + offset
        self.regs[addr] = val
        if addr == 0xE000ED0C and (val >> 16) == 0x05FA and (val & 0x04):
            self.reset_requested = True
            uc.emu_stop()

    def _update_gpio_odr(self, port: int, new_odr: int) -> None:
        old_odr = self.gpio_odr[port]
        self.gpio_odr[port] = new_odr

        if port == 0 and ((old_odr >> 8) & 1) == 1 and ((new_odr >> 8) & 1) == 0:
            self.pa8_falls += 1

        # Decode TM1629 on GPIOB (PB12=STB, PB13=CLK, PB14=DIO)
        if port == 1:
            old_stb = (old_odr >> 12) & 1
            new_stb = (new_odr >> 12) & 1
            old_clk = (old_odr >> 13) & 1
            new_clk = (new_odr >> 13) & 1

            if old_stb == 1 and new_stb == 0:
                self._tm_cur.clear()
                self._tm_byte = 0
                self._tm_bits = 0
            elif new_stb == 0 and old_clk == 0 and new_clk == 1:
                dio = (new_odr >> 14) & 1
                self._tm_byte |= dio << self._tm_bits
                self._tm_bits += 1
                if self._tm_bits == 8:
                    self._tm_cur.append(self._tm_byte)
                    self._tm_byte = 0
                    self._tm_bits = 0
            elif old_stb == 0 and new_stb == 1 and self._tm_cur:
                frame = bytes(self._tm_cur)
                self.tm_frames.append(frame)
                if len(frame) == 17 and frame[0] == 0xC0:
                    self.tm_display = frame[1:]
                self._tm_cur.clear()
                self._tm_bits = 0

        # Latch multiplexed round LEDs (PC0..3 row anode HIGH, PB8..11 active-low sinks)
        if port in (1, 2):
            rows = self.gpio_odr[2] & 0x0F
            if rows in (1, 2, 4, 8):
                r_idx = {1: 0, 2: 1, 4: 2, 8: 3}[rows]
                self.round_led_sinks[r_idx] = (self.gpio_odr[1] >> 8) & 0x0F

    def _run_adc1_dma(self) -> None:
        ccr = self.regs.get(0x40020008, 0)
        cndtr = self.regs.get(0x4002000C, 0)
        cpar = self.regs.get(0x40020010, 0)
        cmar = self.regs.get(0x40020014, 0)
        if (
            (ccr & 1)
            and cpar == 0x4001244C
            and cndtr >= 9
            and SRAM_BASE <= cmar <= SRAM_BASE + SRAM_SIZE - 18
        ):
            payload = struct.pack("<9H", *self.fader_adc)
            self.uc.mem_write(cmar, payload)
            self.regs[0x40020000] = self.regs.get(0x40020000, 0) | 0x03  # GIF1 | TCIF1

    # --- Execution & Interrupt Helpers ---

    def _check_cpu_ok(self) -> None:
        if self.illegal_flash_write is not None:
            raise ValidationError(
                f"Illegal unlocked/unprogrammed Flash write at 0x{self.illegal_flash_write:08X}"
            )
        if self.fault_pc is not None:
            raise ValidationError(f"CPU entered fault handler at 0x{self.fault_pc:08X}")

    def boot(self, max_insns: int = 80_000) -> None:
        sp, reset_pc = struct.unpack_from("<II", self.app_bytes, 0)
        self.uc.reg_write(UC_ARM_REG_MSP, sp)
        self.uc.reg_write(UC_ARM_REG_SP, sp)
        self.run_steps(max_insns, start_pc=reset_pc)

    def run_steps(self, count: int, start_pc: int | None = None) -> None:
        pc = start_pc if start_pc is not None else (self.uc.reg_read(UC_ARM_REG_PC) | 1)
        try:
            self.uc.emu_start(pc, IRQ_RET_ADDR, count=count)
        except u.UcError as exc:
            cur_pc = self.uc.reg_read(UC_ARM_REG_PC)
            cur_lr = self.uc.reg_read(UC_ARM_REG_LR)
            raise ValidationError(
                f"CPU exception ({exc}) at PC=0x{cur_pc:08X}, LR=0x{cur_lr:08X}"
            ) from exc
        self._check_cpu_ok()

    def call_isr(self, vec_num: int, max_insns: int = 5_000_000) -> None:
        vtor = self.regs.get(0xE000ED08, FLASH_BASE)
        if not (FLASH_BASE <= vtor <= FLASH_BASE + FLASH_SIZE - 0x100):
            raise ValidationError(f"Invalid SCB->VTOR: 0x{vtor:08X}")
        handler = struct.unpack("<I", self.uc.mem_read(vtor + vec_num * 4, 4))[0]
        if (handler & 1) == 0 or not (
            APP_BASE <= (handler & ~1) < APP_BASE + APP_CHK_OFF
        ):
            raise ValidationError(
                f"Vector #{vec_num} (VTOR=0x{vtor:08X}) points to invalid handler 0x{handler:08X}"
            )

        saved_regs = (
            UC_ARM_REG_R0,
            UC_ARM_REG_R1,
            UC_ARM_REG_R2,
            UC_ARM_REG_R3,
            UC_ARM_REG_R12,
            UC_ARM_REG_LR,
            UC_ARM_REG_PC,
            UC_ARM_REG_XPSR,
            UC_ARM_REG_APSR,
        )
        saved = [self.uc.reg_read(r) for r in saved_regs]
        self.uc.reg_write(UC_ARM_REG_LR, IRQ_RET_ADDR | 1)
        try:
            self.uc.emu_start(handler, IRQ_RET_ADDR, count=max_insns)
        except u.UcError as exc:
            cur_pc = self.uc.reg_read(UC_ARM_REG_PC)
            raise ValidationError(
                f"ISR #{vec_num} crashed ({exc}) at PC=0x{cur_pc:08X}"
            ) from exc
        self._check_cpu_ok()

        if not self.reset_requested:
            end_pc = self.uc.reg_read(UC_ARM_REG_PC) & ~1
            if end_pc != IRQ_RET_ADDR:
                raise ValidationError(
                    f"ISR #{vec_num} hung (PC=0x{end_pc:08X} after {max_insns} insns)"
                )
            for r, v in zip(saved_regs, saved):
                self.uc.reg_write(r, v)

    # --- USB PMA & Host Transaction Helpers ---

    def _pma_read(self, pma_off: int, length: int) -> bytes:
        out = bytearray()
        for i in range((length + 1) // 2):
            hw = struct.unpack(
                "<H", self.uc.mem_read(PMA_BASE + pma_off * 2 + i * 4, 2)
            )[0]
            out.append(hw & 0xFF)
            if len(out) < length:
                out.append((hw >> 8) & 0xFF)
        return bytes(out[:length])

    def _pma_write(self, pma_off: int, payload: bytes) -> None:
        padded = payload + (b"\x00" if len(payload) % 2 else b"")
        for i in range(0, len(padded), 2):
            hw = padded[i] | (padded[i + 1] << 8)
            self.uc.mem_write(PMA_BASE + pma_off * 2 + i * 2, struct.pack("<H", hw))

    def _btable_entry_cpu_addr(self, ep: int) -> int:
        btable = self.regs.get(0x40005C50, 0) & 0xFFF8
        return PMA_BASE + (btable + ep * 8) * 2

    def _harvest_ep_tx(self, ep: int) -> bytes | None:
        if (self.usb_epr[ep] & 0x0030) != 0x0030:  # EP_TX_VALID
            return None
        ent = self._btable_entry_cpu_addr(ep)
        tx_addr = struct.unpack("<H", self.uc.mem_read(ent + 0, 2))[0]
        tx_cnt = struct.unpack("<H", self.uc.mem_read(ent + 4, 2))[0] & 0x03FF
        pkt = self._pma_read(tx_addr, tx_cnt)
        self.usb_epr[ep] = (self.usb_epr[ep] & ~0x0030) | 0x0020  # STAT_TX -> NAK
        return pkt

    def usb_reset(self) -> None:
        self.usb_istr |= 0x0400  # ISTR_RESET
        self.call_isr(VEC_USB_LP_IRQ)

    def usb_ep0_setup(self, setup8: bytes, expect_in: bool = False) -> bytes:
        ent = self._btable_entry_cpu_addr(0)
        rx_addr = struct.unpack("<H", self.uc.mem_read(ent + 8, 2))[0]
        rx_cnt_hi = struct.unpack("<H", self.uc.mem_read(ent + 12, 2))[0] & 0xFC00
        self._pma_write(rx_addr, setup8)
        self.uc.mem_write(ent + 12, struct.pack("<H", rx_cnt_hi | len(setup8)))

        self.usb_epr[0] = (
            (self.usb_epr[0] & ~0x3000) | 0x2000
        ) | 0x8800  # RX_NAK | CTR_RX | SETUP
        self.call_isr(VEC_USB_LP_IRQ)

        data = b""
        if expect_in:
            pkt = self._harvest_ep_tx(0)
            if pkt is None:
                raise ValidationError("EP0 did not respond with IN data stage")
            data = pkt
            self.usb_epr[0] |= 0x0080  # EP_CTR_TX
            self.call_isr(VEC_USB_LP_IRQ)
            # Status OUT stage (0-byte)
            self.uc.mem_write(ent + 12, struct.pack("<H", rx_cnt_hi))
            self.usb_epr[0] = ((self.usb_epr[0] & ~0x3800) | 0x2000) | 0x8000
            self.call_isr(VEC_USB_LP_IRQ)
        else:
            # Status IN stage (0-byte ACK)
            pkt = self._harvest_ep_tx(0)
            if pkt is None:
                raise ValidationError("EP0 did not complete status IN stage")
            self.usb_epr[0] |= 0x0080
            self.call_isr(VEC_USB_LP_IRQ)
        return data

    def usb_ep_out(self, ep: int, payload: bytes) -> None:
        if (self.usb_epr[ep] & 0x3000) != 0x3000:
            raise ValidationError(
                f"EP{ep} RX is not VALID (EP{ep}R=0x{self.usb_epr[ep]:04X})"
            )
        ent = self._btable_entry_cpu_addr(ep)
        rx_addr = struct.unpack("<H", self.uc.mem_read(ent + 8, 2))[0]
        rx_cnt_hi = struct.unpack("<H", self.uc.mem_read(ent + 12, 2))[0] & 0xFC00
        self._pma_write(rx_addr, payload)
        self.uc.mem_write(ent + 12, struct.pack("<H", rx_cnt_hi | len(payload)))

        self.usb_epr[ep] = (
            (self.usb_epr[ep] & ~0x3800) | 0x2000
        ) | (0x8000 | ep)  # RX_NAK | CTR_RX
        self.call_isr(VEC_USB_LP_IRQ)

    def usb_ep1_out(self, payload: bytes) -> None:
        self.usb_ep_out(1, payload)

    def usb_poll_ep1_in(self) -> list[bytes]:
        out = list(self.ep1_in_packets)
        self.ep1_in_packets.clear()
        pkt = self._harvest_ep_tx(1)
        if pkt is not None:
            self.usb_epr[1] |= 0x0080  # EP_CTR_TX
            self.call_isr(VEC_USB_LP_IRQ)
            out.append(pkt)
        return out

    def usb_poll_midi_in(self) -> list[bytes]:
        pkt1 = self._harvest_ep_tx(1)
        if pkt1 is not None:
            self.usb_epr[1] |= 0x0080
            self.call_isr(VEC_USB_LP_IRQ)
        out: list[bytes] = []
        pkt3 = self._harvest_ep_tx(3)
        if pkt3 is not None:
            self.usb_epr[3] |= 0x0080
            self.call_isr(VEC_USB_LP_IRQ)
            out.append(pkt3)
        return out


def extract_usb_midi_sysex(packets: list[bytes]) -> bytes:
    """Reassembles MIDI SysEx bytes from 4-byte USB-MIDI Event Packets."""
    out = bytearray()
    for pkt in packets:
        for i in range(0, len(pkt), 4):
            cin = pkt[i] & 0x0F
            if cin == 0x04:
                out.extend(pkt[i + 1 : i + 4])
            elif cin in (0x05, 0x06, 0x07):
                out.extend(pkt[i + 1 : i + 1 + (cin - 4)])
                return bytes(out)
    return bytes(out)


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise ValidationError(msg)


def validate_firmware(fw_path: Path, verbose: bool = True) -> None:
    def ok(label: str, detail: str = "") -> None:
        if verbose:
            suffix = f" ({detail})" if detail else ""
            print(f"  [PASS] {label}{suffix}")

    check(fw_path.is_file(), f"Firmware file not found: {fw_path}")
    app = fw_path.read_bytes()

    # 1. Static Binary, Bootloader Checksum & Vector Table
    check(len(app) == APP_SIZE, f"Expected {APP_SIZE} B image, got {len(app)} B")
    sum16 = sum(app[:APP_CHK_OFF]) & 0xFFFF
    chk = struct.unpack_from("<H", app, APP_CHK_OFF)[0]
    check(
        ((sum16 + chk) & 0xFFFF) == 0,
        f"Bootloader checksum mismatch: (0x{sum16:04X} + 0x{chk:04X}) != 0",
    )

    sp, reset_vec = struct.unpack_from("<II", app, 0)
    check(
        SRAM_BASE + 0x200 <= sp <= SRAM_BASE + SRAM_SIZE and (sp & 3) == 0,
        f"Invalid initial MSP: 0x{sp:08X}",
    )
    for vec_idx, name in (
        (1, "Reset"),
        (3, "HardFault"),
        (VEC_SYSTICK, "SysTick"),
        (VEC_USB_LP_IRQ, "USB_LP_IRQ"),
    ):
        vec = struct.unpack_from("<I", app, vec_idx * 4)[0]
        check(
            (vec & 1) == 1 and APP_BASE <= (vec & ~1) < APP_BASE + APP_CHK_OFF,
            f"{name} vector (#{vec_idx}) invalid: 0x{vec:08X}",
        )
    ok("Bootloader contract & vector table", f"MSP=0x{sp:08X}, chk=0x{chk:04X}")

    # 2. Boot, Clock/PLL, VTOR & USB Pull-Up
    mcu = STM32F102Harness(app)
    mcu.boot(max_insns=80_000)

    cfgr = mcu.regs.get(0x40021004, 0)
    check((cfgr & 0x0C) == 0x08, f"SYSCLK not switched to PLL (RCC_CFGR=0x{cfgr:08X})")
    check(
        ((cfgr >> 18) & 0x0F) == 0x04 and (cfgr & (1 << 16)) != 0,
        "PLL not configured for HSE*6 (48 MHz)",
    )
    check(
        (mcu.regs.get(0x40022000, 0) & 0x07) == 2,
        "FLASH latency != 2 wait states for 48 MHz",
    )

    vtor = mcu.regs.get(0xE000ED08, 0)
    check(
        vtor == APP_BASE,
        f"SCB->VTOR not relocated to 0x{APP_BASE:08X} (got 0x{vtor:08X})",
    )

    nvic_iser0 = mcu.regs.get(0xE000E100, 0)
    check(
        (nvic_iser0 & (1 << 20)) != 0, "USB_LP_CAN1_RX0_IRQn (#20) not enabled in NVIC"
    )
    check((mcu.gpio_odr[0] & (1 << 8)) != 0, "PA8 (USB D+ pull-up) not driven HIGH")
    check(
        (mcu.regs.get(0x40005C40, 3) & 0x03) == 0, "USB transceiver still in PDWN/FRES"
    )
    ok("Clock & USB attach", "48 MHz PLL, FLASH WS=2, VTOR=0x08002000, PA8 HIGH")

    # 3. USB Enumeration (RESET + GET_DESCRIPTOR + SET_CONFIGURATION) & Main Loop Watchdog
    mcu.usb_reset()
    dev_desc = mcu.usb_ep0_setup(
        bytes([0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00]), expect_in=True
    )
    check(
        len(dev_desc) == 18
        and dev_desc[0] == 18
        and dev_desc[1] == 0x01
        and dev_desc[7] == 64,
        f"Invalid USB Device Descriptor: {dev_desc.hex()}",
    )
    vid, pid = struct.unpack_from("<HH", dev_desc, 8)

    mcu.usb_ep0_setup(
        bytes([0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00]), expect_in=False
    )
    check(
        (mcu.usb_epr[1] & 0x3000) == 0x3000,
        "EP1 Bulk OUT not armed after USB enumeration",
    )
    ok("USB enumeration", f"VID:PID={vid:04X}:{pid:04X}, EP0/EP1 configured")

    mcu.run_steps(80_000)
    systick_ctrl = mcu.regs.get(0xE000E010, 0)
    systick_load = mcu.regs.get(0xE000E014, 0)
    check(
        (systick_ctrl & 0x03) == 0x03 and systick_load > 0,
        "SysTick timer/IRQ not enabled",
    )
    check(mcu.iwdg_started, "IWDG watchdog was not started (0xCCCC)")
    reloads_before = mcu.iwdg_reloads
    mcu.run_steps(20_000)
    check(
        mcu.iwdg_reloads > reloads_before, "IWDG watchdog is not reloaded in main loop"
    )
    ok("Main loop & timers", f"SysTick LOAD={systick_load}, IWDG active & reloaded")

    # 4. Universal Device Inquiry (F0 7E 7F 06 01 F7 -> Identity Reply)
    mcu.usb_ep1_out(bytes([0x04, 0xF0, 0x7E, 0x7F, 0x07, 0x06, 0x01, 0xF7]))
    mcu.run_steps(25_000)
    id_sysex = extract_usb_midi_sysex(mcu.usb_poll_midi_in())
    check(
        len(id_sysex) == 35
        and id_sysex[:7] == bytes([0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x47, 0x28])
        and id_sysex[-1] == 0xF7,
        f"Bad SysEx Identity Reply ({len(id_sysex)} B): {id_sysex.hex(' ')}",
    )
    ok("SysEx Universal Device Inquiry", "35-byte Akai APC Mini Identity Reply")

    # 5. Ableton Live Intro / Fader Dump (F0 47 00 28 60 00 04 00 01 01 01 F7 -> 0x61 reply)
    mcu.usb_ep1_out(
        bytes(
            [
                0x04,
                0xF0,
                0x47,
                0x00,
                0x04,
                0x28,
                0x60,
                0x00,
                0x04,
                0x04,
                0x00,
                0x01,
                0x07,
                0x01,
                0x01,
                0xF7,
            ]
        )
    )
    mcu.run_steps(25_000)
    dump_sysex = extract_usb_midi_sysex(mcu.usb_poll_midi_in())
    check(
        len(dump_sysex) == 18
        and dump_sysex[:7] == bytes([0xF0, 0x47, 0x00, 0x28, 0x61, 0x00, 0x09])
        and dump_sysex[-1] == 0xF7,
        f"Bad Ableton 0x61 Fader Dump ({len(dump_sysex)} B): {dump_sysex.hex(' ')}",
    )
    ok("Ableton Live Intro (0x60 -> 0x61)", "9-fader initial state dump")

    # 6. Button Matrix Scan & Debounce -> MIDI Note-On & Note-Off
    # First settle baseline unpressed debounce state across 6 scans (8 SysTicks per scan)
    for _ in range(6):
        for _ in range(8):
            mcu.call_isr(VEC_SYSTICK)
        mcu.run_steps(15_000)
    mcu.usb_poll_midi_in()

    # Press Grid Pad (col=0, row=0 -> MIDI note 0x38) and Shift (col=10, row=0 -> MIDI note 0x62)
    mcu.pressed_buttons = {(0, 0), (10, 0)}
    rx_events: list[bytes] = []
    for _ in range(6):
        for _ in range(8):
            mcu.call_isr(VEC_SYSTICK)
        mcu.run_steps(15_000)
        rx_events.extend(mcu.usb_poll_midi_in())

    midi_words = [
        pkt[i : i + 4]
        for pkt in rx_events
        for i in range(0, len(pkt), 4)
        if pkt[i] != 0
    ]
    check(
        bytes([0x09, 0x90, 0x38, 0x7F]) in midi_words,
        f"Missing Pad Note-On (09 90 38 7F), got {[w.hex() for w in midi_words]}",
    )
    check(
        bytes([0x09, 0x90, 0x62, 0x7F]) in midi_words,
        f"Missing Shift Note-On (09 90 62 7F), got {[w.hex() for w in midi_words]}",
    )

    # Release buttons -> verify Note-Off
    mcu.pressed_buttons.clear()
    rx_events.clear()
    for _ in range(6):
        for _ in range(8):
            mcu.call_isr(VEC_SYSTICK)
        mcu.run_steps(15_000)
        rx_events.extend(mcu.usb_poll_midi_in())

    midi_words = [
        pkt[i : i + 4]
        for pkt in rx_events
        for i in range(0, len(pkt), 4)
        if pkt[i] != 0
    ]
    check(
        bytes([0x08, 0x80, 0x38, 0x7F]) in midi_words,
        "Missing Pad Note-Off (08 80 38 7F)",
    )
    check(
        bytes([0x08, 0x80, 0x62, 0x7F]) in midi_words,
        "Missing Shift Note-Off (08 80 62 7F)",
    )
    ok("Button matrix scan & debounce", "Note-On/Off for Grid Pad 0x38 & Shift 0x62")

    # 7. ADC1 + DMA1 Fader Pipeline -> MIDI CC (realistic smooth potentiometer sweep)
    cc_events: list[bytes] = []
    for i in range(
        1, 91
    ):  # 90 samples (30 x 3-sample windows), +46 counts/step -> 4095
        mcu.fader_adc[0] = min(4095, i * 46)
        mcu.fader_adc[8] = min(2048, i * 26)
        mcu._run_adc1_dma()
        mcu.run_steps(10_000)
        cc_events.extend(mcu.usb_poll_midi_in())

    cc_map = {
        pkt[i + 2]: pkt[i + 3]
        for pkt in cc_events
        for i in range(0, len(pkt), 4)
        if pkt[i : i + 2] == bytes([0x0B, 0xB0])
    }
    check(cc_map.get(0x30) == 127, f"Expected Fader 1 CC 0x30=127, got {cc_map}")
    check(
        60 <= cc_map.get(0x38, 0) <= 65,
        f"Expected Master Fader CC 0x38 in 60..65, got {cc_map}",
    )

    # Verify top-stop noise spikes do NOT bounce 127 -> 126, while genuine move hits 126
    spike_events: list[bytes] = []
    for sample in (4092, 4058, 4091, 4093, 4059, 4090, 4094, 4057, 4092, 4060, 4093):
        mcu.fader_adc[0] = sample
        mcu._run_adc1_dma()
        mcu.run_steps(10_000)
        spike_events.extend(mcu.usb_poll_midi_in())

    spurious_f0 = [
        pkt[i + 3]
        for pkt in spike_events
        for i in range(0, len(pkt), 4)
        if pkt[i : i + 3] == bytes([0x0B, 0xB0, 0x30])
    ]
    check(
        not spurious_f0,
        f"Spurious Fader 1 CC bounce at top stop under noise spikes: {spurious_f0}",
    )

    step_down_events: list[bytes] = []
    for _ in range(3):
        mcu.fader_adc[0] = 4035
        mcu._run_adc1_dma()
        mcu.run_steps(10_000)
        step_down_events.extend(mcu.usb_poll_midi_in())

    step_down_f0 = [
        pkt[i + 3]
        for pkt in step_down_events
        for i in range(0, len(pkt), 4)
        if pkt[i : i + 3] == bytes([0x0B, 0xB0, 0x30])
    ]
    check(
        step_down_f0 == [126],
        f"Expected clean step-down from 127 to [126], got {step_down_f0}",
    )
    ok("ADC1+DMA1 9-fader filter", f"CC 0x30=127 (no spike bounce, clean ->126), CC 0x38={cc_map[0x38]}")

    # 8. LED Control: TM1629 SPI Bit-Bang & Multiplexed Round LEDs
    mcu.tm_frames.clear()
    mcu.tm_display = None
    # Note 0x00 vel 1 (bottom-left pad green -> row=7, col=0 -> kTmGreenRow[7]=0x0F bit 0)
    # Note 0x40 vel 1 (Track 1 round LED ON -> group 0, bit_idx 7 -> row_anode 0, sink bit 1 cleared)
    mcu.usb_ep1_out(bytes([0x09, 0x90, 0x00, 0x01, 0x09, 0x90, 0x40, 0x01]))
    mcu.run_steps(20_000)
    for _ in range(60):  # Drain led_flush_gate (50 ticks) & round_gate (8 ticks)
        mcu.call_isr(VEC_SYSTICK)
    for _ in range(8):
        mcu.run_steps(15_000)
        for _ in range(8):
            mcu.call_isr(VEC_SYSTICK)

    check(
        b"\x40" in mcu.tm_frames,
        f"TM1629 missing 0x40 data command (frames={mcu.tm_frames})",
    )
    check(
        mcu.tm_display is not None and (mcu.tm_display[0x0F] & 0x01) == 0x01,
        "TM1629 16B display buffer did not light Pad 0 green",
    )
    check(
        bytes(mcu._tm_cur) == b"\x8f" or b"\x8f" in mcu.tm_frames,
        "TM1629 missing 0x8F display-ON command",
    )
    check(
        (mcu.round_led_sinks[0] & 0x02) == 0,
        f"Round LED Track 1 sink bit not asserted (sinks={mcu.round_led_sinks})",
    )
    ok("LED drivers", "TM1629 bit-bang (0x40, 0xC0+16B, 0x8F) & round LED mux")

    # 9. Diagnostic Telemetry SysEx (F0 7D 'A' 'P' 'C' 01 F7)
    from tools.fw_debug import APC_CRASH_MAGIC, decode_diag_nibbles

    # USB-MIDI packetized F0 7D 'A' 'P' 'C' 01 F7:
    #   [0x04, 0xF0, 0x7D, 'A', 0x04, 'P', 'C', 0x01, 0x05, 0xF7, 0x00, 0x00]
    diag_req = bytes([0x04, 0xF0, 0x7D, 0x41, 0x04, 0x50, 0x43, 0x01, 0x05, 0xF7, 0x00, 0x00])
    mcu.usb_ep1_out(diag_req)
    diag_pkts: list[bytes] = []
    for _ in range(8):
        mcu.run_steps(20_000)
        diag_pkts.extend(mcu.usb_poll_midi_in())
    diag_sysex = extract_usb_midi_sysex(diag_pkts)
    subcmd, telem_bytes = decode_diag_nibbles(diag_sysex)
    check(
        subcmd == 0x01 and len(telem_bytes) == 28,
        f"Bad diagnostic telemetry reply (subcmd={subcmd}, len={len(telem_bytes)})",
    )
    _, _, flash_kb, idcode, _, loop_cnt, _ = struct.unpack("<BBHIIIH", telem_bytes[:18])
    check(
        flash_kb == 128 and idcode == 0x10016410 and loop_cnt > 0,
        f"Unexpected telemetry values: flash_kb={flash_kb}, idcode=0x{idcode:08X}, loops={loop_cnt}",
    )

    # 10. Simultaneous Monome serialosc (FTDI 0403:6001) & APC MINI USB-MIDI Composite Verification
    ftdi_desc = mcu.usb_ep0_setup(
        bytes([0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00]), expect_in=True
    )
    f_vid, f_pid = struct.unpack_from("<HH", ftdi_desc, 8)
    check(
        (f_vid, f_pid) == (0x0403, 0x6001),
        f"Expected FTDI VID:PID 0403:6001 in Monome mode, got {f_vid:04X}:{f_pid:04X}",
    )

    modem_stat = mcu.usb_ep0_setup(
        bytes([0xC0, 0x05, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00]), expect_in=True
    )
    check(
        modem_stat == b"\x01\x60",
        f"FTDI SIO_GET_MODEM_STATUS expected 01 60, got {modem_stat.hex()}",
    )
    check(
        (mcu.usb_epr[2] & 0x3000) == 0x3000,
        "FTDI EP2 Bulk OUT not armed after SET_CONFIGURATION",
    )
    check(
        (mcu.usb_epr[3] & 0x3000) == 0x3000,
        "Composite USB-MIDI EP3 Bulk OUT not armed in Monome mode",
    )

    # Verify composite USB-MIDI interface (EP3 OUT/IN) responds to Universal Device Inquiry while in Monome mode
    mcu.usb_ep_out(3, bytes([0x04, 0xF0, 0x7E, 0x7F, 0x07, 0x06, 0x01, 0xF7]))
    ep3_pkts: list[bytes] = []
    for _ in range(4):
        mcu.run_steps(15_000)
        p3 = mcu._harvest_ep_tx(3)
        if p3 is not None:
            mcu.usb_epr[3] |= 0x0080
            mcu.call_isr(VEC_USB_LP_IRQ)
            ep3_pkts.append(p3)
    ep3_id = extract_usb_midi_sysex(ep3_pkts)
    check(
        len(ep3_id) == 35 and ep3_id[:6] == bytes([0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x47]),
        f"Composite USB-MIDI EP3 did not return Identity Reply in Monome mode: {ep3_id.hex(' ')}",
    )

    # Drain any FTDI serialosc packets emitted during Stages 6-7 button/fader tests
    for _ in range(8):
        mcu.run_steps(10_000)
        mcu.usb_poll_ep1_in()

    # Send mext /sys/id (0x01) and /sys/size (0x03) on EP2 OUT
    mcu.usb_ep_out(2, bytes([0x01, 0x03]))
    mcu.run_steps(30_000)
    ftdi_in = mcu.usb_poll_ep1_in()
    check(
        len(ftdi_in) >= 1 and ftdi_in[0][:2] == b"\x01\x60",
        f"Missing 2-byte FTDI status header (01 60) on Bulk IN: {ftdi_in}",
    )
    mext_stream = b"".join(p[2:] for p in ftdi_in if len(p) > 2)
    check(
        len(mext_stream) == 36
        and mext_stream[0] == 0x01
        and mext_stream[1:9] == b"APC MINI"
        and mext_stream[33:36] == b"\x02\x08\x08",
        f"Bad mext /sys/id + /sys/size reply ({len(mext_stream)} B): {mext_stream.hex(' ')}",
    )

    # Press top-left Grid Pad (col=0, row=0 -> Monome x=0, y=0) -> expect 0x21 0x00 0x00
    mcu.pressed_buttons = {(0, 0)}
    key_pkts: list[bytes] = []
    for _ in range(6):
        for _ in range(8):
            mcu.call_isr(VEC_SYSTICK)
        mcu.run_steps(15_000)
        key_pkts.extend(mcu.usb_poll_ep1_in())
    key_bytes = b"".join(p[2:] for p in key_pkts if len(p) > 2)
    check(
        key_bytes == b"\x21\x00\x00",
        f"Expected Monome top-left key-down (21 00 00), got {key_bytes.hex(' ')}",
    )

    mcu.pressed_buttons.clear()
    key_pkts.clear()
    for _ in range(6):
        for _ in range(8):
            mcu.call_isr(VEC_SYSTICK)
        mcu.run_steps(15_000)
        key_pkts.extend(mcu.usb_poll_ep1_in())
    key_bytes = b"".join(p[2:] for p in key_pkts if len(p) > 2)
    check(
        key_bytes == b"\x20\x00\x00",
        f"Expected Monome top-left key-up (20 00 00), got {key_bytes.hex(' ')}",
    )

    # Test mext varibright LED command (0x18 x=0 y=0 level=15 -> Red on row=0, col=0)
    # and mext LED intensity (0x17 0x0A -> TM1629 brightness 3 -> 0x8B)
    mcu.tm_frames.clear()
    mcu.tm_display = None
    mcu.usb_ep_out(2, bytes([0x18, 0x00, 0x00, 0x0F, 0x17, 0x0A]))
    mcu.run_steps(25_000)
    for _ in range(60):
        mcu.call_isr(VEC_SYSTICK)
    mcu.run_steps(80_000)
    check(
        mcu.tm_display is not None and (mcu.tm_display[0x00] & 0x01) == 0x01,
        f"Monome varibright level 15 did not light top-left pad Red: {mcu.tm_display}",
    )
    check(
        bytes(mcu._tm_cur) == b"\x8b" or b"\x8b" in mcu.tm_frames,
        f"Monome 0x17 intensity did not update TM1629 brightness to 0x8B: {mcu.tm_frames}",
    )

    # Test Fader 1 -> mext Tilt Sensor 0 (0x81 0x00 ...)
    for _ in range(6):
        mcu.fader_adc[0] = 2048
        mcu._run_adc1_dma()
        mcu.run_steps(10_000)
    tilt_pkts = mcu.usb_poll_ep1_in()
    tilt_bytes = b"".join(p[2:] for p in tilt_pkts if len(p) > 2)
    check(
        len(tilt_bytes) >= 8 and tilt_bytes[0] == 0x81 and tilt_bytes[1] == 0x00,
        f"Expected mext tilt packet (81 00 ...), got {tilt_bytes.hex(' ')}",
    )
    ok(
        "Monome serialosc (0403:6001)",
        "FTDI vendor reqs, mext /sys, top-left (0,0) keys, varibright+intensity, fader tilt",
    )

    # 10. On-Chip Lua Co-Processor alongside serialosc (SHIFT + SCENE 3 + live upload)
    # Toggle Lua Co-Processor ON: Hold Shift (col=10, row=0) + Scene 3 (col=9, row=2)
    mcu.pressed_buttons = {(10, 0), (9, 2)}
    for _ in range(6):
        for _ in range(8):
            mcu.call_isr(VEC_SYSTICK)
        mcu.run_steps(40_000)
    mcu.pressed_buttons.clear()
    for _ in range(6):
        for _ in range(8):
            mcu.call_isr(VEC_SYSTICK)
        mcu.run_steps(20_000)
    mcu.usb_poll_ep1_in()

    # Press top-left pad (col=0, row=0 -> Monome x=0, y=0) while Lua Co-Processor is active:
    # 1) Normal key press (21 00 00) passes through to serialosc
    # 2) Built-in bouncy_echo.lua spawns a ripple and fires an echoed key event (21 01 00, 20 01 00) on tick!
    mcu.pressed_buttons = {(0, 0)}
    lua_pkts: list[bytes] = []
    for _ in range(6):
        for _ in range(8):
            mcu.call_isr(VEC_SYSTICK)
        mcu.run_steps(30_000)
        lua_pkts.extend(mcu.usb_poll_ep1_in())
    mcu.pressed_buttons.clear()

    # Advance 140 SysTicks (> 55 ms tempo) so lua_vm_task runs on_tick(0)
    for _ in range(10):
        for _ in range(14):
            mcu.call_isr(VEC_SYSTICK)
        mcu.run_steps(40_000)
        lua_pkts.extend(mcu.usb_poll_ep1_in())

    lua_stream = b"".join(p[2:] for p in lua_pkts if len(p) > 2)
    check(
        b"\x21\x00\x00" in lua_stream and b"\x21\x01\x00\x20\x01\x00" in lua_stream,
        f"Lua Co-Processor did not emit pass-through (21 00 00) + echoed mext key (21 01 00 20 01 00): {lua_stream.hex(' ')}",
    )

    # Test live Lua bytecode upload over FTDI serial (EP2 OUT)
    from tools.lua_compiler import build_upload_sysex_packets, compile_lua_source

    custom_lua = """
    function on_grid(x, y, z)
        if z == 1 then
            key_pulse(7, 7, 10)
            return 1
        end
        return 0
    end
    """
    upload_pkts = build_upload_sysex_packets(compile_lua_source(custom_lua), save_to_flash=False)
    for upkt in upload_pkts:
        mcu.usb_ep_out(2, bytes(upkt))
        mcu.run_steps(40_000)
        mcu.usb_poll_ep1_in()

    # Press top-left pad (0, 0): custom script intercepts it (returns 1) and fires key_pulse(7, 7, 10)
    # -> immediate 21 07 07 (key-down), followed by async 20 07 07 (key-up) after 10 ms (20 SysTicks)
    mcu.pressed_buttons = {(0, 0)}
    intercept_pkts: list[bytes] = []
    for _ in range(6):
        for _ in range(8):
            mcu.call_isr(VEC_SYSTICK)
        mcu.run_steps(30_000)
        intercept_pkts.extend(mcu.usb_poll_ep1_in())
    mcu.pressed_buttons.clear()
    for _ in range(6):
        for _ in range(8):
            mcu.call_isr(VEC_SYSTICK)
        mcu.run_steps(20_000)
        intercept_pkts.extend(mcu.usb_poll_ep1_in())
    intercept_stream = b"".join(p[2:] for p in intercept_pkts if len(p) > 2)
    check(
        intercept_stream.startswith(b"\x21\x07\x07\x20\x07\x07"),
        f"Live-uploaded Lua script did not intercept (0,0) -> async key_pulse (21 07 07 20 07 07): {intercept_stream.hex(' ')}",
    )
    ok(
        "Lua Co-Processor (SHIFT+SCENE 3)",
        "async time-sliced VM, key_pulse gate queue, LED overlay, live bytecode upload",
    )

    # 11. Bootloader DFU Unlock & Reset
    unlock_req = bytes(
        [0x04, 0xF0, 0x5A, 0x57, 0x04, 0x6E, 0x28, 0x3C, 0x07, 0x4E, 0x3E, 0xF7]
    )
    mcu.usb_ep1_out(unlock_req)

    magic = bytes(mcu.uc.mem_read(BOOT_MAGIC_ADDR, 8))
    check(
        magic == BOOT_MAGIC,
        f"Bootloader magic not programmed at 0x{BOOT_MAGIC_ADDR:08X}: {magic.hex(' ')}",
    )
    ack_pkts = mcu.usb_poll_ep1_in()
    check(
        any(p[:4] == bytes([0x07, 0xF0, 0x15, 0xF7]) for p in ack_pkts),
        "Missing DFU unlock ACK (07 F0 15 F7)",
    )
    check(
        (mcu.gpio_odr[0] & (1 << 8)) == 0,
        "PA8 (USB D+) was not pulled LOW before DFU reset",
    )
    check(
        mcu.tm_display == b"\x00" * 16,
        f"TM1629 display RAM was not cleared to all-zero before DFU reboot: {mcu.tm_display}",
    )
    bkp_dr1 = struct.unpack("<H", mcu.uc.mem_read(0x40006C04, 2))[0]
    check(
        bkp_dr1 == 0x4D4F,
        f"BKP_DR1 (0x40006C04) did not save Monome resume magic 0x4D4F before DFU reset: 0x{bkp_dr1:04X}",
    )
    check(mcu.reset_requested, "SCB->AIRCR SYSRESETREQ was not triggered")
    ok(
        "DFU unlock & system reset",
        "0x08007C00 magic written, ACK 0x15 sent, PA8 LOW, SYSRESETREQ",
    )

    # 11. Fault-Handler Black-Box Crash Log (0x08007800) & Self-Healing DFU Recovery
    CRASH_LOG_ADDR = 0x08007800
    mcu.uc.mem_write(BOOT_MAGIC_ADDR, b"\xff" * 0x400)
    mcu.uc.mem_write(CRASH_LOG_ADDR, b"\xff" * 0x400)
    mcu._update_gpio_odr(0, mcu.gpio_odr[0] | (1 << 8))
    mcu.reset_requested = False
    mcu.allow_fault_recovery = True

    # 11a. Test HardFault with a stacked Cortex-M3 exception frame on MSP (0x200017C0)
    fake_msp = 0x200017C0
    fake_frame = struct.pack(
        "<8I",
        0x11111111,  # R0
        0x22222222,  # R1
        0x33333333,  # R2
        0x44444444,  # R3
        0x55555555,  # R12
        0x08002555,  # LR
        0x08002AAA,  # PC
        0x01000000,  # xPSR
    )
    mcu.uc.mem_write(fake_msp, fake_frame)
    mcu.regs[0xE000ED28] = 0x00008200  # SCB->CFSR: PRECISERR | BFARVALID
    mcu.regs[0xE000ED2C] = 0x40000000  # SCB->HFSR: FORCED
    mcu.regs[0xE000ED38] = 0x08008000  # SCB->BFAR: faulting address
    mcu.uc.reg_write(UC_ARM_REG_MSP, fake_msp)
    mcu.uc.reg_write(UC_ARM_REG_SP, fake_msp)
    mcu.call_isr(3)

    crash_words = struct.unpack("<12I", mcu.uc.mem_read(CRASH_LOG_ADDR, 48))
    check(
        crash_words[0] == APC_CRASH_MAGIC
        and crash_words[2] == fake_msp
        and crash_words[3] == 0x00008200
        and crash_words[5] == 0x08008000
        and crash_words[9] == 0x08002555
        and crash_words[10] == 0x08002AAA,
        f"Black-box crash log at 0x08007800 did not capture exception frame: {[hex(w) for w in crash_words]}",
    )

    # 11b. Test HardFault when MSP is completely corrupted/unmapped (0x2000FF00)
    mcu.uc.mem_write(BOOT_MAGIC_ADDR, b"\xff" * 0x400)
    mcu._update_gpio_odr(0, mcu.gpio_odr[0] | (1 << 8))
    mcu.reset_requested = False
    mcu.uc.reg_write(UC_ARM_REG_MSP, 0x2000FF00)
    mcu.uc.reg_write(UC_ARM_REG_SP, 0x2000FF00)
    mcu.call_isr(3)

    fault_magic = bytes(mcu.uc.mem_read(BOOT_MAGIC_ADDR, 8))
    check(
        fault_magic == BOOT_MAGIC,
        f"Fault handler did not program DFU magic at 0x{BOOT_MAGIC_ADDR:08X}: {fault_magic.hex(' ')}",
    )
    check(
        (mcu.gpio_odr[0] & (1 << 8)) == 0,
        "Fault handler did not pull PA8 (USB D+) LOW before DFU reset",
    )
    check(mcu.reset_requested, "Fault handler did not trigger SCB->AIRCR SYSRESETREQ")
    ok(
        "Black-box crash log & DFU recovery",
        "0x08007800 crash dump (PC/LR/CFSR/BFAR), SysEx telemetry, corrupted-MSP DFU reboot",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "firmware",
        type=Path,
        nargs="?",
        default=Path("build/apc_mini_app.bin"),
        help="Path to 22,528-byte stamped firmware binary (default: build/apc_mini_app.bin)",
    )
    parser.add_argument(
        "-q", "--quiet", action="store_true", help="Suppress per-check [PASS] lines"
    )
    args = parser.parse_args()

    if not args.quiet:
        print(f"Validating {args.firmware} on emulated STM32F102RBT6...")
    try:
        validate_firmware(args.firmware, verbose=not args.quiet)
    except ValidationError as exc:
        print(f"  [FAIL] {exc}", file=sys.stderr)
        sys.exit(1)

    if not args.quiet:
        print("All hardware & protocol validation checks passed.")


if __name__ == "__main__":
    main()
