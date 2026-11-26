#!/usr/bin/env bash
# tools/runner/capture_golden_traces.sh
# Orchestrates the 7 Golden Trace capture sessions defined in the TRD.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUT_DIR="${REPO_ROOT}/tests/traces"

SCENARIOS=(
    "01_handshake_9600:9600:Initial INTERLNK/INTERSVR connect handshake at 9600 bps"
    "02_handshake_115200:115200:Initial connect handshake at 115200 bps (with /V check)"
    "03_rcopy_bootstrap:9600:INTERSVR /RCOPY bootstrap transfer over COM1"
    "04_bpb_query_fat16:115200:Drive enumeration, BPB/DPB query on FAT16 volume"
    "05_read_sector_single:115200:Sector read transactions (1B, 511B, 512B, 4096B, 64KB+)"
    "06_write_sector_multi:115200:Multi-sector write transactions and integrity verify"
    "07_media_change_error:115200:Media check (0x01), write-protect, and disk-full errors"
)

if [[ "${1:-}" == "--list" || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    echo "Golden Trace Test Matrix (target dir: ${OUT_DIR}):"
    for item in "${SCENARIOS[@]}"; do
        IFS=":" read -r name baud desc <<< "$item"
        printf "  %-26s [%6s bps]  %s\n" "${name}.hexlog" "$baud" "$desc"
    done
    exit 0
fi

SCENARIO_FILTER="${1:-01_handshake_9600}"
DRY_RUN="${2:-}"

for item in "${SCENARIOS[@]}"; do
    IFS=":" read -r name baud desc <<< "$item"
    if [[ "$name" == "$SCENARIO_FILTER" ]]; then
        echo "[golden-trace] Scenario : ${name}"
        echo "[golden-trace] Baud     : ${baud}"
        echo "[golden-trace] Purpose  : ${desc}"
        echo "[golden-trace] Command  : python3 ${REPO_ROOT}/tools/sniffer/serial_mitm.py --create-ptys --baud ${baud} --out-dir ${OUT_DIR} --session-name ${name}"
        if [[ "$DRY_RUN" != "--dry-run" ]]; then
            exec python3 "${REPO_ROOT}/tools/sniffer/serial_mitm.py" \
                --create-ptys \
                --baud "$baud" \
                --out-dir "$OUT_DIR" \
                --session-name "$name"
        fi
        exit 0
    fi
done

echo "[golden-trace] Unknown scenario '${SCENARIO_FILTER}'. Run with --list." >&2
exit 2
