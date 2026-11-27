# OpenLNK Compatibility & Verification Matrix (`COMPATIBILITY.md`)

This document tracks objective verification evidence against the 5-Stage Definition of Done (DoD) specified in TRD Section 7. Requirements are considered complete only when all 5 stages pass and reference verifiable CI runs or hardware test logs.

## 1. Definition of Done — 5-Stage Compatibility Matrix

| Stage | Designation | Test ID | Required Evidence | Status | Verified Date | Reference / Trace |
| :--- | :--- | :--- | :--- | :---: | :---: | :--- |
| **Level 1** | Physical Link | `TC_LINK_01` | Successful handshake over COM port at all baud rates (`9600`, `19200`, `38400`, `57600`, `115200` bps) | `VERIFIED (86Box + Real DOS HW)` | `2026-11-27` | `tests/traces/01_handshake_9600.hexlog.gz`, `02_handshake_115200.hexlog.gz`, `opensvr --pipe` (86Box) & `opensvr --port` (macOS PL2303 USB-RS232 + null-modem) ↔ real MS-DOS `INTERLNK.EXE` |
| **Level 2** | Discovery & Protocol | `TC_DISC_01` | Reliable enumeration of all remote drives (BPB/DPB/FAT12/FAT16) in MS-DOS 6.22 | `VERIFIED (86Box + Real DOS HW)` | `2026-11-27` | `tests/traces/04_bpb_query_fat16.hexlog.gz`, `opensvr --pipe` (86Box) & `opensvr --port --dir` (PL2303) ↔ real MS-DOS `INTERLNK.EXE` |
| **Level 3** | Raw Sector I/O | `TC_SEC_01` | Error-free reading and writing of 1,000 randomized sectors via Differential Oracle (86Box) | `VERIFIED (86Box + Real DOS HW)` | `2026-11-27` | `tests/traces/05_read_sector_single.hexlog.gz`, `06_write_sector_multi.hexlog.gz`, `opensvr --pipe` (86Box) & `opensvr --port` (PL2303, multi-sector burst) ↔ real MS-DOS `INTERLNK.EXE` |
| **Level 4** | High-Level DOS Workflows | `TC_DOS_01` | Successful execution of `DIR X:`, `COPY`, `FC /B`, `CHKDSK`, and `FORMAT` (V-FAT) | `VERIFIED (86Box + Real DOS HW)` | `2026-11-27` | `INTERLNK.EXE` ↔ `opensvr` (86Box VHD/`--dir` + real MS-DOS PC via PL2303: `DIR`, `TYPE`, `COPY`, Sokoban) AND `openlnk` ↔ `INTERSVR.EXE` live in 86Box |
| **Level 5** | Multi-Platform Parity | `TC_PLAT_01` | ESP32 serves FAT16 SD card over MAX3232 UART to real IBM PC/XT 5160 with zero transfer errors | `PENDING` | — | Hardware test log |

## 2. Golden Trace Capture & Verification Matrix (Agent A Ground Truth ↔ Agent B C99 Parity)

All 7 Golden Traces have been 100% deterministically decoded and verified (**774,590 / 774,590 bytes**, **923 / 923 frames**, **0 CRC-16/GENIBUS errors**, **4,127 keepalives verified**) and formalized into `specs/interlnk_wire.ksy`, `docs/protocol.md`, and 24 JSON test vectors in `tests/fixtures/*.json`. All 24/24 fixtures pass differential Kaitai ↔ C99 (`openlnk-vector-check`) parity.

| Trace File | Baud Rate | Description | Captured | Bytes / Frames Verified | Kaitai Verified | C99 Verified |
| :--- | :---: | :--- | :---: | :---: | :---: | :---: |
| `tests/traces/01_handshake_9600.hexlog.gz` | `9600` | Initial connect & sync (`AA/00 55/FF 5A/11`), burst vs echo pacing, `0x00`/`0x01`/`0x1C` | `[x]` | `902/902 B` (`7` frames) | `[x]` | `[x]` |
| `tests/traces/02_handshake_115200.hexlog.gz` | `115200` | Stepwise baud negotiation (`9600->115200`, 15-pair line test), `/V` echo pacing | `[x]` | `851/851 B` (`7` frames) | `[x]` | `[x]` |
| `tests/traces/03_rcopy_bootstrap.hexlog.gz` | `2400`/`38400` | `INTERSVR /RCOPY`: `CTTY COM1`, 784 B `FXB.COM`, SHA-256-exact `INTERSVR.EXE` & `INTERLNK.EXE`, `CTTY CON` | `[x]` | `61,828/61,828 B` (`34` frames) | `[x]` | `[x]` |
| `tests/traces/04_bpb_query_fat16.hexlog.gz` | `115200` | Drive enumeration, `DIR D:`, `CHKDSK D:` (FAT16 BPB, Generic IOCTL `08h/66h` MID, 43 reads) | `[x]` | `48,638/48,638 B` (`153` frames) | `[x]` | `[x]` |
| `tests/traces/05_read_sector_single.hexlog.gz` | `115200` | Sector reads (512 B, 16 KiB, 65,024 B / 127 sectors across 8x `0x40`-fragmented 8192 B frames) | `[x]` | `541,101/541,101 B` (`402` frames) | `[x]` | `[x]` |
| `tests/traces/06_write_sector_multi.hexlog.gz` | `115200` | Multi-sector writes (`ECHO`, 16 KiB `COPY` via 2x 8192 B fragmented frames, `FC /B`, `DEL`) | `[x]` | `92,177/92,177 B` (`203` frames) | `[x]` | `[x]` |
| `tests/traces/07_media_change_error.hexlog.gz` | `115200` | `A:` + `C:`, FAT12 BPB, `DISK_A->DISK_B` Media Change (`0xFF`), Write-Protect (`0x8100`), Not-Ready (`0x8102`), 2x keepalive timeout & session re-sync | `[x]` | `29,093/29,093 B` (`117` frames) | `[x]` | `[x]` |

## 3. Target Platform Build & Verification Status

| Platform Target | Toolchain | Architectural Constraints | Build Status | Test Status |
| :--- | :--- | :--- | :---: | :---: |
| Linux x86_64 Desktop | GCC / Clang (`-std=c99 -Werror -pedantic`) | Zero `malloc`, Zero FP | `CONFIGURED (CI)` | `READY (CI)` |
| macOS ARM64 Desktop | Apple Clang (`-std=c99 -Werror -pedantic -Wconversion`) | Zero `malloc`, Zero FP | `PASS` | `PASS (CTest, self-test, 24/24 parity, 86Box Oracle + Real DOS HW PL2303 RS-232)` |
| Windows x64 Desktop | MSVC (`/W4 /WX`) | Zero `malloc`, Zero FP | `CONFIGURED (CI, _WIN32 audited)` | `READY (CI)` |
| MS-DOS 16-Bit Real-Mode | Open Watcom v2 (`-ms -0 -za99 -wx -we -os -zm -s -zl` + WASM `-0`) | 8086/8088 / NEC V30 (`8250/16550A` + `INT 25h/26h` + `INT 21h`, 100% Zero-CRT) | `PASS (0 warnings: OPENSVR.EXE 15.5 KiB / 29.8 KiB RAM, OPENLNK.EXE 12.5 KiB / 24.1 KiB RAM, OPENLNK.SYS 14.6 KiB disk / 25.9 KiB resident RAM)` | `PASS (162/162 Sim-16550A + RH-Marshall + Zero-Stdio I/O CTest; 86Box Live DOS EXE Verified)` |
| ESP32 Embedded | ESP-IDF 5.3.2 (`xtensa-esp32`/`riscv32-esp32c3`/`xtensa-esp32s3`) / PlatformIO / Arduino | MAX3232 UART + SDMMC/SDSPI + `.img`/`.vhd` VFS (Zero `malloc`, Zero FP) | `PASS (0 warnings: esp32 283.6 KiB / 22.1 KiB DRAM, esp32c3 287.9 KiB, esp32s3 313.4 KiB, esp32 VFS-IMG 296.6 KiB)` | `PASS (262/262 Host Mock-HAL + E2E Loopback + VFS CTest); PENDING (Real PC/XT HW Soak)` |

## 4. Automated Verification Pipeline Gates

- [x] **Gate 1 — Clean-Room Isolation:** Pre-commit hook & CI script enforce Agent A / Agent B separation and zero-alloc / zero-FP invariants (`tools/ci/check_cleanroom_isolation.py` PASS).
- [x] **Gate 2 — Kaitai & JSON Vectors (24/24 Differential Parity):** Formal `specs/interlnk_wire.ksy` compiles cleanly (0 warnings) for Python & C++11 STL and passes **24/24** Kaitai ↔ C99 differential parity (`tests/harness/test_runner.py --require-kaitai --c-binary cleanroom/build/openlnk-vector-check`).
- [x] **Gate 3 — Loopback, Named-Pipe & Fault-Injection (Real Protocol):** `libopenlnk` (`cleanroom/`) implements verified CRC-16/GENIBUS (`0x1D0F`), Phase A/B/C link state machine, Echo vs Burst pacing, >8192 B fragmentation, FAT12/FAT16/MBR/VHD storage, and Zero-Alloc server engine; verified via `ctest`, `opensvr --self-test`, and live POSIX `--pipe` E2E test.
- [x] **Gate 4 — Bidirectional Differential Oracle (86Box + Real DOS Hardware):** Verified live end-to-end in **both** directions over 86Box Named Pipes (`--pipe`) @ 115200 bps (`INTERLNK.EXE` ↔ `opensvr` and `openlnk` ↔ `INTERSVR.EXE`), **and** live on real MS-DOS PC hardware over Prolific PL2303 USB-RS232 (`/dev/cu.usbserial-2140`) + null-modem cable (`opensvr --port --dir` ↔ MS-DOS `INTERLNK.EXE` @ 9600–115200 bps: `DIR`, `TYPE`, multi-file `COPY`, and running Sokoban directly from remote V-FAT16 share).
- [ ] **Gate 5 — Hardware Parity Sign-Off:** Real IBM PC/XT 5160 + ESP32 SD card soak test signed off.
