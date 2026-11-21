import {
  ACK_APP_UNLOCK,
  ACK_BL_UNLOCK,
  INQUIRY_SYSEX,
  UNLOCK_SYSEX,
  createMidiSession,
  sleep,
  triggerDownload,
} from "./midi-core.js";
import {
  APP_TOTAL_SIZE,
  decodeSyxToBundle,
  encodeFirmwareSteps,
  stepsToSyxBytes,
} from "./fw-codec.js";

const ui = {
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
};

let activeBundle = null; // { versionLabel, steps, binBytes, syxBytes, checksum }
let manifestData = null;

function updateUploaderControls() {
  const portsReady = session.hasPorts();
  const busy = session.isBusy();
  ui.btnFlash.disabled = !portsReady || !activeBundle || busy;
  ui.btnDownloadSyx.disabled = !activeBundle;
  ui.btnDownloadBin.disabled = !activeBundle;
  ui.versionSelect.disabled = busy;
}

async function quietProbeDeviceMode() {
  if (!session.hasPorts() || session.isBusy()) return;
  try {
    const { outPort, inPort } = session.getSelectedPorts();
    await outPort.open().catch(() => {});
    await inPort.open().catch(() => {});
    const rx = await session.sendSysexAndWaitReply(
      outPort,
      inPort,
      INQUIRY_SYSEX,
      700,
    );
    if (rx && rx[1] === 0x7e && rx[3] === 0x06 && rx[4] === 0x02) {
      session.setDeviceModeLabel("Mode: Application (APC MINI + serialosc)");
    }
  } catch (_) {
    // Device may be in Bootloader mode or disconnected; stay quiet on init
  }
}

const session = createMidiSession({
  onControlsUpdate: updateUploaderControls,
  onConnected: quietProbeDeviceMode,
});

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
  session.updateBaseControls();
}

function clearLoadedBundle() {
  activeBundle = null;
  ui.metaVersion.textContent = "—";
  ui.metaSize.textContent = "—";
  ui.metaChecksum.textContent = "—";
  ui.metaSteps.textContent = "—";
  session.updateBaseControls();
}

// =========================================================================
// Manifest & Firmware Loading
// =========================================================================
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
    session.log(`Fetching firmware ${entry.id} (${entry.syx_url})...`);
    const res = await fetch(entry.syx_url, { cache: "no-cache" });
    if (!res.ok) {
      throw new Error(`HTTP ${res.status} fetching ${entry.syx_url}`);
    }
    const bytes = new Uint8Array(await res.arrayBuffer());
    const bundle = decodeSyxToBundle(bytes);
    setLoadedBundle(entry.id, bundle);
    session.log(
      `Loaded ${entry.label}: ${bundle.binBytes.length} B, checksum 0x${bundle.checksum.toString(16).toUpperCase().padStart(4, "0")}.`,
      "ok",
    );
  } catch (err) {
    session.log(`Failed to load firmware ${entry.id}: ${err.message}`, "err");
  }
}

async function loadManifest() {
  try {
    const res = await fetch("firmware/manifest.json", { cache: "no-cache" });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    manifestData = await res.json();
    populateVersionDropdown(manifestData);
    ui.fwSourceBadge.textContent = `${manifestData.versions.length} version(s) available`;
    if (manifestData.default_version) {
      ui.versionSelect.value = manifestData.default_version;
      await onVersionSelectionChanged();
    }
  } catch (_) {
    ui.fwSourceBadge.textContent = "Local / Direct Mode";
    session.log(
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
        session.log(
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

ui.versionSelect.addEventListener("change", onVersionSelectionChanged);

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
    ui.dropzonePrompt.innerHTML = `Loaded <b>${file.name}</b> (${bytes.length.toLocaleString()} B)`;
    setLoadedBundle(file.name, bundle);
    session.log(
      `Validated local file '${file.name}': checksum 0x${bundle.checksum.toString(16).toUpperCase().padStart(4, "0")}.`,
      "ok",
    );
  } catch (err) {
    clearLoadedBundle();
    session.log(`Invalid firmware file '${file.name}': ${err.message}`, "err");
  }
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
// Firmware Flashing Loop
// =========================================================================
async function enterAndUnlockBootloader() {
  if (!session.isConnected()) {
    await session.connectWebMidi();
  }
  let { outPort, inPort } = session.getSelectedPorts();
  await outPort.open().catch(() => {});
  await inPort.open().catch(() => {});
  session.log(`Sending bootloader unlock SysEx to '${outPort.name}'...`);
  let code = await session.sendAndWaitAck(outPort, inPort, UNLOCK_SYSEX, 3000);

  if (code === ACK_BL_UNLOCK) {
    session.setDeviceModeLabel("Mode: Bootloader Unlocked (0x3E)");
    session.log("Bootloader already active and unlocked (ACK 0x3E).", "ok");
    await sleep(100);
    return { outPort, inPort };
  }

  if (code === ACK_APP_UNLOCK) {
    session.setDeviceModeLabel("Mode: Rebooting to DFU...");
    session.log(
      "Application accepted unlock (ACK 0x15). Waiting for USB re-enumeration into bootloader...",
      "warn",
    );
    inPort.onmidimessage = null;
    await outPort.close().catch(() => {});
    await inPort.close().catch(() => {});
    await sleep(1800);

    session.log("Re-connecting WebMIDI to bootloader port...");
    const deadline = performance.now() + 15000;
    while (performance.now() < deadline) {
      await session.connectWebMidi({
        silent: true,
        preferUpdater: true,
      });
      try {
        ({ outPort, inPort } = session.getSelectedPorts());
        await outPort.open().catch(() => {});
        await inPort.open().catch(() => {});
        code = await session.sendAndWaitAck(outPort, inPort, UNLOCK_SYSEX, 1500);
        if (code === ACK_BL_UNLOCK) {
          session.setDeviceModeLabel("Mode: Bootloader Unlocked (0x3E)");
          session.log(
            `Bootloader unlocked on '${outPort.name}' (ACK 0x3E).`,
            "ok",
          );
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

async function sendStepWithRetry(outPort, inPort, step) {
  const timeoutMs =
    step.kind === "flash_write" || step.kind === "finalize" ? 5000 : 1500;
  const maxAttempts =
    step.kind === "ram_load" || step.kind === "flash_write" ? 5 : 1;

  for (let attempt = 0; attempt < maxAttempts; attempt++) {
    try {
      const ack = await session.sendAndWaitAck(
        outPort,
        inPort,
        step.sysex,
        timeoutMs,
        {
          ignoreUnlockAck: true,
        },
      );
      if (
        ack === step.expectedAck ||
        step.kind !== "ram_load" ||
        attempt + 1 >= maxAttempts
      ) {
        return ack;
      }
    } catch (err) {
      if (attempt + 1 >= maxAttempts) {
        throw err;
      }
    }

    session.log(
      `MIDI collision / timeout on page ${(step.pageIdx ?? 0) + 1} (attempt ${attempt + 1}/${maxAttempts}), re-syncing bootloader (0x3E)...`,
      "warn",
    );
    await sleep(400);
    await session
      .sendAndWaitAck(outPort, inPort, UNLOCK_SYSEX, 1500)
      .catch(() => {});
    await sleep(100);
  }
}

async function flashCurrentBundle() {
  if (!activeBundle || session.isBusy()) return;
  session.setBusy(true);

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
    session.log(
      `Starting flash of ${activeBundle.versionLabel} (${steps.length} SysEx steps across ${numPages} pages)...`,
    );

    for (let i = 0; i < steps.length; i++) {
      const step = steps[i];
      if (step.kind === "finalize") {
        ui.flashStateLabel.textContent = "Finalizing...";
        session.log(
          "Verifying application checksum & rebooting into firmware (0x0E)...",
        );
      }

      const ack = await sendStepWithRetry(outPort, inPort, step);

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
        session.log(
          `Flashed page ${pageNum}/${numPages} (0x0800${step.flashOffset.toString(16).toUpperCase().padStart(4, "0")}) [ACK 0x1A]`,
        );
      }
    }

    const elapsedS = ((performance.now() - t0) / 1000).toFixed(1);
    ui.progressBar.style.width = "100%";
    ui.progressPctText.textContent = "100%";
    ui.flashStateLabel.textContent = "Complete (0x77)";
    session.setDeviceModeLabel("Mode: Application Rebooted");
    session.log(
      `Success! Bootloader verified checksum (ACK 0x77) and launched application in ${elapsedS}s.`,
      "ok",
    );

    inPort.onmidimessage = null;
    await outPort.close().catch(() => {});
    await inPort.close().catch(() => {});
    await sleep(1400);

    for (let attempt = 0; attempt < 10; attempt++) {
      await session.connectWebMidi({ silent: true });
      if (session.hasPorts()) {
        try {
          const ports = session.getSelectedPorts();
          await ports.outPort.open().catch(() => {});
          await ports.inPort.open().catch(() => {});
          const rx = await session.sendSysexAndWaitReply(
            ports.outPort,
            ports.inPort,
            INQUIRY_SYSEX,
            700,
          );
          if (rx && rx[1] === 0x7e) {
            session.setDeviceModeLabel(
              "Mode: Application (APC MINI + serialosc)",
            );
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
    session.log(`Flash aborted: ${err.message}`, "err");
  } finally {
    session.setBusy(false);
  }
}

ui.btnFlash.addEventListener("click", flashCurrentBundle);

// Initialize on page load
loadManifest();
session.connectWebMidi().then((ok) => {
  if (ok && session.hasPorts()) {
    quietProbeDeviceMode();
  }
});
