export const UNLOCK_SYSEX = new Uint8Array([
  0xf0, 0x5a, 0x57, 0x6e, 0x28, 0x3c, 0x4e, 0x3e, 0xf7,
]);
export const INQUIRY_SYSEX = new Uint8Array([0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7]);

export const ACK_APP_UNLOCK = 0x15;
export const ACK_BL_UNLOCK = 0x3e;
export const ACK_OK = 0x1a;
export const ACK_DONE = 0x77;

export const DIAG_SUB_TELEMETRY = 0x01;
export const DIAG_SUB_CRASH_GET = 0x02;
export const DIAG_SUB_CRASH_CLR = 0x03;
export const DIAG_SUB_PEEK_MEM = 0x04;
export const DIAG_SUB_TEST_FAULT = 0x05;
export const DIAG_SUB_SET_MODES = 0x06;

export const LUA_SYSEX_CMD_BEGIN = 0x01;
export const LUA_SYSEX_CMD_CHUNK = 0x02;
export const LUA_SYSEX_CMD_COMMIT = 0x03;
export const LUA_SYSEX_CMD_SAVE = 0x04;
export const LUA_SYSEX_CMD_RESET = 0x05;
export const LUA_SYSEX_CMD_ENABLE = 0x06;

const STORAGE_OUT_KEY = "apc_midi_out_id";
const STORAGE_IN_KEY = "apc_midi_in_id";

export function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

export function hex32(val) {
  return "0x" + (val >>> 0).toString(16).toUpperCase().padStart(8, "0");
}

export function isApcPort(port) {
  const n = (port?.name || "").toLowerCase();
  return n.includes("apc") || n.includes("monome") || n.includes("updater");
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

export function triggerDownload(bytes, filename) {
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

function readSession(key) {
  try {
    return sessionStorage.getItem(key) || "";
  } catch (_) {
    return "";
  }
}

function writeSession(key, value) {
  try {
    if (value) {
      sessionStorage.setItem(key, value);
    } else {
      sessionStorage.removeItem(key);
    }
  } catch (_) {
    // Ignore storage restriction errors
  }
}

export function createMidiSession({
  onControlsUpdate,
  onConnected,
  onProbeSuccess,
  onAfterDiag,
} = {}) {
  const ui = {
    midiStatusDot: document.getElementById("midiStatusDot"),
    midiStatusText: document.getElementById("midiStatusText"),
    deviceModeLabel: document.getElementById("deviceModeLabel"),
    midiOutSelect: document.getElementById("midiOutSelect"),
    midiInSelect: document.getElementById("midiInSelect"),
    btnConnectMidi: document.getElementById("btnConnectMidi"),
    btnProbeDevice: document.getElementById("btnProbeDevice"),
    logBox: document.getElementById("logBox"),
    btnClearLog: document.getElementById("btnClearLog"),
  };

  let midiAccess = null;
  let busy = false;

  function log(msg, level = "info") {
    if (!ui.logBox) return;
    const line = document.createElement("div");
    line.className = `log-line ${level}`;
    const ts = new Date().toLocaleTimeString();
    line.textContent = `[${ts}] ${msg}`;
    ui.logBox.appendChild(line);
    ui.logBox.scrollTop = ui.logBox.scrollHeight;
  }

  function hasPorts() {
    return Boolean(ui.midiOutSelect?.value && ui.midiInSelect?.value);
  }

  function updateBaseControls() {
    const portsReady = hasPorts();
    if (ui.btnProbeDevice) ui.btnProbeDevice.disabled = !portsReady || busy;
    if (ui.midiOutSelect) ui.midiOutSelect.disabled = !midiAccess || busy;
    if (ui.midiInSelect) ui.midiInSelect.disabled = !midiAccess || busy;
    if (onControlsUpdate) onControlsUpdate();
  }

  function setBusy(flag) {
    busy = Boolean(flag);
    updateBaseControls();
  }

  function setDeviceModeLabel(text) {
    if (ui.deviceModeLabel) {
      ui.deviceModeLabel.textContent = text;
    }
  }

  function clearAllInputListeners(accessObj = midiAccess) {
    if (!accessObj) return;
    for (const input of accessObj.inputs.values()) {
      input.onmidimessage = null;
    }
  }

  function refreshMidiPorts({ preferUpdater = false } = {}) {
    if (!midiAccess || !ui.midiOutSelect || !ui.midiInSelect) return;
    const prevOut = preferUpdater
      ? ""
      : ui.midiOutSelect.value || readSession(STORAGE_OUT_KEY);
    const prevIn = preferUpdater
      ? ""
      : ui.midiInSelect.value || readSession(STORAGE_IN_KEY);

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

    if (!preferUpdater) {
      if (ui.midiOutSelect.value) {
        writeSession(STORAGE_OUT_KEY, ui.midiOutSelect.value);
      }
      if (ui.midiInSelect.value) {
        writeSession(STORAGE_IN_KEY, ui.midiInSelect.value);
      }
    }

    if (ui.midiStatusDot && ui.midiStatusText) {
      if (apcOut && apcIn) {
        ui.midiStatusDot.className = "dot ok";
        const selectedOut = outputs.find((p) => p.id === ui.midiOutSelect.value);
        ui.midiStatusText.textContent = (selectedOut || apcOut).name;
      } else {
        ui.midiStatusDot.className = "dot err";
        ui.midiStatusText.textContent = "APC Mini Not Connected";
      }
    }

    updateBaseControls();
  }

  async function connectWebMidi({ silent = false, preferUpdater = false } = {}) {
    if (!window.isSecureContext) {
      if (ui.midiStatusDot) ui.midiStatusDot.className = "dot err";
      if (ui.midiStatusText) ui.midiStatusText.textContent = "Insecure Context";
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
      if (ui.midiStatusDot) ui.midiStatusDot.className = "dot err";
      if (ui.midiStatusText) ui.midiStatusText.textContent = "WebMIDI Unsupported";
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
        if (!busy) {
          refreshMidiPorts({ preferUpdater });
        }
      };
      refreshMidiPorts({ preferUpdater });
      if (!silent) {
        log("WebMIDI connected (SysEx enabled).", "ok");
      }
      return true;
    } catch (err) {
      if (ui.midiStatusDot) ui.midiStatusDot.className = "dot err";
      if (ui.midiStatusText) ui.midiStatusText.textContent = "Permission Denied";
      if (!silent) {
        log(`Failed to acquire WebMIDI SysEx access: ${err.message}`, "err");
      }
      return false;
    }
  }

  function getSelectedPorts() {
    if (!midiAccess) throw new Error("WebMIDI not connected");
    const outPort = midiAccess.outputs.get(ui.midiOutSelect?.value || "");
    const inPort = midiAccess.inputs.get(ui.midiInSelect?.value || "");
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
    if (onAfterDiag) onAfterDiag();
    const decoded = decodeDiagNibbles(rx);
    if (decoded.subcmd !== subcmd) {
      throw new Error(
        `Mismatched diagnostic reply subcmd 0x${decoded.subcmd.toString(16)}`,
      );
    }
    return decoded.payload;
  }

  if (ui.btnClearLog && ui.logBox) {
    ui.btnClearLog.addEventListener("click", () => {
      ui.logBox.innerHTML = "";
    });
  }

  if (ui.midiOutSelect) {
    ui.midiOutSelect.addEventListener("change", () => {
      writeSession(STORAGE_OUT_KEY, ui.midiOutSelect.value);
      const selected = midiAccess?.outputs.get(ui.midiOutSelect.value);
      if (selected && ui.midiStatusText && hasPorts()) {
        ui.midiStatusText.textContent = selected.name;
      }
      updateBaseControls();
    });
  }

  if (ui.midiInSelect) {
    ui.midiInSelect.addEventListener("change", () => {
      writeSession(STORAGE_IN_KEY, ui.midiInSelect.value);
      updateBaseControls();
    });
  }

  if (ui.btnConnectMidi) {
    ui.btnConnectMidi.addEventListener("click", async () => {
      const ok = await connectWebMidi();
      if (ok && hasPorts() && onConnected) {
        await onConnected({ fromUserClick: true });
      }
    });
  }

  if (ui.btnProbeDevice) {
    ui.btnProbeDevice.addEventListener("click", async () => {
      try {
        const { outPort, inPort } = getSelectedPorts();
        await outPort.open().catch(() => {});
        await inPort.open().catch(() => {});
        log(`Sending Universal Device Inquiry to '${outPort.name}'...`);
        const rx = await sendSysexAndWaitReply(
          outPort,
          inPort,
          INQUIRY_SYSEX,
          1200,
        );
        if (onAfterDiag) onAfterDiag();
        if (rx[1] === 0x7e && rx[3] === 0x06 && rx[4] === 0x02) {
          setDeviceModeLabel("Mode: Application (APC MINI + serialosc)");
          log(
            `APC Mini responded in Application mode on '${outPort.name}' (Simultaneous USB-MIDI + serialosc).`,
            "ok",
          );
          if (onProbeSuccess) await onProbeSuccess(rx);
        } else {
          log(
            `Received SysEx reply: ${Array.from(rx)
              .map((b) => b.toString(16).padStart(2, "0"))
              .join(" ")}`,
            "info",
          );
        }
      } catch (_) {
        setDeviceModeLabel("Mode: Bootloader or No Reply");
        log(
          "No Application identity reply (device may already be in Bootloader mode). Ready to unlock & flash.",
          "warn",
        );
      }
    });
  }

  return {
    log,
    connectWebMidi,
    refreshMidiPorts,
    hasPorts,
    isConnected: () => Boolean(midiAccess),
    isBusy: () => busy,
    setBusy,
    getMidiAccess: () => midiAccess,
    getSelectedPorts,
    setDeviceModeLabel,
    clearAllInputListeners,
    sendSysexAndWaitReply,
    sendAndWaitAck,
    sendDiagRequest,
    updateBaseControls,
  };
}
