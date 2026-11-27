# OpenLNK for ESP32 (`esp32/`)

Production-ready, zero-allocation ISO C99 HAL and server engine for turning an **ESP32 / ESP32-S2 / ESP32-S3 / ESP32-C3 / ESP32-C6** into a solid-state MS-DOS 5.0 / 6.22 `INTERSVR`-compatible serial block server.

Connect an ESP32 + microSD card + **3.3V MAX3232** RS-232 transceiver directly to a vintage IBM PC/XT/AT/386/486 `COM1:` port and mount FAT12/FAT16 partitions or `.img`/`.vhd` disk images inside MS-DOS using standard `INTERLNK.EXE` or clean-room `OPENLNK.SYS`.

---

## Features

- **Zero Dynamic Allocation & Zero Floating-Point:** Core HALs (`esp32_hal_uart.c`, `esp32_hal_sd.c`, `esp32_server.c`) and `libopenlnk` never call `malloc`/`free` and never use `float`/`double`.
- **Hardened Multi-Instance UART HAL:**
  - Supports 1200–115200 bps (`8N1`) with automatic Phase-B stepwise baud negotiation.
  - Strictly drains pending TX bytes (`uart_wait_tx_done`) before every baud switch.
  - Sub-tick timeout rounding (`timeout_ms > 0` always yields `>= 1` FreeRTOS tick, even at 100 Hz).
  - 1-byte RX FIFO threshold + 1-byte wakeup with opportunistic burst drain for full-speed Echo-Paced and Burst frames.
- **Two Storage Backends:**
  1. **Raw SD Card Block Mode (`esp32_hal_sd.c`):** Direct 512-byte sector access via **SDMMC (1-bit / 4-bit)** or **SDSPI**, auto-detecting superfloppy FAT12/FAT16 VBRs vs. MBR partitions (or attaching explicit MBR partitions `0..3`), `WRITE_VERIFY` read-back check, hot-swap `MEDIA_CHANGED` (`0xFF`), card-detect/write-protect GPIOs, and Generic IOCTL `08h/46h` Set Media ID.
  2. **Disk Image File Mode (`esp32_hal_img.c`):** Serves raw FAT12/FAT16 `.img` or Fixed-Size `.vhd` (`conectix` footer stripped automatically) files stored on a normal FAT32/exFAT SD card mounted via ESP-IDF VFS (`/sdcard/dos_c.vhd`). Cleanly separated so raw-block firmware pays zero VFS/FATFS cost.
- **Monotonic High-Level Server & FreeRTOS Task Runner (`esp32_server.c`):** Uses `esp_timer_get_time()` for accurate protocol timers regardless of early UART returns or tick quantization.

---

## Hardware Wiring (MAX3232 3-Wire RS-232 Null-Modem)

> **CRITICAL VOLTAGE WARNING:** Power the MAX3232 module from **3.3V** (NOT 5V!) so its TTL `R1OUT` pin never exceeds 3.3V on the ESP32 RX GPIO. Never connect RS-232 (`±12V`) directly to ESP32 pins.

OpenLNK uses standard **3-wire null-modem wiring (TX, RX, GND)** — hardware RTS/CTS is **not required** because the protocol handles pacing in-band (`local_requires_echo = true`).

```text
+-------------------+         +------------------+         +-------------------------+
|       ESP32       |  3.3V   |   MAX3232 Board  |  RS-232 |   Vintage PC COM1       |
|                   |  TTL    |  (VCC = 3.3V!)   |  ±12V   |   DB9 Female / DB25     |
|                   |         |                  |         |                         |
|   3V3 ------------+---------+-> VCC            |         |                         |
|   GND ------------+---------+-> GND -----------+---------+-> DB9 Pin 5 (DB25 Pin 7)|
|   UART TX GPIO ---+---------+-> T1IN     T1OUT +---------+-> DB9 Pin 2 RXD (DB25 3)|
|   UART RX GPIO <--+---------+-  R1OUT     R1IN <---------+-- DB9 Pin 3 TXD (DB25 2)|
+-------------------+         +------------------+         +-------------------------+
```

> **Crossover note:** On a PC **DB9 Male** COM port, Pin 3 is PC-TXD (output) and Pin 2 is PC-RXD (input). On an IBM PC/XT **DB25 Male** serial card, Pin 2 is PC-TXD and Pin 3 is PC-RXD, Pin 7 is GND. If your MAX3232 board already carries a **female** DB9 wired as DCE (like a modem), plug it straight into the PC COM port; if it is wired as DTE (like a PC), use a standard null-modem cable/adapter between the MAX3232 DB9 and the PC.

---

## Recommended GPIO Pinouts

Keep `UART0` free for USB-serial debug logs and avoid boot-strapping/flash pins.

| SoC | UART1 TX | UART1 RX | SD Mode | SD Pins |
| :--- | :---: | :---: | :--- | :--- |
| **ESP32** | GPIO 17 | GPIO 16 | SDMMC 1-bit (Slot 1) | CLK=14, CMD=15, D0=2 (fixed IOMUX) |
| **ESP32** | GPIO 17 | GPIO 16 | SDSPI (SPI2) | MOSI=23, MISO=19, CLK=18, CS=5 |
| **ESP32-S3** | GPIO 17 | GPIO 18 | SDMMC 1-bit (GPIO Matrix) | CLK=36, CMD=35, D0=37 |
| **ESP32-S3** | GPIO 17 | GPIO 18 | SDSPI (SPI2) | MOSI=11, MISO=13, CLK=12, CS=10 |
| **ESP32-C3** | GPIO 4 | GPIO 5 | SDSPI (SPI2, no SDMMC HW) | MOSI=7, MISO=2, CLK=6, CS=10 |
| **ESP32-C6** | GPIO 4 | GPIO 5 | SDSPI (SPI2, no SDMMC HW) | MOSI=7, MISO=2, CLK=6, CS=10 |

---

## Memory Footprint & FreeRTOS Stack Safety

All large contexts MUST be placed in `static` / global BSS storage — **never** as local variables inside `app_main()` or a FreeRTOS task!

| Structure | Approximate Size | Allocation Rule |
| :--- | :---: | :--- |
| `olnk_esp32_server_t` (embeds `olnk_server_t` + 8197 B frame buffer) | ~9.3 KiB | **Static / caller-owned BSS** |
| `olnk_esp32_sd_drive_t` (embeds `sdmmc_card_t` + 512 B scratch) | ~0.9 KiB | Static / caller-owned |
| `olnk_esp32_img_drive_t` (embeds 512 B scratch) | ~0.6 KiB | Static / caller-owned |
| `olnk_esp32_uart_t` | ~0.1 KiB | Static / caller-owned |
| UART RX + TX ring buffers (IDF driver) | 4096 B + 1024 B | Configured via `olnk_esp32_uart_cfg_t` |
| Server FreeRTOS task stack (`olnk_esp32_server_start_task`) | `4096 B` default | Only call frames + SD/UART driver calls |

---

## Quickstart

### Option A — ESP-IDF 5.x (Recommended)

```bash
cd esp32/examples/sd_server_idf

# Classic ESP32 (SDMMC 1-bit or SDSPI):
idf.py set-target esp32
idf.py menuconfig      # OpenLNK MS-DOS Server Configuration
idf.py build flash monitor

# ESP32-C3 (automatically selects SDSPI because !SOC_SDMMC_HOST_SUPPORTED):
idf.py set-target esp32c3
idf.py build flash monitor
```

### Option B — PlatformIO (`espressif32`)

**From a local checkout:** `esp32/library.json` uses `pio_build.py` to compile the single-source `../cleanroom/src/*.c` core automatically alongside `esp32/src/*.c`.

```bash
cd esp32/examples/sd_server_arduino
pio run -e esp32dev -t upload -t monitor
# Or for ESP32-C3:
pio run -e esp32c3 -t upload -t monitor
```

**From a GitHub Release ZIP:** Every GitHub Release publishes a self-contained `openlnk-vX.Y.Z-arduino-esp32.zip` bundle containing both `library.properties` and a standalone `library.json`:

```ini
lib_deps =
    https://github.com/joberthel/openlnk/releases/download/v0.2.0/openlnk-v0.2.0-arduino-esp32.zip
```

### Option C — Arduino IDE

Download `openlnk-vX.Y.Z-arduino-esp32.zip` from **GitHub Releases** and install via **Sketch → Include Library → Add .ZIP Library...**.

To build the exact same standalone `.zip` locally from this repository:

```bash
python3 esp32/tools/make_arduino_bundle.py --out dist/openlnk-arduino.zip
```

---

## Preparing Storage Media for MS-DOS

- **Raw SD Mode (`OPENLNK_STORAGE_MODE_RAW`):** Format an SD card (or its first primary MBR partition, `<= 2 GiB`, partition type `0x04`/`0x06`/`0x0E`) as **FAT16** (or **FAT12** for `<= 16 MiB` superfloppies). Note that MS-DOS 6.22 does not support FAT32 or exFAT directly.
- **Disk Image Mode (`OPENLNK_STORAGE_MODE_IMG`):** Format any modern SDHC/SDXC card as **FAT32**, copy a raw FAT16 `.img` or **Fixed-Size `.vhd`** (e.g. `dos_c.vhd`, 10–512 MiB) into `/sdcard/`, and point `CONFIG_OPENLNK_IMG_FILE_PATH` at it.

---

## Gate 5 (`TC_PLAT_01`) Hardware Parity Soak-Test Checklist

Before marking `COMPATIBILITY.md` Level 5 / Gate 5 (`TC_PLAT_01`) as `VERIFIED`, execute and archive logs for all steps on a real vintage DOS PC (e.g. IBM PC/XT 5160 or 286/386/486 with 8250/16450/16550A COM1):

- [ ] **Hardware Setup:** ESP32 + 3.3V MAX3232 + FAT16 microSD card wired via 3-wire null-modem to PC `COM1:`. Confirm common GND and 3.3V VCC on MAX3232.
- [ ] **Phase A & B Connect:** Load `DEVICE=C:\DOS\INTERLNK.EXE /COM1` (or `OPENLNK.SYS /COM1`) on the PC. Verify ESP32 log prints `SYNC_OK`, stepwise `BAUD_SWITCH` up to target ceiling (e.g. 9600 on 8250 XT, up to 115200 on 16550A), `SESSION_SYNC`, and `MAP_CONFIG`.
- [ ] **Directory & BPB Check:** Run `DIR D:`, `VOL D:`, and `CHKDSK D:` on MS-DOS. Confirm zero filesystem errors and accurate volume label/serial/free space.
- [ ] **Multi-Fragment Write & Binary Verify:** Copy a `>= 64 KiB` test file from PC local disk to `D:` (`COPY /B TEST64K.BIN D:\`), then copy it back (`COPY /B D:\TEST64K.BIN C:\VERIFY.BIN`) and run `FC /B C:\TEST64K.BIN C:\VERIFY.BIN` → `FC: no differences encountered`.
- [ ] **Soak & Keepalive Stability:** Leave link idle for `>= 10` minutes while monitoring ESP32 `ka=` counter (zero unexpected disconnects) and run repeated multi-file copies (`>= 1000` sectors total) with `err=0`.
- [ ] **Hot-Swap & Error Semantics:** Enable read-only mode (verify DOS `Write protect error` / `0x8100`), remove/swap SD card (verify `Not ready` / `0x8102` and `MEDIA_CHANGED` `0xFF` on re-insert), and unplug/replug RS-232 cable (verify clean drop to 9600 bps and transparent session resumption).
