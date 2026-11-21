import {
  DIAG_SUB_CRASH_CLR,
  DIAG_SUB_CRASH_GET,
  DIAG_SUB_PEEK_MEM,
  DIAG_SUB_SET_MODES,
  DIAG_SUB_TELEMETRY,
  DIAG_SUB_TEST_FAULT,
  createMidiSession,
  encodeAddrNibbles,
  hex32,
  sleep,
} from "./midi-core.js";

const APC_CRASH_MAGIC = 0x43525348; // "CRSH"

const ui = {
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
};

let activeSymbols = []; // [{ addr, size, name }, ...]
let lastCrashMarkdown = "";
let livePollTimer = null;

function updateConfigControls() {
  const portsReady = session.hasPorts();
  const canDiag = portsReady && !session.isBusy();
  ui.btnReadDiag.disabled = !canDiag;
  ui.btnLivePoll.disabled = !portsReady;
  ui.btnLedSweep.disabled = !canDiag;
  ui.btnLedClear.disabled = !canDiag;
  ui.btnTriggerFault.disabled = !canDiag;
  ui.btnPeekMem.disabled = !canDiag;
  ui.chkModeMidi.disabled = !canDiag;
  ui.chkModeSerialosc.disabled = !canDiag;
  ui.chkModeLua.disabled = !canDiag;
  ui.chkModeAbleton.disabled = !canDiag;
  ui.selPalette.disabled = !canDiag;
  ui.selBrightness.disabled = !canDiag;
}

function attachPassiveMidiMonitor() {
  if (!session.hasPorts() || session.isBusy()) return;
  try {
    const { inPort } = session.getSelectedPorts();
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
  } catch (_) {
    // Ignore when ports aren't ready
  }
}

const session = createMidiSession({
  onControlsUpdate: updateConfigControls,
  onConnected: () => readTelemetryAndCrash({ quiet: false }),
  onProbeSuccess: () => readTelemetryAndCrash({ quiet: true }),
  onAfterDiag: attachPassiveMidiMonitor,
});

// =========================================================================
// Mode, Palette & Brightness Controls
// =========================================================================
function syncModeControlsFromFirmware(flags, palBri) {
  ui.chkModeMidi.checked = (flags & 0x01) !== 0;
  ui.chkModeSerialosc.checked = (flags & 0x02) !== 0;
  ui.chkModeLua.checked = (flags & 0x04) !== 0;
  ui.chkModeAbleton.checked = (flags & 0x08) !== 0;
  ui.selPalette.value = String(palBri & 0x03);
  ui.selBrightness.value = String((palBri >> 4) & 0x03);
}

async function applyModeControlsToFirmware() {
  if (session.isBusy()) return;
  let flags = 0;
  if (ui.chkModeMidi.checked) flags |= 0x01;
  if (ui.chkModeSerialosc.checked) flags |= 0x02;
  if (ui.chkModeLua.checked) flags |= 0x04;
  if (ui.chkModeAbleton.checked) flags |= 0x08;
  const pal = parseInt(ui.selPalette.value, 10) & 3;
  const bri = parseInt(ui.selBrightness.value, 10) & 3;

  session.setBusy(true);
  try {
    const rep = await session.sendDiagRequest(DIAG_SUB_SET_MODES, [
      flags,
      pal,
      bri,
    ]);
    if (rep && rep.length >= 2) {
      syncModeControlsFromFirmware(rep[0], rep[1]);
    }
    session.log(
      `Modes updated via SysEx: MIDI=${flags & 1 ? "ON" : "OFF"}, serialosc=${flags & 2 ? "ON" : "OFF"}, Lua=${flags & 4 ? "ON" : "OFF"}, Ableton=${flags & 8 ? "ON" : "OFF"} (Palette=${pal}, Brightness=${bri}).`,
      "ok",
    );
  } catch (err) {
    session.log(`Failed to update modes via SysEx: ${err.message}`, "err");
  } finally {
    session.setBusy(false);
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
  el.addEventListener("change", applyModeControlsToFirmware);
}

// =========================================================================
// Symbols, Crash Log, Telemetry & Memory Peek
// =========================================================================
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

async function loadSymbolsTable() {
  try {
    const res = await fetch("firmware/symbols.json", { cache: "no-cache" });
    if (res.ok) {
      activeSymbols = await res.json();
      return;
    }
  } catch (_) {
    // Try manifest fallback
  }
  try {
    const mRes = await fetch("firmware/manifest.json", { cache: "no-cache" });
    if (!mRes.ok) return;
    const m = await mRes.json();
    const def = (m.versions || []).find((v) => v.id === m.default_version);
    if (def?.symbols_url) {
      const sRes = await fetch(def.symbols_url, { cache: "no-cache" });
      if (sRes.ok) activeSymbols = await sRes.json();
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

function decodeResetCauses(flags) {
  const list = [];
  if (flags & 0x04) list.push("PINRST (NRST)");
  if (flags & 0x08) list.push("PORRST (Power-On)");
  if (flags & 0x10) list.push("SFTRST (SW/DFU)");
  if (flags & 0x20) list.push("IWDGRST (Watchdog Timeout!)");
  if (flags & 0x40) list.push("WWDGRST");
  if (flags & 0x80) list.push("LPWRRST");
  return list.length > 0
    ? list.join(", ")
    : `0x${flags.toString(16).padStart(2, "0")}`;
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
  session.log(
    `Crash log found: ${excName} at PC=${hex32(pc)}${pcSym}`,
    "err",
  );
}

async function readTelemetryAndCrash({ quiet = false } = {}) {
  if (session.isBusy() || !session.hasPorts()) return;
  session.setBusy(true);
  try {
    if (activeSymbols.length === 0) {
      await loadSymbolsTable();
    }
    const telem = await session.sendDiagRequest(DIAG_SUB_TELEMETRY);
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
    session.setDeviceModeLabel("Mode: Application (APC MINI + serialosc)");

    const crashRaw = await session.sendDiagRequest(DIAG_SUB_CRASH_GET);
    renderCrashLog(crashRaw);

    if (!quiet) {
      session.log(
        `Telemetry: ${flashKb} KB Flash, UID=${hex32(uid0)}, loop ~${hz} Hz (max ${(maxLoopTicks * 0.5).toFixed(1)} ms).`,
        "ok",
      );
    }
  } catch (err) {
    if (!quiet) {
      session.log(`Diagnostics query failed: ${err.message}`, "warn");
    }
  } finally {
    session.setBusy(false);
    attachPassiveMidiMonitor();
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
    session.log("Stopped 1 Hz live telemetry polling.");
  } else {
    ui.btnLivePoll.textContent = "Stop Live Poll (1 Hz)";
    readTelemetryAndCrash({ quiet: true });
    livePollTimer = setInterval(() => {
      if (!session.isBusy()) {
        readTelemetryAndCrash({ quiet: true });
      }
    }, 1000);
    session.log("Started 1 Hz live telemetry & MIDI input monitor.", "ok");
  }
});

ui.btnCopyCrash.addEventListener("click", async () => {
  const text = lastCrashMarkdown || ui.crashPre.textContent;
  if (!text) return;
  await navigator.clipboard.writeText(text);
  session.log("Copied crash report to clipboard.", "ok");
});

ui.btnClearCrash.addEventListener("click", async () => {
  if (session.isBusy()) return;
  session.setBusy(true);
  try {
    const res = await session.sendDiagRequest(DIAG_SUB_CRASH_CLR);
    session.log(`Cleared crash log (status=${res[0]}).`, "ok");
  } catch (err) {
    session.log(`Failed to clear crash log: ${err.message}`, "err");
  } finally {
    session.setBusy(false);
  }
  await readTelemetryAndCrash({ quiet: true });
});

ui.btnPeekMem.addEventListener("click", async () => {
  if (session.isBusy()) return;
  const rawAddr = parseInt(ui.peekAddrInput.value.trim(), 16);
  if (Number.isNaN(rawAddr)) {
    session.log(`Invalid hex address: '${ui.peekAddrInput.value}'`, "err");
    return;
  }
  session.setBusy(true);
  try {
    const lines = [];
    for (let off = 0; off < 64; off += 16) {
      const cur = (rawAddr + off) >>> 0;
      const chunk = await session.sendDiagRequest(
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
    session.log(`Peeked 64 B at ${hex32(rawAddr)}.`, "ok");
  } catch (err) {
    session.log(
      `Memory peek at ${hex32(rawAddr)} failed: ${err.message}`,
      "err",
    );
  } finally {
    session.setBusy(false);
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
    const { outPort } = session.getSelectedPorts();
    await outPort.open().catch(() => {});
    session.log(
      "Running 8x8 Pad + Round LED test sweep (Green -> Yellow -> Red)...",
    );
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
    session.log(
      "LED test sweep complete (click 'Clear All LEDs' to blank).",
      "ok",
    );
  } catch (err) {
    session.log(`LED sweep failed: ${err.message}`, "err");
  }
});

ui.btnLedClear.addEventListener("click", async () => {
  try {
    const { outPort } = session.getSelectedPorts();
    await outPort.open().catch(() => {});
    for (let note = 0; note < 90; note++) {
      outPort.send([0x80, note, 0]);
    }
    session.log("Cleared all 64 pad LEDs and 16 round LEDs.", "ok");
  } catch (err) {
    session.log(`Clear LEDs failed: ${err.message}`, "err");
  }
});

ui.btnTriggerFault.addEventListener("click", async () => {
  try {
    const { outPort } = session.getSelectedPorts();
    await outPort.open().catch(() => {});
    session.log(
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
    session.setDeviceModeLabel("Mode: Fault -> DFU Reboot");
  } catch (err) {
    session.log(`Failed to send test fault: ${err.message}`, "err");
  }
});

// Initialize on page load
loadSymbolsTable();
session.connectWebMidi().then((ok) => {
  if (ok && session.hasPorts()) {
    readTelemetryAndCrash({ quiet: true });
  }
});
