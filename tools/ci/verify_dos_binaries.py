#!/usr/bin/env python3
"""Verify 16-bit Real-Mode MS-DOS Open Watcom v2 build outputs.

Checks shared across CI (.github/workflows/ci.yml) and Release builds
(.github/workflows/release.yml):
  - OPENSVR.EXE & OPENLNK.EXE: valid DOS MZ header, strict size budgets,
    zero clibs.lib linkage (100% Zero-CRT).
  - OPENLNK.SYS: zero clibs.lib linkage, exact disk == _sys_init_end match
    (discardable INIT_TEXT/INIT_DATA placed strictly after _sys_resident_end),
    disk <= 16.5 KiB, max resident RAM (_sys_resident_end, 8 units) <= 14.5 KiB
    (< 64 KiB single-segment limit), valid MS-DOS Block Device Driver header
    (FF FF FF FF, attr 0x2842, valid Strategy/Interrupt entrypoints, 'OPENLNK').
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

EXE_BUDGETS: tuple[tuple[str, str, int], ...] = (
    ("OPENSVR.EXE", "OPENSVR.MAP", 17152),
    ("OPENLNK.EXE", "OPENLNKX.MAP", 12288),
)

SYS_MAX_DISK_BYTES = 16896
SYS_MAX_RESIDENT_BYTES = 14848


def verify_dos_binaries(build_dir: Path) -> list[str]:
    """Verify all three 16-bit DOS targets in build_dir and return summary lines."""
    summary: list[str] = []

    for bin_name, map_name, max_disk in EXE_BUDGETS:
        bin_path = build_dir / bin_name
        map_path = build_dir / map_name
        if not bin_path.is_file():
            raise AssertionError(f"Missing binary: {bin_path}")
        if not map_path.is_file():
            raise AssertionError(f"Missing linker map: {map_path}")

        data = bin_path.read_bytes()
        sz = len(data)
        map_txt = map_path.read_text(encoding="utf-8", errors="replace")

        if data[:2] != b"MZ":
            raise AssertionError(f"Expected DOS MZ header in {bin_path}, got {data[:2]!r}")
        if sz > max_disk:
            raise AssertionError(f"{bin_path} ({sz} B) exceeds size budget ({max_disk} B)")
        if "clibs.lib" in map_txt.lower():
            raise AssertionError(f"Unexpected clibs.lib link in {map_path}")

        summary.append(f"Verified Zero-CRT DOS MZ: {bin_name} ({sz} B <= {max_disk} B)")

    sys_path = build_dir / "OPENLNK.SYS"
    sys_map_path = build_dir / "OPENLNK.MAP"
    if not sys_path.is_file():
        raise AssertionError(f"Missing binary: {sys_path}")
    if not sys_map_path.is_file():
        raise AssertionError(f"Missing linker map: {sys_map_path}")

    sys_data = sys_path.read_bytes()
    sys_map = sys_map_path.read_text(encoding="utf-8", errors="replace")

    if "clibs.lib" in sys_map.lower():
        raise AssertionError(f"Unexpected clibs.lib link in {sys_map_path}")

    bss_m = re.search(r"^([0-9a-fA-F]+)\*?\+?\s+_sys_bss_start$", sys_map, re.M)
    res_m = re.search(r"^([0-9a-fA-F]+)\*?\+?\s+_sys_resident_end$", sys_map, re.M)
    init_m = re.search(r"^([0-9a-fA-F]+)\*?\+?\s+_sys_init_end$", sys_map, re.M)
    if not (bss_m and res_m and init_m):
        raise AssertionError(
            "Missing _sys_bss_start, _sys_resident_end, or _sys_init_end in OPENLNK.MAP"
        )

    bss_start = int(bss_m.group(1), 16)
    res_end = int(res_m.group(1), 16)
    init_end = int(init_m.group(1), 16)

    if not (0 < bss_start < res_end < init_end):
        raise AssertionError(
            f"Invalid OPENLNK.SYS layout: bss_start=0x{bss_start:X}, "
            f"resident_end=0x{res_end:X}, init_end=0x{init_end:X}"
        )
    if len(sys_data) != init_end:
        raise AssertionError(
            f"OPENLNK.SYS disk ({len(sys_data)} B) != _sys_init_end ({init_end} B)"
        )
    if len(sys_data) > SYS_MAX_DISK_BYTES:
        raise AssertionError(
            f"OPENLNK.SYS disk ({len(sys_data)} B) exceeds {SYS_MAX_DISK_BYTES} B budget"
        )
    if res_end > SYS_MAX_RESIDENT_BYTES or res_end >= 65535:
        raise AssertionError(
            f"OPENLNK.SYS resident RAM ({res_end} B) exceeds {SYS_MAX_RESIDENT_BYTES} B budget"
        )

    nxt, attr, strat, intr, _units = struct.unpack_from("<IHHHB", sys_data, 0)
    sig = sys_data[11:18]
    if nxt != 0xFFFFFFFF:
        raise AssertionError(f"Expected FF FF FF FF header, got 0x{nxt:08X}")
    if attr != 0x2842:
        raise AssertionError(f"Expected attr 0x2842, got 0x{attr:04X}")
    if not (0x0012 <= strat < bss_start):
        raise AssertionError(f"Invalid Strategy offset 0x{strat:04X}")
    if not (0x0012 <= intr < bss_start):
        raise AssertionError(f"Invalid Interrupt offset 0x{intr:04X}")
    if sig != b"OPENLNK":
        raise AssertionError(f"Invalid driver signature {sig!r}")

    discarded = init_end - res_end
    summary.append(
        f"Verified OPENLNK.SYS: disk={len(sys_data)} B (== _sys_init_end), "
        f"resident_max={res_end} B ({res_end / 1024:.2f} KiB <= {SYS_MAX_RESIDENT_BYTES} B), "
        f"discarded_init={discarded} B, attr=0x{attr:04X}, "
        f"strat=0x{strat:04X}, intr=0x{intr:04X}, sig={sig!r}"
    )
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Verify 16-bit Real-Mode DOS Open Watcom binaries")
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("cleanroom/dos/build"),
        help="Directory containing OPENSVR.EXE, OPENLNK.EXE, OPENLNK.SYS, and *.MAP files",
    )
    args = parser.parse_args(argv)

    try:
        for line in verify_dos_binaries(args.build_dir):
            print(line)
    except AssertionError as exc:
        print(f"DOS BINARY VERIFICATION FAILED: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
