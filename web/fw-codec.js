export const APP_BASE_OFFSET = 0x2000;
export const APP_CHECKSUM_OFF = 0x57fe;
export const APP_TOTAL_SIZE = 0x5800; // 22,528 bytes (22 x 1 KB pages)
export const FLASH_PAGE_SIZE = 0x0400; // 1,024 bytes
export const RAM_CHUNK_SIZE = 8;

export const UNLOCK_SYSEX = new Uint8Array([
  0xf0, 0x5a, 0x57, 0x6e, 0x28, 0x3c, 0x4e, 0x3e, 0xf7,
]);

export const OPCODE_RAM_LOAD = 0x0a;
export const OPCODE_FLASH_WRITE = 0x0b;
export const OPCODE_FINALIZE = 0x0e;

export const ACK_APP_UNLOCK = 0x15;
export const ACK_BL_UNLOCK = 0x3e;
export const ACK_OK = 0x1a;
export const ACK_DONE = 0x77;

/**
 * Validates a 22,528-byte stamped binary and returns its 16-bit checksum word.
 */
export function validateStampedBin(fwBytes) {
  if (!(fwBytes instanceof Uint8Array)) {
    fwBytes = new Uint8Array(fwBytes);
  }
  if (fwBytes.length !== APP_TOTAL_SIZE) {
    throw new Error(
      `Expected ${APP_TOTAL_SIZE} B stamped binary, got ${fwBytes.length} B`,
    );
  }
  let sum16 = 0;
  for (let i = 0; i < APP_CHECKSUM_OFF; i++) {
    sum16 = (sum16 + fwBytes[i]) & 0xffff;
  }
  const chk = fwBytes[APP_CHECKSUM_OFF] | (fwBytes[APP_CHECKSUM_OFF + 1] << 8);
  if (((sum16 + chk) & 0xffff) !== 0) {
    throw new Error(
      `Invalid bootloader checksum in binary: (0x${sum16.toString(16).toUpperCase().padStart(4, "0")} + ` +
        `0x${chk.toString(16).toUpperCase().padStart(4, "0")}) & 0xFFFF != 0`,
    );
  }
  return chk;
}

function makeRecord(opcode, addr24, length16, payload = new Uint8Array(0)) {
  const out = new Uint8Array(6 + payload.length + 1);
  out[0] = opcode & 0xff;
  out[1] = (addr24 >> 16) & 0xff;
  out[2] = (addr24 >> 8) & 0xff;
  out[3] = addr24 & 0xff;
  out[4] = (length16 >> 8) & 0xff;
  out[5] = length16 & 0xff;
  out.set(payload, 6);
  out[6 + payload.length] = 0x00;
  return out;
}

function buildBootloaderSysex(recordBytes) {
  const bodyLen = 4 + recordBytes.length;
  const body = new Uint8Array(bodyLen);
  body[0] = 0x6a;
  body[1] = 0x04;
  body[2] = 0xda;
  body[3] = 0x33;
  body.set(recordBytes, 4);

  const r6Len = bodyLen + 2;
  let sumBody = 0;
  for (let i = 0; i < bodyLen; i++) {
    sumBody = (sumBody + body[i]) & 0xffff;
  }
  const chkLe = -sumBody & 0xffff;

  const rawFrame = new Uint8Array(2 + bodyLen + 2);
  rawFrame[0] = (r6Len >> 8) & 0xff;
  rawFrame[1] = r6Len & 0xff;
  rawFrame.set(body, 2);
  rawFrame[2 + bodyLen] = chkLe & 0xff;
  rawFrame[2 + bodyLen + 1] = (chkLe >> 8) & 0xff;

  const sysex = new Uint8Array(1 + rawFrame.length * 2 + 1);
  sysex[0] = 0xf0;
  let pos = 1;
  for (let i = 0; i < rawFrame.length; i++) {
    const hex = rawFrame[i].toString(16).toUpperCase().padStart(2, "0");
    sysex[pos++] = hex.charCodeAt(0);
    sysex[pos++] = hex.charCodeAt(1);
  }
  sysex[pos] = 0xf7;
  return sysex;
}

/**
 * Converts a 22,528-byte stamped .bin into 2,839 ordered bootloader SysEx steps.
 */
export function encodeFirmwareSteps(fwBytes) {
  const chk = validateStampedBin(fwBytes);
  const numPages = APP_TOTAL_SIZE / FLASH_PAGE_SIZE;
  const steps = [];

  for (let pageIdx = 0; pageIdx < numPages; pageIdx++) {
    const flashOffset = APP_BASE_OFFSET + pageIdx * FLASH_PAGE_SIZE;
    const pageStart = pageIdx * FLASH_PAGE_SIZE;
    const pageData = fwBytes.subarray(pageStart, pageStart + FLASH_PAGE_SIZE);

    for (
      let chunkOff = 0;
      chunkOff < FLASH_PAGE_SIZE;
      chunkOff += RAM_CHUNK_SIZE
    ) {
      const chunk = pageData.subarray(chunkOff, chunkOff + RAM_CHUNK_SIZE);
      const rec = makeRecord(OPCODE_RAM_LOAD, chunkOff, chunk.length, chunk);
      steps.push({
        kind: "ram_load",
        sysex: buildBootloaderSysex(rec),
        expectedAck: ACK_OK,
        pageIdx,
        flashOffset,
        chunkOffset: chunkOff,
      });
    }

    const writeRec = makeRecord(
      OPCODE_FLASH_WRITE,
      flashOffset,
      FLASH_PAGE_SIZE,
    );
    steps.push({
      kind: "flash_write",
      sysex: buildBootloaderSysex(writeRec),
      expectedAck: ACK_OK,
      pageIdx,
      flashOffset,
      chunkOffset: null,
    });
  }

  const finRec = makeRecord(OPCODE_FINALIZE, 0x000000, 0x0000);
  steps.push({
    kind: "finalize",
    sysex: buildBootloaderSysex(finRec),
    expectedAck: ACK_DONE,
    pageIdx: null,
    flashOffset: null,
    chunkOffset: null,
  });

  return {
    steps,
    binBytes: new Uint8Array(fwBytes),
    checksum: chk,
  };
}

function arraysEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) return false;
  }
  return true;
}

function decodeAsciiHex(asciiBytes) {
  if (asciiBytes.length % 2 !== 0) {
    throw new Error("Odd number of ASCII hex characters in SysEx frame");
  }
  const out = new Uint8Array(asciiBytes.length / 2);
  for (let i = 0; i < out.length; i++) {
    const hi = String.fromCharCode(asciiBytes[i * 2]);
    const lo = String.fromCharCode(asciiBytes[i * 2 + 1]);
    const val = parseInt(hi + lo, 16);
    if (Number.isNaN(val)) {
      throw new Error("Invalid ASCII hex character in SysEx frame");
    }
    out[i] = val;
  }
  return out;
}

/**
 * Parses and validates a .syx stream into steps + reconstructed 22,528-byte .bin.
 */
export function decodeSyxToBundle(syxBytes) {
  if (!(syxBytes instanceof Uint8Array)) {
    syxBytes = new Uint8Array(syxBytes);
  }
  const msgs = [];
  let idx = 0;
  while (idx < syxBytes.length) {
    if (syxBytes[idx] !== 0xf0) {
      throw new Error(
        `Expected 0xF0 at byte offset ${idx}, got 0x${syxBytes[idx].toString(16)}`,
      );
    }
    let end = idx + 1;
    while (end < syxBytes.length && syxBytes[end] !== 0xf7) {
      end++;
    }
    if (end >= syxBytes.length) {
      throw new Error(`Unterminated SysEx message starting at offset ${idx}`);
    }
    msgs.push(syxBytes.subarray(idx, end + 1));
    idx = end + 1;
  }
  if (msgs.length === 0) {
    throw new Error("Empty .syx stream");
  }

  const steps = [];
  const flashImage = new Uint8Array(APP_TOTAL_SIZE);
  flashImage.fill(0xff);
  const ramPage = new Uint8Array(FLASH_PAGE_SIZE);
  ramPage.fill(0xff);
  let pageIdx = 0;
  let finalized = false;

  for (const msg of msgs) {
    if (arraysEqual(msg, UNLOCK_SYSEX)) {
      continue;
    }
    const raw = decodeAsciiHex(msg.subarray(1, msg.length - 1));
    const r6Len = (raw[0] << 8) | raw[1];
    const body = raw.subarray(2, raw.length - 2);
    const chk = raw[raw.length - 2] | (raw[raw.length - 1] << 8);
    let sumBody = 0;
    for (let i = 0; i < body.length; i++) {
      sumBody = (sumBody + body[i]) & 0xffff;
    }
    if (r6Len !== body.length + 2 || ((sumBody + chk) & 0xffff) !== 0) {
      throw new Error("Corrupted bootloader SysEx frame length or checksum");
    }
    if (
      body[0] !== 0x6a ||
      body[1] !== 0x04 ||
      body[2] !== 0xda ||
      body[3] !== 0x33
    ) {
      throw new Error("Invalid bootloader frame magic header");
    }

    const rec = body.subarray(4);
    const opcode = rec[0];
    const addr24 = (rec[1] << 16) | (rec[2] << 8) | rec[3];
    const length16 = (rec[4] << 8) | rec[5];

    if (opcode === OPCODE_RAM_LOAD) {
      const payload = rec.subarray(6, 6 + length16);
      ramPage.set(payload, addr24);
      steps.push({
        kind: "ram_load",
        sysex: new Uint8Array(msg),
        expectedAck: ACK_OK,
        pageIdx,
        flashOffset: APP_BASE_OFFSET + pageIdx * FLASH_PAGE_SIZE,
        chunkOffset: addr24,
      });
    } else if (opcode === OPCODE_FLASH_WRITE) {
      const dst = addr24 - APP_BASE_OFFSET;
      if (
        dst < 0 ||
        dst + length16 > APP_TOTAL_SIZE ||
        length16 !== FLASH_PAGE_SIZE
      ) {
        throw new Error(
          `Invalid flash_write range: 0x${addr24.toString(16)}+${length16}`,
        );
      }
      flashImage.set(ramPage, dst);
      ramPage.fill(0xff);
      steps.push({
        kind: "flash_write",
        sysex: new Uint8Array(msg),
        expectedAck: ACK_OK,
        pageIdx,
        flashOffset: addr24,
        chunkOffset: null,
      });
      pageIdx++;
    } else if (opcode === OPCODE_FINALIZE) {
      finalized = true;
      steps.push({
        kind: "finalize",
        sysex: new Uint8Array(msg),
        expectedAck: ACK_DONE,
        pageIdx: null,
        flashOffset: null,
        chunkOffset: null,
      });
    } else {
      throw new Error(
        `Unknown bootloader SysEx opcode: 0x${opcode.toString(16)}`,
      );
    }
  }

  if (!finalized) {
    throw new Error(
      "Missing bootloader finalize (0x0E) command in .syx stream",
    );
  }
  const binChecksum = validateStampedBin(flashImage);
  return { steps, binBytes: flashImage, checksum: binChecksum };
}

export function stepsToSyxBytes(steps) {
  let total = 0;
  for (const s of steps) total += s.sysex.length;
  const out = new Uint8Array(total);
  let off = 0;
  for (const s of steps) {
    out.set(s.sysex, off);
    off += s.sysex.length;
  }
  return out;
}
