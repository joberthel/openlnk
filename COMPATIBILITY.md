# OpenLNK Compatibility & Verification Matrix (`COMPATIBILITY.md`)

This document tracks objective verification evidence against the 5-Stage Definition of Done (DoD) specified in TRD Section 7. Requirements are considered complete only when all 5 stages pass and reference verifiable CI runs or hardware test logs.

## 1. Definition of Done — 5-Stage Compatibility Matrix

| Stage | Designation | Test ID | Required Evidence | Status | Verified Date | Reference / Trace |
| :--- | :--- | :--- | :--- | :---: | :---: | :--- |
| **Level 1** | Physical Link | `TC_LINK_01` | Successful handshake over COM port at all baud rates (`9600`, `19200`, `38400`, `57600`, `115200` bps) | `PENDING` | — | `tests/traces/01_handshake_9600.hexlog`, `02_handshake_115200.hexlog` |
| **Level 2** | Discovery & Protocol | `TC_DISC_01` | Reliable enumeration of all remote drives (BPB/DPB/FAT12/FAT16) in MS-DOS 6.22 | `PENDING` | — | `tests/traces/04_bpb_query_fat16.hexlog` |
| **Level 3** | Raw Sector I/O | `TC_SEC_01` | Error-free reading and writing of 1,000 randomized sectors via Differential Oracle (86Box) | `PENDING` | — | `tests/traces/05_read_sector_single.hexlog`, `06_write_sector_multi.hexlog` |
| **Level 4** | High-Level DOS Workflows | `TC_DOS_01` | Successful execution of `DIR X:`, `COPY`, `FC /B`, `CHKDSK`, and `FORMAT` (V-FAT) | `PENDING` | — | Differential Oracle CI log |
| **Level 5** | Multi-Platform Parity | `TC_PLAT_01` | ESP32 serves FAT16 SD card over MAX3232 UART to real IBM PC/XT 5160 with zero transfer errors | `PENDING` | — | Hardware test log |

## 2. Golden Trace Capture Matrix (Agent A Ground Truth)

| Trace File | Baud Rate | Description | Captured | Kaitai Verified | C99 Verified |
| :--- | :--- | :--- | :---: | :---: | :---: |
| `tests/traces/01_handshake_9600.hexlog` | `9600` | Initial connect & baud negotiation | `[x]` | `[ ]` | `[ ]` |
| `tests/traces/02_handshake_115200.hexlog` | `115200` | High-speed handshake + `/V` INT 2Fh hook | `[x]` | `[ ]` | `[ ]` |
| `tests/traces/03_rcopy_bootstrap.hexlog` | `2400`/`115200` | `INTERSVR /RCOPY` bare-PC bootstrap transfer | `[x]` | `[ ]` | `[ ]` |
| `tests/traces/04_bpb_query_fat16.hexlog` | `115200` | Drive enumeration, BPB/DPB query, FAT16 | `[x]` | `[ ]` | `[ ]` |
| `tests/traces/05_read_sector_single.hexlog` | `115200` | Sector reads (1 B, 511 B, 512 B, 4096 B, 64 KiB+) | `[x]` | `[ ]` | `[ ]` |
| `tests/traces/06_write_sector_multi.hexlog` | `115200` | Multi-sector writes & integrity verify | `[x]` | `[ ]` | `[ ]` |
| `tests/traces/07_media_change_error.hexlog` | `115200` | Media check (`0x01`), write-protect, disk-full/not-ready | `[x]` | `[ ]` | `[ ]` |

## 3. Target Platform Build & Verification Status

| Platform Target | Toolchain | Architectural Constraints | Build Status | Test Status |
| :--- | :--- | :--- | :---: | :---: |
| Linux x86_64 Desktop | GCC / Clang (`-std=c99 -Werror -pedantic`) | Zero `malloc`, Zero FP | `CONFIGURED (CI)` | `CONFIGURED (CI)` |
| macOS ARM64 Desktop | Apple Clang (`-std=c99 -Werror -pedantic`) | Zero `malloc`, Zero FP | `VERIFIED LOCAL` | `VERIFIED LOCAL` |
| Windows x64 Desktop | MSVC (`/W4 /WX`) | Zero `malloc`, Zero FP | `CONFIGURED (CI)` | `CONFIGURED (CI)` |
| MS-DOS 16-Bit Real-Mode | Open Watcom v2 (`-ms -0 -za99 -wx`) | 8086/8088 / NEC V30 | `CONFIGURED (CI)` | `PENDING (86Box)` |
| ESP32 Embedded | ESP-IDF 5.x / PlatformIO / Arduino | MAX3232 RTS/CTS + SDMMC | `MANIFEST READY` | `PENDING (HW)` |

## 4. Automated Verification Pipeline Gates

- [x] **Gate 1 — Clean-Room Isolation:** Pre-commit hook & CI script enforce Agent A / Agent B separation and zero-alloc / zero-FP invariants.
- [x] **Gate 2 — Kaitai & JSON Vectors:** Formal `.ksy` spec compiles cleanly and validates JSON test vectors with C99 parity.
- [x] **Gate 3 — Loopback & Fault-Injection:** In-memory pipe loopback, single-bit flip, frame truncation, and buffer-overflow recovery tests pass in `test_libopenlnk`.
- [ ] **Gate 4 — Differential Oracle (86Box Headless):** Original MS-DOS 6.22 `INTERLNK.EXE` client verified end-to-end against `opensvr`.
- [ ] **Gate 5 — Hardware Parity Sign-Off:** Real IBM PC/XT 5160 + ESP32 SD card soak test signed off.
