#!/usr/bin/env python3
"""Agent A Headless Ghidra 12.x Project Initializer.

Imports lawfully obtained MS-DOS INTERLNK.EXE and INTERSVR.EXE binaries into a
local Ghidra 12.x project using the x86:LE:16:Real Mode processor architecture
and invokes the DOS Device Header post-script.

Legal Notice: Strictly bound by § 69e UrhG interoperability limits.
Original binaries reside locally in analysis/binaries/ (gitignored) and are
never committed to the repository.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PROJECT_DIR = REPO_ROOT / "analysis" / "ghidra_project"
DEFAULT_BINARIES_DIR = REPO_ROOT / "analysis" / "binaries"
DEFAULT_SCRIPTS_DIR = REPO_ROOT / "analysis" / "ghidra_scripts"
AUDIT_LOG_PATH = REPO_ROOT / "audit_agent_a.log"

PROCESSOR_ID = "x86:LE:16:Real Mode"
CSPEC_ID = "default"
TARGET_BINARIES = ("INTERLNK.EXE", "INTERSVR.EXE")


def find_analyze_headless(ghidra_home: Optional[str]) -> Optional[Path]:
    candidates: list[Path] = []
    if ghidra_home:
        candidates.append(Path(ghidra_home) / "support" / "analyzeHeadless")
    env_home = os.environ.get("GHIDRA_HOME") or os.environ.get("GHIDRA_INSTALL_DIR")
    if env_home:
        candidates.append(Path(env_home) / "support" / "analyzeHeadless")
    in_path = shutil.which("analyzeHeadless")
    if in_path:
        candidates.append(Path(in_path))

    for cand in candidates:
        if cand.is_file():
            return cand
    return None


def append_audit_log(action: str, inputs: list[str], outputs: list[str], note: str) -> None:
    entry = {
        "ts": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "agent": "AGENT_A",
        "action": action,
        "inputs": inputs,
        "outputs": outputs,
        "compliance": "69e_UrhG",
        "note": note,
    }
    with AUDIT_LOG_PATH.open("a", encoding="utf-8") as fp:
        fp.write(json.dumps(entry, sort_keys=False) + "\n")


def build_headless_command(
    analyze_headless: Path,
    project_dir: Path,
    project_name: str,
    binary_path: Path,
    scripts_dir: Path,
) -> list[str]:
    return [
        str(analyze_headless),
        str(project_dir),
        project_name,
        "-import",
        str(binary_path),
        "-processor",
        PROCESSOR_ID,
        "-cspec",
        CSPEC_ID,
        "-scriptPath",
        str(scripts_dir),
        "-postScript",
        "setup_dos_driver.py",
        "-overwrite",
    ]


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Initialize Ghidra 12.x 16-bit Real-Mode project for INTERLNK/INTERSVR."
    )
    parser.add_argument("--ghidra-home", help="Path to Ghidra 12.x installation root")
    parser.add_argument("--project-dir", type=Path, default=DEFAULT_PROJECT_DIR)
    parser.add_argument("--project-name", default="interlnk_re")
    parser.add_argument("--binaries-dir", type=Path, default=DEFAULT_BINARIES_DIR)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Validate environment and binary availability without importing",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print exact analyzeHeadless commands without executing Ghidra",
    )
    args = parser.parse_args(argv)

    analyze_headless = find_analyze_headless(args.ghidra_home)
    found_binaries = [
        args.binaries_dir / name
        for name in TARGET_BINARIES
        if (args.binaries_dir / name).is_file()
    ]

    print(f"[setup_ghidra] Processor    : {PROCESSOR_ID} ({CSPEC_ID})")
    print(f"[setup_ghidra] Project dir  : {args.project_dir}")
    print(f"[setup_ghidra] Binaries dir : {args.binaries_dir}")
    print(
        f"[setup_ghidra] analyzeHeadless: "
        f"{analyze_headless if analyze_headless else 'NOT FOUND (set GHIDRA_HOME)'}"
    )
    for name in TARGET_BINARIES:
        p = args.binaries_dir / name
        status = "FOUND" if p.is_file() else "MISSING (place local binary here)"
        print(f"[setup_ghidra] Binary {name:<12}: {status}")

    if args.check:
        return 0

    args.project_dir.mkdir(parents=True, exist_ok=True)
    effective_bin = analyze_headless or Path("analyzeHeadless")
    targets = found_binaries or [args.binaries_dir / name for name in TARGET_BINARIES]

    if not args.dry_run:
        if analyze_headless is None:
            print("[setup_ghidra] ERROR: Ghidra analyzeHeadless not found.", file=sys.stderr)
            return 1
        if not found_binaries:
            print(
                f"[setup_ghidra] ERROR: No target binaries found in {args.binaries_dir}.",
                file=sys.stderr,
            )
            return 1

    for bin_path in targets:
        cmd = build_headless_command(
            effective_bin,
            args.project_dir,
            args.project_name,
            bin_path,
            DEFAULT_SCRIPTS_DIR,
        )
        print("[setup_ghidra] CMD:", shlex.join(cmd))
        if not args.dry_run:
            subprocess.run(cmd, check=True)
            append_audit_log(
                action="GHIDRA_HEADLESS_IMPORT",
                inputs=[f"local:{bin_path.name}"],
                outputs=[f"local:{args.project_dir.name}/{args.project_name}"],
                note=f"Imported {bin_path.name} as {PROCESSOR_ID}",
            )

    return 0


if __name__ == "__main__":
    sys.exit(main())
