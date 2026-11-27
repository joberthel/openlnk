# OpenLNK for 16-Bit Real-Mode MS-DOS (`cleanroom/dos/`)

Clean-room, **100% Zero-CRT** 16-bit Real-Mode MS-DOS 5.0 / 6.0 / 6.22 driver, server, and client CLI built on `libopenlnk` and cross-compiled with **Open Watcom v2** for **8086/8088** (and NEC V20/V30+) processors.

- **Zero dynamic allocation & zero floating-point** — static BSS state only, zero `clibs.lib` modules linked across all three binaries.
- **Direct 8250 / 16450 / 16550A COM port HAL** — polled register I/O (no IRQ conflicts), automatic 16550A FIFO detection, 3-wire null-modem compatible up to **115200 bps**.
- **Verified on real MS-DOS PC hardware** over RS-232 @ 115200 bps against desktop `opensvr` / `openlnk` and original MS-DOS `INTERLNK.EXE` / `INTERSVR.EXE` (see [COMPATIBILITY.md](../../COMPATIBILITY.md)).

Related docs: [Root README](../../README.md) · [Protocol Specification](../../docs/protocol.md) · [Compatibility Matrix](../../COMPATIBILITY.md)

---

## Targets & Zero-CRT Memory Layout

Compiled at default `MAX_PAYLOAD=8192` (`-ms -0 -za99 -wx -we -os -zm -s -zl` + WASM `-0`):

| Binary | Format | Disk Size (CI Budget) | Conventional RAM (Stack) | `clibs.lib` | Purpose |
| :--- | :--- | :---: | :---: | :---: | :--- |
| **`OPENLNK.SYS`** | MS-DOS Block Device Driver (raw binary) | `15,068 B` / `14.7 KiB` (`<= 15 KiB`) | `26,656 B` / `26.0 KiB` resident (`2,048 B` private stack) | `0` | Mounts remote drives as native DOS letters (`D:`, `E:`, ...) via `CONFIG.SYS` |
| **`OPENSVR.EXE`** | DOS MZ Executable | `17,122 B` / `16.7 KiB` (`<= 17 KiB`) | `31,744 B` / `31.0 KiB` (`3,072 B` stack) | `0` | Serves local DOS drives (hard disks, floppies, RAMDrives) over `COM1`–`COM4` |
| **`OPENLNK.EXE`** | DOS MZ Executable | `12,934 B` / `12.6 KiB` (`<= 13 KiB`) | `24,816 B` / `24.2 KiB` (`3,072 B` stack) | `0` | Standalone one-shot client CLI (drive info, `/DIR`, `/GET`) without reboot |

### How Zero-CRT & Small Footprint Work

- **`OPENLNK.SYS`** uses a single-segment DGROUP layout (`CS == DS == ES == SS`, `< 64 KiB`) with the 18-byte MS-DOS block device driver header (`0xFFFFFFFF`, attribute `0x2842`, Strategy `0x0012`, Interrupt `0x001D`, signature `'OPENLNK'`) pinned at file offset `0x0000` (`openlnk_sys_entry.asm`).
  - `_BSS` and `_STACK` are omitted on disk via `wlink ... noemit` (file size matches `_sys_bss_start` byte-for-byte) and `_BSS` is zeroed once on first Interrupt entry.
  - Every Interrupt call atomically switches `SS:SP` (8088-safe `CLI`/`POPF`) onto a private `2,048 B` driver stack.
  - Sector transfers use a `4 KiB` (8-sector) bounce buffer aliased into `g_sys_client.link.rx_buffer` with 64 KiB segment-wrap-safe far-pointer normalization.
- **`OPENSVR.EXE` & `OPENLNK.EXE`** link a minimal 75-byte assembly startup (`dos_exe_crt0.asm`), direct `INT 21h` console/file/keyboard HAL (`dos_hal_io.c`), and pure-8086 32-bit multiply/divide helpers (`__U4M`, `__I4M`, `__U4D`), eliminating Watcom's `cstart`, `stdio`, `ctype`, `conio`, and heap allocator.
- Optional tighter RAM build: pass `MAX_PAYLOAD=4096` to `Makefile.watcom` to halve the static frame buffer from 8 KiB to 4 KiB when operating on heavily constrained 8088 systems.

---

## 1. `OPENLNK.SYS` — MS-DOS Block Device Driver (`CONFIG.SYS`)

### Installation

Copy `OPENLNK.SYS` to `C:\OPENLNK\OPENLNK.SYS` (or `C:\DOS\`) and add a `DEVICE=` line to `C:\CONFIG.SYS`:

```dosini
REM Standard 16550A PC on COM1 at up to 115200 bps (reserves 4 drive letters, e.g. D:-G:)
DEVICE=C:\OPENLNK\OPENLNK.SYS /COM1 /BAUD:115200 /DRIVES:4

REM Vintage 8088/XT with 8250 UART, single redirected drive
DEVICE=C:\OPENLNK\OPENLNK.SYS /COM1 /BAUD:9600 /DRIVES:1
```

### `CONFIG.SYS` Parameters

Switches are case-insensitive and accept either `/` or `-` prefixes:

| Switch | Default | Range / Values | Description |
| :--- | :---: | :--- | :--- |
| `/COM1`..`/COM4` *(or `/COM:1`..`/COM:4`)* | `COM1` | `1`–`4` | Serial port selection (reads BIOS Data Area `0040:0000`–`0040:0006`, fallback `0x3F8`/`0x2F8`/`0x3E8`/`0x2E8`) |
| `/BAUD:<rate>` | `115200` | `1200`, `2400`, `4800`, `9600`, `19200`, `38400`, `57600`, `115200` | Maximum negotiated baud rate (always syncs at `9600` bps first) |
| `/DRIVES:<N>` | `4` | `1`–`8` | Number of redirected DOS drive letters to reserve (clamped to server count if connected at boot) |

### Online Boot vs. Offline Standby (Lazy-Connect)

During `CONFIG.SYS` `INIT` (Cmd `0x00`), `OPENLNK.SYS` performs 3 quick Phase A sync attempts:

- **Server online at boot:** Negotiates baud, maps remote drives, prefetches live BPBs, and prints:
  ```text
  OPENLNK.SYS v0.2.0: COM1 115200 bps, 1 drive(s) (D:) [connected]
  ```
- **Server offline at boot:** Never hangs or aborts boot. Reserves `/DRIVES:<N>` letters with valid 10 MiB placeholder BPBs (preventing DOS boot division-by-zero) and prints:
  ```text
  OPENLNK.SYS v0.2.0: COM1 115200 bps, 4 drive(s) (D:-G:) [standby]
  ```
  On the first access (e.g. `DIR D:`) after starting `opensvr` or `OPENSVR.EXE`, `OPENLNK.SYS` lazily connects and reports `MEDIA_CHANGED` (`0xFF`) so MS-DOS calls `BUILD_BPB` and loads the live remote filesystem geometry.

> **Idle Timeout Note:** Because `OPENLNK.SYS` is a polled block device driver without a background TSR timer hook, it does not send idle keepalives at the `C:\>` prompt. Both `opensvr` and `OPENSVR.EXE` default to a 60-second idle high-baud timeout (`--idle-timeout 60000`) for this reason; if the link drops after extended inactivity, `OPENLNK.SYS` automatically reconnects on the next I/O request.

### Supported MS-DOS Block Device Driver Commands

Header attribute word `0x2842` (`Non-IBM | Open/Close/Removable | Generic IOCTL | 32-bit LBA`):

| Cmd | Name | Behavior |
| :---: | :--- | :--- |
| `0x00` | `INIT` | Parses `CONFIG.SYS` switches, initializes UART, attempts initial connect, registers BPB pointer array |
| `0x01` | `MEDIA_CHECK` | Forwards opcode `0x02`; forces `0xFF` (`MEDIA_CHANGED`) once after offline standby/remap; returns previous volume label pointer |
| `0x02` | `BUILD_BPB` | Forwards opcode `0x03`; writes packed 25-byte MS-DOS BPB into driver segment |
| `0x04` | `READ` | Forwards opcode `0x04` in `<= 4 KiB` chunks with 16-bit & 32-bit LBA (`start16 == 0xFFFF`) |
| `0x08` / `0x09` | `WRITE` / `WRITE_VERIFY` | Forwards opcode `0x05` / `0x06` in `<= 4 KiB` chunks; propagates `0x8100` (`WRITE_PROTECT`) & `0x810F` (`INVALID_DISK_CHANGE`) |
| `0x0D` / `0x0E` | `OPEN` / `CLOSE` | Forwards opcodes `0x16` / `0x17` |
| `0x0F` | `REMOVABLE_MEDIA` | Returns `DONE` with `BUSY=0` so DOS honors `MEDIA_CHECK` on remote volumes |
| `0x13` | `GENERIC_IOCTL` | Forwards Category `0x08` Functions `0x66` (Get Media ID) & `0x46` (Set Media ID) via opcode `0x1D` |
| `0x17` / `0x18` | `GET` / `SET_LOGICAL` | Single logical letter per unit (`unit = 0`, `DONE`) |
| `0x19` | `IOCTL_QUERY` | Reports support for Disk IOCTL `08h/66h` and `08h/46h` |

---

## 2. `OPENSVR.EXE` — 16-Bit MS-DOS Server

Exposes local MS-DOS drives over `COM1`–`COM4` to a remote `openlnk` desktop client, `OPENLNK.SYS` / `OPENLNK.EXE`, original MS-DOS `INTERLNK.EXE`, or ESP32 client.

### Usage & Switches

```bat
OPENSVR [drives...] [/COM1../COM4] [/BAUD:rate] [/RO] [/BURST] [/V]
```

| Argument | Default | Description |
| :--- | :---: | :--- |
| `A:` `B:` `C:` ... | `C:` | One or more local DOS drive letters to expose (if omitted, exposes `C:`) |
| `/COM1`..`/COM4` | `/COM1` | Serial port selection |
| `/BAUD:<rate>` | `115200` | Maximum negotiated baud rate (`1200`..`115200`) |
| `/RO` | Read-Write | Write-protect all exposed drives (returns DOS error `0x8100` on writes) |
| `/BURST` | Echo-Paced (`/V`) | Allow Burst (no-echo) frame reception instead of default byte-by-byte Echo RX |
| `/V` | Enabled | Explicit byte-by-byte Echo-Paced RX mode (default; compatible with `INTERSVR /V`) |
| `/?`, `/HELP`, `-h`, `--help` | — | Print usage summary and exit |

Press **`ESC`**, **`Q`**, or **`Ctrl-C`** to stop `OPENSVR.EXE` and display transfer statistics (`rpcs`, `rd`, `wr`, `err`).

### Examples

```bat
REM Share drive C: on COM1 at up to 115200 bps (read-write):
OPENSVR C: /COM1 /BAUD:115200

REM Share floppy A:, hard disk C:, and RAMDrive D: read-only on COM2:
OPENSVR A: C: D: /COM2 /RO

REM Conservative 8250 XT setup at 9600 bps:
OPENSVR C: /COM1 /BAUD:9600
```

### Supported MS-DOS Drives & BPB Probing

`OPENSVR.EXE` accesses drives through `dos_hal_disk.c`:

- **Sector I/O:** Uses MS-DOS 4.0+ 32-bit extended packet `INT 25h` / `INT 26h` (`CX=0xFFFF`) with automatic fallback to 16-bit LBA `INT 25h` / `INT 26h` for DOS 3.x volumes `<= 32 MiB`.
- **Supported volume types:** FAT12 and FAT16 hard disk partitions, floppy disks (with serial-number-based media-change detection on `MEDIA_CHECK`), and **RAM drives** (e.g. `RAMDRIVE.SYS` / `RDV 1.20`).
- **Non-bootable BPB support:** When a volume's sector 0 omits the `0x55 0xAA` boot signature or uses a 19-byte DOS 3.x BPB (common on `RAMDRIVE.SYS`), `OPENSVR.EXE` automatically queries MS-DOS Generic IOCTL **`INT 21h AX=440Dh CX=0860h`** (Get Device Parameters) and **`CX=0866h`** (Get Media ID), with a relaxed DOS 3.x VBR fallback.
- **Fast one-shot reconnects:** `OPENSVR.EXE` polls the UART non-blockingly using lock-free BIOS 18.2 Hz ticks (`0040:006C`) and automatically resets back to `9600` bps after `>= 3` UART line/framing errors while idle at `> 9600` bps (or after 60 s idle timeout), so repeated one-shot `openlnk` commands reconnect seamlessly without restarting `OPENSVR.EXE`.

---

## 3. `OPENLNK.EXE` — Standalone 16-Bit MS-DOS Client CLI

Query remote drives, list remote root directories, or download files directly from the DOS command prompt — no `CONFIG.SYS` entry or reboot needed.

### Usage & Switches

```bat
OPENLNK [/COM1../COM4] [/BAUD:rate]
OPENLNK [/COM1../COM4] [/BAUD:rate] /DIR [X:]
OPENLNK [/COM1../COM4] [/BAUD:rate] /GET <remote_file> [local_file] [X:]
```

| Argument | Default | Description |
| :--- | :---: | :--- |
| *(no mode flag)* | Drive info | Connect and display negotiated baud, server DOS version, `max_block`, and mapped remote drives |
| `/DIR [X:]` | First ready unit | List remote FAT12/FAT16 root directory of server drive `X:` |
| `/GET <remote> [local] [X:]` | `local = remote`, first ready unit | Download 8.3 file from remote root directory via direct `INT 21h` (`AH=3Ch`/`40h`/`3Eh`) |
| `/COM1`..`/COM4` | `/COM1` | Serial port selection |
| `/BAUD:<rate>` | `115200` | Maximum negotiated baud rate (`1200`..`115200`) |
| `/?`, `/HELP`, `-h`, `--help` | — | Print usage summary and exit |

Console output writes directly to `STDOUT` (Handle 1, `INT 21h AH=40h`), so standard MS-DOS redirection works:

```bat
OPENLNK /COM1
OPENLNK /COM1 /DIR > REMOTE.TXT
OPENLNK /COM1 /GET SOKOBAN.EXE
OPENLNK /COM1 /GET TEST64K.BIN C:\VERIFY.BIN C:
FC /B C:\TEST64K.BIN C:\VERIFY.BIN
```

> **Scope note:** `OPENLNK.EXE` is intentionally minimal (`12.6 KiB`) for quick file retrieval and bootstrapping. `/DIR` and `/GET` operate on the remote root directory (8.3 filenames). For full subdirectory access, writes, `CHKDSK`, and running programs directly from remote shares, load `OPENLNK.SYS` in `CONFIG.SYS`.

---

## Hardware Requirements & Null-Modem Wiring

### CPU, DOS & UART Support

- **CPU:** Intel `8086` / `8088`, NEC `V20` / `V30`, `80286`, `386`, `486`, Pentium and newer (`wcc -0` / `wasm -0`).
- **OS:** MS-DOS 5.0 / 6.0 / 6.22 (and DOS 3.x `<= 32 MiB` logical sector fallback in `OPENSVR.EXE`).
- **UART:** `8250`, `16450`, and `16550A`.
  - Reads COM1–COM4 base I/O ports from the BIOS Data Area (`0040:0000`–`0040:0006`) with standard fallbacks (`0x3F8`, `0x2F8`, `0x3E8`, `0x2E8`).
  - Operates in deterministic polled mode (`IER=0x00`, `MCR=0x03` DTR+RTS) with zero IRQ dependencies.
  - Detects genuine `16550A` FIFOs (`IIR & 0xC0 == 0xC0`, enables 1-byte trigger) and automatically disables buggy non-A `16550` FIFOs (`IIR & 0xC0 == 0x80`).
  - Uses lock-free reads of the BIOS 18.2 Hz tick counter (`0040:006C`) with midnight rollover handling (`1,573,040` ticks/day).

### Supported Baud Rates

| Baud Rate | UART Divisor (`1.8432 MHz`) | Typical Hardware Recommendation |
| :---: | :---: | :--- |
| `1200` / `2400` / `4800` | `96` / `48` / `24` | Diagnostics / noisy long cables |
| `9600` | `12` (`0x0C`) | Initial connect rate; safe ceiling for 4.77 MHz 8088 + 8250 UART |
| `19200` / `38400` | `6` / `3` | Fast 8088/V20/286 with 16450/16550A |
| `57600` / `115200` | `2` / `1` | 286/386/486/Pentium with `16550A` FIFO (verified @ `115200` bps) |

### 3-Wire Null-Modem Pinout (No RTS/CTS Needed)

| Signal | PC DB9 (Male) | PC DB25 (Male) | Connects To (Remote DB9) | Connects To (Remote DB25) |
| :--- | :---: | :---: | :---: | :---: |
| **TXD** (Transmit) | Pin 3 | Pin 2 | Pin 2 (RXD) | Pin 3 (RXD) |
| **RXD** (Receive) | Pin 2 | Pin 3 | Pin 3 (TXD) | Pin 2 (TXD) |
| **GND** (Ground) | Pin 5 | Pin 7 | Pin 5 (GND) | Pin 7 (GND) |

---

## Building from Source (Open Watcom v2)

### Option A — Reproducible Docker Build (macOS / Linux / Windows)

```bash
# From the repository root (--platform linux/amd64 required on Apple Silicon / ARM64):
docker build --platform linux/amd64 -t openlnk-watcom -f tools/docker/Dockerfile.watcom .
docker run --rm --platform linux/amd64 -v "$PWD:/workspace" openlnk-watcom

# Optional 4 KiB payload build for ultra-tight RAM budgets:
docker run --rm --platform linux/amd64 -v "$PWD:/workspace" openlnk-watcom \
  make -C cleanroom/dos -f Makefile.watcom clean all MAX_PAYLOAD=4096
```

Outputs are written to `cleanroom/dos/build/` (`OPENLNK.SYS`, `OPENSVR.EXE`, `OPENLNK.EXE`, and linker maps `OPENLNK.MAP`, `OPENSVR.MAP`, `OPENLNKX.MAP`).

### Option B — Local Open Watcom v2 Installation (Linux x64)

```bash
export WATCOM=/opt/watcom
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h"

make -C cleanroom/dos -f Makefile.watcom
```

### Automated Verification

```bash
# Verify MZ/.SYS headers, 0 clibs.lib modules, exact disk == _sys_bss_start, and size budgets:
python3 tools/ci/verify_dos_binaries.py --build-dir cleanroom/dos/build

# Run host simulated-16550A + INT 25h/26h + OPENLNK.SYS Request Header test suite (164/164):
cmake -S cleanroom -B cleanroom/build && cmake --build cleanroom/build --target test_dos_hal
./cleanroom/build/test_dos_hal
```

---

## Troubleshooting

| Symptom | Cause & Fix |
| :--- | :--- |
| **`No server response on COM1 (0x03F8)`** | Verify the serial port is enabled in BIOS and matches the physical connector (`COM1` = `0x3F8`, `COM2` = `0x2F8`). Check `OPENSVR`'s startup line, which prints the detected I/O base and UART type (`16550A FIFO` vs `8250/16450`). |
| **Sync never completes on either side** | Confirm you are using a **null-modem (crossover)** cable (`TX↔RX`, `RX↔TX`, `GND↔GND`), not a straight-through modem cable. On Linux, check serial port permissions (`sudo usermod -aG dialout "$USER"`). |
| **Timeouts or CRC retries at 57600 / 115200 bps** | Older `8250`/`16450` UARTs lack a 16-byte RX FIFO and can overrun on slow CPUs. Pass `/BAUD:9600` (or `/BAUD:19200`) on DOS and `--baud 9600` on `opensvr`/`openlnk`, and avoid `/BURST` so byte-by-byte echo pacing stays active. |
| **`Not ready reading drive D:` after idle pause** | `OPENLNK.SYS` does not emit background keepalives while idle at the prompt. Pressing `R` (Retry) reconnects lazily; to prevent idle drops during long interactive work, run `opensvr --idle-timeout 0` (or increase from the default `60000` ms). |
| **One-shot `openlnk` fails on 2nd call** | Ensure both peers run current OpenLNK binaries (high-baud line-error fast-drop resets `OPENSVR.EXE` / `opensvr` back to 9600 bps automatically in `< 300 ms`), or use `openlnk ... shell` for persistent interactive sessions. |
