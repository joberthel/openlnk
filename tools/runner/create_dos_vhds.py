#!/usr/bin/env python3
"""Create 86Box-compatible Fixed-Size MS-DOS 6.22 Client & Server VHDs.

Builds:
  - dos622_boot.img   - Extracted El Torito 1.44 MB boot floppy (direct A:\\> for FDISK /MBR + SYS C:)
  - dos622_client.vhd - 10 MB Fixed-Size FAT16 VHD (CHS 306/4/17 for xta_wdxt150)
                        with 8088/NEC-V30-safe CONFIG.SYS boot menu & C:\\DOS\\ utilities
  - dos622_server.vhd - 10 MB Fixed-Size FAT16 VHD (CHS 306/4/17) with C:\\DOS\\ utilities,
                        AUTOEXEC.BAT, and deterministic 16 KiB C:\\TEST.DAT

Supports combining:
  --iso "MS-DOS 6.22.iso"                  (uncompressed DOS utilities + El Torito boot floppy)
  --disks disk1.img disk2.img disk3.img    (extracts uncompressed tools like EXPAND.EXE and
                                            stages KWAJ-compressed FC.EX_/MODE.CO_ into C:\\SETUP\\
                                            with C:\\FINSETUP.BAT for native DOS EXPAND.EXE)
  --extras-dir analysis/binaries           (injects unpacked INTERLNK.EXE, INTERSVR.EXE, etc.)

Requires macOS native tools (hdiutil, newfs_msdos, fsck_msdos, mount, bsdtar).
"""

from __future__ import annotations

import argparse
import datetime
import shutil
import struct
import subprocess
import sys
import tempfile
import time
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
SZDD_MAGIC = b"SZDD\x88\xf0\x27\x33"
KWAJ_MAGIC = b"KWAJ\x88\xf0\x27\xd1"

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

SKIP_FILES = {
    "CONFIG.SYS",
    "AUTOEXEC.BAT",
    "C.BAT",
    "SETUP.EXE",
    "SETUP.MSG",
    "DOSSETUP.INI",
    "PACKING.LST",
    "BUSETUP.EXE",
    "AUTOEXEC.UMB",
}
ROOT_SYSTEM_FILES = {"IO.SYS", "MSDOS.SYS", "COMMAND.COM"}
KWAJ_STAGE_TARGETS = {
    "FC.EX_": "FC.EXE",
    "MODE.CO_": "MODE.COM",
    "INTERLNK.EX_": "INTERLNK.EXE",
    "INTERSVR.EX_": "INTERSVR.EXE",
}


def decompress_szdd(data: bytes, orig_name: str) -> tuple[str, bytes]:
    """Decompress Microsoft SZDD files; return unchanged if not SZDD."""
    if len(data) < 14 or data[:8] != SZDD_MAGIC or data[8] != 0x41:
        return orig_name, data

    missing_char = data[9]
    expected_size = struct.unpack("<I", data[10:14])[0]

    out_name = orig_name
    if orig_name.endswith("_") and 32 <= missing_char <= 126:
        out_name = orig_name[:-1] + chr(missing_char).upper()

    window = bytearray(b" " * 4096)
    pos = 4096 - 16
    out = bytearray()
    idx = 14
    n = len(data)

    while idx < n and len(out) < expected_size:
        flags = data[idx]
        idx += 1
        for bit in range(8):
            if idx >= n or len(out) >= expected_size:
                break
            if (flags >> bit) & 1:
                b = data[idx]
                idx += 1
                out.append(b)
                window[pos] = b
                pos = (pos + 1) & 0xFFF
            else:
                if idx + 1 >= n:
                    break
                b0 = data[idx]
                b1 = data[idx + 1]
                idx += 2
                offset = b0 | ((b1 & 0xF0) << 4)
                length = (b1 & 0x0F) + 3
                for k in range(length):
                    if len(out) >= expected_size:
                        break
                    c = window[(offset + k) & 0xFFF]
                    out.append(c)
                    window[pos] = c
                    pos = (pos + 1) & 0xFFF

    if len(out) != expected_size:
        raise ValueError(
            f"SZDD decompression size mismatch for {orig_name}: got {len(out)}, expected {expected_size}"
        )
    return out_name, bytes(out)


def extract_fat12_floppy(
    floppy_path: Path,
    dos_stage_dir: Path,
    kwaj_stage_dir: Path,
) -> tuple[list[str], list[str]]:
    """Extract uncompressed/SZDD files to dos_stage_dir and KWAJ targets to kwaj_stage_dir."""
    raw = floppy_path.read_bytes()
    bps, spc, res, fats, root_cnt, _tot16, _media, spf = struct.unpack("<HBHBHHBH", raw[11:24])
    fat_off = res * bps
    root_off = (res + fats * spf) * bps
    root_sectors = ((root_cnt * 32) + (bps - 1)) // bps
    data_off = root_off + root_sectors * bps
    cluster_size = spc * bps

    def next_cluster(c: int) -> int:
        byte_off = fat_off + (c * 3) // 2
        val = struct.unpack("<H", raw[byte_off : byte_off + 2])[0]
        return (val >> 4) if (c & 1) else (val & 0x0FFF)

    unpacked: list[str] = []
    kwaj_staged: list[str] = []

    for i in range(root_cnt):
        ent = raw[root_off + i * 32 : root_off + (i + 1) * 32]
        if ent[0] == 0x00:
            break
        if ent[0] == 0xE5:
            continue
        attr = ent[11]
        if attr & 0x08 or attr & 0x10 or attr == 0x0F:
            continue
        name = ent[0:8].decode("ascii", errors="replace").strip().upper()
        ext = ent[8:11].decode("ascii", errors="replace").strip().upper()
        full_name = f"{name}.{ext}" if ext else name
        cluster = struct.unpack("<H", ent[26:28])[0]
        size = struct.unpack("<I", ent[28:32])[0]

        buf = bytearray()
        cur = cluster
        while 2 <= cur < 0xFF8 and len(buf) < size:
            c_off = data_off + (cur - 2) * cluster_size
            buf.extend(raw[c_off : c_off + cluster_size])
            cur = next_cluster(cur)
        file_bytes = bytes(buf[:size])

        if file_bytes.startswith(KWAJ_MAGIC):
            if full_name in KWAJ_STAGE_TARGETS:
                target = kwaj_stage_dir / full_name
                if target.exists():
                    target.chmod(0o644)
                target.write_bytes(file_bytes)
                kwaj_staged.append(full_name)
            continue

        out_name, expanded = decompress_szdd(file_bytes, full_name)
        target = dos_stage_dir / out_name
        if target.exists():
            target.chmod(0o644)
        target.write_bytes(expanded)
        unpacked.append(out_name)

    return unpacked, kwaj_staged


BOOT_FLOPPY_CONFIG_SYS = (
    "FILES=20\r\n"
    "BUFFERS=10\r\n"
    "LASTDRIVE=Z\r\n"
)

BOOT_FLOPPY_AUTOEXEC_BAT = (
    "@ECHO OFF\r\n"
    "PROMPT $P$G\r\n"
    "ECHO OpenLNK MS-DOS 6.22 XT Setup Floppy (no HIMEM/CDROM).\r\n"
    "ECHO Run: FDISK /MBR  then  SYS C:  then  C:\\FINSETUP.BAT\r\n"
)


def sanitize_boot_floppy_for_xt(floppy_path: Path) -> None:
    """Replace 386+/CD-ROM CONFIG.SYS (HIMEM/CD1.SYS BANANA) with XT-safe files."""
    if sys.platform != "darwin":
        return
    with tempfile.TemporaryDirectory(prefix="openlnk_fdd_") as tmp_str:
        mnt_dir = Path(tmp_str) / "mnt"
        mnt_dir.mkdir()
        attach_out = _run(
            [
                "hdiutil",
                "attach",
                "-imagekey",
                "diskimage-class=CRawDiskImage",
                "-nomount",
                str(floppy_path),
            ]
        )
        dev = attach_out.splitlines()[0].split()[0]
        mounted = False
        try:
            _run(["mount", "-t", "msdos", "-o", "nobrowse", dev, str(mnt_dir)])
            mounted = True
            (mnt_dir / "CONFIG.SYS").write_bytes(BOOT_FLOPPY_CONFIG_SYS.encode("ascii"))
            (mnt_dir / "AUTOEXEC.BAT").write_bytes(BOOT_FLOPPY_AUTOEXEC_BAT.encode("ascii"))
            for junk in (".fseventsd", ".Spotlight-V100", ".Trashes"):
                jp = mnt_dir / junk
                if jp.is_dir():
                    shutil.rmtree(jp, ignore_errors=True)
                elif jp.is_file():
                    jp.unlink(missing_ok=True)
            _unmount_with_retry(mnt_dir)
            mounted = False
            _run(["/sbin/fsck_msdos", "-n", dev])
        finally:
            if mounted:
                subprocess.run(["/usr/sbin/diskutil", "unmount", "force", str(mnt_dir)], check=False, capture_output=True)
            subprocess.run(["hdiutil", "detach", "-force", dev], check=False, capture_output=True)


def extract_eltorito_floppy(iso_path: Path, out_floppy: Path) -> Optional[Path]:
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
                    sanitize_boot_floppy_for_xt(out_floppy)
                    return out_floppy
        elif vd[0] == 255:
            break
    return None


def build_mbr_sector() -> bytes:
    mbr = bytearray(512)
    start_head = 1
    start_sec = 1
    start_cyl = 0
    end_head = HEADS - 1
    end_cyl = CYLINDERS - 1
    end_sec = SECTORS_PER_TRACK

    start_cyl_sec = ((start_cyl >> 8) & 0x03) << 6 | (start_sec & 0x3F)
    end_cyl_sec = ((end_cyl >> 8) & 0x03) << 6 | (end_sec & 0x3F)

    entry = struct.pack(
        "<BBBBBBBBII",
        0x80,
        start_head,
        start_cyl_sec,
        start_cyl & 0xFF,
        0x04,
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
    now = datetime.datetime.now(datetime.timezone.utc)
    ts = int((now - VHD_EPOCH).total_seconds())
    geometry = ((cyl & 0xFFFF) << 16) | ((heads & 0xFF) << 8) | (spt & 0xFF)
    disk_uuid = uuid.uuid4().bytes

    footer = bytearray(512)
    struct.pack_into(
        ">8sIIQI4sI4sQQII",
        footer,
        0,
        b"conectix",
        0x00000002,
        0x00010000,
        0xFFFFFFFFFFFFFFFF,
        ts,
        b"olnk",
        0x00010000,
        b"Wi2k",
        disk_bytes,
        disk_bytes,
        geometry,
        2,
    )
    footer[0x44:0x54] = disk_uuid
    footer[0x54] = 0
    checksum = (~sum(footer)) & 0xFFFFFFFF
    struct.pack_into(">I", footer, 0x40, checksum)
    return bytes(footer)


def _run(cmd: list[str]) -> str:
    res = subprocess.run(cmd, check=True, capture_output=True, text=True)
    return res.stdout


def _unmount_with_retry(mnt_dir: Path, retries: int = 5) -> None:
    subprocess.run(["sync"], check=False)
    for attempt in range(retries):
        res = subprocess.run(["/usr/sbin/diskutil", "unmount", str(mnt_dir)], capture_output=True, text=True)
        if res.returncode == 0:
            return
        time.sleep(0.4 * (attempt + 1))
    _run(["/usr/sbin/diskutil", "unmount", "force", str(mnt_dir)])


def create_single_vhd(
    out_vhd: Path,
    volume_label: str,
    staged_dos_dir: Path,
    kwaj_stage_dir: Path,
    extras_dir: Optional[Path],
    config_sys: str,
    autoexec_bat: str,
    include_test_dat: bool,
) -> dict[str, list[str]]:
    if sys.platform != "darwin":
        raise RuntimeError("This helper uses macOS hdiutil/newfs_msdos.")

    out_vhd.parent.mkdir(parents=True, exist_ok=True)
    copied_dos: list[str] = []
    injected_extras: list[str] = []
    kwaj_pending: list[str] = []

    with tempfile.TemporaryDirectory(prefix="openlnk_vhd_") as tmp_str:
        tmp = Path(tmp_str)
        raw_img = tmp / "disk.raw"
        mnt_dir = tmp / "mnt"
        mnt_dir.mkdir()

        with raw_img.open("wb") as fp:
            fp.write(build_mbr_sector())
            fp.truncate(DISK_BYTES)

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

            _run(["mount", "-t", "msdos", "-o", "nobrowse", part_dev, str(mnt_dir)])
            mounted = True

            dos_dir = mnt_dir / "DOS"
            dos_dir.mkdir(exist_ok=True)

            staged_files = {p.name.upper(): p for p in staged_dos_dir.iterdir() if p.is_file()}
            for sys_name in ("IO.SYS", "MSDOS.SYS", "COMMAND.COM"):
                if sys_name in staged_files:
                    shutil.copyfile(staged_files[sys_name], mnt_dir / sys_name)

            for upper_name, src_path in sorted(staged_files.items()):
                if upper_name in SKIP_FILES or upper_name in ROOT_SYSTEM_FILES:
                    continue
                shutil.copyfile(src_path, dos_dir / upper_name)
                copied_dos.append(upper_name)

            if extras_dir and extras_dir.is_dir():
                for candidate in ("INTERLNK.EXE", "INTERSVR.EXE", "FC.EXE", "MODE.COM"):
                    if candidate in copied_dos:
                        continue
                    for p in extras_dir.iterdir():
                        if p.is_file() and p.name.upper() == candidate:
                            shutil.copyfile(p, dos_dir / candidate)
                            injected_extras.append(candidate)
                            copied_dos.append(candidate)
                            break

            # Stage any remaining needed KWAJ archives (e.g. FC.EX_, MODE.CO_) into C:\SETUP\
            kwaj_files = {p.name.upper(): p for p in kwaj_stage_dir.iterdir() if p.is_file()}
            fin_lines = ["@ECHO OFF\r\n"]
            setup_dir = mnt_dir / "SETUP"
            for src_name, dst_name in sorted(KWAJ_STAGE_TARGETS.items()):
                if dst_name in copied_dos:
                    continue
                if src_name in kwaj_files:
                    setup_dir.mkdir(exist_ok=True)
                    shutil.copyfile(kwaj_files[src_name], setup_dir / src_name)
                    fin_lines.append(f"C:\\DOS\\EXPAND.EXE C:\\SETUP\\{src_name} C:\\DOS\\{dst_name}\r\n")
                    kwaj_pending.append(f"{src_name}->{dst_name}")

            if kwaj_pending:
                fin_lines.append("ECHO Expanded optional MS-DOS 6.22 utilities into C:\\DOS\\\r\n")
                (mnt_dir / "FINSETUP.BAT").write_bytes("".join(fin_lines).encode("ascii"))

            (mnt_dir / "CONFIG.SYS").write_bytes(config_sys.encode("ascii"))
            (mnt_dir / "AUTOEXEC.BAT").write_bytes(autoexec_bat.encode("ascii"))

            if include_test_dat:
                pattern = bytes(((i * 37 + 11) & 0xFF) for i in range(16384))
                (mnt_dir / "TEST.DAT").write_bytes(pattern)

            for junk in (".fseventsd", ".Spotlight-V100", ".Trashes"):
                jp = mnt_dir / junk
                if jp.is_dir():
                    shutil.rmtree(jp, ignore_errors=True)
                elif jp.is_file():
                    jp.unlink(missing_ok=True)

            _unmount_with_retry(mnt_dir)
            mounted = False

            _run(["/sbin/fsck_msdos", "-n", part_dev])
        finally:
            if mounted:
                subprocess.run(["/usr/sbin/diskutil", "unmount", "force", str(mnt_dir)], check=False, capture_output=True)
            subprocess.run(["hdiutil", "detach", "-force", whole_dev], check=False, capture_output=True)

        footer = build_vhd_fixed_footer(DISK_BYTES, CYLINDERS, HEADS, SECTORS_PER_TRACK)
        with raw_img.open("rb") as src, out_vhd.open("wb") as dst:
            shutil.copyfileobj(src, dst)
            dst.write(footer)

    return {"dos_files": copied_dos, "extras": injected_extras, "kwaj_pending": kwaj_pending}


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Build 86Box MS-DOS 6.22 Client & Server Fixed-Size VHDs from floppy disks and/or ISO."
    )
    parser.add_argument("--iso", type=Path, help="Optional MS-DOS 6.22 ISO (boot floppy + uncompressed tools)")
    parser.add_argument(
        "--disks",
        type=Path,
        nargs="+",
        help="MS-DOS 6.22 1.44 MB setup floppy images (e.g. dos-622-disk1.img disk2.img disk3.img)",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("analysis/binaries/vhds"),
        help="Output directory for VHDs and boot floppy",
    )
    parser.add_argument(
        "--extras-dir",
        type=Path,
        default=Path("analysis/binaries"),
        help="Directory with unpacked INTERLNK.EXE / INTERSVR.EXE / FC.EXE / MODE.COM",
    )
    args = parser.parse_args(argv)

    if not args.iso and not args.disks:
        parser.error("Provide --disks and/or --iso.")

    args.out_dir.mkdir(parents=True, exist_ok=True)

    if args.iso:
        if not args.iso.is_file():
            print(f"[create_dos_vhds] ERROR: ISO not found: {args.iso}", file=sys.stderr)
            return 1
        boot_floppy = extract_eltorito_floppy(args.iso, args.out_dir / "dos622_boot.img")
        if boot_floppy:
            print(f"[create_dos_vhds] Extracted El Torito boot floppy: {boot_floppy}")

    with tempfile.TemporaryDirectory(prefix="openlnk_stage_") as stage_str:
        stage_root = Path(stage_str)
        dos_stage = stage_root / "dos"
        kwaj_stage = stage_root / "kwaj"
        dos_stage.mkdir()
        kwaj_stage.mkdir()

        if args.iso:
            _run(["bsdtar", "-xf", str(args.iso), "-C", str(dos_stage)])
        if args.disks:
            for dpath in args.disks:
                if not dpath.is_file():
                    print(f"[create_dos_vhds] ERROR: Disk image not found: {dpath}", file=sys.stderr)
                    return 1
                unpacked, kwaj = extract_fat12_floppy(dpath, dos_stage, kwaj_stage)
                print(
                    f"[create_dos_vhds] {dpath.name}: unpacked={len(unpacked)}, "
                    f"kwaj_staged={kwaj}"
                )

        client_vhd = args.out_dir / "dos622_client.vhd"
        server_vhd = args.out_dir / "dos622_server.vhd"

        c_info = create_single_vhd(
            out_vhd=client_vhd,
            volume_label="DOS622_CLI",
            staged_dos_dir=dos_stage,
            kwaj_stage_dir=kwaj_stage,
            extras_dir=args.extras_dir,
            config_sys=CLIENT_CONFIG_SYS,
            autoexec_bat=CLIENT_AUTOEXEC_BAT,
            include_test_dat=False,
        )
        print(
            f"[create_dos_vhds] Created {client_vhd} ({client_vhd.stat().st_size} bytes, "
            f"C:\\DOS={len(c_info['dos_files'])}, extras={c_info['extras']}, "
            f"C:\\FINSETUP.BAT={c_info['kwaj_pending']})"
        )

        s_info = create_single_vhd(
            out_vhd=server_vhd,
            volume_label="DOS622_SRV",
            staged_dos_dir=dos_stage,
            kwaj_stage_dir=kwaj_stage,
            extras_dir=args.extras_dir,
            config_sys=SERVER_CONFIG_SYS,
            autoexec_bat=SERVER_AUTOEXEC_BAT,
            include_test_dat=True,
        )
        print(
            f"[create_dos_vhds] Created {server_vhd} ({server_vhd.stat().st_size} bytes, "
            f"C:\\DOS={len(s_info['dos_files'])}, extras={s_info['extras']}, "
            f"C:\\FINSETUP.BAT={s_info['kwaj_pending']})"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
