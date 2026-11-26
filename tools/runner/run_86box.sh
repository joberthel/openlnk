#!/usr/bin/env bash
# tools/runner/run_86box.sh
# Automated launcher for 86Box v4.x/v5.x MS-DOS 6.22 integration & oracle tests.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

MODE="client"
MACHINE_PROFILE="xt"
BAUD="115200"
SERIAL_TRANSPORT="pty"
SERIAL_CLIENT=""
SERIAL_SERVER=""
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

Launch 86Box for INTERLNK/INTERSVR ground-truth tracing or Differential Oracle
integration testing.

Options:
  --mode <client|server|dual|oracle>  Execution topology (default: client)
  --machine <xt|486>                  Hardware profile: xt (NEC V20/8250) or 486 (486DX2/16550A FIFO)
  --baud <rate>                       Target COM1 baud rate (default: 115200)
  --transport <pty|pipe>              COM1 backend: serial_passthrough (pty) or Named Pipe (pipe)
  --serial-client <device|prefix>     Host PTY path or Named Pipe prefix for Client COM1
  --serial-server <device|prefix>     Host PTY path or Named Pipe prefix for Server COM1
  --client-vhd <path>                 MS-DOS 6.22 Client VHD image path
  --server-vhd <path>                 MS-DOS 6.22 Server VHD image path
  --client-floppy <path>              Optional 1.44M floppy image for Client VM (A:)
  --server-floppy <path>              Optional 1.44M floppy image for Server VM (A:)
  --roms <dir>                        86Box ROM directory (or set EIGHTYSIXBOX_ROMS)
  --workdir <dir>                     Runtime working directory (default: build/86box_run)
  --gui                               Show 86Box GUI window (always used on macOS cocoa)
  --generate-config                   Only render runtime 86Box .cfg files and exit
  --dry-run                           Print planned commands without launching 86Box
  -h, --help                          Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --machine) MACHINE_PROFILE="$2"; shift 2 ;;
        --baud) BAUD="$2"; shift 2 ;;
        --transport) SERIAL_TRANSPORT="$2"; shift 2 ;;
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
    if [[ -n "${EIGHTYSIXBOX_BIN:-}" && -x "${EIGHTYSIXBOX_BIN}" ]]; then
        echo "${EIGHTYSIXBOX_BIN}"
    elif command -v 86Box >/dev/null 2>&1; then
        command -v 86Box
    elif [[ -x "${HOME}/Applications/86Box.app/Contents/MacOS/86Box" ]]; then
        echo "${HOME}/Applications/86Box.app/Contents/MacOS/86Box"
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
    cp "$template" "$output_cfg"

    if [[ -n "$floppy_path" ]]; then
        printf "fdd_01_fn = %s\n" "$floppy_path" >> "$output_cfg"
    fi

    if [[ -n "$vhd_path" ]]; then
        if [[ "$MACHINE_PROFILE" == "486" ]]; then
            cat >> "$output_cfg" <<EOF

[Hard disks]
hdd_01_parameters = 17, 4, 306, 0, ide
hdd_01_fn = ${vhd_path}
hdd_01_ide_channel = 0:0
EOF
        else
            cat >> "$output_cfg" <<EOF

[Hard disks]
hdd_01_parameters = 17, 4, 306, 0, xta
hdd_01_fn = ${vhd_path}
hdd_01_xta_channel = 0
EOF
        fi
    fi

    if [[ -n "$serial_dev" ]]; then
        if [[ "$SERIAL_TRANSPORT" == "pipe" ]]; then
            awk '
                /^serial1_enabled = 1$/ {
                    print "serial1_device = pipe"
                    next
                }
                { print }
            ' "$output_cfg" > "${output_cfg}.tmp" && mv "${output_cfg}.tmp" "$output_cfg"

            cat >> "$output_cfg" <<EOF

[Named Pipe (COM) #1]
path = ${serial_dev}
mode = 1
reconnect = 1
EOF
        else
            awk '
                /^serial1_enabled = 1$/ {
                    print "serial1_device = serial_passthrough"
                    next
                }
                { print }
            ' "$output_cfg" > "${output_cfg}.tmp" && mv "${output_cfg}.tmp" "$output_cfg"

            cat >> "$output_cfg" <<EOF

[Serial Passthrough (COM) #1]
mode = 3
path = ${serial_dev}
baudrate = ${BAUD}
data_bits = 8
stop_bits = 1
parity = 0
EOF
        fi
    fi
}

mkdir -p "${WORKDIR}/client" "${WORKDIR}/server"

if [[ "$MACHINE_PROFILE" == "486" ]]; then
    CLIENT_TEMPLATE="${SCRIPT_DIR}/configs/client_486.cfg"
    SERVER_TEMPLATE="${SCRIPT_DIR}/configs/server_486.cfg"
elif [[ "$MACHINE_PROFILE" == "xt" ]]; then
    CLIENT_TEMPLATE="${SCRIPT_DIR}/configs/client_86box.cfg"
    SERVER_TEMPLATE="${SCRIPT_DIR}/configs/server_86box.cfg"
else
    echo "[run_86box] ERROR: Unknown --machine '${MACHINE_PROFILE}'. Expected xt|486." >&2
    exit 2
fi

CLIENT_CFG="${WORKDIR}/client/86box.cfg"
SERVER_CFG="${WORKDIR}/server/86box.cfg"

render_config "$CLIENT_TEMPLATE" "$CLIENT_CFG" "$SERIAL_CLIENT" "$CLIENT_VHD" "$CLIENT_FLOPPY"
render_config "$SERVER_TEMPLATE" "$SERVER_CFG" "$SERIAL_SERVER" "$SERVER_VHD" "$SERVER_FLOPPY"

echo "[run_86box] Mode=${MODE} Machine=${MACHINE_PROFILE} Baud=${BAUD} Workdir=${WORKDIR}"
echo "[run_86box] Rendered Client CFG: ${CLIENT_CFG} (serial1=${SERIAL_CLIENT:-none})"
echo "[run_86box] Rendered Server CFG: ${SERVER_CFG} (serial1=${SERIAL_SERVER:-none})"

if [[ "$GENERATE_CONFIG_ONLY" -eq 1 ]]; then
    exit 0
fi

EIGHTYSIXBOX_BIN="$(find_86box)"
if [[ -z "$EIGHTYSIXBOX_BIN" ]]; then
    if [[ "$DRY_RUN" -eq 1 ]]; then
        EIGHTYSIXBOX_BIN="86Box"
    else
        echo "[run_86box] ERROR: 86Box binary not found in PATH, ~/Applications/86Box.app, or /Applications/86Box.app." >&2
        echo "[run_86box] Install 86Box or run with --dry-run / --generate-config." >&2
        exit 1
    fi
fi

EXTRA_ARGS=()
if [[ -n "$ROM_PATH" ]]; then
    EXTRA_ARGS+=("-R" "$ROM_PATH")
fi

launch_vm() {
    local role="$1"
    local vm_dir="$2"
    local cmd=("$EIGHTYSIXBOX_BIN" "-N" "-P" "$vm_dir" "-V" "OpenLNK-${role}" ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

    if [[ "$HEADLESS" -eq 1 && "$(uname -s)" != "Darwin" ]]; then
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
        launch_vm "CLIENT" "${WORKDIR}/client"
        ;;
    server)
        launch_vm "SERVER" "${WORKDIR}/server"
        ;;
    dual)
        launch_vm "SERVER" "${WORKDIR}/server"
        launch_vm "CLIENT" "${WORKDIR}/client"
        ;;
    oracle)
        echo "[run_86box] Differential Oracle mode: MS-DOS Client VM <-> cleanroom opensvr"
        launch_vm "CLIENT" "${WORKDIR}/client"
        echo "[run_86box] Connect opensvr to peer PTY of ${SERIAL_CLIENT:-<pty>} at ${BAUD} bps."
        ;;
    *)
        echo "[run_86box] ERROR: Unknown mode '${MODE}'. Expected client|server|dual|oracle." >&2
        exit 2
        ;;
esac

if [[ "$DRY_RUN" -eq 0 ]]; then
    wait
fi
