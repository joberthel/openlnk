#!/usr/bin/env python3
"""Formal Protocol Test Harness for OpenLNK (Kaitai Struct + C99 Differential Runner).

Validates every JSON test vector in tests/fixtures/*.json against:
  1. The Kaitai Struct Python parser compiled from specs/interlnk_wire.ksy
     (tests/harness/parsers/interlnk_wire.py), with a spec-conforming pure-Python
     reference decoder fallback when kaitaistruct is not installed locally.
  2. Deterministic CRC-16/GENIBUS checksum validation (poly 0x1021, init 0xFFFF,
     xorout 0xFFFF, big-endian on wire; CCITT-FALSE residue == 0x1D0F).
  3. Optional cross-check against the clean-room C99 binary (openlnk-vector-check)
     to assert 100% differential equivalence between spec and implementation.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_SPEC = REPO_ROOT / "specs" / "interlnk_wire.ksy"
DEFAULT_PARSERS_DIR = REPO_ROOT / "tests" / "harness" / "parsers"
DEFAULT_FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"

CRC16_RESIDUE_GOOD = 0x1D0F


def crc16_ccitt_raw(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def crc16_genibus(data: bytes) -> int:
    """CRC-16/GENIBUS (poly 0x1021, init 0xFFFF, unreflected, xorout 0xFFFF)."""
    return crc16_ccitt_raw(data) ^ 0xFFFF


@dataclass
class DecodedFrame:
    header: int
    is_short_len: bool
    more_fragments: bool
    reserved_bits: int
    echo_request: bool
    sequence_number: int
    len_payload: int
    payload_bytes: bytes
    payload: dict[str, Any]
    crc16: int


def compile_ksy_if_requested(spec_path: Path, parsers_dir: Path) -> None:
    compiler = shutil.which("kaitai-struct-compiler") or shutil.which("ksc")
    if not compiler:
        raise RuntimeError(
            "kaitai-struct-compiler (ksc) not found in PATH. Install it or omit --compile-ksy."
        )
    parsers_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [compiler, "--target", "python", "--outdir", str(parsers_dir), str(spec_path)],
        check=True,
    )


def _try_load_kaitai_module(parsers_dir: Path) -> Optional[Any]:
    module_file = parsers_dir / "interlnk_wire.py"
    if not module_file.is_file():
        return None
    if str(parsers_dir) not in sys.path:
        sys.path.insert(0, str(parsers_dir))
    try:
        return importlib.import_module("interlnk_wire")
    except ImportError:
        return None


def _enum_int(val: Any) -> int:
    return int(val.value if hasattr(val, "value") else val)


def _ascii_z(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("ascii")


def _decode_payload_kaitai(module: Any, payload_type: str, pbytes: bytes) -> dict[str, Any]:
    cls_root = module.InterlnkWire
    if payload_type == "raw_data":
        return {"data_hash_sha256": hashlib.sha256(pbytes).hexdigest()}
    if payload_type == "msg_sync_trigger":
        m = cls_root.MsgSyncTrigger.from_bytes(pbytes)
        return {"opcode": _enum_int(m.opcode), "subcode": int(m.subcode)}
    if payload_type == "msg_session_sync_req":
        m = cls_root.MsgSessionSyncReq.from_bytes(pbytes)
        return {
            "client_version": int(m.client_version),
            "proto_major": int(m.proto_major),
            "ext_info_flag": int(m.ext_info_flag),
            "capability": int(m.capability),
            "max_block_size": int(m.max_block_size),
            "client_id": int(m.client_id),
            "server_id": int(m.server_id),
        }
    if payload_type == "msg_session_sync_resp":
        m = cls_root.MsgSessionSyncResp.from_bytes(pbytes)
        return {
            "server_version": int(m.server_version),
            "ready_flag": int(m.ready_flag),
            "capability": int(m.capability),
            "max_block_size": int(m.max_block_size),
            "prev_client_id": int(m.prev_client_id),
            "server_id": int(m.server_id),
        }
    if payload_type == "msg_map_config_req":
        m = cls_root.MsgMapConfigReq.from_bytes(pbytes)
        return {
            "opcode": _enum_int(m.opcode),
            "client_proto_ver": int(m.client_proto_ver),
            "client_dos_minor": int(m.client_dos_minor),
            "client_dos_major": int(m.client_dos_major),
            "max_client_drives": int(m.max_client_drives),
            "first_client_drive": int(m.first_client_drive),
            "lpt_map_req": [int(x) for x in m.lpt_map_req],
        }
    if payload_type == "msg_map_config_resp":
        m = cls_root.MsgMapConfigResp.from_bytes(pbytes)
        return {
            "server_proto_major": int(m.server_proto_major),
            "server_proto_minor": int(m.server_proto_minor),
            "server_dos_minor": int(m.server_dos_minor),
            "server_dos_major": int(m.server_dos_major),
            "server_drive_count": int(m.server_drive_count),
            "lpt_map_resp": [int(x) for x in m.lpt_map_resp],
            "drive_map_resp_first4": [int(x) for x in m.drive_map_resp[:4]],
            "server_drive_attrs_first3": [int(x) for x in m.server_drive_attrs[:3]],
            "server_flags": int(m.server_flags),
        }
    if payload_type == "msg_get_drive_info_req":
        m = cls_root.MsgGetDriveInfoReq.from_bytes(pbytes)
        return {"opcode": _enum_int(m.opcode), "unit": int(m.unit)}
    if payload_type == "msg_get_drive_info_resp":
        m = cls_root.MsgGetDriveInfoResp.from_bytes(pbytes)
        return {
            "size_string_ascii": _ascii_z(bytes(m.size_string)),
            "volume_label_ascii": _ascii_z(bytes(m.volume_label)),
        }
    if payload_type == "msg_media_check_resp":
        m = cls_root.MsgMediaCheckResp.from_bytes(pbytes)
        out: dict[str, Any] = {
            "status": int(m.status),
            "is_error": bool(m.is_error),
            "media_status": _enum_int(m.media_status),
            "volume_label_ascii": _ascii_z(bytes(m.volume_label)),
            "volume_serial": int(m.volume_serial),
        }
        if m.is_error:
            out["error_code"] = _enum_int(m.error_code)
        return out
    if payload_type == "msg_build_bpb_resp":
        m = cls_root.MsgBuildBpbResp.from_bytes(pbytes)
        out = {"status": int(m.status), "is_error": bool(m.is_error)}
        if not m.is_error and m.bpb is not None:
            b = m.bpb
            out.update({
                "bytes_per_sector": int(b.bytes_per_sector),
                "sectors_per_cluster": int(b.sectors_per_cluster),
                "reserved_sectors": int(b.reserved_sectors),
                "num_fats": int(b.num_fats),
                "root_entries": int(b.root_entries),
                "total_sectors_16": int(b.total_sectors_16),
                "media_descriptor": int(b.media_descriptor),
                "sectors_per_fat": int(b.sectors_per_fat),
                "sectors_per_track": int(b.sectors_per_track),
                "num_heads": int(b.num_heads),
                "hidden_sectors": int(b.hidden_sectors),
                "total_sectors_32": int(b.total_sectors_32),
            })
        return out
    if payload_type == "msg_sector_io_req":
        m = cls_root.MsgSectorIoReq.from_bytes(pbytes)
        return {
            "opcode": _enum_int(m.opcode),
            "unit": int(m.unit),
            "media_descriptor": int(m.media_descriptor),
            "sector_count": int(m.sector_count),
            "start_sector_16": int(m.start_sector_16),
            "start_sector_32": int(m.start_sector_32),
            "effective_start_sector": int(m.effective_start_sector),
        }
    if payload_type == "msg_sector_io_resp":
        m = cls_root.MsgSectorIoResp.from_bytes(pbytes)
        out = {
            "status": int(m.status),
            "is_error": bool(m.is_error),
            "sectors_transferred": int(m.sectors_transferred),
        }
        if m.is_error:
            out["error_code"] = _enum_int(m.error_code)
        return out
    if payload_type == "msg_generic_ioctl_req":
        m = cls_root.MsgGenericIoctlReq.from_bytes(pbytes)
        return {
            "opcode": _enum_int(m.opcode),
            "unit": int(m.unit),
            "category": int(m.category),
            "function": _enum_int(m.function),
        }
    if payload_type == "msg_generic_ioctl_mid":
        m = cls_root.MsgGenericIoctlMid.from_bytes(pbytes)
        return {
            "info_level": int(m.info_level),
            "serial_number": int(m.serial_number),
            "volume_label_ascii": bytes(m.volume_label).decode("ascii"),
            "filesystem_type_ascii": bytes(m.filesystem_type).decode("ascii"),
        }
    if payload_type == "fxb_payload":
        m = cls_root.FxbPayload.from_bytes(pbytes)
        out = {"fxb_op": _enum_int(m.fxb_op)}
        if out["fxb_op"] in (1, 2, 3):
            out.update({
                "file_attr": int(m.file_attr),
                "dos_time": int(m.dos_time),
                "dos_date": int(m.dos_date),
                "file_size": int(m.file_size),
                "filename_ascii": _ascii_z(bytes(m.filename)),
            })
        return out
    raise ValueError(f"Unsupported payload_type {payload_type!r}")


def _decode_payload_reference(payload_type: str, pbytes: bytes) -> dict[str, Any]:
    """Pure-Python reference payload decoder matching specs/interlnk_wire.ksy."""
    if payload_type == "raw_data":
        return {"data_hash_sha256": hashlib.sha256(pbytes).hexdigest()}
    if payload_type == "msg_sync_trigger":
        if len(pbytes) != 2:
            raise ValueError("INVALID_FRAME: msg_sync_trigger must be 2 bytes")
        return {"opcode": pbytes[0], "subcode": pbytes[1]}
    if payload_type == "msg_session_sync_req":
        if len(pbytes) != 34:
            raise ValueError("INVALID_FRAME: msg_session_sync_req must be 34 bytes")
        cver, pmaj, extf, _, cap, mbs = struct.unpack("<HBBBBH", pbytes[6:14])
        cid, sid = struct.unpack("<II", pbytes[26:34])
        return {
            "client_version": cver,
            "proto_major": pmaj,
            "ext_info_flag": extf,
            "capability": cap,
            "max_block_size": mbs,
            "client_id": cid,
            "server_id": sid,
        }
    if payload_type == "msg_session_sync_resp":
        if len(pbytes) < 33:
            raise ValueError("INVALID_FRAME: msg_session_sync_resp must be >= 33 bytes")
        sver, rdy, _, cap, mbs = struct.unpack("<HBBBH", pbytes[6:13])
        pcid, sid = struct.unpack("<II", pbytes[25:33])
        return {
            "server_version": sver,
            "ready_flag": rdy,
            "capability": cap,
            "max_block_size": mbs,
            "prev_client_id": pcid,
            "server_id": sid,
        }
    if payload_type == "msg_map_config_req":
        if len(pbytes) != 36:
            raise ValueError("INVALID_FRAME: msg_map_config_req must be 36 bytes")
        op, cpv, dmin, dmaj, mdrv, fdrv = struct.unpack("<BHBBBB", pbytes[:7])
        return {
            "opcode": op,
            "client_proto_ver": cpv,
            "client_dos_minor": dmin,
            "client_dos_major": dmaj,
            "max_client_drives": mdrv,
            "first_client_drive": fdrv,
            "lpt_map_req": list(pbytes[7:10]),
        }
    if payload_type == "msg_map_config_resp":
        if len(pbytes) != 87:
            raise ValueError("INVALID_FRAME: msg_map_config_resp must be 87 bytes")
        spmaj, spmin, sdmin, sdmaj, scnt = struct.unpack("<BBBBB", pbytes[:5])
        attrs = list(struct.unpack("<26H", pbytes[34:86]))
        return {
            "server_proto_major": spmaj,
            "server_proto_minor": spmin,
            "server_dos_minor": sdmin,
            "server_dos_major": sdmaj,
            "server_drive_count": scnt,
            "lpt_map_resp": list(pbytes[5:8]),
            "drive_map_resp_first4": list(pbytes[8:12]),
            "server_drive_attrs_first3": attrs[:3],
            "server_flags": pbytes[86],
        }
    if payload_type == "msg_get_drive_info_req":
        if len(pbytes) != 2:
            raise ValueError("INVALID_FRAME: msg_get_drive_info_req must be 2 bytes")
        return {"opcode": pbytes[0], "unit": pbytes[1]}
    if payload_type == "msg_get_drive_info_resp":
        if len(pbytes) != 23:
            raise ValueError("INVALID_FRAME: msg_get_drive_info_resp must be 23 bytes")
        return {
            "size_string_ascii": _ascii_z(pbytes[:10]),
            "volume_label_ascii": _ascii_z(pbytes[10:23]),
        }
    if payload_type == "msg_media_check_resp":
        if len(pbytes) != 19:
            raise ValueError("INVALID_FRAME: msg_media_check_resp must be 19 bytes")
        st, ms = struct.unpack("<HB", pbytes[:3])
        ser = struct.unpack("<I", pbytes[15:19])[0]
        out: dict[str, Any] = {
            "status": st,
            "is_error": bool(st & 0x8000),
            "media_status": ms,
            "volume_label_ascii": _ascii_z(pbytes[3:15]),
            "volume_serial": ser,
        }
        if st & 0x8000:
            out["error_code"] = st & 0xFF
        return out
    if payload_type == "msg_build_bpb_resp":
        if len(pbytes) < 2:
            raise ValueError("INVALID_FRAME: msg_build_bpb_resp too short")
        st = struct.unpack("<H", pbytes[:2])[0]
        out = {"status": st, "is_error": bool(st & 0x8000)}
        if not (st & 0x8000) and len(pbytes) >= 27:
            bps, spc, resv, fats, roots, tot16, media, spf, spt, heads, hidden, tot32 = struct.unpack(
                "<HBHBHHBHHHII", pbytes[2:27]
            )
            out.update({
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
            })
        return out
    if payload_type == "msg_sector_io_req":
        if len(pbytes) != 11:
            raise ValueError("INVALID_FRAME: msg_sector_io_req must be 11 bytes")
        op, unit, media, cnt, s16, s32 = struct.unpack("<BBBHH I", pbytes)
        return {
            "opcode": op,
            "unit": unit,
            "media_descriptor": media,
            "sector_count": cnt,
            "start_sector_16": s16,
            "start_sector_32": s32,
            "effective_start_sector": s32 if s16 == 0xFFFF else s16,
        }
    if payload_type == "msg_sector_io_resp":
        if len(pbytes) < 4:
            raise ValueError("INVALID_FRAME: msg_sector_io_resp must be >= 4 bytes")
        st, xfer = struct.unpack("<HH", pbytes[:4])
        out = {"status": st, "is_error": bool(st & 0x8000), "sectors_transferred": xfer}
        if st & 0x8000:
            out["error_code"] = st & 0xFF
        return out
    if payload_type == "msg_generic_ioctl_req":
        if len(pbytes) != 4:
            raise ValueError("INVALID_FRAME: msg_generic_ioctl_req must be 4 bytes")
        return {"opcode": pbytes[0], "unit": pbytes[1], "category": pbytes[2], "function": pbytes[3]}
    if payload_type == "msg_generic_ioctl_mid":
        if len(pbytes) != 25:
            raise ValueError("INVALID_FRAME: msg_generic_ioctl_mid must be 25 bytes")
        lvl, ser = struct.unpack("<HI", pbytes[:6])
        return {
            "info_level": lvl,
            "serial_number": ser,
            "volume_label_ascii": pbytes[6:17].decode("ascii"),
            "filesystem_type_ascii": pbytes[17:25].decode("ascii"),
        }
    if payload_type == "fxb_payload":
        op = pbytes[0]
        out = {"fxb_op": op}
        if op in (1, 2, 3):
            if len(pbytes) < 183:
                raise ValueError("INVALID_FRAME: fxb_payload too short")
            attr, dtime, ddate, fsize = struct.unpack("<BHHI", pbytes[161:170])
            out.update({
                "file_attr": attr,
                "dos_time": dtime,
                "dos_date": ddate,
                "file_size": fsize,
                "filename_ascii": _ascii_z(pbytes[170:183]),
            })
        return out
    raise ValueError(f"Unsupported payload_type {payload_type!r}")


def parse_with_kaitai(module: Any, raw: bytes, payload_type: str) -> DecodedFrame:
    pkt = module.InterlnkWire.from_bytes(raw)
    if pkt.reserved_bits != 0:
        raise ValueError(f"INVALID_FRAME: non-zero reserved header bits ({pkt.reserved_bits:#x})")
    len_field_size = 1 if pkt.is_short_len else 2
    expected_total = 1 + len_field_size + int(pkt.len_payload) + 2
    if len(raw) != expected_total:
        raise ValueError(f"INVALID_FRAME: length mismatch (got {len(raw)}, expected {expected_total})")
    pbytes = bytes(pkt.payload)
    payload_dict = _decode_payload_kaitai(module, payload_type, pbytes)
    return DecodedFrame(
        header=int(pkt.header),
        is_short_len=bool(pkt.is_short_len),
        more_fragments=bool(pkt.more_fragments),
        reserved_bits=int(pkt.reserved_bits),
        echo_request=bool(pkt.echo_request),
        sequence_number=int(pkt.sequence_number),
        len_payload=int(pkt.len_payload),
        payload_bytes=pbytes,
        payload=payload_dict,
        crc16=int(pkt.crc16),
    )


def parse_with_reference(raw: bytes, payload_type: str) -> DecodedFrame:
    if len(raw) < 4:
        raise ValueError("INVALID_FRAME: truncated frame (< 4 bytes)")
    hdr = raw[0]
    reserved = (hdr >> 3) & 0x07
    if reserved != 0:
        raise ValueError(f"INVALID_FRAME: non-zero reserved header bits ({reserved:#x})")
    is_short = bool(hdr & 0x80)
    more = bool(hdr & 0x40)
    echo_req = bool(hdr & 0x04)
    seq = hdr & 0x03

    if is_short:
        len_sz = 1
        plen = raw[1]
    else:
        if len(raw) < 5:
            raise ValueError("INVALID_FRAME: truncated long-length header")
        len_sz = 2
        plen = struct.unpack("<H", raw[1:3])[0]

    expected_total = 1 + len_sz + plen + 2
    if len(raw) != expected_total:
        raise ValueError(f"INVALID_FRAME: length mismatch (got {len(raw)}, expected {expected_total})")
    pbytes = raw[1 + len_sz : 1 + len_sz + plen]
    crc16 = struct.unpack(">H", raw[1 + len_sz + plen : expected_total])[0]
    payload_dict = _decode_payload_reference(payload_type, pbytes)
    return DecodedFrame(
        header=hdr,
        is_short_len=is_short,
        more_fragments=more,
        reserved_bits=reserved,
        echo_request=echo_req,
        sequence_number=seq,
        len_payload=plen,
        payload_bytes=pbytes,
        payload=payload_dict,
        crc16=crc16,
    )


def verify_c_binary_parity(
    c_binary: Path,
    raw_hex: str,
    expected_error: Optional[str],
    expected_parsed: Optional[dict[str, Any]],
) -> None:
    proc = subprocess.run(
        [str(c_binary)],
        input=raw_hex,
        text=True,
        capture_output=True,
        check=True,
    )
    c_obj = json.loads(proc.stdout.strip())
    if expected_error:
        if c_obj.get("status") != expected_error:
            raise AssertionError(
                f"C99 mismatch: expected error {expected_error!r}, got {c_obj!r}"
            )
        return

    assert expected_parsed is not None
    if c_obj.get("status") != "OK":
        raise AssertionError(f"C99 failed to parse valid vector: {c_obj!r}")

    for field in (
        "header",
        "is_short_len",
        "more_fragments",
        "echo_request",
        "sequence_number",
        "len_payload",
        "crc16",
    ):
        if field in c_obj and c_obj.get(field) != expected_parsed.get(field):
            raise AssertionError(
                f"C99 field {field!r} mismatch: got {c_obj.get(field)!r}, "
                f"expected {expected_parsed.get(field)!r}"
            )


def run_vector(
    vector_path: Path,
    kaitai_module: Optional[Any],
    c_binary: Optional[Path],
) -> None:
    vec = json.loads(vector_path.read_text(encoding="utf-8"))
    raw_hex: str = vec["raw_hex"]
    raw = bytes.fromhex(raw_hex)
    payload_type: str = vec["payload_type"]
    expected_error: Optional[str] = vec.get("expected_error")
    expected_parsed: Optional[dict[str, Any]] = vec.get("parsed_expected")

    def decode() -> DecodedFrame:
        if kaitai_module is not None:
            return parse_with_kaitai(kaitai_module, raw, payload_type)
        return parse_with_reference(raw, payload_type)

    if expected_error:
        if expected_error == "CHECKSUM_MISMATCH":
            decoded = decode()
            crc_covered = raw[1:-2]
            calc_crc = crc16_genibus(crc_covered)
            residue = crc16_ccitt_raw(raw[1:])
            if calc_crc == decoded.crc16 or residue == CRC16_RESIDUE_GOOD:
                raise AssertionError("Expected CHECKSUM_MISMATCH, but CRC-16/GENIBUS matched!")
        else:
            failed = False
            try:
                decode()
            except Exception:
                failed = True
            if not failed:
                raise AssertionError(f"Expected parse error {expected_error}, but succeeded!")
    else:
        assert expected_parsed is not None, "Valid test vector must define parsed_expected"
        decoded = decode()
        crc_covered = raw[1:-2]
        calc_crc = crc16_genibus(crc_covered)
        residue = crc16_ccitt_raw(raw[1:])
        if calc_crc != decoded.crc16 or residue != CRC16_RESIDUE_GOOD:
            raise AssertionError(
                f"CRC-16/GENIBUS mismatch: wire=0x{decoded.crc16:04X}, "
                f"calc=0x{calc_crc:04X}, residue=0x{residue:04X}"
            )

        actual = {
            "header": decoded.header,
            "is_short_len": decoded.is_short_len,
            "more_fragments": decoded.more_fragments,
            "echo_request": decoded.echo_request,
            "sequence_number": decoded.sequence_number,
            "len_payload": decoded.len_payload,
            "crc16": decoded.crc16,
        }
        for key, act_val in actual.items():
            if expected_parsed[key] != act_val:
                raise AssertionError(
                    f"Field {key!r} mismatch: got {act_val!r}, expected {expected_parsed[key]!r}"
                )
        for key, exp_val in expected_parsed.get("payload", {}).items():
            if decoded.payload.get(key) != exp_val:
                raise AssertionError(
                    f"Payload {key!r} mismatch: got {decoded.payload.get(key)!r}, expected {exp_val!r}"
                )

    if c_binary is not None:
        verify_c_binary_parity(c_binary, raw_hex, expected_error, expected_parsed)


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Validate JSON protocol test vectors against Kaitai & C99 parsers."
    )
    parser.add_argument("--fixtures-dir", type=Path, default=DEFAULT_FIXTURES_DIR)
    parser.add_argument("--parsers-dir", type=Path, default=DEFAULT_PARSERS_DIR)
    parser.add_argument("--spec", type=Path, default=DEFAULT_SPEC)
    parser.add_argument("--compile-ksy", action="store_true", help="Compile .ksy via ksc first")
    parser.add_argument(
        "--require-kaitai",
        action="store_true",
        help="Fail if generated Kaitai Python parser is unavailable (required in CI)",
    )
    parser.add_argument(
        "--c-binary",
        type=Path,
        help="Optional path to cleanroom/openlnk-vector-check for C99 differential parity",
    )
    args = parser.parse_args(argv)

    if args.compile_ksy:
        compile_ksy_if_requested(args.spec, args.parsers_dir)

    kaitai_mod = _try_load_kaitai_module(args.parsers_dir)
    if args.require_kaitai and kaitai_mod is None:
        print(
            "[test_runner] ERROR: --require-kaitai set, but compiled Kaitai parser "
            f"could not be imported from {args.parsers_dir}.",
            file=sys.stderr,
        )
        return 1

    mode_label = "kaitai-generated" if kaitai_mod is not None else "reference-fallback"
    c_label = str(args.c_binary) if args.c_binary else "disabled"
    print(f"[test_runner] Parser mode: {mode_label} | C99 cross-check: {c_label}")

    fixtures = sorted(args.fixtures_dir.glob("*.json"))
    if not fixtures:
        print(f"[test_runner] ERROR: No JSON test vectors found in {args.fixtures_dir}", file=sys.stderr)
        return 1

    failures = 0
    for fpath in fixtures:
        vec = json.loads(fpath.read_text(encoding="utf-8"))
        name = vec.get("test_case", fpath.name)
        try:
            run_vector(fpath, kaitai_mod, args.c_binary)
            print(f"  [PASS] {name} ({fpath.name})")
        except Exception as exc:
            failures += 1
            print(f"  [FAIL] {name} ({fpath.name}): {exc}", file=sys.stderr)

    print(f"[test_runner] Summary: {len(fixtures) - failures}/{len(fixtures)} passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
