import {
  DIAG_SUB_TELEMETRY,
  LUA_SYSEX_CMD_BEGIN,
  LUA_SYSEX_CMD_CHUNK,
  LUA_SYSEX_CMD_COMMIT,
  LUA_SYSEX_CMD_ENABLE,
  LUA_SYSEX_CMD_RESET,
  LUA_SYSEX_CMD_SAVE,
  createMidiSession,
  sleep,
  triggerDownload,
} from "./midi-core.js";

const DRAFT_STORAGE_KEY = "apc_lua_custom_draft";
const PRESET_STORAGE_KEY = "apc_lua_selected_preset";

const FALLBACK_LUA_SCRIPT = `-- scripts/bouncing_balls.lua
function init()
    for x = 0, 7 do
        mem[x] = -1
        mem[x + 8] = 1
    end
    set_tempo(75)
end

function on_grid(x, y, z)
    if z == 1 then
        if shift() == 1 or mem[x] == y then
            mem[x] = -1
        else
            mem[x] = y
            if y < 4 then mem[x + 8] = 1 else mem[x + 8] = -1 end
        end
        return 1
    end
    return 0
end

function on_fader(idx, val, hires)
    if idx == 8 then
        set_tempo(30 + ((16383 - hires) >> 7))
    end
end

function on_tick(step)
    led_all(0)
    for x = 0, 7 do
        local y = mem[x]
        if y >= 0 then
            local dy = mem[x + 8]
            local ny = y + dy
            if ny < 0 or ny > 7 or host_led(x, ny) > 0 then
                dy = -dy
                mem[x + 8] = dy
                ny = y + dy
                if ny < 0 then ny = 0 end
                if ny > 7 then ny = 7 end
                key_pulse(x, y, 50)
                led(x, y, 15)
            else
                led(x, y, 4)
            end
            mem[x] = ny
            led(x, ny, 12)
        end
    end
end
`;

const ui = {
  luaWasmBadge: document.getElementById("luaWasmBadge"),
  luaPresetSelect: document.getElementById("luaPresetSelect"),
  luaFileInput: document.getElementById("luaFileInput"),
  luaEditorWrap: document.getElementById("luaEditorWrap"),
  luaHighlightPre: document.getElementById("luaHighlightPre"),
  luaHighlightCode: document.getElementById("luaHighlightCode"),
  luaEditor: document.getElementById("luaEditor"),
  guideStarterCode: document.getElementById("guideStarterCode"),
  luaErrorBox: document.getElementById("luaErrorBox"),
  luaMetaStatus: document.getElementById("luaMetaStatus"),
  luaMetaSize: document.getElementById("luaMetaSize"),
  luaMetaEntries: document.getElementById("luaMetaEntries"),
  luaMetaFrames: document.getElementById("luaMetaFrames"),
  chkLuaVmEnabled: document.getElementById("chkLuaVmEnabled"),
  btnLuaUploadRam: document.getElementById("btnLuaUploadRam"),
  btnLuaSaveFlash: document.getElementById("btnLuaSaveFlash"),
  btnLuaResetRom: document.getElementById("btnLuaResetRom"),
  btnLuaLoadFile: document.getElementById("btnLuaLoadFile"),
  btnLuaDownloadLuac: document.getElementById("btnLuaDownloadLuac"),
};

let luaWasmExports = null;
let activeLuaCompiled = null; // { image, codeLen, entryInit, entryTick, entryGrid, entryFader, entryButton, packetsRam, packetsFlash }
let scriptManifestUrls = {}; // id -> url
let syntaxHighlightReady = false;

function readLocal(key) {
  try {
    return localStorage.getItem(key) || "";
  } catch (_) {
    return "";
  }
}

function writeLocal(key, value) {
  try {
    localStorage.setItem(key, value);
  } catch (_) {
    // Ignore storage quota / privacy mode errors
  }
}

function updateScriptControls() {
  const canMidi = session.hasPorts() && !session.isBusy();
  const canUpload = canMidi && Boolean(activeLuaCompiled);
  ui.chkLuaVmEnabled.disabled = !canMidi;
  ui.btnLuaUploadRam.disabled = !canUpload;
  ui.btnLuaSaveFlash.disabled = !canUpload;
  ui.btnLuaResetRom.disabled = !canMidi;
  ui.btnLuaDownloadLuac.disabled = !activeLuaCompiled;
}

async function syncLuaVmState({ quiet = true } = {}) {
  if (!session.hasPorts() || session.isBusy()) return;
  session.setBusy(true);
  try {
    const telem = await session.sendDiagRequest(DIAG_SUB_TELEMETRY);
    if (telem.length >= 29) {
      ui.chkLuaVmEnabled.checked = (telem[28] & 0x04) !== 0;
    }
    session.setDeviceModeLabel("Mode: Application (APC MINI + serialosc)");
  } catch (err) {
    if (!quiet) {
      session.log(`Failed to read Lua VM status: ${err.message}`, "warn");
    }
  } finally {
    session.setBusy(false);
  }
}

const session = createMidiSession({
  onControlsUpdate: updateScriptControls,
  onConnected: () => syncLuaVmState({ quiet: false }),
  onProbeSuccess: () => syncLuaVmState({ quiet: true }),
});

// =========================================================================
// Bytecode Compilation & SysEx Packet Builder
// =========================================================================
export function buildLuaUploadSysexPackets(
  imageBytes,
  { saveToFlash = false } = {},
) {
  const packets = [];
  const totalLen = imageBytes.length;
  packets.push(
    new Uint8Array([
      0xf0,
      0x7d,
      0x4c,
      0x55,
      0x41,
      LUA_SYSEX_CMD_BEGIN,
      totalLen & 0x7f,
      (totalLen >> 7) & 0x7f,
      0xf7,
    ]),
  );

  const chunkSize = 16;
  for (let off = 0; off < totalLen; off += chunkSize) {
    const chunk = imageBytes.subarray(off, Math.min(off + chunkSize, totalLen));
    const nibbles = [];
    for (let i = 0; i < chunk.length; i++) {
      nibbles.push((chunk[i] >> 4) & 0x0f, chunk[i] & 0x0f);
    }
    packets.push(
      new Uint8Array([
        0xf0,
        0x7d,
        0x4c,
        0x55,
        0x41,
        LUA_SYSEX_CMD_CHUNK,
        off & 0x7f,
        (off >> 7) & 0x7f,
        ...nibbles,
        0xf7,
      ]),
    );
  }

  let xorCsum = 0;
  for (let i = 0; i < totalLen; i++) {
    xorCsum ^= imageBytes[i];
  }
  packets.push(
    new Uint8Array([
      0xf0,
      0x7d,
      0x4c,
      0x55,
      0x41,
      LUA_SYSEX_CMD_COMMIT,
      (xorCsum >> 4) & 0x0f,
      xorCsum & 0x0f,
      0xf7,
    ]),
  );

  if (saveToFlash) {
    packets.push(
      new Uint8Array([0xf0, 0x7d, 0x4c, 0x55, 0x41, LUA_SYSEX_CMD_SAVE, 0xf7]),
    );
  }
  return packets;
}

export function compileLuaWithWasm(sourceText, exports = luaWasmExports) {
  if (!exports) {
    throw new Error("Rust/WASM Lua compiler not loaded yet");
  }
  const encoded = new TextEncoder().encode(sourceText);
  const srcPtr = exports.wasm_alloc(encoded.length || 1);
  try {
    if (encoded.length > 0) {
      new Uint8Array(exports.memory.buffer, srcPtr, encoded.length).set(
        encoded,
      );
    }
    const rc = exports.wasm_compile_lua(srcPtr, encoded.length);
    if (rc !== 0) {
      const errPtr = exports.wasm_error_ptr();
      const errLen = exports.wasm_error_len();
      const errMsg = new TextDecoder().decode(
        new Uint8Array(exports.memory.buffer, errPtr, errLen),
      );
      throw new Error(errMsg);
    }
    const outPtr = exports.wasm_output_ptr();
    const outLen = exports.wasm_output_len();
    const image = new Uint8Array(
      exports.memory.buffer.slice(outPtr, outPtr + outLen),
    );
    const dv = new DataView(image.buffer, image.byteOffset, image.byteLength);
    return {
      image,
      codeLen: dv.getUint16(4, true),
      entryInit: dv.getUint16(6, true),
      entryTick: dv.getUint16(8, true),
      entryGrid: dv.getUint16(10, true),
      entryFader: dv.getUint16(12, true),
      entryButton: dv.getUint16(14, true),
    };
  } finally {
    exports.wasm_free(srcPtr, encoded.length || 1);
  }
}

function fmtEntryOffset(off) {
  return off === 0xffff
    ? "—"
    : `0x${off.toString(16).toUpperCase().padStart(2, "0")}`;
}

function syncEditorScroll() {
  if (!syntaxHighlightReady || !ui.luaHighlightPre) return;
  ui.luaHighlightPre.scrollTop = ui.luaEditor.scrollTop;
  ui.luaHighlightPre.scrollLeft = ui.luaEditor.scrollLeft;
}

function updateLuaHighlight() {
  if (!syntaxHighlightReady || !ui.luaHighlightCode) return;
  let src = ui.luaEditor.value || "";
  if (src.endsWith("\n")) {
    src += " ";
  }
  ui.luaHighlightCode.innerHTML = window.Prism.highlight(
    src,
    window.Prism.languages.lua,
    "lua",
  );
  syncEditorScroll();
}

function initSyntaxHighlight() {
  const prism = window.Prism;
  if (!prism?.languages?.lua || typeof prism.highlight !== "function") {
    return;
  }
  if (!prism.languages.lua.builtin && prism.languages.insertBefore) {
    prism.languages.insertBefore("lua", "function", {
      builtin: /\bmem(?=\s*\[)/,
    });
  }
  syntaxHighlightReady = true;
  if (ui.luaEditorWrap) {
    ui.luaEditorWrap.classList.add("has-highlight");
  }
  if (ui.guideStarterCode) {
    ui.guideStarterCode.innerHTML = prism.highlight(
      ui.guideStarterCode.textContent,
      prism.languages.lua,
      "lua",
    );
  }
  updateLuaHighlight();
}

function recompileLuaEditor() {
  updateLuaHighlight();
  if (!luaWasmExports) return;
  try {
    const res = compileLuaWithWasm(ui.luaEditor.value);
    const packetsRam = buildLuaUploadSysexPackets(res.image, {
      saveToFlash: false,
    });
    const packetsFlash = buildLuaUploadSysexPackets(res.image, {
      saveToFlash: true,
    });
    activeLuaCompiled = {
      ...res,
      packetsRam,
      packetsFlash,
    };
    const pct = Math.round((res.image.length * 100) / 4096);
    ui.luaErrorBox.classList.add("hidden");
    ui.luaErrorBox.textContent = "";
    ui.luaMetaStatus.textContent = "OK (full_moon Lua 5.4)";
    ui.luaMetaSize.textContent = `${res.image.length} / 4096 B (${pct}%)`;
    ui.luaMetaEntries.textContent = `${fmtEntryOffset(res.entryInit)} / ${fmtEntryOffset(res.entryTick)} / ${fmtEntryOffset(res.entryGrid)} / ${fmtEntryOffset(res.entryFader)} / ${fmtEntryOffset(res.entryButton)}`;
    ui.luaMetaFrames.textContent = `${packetsRam.length} frames`;
  } catch (err) {
    activeLuaCompiled = null;
    ui.luaErrorBox.classList.remove("hidden");
    ui.luaErrorBox.textContent = err.message;
    ui.luaMetaStatus.textContent = "Syntax / Size Error";
    ui.luaMetaSize.textContent = "—";
    ui.luaMetaEntries.textContent = "—";
    ui.luaMetaFrames.textContent = "—";
  }
  session.updateBaseControls();
}

async function loadScriptsManifest() {
  try {
    const res = await fetch("scripts/manifest.json", { cache: "no-cache" });
    if (!res.ok) return null;
    const manifest = await res.json();
    const scripts = manifest.scripts || [];
    if (scripts.length === 0) return null;

    scriptManifestUrls = {};
    ui.luaPresetSelect.innerHTML = "";
    for (const s of scripts) {
      scriptManifestUrls[s.id] = s.url;
      const opt = document.createElement("option");
      opt.value = s.id;
      opt.textContent = s.label || s.id;
      ui.luaPresetSelect.appendChild(opt);
    }
    const customOpt = document.createElement("option");
    customOpt.value = "__custom__";
    customOpt.textContent = "Custom / Edited Script...";
    ui.luaPresetSelect.appendChild(customOpt);
    return manifest.default_script || scripts[0].id;
  } catch (_) {
    return null;
  }
}

async function loadLuaPresetScript(scriptId) {
  if (scriptId === "__custom__") {
    const draft = readLocal(DRAFT_STORAGE_KEY);
    if (draft) {
      ui.luaEditor.value = draft;
      recompileLuaEditor();
    }
    return;
  }
  const candidates = [];
  if (scriptManifestUrls[scriptId]) {
    candidates.push(scriptManifestUrls[scriptId]);
  }
  candidates.push(`scripts/${scriptId}`, `../scripts/${scriptId}`);

  for (const url of candidates) {
    try {
      const res = await fetch(url, { cache: "no-cache" });
      if (res.ok) {
        ui.luaEditor.value = await res.text();
        recompileLuaEditor();
        return;
      }
    } catch (_) {
      // Try next candidate
    }
  }
  if (!ui.luaEditor.value) {
    ui.luaEditor.value = FALLBACK_LUA_SCRIPT;
    recompileLuaEditor();
  }
}

async function initLuaWasmStudio() {
  try {
    const [wasmRes, defaultManifestScript] = await Promise.all([
      fetch("apc_lua_compiler.wasm", { cache: "no-cache" }),
      loadScriptsManifest(),
    ]);
    if (!wasmRes.ok) throw new Error(`HTTP ${wasmRes.status}`);
    const wasmBytes = await wasmRes.arrayBuffer();
    const { instance } = await WebAssembly.instantiate(wasmBytes, {});
    luaWasmExports = instance.exports;
    ui.luaWasmBadge.textContent = `Rust + full_moon WASM (${Math.round(wasmBytes.byteLength / 1024)} KB)`;

    const savedPreset = readLocal(PRESET_STORAGE_KEY);
    const savedDraft = readLocal(DRAFT_STORAGE_KEY);

    if (savedPreset === "__custom__" && savedDraft) {
      ui.luaPresetSelect.value = "__custom__";
      ui.luaEditor.value = savedDraft;
      recompileLuaEditor();
    } else if (
      savedPreset &&
      Array.from(ui.luaPresetSelect.options).some(
        (o) => o.value === savedPreset,
      )
    ) {
      ui.luaPresetSelect.value = savedPreset;
      await loadLuaPresetScript(savedPreset);
    } else {
      const initialScript =
        defaultManifestScript || ui.luaPresetSelect.value || "bouncing_balls.lua";
      ui.luaPresetSelect.value = initialScript;
      await loadLuaPresetScript(initialScript);
    }
  } catch (err) {
    ui.luaWasmBadge.textContent = "WASM Load Error";
    session.log(
      `Failed to initialize Lua Rust/WASM compiler: ${err.message}`,
      "err",
    );
  }
}

ui.luaPresetSelect.addEventListener("change", () => {
  const val = ui.luaPresetSelect.value;
  writeLocal(PRESET_STORAGE_KEY, val);
  loadLuaPresetScript(val);
});

ui.luaEditor.addEventListener("input", () => {
  if (ui.luaPresetSelect.value !== "__custom__") {
    ui.luaPresetSelect.value = "__custom__";
    writeLocal(PRESET_STORAGE_KEY, "__custom__");
  }
  writeLocal(DRAFT_STORAGE_KEY, ui.luaEditor.value);
  recompileLuaEditor();
});

ui.luaEditor.addEventListener("keydown", (e) => {
  if (e.key === "Tab") {
    e.preventDefault();
    const start = ui.luaEditor.selectionStart;
    const end = ui.luaEditor.selectionEnd;
    const val = ui.luaEditor.value;
    ui.luaEditor.value = val.slice(0, start) + "    " + val.slice(end);
    ui.luaEditor.selectionStart = ui.luaEditor.selectionEnd = start + 4;
    if (ui.luaPresetSelect.value !== "__custom__") {
      ui.luaPresetSelect.value = "__custom__";
      writeLocal(PRESET_STORAGE_KEY, "__custom__");
    }
    writeLocal(DRAFT_STORAGE_KEY, ui.luaEditor.value);
    recompileLuaEditor();
  }
});

ui.luaEditor.addEventListener("scroll", syncEditorScroll);

ui.btnLuaLoadFile.addEventListener("click", () => ui.luaFileInput.click());
ui.luaFileInput.addEventListener("change", async () => {
  if (ui.luaFileInput.files.length === 0) return;
  const file = ui.luaFileInput.files[0];
  ui.luaEditor.value = await file.text();
  ui.luaPresetSelect.value = "__custom__";
  writeLocal(PRESET_STORAGE_KEY, "__custom__");
  writeLocal(DRAFT_STORAGE_KEY, ui.luaEditor.value);
  recompileLuaEditor();
  session.log(`Loaded Lua script '${file.name}' into editor.`, "ok");
});

ui.btnLuaDownloadLuac.addEventListener("click", () => {
  if (!activeLuaCompiled) return;
  triggerDownload(activeLuaCompiled.image, "apc_script.luac");
});

// =========================================================================
// Hardware SysEx Upload, Reset & Quick VM Toggle
// =========================================================================
async function uploadCompiledLuaToHardware({ saveToFlash = false } = {}) {
  if (!activeLuaCompiled || session.isBusy()) return;
  session.setBusy(true);
  try {
    const { outPort } = session.getSelectedPorts();
    await outPort.open().catch(() => {});
    const packets = saveToFlash
      ? activeLuaCompiled.packetsFlash
      : activeLuaCompiled.packetsRam;
    for (const pkt of packets) {
      outPort.send(pkt);
      await sleep(4);
    }
    ui.chkLuaVmEnabled.checked = true;
    const destLabel = saveToFlash
      ? "RAM + Flash (0x08008000)"
      : "RAM (live execution)";
    session.log(
      `Uploaded ${activeLuaCompiled.image.length} B LUA1 bytecode (${packets.length} SysEx frames) to ${destLabel}.`,
      "ok",
    );
  } catch (err) {
    session.log(`Lua SysEx upload failed: ${err.message}`, "err");
  } finally {
    session.setBusy(false);
  }
  await syncLuaVmState({ quiet: true });
}

ui.btnLuaUploadRam.addEventListener("click", () =>
  uploadCompiledLuaToHardware({ saveToFlash: false }),
);

ui.btnLuaSaveFlash.addEventListener("click", () =>
  uploadCompiledLuaToHardware({ saveToFlash: true }),
);

ui.btnLuaResetRom.addEventListener("click", async () => {
  if (session.isBusy()) return;
  session.setBusy(true);
  try {
    const { outPort } = session.getSelectedPorts();
    await outPort.open().catch(() => {});
    outPort.send(
      new Uint8Array([
        0xf0,
        0x7d,
        0x4c,
        0x55,
        0x41,
        LUA_SYSEX_CMD_RESET,
        0xf7,
      ]),
    );
    await sleep(20);
    session.log(
      "Reset Lua Co-Processor to built-in ROM script and cleared Flash 0x08008000.",
      "ok",
    );
  } catch (err) {
    session.log(`Lua ROM reset failed: ${err.message}`, "err");
  } finally {
    session.setBusy(false);
  }
  await syncLuaVmState({ quiet: true });
});

ui.chkLuaVmEnabled.addEventListener("change", async () => {
  if (session.isBusy()) return;
  const enabled = ui.chkLuaVmEnabled.checked;
  session.setBusy(true);
  try {
    const { outPort } = session.getSelectedPorts();
    await outPort.open().catch(() => {});
    outPort.send(
      new Uint8Array([
        0xf0,
        0x7d,
        0x4c,
        0x55,
        0x41,
        LUA_SYSEX_CMD_ENABLE,
        enabled ? 1 : 0,
        0xf7,
      ]),
    );
    await sleep(20);
    session.log(
      `Lua VM ${enabled ? "enabled" : "disabled"} via SysEx.`,
      "ok",
    );
  } catch (err) {
    session.log(`Failed to toggle Lua VM: ${err.message}`, "err");
  } finally {
    session.setBusy(false);
  }
  await syncLuaVmState({ quiet: true });
});

// Initialize on page load
initSyntaxHighlight();
initLuaWasmStudio();
session.connectWebMidi().then((ok) => {
  if (ok && session.hasPorts()) {
    syncLuaVmState({ quiet: true });
  }
});
