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
  logBox: document.getElementById("logBox"),
  btnClearLog: document.getElementById("btnClearLog"),
};

let midiAccess = null;
let activeBundle = null; // { versionLabel, steps, binBytes, syxBytes, checksum }
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
  ui.btnProbeDevice.disabled = !hasMidiPorts || isFlashing;
  ui.btnFlash.disabled = !hasMidiPorts || !activeBundle || isFlashing;
  ui.btnDownloadSyx.disabled = !activeBundle;
  ui.btnDownloadBin.disabled = !activeBundle;
  ui.versionSelect.disabled = isFlashing;
  ui.midiOutSelect.disabled = !midiAccess || isFlashing;
  ui.midiInSelect.disabled = !midiAccess || isFlashing;
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
  return (port.name || "").toLowerCase().includes("apc");
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

ui.btnConnectMidi.addEventListener("click", () => connectWebMidi());

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
      ui.deviceModeLabel.textContent = "Mode: Application (0x47:0x28)";
      log(
        "APC Mini responded in Application mode (Universal Identity Reply received).",
        "ok",
      );
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

    // Reconnect WebMIDI after the device re-enumerates back into Application mode
    await sleep(1500);
    await connectWebMidi({ silent: true });
  } catch (err) {
    ui.flashStateLabel.textContent = "Error";
    log(`Flash aborted: ${err.message}`, "err");
  } finally {
    isFlashing = false;
    updateControlsState();
  }
}

ui.btnFlash.addEventListener("click", flashCurrentBundle);

// Initialize on page load: load firmware manifest & auto-connect WebMIDI
loadManifest();
connectWebMidi();
