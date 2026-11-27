# MS-DOS 6.22 Conventional RAM Benchmark: OpenLNK vs. `INTERLNK` / `INTERSVR`

This report documents empirical 16-bit Real-Mode conventional memory measurements comparing **OpenLNK** (`OPENLNK.SYS`, `OPENSVR.EXE`, `OPENLNK.EXE` — both default `MAX_PAYLOAD=8192` and tight-RAM `MAX_PAYLOAD=4096` builds) against original **MS-DOS 6.22** `INTERLNK.EXE` and `INTERSVR.EXE`.

All measurements were executed live under authentic MS-DOS 6.22 (`IO.SYS` / `MSDOS.SYS` / `COMMAND.COM` / `MEM.EXE`) using the automated benchmark harness in [`tools/bench/run_dos_ram_benchmark.py`](../tools/bench/run_dos_ram_benchmark.py).

---

## Executive Summary

| Role | Higher RAM Usage | Ratio | Headline Comparison (Conventional RAM) |
| :--- | :--- | :---: | :--- |
| **Client Block Device Driver (`CONFIG.SYS`)** | **`OPENLNK.SYS`** | **~3.4×** (8K) / **~2.9×** (4K) | `OPENLNK.SYS`: **26.23 KiB** (4K: **22.23 KiB**) vs. `INTERLNK.EXE`: **7.67–9.33 KiB** |
| **Server Executable (`EXE`)** | **`INTERSVR.EXE`** | **4.1× – 5.3×** (up to **6.0×** vs. 4K) | `INTERSVR.EXE`: **132–165 KiB** vs. `OPENSVR.EXE`: **31.25 KiB** (4K: **27.25 KiB**) |
| **Client CLI (`EXE`)** | **Context-dependent** | ~1.5× alone / ~1.0× combined | `OPENLNK.EXE` standalone: **24.48 KiB** (4K: **20.48 KiB**) vs. `INTERLNK.EXE` CLI: **16.55 KiB** *(requires **+7.67–9.33 KiB** resident driver = **~24.2–25.9 KiB**)* |

- **Server winner — `OPENSVR.EXE`:** Thanks to 100% Zero-CRT linkage (`0` `clibs.lib` modules) and zero dynamic allocation, `OPENSVR.EXE` starts cleanly in **32 KiB** of free conventional RAM (**28 KiB** at `MAX_PAYLOAD=4096`), compared to **132 KiB minimum** / **165.1 KiB claimed** for original MS-DOS `INTERSVR.EXE` (**4.1×–5.3× less RAM**, and **2.2× smaller on disk**).
- **Resident driver winner — `INTERLNK.EXE`:** Original MS-DOS `INTERLNK.EXE` is hand-tuned x86 assembly that discards its `INIT`/CLI portions via the DOS break address and avoids an 8 KiB static frame buffer, settling at **7.67–9.33 KiB** resident. `OPENLNK.SYS` (compiled C99 `libopenlnk` with an 8 KiB static `rx_buffer` and 2 KiB private stack) uses **26.23 KiB** resident (**22.23 KiB** at `MAX_PAYLOAD=4096`).

---

## Testbed & Measurement Methodology

- **Machine Profile:** 86Box `486vipio2` (`i486DX2/66`, 4 MiB RAM, S3 Trio64, `xta_wdxt150` 10 MB Fixed-Size FAT16 VHD).
- **OS Baseline:** Authentic MS-DOS 6.22 (`FILES=20`, `BUFFERS=10`, `LASTDRIVE=Z`, **no `HIMEM.SYS` / `EMM386.EXE`** so 100% of allocations occur cleanly in 640 KiB Conventional RAM without UMB skew).
  - Clean baseline (`MEM /C`): **`63,152 B` used**, **`592,208 B` free**, **`592,192 B` largest executable program size**.
- **Driver Measurements:** Cold boot per `CONFIG.SYS` entry, capturing both `MEM /C` (exact system-wide Conventional RAM delta from baseline) and `MEM /D` (exact `IO.SYS` driver sub-MCB size), both offline and live-connected over `COM1` @ 115200 bps against `opensvr`.
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
| `OPENLNK.SYS /COM1 /BAUD:115200 /DRIVES:4` *(`MAX_PAYLOAD=8192`, offline)* | `15,068 B` | `26,848 B` | **`26,864 B` (`26.23 KiB`)** | `565,328 B` | **`3.42×` (`+19,008 B`)** |
| `OPENLNK.SYS /COM1 /BAUD:115200 /DRIVES:1` *(or online w/ 1 server drive)* | `15,068 B` | `26,704 B` | **`26,720 B` (`26.09 KiB`)** | `565,472 B` | **`3.40×` (`+18,864 B`)** |
| `OPENLNK4.SYS /COM1 /BAUD:115200 /DRIVES:4` *(`MAX_PAYLOAD=4096`, offline)* | `15,068 B` | `22,752 B` | **`22,768 B` (`22.23 KiB`)** | `569,424 B` | **`2.90×` (`+14,912 B`)** |

### Byte-Exact Linker Map Reconciliation (`OPENLNK.SYS`)

The live MS-DOS 6.22 `MEM /C` & `MEM /D` readings match the Open Watcom linker map (`cleanroom/dos/build/OPENLNK.MAP`) to the exact byte:

| Component | `MAX_PAYLOAD=8192` | `MAX_PAYLOAD=4096` | Notes |
| :--- | :---: | :---: | :--- |
| `SYS_HEAD` + `_TEXT` + `_DATA` + `CONST`/`CONST2` | `15,068 B` | `15,068 B` | On-disk raw `.SYS` image (`== _sys_bss_start`); includes resident `INIT` & arg parser |
| `_BSS` (`olnk_client_t`, UART, 8-unit BPB/label tables, init scratch) | `9,530 B` | `5,434 B` | Dominated by static `link.rx_buffer` (`8,197 B` vs. `4,101 B`) |
| Alignment padding (`_BSSEND` -> `_STACK`) | `10 B` | `10 B` | Paragraph boundary alignment |
| `_STACK` (private 8088-safe driver stack) | `2,048 B` | `2,048 B` | Switched atomically on every Interrupt call |
| **Driver Resident Image (`_sys_resident_end`)** | **`26,656 B`** | **`22,560 B`** | Matches linker `Memory size` |
| MS-DOS kernel DPB allocation (`48 B × units`) | `+192 B` *(4 drv)* / `+48 B` *(1 drv)* | `+192 B` *(4 drv)* | Clamping `/DRIVES:4` to 1 online server drive saves `144 B` |
| **`MEM /D` Installed Device Block** | **`26,848 B`** *(4 drv)* / **`26,704 B`** *(1 drv)* | **`22,752 B`** | Exact `MEM /D` output |
| MS-DOS `IO.SYS` Sub-MCB header | `+16 B` | `+16 B` | Per-driver subsegment header |
| **Total Conventional RAM Delta (`MEM /C`)** | **`26,864 B` (`26.23 KiB`)** | **`22,768 B` (`22.23 KiB`)** | Exact `MEM /C` delta (`0 B` discrepancy) |

---

## 2. Foreground Server Executables (`INTERSVR.EXE` vs. `OPENSVR.EXE`)

| Binary | Disk Size | MZ Static Min | Min. Free RAM (`/LIMIT`) | Live MCB Claim (`INT 21h`) | High-Water Mark (from PSP) | Touched Working Set (Active 115200 `dir`) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **`INTERSVR.EXE`** *(MS-DOS 6.22)* | `37,426 B` *(packed)* | `134,322 B` (`131.17 KiB`) | **`132 KiB`** *(fails at `131 KiB`)* | **`169,072 B` (`165.11 KiB`)** *(when ≥166 KiB free)* | `168,960 B` (`165.00 KiB`) normal / `134,400 B` (`131.25 KiB`) tight | **`115,136 B` (`112.44 KiB`)** |
| **`OPENSVR.EXE`** *(`MAX_PAYLOAD=8192`)* | **`17,122 B`** | `32,002 B` (`31.25 KiB`) | **`32 KiB`** *(fails at `31 KiB`)* | Static *(0 `AH=48h`/`4Ah` calls)* | **`32,000 B` (`31.25 KiB`)** *(+`112 B` env = `32,112 B`)* | **`29,648 B` (`28.95 KiB`)** *(idle `29,296 B`, `+352 B` stack)* |
| **`OPENSVR4.EXE`** *(`MAX_PAYLOAD=4096`)* | **`17,122 B`** | `27,906 B` (`27.25 KiB`) | **`28 KiB`** *(fails at `27 KiB`)* | Static *(0 `AH=48h`/`4Ah` calls)* | **`27,904 B` (`27.25 KiB`)** *(+`112 B` env = `28,016 B`)* | **`25,200 B` (`24.61 KiB`)** |

### Notable Runtime Behavior

- **`INTERSVR.EXE` adaptive memory claim:** `INTERSVR.EXE` is runtime-compressed on disk (`36.55 KiB` packed -> `>112 KiB` unpacked code/data/TUI in RAM). At startup it invokes `INT 21h AH=4Ah` with `BX = 0x293F` (**`168,944 B` / `165.0 KiB`**).
  - With **≥ 166 KiB** free conventional RAM, the resize succeeds and `INTERSVR.EXE` retains **`169,072 B` (`165.11 KiB`)** including MCB and 112 B environment block.
  - Between **132 KiB and 165 KiB** free, it falls back cleanly to a tighter **`~131–132 KiB`** footprint (`134,400 B`–`135,152 B` HWM).
  - At **≤ 131 KiB**, MS-DOS refuses to load it (`0x08 Insufficient memory`).
- **`OPENSVR.EXE` deterministic zero-alloc footprint:** Static `CODE` (`16,139 B`) + `DATA`/`CONST` (`928 B`) + `_BSS` (`11,596 B` at 8K / `7,500 B` at 4K) + `STACK` (`3,072 B`) = `31,744 B` (`27,648 B` at 4K) + `256 B` PSP = **`32,000 B`** (`27,904 B` at 4K). Serving live `openlnk --pipe ... dir` traffic at 115200 bps adds only **352 bytes** of stack depth and **0** dynamic heap allocations.

---

## 3. Client Standalone CLI (`INTERLNK.EXE` CLI vs. `OPENLNK.EXE`)

| Binary | Disk Size | MZ Static Min | Min. Free RAM (`/LIMIT`) | High-Water Mark (from PSP) | Touched Working Set (Active) | Architectural Scope |
| :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **`INTERLNK.EXE`** *(CLI mode)* | `17,197 B` | `16,941 B` (`16.54 KiB`) | **`17 KiB`** *(fails at `16 KiB`)* | **`16,944 B` (`16.55 KiB`)** | **`17,056 B` (`16.66 KiB`)** | Status/remap frontend only — **requires** resident `DEVICE=INTERLNK.EXE` (`+7.67–9.33 KiB`, total `~24.2–25.9 KiB`) |
| **`OPENLNK.EXE`** *(`MAX_PAYLOAD=8192`)* | **`12,934 B`** | `25,078 B` (`24.49 KiB`) | **`25 KiB`** *(fails at `24 KiB`)* | **`25,072 B` (`24.48 KiB`)** | **`22,512 B` (`21.98 KiB`)** *(live 115200 `/DIR`)* | Fully standalone client (sync, baud negotiation, `/DIR`, `/GET`) — no `CONFIG.SYS` or reboot needed |
| **`OPENLNK4.EXE`** *(`MAX_PAYLOAD=4096`)* | **`12,934 B`** | `20,982 B` (`20.49 KiB`) | **`21 KiB`** *(fails at `20 KiB`)* | **`20,976 B` (`20.48 KiB`)** | **`18,416 B` (`17.98 KiB`)** *(live 115200 `/DIR`)* | Fully standalone tight-RAM build |

---

## 4. Optimization Levers for `OPENLNK.SYS`

Because `OPENSVR.EXE` already uses **4×–6× less RAM** than `INTERSVR.EXE`, the primary conventional-RAM optimization target for constrained 8088/XT systems is `OPENLNK.SYS` (`26.23 KiB` -> target `~15–17 KiB` in pure C99):

1. **Cap `MAX_PAYLOAD=4096` for `OPENLNK.SYS` (already supported — saves `4,096 B` immediately):** `OPENLNK.SYS` already chunks sector I/O through a 4 KiB bounce buffer, so the upper 4 KiB of the 8 KiB `link.rx_buffer` are unused in the driver (`26.23 KiB` -> `22.23 KiB`).
2. **Discard `INIT` code & strings via DOS break address (~`2.5–3.5 KiB` savings):** Place `olnk_dos_sys_parse_args`, `olnk_dos_sys_init`, startup banner formatting, and `g_sys_init_cmdline`/`g_sys_init_banner` into a trailing `INIT` segment after resident `_STACK` and return the break address before `INIT` on `Cmd 0x00`.
3. **Right-size private driver stack (`2,048 B` -> `1,024 B`, saves `1,024 B`):** Live stack depth across `READ`/`WRITE`/`BUILD_BPB`/`IOCTL` is well below 600 bytes; verifying peak stack watermark under worst-case retry paths can safely reclaim 1 KiB.
4. **Trim unused `olnk_client_t` / per-unit tables (~`0.5–1.0 KiB` savings):** Dynamically size per-unit BPB/label arrays to the active `/DRIVES:<N>` count before computing the `INIT` break address.

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
