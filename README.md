# OpenLNK (`libinterlnk`)

Clean-room reverse engineering, formal protocol specification, and portable C99 reimplementation of the MS-DOS 5.00/6.00/6.22 `INTERLNK.EXE` / `INTERSVR.EXE` serial/parallel transport and remote block-device protocol.

## Architecture Overview

```
+-----------------------------------------------------------------------------------+
|                                 libinterlnk (C99 Core)                            |
|               -std=c99 | Zero malloc | Zero floating-point | 16-bit safe          |
+-----------------------------------------------------------------------------------+
       ^                                 ^                                 ^
       |                                 |                                 |
+--------------+                 +---------------+                 +----------------+
|  ESP32 Port  |                 | Native Desktop|                 | MS-DOS 16-Bit  |
| ESP-IDF / SD |                 | Linux/macOS/Win               | Open Watcom v2 |
+--------------+                 +---------------+                 +----------------+
```

## Dual-Agent Clean-Room Isolation

To guarantee strict legal compliance under German/EU interoperability law (§ 69d Abs. 3, § 69e UrhG; EU Directive 2009/24/EG), this repository enforces a hard boundary between:

- **Agent A (`analysis/`) — Analysis / Dirty Room:** Works with locally supplied original binaries, Ghidra 12.x + MCP, and dynamic serial traces from 86Box. Outputs *only* formal specifications (`specs/interlnk_wire.ksy`), protocol documentation (`docs/protocol.md`), and test vectors (`tests/fixtures/*.json`).
- **Agent B (`cleanroom/`, `esp32/`) — Implementation / Clean Room:** Never accesses `analysis/`, original binaries, or disassembly. Implements `libinterlnk` exclusively from `specs/` and `tests/fixtures/`.

See [docs/LEGAL_COMPLIANCE.md](docs/LEGAL_COMPLIANCE.md) for the full compliance charter and audit logging rules (`audit_agent_a.log`, `audit_agent_b.log`).

## Repository Layout

| Directory | Purpose |
| :--- | :--- |
| `analysis/` | Agent A workspace: Ghidra headless setup, DOS driver scripts, deterministic verifiers (`verify_crc.py`), raw traces |
| `cleanroom/` | Agent B workspace: allocation-free C99 `libinterlnk`, Desktop CLI tools, 16-bit DOS Open Watcom targets, C unit tests |
| `docs/` | Protocol specification (`protocol.md`) and legal clean-room charter (`LEGAL_COMPLIANCE.md`) |
| `esp32/` | ESP-IDF 5.x, PlatformIO, and Arduino component manifests and UART/SDMMC HAL |
| `specs/` | Machine-readable Kaitai Struct wire protocol specification (`interlnk_wire.ksy`) |
| `tests/` | JSON test vectors (`fixtures/`), Kaitai/C99 verification harness (`harness/`), Golden Traces (`traces/`) |
| `tools/` | Bidirectional serial MITM sniffer (`sniffer/`), 86Box runner (`runner/`), CI & clean-room isolation checks (`ci/`) |

## Quickstart

### 1. Install Git Clean-Room Isolation Hooks
```bash
./tools/ci/install_hooks.sh
```

### 2. Build & Test `libinterlnk` (C99 Core)
Using CMake:
```bash
cmake -S cleanroom -B cleanroom/build
cmake --build cleanroom/build
ctest --test-dir cleanroom/build --output-on-failure
```
Or using plain Make (`clang`/`gcc`):
```bash
make -C cleanroom test
```

### 3. Validate Formal Spec & Test Vectors
```bash
# With Kaitai Struct Compiler (recommended / required in CI):
kaitai-struct-compiler --target python --outdir tests/harness/parsers/ specs/interlnk_wire.ksy
python3 tests/harness/test_runner.py --require-kaitai --c-binary cleanroom/ilnk-vector-check

# Without Kaitai installed locally (reference parser + C99 cross-check):
python3 tests/harness/test_runner.py --c-binary cleanroom/ilnk-vector-check
```

### 4. Dynamic Ground-Truth Capture (86Box + Serial MITM)
```bash
# Launch serial MITM proxy with auto-created virtual PTYs (macOS/Linux):
python3 tools/sniffer/serial_mitm.py --create-ptys --baud 115200 --out-dir analysis/traces --session-name 02_handshake_115200

# Or orchestrate headless 86Box VMs:
./tools/runner/run_86box.sh --help
```

### 5. Static Analysis Environment (Agent A Only)
```bash
python3 analysis/setup_ghidra.py --check
```

## Verification & Compatibility

Progress against the 5-stage Definition of Done (`TC_LINK_01` through `TC_PLAT_01`) is tracked in [COMPATIBILITY.md](COMPATIBILITY.md).
