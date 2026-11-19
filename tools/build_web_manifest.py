"""Assemble the static WebMIDI updater site and firmware/manifest.json bundle."""

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from tools.bin_to_sysex import (
    APP_CHECKSUM_OFF,
    decode_syx_to_bin,
    encode_syx_stream,
    validate_stamped_bin,
)


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def extract_checksum_hex(bin_bytes: bytes) -> str:
    validate_stamped_bin(bin_bytes)
    chk = struct.unpack_from("<H", bin_bytes, APP_CHECKSUM_OFF)[0]
    return f"0x{chk:04X}"


def extract_elf_symbols(elf_path: Path | None) -> list[dict[str, Any]]:
    """Extracts Flash function symbols from an ARM ELF binary via arm-none-eabi-nm."""
    if elf_path is None or not elf_path.is_file():
        return []
    nm_bin = shutil.which("arm-none-eabi-nm")
    if not nm_bin:
        return []
    try:
        res = subprocess.run(
            [nm_bin, "-n", "-S", "--defined-only", str(elf_path)],
            check=True,
            capture_output=True,
            text=True,
        )
    except Exception:
        return []

    symbols: list[dict[str, Any]] = []
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) == 4:
            addr_s, size_s, sym_type, name = parts
            size = int(size_s, 16)
        elif len(parts) == 3:
            addr_s, sym_type, name = parts
            size = 0
        else:
            continue
        if sym_type.lower() not in ("t", "w"):
            continue
        addr = int(addr_s, 16) & ~1
        if 0x08002000 <= addr < 0x08007800:
            symbols.append({"addr": addr, "size": size, "name": name})
    return symbols


def stage_firmware_pair(
    version_id: str,
    label: str,
    channel: str,
    published_at: str,
    out_dir: Path,
    *,
    bin_bytes: bytes | None = None,
    syx_bytes: bytes | None = None,
    symbols: list[dict[str, Any]] | None = None,
    notes: str = "",
) -> dict[str, Any]:
    """Validates and writes both .bin and .syx into out_dir/firmware/<version_id>/."""
    if bin_bytes is None and syx_bytes is None:
        raise ValueError(f"Neither .bin nor .syx provided for version '{version_id}'")

    if bin_bytes is not None:
        validate_stamped_bin(bin_bytes)
        if syx_bytes is None:
            syx_bytes = encode_syx_stream(bin_bytes)
        else:
            decoded = decode_syx_to_bin(syx_bytes)
            if decoded != bin_bytes:
                raise ValueError(f".bin and .syx mismatch for version '{version_id}'")
    else:
        assert syx_bytes is not None
        bin_bytes = decode_syx_to_bin(syx_bytes)

    safe_id = version_id.replace("/", "_")
    fw_rel_dir = Path("firmware") / safe_id
    fw_abs_dir = out_dir / fw_rel_dir
    fw_abs_dir.mkdir(parents=True, exist_ok=True)

    bin_rel = fw_rel_dir / "apc_mini_app.bin"
    syx_rel = fw_rel_dir / "apc_mini_app.syx"
    (out_dir / bin_rel).write_bytes(bin_bytes)
    (out_dir / syx_rel).write_bytes(syx_bytes)

    symbols_url: str | None = None
    if symbols:
        sym_rel = fw_rel_dir / "symbols.json"
        (out_dir / sym_rel).write_text(json.dumps(symbols, indent=2) + "\n", encoding="utf-8")
        symbols_url = sym_rel.as_posix()

    return {
        "id": version_id,
        "label": label,
        "channel": channel,
        "published_at": published_at,
        "syx_url": syx_rel.as_posix(),
        "bin_url": bin_rel.as_posix(),
        "symbols_url": symbols_url,
        "bin_size": len(bin_bytes),
        "syx_size": len(syx_bytes),
        "checksum": extract_checksum_hex(bin_bytes),
        "sha256_bin": sha256_hex(bin_bytes),
        "sha256_syx": sha256_hex(syx_bytes),
        "notes": notes,
    }


def git_short_sha() -> str:
    try:
        res = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        )
        return res.stdout.strip() or "dev"
    except Exception:
        return "dev"


def git_exact_tag() -> str | None:
    try:
        res = subprocess.run(
            ["git", "describe", "--tags", "--exact-match"],
            check=True,
            capture_output=True,
            text=True,
        )
        return res.stdout.strip() or None
    except Exception:
        return None


def fetch_github_releases(out_dir: Path, repo: str | None = None) -> list[dict[str, Any]]:
    """Downloads .bin/.syx assets from published GitHub Releases via `gh` CLI."""
    cmd = [
        "gh",
        "release",
        "list",
        "--limit",
        "50",
        "--json",
        "tagName,name,publishedAt,isDraft,isPrerelease",
    ]
    if repo:
        cmd.extend(["--repo", repo])

    try:
        res = subprocess.run(cmd, check=True, capture_output=True, text=True)
        releases = json.loads(res.stdout)
    except Exception as exc:
        print(f"[WEB] Skipping GitHub release fetch ({exc})")
        return []

    staged: list[dict[str, Any]] = []
    for rel in releases:
        if rel.get("isDraft"):
            continue
        tag = rel["tagName"]
        name = rel.get("name") or tag
        published_at = rel.get("publishedAt") or datetime.now(timezone.utc).isoformat()
        is_prerelease = bool(rel.get("isPrerelease"))

        with tempfile.TemporaryDirectory() as tmpdir:
            dl_cmd = [
                "gh",
                "release",
                "download",
                tag,
                "--dir",
                tmpdir,
                "--pattern",
                "*.bin",
                "--pattern",
                "*.syx",
            ]
            if repo:
                dl_cmd.extend(["--repo", repo])
            dl_res = subprocess.run(dl_cmd, capture_output=True, text=True)
            if dl_res.returncode != 0:
                print(f"[WEB] No .bin/.syx assets found on release {tag}, skipping.")
                continue

            tmp_path = Path(tmpdir)
            bin_files = sorted(tmp_path.glob("*.bin"))
            syx_files = sorted(tmp_path.glob("*.syx"))
            bin_bytes = bin_files[0].read_bytes() if bin_files else None
            syx_bytes = syx_files[0].read_bytes() if syx_files else None

            try:
                channel = "prerelease" if is_prerelease else "release"
                label = f"{tag}" if name == tag else f"{tag} — {name}"
                entry = stage_firmware_pair(
                    version_id=tag,
                    label=label,
                    channel=channel,
                    published_at=published_at,
                    out_dir=out_dir,
                    bin_bytes=bin_bytes,
                    syx_bytes=syx_bytes,
                    notes=f"GitHub Release {tag}",
                )
                staged.append(entry)
                print(f"[WEB] Staged release {tag} (chk={entry['checksum']})")
            except Exception as exc:
                print(f"[WEB] Warning: skipping invalid release asset on {tag}: {exc}")

    return staged


def stage_lua_scripts(out_dir: Path, scripts_src: Path = Path("scripts")) -> None:
    """Copies scripts/*.lua into out_dir/scripts/ and writes scripts/manifest.json."""
    if not scripts_src.is_dir():
        return
    dest_dir = out_dir / "scripts"
    dest_dir.mkdir(parents=True, exist_ok=True)

    descriptions = {
        "bouncing_balls.lua": "bouncing_balls.lua — Polyrhythmic Bouncing Ball Sequencer (14-bit Fader Tempo)",
        "game_of_life.lua": "game_of_life.lua — Conway's Game of Life Generative Sequencer",
        "bouncy_echo.lua": "bouncy_echo.lua — Built-in ROM Varibright Diamond Ripple & Echo",
    }

    entries: list[dict[str, str]] = []
    for lua_file in sorted(scripts_src.glob("*.lua")):
        shutil.copy2(lua_file, dest_dir / lua_file.name)
        entries.append(
            {
                "id": lua_file.name,
                "label": descriptions.get(lua_file.name, lua_file.name),
                "url": f"scripts/{lua_file.name}",
            }
        )

    manifest = {
        "default_script": "bouncing_balls.lua"
        if any(e["id"] == "bouncing_balls.lua" for e in entries)
        else (entries[0]["id"] if entries else ""),
        "scripts": entries,
    }
    (dest_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(f"[WEB] Staged {len(entries)} Lua preset script(s) in {dest_dir}")


def build_web_bundle(
    web_src: Path,
    out_dir: Path,
    local_bin: Path | None,
    local_syx: Path | None,
    *,
    local_elf: Path | None = None,
    fetch_releases: bool = False,
    repo: str | None = None,
) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    for item in web_src.iterdir():
        dest = out_dir / item.name
        if item.is_dir():
            shutil.copytree(item, dest, dirs_exist_ok=True)
        else:
            shutil.copy2(item, dest)

    stage_lua_scripts(out_dir)

    versions: list[dict[str, Any]] = []
    if fetch_releases:
        versions.extend(fetch_github_releases(out_dir, repo=repo))

    existing_ids = {v["id"] for v in versions}
    now_iso = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    bin_bytes = local_bin.read_bytes() if local_bin and local_bin.is_file() else None
    syx_bytes = local_syx.read_bytes() if local_syx and local_syx.is_file() else None
    local_symbols = extract_elf_symbols(local_elf)

    if local_symbols:
        top_sym = out_dir / "firmware" / "symbols.json"
        top_sym.parent.mkdir(parents=True, exist_ok=True)
        top_sym.write_text(json.dumps(local_symbols, indent=2) + "\n", encoding="utf-8")

    if bin_bytes is not None or syx_bytes is not None:
        exact_tag = git_exact_tag()
        short_sha = git_short_sha()

        if exact_tag and exact_tag not in existing_ids:
            entry = stage_firmware_pair(
                version_id=exact_tag,
                label=f"{exact_tag}",
                channel="release",
                published_at=now_iso,
                out_dir=out_dir,
                bin_bytes=bin_bytes,
                syx_bytes=syx_bytes,
                symbols=local_symbols,
                notes=f"Release {exact_tag} ({short_sha})",
            )
            versions.insert(0, entry)
            existing_ids.add(exact_tag)

        # Always stage the current development/main build
        dev_entry = stage_firmware_pair(
            version_id="main",
            label=f"main ({short_sha}) — Development Build",
            channel="dev",
            published_at=now_iso,
            out_dir=out_dir,
            bin_bytes=bin_bytes,
            syx_bytes=syx_bytes,
            symbols=local_symbols,
            notes=f"Built from commit {short_sha}",
        )
        versions.append(dev_entry)

    # Mark the latest release in its label for clarity
    default_version = None
    for v in versions:
        if v["channel"] == "release":
            default_version = v["id"]
            if not v["label"].endswith("(Latest)"):
                v["label"] = f"{v['label']} (Latest)"
            break

    if default_version is None and versions:
        default_version = versions[0]["id"]

    manifest = {
        "generated_at": now_iso,
        "default_version": default_version,
        "versions": versions,
    }

    manifest_path = out_dir / "firmware" / "manifest.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(
        f"[WEB] Wrote {manifest_path} ({len(versions)} version(s), default={default_version})"
    )
    return manifest_path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--web-src",
        type=Path,
        default=Path("web"),
        help="Source directory containing index.html (default: web)",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("build/web"),
        help="Output directory for assembled static site (default: build/web)",
    )
    parser.add_argument(
        "--bin",
        type=Path,
        default=Path("build/apc_mini_app.bin"),
        help="Path to local stamped .bin build",
    )
    parser.add_argument(
        "--syx",
        type=Path,
        default=Path("build/apc_mini_app.syx"),
        help="Path to local .syx build",
    )
    parser.add_argument(
        "--elf",
        type=Path,
        default=Path("build/apc_mini_app.elf"),
        help="Path to local .elf build for symbol extraction",
    )
    parser.add_argument(
        "--fetch-releases",
        action="store_true",
        help="Download and bundle all published GitHub Releases via `gh` CLI",
    )
    parser.add_argument(
        "--repo",
        type=str,
        default=None,
        help="Optional owner/repo override for `gh release` commands",
    )
    args = parser.parse_args()

    build_web_bundle(
        web_src=args.web_src,
        out_dir=args.out_dir,
        local_bin=args.bin,
        local_syx=args.syx,
        local_elf=args.elf,
        fetch_releases=args.fetch_releases,
        repo=args.repo,
    )


if __name__ == "__main__":
    main()
