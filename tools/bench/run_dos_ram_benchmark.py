#!/usr/bin/env python3
"""Automated MS-DOS 6.22 Conventional RAM Benchmark Runner (86Box 486DX2/66).

Compares conventional RAM consumption under authentic MS-DOS 6.22 between:
  - Resident Block Device Drivers : INTERLNK.EXE vs. OPENLNK.SYS (8K & 4K)
  - Foreground Server Executables : INTERSVR.EXE vs. OPENSVR.EXE (8K & 4K)
  - Standalone Client CLIs        : INTERLNK.EXE (CLI) vs. OPENLNK.EXE (8K & 4K)

How it works:
  1. Compiles MEMBENCH.EXE, COM2LOG.EXE, and MAX_PAYLOAD=4096 OpenLNK variants
     (OPENLNK4.SYS, OPENSVR4.EXE, OPENLNK4.EXE) into <workdir>/out/ via local
     Open Watcom v2 ($WATCOM) or the openlnk-watcom Docker image.
  2. Clones a bootable MS-DOS 6.22 Fixed-Size VHD into <workdir>/vm/ (never
     mutating the source VHD) and injects the multi-boot CONFIG.SYS matrix and
     AUTOEXEC.BAT orchestrator.
  3. Launches 86Box with COM1 (live OpenLNK/INTERLNK protocol pipe) and COM2
     (polled 115200 bps telemetry pipe), cold-restarting 86Box cleanly between
     each CONFIG.SYS boot step.
  4. Captures exact MS-DOS MEM /C & MEM /D outputs (offline and online-connected)
     plus MEMBENCH.EXE profiles (live INT 21h MCB allocation trace, 0xA5 dirty-
     memory working set / high-water mark under idle and active 115200 bps load,
     and /LIMIT:<KB> minimum conventional RAM probes).
  5. Parses the captured log into a structured Markdown/JSON summary.

Requires macOS (hdiutil/mount/fsck_msdos), 86Box with ROMs, a local bootable
MS-DOS 6.22 10 MB Fixed-Size VHD (e.g. analysis/binaries/vhds/dos622_client.vhd),
and Open Watcom v2 (or Docker image openlnk-watcom).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
PROFILER_DIR = Path(__file__).resolve().parent / "dos_profiler"
DEFAULT_VHD = REPO_ROOT / "analysis/binaries/vhds/dos622_client.vhd"
DEFAULT_WORKDIR = REPO_ROOT / "build/ram_bench"

# Fixed-Size 10 MB VHD geometry matching tools/runner/create_dos_vhds.py
DISK_BYTES = 306 * 4 * 17 * 512  # 10,653,696 bytes
DOS_MAX_CMDLINE = 126

DRIVER_BOOT_CONFIGS: tuple[tuple[str, str, str], ...] = (
    (
        "STEP0",
        "BASELINE_CLEAN_DOS622",
        "FILES=20\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\n",
    ),
    (
        "STEP1",
        "INTERLNK_DEFAULT_3DRV_COM_LPT",
        "FILES=20\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\nDEVICE=C:\\DOS\\INTERLNK.EXE\r\n",
    ),
    (
        "STEP2",
        "INTERLNK_4DRV_COM1_115200",
        "FILES=20\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\nDEVICE=C:\\DOS\\INTERLNK.EXE /DRIVES:4 /COM1 /BAUD:115200\r\n",
    ),
    (
        "STEP3",
        "INTERLNK_4DRV_COM1_NOPRINTER",
        "FILES=20\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\nDEVICE=C:\\DOS\\INTERLNK.EXE /DRIVES:4 /COM1 /BAUD:115200 /NOPRINTER\r\n",
    ),
    (
        "STEP4",
        "INTERLNK_1DRV_COM1_NOPRINTER",
        "FILES=20\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\nDEVICE=C:\\DOS\\INTERLNK.EXE /DRIVES:1 /COM1 /BAUD:115200 /NOPRINTER\r\n",
    ),
    (
        "STEP5",
        "OPENLNK_8K_4DRV_COM1_OFFLINE",
        "FILES=20\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\nDEVICE=C:\\BENCH\\OPENLNK.SYS /COM1 /BAUD:115200 /DRIVES:4\r\n",
    ),
    (
        "STEP6",
        "OPENLNK_8K_1DRV_COM1_OFFLINE",
        "FILES=20\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\nDEVICE=C:\\BENCH\\OPENLNK.SYS /COM1 /BAUD:115200 /DRIVES:1\r\n",
    ),
    (
        "STEP7",
        "OPENLNK_4K_4DRV_COM1_OFFLINE",
        "FILES=20\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\nDEVICE=C:\\BENCH\\OPENLNK4.SYS /COM1 /BAUD:115200 /DRIVES:4\r\n",
    ),
    (
        "STEP8",
        "INTERLNK_4DRV_COM1_NOPRINTER_ONLINE",
        "FILES=20\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\nDEVICE=C:\\DOS\\INTERLNK.EXE /DRIVES:4 /COM1 /BAUD:115200 /NOPRINTER\r\n",
    ),
    (
        "STEP9",
        "OPENLNK_8K_4DRV_COM1_ONLINE",
        "FILES=20\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\nDEVICE=C:\\BENCH\\OPENLNK.SYS /COM1 /BAUD:115200 /DRIVES:4\r\n",
    ),
    (
        "STEP10",
        "EXE_BENCHMARKS_CLEAN_DOS622",
        "FILES=20\r\nBUFFERS=10\r\nLASTDRIVE=Z\r\n",
    ),
)

LIMIT_PROBES: dict[str, tuple[str, tuple[int, ...], str]] = {
    "OPENLNK8": ("C:\\BENCH\\OPENLNK.EXE /?", (20, 22, 23, 24, 25, 26, 28, 32), ""),
    "OPENLNK4": ("C:\\BENCH\\OPENLNK4.EXE /?", (16, 18, 19, 20, 21, 22, 24, 28), ""),
    "INTERLNK_CLI": ("C:\\DOS\\INTERLNK.EXE", (14, 16, 17, 18, 20, 24), ""),
    "OPENSVR8": ("C:\\BENCH\\OPENSVR.EXE C:", (28, 30, 31, 32, 33, 34, 36, 40), "/KEY:ESC /TICKS:18"),
    "OPENSVR4": ("C:\\BENCH\\OPENSVR4.EXE C:", (24, 26, 27, 28, 29, 30, 32), "/KEY:ESC /TICKS:18"),
    "INTERSVR": (
        "C:\\DOS\\INTERSVR.EXE C:",
        (128, 131, 132, 133, 136, 144, 152, 160, 164, 165, 166, 168, 176),
        "/KEY:ALTF4 /TICKS:22",
    ),
}


def inspect_dos_binary(path: Path) -> dict[str, Any]:
    """Read static disk size and DOS MZ header memory metadata (or RAW .SYS header)."""
    data = path.read_bytes()
    info: dict[str, Any] = {
        "path": str(path),
        "name": path.name,
        "disk_bytes": len(data),
    }
    if len(data) >= 28 and data[:2] == b"MZ":
        (
            cblp, cp, crlc, cparhdr, minalloc, maxalloc,
            ss, sp, _csum, ip, cs, _lfarlc, _ovno,
        ) = struct.unpack_from("<13H", data, 2)
        hdr_bytes = cparhdr * 16
        file_img = cp * 512 - (0 if cblp == 0 else (512 - cblp))
        load_img = file_img - hdr_bytes
        static_min = load_img + minalloc * 16 + 256  # +256 B PSP
        info.update({
            "format": "MZ",
            "hdr_bytes": hdr_bytes,
            "load_image_bytes": load_img,
            "relocs": crlc,
            "minalloc_bytes": minalloc * 16,
            "maxalloc_bytes": maxalloc * 16,
            "static_min_dos_ram_bytes": static_min,
            "cs_ip": f"{cs:04X}:{ip:04X}",
            "ss_sp": f"{ss:04X}:{sp:04X}",
        })
    elif len(data) >= 18:
        nxt, attr, strat, intr, units = struct.unpack_from("<IHHHB", data, 0)
        info.update({
            "format": "SYS",
            "next_ptr": f"0x{nxt:08X}",
            "attr": f"0x{attr:04X}",
            "strategy_off": f"0x{strat:04X}",
            "interrupt_off": f"0x{intr:04X}",
            "units": units,
            "signature": data[11:18].decode("ascii", errors="replace"),
        })
    return info


def build_autoexec() -> str:
    """Build MS-DOS 6.22 AUTOEXEC.BAT orchestrator (all lines strictly <= 126 chars)."""
    lines: list[str] = [
        "@ECHO OFF",
        "PROMPT $P$G",
        "PATH C:\\DOS;C:\\BENCH;C:\\",
    ]
    for idx in range(len(DRIVER_BOOT_CONFIGS)):
        lines.append(f"IF EXIST C:\\BENCH\\STEP{idx}.FLG GOTO S{idx}")
    lines.append("C:\\BENCH\\COM2LOG -m ===BENCHMARK_ALREADY_DONE===")
    lines.append("GOTO END")

    for idx, (step_id, label, _cfg) in enumerate(DRIVER_BOOT_CONFIGS):
        lines.append(f":S{idx}")
        lines.append(f"C:\\BENCH\\COM2LOG -m @@@STEP_BEGIN {step_id} {label}")
        lines.append("C:\\DOS\\MEM.EXE /C > C:\\BENCH\\MEMC.TMP")
        lines.append("C:\\DOS\\MEM.EXE /D > C:\\BENCH\\MEMD.TMP")
        lines.append(f"C:\\BENCH\\COM2LOG -m ---MEM_C {step_id} {label}---")
        lines.append("C:\\BENCH\\COM2LOG -f C:\\BENCH\\MEMC.TMP")
        lines.append(f"C:\\BENCH\\COM2LOG -m ---MEM_D {step_id} {label}---")
        lines.append("C:\\BENCH\\COM2LOG -f C:\\BENCH\\MEMD.TMP")

        if idx == 3:
            lines.append("C:\\BENCH\\COM2LOG -m ---CLI_INTERLNK_WITH_DRV_OFFLINE---")
            lines.append("C:\\BENCH\\MEMBENCH /COM2 C:\\DOS\\INTERLNK.EXE > NUL")
        elif idx == 8:
            lines.append("C:\\BENCH\\COM2LOG -m ---CLI_INTERLNK_WITH_DRV_ONLINE---")
            lines.append("C:\\BENCH\\MEMBENCH /COM2 C:\\DOS\\INTERLNK.EXE > C:\\BENCH\\ICLI.TMP")
            lines.append("C:\\BENCH\\COM2LOG -f C:\\BENCH\\ICLI.TMP")
        elif idx == 9:
            lines.append("DIR D: > C:\\BENCH\\DIRD.TMP")
            lines.append("C:\\BENCH\\COM2LOG -m ---DIR_D_OPENLNK_SYS_ONLINE---")
            lines.append("C:\\BENCH\\COM2LOG -f C:\\BENCH\\DIRD.TMP")

        if idx < len(DRIVER_BOOT_CONFIGS) - 1:
            next_idx = idx + 1
            lines.append(f"DEL C:\\BENCH\\STEP{idx}.FLG")
            lines.append(f"COPY /Y C:\\BENCH\\CFG{next_idx}.SYS C:\\CONFIG.SYS > NUL")
            lines.append(f"ECHO 1 > C:\\BENCH\\STEP{next_idx}.FLG")
            lines.append(f"C:\\BENCH\\COM2LOG -m @@@STEP_DONE {step_id} NEXT STEP{next_idx}")
            lines.append("PAUSE > NUL")
            lines.append("GOTO END")
        else:
            # STEP 10: Foreground EXE Benchmarks on Clean MS-DOS 6.22
            lines.extend([
                "DEL C:\\BENCH\\STEP10.FLG",
                "C:\\BENCH\\COM2LOG -m @@@EXE_PHASE_CLI_ONLINE",
                "C:\\BENCH\\MEMBENCH /COM2 C:\\BENCH\\OPENLNK.EXE /COM1 /BAUD:115200 /DIR > NUL",
                "C:\\BENCH\\MEMBENCH /COM2 C:\\BENCH\\OPENLNK4.EXE /COM1 /BAUD:115200 /DIR > NUL",
                "C:\\BENCH\\COM2LOG -m @@@HOST_STOP_OPENSVR",
                "C:\\BENCH\\COM2LOG -m @@@EXE_PHASE_INTERLNK_CLI_CLEAN",
                "C:\\BENCH\\MEMBENCH /COM2 C:\\DOS\\INTERLNK.EXE > NUL",
                # OPENSVR.EXE (8K) — Idle + Active 115200 bps dir
                "C:\\BENCH\\COM2LOG -m @@@EXE_PHASE_OPENSVR8_IDLE",
                "C:\\BENCH\\MEMBENCH /COM2 /KEY:ESC /TICKS:36 C:\\BENCH\\OPENSVR.EXE C: /COM1 /BAUD:115200",
                "C:\\BENCH\\COM2LOG -m @@@EXE_PHASE_OPENSVR8_ACTIVE",
                "C:\\BENCH\\COM2LOG -m @@@HOST_RUN_CLIENT_DIR",
                "C:\\BENCH\\MEMBENCH /COM2 /KEY:ESC /TICKS:90 C:\\BENCH\\OPENSVR.EXE C: /COM1 /BAUD:115200",
                # OPENSVR4.EXE (4K) — Idle
                "C:\\BENCH\\COM2LOG -m @@@EXE_PHASE_OPENSVR4_IDLE",
                "C:\\BENCH\\MEMBENCH /COM2 /KEY:ESC /TICKS:36 C:\\BENCH\\OPENSVR4.EXE C: /COM1 /BAUD:115200",
                # INTERSVR.EXE — Idle + Active 115200 bps dir
                "C:\\BENCH\\COM2LOG -m @@@EXE_PHASE_INTERSVR_IDLE",
                "C:\\BENCH\\MEMBENCH /COM2 /KEY:ALTF4 /TICKS:45 C:\\DOS\\INTERSVR.EXE C: /COM1 /BAUD:115200",
                "C:\\BENCH\\COM2LOG -m @@@EXE_PHASE_INTERSVR_ACTIVE",
                "C:\\BENCH\\COM2LOG -m @@@HOST_RUN_CLIENT_DIR",
                "C:\\BENCH\\MEMBENCH /COM2 /KEY:ALTF4 /TICKS:110 C:\\DOS\\INTERSVR.EXE C: /COM1 /BAUD:115200",
            ])

            # Explicit one-line-per-probe /LIMIT:<KB> runs (stays well under 127-char COMMAND.COM cap)
            for tag, (target_cmd, kb_values, key_opts) in LIMIT_PROBES.items():
                lines.append(f"C:\\BENCH\\COM2LOG -m @@@LIMIT_PROBE_{tag}")
                opts = f"{key_opts} " if key_opts else ""
                redir = " > NUL" if not key_opts else ""
                for kb in kb_values:
                    lines.append(
                        f"C:\\BENCH\\MEMBENCH /QUIET /COM2 {opts}/LIMIT:{kb} {target_cmd}{redir}"
                    )

            lines.append("C:\\BENCH\\COM2LOG -m ===BENCHMARK_DONE===")

    lines.append(":END")

    for idx, ln in enumerate(lines, start=1):
        if len(ln) > DOS_MAX_CMDLINE:
            raise ValueError(f"AUTOEXEC.BAT line {idx} exceeds {DOS_MAX_CMDLINE} chars ({len(ln)}): {ln}")
    return "\r\n".join(lines) + "\r\n"


def find_86box(explicit: Optional[Path] = None) -> Optional[Path]:
    if explicit is not None and explicit.is_file():
        return explicit
    env_bin = os.environ.get("EIGHTYSIXBOX_BIN")
    if env_bin and Path(env_bin).is_file():
        return Path(env_bin)
    for candidate in (
        shutil.which("86Box"),
        shutil.which("86box"),
        str(Path.home() / "Applications/86Box.app/Contents/MacOS/86Box"),
        "/Applications/86Box.app/Contents/MacOS/86Box",
    ):
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


def find_host_binary(name: str) -> Optional[Path]:
    """Return the newest built desktop cleanroom binary (opensvr or openlnk)."""
    candidates = [
        p for p in (REPO_ROOT / "cleanroom/build" / name, REPO_ROOT / "cleanroom" / name)
        if p.is_file() and os.access(p, os.X_OK)
    ]
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime)


def build_dos_artifacts(out_dir: Path, watcom_docker: str) -> None:
    """Compile MEMBENCH.EXE, COM2LOG.EXE, and 4 KiB MAX_PAYLOAD=4096 OpenLNK variants."""
    out_dir.mkdir(parents=True, exist_ok=True)
    watcom_env = os.environ.get("WATCOM")
    if watcom_env and (Path(watcom_env) / "binl64/wcc").is_file():
        env = os.environ.copy()
        env["PATH"] = f"{watcom_env}/binl64:{env.get('PATH', '')}"
        env["INCLUDE"] = f"{watcom_env}/h"
        subprocess.run(
            ["make", "-C", str(PROFILER_DIR), "-f", "Makefile.watcom", f"OUT_DIR={out_dir}", "all"],
            env=env,
            check=True,
        )
        with tempfile.TemporaryDirectory(prefix="olnk_4k_") as tmp_s:
            cdir = Path(tmp_s) / "cleanroom"
            shutil.copytree(REPO_ROOT / "cleanroom", cdir, ignore=shutil.ignore_patterns("build"))
            subprocess.run(
                [
                    "make", "-C", str(cdir / "dos"), "-f", "Makefile.watcom",
                    "clean", "all", "MAX_PAYLOAD=4096", "SYS_MAX_PAYLOAD=4096",
                ],
                env=env,
                check=True,
            )
            for src_n, dst_n in (
                ("OPENLNK.SYS", "OPENLNK4.SYS"),
                ("OPENSVR.EXE", "OPENSVR4.EXE"),
                ("OPENLNK.EXE", "OPENLNK4.EXE"),
                ("OPENLNK.MAP", "OPENLNK4.MAP"),
                ("OPENSVR.MAP", "OPENSVR4.MAP"),
                ("OPENLNKX.MAP", "OPENLNKX4.MAP"),
            ):
                shutil.copyfile(cdir / "dos/build" / src_n, out_dir / dst_n)
        return

    cmd = [
        "docker", "run", "--rm", "--platform", "linux/amd64",
        "-v", f"{REPO_ROOT}:/workspace:ro",
        "-v", f"{out_dir}:/bench_out",
        watcom_docker,
        "bash", "-c",
        (
            "set -euo pipefail; "
            "export WATCOM=/opt/watcom; export PATH=\"$WATCOM/binl64:$PATH\"; export INCLUDE=\"$WATCOM/h\"; "
            "cp -r /workspace/tools/bench/dos_profiler /tmp/dos_profiler; "
            "make -C /tmp/dos_profiler -f Makefile.watcom OUT_DIR=/bench_out all; "
            "cp -r /workspace/cleanroom /tmp/cleanroom4k; "
            "make -C /tmp/cleanroom4k/dos -f Makefile.watcom clean all MAX_PAYLOAD=4096 SYS_MAX_PAYLOAD=4096; "
            "cp /tmp/cleanroom4k/dos/build/OPENLNK.SYS /bench_out/OPENLNK4.SYS; "
            "cp /tmp/cleanroom4k/dos/build/OPENSVR.EXE /bench_out/OPENSVR4.EXE; "
            "cp /tmp/cleanroom4k/dos/build/OPENLNK.EXE /bench_out/OPENLNK4.EXE; "
            "cp /tmp/cleanroom4k/dos/build/OPENLNK.MAP /bench_out/OPENLNK4.MAP; "
            "cp /tmp/cleanroom4k/dos/build/OPENSVR.MAP /bench_out/OPENSVR4.MAP; "
            "cp /tmp/cleanroom4k/dos/build/OPENLNKX.MAP /bench_out/OPENLNKX4.MAP"
        ),
    ]
    subprocess.run(cmd, check=True)


def _run(cmd: list[str]) -> str:
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def prepare_temp_vhd(src_vhd: Path, dst_vhd: Path, dos8k_dir: Path, out_dir: Path, vm_dir: Path) -> None:
    """Clone src_vhd into dst_vhd and inject benchmark configs & binaries (macOS hdiutil)."""
    if sys.platform != "darwin":
        raise RuntimeError("Automatic VHD staging currently requires macOS (hdiutil/mount/fsck_msdos).")

    vm_dir.mkdir(parents=True, exist_ok=True)
    src_nvr = REPO_ROOT / "build/86box_run_486/client/nvr"
    dst_nvr = vm_dir / "nvr"
    if src_nvr.is_dir() and not dst_nvr.exists():
        shutil.copytree(src_nvr, dst_nvr)

    raw_bytes = src_vhd.read_bytes()
    if len(raw_bytes) != DISK_BYTES + 512 or raw_bytes[-512:-504] != b"conectix":
        raise ValueError(f"Expected 10 MB Fixed-Size VHD at {src_vhd}")
    footer = raw_bytes[DISK_BYTES:]

    with tempfile.TemporaryDirectory(prefix="olnk_rambench_") as tmp_s:
        tmp = Path(tmp_s)
        raw_img = tmp / "disk.raw"
        mnt_dir = tmp / "mnt"
        mnt_dir.mkdir()
        raw_img.write_bytes(raw_bytes[:DISK_BYTES])

        attach_out = _run([
            "hdiutil", "attach",
            "-imagekey", "diskimage-class=CRawDiskImage",
            "-nomount", str(raw_img),
        ])
        whole_dev = part_dev = ""
        for line in attach_out.splitlines():
            parts = line.split()
            if not parts:
                continue
            if not whole_dev:
                whole_dev = parts[0]
            if parts[0].startswith("/dev/disk") and "s1" in parts[0]:
                part_dev = parts[0]
        if not (whole_dev and part_dev):
            raise RuntimeError(f"Failed to parse hdiutil output:\n{attach_out}")

        mounted = False
        try:
            _run(["mount", "-t", "msdos", "-o", "nobrowse", part_dev, str(mnt_dir)])
            mounted = True

            bench_dir = mnt_dir / "BENCH"
            bench_dir.mkdir(exist_ok=True)
            for flg in bench_dir.glob("*.FLG"):
                flg.unlink()

            for name in ("OPENLNK.SYS", "OPENSVR.EXE", "OPENLNK.EXE"):
                shutil.copyfile(dos8k_dir / name, bench_dir / name)
            for name in ("OPENLNK4.SYS", "OPENSVR4.EXE", "OPENLNK4.EXE", "MEMBENCH.EXE", "COM2LOG.EXE"):
                shutil.copyfile(out_dir / name, bench_dir / name)

            for idx, (_sid, _lbl, cfg_txt) in enumerate(DRIVER_BOOT_CONFIGS):
                (bench_dir / f"CFG{idx}.SYS").write_bytes(cfg_txt.encode("ascii"))

            (mnt_dir / "CONFIG.SYS").write_bytes(DRIVER_BOOT_CONFIGS[0][2].encode("ascii"))
            (mnt_dir / "AUTOEXEC.BAT").write_bytes(build_autoexec().encode("ascii"))
            (bench_dir / "STEP0.FLG").write_bytes(b"1\r\n")

            for dot_under in mnt_dir.rglob("._*"):
                dot_under.unlink(missing_ok=True)
            for junk in (".fseventsd", ".Spotlight-V100", ".Trashes"):
                jp = mnt_dir / junk
                if jp.is_dir():
                    shutil.rmtree(jp, ignore_errors=True)
                elif jp.is_file():
                    jp.unlink(missing_ok=True)

            subprocess.run(["sync"], check=False)
            for attempt in range(5):
                if subprocess.run(["/usr/sbin/diskutil", "unmount", str(mnt_dir)], capture_output=True).returncode == 0:
                    mounted = False
                    break
                time.sleep(0.4 * (attempt + 1))
            if mounted:
                _run(["/usr/sbin/diskutil", "unmount", "force", str(mnt_dir)])
                mounted = False
            _run(["/sbin/fsck_msdos", "-n", part_dev])
        finally:
            if mounted:
                subprocess.run(["/usr/sbin/diskutil", "unmount", "force", str(mnt_dir)], check=False)
            subprocess.run(["hdiutil", "detach", "-force", whole_dev], check=False)

        with raw_img.open("rb") as src, dst_vhd.open("wb") as dst:
            shutil.copyfileobj(src, dst)
            dst.write(footer)


def write_86box_cfg(vm_dir: Path, bench_vhd: Path, pipe_com1: str, pipe_com2: str) -> Path:
    cfg_path = vm_dir / "86box.cfg"
    cfg_path.write_text(
        f"""[General]
vid_renderer = qt_software
uuid = 9f914830-74e7-5f8f-af20-c6eaf439a1d9

[Machine]
cpu_family = i486dx2
cpu_multi = 2
cpu_speed = 66666666
cpu_use_dynarec = 0
fpu_type = internal
machine = 486vipio2
mem_size = 4096
time_sync = disabled

[Video]
gfxcard = s3_trio64_pci

[Input devices]
keyboard_type = keyboard_at
mouse_type = none

[Ports (COM & LPT)]
serial1_device = pipe
serial2_device = pipe
lpt1_enabled = 1

[Storage controllers]
hdc_1 = xta_wdxt150

[Floppy and CD-ROM drives]
fdd_01_type = 35_2hd
fdd_02_type = none
fdd_host_buffering = 1

[Hard disks]
hdd_01_fn = {bench_vhd}
hdd_01_parameters = 17, 4, 306, 0, xta
hdd_01_xta_channel = 0

[Named Pipe (COM) #1]
path = {pipe_com1}
mode = 1
reconnect = 1

[Named Pipe (COM) #2]
path = {pipe_com2}
mode = 1
reconnect = 1
""",
        encoding="ascii",
    )
    return cfg_path


def parse_benchmark_log(raw_bytes: bytes) -> dict[str, Any]:
    """Parse captured COM2 benchmark log into structured driver & executable metrics."""
    clean = (
        bytes(b for b in raw_bytes if b in (9, 10, 13) or 32 <= b <= 126)
        .decode("ascii")
        .replace("\r\n", "\n")
        .replace("\r", "\n")
    )
    drivers: list[dict[str, Any]] = []
    baseline_used: Optional[int] = None

    for blk in re.split(r"(?=@@@STEP_BEGIN\s+)", clean):
        m_hdr = re.match(r"@@@STEP_BEGIN\s+(\S+)\s+(\S+)", blk)
        if not m_hdr:
            continue
        step_id, label = m_hdr.group(1), m_hdr.group(2)
        m_conv = re.search(r"Conventional\s+655,360\s+([\d,]+)\s+([\d,]+)", blk)
        m_lrg = re.search(r"Largest executable program size\s+([\d,]+)", blk)
        m_drv = re.search(r"^\s+([\d,]+)\s+\(\d+K\)\s+(.+?)\s+Installed Device=(\S+)", blk, re.M)
        if not (m_conv and m_lrg):
            continue
        used = int(m_conv.group(1).replace(",", ""))
        free = int(m_conv.group(2).replace(",", ""))
        largest = int(m_lrg.group(1).replace(",", ""))
        if baseline_used is None and "BASELINE" in label:
            baseline_used = used
        drivers.append({
            "step": step_id,
            "label": label,
            "used_bytes": used,
            "free_bytes": free,
            "largest_exe_bytes": largest,
            "delta_used_bytes": (used - baseline_used) if baseline_used is not None else 0,
            "mem_d_driver_bytes": int(m_drv.group(1).replace(",", "")) if m_drv else 0,
            "mem_d_device": m_drv.group(3) if m_drv else None,
            "mem_d_units": m_drv.group(2).strip() if m_drv else None,
        })

    # Parse MEMBENCH blocks
    exe_runs: list[dict[str, Any]] = []
    for m in re.finditer(
        r"=== MEMBENCH target=(\S+) limit_kb=(\d+) free_before=(\d+) paras \((\d+) B\) key=0x([0-9A-Fa-f]+) ticks=(\d+) ===\n"
        r"((?:  .*\n)*)",
        clean,
    ):
        body = m.group(7)
        rec: dict[str, Any] = {
            "target": m.group(1),
            "limit_kb": int(m.group(2)),
            "free_before_bytes": int(m.group(4)),
        }
        m_res = re.search(
            r"EXEC_RESULT: rc=0x([0-9A-Fa-f]+) exit_code=0x([0-9A-Fa-f]+) ticks=(\d+) inj_sent=(\d+) alloc_events=(\d+)",
            body,
        )
        if m_res:
            rec["exec_rc"] = int(m_res.group(1), 16)
            rec["exit_code"] = int(m_res.group(2), 16)
            rec["ticks"] = int(m_res.group(3))
            rec["alloc_events"] = int(m_res.group(5))
        m_mcb = re.search(r"MCB_SUM\[(?:LIVE|EXIT)\]: child_psp=([0-9A-Fa-f]+) blocks=(\d+) total_incl_hdr=(\d+) B aux=(\d+) B", body)
        if m_mcb:
            rec["mcb_total_incl_hdr_bytes"] = int(m_mcb.group(3))
            rec["mcb_aux_bytes"] = int(m_mcb.group(4))
        m_dirty = re.search(
            r"DIRTY_SUMMARY: touched_paras=(\d+) \((\d+) B\), exact_dirty_bytes=(\d+) B, "
            r"hwm_from_free=(\d+) B, hwm_from_psp=(\d+) B",
            body,
        )
        if m_dirty:
            rec["touched_bytes"] = int(m_dirty.group(2))
            rec["exact_dirty_bytes"] = int(m_dirty.group(3))
            rec["hwm_from_free_bytes"] = int(m_dirty.group(4))
            rec["hwm_from_psp_bytes"] = int(m_dirty.group(5))
        exe_runs.append(rec)

    # Summarize minimum passing /LIMIT:<KB> per target
    min_limit_kb: dict[str, int] = {}
    for run in exe_runs:
        if run["limit_kb"] > 0 and run.get("exec_rc") == 0:
            tgt = run["target"]
            if tgt not in min_limit_kb or run["limit_kb"] < min_limit_kb[tgt]:
                min_limit_kb[tgt] = run["limit_kb"]

    return {
        "drivers": drivers,
        "exe_runs": exe_runs,
        "min_limit_kb": min_limit_kb,
    }


def format_markdown_summary(parsed: dict[str, Any]) -> str:
    lines = [
        "### Resident Driver RAM (`MEM /C` & `MEM /D`)",
        "",
        "| Step | Configuration | `MEM /D` Driver | Total RAM Delta | Largest Free EXE |",
        "| :--- | :--- | :---: | :---: | :---: |",
    ]
    for d in parsed["drivers"]:
        delta = d["delta_used_bytes"]
        lines.append(
            f"| `{d['step']}` | `{d['label']}` | `{d['mem_d_driver_bytes']:,} B` | "
            f"`{delta:,} B` ({delta / 1024:.2f} KiB) | `{d['largest_exe_bytes']:,} B` |"
        )
    if parsed["min_limit_kb"]:
        lines.extend([
            "",
            "### Minimum Free Conventional RAM (`/LIMIT:<KB>`)",
            "",
            "| Target | Minimum Free Conventional RAM |",
            "| :--- | :---: |",
        ])
        for tgt, kb in sorted(parsed["min_limit_kb"].items()):
            lines.append(f"| `{tgt}` | **`{kb} KiB`** |")
    return "\n".join(lines) + "\n"


def _ensure_fifos(prefix: str) -> None:
    for suffix in (".in", ".out"):
        p = Path(prefix + suffix)
        if p.exists():
            p.unlink()
        os.mkfifo(p, 0o600)


def run_live_benchmark(args: argparse.Namespace) -> int:
    workdir: Path = args.workdir.resolve()
    out_dir = workdir / "out"
    vm_dir = workdir / "vm_486"
    bench_vhd = vm_dir / "dos622_bench.vhd"
    share_dir = workdir / "share_dir"
    log_file = workdir / "benchmark_live.log"
    dos8k_dir = REPO_ROOT / "cleanroom/dos/build"

    box_bin = find_86box(args.eightysixbox)
    opensvr_bin = find_host_binary("opensvr")
    openlnk_bin = find_host_binary("openlnk")

    if not args.resume and not args.skip_build:
        print("[ram-bench] Compiling MEMBENCH.EXE, COM2LOG.EXE, and 4K OpenLNK variants...", flush=True)
        build_dos_artifacts(out_dir, args.watcom_docker)

    if args.dry_run:
        autoexec = build_autoexec()
        print(f"[ram-bench] DRY RUN OK:")
        print(f"  86Box       : {box_bin or '(not found)'}")
        print(f"  opensvr     : {opensvr_bin or '(not found)'}")
        print(f"  openlnk     : {openlnk_bin or '(not found)'}")
        print(f"  Source VHD  : {args.vhd} (exists={args.vhd.is_file()})")
        print(f"  Workdir     : {workdir}")
        print(f"  Boot steps  : {len(DRIVER_BOOT_CONFIGS)}")
        print(f"  AUTOEXEC    : {len(autoexec.splitlines())} lines (max {max(len(l) for l in autoexec.splitlines())} chars <= {DOS_MAX_CMDLINE})")
        return 0

    if box_bin is None:
        print("[ram-bench] ERROR: 86Box binary not found. Pass --86box <path>.", file=sys.stderr)
        return 1
    if opensvr_bin is None or openlnk_bin is None:
        print("[ram-bench] ERROR: Build desktop opensvr & openlnk first (cmake --build cleanroom/build).", file=sys.stderr)
        return 1
    if not args.resume:
        if not args.vhd.is_file():
            print(f"[ram-bench] ERROR: MS-DOS 6.22 VHD not found: {args.vhd}", file=sys.stderr)
            return 1
        print(f"[ram-bench] Staging temporary VHD copy at {bench_vhd}...", flush=True)
        prepare_temp_vhd(args.vhd, bench_vhd, dos8k_dir, out_dir, vm_dir)

    pipe_com1 = f"/tmp/openlnk_rambench_com1_{os.getpid()}"
    pipe_com2 = f"/tmp/openlnk_rambench_com2_{os.getpid()}"
    cfg_path = write_86box_cfg(vm_dir, bench_vhd, pipe_com1, pipe_com2)
    share_dir.mkdir(parents=True, exist_ok=True)
    (share_dir / "BENCH.TXT").write_text("OpenLNK RAM Benchmark Host Share\r\n", encoding="ascii")

    _ensure_fifos(pipe_com1)
    _ensure_fifos(pipe_com2)

    com2_rx = os.open(pipe_com2 + ".out", os.O_RDWR | os.O_NONBLOCK)
    com2_tx = os.open(pipe_com2 + ".in", os.O_RDWR | os.O_NONBLOCK)
    com1_rx = os.open(pipe_com1 + ".out", os.O_RDWR | os.O_NONBLOCK)
    com1_tx = os.open(pipe_com1 + ".in", os.O_RDWR | os.O_NONBLOCK)

    opensvr_proc: Optional[subprocess.Popen[bytes]] = None
    client_proc: Optional[subprocess.Popen[bytes]] = None
    vm_proc: Optional[subprocess.Popen[bytes]] = None

    def stop_vm() -> None:
        nonlocal vm_proc
        if vm_proc is not None and vm_proc.poll() is None:
            vm_proc.terminate()
            try:
                vm_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                vm_proc.kill()
                vm_proc.wait(timeout=3)
        vm_proc = None

    def start_vm(title: str) -> None:
        nonlocal vm_proc
        stop_vm()
        vm_proc = subprocess.Popen(
            [str(box_bin), "-N", "-P", str(vm_dir), "-V", title],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def start_opensvr() -> None:
        nonlocal opensvr_proc, com1_rx, com1_tx
        if opensvr_proc is not None and opensvr_proc.poll() is None:
            opensvr_proc.terminate()
            try:
                opensvr_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                opensvr_proc.kill()
        opensvr_proc = None
        if com1_rx >= 0:
            os.close(com1_rx)
            os.close(com1_tx)
            com1_rx = com1_tx = -1
        opensvr_proc = subprocess.Popen(
            [str(opensvr_bin), "--pipe", pipe_com1, "--dir", str(share_dir), "--baud", "115200"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(0.25)

    try:
        print(f"[ram-bench] Launching 86Box (486DX2/66, MS-DOS 6.22): {cfg_path}", flush=True)
        start_vm("OpenLNK-RAM-Bench-STEP0")

        buf = ""
        done = False
        last_activity = time.monotonic()
        start_time = last_activity

        with log_file.open("a" if args.resume else "w", encoding="utf-8", errors="replace") as lf:
            while not done:
                now = time.monotonic()
                if now - start_time > args.timeout:
                    print(f"[ram-bench] ERROR: Overall {args.timeout}s timeout reached!", file=sys.stderr)
                    break
                if now - last_activity > 90:
                    print("[ram-bench] ERROR: 90s silence on COM2 — aborting.", file=sys.stderr)
                    break
                if vm_proc is not None and vm_proc.poll() is not None:
                    print(f"[ram-bench] 86Box exited unexpectedly (rc={vm_proc.returncode}).", file=sys.stderr)
                    break

                rlist, _, _ = select.select([com2_rx], [], [], 0.2)
                if com2_rx not in rlist:
                    continue
                try:
                    chunk = os.read(com2_rx, 4096)
                except BlockingIOError:
                    chunk = b""
                if not chunk:
                    continue

                last_activity = time.monotonic()
                text = chunk.decode("latin-1", errors="replace")
                lf.write(text)
                lf.flush()
                buf += text
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.rstrip("\r")
                    if line.startswith(("@@@", "===", "  EXEC_RESULT", "  DIRTY_SUMMARY", "  MCB_SUM", "  ALLOC_EV")):
                        print(line, flush=True)

                    if "@@@STEP_DONE" in line:
                        time.sleep(0.35)
                        stop_vm()
                        while True:
                            try:
                                if not os.read(com2_rx, 4096):
                                    break
                            except BlockingIOError:
                                break
                        if any(tok in line for tok in ("NEXT STEP8", "NEXT STEP9", "NEXT STEP10")):
                            start_opensvr()
                        next_tag = line.split()[-1] if line.split() else "NEXT"
                        last_activity = time.monotonic()
                        start_vm(f"OpenLNK-RAM-Bench-{next_tag}")
                        break
                    elif "@@@HOST_STOP_OPENSVR" in line:
                        if opensvr_proc is not None:
                            opensvr_proc.terminate()
                            try:
                                opensvr_proc.wait(timeout=3)
                            except subprocess.TimeoutExpired:
                                opensvr_proc.kill()
                            opensvr_proc = None
                            com1_rx = os.open(pipe_com1 + ".out", os.O_RDWR | os.O_NONBLOCK)
                            com1_tx = os.open(pipe_com1 + ".in", os.O_RDWR | os.O_NONBLOCK)
                    elif "@@@HOST_RUN_CLIENT_DIR" in line:
                        if client_proc is not None and client_proc.poll() is None:
                            client_proc.kill()
                        client_proc = subprocess.Popen(
                            [
                                "bash", "-c",
                                f"sleep 0.9 && {openlnk_bin} --pipe {pipe_com1} --baud 115200 dir >> {workdir / 'host_client.log'} 2>&1",
                            ]
                        )
                    elif "===BENCHMARK_DONE===" in line:
                        done = True
                        break

        parsed = parse_benchmark_log(log_file.read_bytes())
        md = format_markdown_summary(parsed)
        print("\n" + md)
        if args.out_json:
            args.out_json.write_text(json.dumps(parsed, indent=2) + "\n", encoding="utf-8")
        if args.out_md:
            args.out_md.write_text(md, encoding="utf-8")
        return 0 if done else 1
    finally:
        for proc in (client_proc, opensvr_proc, vm_proc):
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
        for fd in (com1_rx, com1_tx, com2_rx, com2_tx):
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError:
                    pass
        for pfx in (pipe_com1, pipe_com2):
            for suf in (".in", ".out"):
                Path(pfx + suf).unlink(missing_ok=True)


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Automated MS-DOS 6.22 RAM benchmark comparing OpenLNK vs. original INTERLNK/INTERSVR in 86Box."
    )
    parser.add_argument("--vhd", type=Path, default=DEFAULT_VHD, help="Bootable MS-DOS 6.22 10 MB Fixed-Size VHD")
    parser.add_argument("--workdir", type=Path, default=DEFAULT_WORKDIR, help="Temporary build/VM/log directory")
    parser.add_argument("--86box", dest="eightysixbox", type=Path, help="Explicit path to 86Box executable")
    parser.add_argument("--watcom-docker", default="openlnk-watcom", help="Docker image name if $WATCOM is not local")
    parser.add_argument("--timeout", type=int, default=900, help="Overall benchmark timeout in seconds (default: 900)")
    parser.add_argument("--skip-build", action="store_true", help="Reuse prebuilt profiler & 4K binaries in <workdir>/out")
    parser.add_argument("--resume", action="store_true", help="Resume from existing staged VHD in <workdir>/vm_486")
    parser.add_argument("--dry-run", action="store_true", help="Validate environment & AUTOEXEC.BAT without launching 86Box")
    parser.add_argument("--parse-log", type=Path, help="Parse an existing COM2 benchmark log offline and exit")
    parser.add_argument("--out-json", type=Path, help="Optional path to write structured JSON benchmark results")
    parser.add_argument("--out-md", type=Path, help="Optional path to write Markdown summary table")
    args = parser.parse_args(argv)

    if args.parse_log:
        parsed = parse_benchmark_log(args.parse_log.read_bytes())
        md = format_markdown_summary(parsed)
        print(md)
        if args.out_json:
            args.out_json.write_text(json.dumps(parsed, indent=2) + "\n", encoding="utf-8")
        if args.out_md:
            args.out_md.write_text(md, encoding="utf-8")
        return 0

    return run_live_benchmark(args)


if __name__ == "__main__":
    sys.exit(main())
