# MS-DOS 6.22 Conventional RAM Benchmark: OpenLNK vs. `INTERLNK` / `INTERSVR`

This report documents empirical 16-bit Real-Mode conventional memory measurements comparing **OpenLNK** (`OPENLNK.SYS`, `OPENSVR.EXE`, `OPENLNK.EXE` — both default `SYS_MAX_PAYLOAD=2048` / `MAX_PAYLOAD=8192` and `4096`-byte payload builds) against original **MS-DOS 6.22** `INTERLNK.EXE` and `INTERSVR.EXE`.

All measurements were executed live under authentic MS-DOS 6.22 (`IO.SYS` / `MSDOS.SYS` / `COMMAND.COM` / `MEM.EXE`) using the automated benchmark harness in [`tools/bench/run_dos_ram_benchmark.py`](../tools/bench/run_dos_ram_benchmark.py).

---

## Executive Summary

| Role | Higher RAM Usage | Ratio | Headline Comparison (Conventional RAM) |
| :--- | :--- | :---: | :--- |
| **Client Block Device Driver (`CONFIG.SYS`)** | **`OPENLNK.SYS`** | **~1.78×** (2K default) / **~2.04×** (4K) | `OPENLNK.SYS`: **13.67 KiB** (`/DRIVES:4`, **13.42 KiB** `/DRIVES:1`) / 4K: **15.67 KiB** vs. `INTERLNK.EXE`: **7.67–9.33 KiB** *(down **-47.9%** from original 26.23 KiB)* |
| **Server Executable (`EXE`)** | **`INTERSVR.EXE`** | **4.2× – 5.3×** (up to **6.1×** vs. 4K) | `INTERSVR.EXE`: **132–165 KiB** vs. `OPENSVR.EXE`: **30.95 KiB** (4K: **26.95 KiB**) |
| **Client CLI (`EXE`)** | **Context-dependent** | ~1.4× alone / ~0.95× combined | `OPENLNK.EXE` standalone: **23.17 KiB** (`24 KiB` min; 4K: **19.17 KiB** / `20 KiB` min) vs. `INTERLNK.EXE` CLI: **16.55 KiB** *(requires **+7.67–9.33 KiB** resident driver = **~24.2–25.9 KiB**)* |

- **Server winner — `OPENSVR.EXE`:** Thanks to 100% Zero-CRT linkage (`0` `clibs.lib` modules) and zero dynamic allocation, `OPENSVR.EXE` starts cleanly in **32 KiB** of free conventional RAM (**28 KiB** at `MAX_PAYLOAD=4096`), compared to **132 KiB minimum** / **165.1 KiB claimed** for original MS-DOS `INTERSVR.EXE` (**4.2×–5.3× less RAM**, and **2.2× smaller on disk**).
- **Client driver (`OPENLNK.SYS`):** Built in portable, zero-alloc ISO C99 with discardable `INIT_TEXT`/`INIT_DATA` segments (`2,102 B` reclaimed on `Cmd 0x00`), dynamically trimmed per-unit `_UNIT_BSS` slots (`38 B/unit`), a `2 KiB` default bounce/frame buffer (`SYS_MAX_PAYLOAD=2048`), and a watermark-verified `896 B` private 8088-safe stack (live MS-DOS 6.22 peak `464 B`, `+432 B` headroom), `OPENLNK.SYS` uses **`13.67 KiB`** (`14,000 B`) in `MEM /C` for `/DRIVES:4` (**`13.42 KiB`** for `/DRIVES:1` or 1 online server drive) — cutting resident RAM by **`12.56 KiB` (`-47.9%`)** from the initial `26.23 KiB` baseline and landing well under `14 KiB` (vs. `7.67–9.33 KiB` for hand-written assembly `INTERLNK.EXE`).

---

## Testbed & Measurement Methodology

- **Machine Profile:** 86Box `486vipio2` (`i486DX2/66`, 4 MiB RAM, S3 Trio64, `xta_wdxt150` 10 MB Fixed-Size FAT16 VHD).
- **OS Baseline:** Authentic MS-DOS 6.22 (`FILES=20`, `BUFFERS=10`, `LASTDRIVE=Z`, **no `HIMEM.SYS` / `EMM386.EXE`** so 100% of allocations occur cleanly in 640 KiB Conventional RAM without UMB skew).
  - Clean baseline (`MEM /C`): **`63,152 B` used**, **`592,208 B` free**, **`592,192 B` largest executable program size**.
- **Driver Measurements:** Cold boot per `CONFIG.SYS` entry, capturing both `MEM /C` (exact system-wide Conventional RAM delta from baseline) and `MEM /D` (exact `IO.SYS` driver sub-MCB size), both offline and live-connected over `COM1` @ 115200 bps against `opensvr` (including live `DIR D:`).
- **Live Stack Watermark Probe (`0xA5` Sentinel):** Pre-filled `_DRVSTK` with `0xA5` sentinel bytes under MS-DOS 6.22, ran online boot (`INIT` + BPB prefetch + `DIR D:` + `VOL D:` Generic IOCTL `08h/66h` + 24 KiB `COPY` read & write + `FC /B` + `CHKDSK D:` + `DEL`) and offline standby boot (`INIT` retries + lazy-connect `DIR D:` + `VOL D:`), then walked the MS-DOS device chain (`INT 21h AH=52h`) to scan untouched sentinel bytes. Measured live peak: **`350 B`** after offline `INIT`, **`464 B`** worst-case across full online and lazy-connect I/O.
- **Foreground Executable Measurements ([`tools/bench/dos_profiler/membench.c`](../tools/bench/dos_profiler/membench.c)):**
  1. **Static MZ Header:** `PSP (256 B) + Load Image + (e_minalloc * 16)`.
  2. **Live `INT 21h` / `INT 1Ch` MCB Trace:** Captures every `AH=48h`/`49h`/`4Ah` allocation/resize call plus live MCB chain snapshots (`INDOS == 0` during execution and right before `AH=4Ch` exit).
  3. **Dirty-Memory Working Set & High-Water Mark (HWM):** Pre-fills all free conventional RAM above `MEMBENCH.EXE` with `0xA5` sentinel bytes before `INT 21h AH=4Bh` (`EXEC`), runs the binary (both idle and serving live `openlnk --pipe ... dir` @ 115200 bps), injects a clean exit key (`Alt+F4` for `INTERSVR.EXE`, `ESC` for `OPENSVR.EXE`), and scans paragraph-by-paragraph for touched memory and peak offset from the child PSP.
  4. **Minimum Free RAM Ceiling (`/LIMIT:<KB>`):** Allocates a temporary ballast MCB before `EXEC` leaving exactly `<KB>` KiB of free conventional RAM to prove the exact startup minimum to 1 KiB resolution.

---

## 1. Resident Block Device Drivers (`CONFIG.SYS` `DEVICE=...`)

| Configuration (`DEVICE=...`) | Disk Size | `MEM /D` Driver Block | Total RAM Delta (`MEM /C`) | Largest Free EXE | Ratio vs. `INTERLNK` (`/DRIVES:4 /NOPRINTER`) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Baseline (Clean MS-DOS 6.22)** | — | `0 B` | `0 B` | `592,192 B` | — |
| `INTERLNK.EXE` *(default: 3 drives, COM+LPT)* | `17,197 B` | `9,536 B` | **`9,552 B` (`9.33 KiB`)** | `582,640 B` | `1.22×` |
| `INTERLNK.EXE /DRIVES:4 /COM1 /BAUD:115200` | `17,197 B` | `8,032 B` | **`8,048 B` (`7.86 KiB`)** | `584,144 B` | `1.02×` |
| `INTERLNK.EXE /DRIVES:4 /COM1 /BAUD:115200 /NOPRINTER` *(offline & online)* | `17,197 B` | `7,840 B` | **`7,856 B` (`7.67 KiB`)** | `584,336 B` | **`1.00×` (baseline)** |
| `INTERLNK.EXE /DRIVES:1 /COM1 /BAUD:115200 /NOPRINTER` | `17,197 B` | `7,600 B` | **`7,616 B` (`7.44 KiB`)** | `584,576 B` | `0.97×` |
| **`OPENLNK.SYS /COM1 /BAUD:115200 /DRIVES:4`** *(`SYS_MAX_PAYLOAD=2048`, default, offline)* | **`16,038 B`** | **`13,984 B`** | **`14,000 B` (`13.67 KiB`)** | `578,192 B` | **`1.78×` (`+6,144 B`)** |
| **`OPENLNK.SYS /COM1 /BAUD:115200 /DRIVES:1`** *(or online w/ 1 server drive)* | **`16,038 B`** | **`13,728 B`** | **`13,744 B` (`13.42 KiB`)** | `578,448 B` | **`1.75×` (`+5,888 B`)** |
| **`OPENLNK4.SYS /COM1 /BAUD:115200 /DRIVES:4`** *(`SYS_MAX_PAYLOAD=4096`, offline)* | `18,086 B` | `16,032 B` | **`16,048 B` (`15.67 KiB`)** | `576,144 B` | **`2.04×` (`+8,192 B`)** |
| *(Pre-optimization baseline `OPENLNK.SYS`, `MAX_PAYLOAD=8192`, `/DRIVES:4`)* | *`15,068 B`* | *`26,848 B`* | *`26,864 B` (`26.23 KiB`)* | *`565,328 B`* | *`3.42×` (`+19,008 B`)* |

### Byte-Exact Linker Map Reconciliation (`OPENLNK.SYS`)

The live MS-DOS 6.22 `MEM /C` & `MEM /D` readings match the Open Watcom linker maps (`OPENLNK.MAP` / `OPENLNK4.MAP`) to the exact byte:

| Component | Baseline (`8K`, pre-opt) | Default (`SYS_MAX_PAYLOAD=2048`) | Optional (`SYS_MAX_PAYLOAD=4096`) | Notes |
| :--- | :---: | :---: | :---: | :--- |
| `SYS_HEAD` + resident `_TEXT` | `14,868 B` | **`10,257 B`** | **`10,257 B`** | `INIT` moved to `INIT_TEXT`; UART/client/link streamlined (`-4,611 B`) |
| Resident `_DATA` + `CONST` + `CONST2` | `199 B` | **`91 B`** | **`91 B`** | `CONST = 0 B`; all `INIT` strings moved to `INIT_DATA` |
| Resident `_BSS` (`olnk_client_t`, UART, ptrs) | `9,530 B` | **`2,374 B`** | **`4,422 B`** | `rx_buffer` `2,053 B` / `4,101 B`, `ctrl_buf` `96 B`, no redundant BPB/init scratch |
| Alignment padding (`_BSSEND` -> `_DRVSTK`) | `10 B` | `12 B` | `12 B` | Paragraph alignment for private driver stack |
| `_DRVSTK` (private 8088-safe driver stack) | `2,048 B` | **`896 B`** | **`896 B`** | Live DOS 6.22 peak `464 B` -> `+432 B` headroom (`-1,152 B`) |
| `_UNIT_BSS` (active `/DRIVES:<N>` slots × `38 B`) | *(in `_BSS`)* | **`152 B`** *(4 drv)* / **`38 B`** *(1 drv)* | **`152 B`** *(4 drv)* | Unused slots `[units..7]` + `INIT` (`2,102 B`) reclaimed on `Cmd 0x00` |
| **Driver Resident Image (MS-DOS para-aligned)** | **`26,656 B`** | **`13,792 B`** *(4 drv)* / **`13,680 B`** *(1 drv)* | **`15,840 B`** *(4 drv)* | Max 8-drive `_sys_resident_end` = `13,936 B` (2K) / `15,984 B` (4K) |
| MS-DOS kernel DPB allocation (`48 B × units`) | `+192 B` *(4 drv)* / `+48 B` *(1 drv)* | `+192 B` *(4 drv)* / `+48 B` *(1 drv)* | `+192 B` *(4 drv)* | Clamping `/DRIVES:4` to 1 online server drive saves `144 B` DPB + `112 B` units |
| **`MEM /D` Installed Device Block** | **`26,848 B`** *(4 drv)* | **`13,984 B`** *(4 drv)* / **`13,728 B`** *(1 drv)* | **`16,032 B`** *(4 drv)* | Exact `MEM /D` output (`0 B` discrepancy) |
| MS-DOS `IO.SYS` Sub-MCB header | `+16 B` | `+16 B` | `+16 B` | Per-driver subsegment header |
| **Total Conventional RAM Delta (`MEM /C`)** | **`26,864 B` (`26.23 KiB`)** | **`14,000 B` (`13.67 KiB`)** *(4 drv)* / **`13,744 B` (`13.42 KiB`)** *(1 drv)* | **`16,048 B` (`15.67 KiB`)** *(4 drv)* | **`-12,864 B` (`-47.9%`)** vs. baseline (`0 B` discrepancy) |
| Discarded `INIT` tail (`INIT_TEXT` + `INIT_DATA`) | `0 B` *(kept resident)* | **`2,102 B`** *(`1,960 B` + `142 B`)* | **`2,102 B`** | Reclaimed immediately by MS-DOS via `rh+14..17` Break Address |
| On-disk `.SYS` image (`_sys_init_end`) | `15,068 B` | **`16,038 B` (`15.66 KiB`)** | `18,086 B` (`17.66 KiB`) | 1:1 load image (UMB/`DEVICEHIGH`-safe); still smaller than `INTERLNK.EXE` (`17,197 B`) |

---

## 2. Foreground Server Executables (`INTERSVR.EXE` vs. `OPENSVR.EXE`)

| Binary | Disk Size | MZ Static Min | Min. Free RAM (`/LIMIT`) | Live MCB Claim (`INT 21h`) | High-Water Mark (from PSP) | Touched Working Set (Active 115200 `dir`) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **`INTERSVR.EXE`** *(MS-DOS 6.22)* | `37,426 B` *(packed)* | `134,322 B` (`131.17 KiB`) | **`132 KiB`** *(fails at `131 KiB`)* | **`169,072 B` (`165.11 KiB`)** *(when ≥166 KiB free)* | `168,960 B` (`165.00 KiB`) normal / `134,400 B` (`131.25 KiB`) tight | **`115,136 B` (`112.44 KiB`)** |
| **`OPENSVR.EXE`** *(`MAX_PAYLOAD=8192`)* | **`16,824 B`** | `31,704 B` (`30.96 KiB`) | **`32 KiB`** *(fails at `31 KiB`)* | Static *(0 `AH=48h`/`4Ah` calls)* | **`31,696 B` (`30.95 KiB`)** *(+`112 B` env = `31,808 B`)* | **`29,344 B` (`28.66 KiB`)** *(idle `28,976 B`, `+368 B` stack)* |
| **`OPENSVR4.EXE`** *(`MAX_PAYLOAD=4096`)* | **`16,824 B`** | `27,608 B` (`26.96 KiB`) | **`28 KiB`** *(fails at `27 KiB`)* | Static *(0 `AH=48h`/`4Ah` calls)* | **`27,600 B` (`26.95 KiB`)** *(+`112 B` env = `27,712 B`)* | **`24,880 B` (`24.30 KiB`)** |

### Notable Runtime Behavior

- **`INTERSVR.EXE` adaptive memory claim:** `INTERSVR.EXE` is runtime-compressed on disk (`36.55 KiB` packed -> `>112 KiB` unpacked code/data/TUI in RAM). At startup it invokes `INT 21h AH=4Ah` with `BX = 0x293F` (**`168,944 B` / `165.0 KiB`**).
  - With **≥ 166 KiB** free conventional RAM, the resize succeeds and `INTERSVR.EXE` retains **`169,072 B` (`165.11 KiB`)** including MCB and 112 B environment block.
  - Between **132 KiB and 165 KiB** free, it falls back cleanly to a tighter **`~131–132 KiB`** footprint (`134,400 B`–`135,152 B` HWM).
  - At **≤ 131 KiB**, MS-DOS refuses to load it (`0x08 Insufficient memory`).
- **`OPENSVR.EXE` deterministic zero-alloc footprint:** Shared core code & UART HAL optimizations trimmed `OPENSVR.EXE` by `-298 B` on disk (`17,122 B` -> `16,824 B`) and `-304 B` in RAM (`32,000 B` -> `31,696 B` HWM; `27,600 B` at 4K), with `0` dynamic heap allocations and only `+368 B` active stack delta under live 115200 bps `dir` traffic.

---

## 3. Client Standalone CLI (`INTERLNK.EXE` CLI vs. `OPENLNK.EXE`)

| Binary | Disk Size | MZ Static Min | Min. Free RAM (`/LIMIT`) | High-Water Mark (from PSP) | Touched Working Set (Active) | Architectural Scope |
| :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **`INTERLNK.EXE`** *(CLI mode)* | `17,197 B` | `16,941 B` (`16.54 KiB`) | **`17 KiB`** *(fails at `16 KiB`)* | **`16,944 B` (`16.55 KiB`)** | **`17,056 B` (`16.66 KiB`)** | Status/remap frontend only — **requires** resident `DEVICE=INTERLNK.EXE` (`+7.67–9.33 KiB`, total `~24.2–25.9 KiB`) |
| **`OPENLNK.EXE`** *(`MAX_PAYLOAD=8192`)* | **`11,596 B`** | `23,740 B` (`23.18 KiB`) | **`24 KiB`** *(fails at `23 KiB`)* | **`23,728 B` (`23.17 KiB`)** | **`21,168 B` (`20.67 KiB`)** *(live 115200 `/DIR`)* | Fully standalone client (sync, baud negotiation, `/DIR`, `/GET`) — no `CONFIG.SYS` or reboot needed |
| **`OPENLNK4.EXE`** *(`MAX_PAYLOAD=4096`)* | **`11,596 B`** | `19,644 B` (`19.18 KiB`) | **`20 KiB`** *(fails at `19 KiB`)* | **`19,632 B` (`19.17 KiB`)** | **`17,072 B` (`16.67 KiB`)** *(live 115200 `/DIR`)* | Fully standalone tight-RAM build |

Shared client/link-layer factoring reduced `OPENLNK.EXE` by **`-1,338 B` on disk** (`12,934 B` -> `11,596 B`) and lowered its startup minimum by a full KiB (**`25 KiB` -> `24 KiB`** at 8K; **`21 KiB` -> `20 KiB`** at 4K).

---

## 4. `OPENLNK.SYS` Optimization Breakdown & Trade-Offs

Moving `OPENLNK.SYS` from **`26.23 KiB`** (`26,864 B`) down to **`13.67 KiB`** (`14,000 B`, `/DRIVES:4`) in `MEM /C` combined four orthogonal levers without sacrificing 8086/8088 safety, Zero-CRT, Zero-Alloc, or MS-DOS 6.22 interoperability:

1. **Buffer & BSS right-sizing (`-6,844 B` `_BSS`):**
   - Separate `SYS_MAX_PAYLOAD ?= 2048` compilation for `OPENLNK.SYS` (4 sectors / 2 KiB bounce buffer, while `OPENSVR.EXE` & `OPENLNK.EXE` retain `MAX_PAYLOAD=8192`), plus `-DOLNK_CLIENT_CTRL_BUF_SIZE=96U` (largest driver control response is 87 B `MAP_CONFIG`).
   - Removed redundant unpacked `g_sys_bpbs[8]` (`224 B`; reads `bytes_per_sector` directly from packed 25 B `bpb_wire`, eliminating `olnk_encode_dos_bpb_`), collapsed `g_sys_prev_vol_labels[8][12]` to a single `12 B` return label (`-84 B`), packed `g_sys_bpb_valid[8]` into a 1-byte bitmask, and reused `rx_buffer` for `INIT` cmdline/banner scratch (`-224 B`).
   - **Trade-off (`2 KiB` default vs. `4 KiB` optional):** On 115200 bps RS-232, `SYS_MAX_PAYLOAD=2048` matches a standard 4-sector (2 KiB) FAT16 cluster and adds `< 1.5%` wire header overhead on large 64 KiB `COPY` transfers compared to `4096`, while saving a full **`2,048 B`** of Conventional RAM. Users prioritizing maximum Burst-mode throughput can still build `SYS_MAX_PAYLOAD=4096` (`15.67 KiB` `MEM /C`).
2. **Discardable `INIT_TEXT` / `INIT_DATA` + Dynamic `_UNIT_BSS` Break Address (`-2,254 B` for `/DRIVES:4`):**
   - Placed `olnk_dos_sys_parse_args`, `olnk_dos_sys_init`, `sys_handle_init`, banner formatting, `olnk_dos_uart_init`, `olnk_dos_com_port_base`, `INIT` asm helpers, and all `INIT` strings in trailing `INIT_TEXT`/`INIT_DATA` segments (`2,102 B`) after `_UNIT_BSS`.
   - Placed `g_sys_units[8]` (`38 B/unit`) in `_UNIT_BSS` immediately after `_DRVSTK` and returned `&g_sys_units[units_out]` as the MS-DOS `Cmd 0x00` Break Address, reclaiming unused unit slots (`152 B` for `/DRIVES:4`, `266 B` for `/DRIVES:1`) along with all `INIT` code/strings. Resident `CONST` is now **`0 B`**.
3. **Resident Code & Stack-Frame Streamlining (`-4,611 B` `SYS_HEAD`+`_TEXT`):**
   - Direct inline `in`/`out`/`BDA` hardware I/O on 16-bit DOS (`dos_hal_uart.c`) + shared `uart_wait_lsr_bit()` polling helper and 16-bit `ms_to_bios_ticks()`.
   - Direct `_sys_asm_far_read`/`_write` calls on DOS, compact `rep stosb`/`rep movsb` in `dos_sys_rt.c`, 16-bit half-word `olnk_read_u32_le`/`olnk_write_u32_le` in `protocol.c`.
   - Shared Echo/CRC helpers and table-driven sync/baud-revert paths in `openlnk.c`; shared RPC exchange helpers and in-place `cli->map` decoding in `client.c`.
4. **Measured Private Stack Right-Sizing (`2,048 B` -> `896 B`, `-1,152 B`):**
   - Slashed worst-case call-chain stack frames (`olnk_dos_sys_handle_rh` `286 B -> 156 B`, `olnk_client_map_config` `172 B -> 40 B`, `olnk_dos_sys_dispatch` `108 B -> 40 B`, `sys_ensure_connected` `102 B -> 0 B`, `sys_handle_init` `108 B -> 18 B`).
   - Verified via live `0xA5` sentinel scan in MS-DOS 6.22 (`464 B` worst-case peak including nested BIOS IRQs across online `DIR`/`VOL`/`COPY`/`FC /B`/`CHKDSK` and offline lazy-connect), setting `_DRVSTK = 896 B` (`+432 B` / `+93%` safety margin).

---

## 5. Reproducing the Benchmark

Prerequisites:
- Built desktop CLIs (`cmake -S cleanroom -B cleanroom/build && cmake --build cleanroom/build`) and 16-bit DOS targets (`cleanroom/dos/build/`).
- Local Open Watcom v2 (`$WATCOM`) or Docker image `openlnk-watcom` (see [`cleanroom/dos/README.md`](../cleanroom/dos/README.md#building-from-source-open-watcom-v2)).
- 86Box + ROMs installed and a local bootable MS-DOS 6.22 10 MB Fixed-Size VHD (`analysis/binaries/vhds/dos622_client.vhd`, containing `C:\DOS\MEM.EXE`, `INTERLNK.EXE`, `INTERSVR.EXE`).

```bash
# 1. Dry-run check (validates environment, 86Box discovery, and AUTOEXEC.BAT <= 126 chars):
python3 tools/bench/run_dos_ram_benchmark.py --dry-run

# 2. Run the full automated 11-boot MS-DOS 6.22 RAM benchmark in 86Box:
python3 tools/bench/run_dos_ram_benchmark.py \
  --out-json build/ram_bench/results.json \
  --out-md build/ram_bench/results.md

# 3. Re-parse an existing captured COM2 benchmark log offline:
python3 tools/bench/run_dos_ram_benchmark.py --parse-log build/ram_bench/benchmark_live.log
```
