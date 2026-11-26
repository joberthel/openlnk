# OpenLNK Compatibility & Verification Matrix (`COMPATIBILITY.md`)

This document tracks objective verification evidence against the 5-Stage Definition of Done (DoD) specified in TRD Section 7. Requirements are considered complete only when all 5 stages pass and reference verifiable CI runs or hardware test logs.

## 1. Definition of Done — 5-Stage Compatibility Matrix

| Stage | Designation | Test ID | Required Evidence | Status | Verified Date | Reference / Trace |
| :--- | :--- | :--- | :--- | :---: | :---: | :--- |
| **Level 1** | Physical Link | `TC_LINK_01` | Successful handshake over COM port at all baud rates (`9600`, `19200`, `38400`, `57600`, `115200` bps) | `SPEC VERIFIED (Agent B PENDING)` | `2026-11-26` | `tests/traces/01_handshake_9600.hexlog.gz`, `02_handshake_115200.hexlog.gz` |
| **Level 2** | Discovery & Protocol | `TC_DISC_01` | Reliable enumeration of all remote drives (BPB/DPB/FAT12/FAT16) in MS-DOS 6.22 | `SPEC VERIFIED (Agent B PENDING)` | `2026-11-26` | `tests/traces/04_bpb_query_fat16.hexlog.gz` |
| **Level 3** | Raw Sector I/O | `TC_SEC_01` | Error-free reading and writing of 1,000 randomized sectors via Differential Oracle (86Box) | `SPEC VERIFIED (Agent B PENDING)` | `2026-11-26` | `tests/traces/05_read_sector_single.hexlog.gz`, `06_write_sector_multi.hexlog.gz` |
| **Level 4** | High-Level DOS Workflows | `TC_DOS_01` | Successful execution of `DIR X:`, `COPY`, `FC /B`, `CHKDSK`, and `FORMAT` (V-FAT) | `PENDING` | — | Differential Oracle CI log |
| **Level 5** | Multi-Platform Parity | `TC_PLAT_01` | ESP32 serves FAT16 SD card over MAX3232 UART to real IBM PC/XT 5160 with zero transfer errors | `PENDING` | — | Hardware test log |

## 2. Golden Trace Capture & Verification Matrix (Agent A Ground Truth)

All 7 Golden Traces have been 100% deterministically decoded and verified (**774,590 / 774,590 bytes**, **923 / 923 frames**, **0 CRC-16/GENIBUS errors**, **4,127 keepalives verified**) by `analysis/verifiers/verify_traces.py` and formalized into `specs/interlnk_wire.ksy`, `docs/protocol.md`, and 24 JSON test vectors in `tests/fixtures/*.json`.

| Trace File | Baud Rate | Description | Captured | Bytes / Frames Verified | Kaitai Verified | C99 Verified |
| :--- | :--- | :--- | :---: | :---: | :---: | :---: |
| `tests/traces/01_handshake_9600.hexlog.gz` | `9600` | Initial connect & sync (`AA/00 55/FF 5A/11`), burst vs echo pacing, `0x00`/`0x01`/`0x1C` | `[x]` | `902/902 B` (`7` frames) | `[x]` | `[ ]` |
| `tests/traces/02_handshake_115200.hexlog.gz` | `115200` | Stepwise baud negotiation (`9600->115200`, 15-pair line test), `/V` echo pacing | `[x]` | `851/851 B` (`7` frames) | `[x]` | `[ ]` |
| `tests/traces/03_rcopy_bootstrap.hexlog.gz` | `2400`/`38400` | `INTERSVR /RCOPY`: `CTTY COM1`, 784 B `FXB.COM`, SHA-256-exact `INTERSVR.EXE` & `INTERLNK.EXE`, `CTTY CON` | `[x]` | `61,828/61,828 B` (`34` frames) | `[x]` | `[ ]` |
| `tests/traces/04_bpb_query_fat16.hexlog.gz` | `115200` | Drive enumeration, `DIR D:`, `CHKDSK D:` (FAT16 BPB, Generic IOCTL `08h/66h` MID, 43 reads) | `[x]` | `48,638/48,638 B` (`153` frames) | `[x]` | `[ ]` |
| `tests/traces/05_read_sector_single.hexlog.gz` | `115200` | Sector reads (512 B, 16 KiB, 65,024 B / 127 sectors across 8x `0x40`-fragmented 8192 B frames) | `[x]` | `541,101/541,101 B` (`402` frames) | `[x]` | `[ ]` |
| `tests/traces/06_write_sector_multi.hexlog.gz` | `115200` | Multi-sector writes (`ECHO`, 16 KiB `COPY` via 2x 8192 B fragmented frames, `FC /B`, `DEL`) | `[x]` | `92,177/92,177 B` (`203` frames) | `[x]` | `[ ]` |
| `tests/traces/07_media_change_error.hexlog.gz` | `115200` | `A:` + `C:`, FAT12 BPB, `DISK_A->DISK_B` Media Change (`0xFF`), Write-Protect (`0x8100`), Not-Ready (`0x8102`), 2x keepalive timeout & session re-sync | `[x]` | `29,093/29,093 B` (`117` frames) | `[x]` | `[ ]` |

## 3. Target Platform Build & Verification Status

| Platform Target | Toolchain | Architectural Constraints | Build Status | Test Status |
| :--- | :--- | :--- | :---: | :---: |
| Linux x86_64 Desktop | GCC / Clang (`-std=c99 -Werror -pedantic`) | Zero `malloc`, Zero FP | `CONFIGURED (CI)` | `PENDING (Agent B Sync)` |
| macOS ARM64 Desktop | Apple Clang (`-std=c99 -Werror -pedantic`) | Zero `malloc`, Zero FP | `CONFIGURED` | `PENDING (Agent B Sync)` |
| Windows x64 Desktop | MSVC (`/W4 /WX`) | Zero `malloc`, Zero FP | `CONFIGURED (CI)` | `PENDING (Agent B Sync)` |
| MS-DOS 16-Bit Real-Mode | Open Watcom v2 (`-ms -0 -za99 -wx`) | 8086/8088 / NEC V30 | `CONFIGURED (CI)` | `PENDING (Agent B / 86Box)` |
| ESP32 Embedded | ESP-IDF 5.x / PlatformIO / Arduino | MAX3232 UART + SDMMC | `MANIFEST READY` | `PENDING (HW)` |

## 4. Automated Verification Pipeline Gates

- [x] **Gate 1 — Clean-Room Isolation:** Pre-commit hook & CI script enforce Agent A / Agent B separation and zero-alloc / zero-FP invariants.
- [x] **Gate 2 — Kaitai & JSON Vectors (Agent A Spec):** Formal `specs/interlnk_wire.ksy` compiles cleanly (0 warnings) for Python & C++11 STL and validates all 24 real-trace JSON fixtures (`tests/harness/test_runner.py --require-kaitai`). C99 parity (`--c-binary`) queued for Agent B synchronization.
- [ ] **Gate 3 — Loopback & Fault-Injection (Real Protocol):** Update `libopenlnk` (`cleanroom/`) to verified CRC-16/GENIBUS wire format and verify loopback, single-bit flip, truncation, and echo/burst recovery in Agent B.
- [ ] **Gate 4 — Differential Oracle (86Box Headless):** Original MS-DOS 6.22 `INTERLNK.EXE` client verified end-to-end against `opensvr`.
- [ ] **Gate 5 — Hardware Parity Sign-Off:** Real IBM PC/XT 5160 + ESP32 SD card soak test signed off.
