# OpenLNK (`libopenlnk`)

Clean-room ISO C99 reimplementation, formal protocol specification, and multi-platform toolchain for the MS-DOS 5.00/6.00/6.22 `INTERLNK.EXE` / `INTERSVR.EXE` serial remote block-device protocol.

- **Zero dynamic allocation & zero floating-point** in `libopenlnk` (`cleanroom/src/`, `cleanroom/include/`) — strictly ISO C99 (`-std=c99 -Werror -pedantic`), 16-bit safe, statically buffered.
- **Full bidirectional interoperability** with original MS-DOS 6.22 `INTERLNK.EXE` and `INTERSVR.EXE` as well as all clean-room OpenLNK targets over a standard **3-wire RS-232 null-modem cable** (no hardware RTS/CTS required) at up to **115200 bps**.
- **Verified on real MS-DOS hardware** (Levels 1–4 of 5 passed across all 5 live desktop ↔ MS-DOS combinations; see [COMPATIBILITY.md](COMPATIBILITY.md)).

```text
+---------------------------------------------------------------------------------------+
|                                 libopenlnk (C99 Core)                                 |
|                -std=c99 | Zero malloc | Zero floating-point | 16-bit safe             |
+---------------------------------------------------------------------------------------+
          ^                                  ^                                  ^
          |                                  |                                  |
+-------------------+              +-------------------+              +-----------------+
|  Native Desktop   |              |  MS-DOS 16-Bit    |              |   ESP32 Port    |
|  opensvr / openlnk|              |  Open Watcom v2   |              | ESP-IDF / Pio / |
|  Linux/macOS/Win  |              |  OPENLNK.SYS      |              | Arduino         |
|  V-FAT16 & .vhd   |              |  OPENSVR.EXE      |              | Server + /dos   |
|  Serial & 86Box   |              |  OPENLNK.EXE      |              | FatFs VFS Client|
+-------------------+              +-------------------+              +-----------------+
```

## Supported Platforms & Verification Status

| Platform | Targets & Roles | Transport / Storage | Real-Hardware & CI Status |
| :--- | :--- | :--- | :--- |
| **macOS / Linux Desktop** | `opensvr` (Server), `openlnk` (Client) | POSIX serial (`--port`), 86Box Named Pipe (`--pipe`, `--peer-pipe`); Virtual FAT16 host folder (`--dir`), raw `.img` & Fixed-Size `.vhd` | **Verified** on real MS-DOS PC (PL2303 USB-RS232 @ 115200 bps) & 86Box; CI GCC/Clang + ASan/UBSan |
| **Windows Desktop** | `libopenlnk`, `opensvr`, `openlnk` | MSVC (`/W4 /WX`) library build, unit tests & `--self-test` *(live POSIX `--port`/`--pipe` CLI requires Linux/macOS)* | **CI Configured** (Windows x64 MSVC build, CTest & `--self-test`) |
| **16-Bit Real-Mode MS-DOS 5.0/6.22** | [`OPENLNK.SYS`, `OPENSVR.EXE`, `OPENLNK.EXE`](cleanroom/dos/README.md) | Direct `8250`/`16450`/`16550A` UART (`COM1`–`COM4`), `INT 25h`/`26h` + `INT 21h 440Dh` IOCTL BPB (hard disks, floppies, RAMDrives) | **Verified** on real MS-DOS PC @ 115200 bps (`OPENLNK.SYS` T1–T7, `OPENSVR.EXE`, `OPENLNK.EXE`), 86Box & 164/164 CTest |
| **ESP32 Embedded** | [Server & Client + `/dos` VFS](esp32/README.md) | 3.3V MAX3232 UART (`1200`–`115200` bps); Raw SDMMC/SDSPI, `.img`/`.vhd` VFS, and FatFs `/dos` POSIX mount | **CI Verified** (ESP-IDF 5.3.2 `esp32`/`esp32c3`, 262/262 CTest); Gate 5 PC/XT soak test **Pending** |

Full stage-by-stage verification evidence (`TC_LINK_01` through `TC_PLAT_01`) and Golden Trace parity results are tracked in [COMPATIBILITY.md](COMPATIBILITY.md).

---

## Real MS-DOS Hardware Quickstart

### Wiring Prerequisites

- **Modern host:** Any USB-to-RS232 adapter (e.g. PL2303 or FTDI; `/dev/cu.usbserial-*` on macOS, `/dev/ttyUSB0` on Linux).
- **Cable:** Standard **3-wire RS-232 null-modem cable** (`TXD ↔ RXD`, `RXD ↔ TXD`, `GND ↔ GND`). Hardware RTS/CTS flow control is **not required** — OpenLNK negotiates byte pacing in-band.
- **Speed:** Both sides always establish Phase A sync at **9600 bps (`8N1`)**, then automatically step up (`9600 → 19200 → 38400 → 57600 → 115200` bps) with a 15-pair line quality check. On vintage 8250/16450 UARTs without a 16550A FIFO, cap the ceiling with `--baud 9600` / `/BAUD:9600` (or `19200`).

> Prebuilt binaries for macOS, Linux, Windows, 16-bit MS-DOS (`openlnk-vX.Y.Z-dos16.zip`), and ESP32 Arduino/PlatformIO (`openlnk-vX.Y.Z-arduino-esp32.zip`) are published on [GitHub Releases](https://github.com/joberthel/openlnk/releases). To build from source first, see [Build, Tests, and CI](#build-tests-and-ci).

### Scenario A: Modern OS as Server ↔ MS-DOS Client (`OPENLNK.SYS` or `INTERLNK.EXE`)

Share a host directory directly as a Read-Write Virtual FAT16 drive (with live host change detection via `MEDIA_CHECK=0xFF`), or serve a raw FAT12/FAT16 `.img` / Fixed-Size `.vhd` image:

```bash
# On macOS/Linux — share a host folder over USB-RS232 (add --read-only to write-protect):
./cleanroom/build/opensvr --port /dev/cu.usbserial-2140 --dir ~/dos_share --label DOSSHARE -v

# Or serve a raw FAT12/FAT16 disk image or Fixed-Size VHD:
./cleanroom/build/opensvr --port /dev/ttyUSB0 --image dos_data.vhd -v
```

On the MS-DOS PC, load the clean-room `OPENLNK.SYS` block device driver in `CONFIG.SYS` (or use original MS-DOS `DEVICE=C:\DOS\INTERLNK.EXE /COM1`):

```dosini
DEVICE=C:\OPENLNK\OPENLNK.SYS /COM1 /BAUD:115200 /DRIVES:4
```

Reboot MS-DOS, then access the remote share as a native drive letter (e.g. `D:`):

```bat
DIR D:
COPY C:\AUTOEXEC.BAT D:\
FC /B C:\AUTOEXEC.BAT D:\AUTOEXEC.BAT
```

### Scenario B: MS-DOS as Server (`OPENSVR.EXE` or `INTERSVR.EXE`) ↔ Modern OS Client (`openlnk`)

Start the 16-bit server on the MS-DOS PC (or run original MS-DOS `INTERSVR C: /COM1`):

```bat
OPENSVR C: /COM1 /BAUD:115200
```

From macOS/Linux, query drives, browse FAT12/FAT16 directories, copy files, dump raw partitions, or open an interactive session with automatic keepalives:

```bash
# Inspect remote drives, BPBs, and Generic IOCTL Media IDs:
./cleanroom/build/openlnk --port /dev/cu.usbserial-2140 info

# List root or subdirectory on server drive C:
./cleanroom/build/openlnk --port /dev/cu.usbserial-2140 --unit C ls
./cleanroom/build/openlnk --port /dev/cu.usbserial-2140 --unit C ls DOS

# Print or download a remote file:
./cleanroom/build/openlnk --port /dev/cu.usbserial-2140 --unit C cat AUTOEXEC.BAT
./cleanroom/build/openlnk --port /dev/cu.usbserial-2140 --unit C get CONFIG.SYS -o config.sys

# Dump raw partition sectors to a local image file:
./cleanroom/build/openlnk --port /dev/cu.usbserial-2140 --unit C dump -o dos_c.img

# Interactive shell (keeps link alive automatically while idle):
./cleanroom/build/openlnk --port /dev/cu.usbserial-2140 --unit C shell
```

### Scenario C: Standalone MS-DOS Client CLI (`OPENLNK.EXE`) — No `CONFIG.SYS` Reboot

Use `OPENLNK.EXE` from the DOS prompt to query remote drives, list root directories, or download files from `opensvr` (or `OPENSVR.EXE` / `INTERSVR.EXE`) without installing a driver or rebooting:

```bat
OPENLNK /COM1
OPENLNK /COM1 /DIR
OPENLNK /COM1 /GET README.TXT
OPENLNK /COM1 /GET TOOLS.ZIP C:\TOOLS.ZIP C:
```

---

## Desktop CLI Quick Reference

### `opensvr` (Server CLI)

```text
opensvr (--port <tty/pty> | --pipe <prefix>) (--dir <folder> | --image <path> | --drive <L>:<path>) [options]
```

| Flag | Default | Description |
| :--- | :--- | :--- |
| `--port <tty/pty>` | — | POSIX raw `8N1` serial device (Linux/macOS; clears `CRTSCTS` & `IGNPAR`) |
| `--pipe <prefix>` | — | 86Box Named Pipe FIFOs (`<prefix>.in` / `<prefix>.out`, auto-created) |
| `--dir <folder>` | — | Share host folder as Virtual FAT16 drive (RW + live `MEDIA_CHECK=0xFF` refresh; auto unit) |
| `--image <path>` | — | Attach raw FAT12/FAT16 `.img` or Fixed-Size `.vhd` image (auto MBR/VHD detection; auto unit) |
| `--drive <L>:<path>` | — | Map image or host folder (auto-detected) to explicit server drive letter `A`–`Z` |
| `--label <name>` | Folder name / `OPENLNK_DIR` | Optional DOS volume label for `--dir` shares (up to 11 ASCII characters) |
| `--baud <rate>` | `115200` | Maximum negotiated baud (`1200`, `2400`, `4800`, `9600`, `19200`, `38400`, `57600`, `115200`) |
| `--read-only` | `false` | Write-protect all attached drives (returns DOS error `0x8100` `WRITE_PROTECT`) |
| `--burst` | `false` (Echo-Paced RX) | Allow Burst (no-echo) frame reception instead of default byte-by-byte Echo RX |
| `--pace-us <us>` | `0` | Per-byte TX delay in microseconds (for `--pipe` emulation) |
| `--idle-timeout <ms>` | `60000` | Drop idle high-baud link back to `9600` bps after `<ms>` (`0` disables idle drop) |
| `--verbose`, `-v` | `false` | Log sync, baud negotiation, session, and RPC events to `stderr` |
| `--self-test` | — | Run built-in in-memory loopback verification (Sync, 115200 baud, 16 KiB R/W, error codes) |
| `--version`, `--help` | — | Print version string or usage summary |

### `openlnk` (Client CLI)

```text
openlnk (--port <tty/pty> | --pipe <prefix> | --peer-pipe <prefix>) [options] <command> [args...]
```

| Flag | Default | Description |
| :--- | :--- | :--- |
| `--port <tty/pty>` | — | POSIX raw `8N1` serial device (Linux/macOS; clears `CRTSCTS` & `IGNPAR`) |
| `--pipe <prefix>` | — | Connect to 86Box Named Pipe FIFOs (86Box running `INTERSVR.EXE` / `OPENSVR.EXE`) |
| `--peer-pipe <prefix>` | — | Connect to host `opensvr --pipe <prefix>` (swaps `.in`/`.out` roles) |
| `--baud <rate>` | `115200` | Maximum negotiated baud (`1200`..`115200`) |
| `--unit <L\|N>` | First ready unit | Target server drive letter (`A`–`Z` or `A:`–`Z:`) or 0-based unit index (`0`–`25`) |
| `--retries <N>` | `25` | Phase A sync retry count |
| `--force-echo` | `false` | Force byte-by-byte Echo pacing on TX (helpful for XT/8250 without `/V`) |
| `--burst` | `false` (Echo-Paced RX) | Allow Burst (no-echo) RX from server |
| `--pace-us <us>` | `0` | Per-byte TX pacing delay in microseconds for pipe modes |
| `-o`, `--out <file>` | `stdout` (`get`) | Output file path for `get` (optional) and `dump` (required) |
| `--in <file>` | — | Input file path for `write-sector` (required; up to `8192` B) |
| `--lba <L>` / `--start <L>` | `0` | Starting LBA sector for `dump` and `write-sector` |
| `--count <N>` | `0` (full volume) | Number of sectors for `dump` |
| `--verify` | `false` | Use opcode `0x06` `WRITE_VERIFY` instead of `0x05` `WRITE_SECTORS` |
| `--self-test`, `--version`, `--help` | — | Run built-in client↔server loopback test, print version, or show usage |

| Subcommand | Description |
| :--- | :--- |
| `info` *(default)* | Connect and display baud, `server_id`, `max_block`, DOS version, mapped drives, BPB, and Media ID |
| `ls [subdir]` *(alias `dir`)* | List remote FAT12/FAT16 root directory or subdirectory |
| `cat <remote_path>` | Print remote FAT12/FAT16 file to `stdout` |
| `get <remote_path> [-o <file>]` | Download remote file (supports subdirectories; writes to `stdout` if `-o` omitted) |
| `dump -o <out.img> [--start L] [--count N]` | Dump raw partition sectors to a local image file |
| `write-sector --lba <L> --in <file> [--verify]` | Write raw sector(s) from `<file>` starting at `--lba` |
| `shell` | Interactive REPL with automatic 100 ms idle keepalives (`info`, `unit`, `ls`/`dir`, `cat`, `get`, `dump`, `ping`, `help`, `quit`) |

---

## 16-Bit Real-Mode MS-DOS Targets

> **Full Guide:** See **[cleanroom/dos/README.md](cleanroom/dos/README.md)** for installation, all `CONFIG.SYS` and CLI switches, RAMDrive/FAT12/FAT16 support, null-modem pinouts, and troubleshooting.

All three targets compile with **Open Watcom v2** (`-ms -0 -za99 -wx -we -os -zm -s -zl` + WASM `-0`) for `8086`/`8088` and `NEC V20`/`V30` CPUs with **100% Zero-CRT linkage (`0` `clibs.lib` modules)** at the default `MAX_PAYLOAD=8192`:

| Binary | Type | Disk Size | Conventional RAM | Role |
| :--- | :--- | :---: | :---: | :--- |
| `OPENLNK.SYS` | Block Device Driver (`CONFIG.SYS`) | `15,068 B` (`14.7 KiB`) | `26,656 B` (`26.0 KiB` resident, incl. 2 KiB private stack) | Redirects remote drives as native DOS drive letters; supports offline boot & lazy connect |
| `OPENSVR.EXE` | Standalone DOS MZ Server | `17,122 B` (`16.7 KiB`) | `31,744 B` (`31.0 KiB`, incl. 3 KiB stack) | Serves local DOS drives (hard disks, floppies, RAMDrives) over `COM1`–`COM4` |
| `OPENLNK.EXE` | Standalone DOS MZ Client CLI | `12,934 B` (`12.6 KiB`) | `24,816 B` (`24.2 KiB`, incl. 3 KiB stack) | One-shot drive info, `/DIR`, and `/GET` without modifying `CONFIG.SYS` |

**Offline Boot & Lazy-Connect (`CONFIG.SYS`):**

```dosini
DEVICE=C:\OPENLNK\OPENLNK.SYS /COM1 /BAUD:115200 /DRIVES:4
```

If the remote server is off when DOS boots, `OPENLNK.SYS` prints `[standby]`, reserves the configured drive letters with valid placeholder BPBs, and connects automatically on the first drive access (invalidating the placeholder BPB so DOS reads the live volume geometry).

**Reproducible Open Watcom v2 Docker Build & Verification:**

```bash
# Build the Open Watcom v2 container (--platform linux/amd64 needed on ARM64/Apple Silicon):
docker build --platform linux/amd64 -t openlnk-watcom -f tools/docker/Dockerfile.watcom .

# Cross-compile OPENSVR.EXE, OPENLNK.EXE, and OPENLNK.SYS into cleanroom/dos/build/:
docker run --rm --platform linux/amd64 -v "$PWD:/workspace" openlnk-watcom

# Verify MZ/.SYS headers, Zero-CRT linkage, exact disk == _sys_bss_start, and size budgets:
python3 tools/ci/verify_dos_binaries.py --build-dir cleanroom/dos/build
```

---

## ESP32 Embedded Port

OpenLNK provides a production-ready, zero-allocation C99 HAL and high-level Server/Client stack for **ESP32, ESP32-S2, ESP32-S3, ESP32-C3, and ESP32-C6** over a 3.3V MAX3232 RS-232 transceiver:

- **Server Mode (`esp32_server.c`):** Serves either a raw FAT12/FAT16 SD card via SDMMC (1/4-bit) / SDSPI (`esp32_hal_sd.c`) or `.img` / Fixed-Size `.vhd` files from a FAT32 SD card (`esp32_hal_img.c`) to MS-DOS `INTERLNK.EXE` / `OPENLNK.SYS`.
- **Client Mode + `/dos` VFS (`esp32_client.c`, `esp32_client_vfs.c`):** Thread-safe client wrapper with static FreeRTOS mutex, auto-reconnect, idle keepalive task, and FatFs POSIX VFS bridge (`/dos`) to mount remote `INTERSVR.EXE` / `OPENSVR.EXE` / `opensvr` drives via standard `fopen`/`fread`/`fwrite`/`opendir`.
- **Packaging:** Native ESP-IDF 5.x component, PlatformIO `library.json`, and standalone Arduino IDE / PlatformIO release ZIP (`esp32/tools/make_arduino_bundle.py`).

See **[esp32/README.md](esp32/README.md)** for wiring diagrams, GPIO pinouts, memory budgets, ESP-IDF/PlatformIO/Arduino quickstarts, and the Gate 5 (`TC_PLAT_01`) soak-test checklist.

---

## Build, Tests, and CI

### 1. Native CMake Build & CTest Suite

```bash
cmake -S cleanroom -B cleanroom/build -DCMAKE_BUILD_TYPE=Release
cmake --build cleanroom/build
ctest --test-dir cleanroom/build --output-on-failure
./cleanroom/build/opensvr --self-test
./cleanroom/build/openlnk --self-test
```

CTest runs three host test binaries:
- `test_libopenlnk` (`libopenlnk_unit_loopback`) — CRC-16/GENIBUS, wire codecs, Phase A/B/C state machine, echo/burst pacing, fragmentation, loopback & fault injection, high-baud reconnect recovery.
- `test_dos_hal` (`libopenlnk_dos_hal_16bit`) — **164/164** assertions covering simulated dual-16550A UART, `INT 25h`/`26h` + IOCTL BPB / RAMDrive disk HAL, and `OPENLNK.SYS` Request Header marshalling.
- `test_esp32_hal` (`libopenlnk_esp32_hal`) — **262/262** assertions covering mock UART/SD/VFS-IMG HALs, server engine, thread-safe client wrapper, and `/dos` FatFs VFS bridge.

### 2. Portable POSIX Make Fallback

```bash
make -C cleanroom test
```

Builds `libopenlnk.a`, `opensvr`, `openlnk`, `openlnk-vector-check`, and all three test binaries, then runs `test_libopenlnk`, `test_dos_hal`, `test_esp32_hal`, `opensvr --self-test`, and `openlnk --self-test`.

### 3. Formal Spec (Kaitai Struct) & Differential Test Vectors (`24/24`)

```bash
# With Kaitai Struct Compiler installed (required in CI):
kaitai-struct-compiler --target python --outdir tests/harness/parsers/ specs/interlnk_wire.ksy
python3 tests/harness/test_runner.py --require-kaitai --c-binary cleanroom/build/openlnk-vector-check

# Without Kaitai installed locally (reference parser + C99 cross-check):
python3 tests/harness/test_runner.py --c-binary cleanroom/build/openlnk-vector-check
```

*(If you built with `make -C cleanroom`, pass `--c-binary cleanroom/openlnk-vector-check`.)*

### 4. Python Tooling & Clean-Room Isolation Checks

```bash
python3 tools/ci/check_cleanroom_isolation.py
python3 -m unittest discover -s tests -p "test_*.py" -v   # 14/14 tests
```

### 5. 86Box Integration & Serial MITM Workflow

```bash
# Host-to-host smoke test via Named Pipe FIFOs:
./cleanroom/build/opensvr --pipe /tmp/openlnk_demo --dir ~/dos_share -v
./cleanroom/build/openlnk --peer-pipe /tmp/openlnk_demo info

# Launch 86Box with Named Pipe or PTY serial transport:
./tools/runner/run_86box.sh --help

# Bidirectional serial MITM capture with auto-created virtual PTYs (macOS/Linux):
python3 tools/sniffer/serial_mitm.py --create-ptys --baud 115200 --out-dir /tmp/traces --session-name capture
```

### 6. GitHub Actions CI Pipeline

`.github/workflows/ci.yml` enforces 6 automated jobs on every push and PR to `main`:
1. **Clean-Room Isolation & Lint** — `check_cleanroom_isolation.py`, shell syntax, JSON manifests, Python bytecode compilation.
2. **C99 Native Matrix** — Linux GCC, Linux Clang (+ Debug **ASan & UBSan**), macOS Clang, Windows MSVC (`/W4 /WX`).
3. **MS-DOS 16-Bit Real-Mode** — Open Watcom v2 cross-compile + `verify_dos_binaries.py`.
4. **Kaitai Struct & Differential Vectors** — `specs/interlnk_wire.ksy` (Python + C++11) + `24/24` `--require-kaitai` C99 parity.
5. **Python Tooling Tests** — `14/14` unit tests (`tests/test_tooling.py`).
6. **ESP32 Embedded (`espressif/idf:v5.3.2`)** — Zero-warning cross-compiles of `sd_server_idf` (Raw SD & VFS-IMG) and `dos_client_idf` for `esp32` and `esp32c3`.

---

## Clean-Room Architecture & Legal Compliance

To guarantee legal compliance under German/EU interoperability law (§ 69d Abs. 3, § 69e UrhG; EU Directive 2009/24/EG), the repository enforces a strict dual-agent boundary:

- **Agent A (`analysis/`) — Analysis / Dirty Room:** Operates on locally supplied original binaries and dynamic traces. Produces *only* declarative specifications ([specs/interlnk_wire.ksy](specs/interlnk_wire.ksy)), protocol documentation ([docs/protocol.md](docs/protocol.md)), sanitized Golden Traces ([tests/traces/README.md](tests/traces/README.md)), and JSON test vectors (`tests/fixtures/*.json`).
- **Agent B (`cleanroom/`, `esp32/`) — Implementation / Clean Room:** Never accesses `analysis/`, original binaries, or disassembly. Implements `libopenlnk`, desktop CLIs, 16-bit DOS targets, and ESP32 firmware solely from `specs/`, `docs/protocol.md`, and `tests/fixtures/`.

Install the local Git clean-room pre-commit hook before contributing:

```bash
./tools/ci/install_hooks.sh
```

See **[docs/LEGAL_COMPLIANCE.md](docs/LEGAL_COMPLIANCE.md)** for the full compliance charter and append-only audit log format (`audit_agent_a.log`, `audit_agent_b.log`).

## Repository Layout

| Path | Description |
| :--- | :--- |
| `analysis/` | Agent A workspace (protocol analysis scripts and verification tooling; isolated from Agent B) |
| `cleanroom/` | Agent B C99 core (`libopenlnk`), Desktop CLIs (`opensvr`, `openlnk`), 16-bit DOS targets ([`dos/README.md`](cleanroom/dos/README.md)), and host CTest suites |
| `docs/` | Verified wire & RPC specification ([`protocol.md`](docs/protocol.md)) and clean-room charter ([`LEGAL_COMPLIANCE.md`](docs/LEGAL_COMPLIANCE.md)) |
| `esp32/` | ESP-IDF 5.x, PlatformIO, and Arduino library, examples, and guide ([`esp32/README.md`](esp32/README.md)) |
| `specs/` | Authoritative Kaitai Struct wire specification ([`interlnk_wire.ksy`](specs/interlnk_wire.ksy)) |
| `tests/` | 24 JSON differential test vectors (`fixtures/`), Kaitai/C99 runner (`harness/`), Golden Traces ([`traces/README.md`](tests/traces/README.md)), and Python unit tests (`test_tooling.py`) |
| `tools/` | Serial MITM sniffer (`sniffer/`), 86Box runner (`runner/`), Open Watcom Dockerfile (`docker/`), and CI isolation/binary verifiers (`ci/`) |
| [COMPATIBILITY.md](COMPATIBILITY.md) | 5-stage Definition of Done verification matrix and hardware test evidence |
