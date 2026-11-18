const APP_BASE_OFFSET = 0x2000;
const APP_CHECKSUM_OFF = 0x57fe;
const APP_TOTAL_SIZE = 0x5800; // 22,528 bytes (22 x 1 KB pages)
const FLASH_PAGE_SIZE = 0x0400; // 1,024 bytes
const RAM_CHUNK_SIZE = 8;

const UNLOCK_SYSEX = new Uint8Array([
  0xf0, 0x5a, 0x57, 0x6e, 0x28, 0x3c, 0x4e, 0x3e, 0xf7,
]);
const INQUIRY_SYSEX = new Uint8Array([0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7]);

const OPCODE_RAM_LOAD = 0x0a;
const OPCODE_FLASH_WRITE = 0x0b;
const OPCODE_FINALIZE = 0x0e;

const ACK_APP_UNLOCK = 0x15;
const ACK_BL_UNLOCK = 0x3e;
const ACK_OK = 0x1a;
const ACK_DONE = 0x77;

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

// =========================================================================
// UI & WebMIDI State
// =========================================================================
const ui = {
  midiStatusDot: document.getElementById("midiStatusDot"),
  midiStatusText: document.getElementById("midiStatusText"),
  deviceModeLabel: document.getElementById("deviceModeLabel"),
  midiOutSelect: document.getElementById("midiOutSelect"),
  midiInSelect: document.getElementById("midiInSelect"),
  btnConnectMidi: document.getElementById("btnConnectMidi"),
  btnProbeDevice: document.getElementById("btnProbeDevice"),
  btnSwitchProfile: document.getElementById("btnSwitchProfile"),
  fwSourceBadge: document.getElementById("fwSourceBadge"),
  versionSelect: document.getElementById("versionSelect"),
  localFileZone: document.getElementById("localFileZone"),
  fileInput: document.getElementById("fileInput"),
  dropzonePrompt: document.getElementById("dropzonePrompt"),
  metaVersion: document.getElementById("metaVersion"),
  metaSize: document.getElementById("metaSize"),
  metaChecksum: document.getElementById("metaChecksum"),
  metaSteps: document.getElementById("metaSteps"),
  btnDownloadSyx: document.getElementById("btnDownloadSyx"),
  btnDownloadBin: document.getElementById("btnDownloadBin"),
  btnFlash: document.getElementById("btnFlash"),
  flashStateLabel: document.getElementById("flashStateLabel"),
  progressBar: document.getElementById("progressBar"),
  progressPageText: document.getElementById("progressPageText"),
  progressAddrText: document.getElementById("progressAddrText"),
  progressPctText: document.getElementById("progressPctText"),
  diagStatusLabel: document.getElementById("diagStatusLabel"),
  btnReadDiag: document.getElementById("btnReadDiag"),
  btnLivePoll: document.getElementById("btnLivePoll"),
  btnLedSweep: document.getElementById("btnLedSweep"),
  btnLedClear: document.getElementById("btnLedClear"),
  btnTriggerFault: document.getElementById("btnTriggerFault"),
  chkModeMidi: document.getElementById("chkModeMidi"),
  chkModeSerialosc: document.getElementById("chkModeSerialosc"),
  chkModeLua: document.getElementById("chkModeLua"),
  chkModeAbleton: document.getElementById("chkModeAbleton"),
  selPalette: document.getElementById("selPalette"),
  selBrightness: document.getElementById("selBrightness"),
  telemSilicon: document.getElementById("telemSilicon"),
  telemReset: document.getElementById("telemReset"),
  telemUptime: document.getElementById("telemUptime"),
  telemLoop: document.getElementById("telemLoop"),
  telemFaders: document.getElementById("telemFaders"),
  telemShift: document.getElementById("telemShift"),
  telemLastMidi: document.getElementById("telemLastMidi"),
  crashBox: document.getElementById("crashBox"),
  crashTitle: document.getElementById("crashTitle"),
  crashPre: document.getElementById("crashPre"),
  btnCopyCrash: document.getElementById("btnCopyCrash"),
  btnClearCrash: document.getElementById("btnClearCrash"),
  peekAddrInput: document.getElementById("peekAddrInput"),
  btnPeekMem: document.getElementById("btnPeekMem"),
  peekPre: document.getElementById("peekPre"),
  logBox: document.getElementById("logBox"),
  btnClearLog: document.getElementById("btnClearLog"),
};

let midiAccess = null;
let activeBundle = null; // { versionLabel, steps, binBytes, syxBytes, checksum }
let activeSymbols = []; // [{ addr, size, name }, ...]
let lastCrashMarkdown = "";
let livePollTimer = null;
let isDiagBusy = false;
let manifestData = null;
let isFlashing = false;
let pendingRxResolver = null;

function log(msg, level = "info") {
  const line = document.createElement("div");
  line.className = `log-line ${level}`;
  const ts = new Date().toLocaleTimeString();
  line.textContent = `[${ts}] ${msg}`;
  ui.logBox.appendChild(line);
  ui.logBox.scrollTop = ui.logBox.scrollHeight;
}

ui.btnClearLog.addEventListener("click", () => {
  ui.logBox.innerHTML = "";
});

function updateControlsState() {
  const hasMidiPorts = Boolean(ui.midiOutSelect.value && ui.midiInSelect.value);
  const canDiag = hasMidiPorts && !isFlashing && !isDiagBusy;
  ui.btnProbeDevice.disabled = !hasMidiPorts || isFlashing;
  if (ui.btnSwitchProfile)
    ui.btnSwitchProfile.disabled = !hasMidiPorts || isFlashing;
  ui.btnFlash.disabled = !hasMidiPorts || !activeBundle || isFlashing;
  ui.btnDownloadSyx.disabled = !activeBundle;
  ui.btnDownloadBin.disabled = !activeBundle;
  ui.versionSelect.disabled = isFlashing;
  ui.midiOutSelect.disabled = !midiAccess || isFlashing;
  ui.midiInSelect.disabled = !midiAccess || isFlashing;
  if (ui.btnReadDiag) ui.btnReadDiag.disabled = !canDiag;
  if (ui.btnLivePoll) ui.btnLivePoll.disabled = !hasMidiPorts || isFlashing;
  if (ui.btnLedSweep) ui.btnLedSweep.disabled = !canDiag;
  if (ui.btnLedClear) ui.btnLedClear.disabled = !canDiag;
  if (ui.btnTriggerFault) ui.btnTriggerFault.disabled = !canDiag;
  if (ui.btnPeekMem) ui.btnPeekMem.disabled = !canDiag;
  if (ui.chkModeMidi) ui.chkModeMidi.disabled = !canDiag;
  if (ui.chkModeSerialosc) ui.chkModeSerialosc.disabled = !canDiag;
  if (ui.chkModeLua) ui.chkModeLua.disabled = !canDiag;
  if (ui.chkModeAbleton) ui.chkModeAbleton.disabled = !canDiag;
  if (ui.selPalette) ui.selPalette.disabled = !canDiag;
  if (ui.selBrightness) ui.selBrightness.disabled = !canDiag;
}

function setLoadedBundle(versionLabel, bundle) {
  const syxBytes = stepsToSyxBytes(bundle.steps);
  activeBundle = {
    versionLabel,
    steps: bundle.steps,
    binBytes: bundle.binBytes,
    syxBytes,
    checksum: bundle.checksum,
  };
  ui.metaVersion.textContent = versionLabel;
  ui.metaSize.textContent = `${bundle.binBytes.length.toLocaleString()} B (22 pages)`;
  ui.metaChecksum.textContent = `0x${bundle.checksum.toString(16).toUpperCase().padStart(4, "0")}`;
  ui.metaSteps.textContent = `${bundle.steps.length.toLocaleString()} msgs`;
  updateControlsState();
}

function clearLoadedBundle() {
  activeBundle = null;
  ui.metaVersion.textContent = "—";
  ui.metaSize.textContent = "—";
  ui.metaChecksum.textContent = "—";
  ui.metaSteps.textContent = "—";
  updateControlsState();
}

// =========================================================================
// Manifest & Firmware Loading
// =========================================================================
async function loadManifest() {
  try {
    const res = await fetch("firmware/manifest.json", {
      cache: "no-cache",
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    manifestData = await res.json();
    populateVersionDropdown(manifestData);
    ui.fwSourceBadge.textContent = `${manifestData.versions.length} version(s) available`;
    if (manifestData.default_version) {
      ui.versionSelect.value = manifestData.default_version;
      await onVersionSelectionChanged();
    }
  } catch (err) {
    // Fallback when opened directly from repo root or without manifest
    ui.fwSourceBadge.textContent = "Local / Direct Mode";
    log(
      "No firmware/manifest.json found; checking ../build/apc_mini_app.syx fallback...",
      "warn",
    );
    try {
      const fallbackRes = await fetch("../build/apc_mini_app.syx", {
        cache: "no-cache",
      });
      if (fallbackRes.ok) {
        const buf = new Uint8Array(await fallbackRes.arrayBuffer());
        const opt = document.createElement("option");
        opt.value = "__build_fallback__";
        opt.textContent = "Local build (../build/apc_mini_app.syx)";
        ui.versionSelect.insertBefore(opt, ui.versionSelect.firstChild);
        ui.versionSelect.value = "__build_fallback__";
        const decoded = decodeSyxToBundle(buf);
        setLoadedBundle("build/apc_mini_app.syx", decoded);
        log(
          `Loaded local build/apc_mini_app.syx (checksum 0x${decoded.checksum.toString(16).toUpperCase().padStart(4, "0")}).`,
          "ok",
        );
        return;
      }
    } catch (_) {
      // Ignore fallback error
    }
    ui.versionSelect.value = "__local__";
    await onVersionSelectionChanged();
  }
}

function populateVersionDropdown(manifest) {
  ui.versionSelect.innerHTML = "";
  for (const v of manifest.versions || []) {
    const opt = document.createElement("option");
    opt.value = v.id;
    opt.textContent = `${v.label} [chk ${v.checksum}]`;
    ui.versionSelect.appendChild(opt);
  }
  const localOpt = document.createElement("option");
  localOpt.value = "__local__";
  localOpt.textContent = "Custom local file (.syx or .bin)...";
  ui.versionSelect.appendChild(localOpt);
}

async function onVersionSelectionChanged() {
  const val = ui.versionSelect.value;
  if (val === "__local__") {
    ui.localFileZone.classList.remove("hidden");
    clearLoadedBundle();
    return;
  }
  ui.localFileZone.classList.add("hidden");
  if (!manifestData) return;

  const entry = (manifestData.versions || []).find((v) => v.id === val);
  if (!entry) return;

  try {
    clearLoadedBundle();
    log(`Fetching firmware ${entry.id} (${entry.syx_url})...`);
    const res = await fetch(entry.syx_url, {
      cache: "no-cache",
    });
    if (!res.ok)
      throw new Error(`HTTP ${res.status} fetching ${entry.syx_url}`);
    const bytes = new Uint8Array(await res.arrayBuffer());
    const bundle = decodeSyxToBundle(bytes);
    setLoadedBundle(entry.id, bundle);
    await loadSymbolsTable(entry.symbols_url || "firmware/symbols.json");
    log(
      `Loaded ${entry.label}: ${bundle.binBytes.length} B, checksum 0x${bundle.checksum.toString(16).toUpperCase().padStart(4, "0")}.`,
      "ok",
    );
  } catch (err) {
    log(`Failed to load firmware ${entry.id}: ${err.message}`, "err");
  }
}

ui.versionSelect.addEventListener("change", onVersionSelectionChanged);

// Local file drag-and-drop & picker
ui.localFileZone.addEventListener("click", () => ui.fileInput.click());
ui.localFileZone.addEventListener("dragover", (e) => {
  e.preventDefault();
  ui.localFileZone.classList.add("dragover");
});
ui.localFileZone.addEventListener("dragleave", () => {
  ui.localFileZone.classList.remove("dragover");
});
ui.localFileZone.addEventListener("drop", async (e) => {
  e.preventDefault();
  ui.localFileZone.classList.remove("dragover");
  if (e.dataTransfer.files.length > 0) {
    await handleLocalFile(e.dataTransfer.files[0]);
  }
});
ui.fileInput.addEventListener("change", async () => {
  if (ui.fileInput.files.length > 0) {
    await handleLocalFile(ui.fileInput.files[0]);
  }
});

async function handleLocalFile(file) {
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    let bundle;
    if (
      file.name.toLowerCase().endsWith(".syx") ||
      (bytes.length !== APP_TOTAL_SIZE && bytes[0] === 0xf0)
    ) {
      bundle = decodeSyxToBundle(bytes);
    } else {
      bundle = encodeFirmwareSteps(bytes);
    }
    ui.dropzonePrompt.innerHTML = `Loaded <strong>${file.name}</strong> (${bytes.length.toLocaleString()} B)`;
    setLoadedBundle(file.name, bundle);
    log(
      `Validated local file '${file.name}': checksum 0x${bundle.checksum.toString(16).toUpperCase().padStart(4, "0")}.`,
      "ok",
    );
  } catch (err) {
    clearLoadedBundle();
    log(`Invalid firmware file '${file.name}': ${err.message}`, "err");
  }
}

function triggerDownload(bytes, filename) {
  const blob = new Blob([bytes], {
    type: "application/octet-stream",
  });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

ui.btnDownloadSyx.addEventListener("click", () => {
  if (!activeBundle) return;
  const safe = activeBundle.versionLabel.replace(/[^a-zA-Z0-9._-]/g, "_");
  triggerDownload(activeBundle.syxBytes, `apc_mini_${safe}.syx`);
});

ui.btnDownloadBin.addEventListener("click", () => {
  if (!activeBundle) return;
  const safe = activeBundle.versionLabel.replace(/[^a-zA-Z0-9._-]/g, "_");
  triggerDownload(activeBundle.binBytes, `apc_mini_${safe}.bin`);
});

// =========================================================================
// WebMIDI Port Management & Handshake
// =========================================================================
function isApcPort(port) {
  const n = (port.name || "").toLowerCase();
  return n.includes("apc") || n.includes("monome") || n.includes("updater");
}

function refreshMidiPorts({ preferUpdater = false } = {}) {
  if (!midiAccess) return;
  const prevOut = preferUpdater ? "" : ui.midiOutSelect.value;
  const prevIn = preferUpdater ? "" : ui.midiInSelect.value;

  const outputs = Array.from(midiAccess.outputs.values()).filter(
    (p) => p.state !== "disconnected" && isApcPort(p),
  );
  const inputs = Array.from(midiAccess.inputs.values()).filter(
    (p) => p.state !== "disconnected" && isApcPort(p),
  );

  ui.midiOutSelect.innerHTML = "";
  ui.midiInSelect.innerHTML = "";

  if (outputs.length === 0) {
    const opt = document.createElement("option");
    opt.value = "";
    opt.textContent = "No APC Mini Output port found";
    ui.midiOutSelect.appendChild(opt);
  } else {
    for (const p of outputs) {
      const opt = document.createElement("option");
      opt.value = p.id;
      opt.textContent = p.name || p.id;
      ui.midiOutSelect.appendChild(opt);
    }
  }

  if (inputs.length === 0) {
    const opt = document.createElement("option");
    opt.value = "";
    opt.textContent = "No APC Mini Input port found";
    ui.midiInSelect.appendChild(opt);
  } else {
    for (const p of inputs) {
      const opt = document.createElement("option");
      opt.value = p.id;
      opt.textContent = p.name || p.id;
      ui.midiInSelect.appendChild(opt);
    }
  }

  const pickPort = (list) => {
    if (preferUpdater) {
      const updater = list.find((p) =>
        (p.name || "").toLowerCase().includes("updater"),
      );
      if (updater) return updater;
      const apc = list.find((p) =>
        (p.name || "").toLowerCase().includes("apc"),
      );
      if (apc) return apc;
    }
    return list[0] || null;
  };

  const apcOut = pickPort(outputs);
  const apcIn = pickPort(inputs);

  if (prevOut && outputs.some((p) => p.id === prevOut)) {
    ui.midiOutSelect.value = prevOut;
  } else if (apcOut) {
    ui.midiOutSelect.value = apcOut.id;
  } else {
    ui.midiOutSelect.value = "";
  }

  if (prevIn && inputs.some((p) => p.id === prevIn)) {
    ui.midiInSelect.value = prevIn;
  } else if (apcIn) {
    ui.midiInSelect.value = apcIn.id;
  } else {
    ui.midiInSelect.value = "";
  }

  if (apcOut && apcIn) {
    ui.midiStatusDot.className = "dot ok";
    ui.midiStatusText.textContent = apcOut.name;
  } else {
    ui.midiStatusDot.className = "dot err";
    ui.midiStatusText.textContent = "APC Mini Not Connected";
  }

  updateControlsState();
}

function clearAllInputListeners(accessObj) {
  if (!accessObj) return;
  for (const input of accessObj.inputs.values()) {
    input.onmidimessage = null;
  }
}

ui.midiInSelect.addEventListener("change", updateControlsState);
ui.midiOutSelect.addEventListener("change", updateControlsState);

async function connectWebMidi({ silent = false, preferUpdater = false } = {}) {
  if (!window.isSecureContext) {
    ui.midiStatusDot.className = "dot err";
    ui.midiStatusText.textContent = "Insecure Context";
    if (!silent) {
      log(
        "WebMIDI requires HTTPS or http://localhost (127.0.0.1). " +
          `Current origin (${location.origin}) is not a Secure Context.`,
        "err",
      );
    }
    return false;
  }
  if (!navigator.requestMIDIAccess) {
    ui.midiStatusDot.className = "dot err";
    ui.midiStatusText.textContent = "WebMIDI Unsupported";
    if (!silent) {
      log(
        "WebMIDI API is not available in this browser. Please use Chrome, Edge, Brave, or Opera.",
        "err",
      );
    }
    return false;
  }
  try {
    if (midiAccess) {
      midiAccess.onstatechange = null;
      clearAllInputListeners(midiAccess);
    }
    midiAccess = await navigator.requestMIDIAccess({
      sysex: true,
    });
    midiAccess.onstatechange = () => {
      if (!isFlashing) {
        refreshMidiPorts({ preferUpdater });
      }
    };
    refreshMidiPorts({ preferUpdater });
    if (!silent) {
      log("WebMIDI connected (SysEx enabled).", "ok");
    }
    return true;
  } catch (err) {
    ui.midiStatusDot.className = "dot err";
    ui.midiStatusText.textContent = "Permission Denied";
    if (!silent) {
      log(`Failed to acquire WebMIDI SysEx access: ${err.message}`, "err");
    }
    return false;
  }
}

ui.btnConnectMidi.addEventListener("click", async () => {
  const ok = await connectWebMidi();
  if (ok && ui.midiOutSelect.value && ui.midiInSelect.value) {
    await readTelemetryAndCrash({ quiet: false });
  }
});

function getSelectedPorts() {
  if (!midiAccess) throw new Error("WebMIDI not connected");
  const outPort = midiAccess.outputs.get(ui.midiOutSelect.value);
  const inPort = midiAccess.inputs.get(ui.midiInSelect.value);
  if (!outPort || !inPort) {
    throw new Error("MIDI Output/Input ports not selected or disconnected");
  }
  return { outPort, inPort };
}

function sendSysexAndWaitReply(
  outPort,
  inPort,
  msgBytes,
  timeoutMs = 3000,
  { ignoreUnlockAck = false } = {},
) {
  return new Promise((resolve, reject) => {
    clearAllInputListeners(midiAccess);
    let settled = false;

    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      inPort.onmidimessage = null;
      reject(new Error("Timed out waiting for SysEx reply from APC Mini"));
    }, timeoutMs);

    inPort.onmidimessage = (ev) => {
      if (settled) return;
      const rxData = ev.data;
      if (!rxData || rxData.length < 3) return;
      if (rxData[0] !== 0xf0 || rxData[rxData.length - 1] !== 0xf7) return;
      if (
        ignoreUnlockAck &&
        rxData.length === 3 &&
        (rxData[1] === ACK_BL_UNLOCK || rxData[1] === ACK_APP_UNLOCK)
      ) {
        return;
      }
      settled = true;
      inPort.onmidimessage = null;
      clearTimeout(timer);
      resolve(rxData);
    };

    outPort.send(msgBytes);
  });
}

async function sendAndWaitAck(
  outPort,
  inPort,
  msgBytes,
  timeoutMs = 3000,
  options = {},
) {
  const rx = await sendSysexAndWaitReply(
    outPort,
    inPort,
    msgBytes,
    timeoutMs,
    options,
  );
  return rx[1];
}

// Probe device mode via Universal Device Inquiry (only Application responds to F0 7E 7F 06 01 F7)
if (ui.btnSwitchProfile) {
  ui.btnSwitchProfile.addEventListener("click", async () => {
    try {
      const { outPort, inPort } = getSelectedPorts();
      const isMonome = (outPort.name || "").toLowerCase().includes("monome");
      const targetVal = isMonome ? 0 : 1;
      const targetLabel = isMonome
        ? "APC MIDI (09E8:0028)"
        : "Monome serialosc + MIDI (0403:6001)";
      log(`Switching USB profile to ${targetLabel}...`, "warn");
      await outPort.open().catch(() => {});
      outPort.send(
        new Uint8Array([
          0xf0,
          0x7d,
          0x41,
          0x50,
          0x43,
          DIAG_SUB_SET_PROFILE,
          targetVal,
          0xf7,
        ]),
      );
      inPort.onmidimessage = null;
      await outPort.close().catch(() => {});
      await inPort.close().catch(() => {});
      await sleep(1600);
      for (let attempt = 0; attempt < 8; attempt++) {
        await connectWebMidi({ silent: true });
        if (ui.midiOutSelect.value && ui.midiInSelect.value) {
          await readTelemetryAndCrash({ quiet: false });
          break;
        }
        await sleep(500);
      }
    } catch (err) {
      log(`Profile switch failed: ${err.message}`, "err");
    }
  });
}

ui.btnProbeDevice.addEventListener("click", async () => {
  try {
    const { outPort, inPort } = getSelectedPorts();
    log(`Sending Universal Device Inquiry to '${outPort.name}'...`);
    const rx = await sendSysexAndWaitReply(
      outPort,
      inPort,
      INQUIRY_SYSEX,
      1200,
    );
    if (rx[1] === 0x7e && rx[3] === 0x06 && rx[4] === 0x02) {
      ui.deviceModeLabel.textContent =
        "Mode: Application (APC MINI + serialosc)";
      log(
        `APC Mini responded in Application mode on '${outPort.name}' (Simultaneous USB-MIDI + serialosc).`,
        "ok",
      );
      await readTelemetryAndCrash({ quiet: true });
    } else {
      log(
        `Received SysEx reply: ${Array.from(rx)
          .map((b) => b.toString(16).padStart(2, "0"))
          .join(" ")}`,
        "info",
      );
    }
  } catch (_) {
    ui.deviceModeLabel.textContent = "Mode: Bootloader or No Reply";
    log(
      "No Application identity reply (device may already be in Bootloader mode). Ready to unlock & flash.",
      "warn",
    );
  }
});

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function enterAndUnlockBootloader() {
  if (!midiAccess) {
    await connectWebMidi();
  }
  let { outPort, inPort } = getSelectedPorts();
  await outPort.open().catch(() => {});
  await inPort.open().catch(() => {});
  log(`Sending bootloader unlock SysEx to '${outPort.name}'...`);
  let code = await sendAndWaitAck(outPort, inPort, UNLOCK_SYSEX, 3000);

  if (code === ACK_BL_UNLOCK) {
    ui.deviceModeLabel.textContent = "Mode: Bootloader Unlocked (0x3E)";
    log("Bootloader already active and unlocked (ACK 0x3E).", "ok");
    await sleep(100);
    return { outPort, inPort };
  }

  if (code === ACK_APP_UNLOCK) {
    ui.deviceModeLabel.textContent = "Mode: Rebooting to DFU...";
    log(
      "Application accepted unlock (ACK 0x15). Waiting for USB re-enumeration into bootloader...",
      "warn",
    );
    inPort.onmidimessage = null;
    await outPort.close().catch(() => {});
    await inPort.close().catch(() => {});
    await sleep(1800);

    log("Re-connecting WebMIDI to bootloader port...");
    const deadline = performance.now() + 15000;
    while (performance.now() < deadline) {
      await connectWebMidi({
        silent: true,
        preferUpdater: true,
      });
      try {
        ({ outPort, inPort } = getSelectedPorts());
        await outPort.open().catch(() => {});
        await inPort.open().catch(() => {});
        code = await sendAndWaitAck(outPort, inPort, UNLOCK_SYSEX, 1500);
        if (code === ACK_BL_UNLOCK) {
          ui.deviceModeLabel.textContent = "Mode: Bootloader Unlocked (0x3E)";
          log(`Bootloader unlocked on '${outPort.name}' (ACK 0x3E).`, "ok");
          await sleep(150);
          return { outPort, inPort };
        }
      } catch (_) {
        // Keep polling while USB re-enumerates
      }
      await sleep(500);
    }
    throw new Error(
      "Timed out waiting for APC Mini to re-enumerate in Bootloader mode (0x3E).",
    );
  }

  throw new Error(
    `Unexpected unlock ACK code: 0x${code.toString(16).toUpperCase().padStart(2, "0")}`,
  );
}

// =========================================================================
// Firmware Flashing Loop
// =========================================================================
async function flashCurrentBundle() {
  if (!activeBundle || isFlashing) return;
  isFlashing = true;
  updateControlsState();

  const steps = activeBundle.steps;
  const numPages = steps.filter((s) => s.kind === "flash_write").length;
  const t0 = performance.now();

  ui.progressBar.style.width = "0%";
  ui.progressPageText.textContent = `Page 0 / ${numPages}`;
  ui.progressAddrText.textContent = "0x08002000";
  ui.progressPctText.textContent = "0%";
  ui.flashStateLabel.textContent = "Unlocking...";

  try {
    const { outPort, inPort } = await enterAndUnlockBootloader();
    ui.flashStateLabel.textContent = "Flashing...";
    log(
      `Starting flash of ${activeBundle.versionLabel} (${steps.length} SysEx steps across ${numPages} pages)...`,
    );

    for (let i = 0; i < steps.length; i++) {
      const step = steps[i];
      if (step.kind === "finalize") {
        ui.flashStateLabel.textContent = "Finalizing...";
        log(
          "Verifying application checksum & rebooting into firmware (0x0E)...",
        );
      }

      const timeoutMs =
        step.kind === "flash_write" || step.kind === "finalize" ? 5000 : 3000;
      const ack = await sendAndWaitAck(outPort, inPort, step.sysex, timeoutMs, {
        ignoreUnlockAck: true,
      });

      if (ack !== step.expectedAck) {
        const hexAck = `0x${ack.toString(16).toUpperCase().padStart(2, "0")}`;
        if (step.kind === "ram_load") {
          throw new Error(
            `RAM load failed at page 0x${step.flashOffset.toString(16).toUpperCase().padStart(4, "0")}` +
              `+0x${step.chunkOffset.toString(16).toUpperCase().padStart(3, "0")}: ACK ${hexAck}`,
          );
        }
        if (step.kind === "flash_write") {
          throw new Error(
            `Flash write failed at 0x0800${step.flashOffset.toString(16).toUpperCase().padStart(4, "0")}: ACK ${hexAck}`,
          );
        }
        throw new Error(`Finalization failed: ACK ${hexAck} (expected 0x77)`);
      }

      if (step.kind === "ram_load" && (step.chunkOffset & 0xff) === 0) {
        const overallPct = Math.floor(((i + 1) * 100) / steps.length);
        ui.progressBar.style.width = `${overallPct}%`;
        ui.progressPctText.textContent = `${overallPct}%`;
        ui.progressAddrText.textContent = `0x0800${step.flashOffset.toString(16).toUpperCase().padStart(4, "0")}+0x${step.chunkOffset.toString(16).toUpperCase().padStart(3, "0")}`;
      } else if (step.kind === "flash_write") {
        const pageNum = step.pageIdx + 1;
        const pct = Math.floor((pageNum * 100) / numPages);
        ui.progressBar.style.width = `${pct}%`;
        ui.progressPageText.textContent = `Page ${pageNum} / ${numPages}`;
        ui.progressAddrText.textContent = `0x0800${step.flashOffset.toString(16).toUpperCase().padStart(4, "0")}`;
        ui.progressPctText.textContent = `${pct}%`;
        log(
          `Flashed page ${pageNum}/${numPages} (0x0800${step.flashOffset.toString(16).toUpperCase().padStart(4, "0")}) [ACK 0x1A]`,
        );
      }
    }

    const elapsedS = ((performance.now() - t0) / 1000).toFixed(1);
    ui.progressBar.style.width = "100%";
    ui.progressPctText.textContent = "100%";
    ui.flashStateLabel.textContent = "Complete (0x77)";
    ui.deviceModeLabel.textContent = "Mode: Application Rebooted";
    log(
      `Success! Bootloader verified checksum (ACK 0x77) and launched application in ${elapsedS}s.`,
      "ok",
    );

    // Close bootloader handles and wait for application USB re-enumeration (09E8:0028 or 0403:6001)
    inPort.onmidimessage = null;
    await outPort.close().catch(() => {});
    await inPort.close().catch(() => {});
    await sleep(1400);

    for (let attempt = 0; attempt < 10; attempt++) {
      await connectWebMidi({ silent: true });
      if (ui.midiOutSelect.value && ui.midiInSelect.value) {
        try {
          const ports = getSelectedPorts();
          await ports.outPort.open().catch(() => {});
          await ports.inPort.open().catch(() => {});
          const rx = await sendSysexAndWaitReply(
            ports.outPort,
            ports.inPort,
            INQUIRY_SYSEX,
            700,
          );
          if (rx && rx[1] === 0x7e) {
            ui.deviceModeLabel.textContent =
              "Mode: Application (APC MINI + serialosc)";
            break;
          }
        } catch (_) {
          // Keep polling while application finishes USB enumeration
        }
      }
      await sleep(600);
    }
  } catch (err) {
    ui.flashStateLabel.textContent = "Error";
    log(`Flash aborted: ${err.message}`, "err");
  } finally {
    isFlashing = false;
    updateControlsState();
  }
  if (ui.midiOutSelect.value && ui.midiInSelect.value) {
    await readTelemetryAndCrash({ quiet: true });
  }
}

ui.btnFlash.addEventListener("click", flashCurrentBundle);

// =========================================================================
// 4. SWD-Less Diagnostics, Post-Mortem Black-Box & Memory Peek
// =========================================================================
const DIAG_SUB_TELEMETRY = 0x01;
const DIAG_SUB_CRASH_GET = 0x02;
const DIAG_SUB_CRASH_CLR = 0x03;
const DIAG_SUB_PEEK_MEM = 0x04;
const DIAG_SUB_TEST_FAULT = 0x05;
const DIAG_SUB_SET_MODES = 0x06;
const APC_CRASH_MAGIC = 0x43525348; // "CRSH"

function syncModeControlsFromFirmware(flags, palBri) {
  if (ui.chkModeMidi) ui.chkModeMidi.checked = (flags & 0x01) !== 0;
  if (ui.chkModeSerialosc) ui.chkModeSerialosc.checked = (flags & 0x02) !== 0;
  if (ui.chkModeLua) ui.chkModeLua.checked = (flags & 0x04) !== 0;
  if (ui.chkModeAbleton) ui.chkModeAbleton.checked = (flags & 0x08) !== 0;
  if (ui.selPalette) ui.selPalette.value = String(palBri & 0x03);
  if (ui.selBrightness) ui.selBrightness.value = String((palBri >> 4) & 0x03);
}

async function applyModeControlsToFirmware() {
  if (isFlashing || isDiagBusy) return;
  let flags = 0;
  if (ui.chkModeMidi && ui.chkModeMidi.checked) flags |= 0x01;
  if (ui.chkModeSerialosc && ui.chkModeSerialosc.checked) flags |= 0x02;
  if (ui.chkModeLua && ui.chkModeLua.checked) flags |= 0x04;
  if (ui.chkModeAbleton && ui.chkModeAbleton.checked) flags |= 0x08;
  const pal = ui.selPalette ? parseInt(ui.selPalette.value, 10) & 3 : 0;
  const bri = ui.selBrightness ? parseInt(ui.selBrightness.value, 10) & 3 : 3;

  isDiagBusy = true;
  updateControlsState();
  try {
    const rep = await sendDiagRequest(DIAG_SUB_SET_MODES, [flags, pal, bri]);
    if (rep && rep.length >= 2) {
      syncModeControlsFromFirmware(rep[0], rep[1]);
    }
    log(
      `Modes updated via SysEx: MIDI=${flags & 1 ? "ON" : "OFF"}, serialosc=${flags & 2 ? "ON" : "OFF"}, Lua=${flags & 4 ? "ON" : "OFF"}, Ableton=${flags & 8 ? "ON" : "OFF"} (Palette=${pal}, Brightness=${bri}).`,
      "ok",
    );
  } catch (err) {
    log(`Failed to update modes via SysEx: ${err.message}`, "err");
  } finally {
    isDiagBusy = false;
    updateControlsState();
  }
}

for (const el of [
  ui.chkModeMidi,
  ui.chkModeSerialosc,
  ui.chkModeLua,
  ui.chkModeAbleton,
  ui.selPalette,
  ui.selBrightness,
]) {
  if (el) el.addEventListener("change", applyModeControlsToFirmware);
}

const IPSR_NAMES = {
  2: "NMI (Non-Maskable Interrupt / HSE Clock Security)",
  3: "HardFault",
  4: "MemManage Fault",
  5: "BusFault",
  6: "UsageFault",
};

const CFSR_BITS = [
  [0, "IACCVIOL (Instruction access violation)"],
  [1, "DACCVIOL (Data access violation)"],
  [3, "MUNSTKERR (MemManage unstacking error)"],
  [4, "MSTKERR (MemManage stacking error)"],
  [7, "MMARVALID (MMFAR valid)"],
  [8, "IBUSERR (Instruction bus error)"],
  [9, "PRECISERR (Precise data bus error)"],
  [10, "IMPRECISERR (Imprecise data bus error)"],
  [11, "UNSTKERR (BusFault unstacking error)"],
  [12, "STKERR (BusFault stacking error)"],
  [15, "BFARVALID (BFAR valid)"],
  [16, "UNDEFINSTR (Undefined instruction)"],
  [17, "INVSTATE (Invalid EPSR/Thumb state)"],
  [18, "INVPC (Invalid EXC_RETURN)"],
  [19, "NOCP (No coprocessor)"],
  [24, "UNALIGNED (Unaligned memory access)"],
  [25, "DIVBYZERO (Divide by zero)"],
];

const HFSR_BITS = [
  [1, "VECTTBL (Vector table read fault)"],
  [30, "FORCED (Escalated configurable fault)"],
  [31, "DEBUGEVT (Debug event)"],
];

async function loadSymbolsTable(url = "firmware/symbols.json") {
  try {
    const res = await fetch(url, { cache: "no-cache" });
    if (res.ok) {
      activeSymbols = await res.json();
    }
  } catch (_) {
    // Symbol resolution is optional
  }
}

export function resolveSymbolFromTable(addr, symbols = activeSymbols) {
  if (!addr || !symbols || symbols.length === 0) return "";
  const clean = (addr >>> 0) & ~1;
  let best = null;
  for (const s of symbols) {
    if (s.addr <= clean) {
      best = s;
    } else {
      break;
    }
  }
  if (!best) return "";
  const maxSpan = best.size > 0 ? best.size : 512;
  const diff = clean - best.addr;
  if (diff > maxSpan) return "";
  return ` (${best.name}+0x${diff.toString(16)})`;
}

export function decodeDiagNibbles(rxBytes) {
  if (
    rxBytes.length < 7 ||
    rxBytes[0] !== 0xf0 ||
    rxBytes[rxBytes.length - 1] !== 0xf7 ||
    rxBytes[1] !== 0x7d ||
    rxBytes[2] !== 0x41 ||
    rxBytes[3] !== 0x50 ||
    rxBytes[4] !== 0x43
  ) {
    throw new Error("Invalid diagnostic SysEx header");
  }
  const subcmd = rxBytes[5];
  const nibLen = rxBytes.length - 7;
  if (nibLen % 2 !== 0) {
    throw new Error("Odd nibble count in diagnostic SysEx payload");
  }
  const out = new Uint8Array(nibLen / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = ((rxBytes[6 + i * 2] & 0x0f) << 4) | (rxBytes[7 + i * 2] & 0x0f);
  }
  return { subcmd, payload: out };
}

export function encodeAddrNibbles(addr) {
  const u = addr >>> 0;
  const bytes = [
    u & 0xff,
    (u >>> 8) & 0xff,
    (u >>> 16) & 0xff,
    (u >>> 24) & 0xff,
  ];
  const nibs = [];
  for (const b of bytes) {
    nibs.push((b >>> 4) & 0x0f, b & 0x0f);
  }
  return nibs;
}

function decodeResetCauses(flags) {
  const list = [];
  if (flags & 0x04) list.push("IWDGRST (Watchdog Timeout!)");
  if (flags & 0x08) list.push("WWDGRST");
  if (flags & 0x10) list.push("SFTRST (SW/DFU)");
  if (flags & 0x20) list.push("PORRST (Power-On)");
  if (flags & 0x40) list.push("PINRST (NRST)");
  if (flags & 0x80) list.push("LPWRRST");
  return list.length > 0
    ? list.join(", ")
    : `0x${flags.toString(16).padStart(2, "0")}`;
}

function attachPassiveMidiMonitor() {
  if (!midiAccess || isFlashing || isDiagBusy) return;
  const inPort = midiAccess.inputs.get(ui.midiInSelect.value);
  if (!inPort) return;
  inPort.onmidimessage = (ev) => {
    const d = ev.data;
    if (!d || d.length < 3) return;
    const st = d[0] & 0xf0;
    if (st === 0x90 || st === 0x80) {
      const note = d[1];
      const vel = d[2];
      const kind = st === 0x90 && vel > 0 ? "NoteOn" : "NoteOff";
      const coord =
        note < 64
          ? `Pad (x=${note % 8}, y=${7 - Math.floor(note / 8)})`
          : note === 0x62
            ? "SHIFT"
            : `Btn 0x${note.toString(16).toUpperCase()}`;
      ui.telemLastMidi.textContent = `${kind} ${coord} [0x${note.toString(16).toUpperCase()} vel=${vel}]`;
    } else if (st === 0xb0) {
      const cc = d[1];
      const val = d[2];
      const fIdx =
        cc >= 0x30 && cc <= 0x38
          ? `Fader ${cc - 0x30 + 1}`
          : `CC 0x${cc.toString(16)}`;
      ui.telemLastMidi.textContent = `${fIdx} = ${val}`;
    }
  };
}

async function sendDiagRequest(subcmd, extraBytes = [], timeoutMs = 1500) {
  const { outPort, inPort } = getSelectedPorts();
  await outPort.open().catch(() => {});
  await inPort.open().catch(() => {});
  const req = new Uint8Array([
    0xf0,
    0x7d,
    0x41,
    0x50,
    0x43,
    subcmd,
    ...extraBytes,
    0xf7,
  ]);
  const rx = await sendSysexAndWaitReply(outPort, inPort, req, timeoutMs);
  attachPassiveMidiMonitor();
  const decoded = decodeDiagNibbles(rx);
  if (decoded.subcmd !== subcmd) {
    throw new Error(
      `Mismatched diagnostic reply subcmd 0x${decoded.subcmd.toString(16)}`,
    );
  }
  return decoded.payload;
}

function hex32(val) {
  return "0x" + (val >>> 0).toString(16).toUpperCase().padStart(8, "0");
}

function renderCrashLog(payload) {
  if (!payload || payload.length < 48) return;
  const dv = new DataView(
    payload.buffer,
    payload.byteOffset,
    payload.byteLength,
  );
  const magic = dv.getUint32(0, true);
  if (magic !== APC_CRASH_MAGIC) {
    ui.crashBox.classList.remove("hidden");
    ui.crashBox.classList.add("clean");
    ui.crashTitle.textContent = "CRASH LOG: CLEAN";
    ui.crashPre.textContent = "No crash log recorded in Flash Page 30.";
    lastCrashMarkdown = "";
    return;
  }

  const ipsr = dv.getUint32(4, true);
  const faultMsp = dv.getUint32(8, true);
  const cfsr = dv.getUint32(12, true);
  const hfsr = dv.getUint32(16, true);
  const far = dv.getUint32(20, true);
  const r0 = dv.getUint32(24, true);
  const r1 = dv.getUint32(28, true);
  const r2 = dv.getUint32(32, true);
  const lr = dv.getUint32(36, true);
  const pc = dv.getUint32(40, true);
  const uptimeTicks = dv.getUint32(44, true);

  const excName = IPSR_NAMES[ipsr & 0x1ff] || `Exception #${ipsr & 0x1ff}`;
  const cfsrFlags = CFSR_BITS.filter(([b]) => (cfsr & (1 << b)) !== 0).map(
    ([, d]) => d,
  );
  const hfsrFlags = HFSR_BITS.filter(([b]) => (hfsr & (1 << b)) !== 0).map(
    ([, d]) => d,
  );

  const pcSym = resolveSymbolFromTable(pc);
  const lrSym = resolveSymbolFromTable(lr);

  const reportText = [
    `Exception  : ${excName} (IPSR=${hex32(ipsr)})`,
    `Uptime     : ${(uptimeTicks * 0.5).toFixed(1)} ms (${uptimeTicks} ticks)`,
    `Fault PC   : ${hex32(pc)}${pcSym}`,
    `Caller LR  : ${hex32(lr)}${lrSym}`,
    `Fault MSP  : ${hex32(faultMsp)}`,
    `SCB->CFSR  : ${hex32(cfsr)} (${cfsrFlags.join(", ") || "None"})`,
    `SCB->HFSR  : ${hex32(hfsr)} (${hfsrFlags.join(", ") || "None"})`,
    `Fault Addr : ${hex32(far)} (BFAR/MMFAR)`,
    `Registers  : R0=${hex32(r0)}  R1=${hex32(r1)}  R2=${hex32(r2)}`,
  ].join("\n");

  ui.crashBox.classList.remove("hidden", "clean");
  ui.crashTitle.textContent = `CRASH DETECTED (${excName})`;
  ui.crashPre.textContent = reportText;
  lastCrashMarkdown = "```text\n=== CRASH LOG ===\n" + reportText + "\n```";
  log(`Crash log found: ${excName} at PC=${hex32(pc)}${pcSym}`, "err");
}

async function readTelemetryAndCrash({ quiet = false } = {}) {
  if (isFlashing || isDiagBusy) return;
  isDiagBusy = true;
  updateControlsState();
  try {
    if (activeSymbols.length === 0) {
      await loadSymbolsTable();
    }
    const telem = await sendDiagRequest(DIAG_SUB_TELEMETRY);
    const dv = new DataView(telem.buffer, telem.byteOffset, telem.byteLength);
    const rstFlags = dv.getUint8(0);
    const crashPresent = dv.getUint8(1);
    const flashKb = dv.getUint16(2, true);
    const uid0 = dv.getUint32(4, true);
    const uptimeTicks = dv.getUint32(8, true);
    const loopCnt = dv.getUint32(12, true);
    const maxLoopTicks = dv.getUint16(16, true);
    const faders = Array.from(telem.subarray(18, 27));
    const shift = dv.getUint8(27);
    if (telem.length >= 30) {
      syncModeControlsFromFirmware(dv.getUint8(28), dv.getUint8(29));
    }

    const uptimeS = uptimeTicks * 0.0005;
    const hz = uptimeS > 0 ? Math.round(loopCnt / uptimeS) : 0;

    ui.telemSilicon.textContent = `STM32F102 (${flashKb} KB) UID=${hex32(uid0)}`;
    ui.telemReset.textContent = decodeResetCauses(rstFlags);
    ui.telemUptime.textContent = `${uptimeS.toFixed(1)} s (${uptimeTicks.toLocaleString()} t)`;
    ui.telemLoop.textContent = `~${hz.toLocaleString()} Hz (max ${(maxLoopTicks * 0.5).toFixed(1)} ms)`;
    ui.telemFaders.textContent = `[${faders.join(", ")}]`;
    ui.telemShift.textContent = shift ? "PRESSED (1)" : "0";
    ui.diagStatusLabel.textContent = crashPresent
      ? "Crash Log Present"
      : "Healthy (0x08007800 Clean)";
    ui.deviceModeLabel.textContent =
      "Mode: Application (APC MINI + serialosc)";

    const crashRaw = await sendDiagRequest(DIAG_SUB_CRASH_GET);
    renderCrashLog(crashRaw);

    if (!quiet) {
      log(
        `Telemetry: ${flashKb} KB Flash, UID=${hex32(uid0)}, loop ~${hz} Hz (max ${(maxLoopTicks * 0.5).toFixed(1)} ms).`,
        "ok",
      );
    }
  } catch (err) {
    if (!quiet) {
      log(`Diagnostics query failed: ${err.message}`, "warn");
    }
  } finally {
    isDiagBusy = false;
    updateControlsState();
  }
}

ui.btnReadDiag.addEventListener("click", () =>
  readTelemetryAndCrash({ quiet: false }),
);

ui.btnLivePoll.addEventListener("click", () => {
  if (livePollTimer) {
    clearInterval(livePollTimer);
    livePollTimer = null;
    ui.btnLivePoll.textContent = "Start Live Poll (1 Hz)";
    log("Stopped 1 Hz live telemetry polling.");
  } else {
    ui.btnLivePoll.textContent = "Stop Live Poll (1 Hz)";
    readTelemetryAndCrash({ quiet: true });
    livePollTimer = setInterval(() => {
      if (!isFlashing && !isDiagBusy) {
        readTelemetryAndCrash({ quiet: true });
      }
    }, 1000);
    log("Started 1 Hz live telemetry & MIDI input monitor.", "ok");
  }
});

ui.btnCopyCrash.addEventListener("click", async () => {
  const text = lastCrashMarkdown || ui.crashPre.textContent;
  if (!text) return;
  await navigator.clipboard.writeText(text);
  log("Copied crash report to clipboard.", "ok");
});

ui.btnClearCrash.addEventListener("click", async () => {
  try {
    const res = await sendDiagRequest(DIAG_SUB_CRASH_CLR);
    log(`Cleared crash log (status=${res[0]}).`, "ok");
    await readTelemetryAndCrash({ quiet: true });
  } catch (err) {
    log(`Failed to clear crash log: ${err.message}`, "err");
  }
});

ui.btnPeekMem.addEventListener("click", async () => {
  if (isFlashing || isDiagBusy) return;
  const rawAddr = parseInt(ui.peekAddrInput.value.trim(), 16);
  if (Number.isNaN(rawAddr)) {
    log(`Invalid hex address: '${ui.peekAddrInput.value}'`, "err");
    return;
  }
  isDiagBusy = true;
  updateControlsState();
  try {
    const lines = [];
    for (let off = 0; off < 64; off += 16) {
      const cur = (rawAddr + off) >>> 0;
      const chunk = await sendDiagRequest(
        DIAG_SUB_PEEK_MEM,
        encodeAddrNibbles(cur),
      );
      const hexCols = Array.from(chunk)
        .map((b) => b.toString(16).toUpperCase().padStart(2, "0"))
        .join(" ");
      const asciiCols = Array.from(chunk)
        .map((b) => (b >= 32 && b < 127 ? String.fromCharCode(b) : "."))
        .join("");
      lines.push(`${hex32(cur)}: ${hexCols}  |${asciiCols}|`);
    }
    ui.peekPre.classList.remove("hidden");
    ui.peekPre.textContent = lines.join("\n");
    log(`Peeked 64 B at ${hex32(rawAddr)}.`, "ok");
  } catch (err) {
    log(`Memory peek at ${hex32(rawAddr)} failed: ${err.message}`, "err");
  } finally {
    isDiagBusy = false;
    updateControlsState();
  }
});

for (const btn of document.querySelectorAll(".preset-btn")) {
  btn.addEventListener("click", () => {
    ui.peekAddrInput.value = btn.dataset.addr;
    if (!ui.btnPeekMem.disabled) {
      ui.btnPeekMem.click();
    }
  });
}

ui.btnLedSweep.addEventListener("click", async () => {
  try {
    const { outPort } = getSelectedPorts();
    await outPort.open().catch(() => {});
    log("Running 8x8 Pad + Round LED test sweep (Green -> Yellow -> Red)...");
    for (const vel of [1, 5, 3]) {
      for (let note = 0; note < 64; note++) {
        outPort.send([0x90, note, vel]);
      }
      for (let note = 64; note <= 71; note++) {
        outPort.send([0x90, note, 1]);
      }
      for (let note = 82; note <= 89; note++) {
        outPort.send([0x90, note, 1]);
      }
      await sleep(250);
    }
    log("LED test sweep complete (click 'Clear All LEDs' to blank).", "ok");
  } catch (err) {
    log(`LED sweep failed: ${err.message}`, "err");
  }
});

ui.btnLedClear.addEventListener("click", async () => {
  try {
    const { outPort } = getSelectedPorts();
    await outPort.open().catch(() => {});
    for (let note = 0; note < 90; note++) {
      outPort.send([0x80, note, 0]);
    }
    log("Cleared all 64 pad LEDs and 16 round LEDs.", "ok");
  } catch (err) {
    log(`Clear LEDs failed: ${err.message}`, "err");
  }
});

ui.btnTriggerFault.addEventListener("click", async () => {
  try {
    const { outPort } = getSelectedPorts();
    await outPort.open().catch(() => {});
    log(
      "Triggering deliberate UDF #0 fault (0x05 0x5A) — device will log crash to 0x08007800 and reboot into DFU...",
      "warn",
    );
    outPort.send(
      new Uint8Array([
        0xf0,
        0x7d,
        0x41,
        0x50,
        0x43,
        DIAG_SUB_TEST_FAULT,
        0x5a,
        0xf7,
      ]),
    );
    ui.deviceModeLabel.textContent = "Mode: Fault -> DFU Reboot";
  } catch (err) {
    log(`Failed to send test fault: ${err.message}`, "err");
  }
});

// Initialize on page load: load firmware manifest & auto-connect WebMIDI
loadManifest();
connectWebMidi().then((ok) => {
  if (ok && ui.midiOutSelect.value && ui.midiInSelect.value) {
    readTelemetryAndCrash({ quiet: true });
  }
});
