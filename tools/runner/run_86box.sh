#!/usr/bin/env bash
# tools/runner/run_86box.sh
# Automated headless launcher for 86Box v4.x MS-DOS 6.22 integration & oracle tests.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

MODE="client"
BAUD="115200"
SERIAL_CLIENT="/dev/ttyS0"
SERIAL_SERVER="/dev/ttyS1"
WORKDIR="${REPO_ROOT}/build/86box_run"
CLIENT_VHD=""
SERVER_VHD=""
CLIENT_FLOPPY=""
SERVER_FLOPPY=""
ROM_PATH="${EIGHTYSIXBOX_ROMS:-}"
DRY_RUN=0
GENERATE_CONFIG_ONLY=0
HEADLESS=1

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Launch 86Box v4.x in headless mode for INTERLNK/INTERSVR ground-truth tracing
or Differential Oracle integration testing.

Options:
  --mode <client|server|dual|oracle>  Execution topology (default: client)
  --baud <rate>                       Target COM1 baud rate (default: 115200)
  --serial-client <device>            Serial device/PTY for Client VM (default: /dev/ttyS0)
  --serial-server <device>            Serial device/PTY for Server VM (default: /dev/ttyS1)
  --client-vhd <path>                 MS-DOS 6.22 Client VHD image path
  --server-vhd <path>                 MS-DOS 6.22 Server VHD image path
  --client-floppy <path>              Optional 1.44M floppy image for Client VM (A:)
  --server-floppy <path>              Optional 1.44M floppy image for Server VM (A:)
  --roms <dir>                        86Box ROM directory (or set EIGHTYSIXBOX_ROMS)
  --workdir <dir>                     Runtime working directory (default: build/86box_run)
  --gui                               Disable headless flags (show 86Box window)
  --generate-config                   Only render runtime 86Box .cfg files and exit
  --dry-run                           Print planned commands without launching 86Box
  -h, --help                          Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --baud) BAUD="$2"; shift 2 ;;
        --serial-client) SERIAL_CLIENT="$2"; shift 2 ;;
        --serial-server) SERIAL_SERVER="$2"; shift 2 ;;
        --client-vhd) CLIENT_VHD="$2"; shift 2 ;;
        --server-vhd) SERVER_VHD="$2"; shift 2 ;;
        --client-floppy) CLIENT_FLOPPY="$2"; shift 2 ;;
        --server-floppy) SERVER_FLOPPY="$2"; shift 2 ;;
        --roms) ROM_PATH="$2"; shift 2 ;;
        --workdir) WORKDIR="$2"; shift 2 ;;
        --gui) HEADLESS=0; shift ;;
        --generate-config) GENERATE_CONFIG_ONLY=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

find_86box() {
    if command -v 86Box >/dev/null 2>&1; then
        command -v 86Box
    elif [[ -x "/Applications/86Box.app/Contents/MacOS/86Box" ]]; then
        echo "/Applications/86Box.app/Contents/MacOS/86Box"
    elif command -v 86box >/dev/null 2>&1; then
        command -v 86box
    else
        echo ""
    fi
}

render_config() {
    local template="$1"
    local output_cfg="$2"
    local serial_dev="$3"
    local vhd_path="$4"
    local floppy_path="$5"

    mkdir -p "$(dirname "$output_cfg")"
    sed "s|^serial1_device = .*|serial1_device = ${serial_dev}|" "$template" > "$output_cfg"

    if [[ -n "$floppy_path" ]]; then
        printf "fdd_01_fn = %s\n" "$floppy_path" >> "$output_cfg"
    fi

    if [[ -n "$vhd_path" ]]; then
        cat >> "$output_cfg" <<EOF

[Hard disks]
hdd_01_parameters = 17, 4, 306, 0, xta
hdd_01_fn = ${vhd_path}
hdd_01_speed = ramdisk
hdd_01_xta_channel = 0:0
EOF
    fi
}

mkdir -p "${WORKDIR}/client" "${WORKDIR}/server"

CLIENT_CFG="${WORKDIR}/client/86box.cfg"
SERVER_CFG="${WORKDIR}/server/86box.cfg"

render_config "${SCRIPT_DIR}/configs/client_86box.cfg" "$CLIENT_CFG" "$SERIAL_CLIENT" "$CLIENT_VHD" "$CLIENT_FLOPPY"
render_config "${SCRIPT_DIR}/configs/server_86box.cfg" "$SERVER_CFG" "$SERIAL_SERVER" "$SERVER_VHD" "$SERVER_FLOPPY"

echo "[run_86box] Mode=${MODE} Baud=${BAUD} Workdir=${WORKDIR}"
echo "[run_86box] Rendered Client CFG: ${CLIENT_CFG} (serial1=${SERIAL_CLIENT})"
echo "[run_86box] Rendered Server CFG: ${SERVER_CFG} (serial1=${SERIAL_SERVER})"

if [[ "$GENERATE_CONFIG_ONLY" -eq 1 ]]; then
    exit 0
fi

EIGHTYSIXBOX_BIN="$(find_86box)"
if [[ -z "$EIGHTYSIXBOX_BIN" ]]; then
    if [[ "$DRY_RUN" -eq 1 ]]; then
        EIGHTYSIXBOX_BIN="86Box"
    else
        echo "[run_86box] ERROR: 86Box binary not found in PATH or /Applications/86Box.app." >&2
        echo "[run_86box] Install 86Box v4.x or run with --dry-run / --generate-config." >&2
        exit 1
    fi
fi

EXTRA_ARGS=()
if [[ -n "$ROM_PATH" ]]; then
    EXTRA_ARGS+=("--rompath" "$ROM_PATH")
fi

launch_vm() {
    local role="$1"
    local vm_dir="$2"
    local vm_cfg="$3"
    local cmd=("$EIGHTYSIXBOX_BIN" "--vmpath" "$vm_dir" "--config" "$vm_cfg" ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

    if [[ "$HEADLESS" -eq 1 ]]; then
        echo "[run_86box] Launching ${role} VM (headless): QT_QPA_PLATFORM=offscreen ${cmd[*]}"
        if [[ "$DRY_RUN" -eq 0 ]]; then
            QT_QPA_PLATFORM=offscreen "${cmd[@]}" &
        fi
    else
        echo "[run_86box] Launching ${role} VM (GUI): ${cmd[*]}"
        if [[ "$DRY_RUN" -eq 0 ]]; then
            "${cmd[@]}" &
        fi
    fi
}

case "$MODE" in
    client)
        launch_vm "CLIENT" "${WORKDIR}/client" "$CLIENT_CFG"
        ;;
    server)
        launch_vm "SERVER" "${WORKDIR}/server" "$SERVER_CFG"
        ;;
    dual)
        launch_vm "SERVER" "${WORKDIR}/server" "$SERVER_CFG"
        launch_vm "CLIENT" "${WORKDIR}/client" "$CLIENT_CFG"
        ;;
    oracle)
        echo "[run_86box] Differential Oracle mode: MS-DOS Client VM <-> cleanroom opensvr"
        launch_vm "CLIENT" "${WORKDIR}/client" "$CLIENT_CFG"
        echo "[run_86box] Connect opensvr to peer PTY of ${SERIAL_CLIENT} at ${BAUD} bps."
        ;;
    *)
        echo "[run_86box] ERROR: Unknown mode '${MODE}'. Expected client|server|dual|oracle." >&2
        exit 2
        ;;
esac

if [[ "$DRY_RUN" -eq 0 ]]; then
    wait
fi
