#!/usr/bin/env python3
"""Collect and validate the reproducible M6 integration evidence.

This tool deliberately does not implement DOS behavior.  It checks an already
built ISO and the logs produced by the existing BIOS/UEFI terminal acceptance,
then copies the evidence into a self-contained archive with SHA-256 hashes.
Optional MS-DOS 4.0 EDLIN.COM and DEBUG.COM binaries are recorded as external
inputs; they are never silently treated as FunnyOS source or as full
compatibility proof.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD = Path("/var/tmp/funyos-m6/funyos-build")
DEFAULT_ARCHIVE = Path("/var/tmp/funyos-m6/m6-acceptance")

REQUIRED_LOGS = {
    "bios_terminal": ("terminal-test/bios/serial.log", (
        "M6 MZ loader PASS", "M6 EXEC child PASS", "M6 EXEC parent PASS",
        "DOS program returned 0",
    )),
    "uefi_terminal": ("terminal-test/uefi/serial.log", (
        "M6 MZ loader PASS", "M6 EXEC child PASS", "M6 EXEC parent PASS",
        "DOS program returned 0",
    )),
}
FORBIDDEN = ("PANIC", "CPU EXCEPTION", "CLOBBERED", "RUN LIMIT", "stream overflow")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def copy_evidence(src: Path, destination: Path, files: dict[str, dict], root: Path) -> None:
    if not src.is_file():
        return
    destination.mkdir(parents=True, exist_ok=True)
    target = destination / src.name
    shutil.copy2(src, target)
    files[str(target.relative_to(root))] = {
        "source": str(src), "size": target.stat().st_size, "sha256": sha256(target)
    }


def require_markers(path: Path, markers: tuple[str, ...]) -> dict:
    if not path.is_file():
        raise RuntimeError(f"missing required log: {path}")
    text = path.read_text(errors="replace")
    missing = [marker for marker in markers if marker not in text]
    bad = [marker for marker in FORBIDDEN if marker in text]
    if missing:
        raise RuntimeError(f"{path}: missing markers: {', '.join(missing)}")
    if bad:
        raise RuntimeError(f"{path}: forbidden failure markers: {', '.join(bad)}")
    return {"path": str(path), "size": path.stat().st_size,
            "sha256": sha256(path), "markers": list(markers)}


def check_external_apps(args, files: dict[str, dict]) -> dict:
    result = {"status": "not supplied", "full_compatibility": False}
    for label, supplied in (("edlin", args.edlin), ("debug", args.debug)):
        if supplied is None:
            continue
        path = supplied.resolve()
        if not path.is_file():
            raise RuntimeError(f"missing external {label} binary: {path}")
        target = args.archive / "external" / path.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        files[str(target.relative_to(args.archive))] = {
            "source": str(path), "size": target.stat().st_size,
            "sha256": sha256(target), "kind": "external MS-DOS 4.0 artifact"
        }
        result[label] = {"path": str(path), "size": path.stat().st_size,
                         "sha256": sha256(path)}
    if any(key in result for key in ("edlin", "debug")):
        result["status"] = "external inputs recorded; smoke/full application status is separate"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--archive", type=Path, default=DEFAULT_ARCHIVE)
    parser.add_argument("--full-check-log", type=Path)
    parser.add_argument("--vbox-log", type=Path)
    parser.add_argument("--edlin", type=Path)
    parser.add_argument("--debug", type=Path)
    parser.add_argument("--run-terminals", action="store_true",
                        help="run the normal BIOS/UEFI terminal test before collecting")
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()
    args.archive = args.archive.resolve()
    if args.archive == ROOT or ROOT in args.archive.parents:
        parser.error("--archive must be outside the repository")

    if args.run_terminals:
        command = [sys.executable, str(ROOT / "tools/run-terminal-test.py"),
                   "--build-dir", str(args.build_dir), "--log-dir",
                   str(args.build_dir / "terminal-test"), "bios", "uefi"]
        subprocess.run(command, check=True)

    iso = args.build_dir / "funyos.iso"
    if not iso.is_file():
        raise RuntimeError(f"missing ISO: {iso}; build it with make all first")
    args.archive.mkdir(parents=True, exist_ok=True)
    files: dict[str, dict] = {}
    shutil.copy2(iso, args.archive / iso.name)
    files[iso.name] = {"source": str(iso), "size": iso.stat().st_size,
                       "sha256": sha256(iso)}

    logs = {}
    for name, (relative, markers) in REQUIRED_LOGS.items():
        source = args.build_dir / relative
        logs[name] = require_markers(source, markers)
        evidence_dir = args.archive / "terminal" / name
        copy_evidence(source, evidence_dir, files, args.archive)
        qemu = source.with_name("qemu.log")
        copy_evidence(qemu, evidence_dir, files, args.archive)

    external = check_external_apps(args, files)
    for candidate, label in ((args.full_check_log, "full_check"), (args.vbox_log, "virtualbox")):
        if candidate is not None:
            if not candidate.is_file():
                raise RuntimeError(f"missing supplied {label} log: {candidate}")
            copy_evidence(candidate, args.archive / "logs", files, args.archive)

    manifest = {
        "schema": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "project": "FunnyOS",
        "scope": "M6 integration evidence, not a claim of complete DOS application compatibility",
        "build_dir": str(args.build_dir),
        "iso": files[iso.name],
        "terminal_logs": logs,
        "external_applications": external,
        "files": files,
        "claims": {
            "mz_and_exec_iso_smoke": True,
            "edlin_full_compatibility": False,
            "debug_full_compatibility": False,
            "self_written_fixtures_are_application_proof": False,
        },
    }
    (args.archive / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    (args.archive / "SHA256SUMS").write_text(
        "\n".join(f"{entry['sha256']}  {name}" for name, entry in sorted(files.items())) + "\n"
    )
    (args.archive / "README.txt").write_text(
        "M6 acceptance evidence\n"
        "======================\n"
        "This directory is generated by tools/run-m6-acceptance.py.\n"
        "It records the ISO and selected logs with SHA-256 hashes.\n"
        "The MZ/EXEC strings are guest fixtures, not proof that all real DOS\n"
        "applications are compatible. EDLIN/DEBUG inputs, when present, are\n"
        "external MS-DOS 4.0 artifacts and require their own smoke/full report.\n"
    )
    print(f"M6 acceptance archive: {args.archive}")
    print(f"ISO SHA-256: {files[iso.name]['sha256']}")
    print("Terminal BIOS/UEFI markers: PASS")
    print("EDLIN/DEBUG full compatibility: NOT CLAIMED")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"M6 acceptance failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
