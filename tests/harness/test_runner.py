#!/usr/bin/env python3
"""Formal Protocol Test Harness for OpenLNK (Kaitai Struct + C99 Differential Runner).

Validates every JSON test vector in tests/fixtures/*.json against:
  1. The Kaitai Struct Python parser compiled from specs/interlnk_wire.ksy
     (tests/harness/parsers/interlnk_wire.py), with an optional built-in
     reference decoder fallback when kaitaistruct is not installed locally.
  2. Deterministic CRC-16/CCITT-FALSE checksum validation.
  3. Optional cross-check against the clean-room C99 binary (ilnk-vector-check)
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


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


@dataclass
class DecodedFrame:
    sync_bytes: list[int]
    frame_type: int
    sequence_number: int
    payload_length: int
    payload: dict[str, Any]
    checksum: int
    data_bytes: Optional[bytes] = None


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


def parse_with_kaitai(module: Any, raw: bytes) -> DecodedFrame:
    pkt = module.InterlnkWire.from_bytes(raw)
    ft = int(pkt.frame_type.value if hasattr(pkt.frame_type, "value") else pkt.frame_type)
    payload_obj = pkt.payload
    payload_dict: dict[str, Any] = {}
    data_bytes: Optional[bytes] = None

    if ft == 0x01:  # cmd_init
        payload_dict = {
            "client_version": int(payload_obj.client_version),
            "drive_count": int(payload_obj.drive_count),
        }
    elif ft == 0x02:  # cmd_read_sector
        payload_dict = {
            "drive_index": int(payload_obj.drive_index),
            "start_sector": int(payload_obj.start_sector),
            "sector_count": int(payload_obj.sector_count),
        }
    elif ft == 0x03:  # cmd_write_sector
        data_bytes = bytes(payload_obj.data)
        payload_dict = {
            "drive_index": int(payload_obj.drive_index),
            "start_sector": int(payload_obj.start_sector),
            "sector_count": int(payload_obj.sector_count),
            "data_hash_sha256": hashlib.sha256(data_bytes).hexdigest(),
        }
    elif ft == 0x81:  # resp_init_ack
        payload_dict = {
            "server_version": int(payload_obj.server_version),
            "drive_count": int(payload_obj.drive_count),
            "status": int(payload_obj.status),
        }
    elif ft == 0x82:  # resp_read_data
        data_bytes = bytes(payload_obj.data)
        payload_dict = {
            "drive_index": int(payload_obj.drive_index),
            "start_sector": int(payload_obj.start_sector),
            "data_hash_sha256": hashlib.sha256(data_bytes).hexdigest(),
        }
    elif ft == 0xFF:  # resp_error
        payload_dict = {
            "error_code": int(payload_obj.error_code),
            "detail": int(payload_obj.detail),
        }

    return DecodedFrame(
        sync_bytes=list(pkt.sync_bytes),
        frame_type=ft,
        sequence_number=int(pkt.sequence_number),
        payload_length=int(pkt.payload_length),
        payload=payload_dict,
        checksum=int(pkt.checksum),
        data_bytes=data_bytes,
    )


def parse_with_reference(raw: bytes) -> DecodedFrame:
    """Spec-conforming pure-Python reference decoder matching specs/interlnk_wire.ksy."""
    if len(raw) < 8:
        raise ValueError("INVALID_FRAME: truncated frame (< 8 bytes)")
    sync = list(raw[0:2])
    if sync != [0x55, 0xAA]:
        raise ValueError(f"INVALID_FRAME: bad sync bytes {sync!r}")
    frame_type, seq_num, payload_len = struct.unpack("<BBH", raw[2:6])
    expected_total = 6 + payload_len + 2
    if len(raw) != expected_total:
        raise ValueError(
            f"INVALID_FRAME: length mismatch (got {len(raw)}, expected {expected_total})"
        )
    pbytes = raw[6 : 6 + payload_len]
    checksum = struct.unpack("<H", raw[6 + payload_len : expected_total])[0]

    payload_dict: dict[str, Any] = {}
    data_bytes: Optional[bytes] = None

    if frame_type == 0x01:
        if payload_len != 3:
            raise ValueError("INVALID_FRAME: payload_init must be 3 bytes")
        ver, drv_cnt = struct.unpack("<HB", pbytes)
        payload_dict = {"client_version": ver, "drive_count": drv_cnt}
    elif frame_type == 0x02:
        if payload_len != 7:
            raise ValueError("INVALID_FRAME: payload_read must be 7 bytes")
        drv, start_sec, sec_cnt = struct.unpack("<BIH", pbytes)
        payload_dict = {
            "drive_index": drv,
            "start_sector": start_sec,
            "sector_count": sec_cnt,
        }
    elif frame_type == 0x03:
        if payload_len < 7:
            raise ValueError("INVALID_FRAME: payload_write must be >= 7 bytes")
        drv, start_sec, sec_cnt = struct.unpack("<BIH", pbytes[:7])
        data_bytes = pbytes[7:]
        if len(data_bytes) != sec_cnt * 512:
            raise ValueError("INVALID_FRAME: payload_write data length mismatch")
        payload_dict = {
            "drive_index": drv,
            "start_sector": start_sec,
            "sector_count": sec_cnt,
            "data_hash_sha256": hashlib.sha256(data_bytes).hexdigest(),
        }
    elif frame_type == 0x81:
        if payload_len != 4:
            raise ValueError("INVALID_FRAME: payload_init_ack must be 4 bytes")
        ver, drv_cnt, status = struct.unpack("<HBB", pbytes)
        payload_dict = {"server_version": ver, "drive_count": drv_cnt, "status": status}
    elif frame_type == 0x82:
        if payload_len < 5:
            raise ValueError("INVALID_FRAME: payload_read_resp must be >= 5 bytes")
        drv, start_sec = struct.unpack("<BI", pbytes[:5])
        data_bytes = pbytes[5:]
        payload_dict = {
            "drive_index": drv,
            "start_sector": start_sec,
            "data_hash_sha256": hashlib.sha256(data_bytes).hexdigest(),
        }
    elif frame_type == 0xFF:
        if payload_len != 3:
            raise ValueError("INVALID_FRAME: payload_error must be 3 bytes")
        err_code, detail = struct.unpack("<BH", pbytes)
        payload_dict = {"error_code": err_code, "detail": detail}
    else:
        raise ValueError(f"INVALID_FRAME: unknown frame_type 0x{frame_type:02X}")

    return DecodedFrame(
        sync_bytes=sync,
        frame_type=frame_type,
        sequence_number=seq_num,
        payload_length=payload_len,
        payload=payload_dict,
        checksum=checksum,
        data_bytes=data_bytes,
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

    for field in ("sync_bytes", "frame_type", "sequence_number", "payload_length", "checksum"):
        if c_obj.get(field) != expected_parsed.get(field):
            raise AssertionError(
                f"C99 field {field!r} mismatch: got {c_obj.get(field)!r}, "
                f"expected {expected_parsed.get(field)!r}"
            )

    c_payload = dict(c_obj.get("payload", {}))
    if "data_hex" in c_payload:
        data_bytes = bytes.fromhex(c_payload.pop("data_hex"))
        c_payload["data_hash_sha256"] = hashlib.sha256(data_bytes).hexdigest()

    for key, exp_val in expected_parsed.get("payload", {}).items():
        if c_payload.get(key) != exp_val:
            raise AssertionError(
                f"C99 payload.{key} mismatch: got {c_payload.get(key)!r}, expected {exp_val!r}"
            )


def run_vector(
    vector_path: Path,
    kaitai_module: Optional[Any],
    c_binary: Optional[Path],
) -> None:
    vec = json.loads(vector_path.read_text(encoding="utf-8"))
    raw_hex: str = vec["raw_hex"]
    raw = bytes.fromhex(raw_hex)
    expected_error: Optional[str] = vec.get("expected_error")
    expected_parsed: Optional[dict[str, Any]] = vec.get("parsed_expected")

    if expected_error:
        if expected_error == "CHECKSUM_MISMATCH":
            decoded = (
                parse_with_kaitai(kaitai_module, raw)
                if kaitai_module is not None
                else parse_with_reference(raw)
            )
            calc_crc = crc16_ccitt_false(raw[2 : 6 + decoded.payload_length])
            if calc_crc == decoded.checksum:
                raise AssertionError("Expected CHECKSUM_MISMATCH, but CRC matched!")
        else:
            failed = False
            try:
                if kaitai_module is not None:
                    parse_with_kaitai(kaitai_module, raw)
                else:
                    parse_with_reference(raw)
            except Exception:
                failed = True
            if not failed:
                raise AssertionError(f"Expected parse error {expected_error}, but succeeded!")
    else:
        assert expected_parsed is not None, "Valid test vector must define parsed_expected"
        decoded = (
            parse_with_kaitai(kaitai_module, raw)
            if kaitai_module is not None
            else parse_with_reference(raw)
        )
        calc_crc = crc16_ccitt_false(raw[2 : 6 + decoded.payload_length])
        if calc_crc != decoded.checksum:
            raise AssertionError(
                f"CRC16 mismatch: wire=0x{decoded.checksum:04X}, calc=0x{calc_crc:04X}"
            )

        actual = {
            "sync_bytes": decoded.sync_bytes,
            "frame_type": decoded.frame_type,
            "sequence_number": decoded.sequence_number,
            "payload_length": decoded.payload_length,
            "checksum": decoded.checksum,
        }
        for key, exp_val in actual.items():
            if expected_parsed[key] != exp_val:
                raise AssertionError(
                    f"Field {key!r} mismatch: got {exp_val!r}, expected {expected_parsed[key]!r}"
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
        help="Optional path to cleanroom/ilnk-vector-check for C99 differential parity",
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
