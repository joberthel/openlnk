#!/usr/bin/env python3
"""Create 86Box-compatible Fixed-Size MS-DOS 6.22 Client & Server VHDs from an ISO.

Builds:
  1. dos622_boot.img   - Extracted El Torito 1.44 MB boot floppy (for FDISK /MBR + SYS C:)
  2. dos622_client.vhd - Fixed-size 10 MB FAT16 VHD (CHS 306/4/17 for xta_wdxt150)
                         with 8088/NEC-V30-safe CONFIG.SYS boot menu & DOS utilities
  3. dos622_server.vhd - Fixed-size 10 MB FAT16 VHD (CHS 306/4/17) with DOS utilities,
                         AUTOEXEC.BAT, and deterministic C:\\TEST.DAT verification file

Optional --extras-dir (e.g. analysis/binaries) injects INTERLNK.EXE, INTERSVR.EXE,
and FC.EXE into C:\\DOS\\ of both VHDs when present.

Requires macOS native tools (bsdtar, hdiutil, newfs_msdos, fsck_msdos, mount).
"""

from __future__ import annotations

import argparse
import datetime
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path
from typing import Optional

# CHS geometry matching tools/runner/run_86box.sh (xta_wdxt150)
CYLINDERS = 306
HEADS = 4
SECTORS_PER_TRACK = 17
SECTOR_SIZE = 512
TOTAL_SECTORS = CYLINDERS * HEADS * SECTORS_PER_TRACK  # 20,808 sectors (~10.16 MiB)
PART_START_LBA = SECTORS_PER_TRACK  # Track 0, Head 1, Sector 1 (LBA 17)
PART_SECTORS = TOTAL_SECTORS - PART_START_LBA  # 20,791 sectors
DISK_BYTES = TOTAL_SECTORS * SECTOR_SIZE  # 10,653,696 bytes

VHD_EPOCH = datetime.datetime(2000, 1, 1, tzinfo=datetime.timezone.utc)

CLIENT_CONFIG_SYS = (
    "[menu]\r\n"
    "menuitem=B115200, INTERLNK COM1 @ 115200\r\n"
    "menuitem=B9600,   INTERLNK COM1 @ 9600\r\n"
    "menuitem=B115200V,INTERLNK COM1 @ 115200 (/V)\r\n"
    "menuitem=CLEAN,   Clean DOS (fuer /RCOPY Bootstrap)\r\n"
    "menudefault=B115200,3\r\n"
    "\r\n"
    "[common]\r\n"
    "FILES=20\r\n"
    "BUFFERS=10\r\n"
    "LASTDRIVE=Z\r\n"
    "\r\n"
    "[B115200]\r\n"
    "DEVICE=C:\\DOS\\INTERLNK.EXE /DRIVES:4 /COM1:115200\r\n"
    "\r\n"
    "[B9600]\r\n"
    "DEVICE=C:\\DOS\\INTERLNK.EXE /DRIVES:4 /COM1:9600\r\n"
    "\r\n"
    "[B115200V]\r\n"
    "DEVICE=C:\\DOS\\INTERLNK.EXE /DRIVES:4 /COM1:115200 /V\r\n"
    "\r\n"
    "[CLEAN]\r\n"
)

CLIENT_AUTOEXEC_BAT = (
    "@ECHO OFF\r\n"
    "PROMPT $P$G\r\n"
    "PATH C:\\DOS;C:\\\r\n"
    "ECHO OpenLNK MS-DOS 6.22 Client VM Ready.\r\n"
)

SERVER_CONFIG_SYS = (
    "FILES=20\r\n"
    "BUFFERS=10\r\n"
    "LASTDRIVE=Z\r\n"
)

SERVER_AUTOEXEC_BAT = (
    "@ECHO OFF\r\n"
    "PROMPT $P$G\r\n"
    "PATH C:\\DOS;C:\\\r\n"
    "ECHO OpenLNK MS-DOS 6.22 Server VM Ready.\r\n"
    "ECHO Run: INTERSVR C: /COM1 /B:115200  (or INTERSVR /RCOPY)\r\n"
)

# Files to skip from a generic CD boot image that are incompatible with 8088/NEC V30 or unnecessary
SKIP_ROOT_FILES = {"CONFIG.SYS", "AUTOEXEC.BAT", "C.BAT"}
ROOT_SYSTEM_FILES = {"IO.SYS", "MSDOS.SYS", "COMMAND.COM"}


def extract_eltorito_floppy(iso_path: Path, out_floppy: Path) -> Optional[Path]:
    """Extract embedded El Torito 1.44 MB / 2.88 MB boot floppy image if present."""
    data = iso_path.read_bytes()
    for sec in range(16, 32):
        off = sec * 2048
        if off + 2048 > len(data):
            break
        vd = data[off : off + 2048]
        if vd[0] == 0 and vd[1:6] == b"CD001":
            cat_lba = struct.unpack("<I", vd[71:75])[0]
            cat = data[cat_lba * 2048 : (cat_lba + 1) * 2048]
            media_type = cat[33]
            load_rba = struct.unpack("<I", cat[40:44])[0]
            sizes = {1: 1228800, 2: 1474560, 3: 2949120}
            if media_type in sizes:
                start = load_rba * 2048
                floppy = data[start : start + sizes[media_type]]
                if len(floppy) == sizes[media_type] and floppy[510:512] == b"\x55\xaa":
                    out_floppy.parent.mkdir(parents=True, exist_ok=True)
                    out_floppy.write_bytes(floppy)
                    return out_floppy
        elif vd[0] == 255:
            break
    return None


def build_mbr_sector() -> bytes:
    """Build a 512-byte MBR sector with 1 active FAT16 (<32MB, type 0x04) partition."""
    mbr = bytearray(512)
    # Partition 1 entry at offset 0x1BE (16 bytes)
    start_head = 1
    start_sec = 1
    start_cyl = 0
    end_head = HEADS - 1  # 3
    end_cyl = CYLINDERS - 1  # 305
    end_sec = SECTORS_PER_TRACK  # 17

    start_cyl_sec = ((start_cyl >> 8) & 0x03) << 6 | (start_sec & 0x3F)
    end_cyl_sec = ((end_cyl >> 8) & 0x03) << 6 | (end_sec & 0x3F)

    entry = struct.pack(
        "<BBBBBBBBII",
        0x80,  # Bootable / active
        start_head,
        start_cyl_sec,
        start_cyl & 0xFF,
        0x04,  # FAT16 < 32 MB
        end_head,
        end_cyl_sec,
        end_cyl & 0xFF,
        PART_START_LBA,
        PART_SECTORS,
    )
    mbr[0x1BE : 0x1CE] = entry
    mbr[0x1FE : 0x200] = b"\x55\xaa"
    return bytes(mbr)


def build_vhd_fixed_footer(disk_bytes: int, cyl: int, heads: int, spt: int) -> bytes:
    """Build a 512-byte Microsoft Fixed-Size VHD footer with valid checksum."""
    now = datetime.datetime.now(datetime.timezone.utc)
    ts = int((now - VHD_EPOCH).total_seconds())
    geometry = ((cyl & 0xFFFF) << 16) | ((heads & 0xFF) << 8) | (spt & 0xFF)
    disk_uuid = uuid.uuid4().bytes

    footer = bytearray(512)
    struct.pack_into(
        ">8sIIQI4sI4sQQII",
        footer,
        0,
        b"conectix",          # Cookie
        0x00000002,           # Features (Reserved)
        0x00010000,           # File Format Version 1.0
        0xFFFFFFFFFFFFFFFF,   # Data Offset (Fixed VHD)
        ts,                   # Timestamp
        b"olnk",              # Creator Application
        0x00010000,           # Creator Version
        b"Wi2k",              # Creator Host OS
        disk_bytes,           # Original Size
        disk_bytes,           # Current Size
        geometry,             # CHS Disk Geometry
        2,                    # Disk Type: Fixed hard disk
    )
    footer[0x44:0x54] = disk_uuid
    footer[0x54] = 0  # Saved state

    # VHD footer checksum: one's complement of sum of all bytes (checksum field = 0)
    checksum = (~sum(footer)) & 0xFFFFFFFF
    struct.pack_into(">I", footer, 0x40, checksum)
    return bytes(footer)


def _run(cmd: list[str], env: Optional[dict[str, str]] = None) -> str:
    res = subprocess.run(cmd, check=True, capture_output=True, text=True, env=env)
    return res.stdout


def create_single_vhd(
    out_vhd: Path,
    volume_label: str,
    extracted_iso_dir: Path,
    extras_dir: Optional[Path],
    config_sys: str,
    autoexec_bat: str,
    include_test_dat: bool,
) -> dict[str, list[str]]:
    """Create a raw MBR+FAT16 image via macOS hdiutil/newfs_msdos and append VHD footer."""
    if sys.platform != "darwin":
        raise RuntimeError("This helper uses macOS hdiutil/newfs_msdos.")

    out_vhd.parent.mkdir(parents=True, exist_ok=True)
    copied_dos: list[str] = []
    injected_extras: list[str] = []

    with tempfile.TemporaryDirectory(prefix="openlnk_vhd_") as tmp_str:
        tmp = Path(tmp_str)
        raw_img = tmp / "disk.raw"
        mnt_dir = tmp / "mnt"
        mnt_dir.mkdir()

        # 1. Create zeroed raw disk + write MBR partition table
        with raw_img.open("wb") as fp:
            fp.write(build_mbr_sector())
            fp.truncate(DISK_BYTES)

        # 2. Attach raw image without mounting
        attach_out = _run(
            [
                "hdiutil",
                "attach",
                "-imagekey",
                "diskimage-class=CRawDiskImage",
                "-nomount",
                str(raw_img),
            ]
        )
        whole_dev = ""
        part_dev = ""
        for line in attach_out.splitlines():
            parts = line.split()
            if not parts:
                continue
            dev = parts[0]
            if not whole_dev:
                whole_dev = dev
            if dev.startswith("/dev/disk") and "s1" in dev:
                part_dev = dev

        if not whole_dev or not part_dev:
            raise RuntimeError(f"Failed to parse hdiutil attach output:\n{attach_out}")

        mounted = False
        try:
            # 3. Format partition 1 as FAT16 matching CHS 306/4/17 and HiddenSectors=17
            _run(
                [
                    "/sbin/newfs_msdos",
                    "-F",
                    "16",
                    "-S",
                    "512",
                    "-u",
                    str(SECTORS_PER_TRACK),
                    "-h",
                    str(HEADS),
                    "-o",
                    str(PART_START_LBA),
                    "-v",
                    volume_label,
                    part_dev,
                ]
            )

            # 4. Mount with nobrowse to avoid Spotlight metadata clutter
            _run(["mount", "-t", "msdos", "-o", "nobrowse", part_dev, str(mnt_dir)])
            mounted = True

            dos_dir = mnt_dir / "DOS"
            dos_dir.mkdir(exist_ok=True)

            # Copy IO.SYS, MSDOS.SYS, COMMAND.COM first to root, everything else to C:\DOS
            iso_files = {p.name.upper(): p for p in extracted_iso_dir.iterdir() if p.is_file()}
            for sys_name in ("IO.SYS", "MSDOS.SYS", "COMMAND.COM"):
                if sys_name in iso_files:
                    shutil.copyfile(iso_files[sys_name], mnt_dir / sys_name)

            for upper_name, src_path in sorted(iso_files.items()):
                if upper_name in SKIP_ROOT_FILES or upper_name in ROOT_SYSTEM_FILES:
                    continue
                shutil.copyfile(src_path, dos_dir / upper_name)
                copied_dos.append(upper_name)

            # Inject optional extra MS-DOS binaries (INTERLNK.EXE, INTERSVR.EXE, FC.EXE)
            if extras_dir and extras_dir.is_dir():
                for candidate in ("INTERLNK.EXE", "INTERSVR.EXE", "FC.EXE"):
                    for p in extras_dir.iterdir():
                        if p.is_file() and p.name.upper() == candidate:
                            shutil.copyfile(p, dos_dir / candidate)
                            injected_extras.append(candidate)
                            break

            # Write DOS CRLF CONFIG.SYS and AUTOEXEC.BAT
            (mnt_dir / "CONFIG.SYS").write_bytes(config_sys.encode("ascii"))
            (mnt_dir / "AUTOEXEC.BAT").write_bytes(autoexec_bat.encode("ascii"))

            if include_test_dat:
                # 16 KiB deterministic test file for COPY / FC /B Oracle verification
                pattern = bytes(((i * 37 + 11) & 0xFF) for i in range(16384))
                (mnt_dir / "TEST.DAT").write_bytes(pattern)

            # Remove any macOS dotfiles if created
            for junk in (".fseventsd", ".Spotlight-V100", ".Trashes"):
                jp = mnt_dir / junk
                if jp.is_dir():
                    shutil.rmtree(jp, ignore_errors=True)
                elif jp.is_file():
                    jp.unlink(missing_ok=True)

            _run(["umount", str(mnt_dir)])
            mounted = False

            # 5. Verify FAT16 filesystem integrity with fsck_msdos
            _run(["/sbin/fsck_msdos", "-n", part_dev])
        finally:
            if mounted:
                subprocess.run(["umount", str(mnt_dir)], check=False)
            subprocess.run(["hdiutil", "detach", whole_dev], check=False, capture_output=True)

        # 6. Write Fixed-Size VHD = raw disk + 512-byte Microsoft VHD footer
        footer = build_vhd_fixed_footer(DISK_BYTES, CYLINDERS, HEADS, SECTORS_PER_TRACK)
        with raw_img.open("rb") as src, out_vhd.open("wb") as dst:
            shutil.copyfileobj(src, dst)
            dst.write(footer)

    return {"dos_files": copied_dos, "extras": injected_extras}


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Build 86Box MS-DOS 6.22 Client & Server Fixed-Size VHDs from ISO."
    )
    parser.add_argument("--iso", type=Path, required=True, help="Path to MS-DOS 6.22 ISO")
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("analysis/binaries/vhds"),
        help="Output directory for VHDs and extracted boot floppy",
    )
    parser.add_argument(
        "--extras-dir",
        type=Path,
        default=Path("analysis/binaries"),
        help="Optional directory containing INTERLNK.EXE / INTERSVR.EXE / FC.EXE to inject into C:\\DOS",
    )
    args = parser.parse_args(argv)

    if not args.iso.is_file():
        print(f"[create_dos_vhds] ERROR: ISO not found: {args.iso}", file=sys.stderr)
        return 1

    args.out_dir.mkdir(parents=True, exist_ok=True)

    # 1. Extract El Torito boot floppy if present
    boot_floppy = extract_eltorito_floppy(args.iso, args.out_dir / "dos622_boot.img")
    if boot_floppy:
        print(f"[create_dos_vhds] Extracted El Torito boot floppy: {boot_floppy} ({boot_floppy.stat().st_size} bytes)")
    else:
        print("[create_dos_vhds] Note: No El Torito floppy image found in ISO.")

    # 2. Extract ISO files into temporary staging directory
    with tempfile.TemporaryDirectory(prefix="openlnk_iso_") as iso_tmp_str:
        iso_tmp = Path(iso_tmp_str)
        _run(["bsdtar", "-xf", str(args.iso), "-C", str(iso_tmp)])

        client_vhd = args.out_dir / "dos622_client.vhd"
        server_vhd = args.out_dir / "dos622_server.vhd"

        c_info = create_single_vhd(
            out_vhd=client_vhd,
            volume_label="DOS622_CLI",
            extracted_iso_dir=iso_tmp,
            extras_dir=args.extras_dir,
            config_sys=CLIENT_CONFIG_SYS,
            autoexec_bat=CLIENT_AUTOEXEC_BAT,
            include_test_dat=False,
        )
        print(
            f"[create_dos_vhds] Created {client_vhd} ({client_vhd.stat().st_size} bytes, "
            f"DOS utils={len(c_info['dos_files'])}, extras={c_info['extras']})"
        )

        s_info = create_single_vhd(
            out_vhd=server_vhd,
            volume_label="DOS622_SRV",
            extracted_iso_dir=iso_tmp,
            extras_dir=args.extras_dir,
            config_sys=SERVER_CONFIG_SYS,
            autoexec_bat=SERVER_AUTOEXEC_BAT,
            include_test_dat=True,
        )
        print(
            f"[create_dos_vhds] Created {server_vhd} ({server_vhd.stat().st_size} bytes, "
            f"DOS utils={len(s_info['dos_files'])}, extras={s_info['extras']})"
        )

    missing_inter = [x for x in ("INTERLNK.EXE", "INTERSVR.EXE") if x not in c_info["extras"]]
    if missing_inter:
        print(
            f"[create_dos_vhds] NOTICE: {', '.join(missing_inter)} were not in ISO or {args.extras_dir}. "
            "Place them in analysis/binaries/ and re-run this script to inject them into C:\\DOS\\."
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
