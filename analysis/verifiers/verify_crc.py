#!/usr/bin/env python3
"""Agent A Deterministic Checksum & Algorithm Verifier (Anti-Hallucination Guardrail).

Per TRD Section 4, Agent A must never compute checksums or hypothesize frame
algorithms inside an LLM context. All candidate algorithms are coded here and
verified deterministically against captured raw bytes or Golden Traces.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Callable, Optional


def crc16_ccitt_false(payload: bytes, init: int = 0xFFFF, poly: int = 0x1021) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, unreflected, xorout 0x0000)."""
    crc = init & 0xFFFF
    for byte in payload:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ poly) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def crc16_ibm(payload: bytes, init: int = 0xFFFF, poly: int = 0xA001) -> int:
    """Reflected CRC-16/MODBUS-IBM (poly 0x8005 reflected as 0xA001)."""
    crc = init & 0xFFFF
    for byte in payload:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = ((crc >> 1) ^ poly) & 0xFFFF
            else:
                crc >>= 1
    return crc


def checksum_sum16(payload: bytes) -> int:
    """16-bit wrapping additive byte sum."""
    return sum(payload) & 0xFFFF


def checksum_xor16(payload: bytes) -> int:
    """16-bit little-endian word XOR accumulator."""
    acc = 0
    for idx in range(0, len(payload), 2):
        lo = payload[idx]
        hi = payload[idx + 1] if idx + 1 < len(payload) else 0
        acc ^= (lo | (hi << 8))
    return acc & 0xFFFF


def custom_interlnk_crc16(raw_payload: bytes) -> int:
    """Active reference checksum function for INTERLNK wire verification."""
    return crc16_ccitt_false(raw_payload)


CANDIDATE_ALGORITHMS: dict[str, Callable[[bytes], int]] = {
    "crc16_ccitt_false": crc16_ccitt_false,
    "crc16_ibm": crc16_ibm,
    "sum16": checksum_sum16,
    "xor16": checksum_xor16,
}


def check_hypothesis(
    raw_payload: bytes,
    expected_crc: int,
    algorithm: str = "crc16_ccitt_false",
) -> bool:
    """Deterministically verify whether a candidate checksum matches expected_crc."""
    func = CANDIDATE_ALGORITHMS.get(algorithm, custom_interlnk_crc16)
    calculated = func(raw_payload)
    return calculated == (expected_crc & 0xFFFF)


def probe_all_algorithms(raw_payload: bytes, expected_crc: int) -> list[str]:
    """Return every candidate algorithm matching expected_crc for raw_payload."""
    return [
        name
        for name, func in CANDIDATE_ALGORITHMS.items()
        if func(raw_payload) == (expected_crc & 0xFFFF)
    ]


HEXLOG_RE = re.compile(r"^\[(\d+)\]\s+\[([^\]]+)\]\s+([0-9a-fA-F]+)\s*$")


def verify_hexlog_frames(hexlog_path: Path, algorithm: str = "crc16_ccitt_false") -> tuple[int, int]:
    """Scan a .hexlog file for 0x55 0xAA framed packets and verify checksums."""
    passed = 0
    failed = 0
    for line in hexlog_path.read_text(encoding="utf-8").splitlines():
        m = HEXLOG_RE.match(line.strip())
        if not m:
            continue
        raw = bytes.fromhex(m.group(3))
        if len(raw) >= 8 and raw[0] == 0x55 and raw[1] == 0xAA:
            payload_len = int.from_bytes(raw[4:6], "little")
            total = 6 + payload_len + 2
            if len(raw) >= total:
                body = raw[2 : 6 + payload_len]
                expected = int.from_bytes(raw[6 + payload_len : total], "little")
                if check_hypothesis(body, expected, algorithm=algorithm):
                    passed += 1
                else:
                    failed += 1
    return passed, failed


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Deterministic checksum hypothesis verifier for Agent A"
    )
    parser.add_argument("--payload-hex", help="Hex bytes to checksum")
    parser.add_argument("--expected", help="Expected 16-bit checksum (hex 0x.... or decimal)")
    parser.add_argument(
        "--algorithm",
        default="crc16_ccitt_false",
        choices=sorted(CANDIDATE_ALGORITHMS.keys()),
    )
    parser.add_argument("--hexlog", type=Path, help="Verify framed packets in a .hexlog trace")
    args = parser.parse_args(argv)

    if args.hexlog:
        ok, bad = verify_hexlog_frames(args.hexlog, algorithm=args.algorithm)
        print(f"[verify_crc] {args.hexlog}: passed={ok}, failed={bad} ({args.algorithm})")
        return 0 if bad == 0 else 1

    if not args.payload_hex:
        parser.error("Provide either --payload-hex or --hexlog.")

    payload = bytes.fromhex(args.payload_hex)
    if args.expected is not None:
        expected = int(args.expected, 0)
        matched = check_hypothesis(payload, expected, algorithm=args.algorithm)
        all_matches = probe_all_algorithms(payload, expected)
        print(
            f"[verify_crc] algorithm={args.algorithm} expected=0x{expected:04X} "
            f"match={matched} all_matches={all_matches}"
        )
        return 0 if matched else 1

    calc = CANDIDATE_ALGORITHMS[args.algorithm](payload)
    print(f"0x{calc:04X} ({calc})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
