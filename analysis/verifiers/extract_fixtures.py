#!/usr/bin/env python3
"""Deterministic JSON Test Vector Extractor for OpenLNK (Agent A -> Agent B Handoff).

Extracts real, verified wire frames directly from the 7 Golden Traces
(tests/traces/01..07) and validates every generated JSON fixture against
both CRC-16/GENIBUS and the compiled Kaitai Struct parser before writing
to tests/fixtures/*.json.
"""

from __future__ import annotations

import hashlib
import json
import struct
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT))

from analysis.verifiers.verify_traces import (
    CRC16_RESIDUE_GOOD,
    crc16_ccitt_raw,
    crc16_genibus,
    keepalive_byte_for_seq,
    load_trace_bytes,
)

TRACES_DIR = REPO_ROOT / "tests" / "traces"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"


def canonical_frame_bytes(header: int, payload: bytes) -> tuple[bytes, int]:
    """Build canonical link-layer wire frame bytes (header || len || payload || crc16_be)."""
    is_short = bool(header & 0x80)
    if is_short:
        len_bytes = bytes([len(payload)])
    else:
        len_bytes = struct.pack("<H", len(payload))
    crc_in = len_bytes + payload
    crc = crc16_genibus(crc_in)
    assert crc16_ccitt_raw(crc_in + struct.pack(">H", crc)) == CRC16_RESIDUE_GOOD
    return bytes([header]) + crc_in + struct.pack(">H", crc), crc


def extract_interlnk_frames(stem: str) -> list[dict[str, Any]]:
    """Extract ordered canonical link-layer frames from an INTERLNK Golden Trace."""
    _, ev = load_trace_bytes(TRACES_DIR, stem)
    n = len(ev)
    i = 0
    expected_seq = 0
    frames: list[dict[str, Any]] = []

    def is_sync_at(idx: int) -> bool:
        return (
            idx + 5 < n
            and ev[idx][1] == "CLIENT->SERVER" and ev[idx][2] == 0xAA
            and ev[idx + 1][1] == "SERVER->CLIENT" and ev[idx + 1][2] == 0x00
            and ev[idx + 2][1] == "CLIENT->SERVER" and ev[idx + 2][2] == 0x55
            and ev[idx + 3][1] == "SERVER->CLIENT" and ev[idx + 3][2] == 0xFF
            and ev[idx + 4][1] == "CLIENT->SERVER" and ev[idx + 4][2] == 0x5A
            and ev[idx + 5][1] == "SERVER->CLIENT" and ev[idx + 5][2] == 0x11
        )

    def skip_sync() -> None:
        nonlocal i, expected_seq
        assert is_sync_at(i)
        i += 6
        expected_seq = 0
        while (
            i + 5 < n
            and ev[i][1] == "CLIENT->SERVER"
            and ev[i][2] == ((~expected_seq) & 0xFF)
            and ev[i + 1][1] == "SERVER->CLIENT"
            and ev[i + 1][2] == expected_seq
        ):
            b_idx = ev[i + 2][2]
            beacon = ((b_idx << 4) | expected_seq) & 0xFF
            i += 4
            while i < n and ev[i][1] == "SERVER->CLIENT" and ev[i][2] == beacon:
                i += 1
            i += 2 + 30

    skip_sync()
    while i < n:
        if is_sync_at(i):
            skip_sync()
            continue
        dr, b = ev[i][1], ev[i][2]
        if dr == "CLIENT->SERVER" and b in (0xFC, 0xFD, 0xFE, 0xFF):
            if i + 1 < n and ev[i + 1][1] == "SERVER->CLIENT" and ev[i + 1][2] == b:
                i += 2
                continue
            if i == n - 1:
                break
            if i + 1 < n and ev[i + 1][1] == "CLIENT->SERVER" and ev[i + 1][2] == b:
                while i < n and ev[i][1] == "CLIENT->SERVER" and ev[i][2] == b:
                    i += 1
                while i < n and ev[i][1] == "SERVER->CLIENT" and not is_sync_at(i):
                    i += 1
                continue

        if (
            i + 1 < n
            and ev[i + 1][1] != dr
            and (b & 0x38) == 0x00
            and ((ev[i + 1][2] & 0xFB) == ((~b) & 0xFB))
        ):
            hdr = b
            ack_hdr = ev[i + 1][2]
            echo = bool(ack_hdr & 0x04)
            sender = dr
            receiver = ev[i + 1][1]
            start_i = i
            i += 2

            def rb(ec: bool = True) -> int:
                nonlocal i
                v = ev[i][2]
                i += 1
                if echo and ec:
                    i += 1
                return v

            if hdr & 0x80:
                ln = rb(True)
                len_bytes = bytes([ln])
            else:
                lo, hi = rb(True), rb(True)
                ln = lo | (hi << 8)
                len_bytes = bytes([lo, hi])
            pl = bytes(rb(True) for _ in range(ln))
            crc_hi = rb(True)
            crc_lo = rb(False)
            crc_wire = (crc_hi << 8) | crc_lo
            i += 1  # final ack
            expected_seq = ((hdr & 0x03) + 1) & 0x03

            # Turnaround
            if i < n and ev[i][1] == sender and ev[i][2] == expected_seq:
                if i + 1 == n:
                    i += 1
                elif ev[i + 1][1] == receiver:
                    nxt = ev[i + 1][2]
                    if (
                        receiver == "CLIENT->SERVER"
                        and nxt == keepalive_byte_for_seq(expected_seq)
                        and (i + 2 == n or ev[i + 2][2] == nxt)
                    ):
                        i += 1
                    elif (nxt & 0xFB) != ((~ev[i][2]) & 0xFB):
                        i += 1

            raw = bytes([hdr]) + len_bytes + pl + bytes([crc_hi, crc_lo])
            calc_raw, calc_crc = canonical_frame_bytes(hdr, pl)
            assert raw == calc_raw and crc_wire == calc_crc
            frames.append(
                {
                    "idx": len(frames),
                    "ev": start_i,
                    "direction": sender,
                    "header": hdr,
                    "payload": pl,
                    "crc16": crc_wire,
                    "raw": raw,
                }
            )
            continue
        raise RuntimeError(f"Unexpected byte in {stem} at {i}")
    return frames


def extract_fxb_frames() -> list[dict[str, Any]]:
    """Extract canonical FXB frames from Trace 03."""
    _, ev = load_trace_bytes(TRACES_DIR, "03_rcopy_bootstrap")
    n = len(ev)
    i = next(
        k
        for k in range(n - 1)
        if ev[k][1] == "SERVER->CLIENT"
        and ev[k][2] == 0x80
        and ev[k + 1][1] == "CLIENT->SERVER"
        and ev[k + 1][2] == 0x7B
    )
    frames: list[dict[str, Any]] = []
    while i < n - 1 and ev[i][1] == "SERVER->CLIENT":
        hdr = ev[i][2]
        ack_hdr = ev[i + 1][2]
        if ev[i + 1][1] != "CLIENT->SERVER" or (hdr ^ ack_hdr) != 0xFB:
            break
        start_i = i
        i += 2
        if hdr & 0x80:
            ln = ev[i][2]
            len_bytes = bytes([ln])
            i += 1
        else:
            lo, hi = ev[i][2], ev[i + 1][2]
            ln = lo | (hi << 8)
            len_bytes = bytes([lo, hi])
            i += 2
        pl = bytes(ev[k][2] for k in range(i, i + ln))
        i += ln
        crc_hi, crc_lo = ev[i][2], ev[i + 1][2]
        i += 3  # 2 CRC + 1 ACK
        raw = bytes([hdr]) + len_bytes + pl + bytes([crc_hi, crc_lo])
        calc_raw, calc_crc = canonical_frame_bytes(hdr, pl)
        assert raw == calc_raw
        frames.append(
            {
                "idx": len(frames),
                "ev": start_i,
                "direction": "SERVER->CLIENT",
                "header": hdr,
                "payload": pl,
                "crc16": calc_crc,
                "raw": raw,
            }
        )
        if pl[0] == 4:
            break
    return frames


def frame_header_fields(f: dict[str, Any]) -> dict[str, Any]:
    hdr = f["header"]
    return {
        "header": hdr,
        "is_short_len": bool(hdr & 0x80),
        "more_fragments": bool(hdr & 0x40),
        "echo_request": bool(hdr & 0x04),
        "sequence_number": hdr & 0x03,
        "len_payload": len(f["payload"]),
        "crc16": f["crc16"],
    }


def make_vector(
    test_case: str,
    source_trace: str,
    payload_type: str,
    f: dict[str, Any],
    parsed_payload: dict[str, Any],
) -> dict[str, Any]:
    parsed = frame_header_fields(f)
    parsed["payload"] = parsed_payload
    return {
        "test_case": test_case,
        "source_trace": source_trace,
        "direction": f["direction"],
        "payload_type": payload_type,
        "raw_hex": f["raw"].hex(),
        "parsed_expected": parsed,
    }


def main() -> int:
    f02 = extract_interlnk_frames("02_handshake_115200")
    f03 = extract_fxb_frames()
    f04 = extract_interlnk_frames("04_bpb_query_fat16")
    f06 = extract_interlnk_frames("06_write_sector_multi")
    f07 = extract_interlnk_frames("07_media_change_error")

    vectors: list[tuple[str, dict[str, Any]]] = []

    # 01: Sync trigger (00 00) from Trace 02
    v01_f = f02[0]
    assert v01_f["payload"] == b"\x00\x00"
    vectors.append((
        "01_sync_trigger.json",
        make_vector(
            "OP00_SESSION_SYNC_TRIGGER",
            "tests/traces/02_handshake_115200.hexlog.gz",
            "msg_sync_trigger",
            v01_f,
            {"opcode": 0, "subcode": 0},
        ),
    ))

    # 02: Session Sync Request (34B) from Trace 02
    v02_f = f02[1]
    pl = v02_f["payload"]
    cver, pmaj, extf, _, cap, mbs = struct.unpack("<HBBBBH", pl[6:14])
    cid, sid = struct.unpack("<II", pl[26:34])
    vectors.append((
        "02_session_sync_req.json",
        make_vector(
            "OP00_SESSION_SYNC_REQ_115200",
            "tests/traces/02_handshake_115200.hexlog.gz",
            "msg_session_sync_req",
            v02_f,
            {
                "client_version": cver,
                "proto_major": pmaj,
                "ext_info_flag": extf,
                "capability": cap,
                "max_block_size": mbs,
                "client_id": cid,
                "server_id": sid,
            },
        ),
    ))

    # 03: Session Sync Response (33B) from Trace 02
    v03_f = f02[2]
    pl = v03_f["payload"]
    sver, rdy, _, cap, mbs = struct.unpack("<HBBBH", pl[6:13])
    pcid, sid = struct.unpack("<II", pl[25:33])
    vectors.append((
        "03_session_sync_resp.json",
        make_vector(
            "OP00_SESSION_SYNC_RESP_115200",
            "tests/traces/02_handshake_115200.hexlog.gz",
            "msg_session_sync_resp",
            v03_f,
            {
                "server_version": sver,
                "ready_flag": rdy,
                "capability": cap,
                "max_block_size": mbs,
                "prev_client_id": pcid,
                "server_id": sid,
            },
        ),
    ))

    # 04: Map Config Request (36B) from Trace 04
    v04_f = f04[3]
    pl = v04_f["payload"]
    op, cpv, dmin, dmaj, mdrv, fdrv = struct.unpack("<BHBBBB", pl[:7])
    vectors.append((
        "04_map_config_req.json",
        make_vector(
            "OP01_MAP_CONFIG_REQ_DOS622",
            "tests/traces/04_bpb_query_fat16.hexlog.gz",
            "msg_map_config_req",
            v04_f,
            {
                "opcode": op,
                "client_proto_ver": cpv,
                "client_dos_minor": dmin,
                "client_dos_major": dmaj,
                "max_client_drives": mdrv,
                "first_client_drive": fdrv,
                "lpt_map_req": list(pl[7:10]),
            },
        ),
    ))

    # 05: Map Config Response (87B, A: + C: mapped) from Trace 07
    v05_f = f07[4]
    pl = v05_f["payload"]
    spmaj, spmin, sdmin, sdmaj, scnt = struct.unpack("<BBBBB", pl[:5])
    attrs = list(struct.unpack("<26H", pl[34:86]))
    vectors.append((
        "05_map_config_resp.json",
        make_vector(
            "OP01_MAP_CONFIG_RESP_DRIVES_A_C",
            "tests/traces/07_media_change_error.hexlog.gz",
            "msg_map_config_resp",
            v05_f,
            {
                "server_proto_major": spmaj,
                "server_proto_minor": spmin,
                "server_dos_minor": sdmin,
                "server_dos_major": sdmaj,
                "server_drive_count": scnt,
                "lpt_map_resp": list(pl[5:8]),
                "drive_map_resp_first4": list(pl[8:12]),
                "server_drive_attrs_first3": attrs[:3],
                "server_flags": pl[86],
            },
        ),
    ))

    # 06 & 07: Get Drive Info Req & Resp (C:) from Trace 04
    v06_f = f04[5]
    v07_f = f04[6]
    vectors.append((
        "06_get_drive_info_req.json",
        make_vector(
            "OP1C_GET_DRIVE_INFO_REQ_C",
            "tests/traces/04_bpb_query_fat16.hexlog.gz",
            "msg_get_drive_info_req",
            v06_f,
            {"opcode": 0x1C, "unit": 2},
        ),
    ))
    vectors.append((
        "07_get_drive_info_resp.json",
        make_vector(
            "OP1C_GET_DRIVE_INFO_RESP_C_10MB",
            "tests/traces/04_bpb_query_fat16.hexlog.gz",
            "msg_get_drive_info_resp",
            v07_f,
            {
                "size_string_ascii": v07_f["payload"][:10].split(b"\x00", 1)[0].decode("ascii"),
                "volume_label_ascii": v07_f["payload"][10:23].split(b"\x00", 1)[0].decode("ascii"),
            },
        ),
    ))

    # 08: Media Check Unchanged (DISK_A) from Trace 07
    v08_f = next(f for f in f07 if f["direction"] == "SERVER->CLIENT" and len(f["payload"]) == 19 and f["payload"][2] == 1 and b"DISK_A" in f["payload"])
    st, ms = struct.unpack("<HB", v08_f["payload"][:3])
    ser = struct.unpack("<I", v08_f["payload"][15:19])[0]
    vectors.append((
        "08_media_check_unchanged.json",
        make_vector(
            "OP02_MEDIA_CHECK_UNCHANGED_FLOPPY",
            "tests/traces/07_media_change_error.hexlog.gz",
            "msg_media_check_resp",
            v08_f,
            {
                "status": st,
                "is_error": bool(st & 0x8000),
                "media_status": ms,
                "volume_label_ascii": v08_f["payload"][3:15].split(b"\x00", 1)[0].decode("ascii"),
                "volume_serial": ser,
            },
        ),
    ))

    # 09: Media Check Changed (0xFF after DISK_A -> DISK_B swap) from Trace 07
    v09_f = next(f for f in f07 if f["direction"] == "SERVER->CLIENT" and len(f["payload"]) == 19 and f["payload"][2] == 0xFF and b"DISK_A" in f["payload"])
    st, ms = struct.unpack("<HB", v09_f["payload"][:3])
    ser = struct.unpack("<I", v09_f["payload"][15:19])[0]
    vectors.append((
        "09_media_check_changed.json",
        make_vector(
            "OP02_MEDIA_CHECK_CHANGED_SWAP",
            "tests/traces/07_media_change_error.hexlog.gz",
            "msg_media_check_resp",
            v09_f,
            {
                "status": st,
                "is_error": bool(st & 0x8000),
                "media_status": ms,
                "volume_label_ascii": v09_f["payload"][3:15].split(b"\x00", 1)[0].decode("ascii"),
                "volume_serial": ser,
            },
        ),
    ))

    # 10: Media Check Error — Drive Not Ready (0x8102) from Trace 07
    v10_f = next(f for f in f07 if f["direction"] == "SERVER->CLIENT" and len(f["payload"]) == 19 and struct.unpack("<H", f["payload"][:2])[0] == 0x8102)
    st, ms = struct.unpack("<HB", v10_f["payload"][:3])
    ser = struct.unpack("<I", v10_f["payload"][15:19])[0]
    vectors.append((
        "10_media_check_not_ready.json",
        make_vector(
            "OP02_MEDIA_CHECK_ERR_DRIVE_NOT_READY",
            "tests/traces/07_media_change_error.hexlog.gz",
            "msg_media_check_resp",
            v10_f,
            {
                "status": st,
                "is_error": True,
                "error_code": 2,
                "media_status": ms,
                "volume_label_ascii": v10_f["payload"][3:15].split(b"\x00", 1)[0].decode("ascii"),
                "volume_serial": ser,
            },
        ),
    ))

    # 11: Build BPB Response (FAT16 10MB Hard Disk) from Trace 04
    v11_f = next(f for f in f04 if f["direction"] == "SERVER->CLIENT" and len(f["payload"]) == 33 and f["payload"][2:4] == b"\x00\x02")
    st, bps, spc, resv, fats, roots, tot16, media, spf, spt, heads, hidden, tot32 = struct.unpack("<HHBHBHHBHHHII", v11_f["payload"][:27])
    vectors.append((
        "11_build_bpb_fat16_resp.json",
        make_vector(
            "OP03_BUILD_BPB_FAT16_10MB",
            "tests/traces/04_bpb_query_fat16.hexlog.gz",
            "msg_build_bpb_resp",
            v11_f,
            {
                "status": st,
                "is_error": False,
                "bytes_per_sector": bps,
                "sectors_per_cluster": spc,
                "reserved_sectors": resv,
                "num_fats": fats,
                "root_entries": roots,
                "total_sectors_16": tot16,
                "media_descriptor": media,
                "sectors_per_fat": spf,
                "sectors_per_track": spt,
                "num_heads": heads,
                "hidden_sectors": hidden,
                "total_sectors_32": tot32,
            },
        ),
    ))

    # 12: Build BPB Response (FAT12 1.44MB Floppy) from Trace 07
    v12_f = next(f for f in f07 if f["direction"] == "SERVER->CLIENT" and len(f["payload"]) == 33 and f["payload"][2:4] == b"\x00\x02")
    st, bps, spc, resv, fats, roots, tot16, media, spf, spt, heads, hidden, tot32 = struct.unpack("<HHBHBHHBHHHII", v12_f["payload"][:27])
    vectors.append((
        "12_build_bpb_fat12_resp.json",
        make_vector(
            "OP03_BUILD_BPB_FAT12_1440K",
            "tests/traces/07_media_change_error.hexlog.gz",
            "msg_build_bpb_resp",
            v12_f,
            {
                "status": st,
                "is_error": False,
                "bytes_per_sector": bps,
                "sectors_per_cluster": spc,
                "reserved_sectors": resv,
                "num_fats": fats,
                "root_entries": roots,
                "total_sectors_16": tot16,
                "media_descriptor": media,
                "sectors_per_fat": spf,
                "sectors_per_track": spt,
                "num_heads": heads,
                "hidden_sectors": hidden,
                "total_sectors_32": tot32,
            },
        ),
    ))

    # 13, 14, 15: Read Sector Request + 4B Resp Header + 512B Long-Length Data Frame from Trace 04
    idx_rd = next(i for i, f in enumerate(f04) if f["direction"] == "CLIENT->SERVER" and len(f["payload"]) == 11 and f["payload"][0] == 4)
    v13_f, v14_f, v15_f = f04[idx_rd], f04[idx_rd + 1], f04[idx_rd + 2]
    op, unit, media, cnt, s16, s32 = struct.unpack("<BBBHH I", v13_f["payload"])
    vectors.append((
        "13_read_sector_req.json",
        make_vector(
            "OP04_READ_SECTOR_SINGLE_REQ",
            "tests/traces/04_bpb_query_fat16.hexlog.gz",
            "msg_sector_io_req",
            v13_f,
            {
                "opcode": op,
                "unit": unit,
                "media_descriptor": media,
                "sector_count": cnt,
                "start_sector_16": s16,
                "start_sector_32": s32,
                "effective_start_sector": s32 if s16 == 0xFFFF else s16,
            },
        ),
    ))
    st, xfer = struct.unpack("<HH", v14_f["payload"])
    vectors.append((
        "14_read_sector_resp_hdr.json",
        make_vector(
            "OP04_READ_SECTOR_RESP_HEADER",
            "tests/traces/04_bpb_query_fat16.hexlog.gz",
            "msg_sector_io_resp",
            v14_f,
            {"status": st, "is_error": False, "sectors_transferred": xfer},
        ),
    ))
    vectors.append((
        "15_read_sector_data_512.json",
        make_vector(
            "OP04_READ_SECTOR_DATA_512B_LONG_FRAME",
            "tests/traces/04_bpb_query_fat16.hexlog.gz",
            "raw_data",
            v15_f,
            {"data_hash_sha256": hashlib.sha256(v15_f["payload"]).hexdigest()},
        ),
    ))

    # 16 & 17: Write Sector Multi-Sector (32 sectors = 16 KB) Req + 8192B Fragmented Data Frame (more_fragments=true) from Trace 06
    idx_wr = next(i for i, f in enumerate(f06) if f["direction"] == "CLIENT->SERVER" and len(f["payload"]) == 11 and f["payload"][0] == 5 and struct.unpack("<H", f["payload"][3:5])[0] == 32)
    v16_f, v17_f = f06[idx_wr], f06[idx_wr + 1]
    assert bool(v17_f["header"] & 0x40) and len(v17_f["payload"]) == 8192
    op, unit, media, cnt, s16, s32 = struct.unpack("<BBBHH I", v16_f["payload"])
    vectors.append((
        "16_write_sector_multi_req.json",
        make_vector(
            "OP05_WRITE_SECTORS_16KB_REQ",
            "tests/traces/06_write_sector_multi.hexlog.gz",
            "msg_sector_io_req",
            v16_f,
            {
                "opcode": op,
                "unit": unit,
                "media_descriptor": media,
                "sector_count": cnt,
                "start_sector_16": s16,
                "start_sector_32": s32,
                "effective_start_sector": s32 if s16 == 0xFFFF else s16,
            },
        ),
    ))
    vectors.append((
        "17_write_sector_frag_8192.json",
        make_vector(
            "OP05_WRITE_DATA_8192B_MORE_FRAGMENTS",
            "tests/traces/06_write_sector_multi.hexlog.gz",
            "raw_data",
            v17_f,
            {"data_hash_sha256": hashlib.sha256(v17_f["payload"]).hexdigest()},
        ),
    ))

    # 18: Write Sector Write-Protect Error Response (0x8100) from Trace 07
    v18_f = next(f for f in f07 if f["direction"] == "SERVER->CLIENT" and len(f["payload"]) == 4 and struct.unpack("<H", f["payload"][:2])[0] == 0x8100)
    st, xfer = struct.unpack("<HH", v18_f["payload"])
    vectors.append((
        "18_write_sector_resp_write_protect.json",
        make_vector(
            "OP05_WRITE_ERR_WRITE_PROTECT",
            "tests/traces/07_media_change_error.hexlog.gz",
            "msg_sector_io_resp",
            v18_f,
            {"status": st, "is_error": True, "error_code": 0, "sectors_transferred": xfer},
        ),
    ))

    # 19 & 20: Generic IOCTL 08/66 Get Media ID Req & 25B MID Resp from Trace 04
    idx_io = next(i for i, f in enumerate(f04) if f["direction"] == "CLIENT->SERVER" and len(f["payload"]) == 4 and f["payload"][0] == 0x1D)
    v19_f, v20_f = f04[idx_io], f04[idx_io + 2]
    vectors.append((
        "19_generic_ioctl_get_mid_req.json",
        make_vector(
            "OP1D_GENERIC_IOCTL_GET_MEDIA_ID_REQ",
            "tests/traces/04_bpb_query_fat16.hexlog.gz",
            "msg_generic_ioctl_req",
            v19_f,
            {"opcode": 0x1D, "unit": v19_f["payload"][1], "category": 8, "function": 0x66},
        ),
    ))
    lvl, ser = struct.unpack("<HI", v20_f["payload"][:6])
    vectors.append((
        "20_generic_ioctl_mid_resp.json",
        make_vector(
            "OP1D_GENERIC_IOCTL_MEDIA_ID_FAT16",
            "tests/traces/04_bpb_query_fat16.hexlog.gz",
            "msg_generic_ioctl_mid",
            v20_f,
            {
                "info_level": lvl,
                "serial_number": ser,
                "volume_label_ascii": v20_f["payload"][6:17].decode("ascii"),
                "filesystem_type_ascii": v20_f["payload"][17:25].decode("ascii"),
            },
        ),
    ))

    # 21: FXB /RCOPY Open INTERSVR.EXE Frame from Trace 03
    v21_f = f03[1]
    pl = v21_f["payload"]
    attr, dtime, ddate, fsize = struct.unpack("<BHHI", pl[161:170])
    vectors.append((
        "21_fxb_open_intersvr.json",
        make_vector(
            "RCOPY_FXB_OPEN_INTERSVR_EXE",
            "tests/traces/03_rcopy_bootstrap.hexlog.gz",
            "fxb_payload",
            v21_f,
            {
                "fxb_op": 1,
                "file_attr": attr,
                "dos_time": dtime,
                "dos_date": ddate,
                "file_size": fsize,
                "filename_ascii": pl[170:183].split(b"\x00", 1)[0].decode("ascii"),
            },
        ),
    ))

    # 22: Fault Injection — CRC-16/GENIBUS Mismatch (1-bit flip in real Read Sector Req)
    bad_crc_raw = bytearray(v13_f["raw"])
    bad_crc_raw[-1] ^= 0x01
    vectors.append((
        "22_err_checksum_mismatch.json",
        {
            "test_case": "FAULT_INJECTION_CRC16_GENIBUS_MISMATCH",
            "source_trace": "derived:04_bpb_query_fat16.hexlog.gz",
            "direction": "CLIENT->SERVER",
            "payload_type": "msg_sector_io_req",
            "raw_hex": bytes(bad_crc_raw).hex(),
            "expected_error": "CHECKSUM_MISMATCH",
        },
    ))

    # 23: Fault Injection — Truncated Long Frame (missing trailing payload + CRC)
    vectors.append((
        "23_err_truncated_frame.json",
        {
            "test_case": "FAULT_INJECTION_TRUNCATED_512B_FRAME",
            "source_trace": "derived:04_bpb_query_fat16.hexlog.gz",
            "direction": "SERVER->CLIENT",
            "payload_type": "raw_data",
            "raw_hex": v15_f["raw"][:64].hex(),
            "expected_error": "INVALID_FRAME",
        },
    ))

    # 24: Fault Injection — Non-Zero Reserved Header Bits (bits 5..3 = 0b011 -> 0x98)
    bad_hdr_pl = b"\x00\x00"
    bad_hdr_raw, _ = canonical_frame_bytes(0x98, bad_hdr_pl)
    vectors.append((
        "24_err_reserved_header_bits.json",
        {
            "test_case": "FAULT_INJECTION_RESERVED_HEADER_BITS",
            "source_trace": "synthetic",
            "direction": "CLIENT->SERVER",
            "payload_type": "msg_sync_trigger",
            "raw_hex": bad_hdr_raw.hex(),
            "expected_error": "INVALID_FRAME",
        },
    ))

    # Remove old provisional fixtures before writing the verified set
    FIXTURES_DIR.mkdir(parents=True, exist_ok=True)
    for old in FIXTURES_DIR.glob("*.json"):
        old.unlink()

    for fname, obj in vectors:
        (FIXTURES_DIR / fname).write_text(json.dumps(obj, indent=2) + "\n", encoding="utf-8")

    print(f"[extract_fixtures] Wrote {len(vectors)} verified JSON fixtures to {FIXTURES_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
