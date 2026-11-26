# MS-DOS INTERLNK / INTERSVR Protocol Specification

> **Status:** `DRAFT / PROVISIONAL WIRE FORMAT`
>
> This document defines the formal clean-room protocol interface between Agent A (Analysis) and Agent B (Implementation). The initial frame layout corresponds to the bootstrap schema in [specs/interlnk_wire.ksy](../specs/interlnk_wire.ksy) and will be locked field-by-field as Golden Traces (`tests/traces/01_*.hexlog` .. `07_*.hexlog`) are captured and verified.

## 1. Transport Layer

### 1.1 Serial Transport (RS-232 / 8250 / 16450 / 16550A UART)

- **Supported Baud Rates:** `2400` (`/RCOPY` bootstrap), `9600`, `19200`, `38400`, `57600`, `115200` bps.
- **Line Parameters:** 8 data bits, No parity, 1 stop bit (`8N1`).
- **Flow Control:** Hardware RTS/CTS recommended at `>= 57600` bps on embedded targets (MAX3232 / ESP32); software/wire-level pacing on 8086/8088/NEC V30 targets.
- **Legacy PC COM1 Hardware Registers (Reference):**
  - `0x3F8`: THR (Write) / RBR (Read) / DLL (DLAB=1)
  - `0x3F9`: IER / DLM (DLAB=1)
  - `0x3FA`: IIR (Read) / FCR (Write)
  - `0x3FB`: LCR (Line Control Register)
  - `0x3FC`: MCR (Modem Control Register)
  - `0x3FD`: LSR (Line Status Register)
  - `0x3FE`: MSR (Modem Status Register)

### 1.2 Parallel Transport (Future Phase)

- 4-bit nibble mode / LapLink-compatible LPT cable transfer will be documented in a subsequent revision after serial Level 1-5 verification is complete.

## 2. Wire Frame Format

All multi-byte integers are **Little-Endian (`le`)**. The authoritative machine-readable definition is [specs/interlnk_wire.ksy](../specs/interlnk_wire.ksy).

| Offset | Size (Bytes) | Field | Type | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` | 2 | `sync_bytes` | `[0x55, 0xAA]` | Frame synchronization preamble |
| `0x02` | 1 | `frame_type` | `u1` (`frame_types`) | Command or response opcode |
| `0x03` | 1 | `sequence_number` | `u1` | Monotonic frame sequence counter (wraps `0xFF -> 0x00`) |
| `0x04` | 2 | `payload_length` | `u2` (LE) | Byte length `N` of `payload` |
| `0x06` | `N` | `payload` | Struct (by `frame_type`) | Command or response payload |
| `0x06 + N` | 2 | `checksum` | `u2` (LE) | Frame integrity checksum |

### 2.1 Checksum Specification (Provisional)

- **Algorithm:** CRC-16/CCITT-FALSE
  - Polynomial: `0x1021`
  - Initial value: `0xFFFF`
  - Reflect input / output: `false`
  - Final XOR: `0x0000`
- **Covered Byte Range:** `[frame_type, sequence_number, payload_length (2B LE), payload (N bytes)]` (offsets `0x02` through `0x05 + N` inclusive; excludes `sync_bytes` and `checksum` itself).
- **Wire Encoding:** 16-bit unsigned little-endian (`checksum_lo`, `checksum_hi`).
- *Note:* Once Agent A verifies the exact hardware wire checksum against Golden Traces using `analysis/verifiers/verify_crc.py`, this section and `specs/interlnk_wire.ksy` will be updated atomically.

## 3. Frame Types & Payloads

| Opcode | Enum Name | Direction | Payload Fields (Little-Endian) |
| :--- | :--- | :--- | :--- |
| `0x01` | `cmd_init` | Client -> Server | `client_version: u2`, `drive_count: u1` |
| `0x02` | `cmd_read_sector` | Client -> Server | `drive_index: u1`, `start_sector: u4`, `sector_count: u2` |
| `0x03` | `cmd_write_sector` | Client -> Server | `drive_index: u1`, `start_sector: u4`, `sector_count: u2`, `data: bytes[sector_count * 512]` |
| `0x81` | `resp_init_ack` | Server -> Client | `server_version: u2`, `drive_count: u1`, `status: u1` |
| `0x82` | `resp_read_data` | Server -> Client | `drive_index: u1`, `start_sector: u4`, `data: bytes[payload_length - 5]` |
| `0xFF` | `resp_error` | Server -> Client | `error_code: u1`, `detail: u2` |

### 3.1 Error Codes (`resp_error.error_code`)

| Code | Name | Meaning |
| :--- | :--- | :--- |
| `0x00` | `ERR_NONE` | No error |
| `0x01` | `ERR_MEDIA_CHANGED` | Removable media changed (`Media Check` return code `0x01`) |
| `0x02` | `ERR_WRITE_PROTECTED` | Target drive is write-protected |
| `0x03` | `ERR_INVALID_DRIVE` | Requested `drive_index` out of range |
| `0x04` | `ERR_SECTOR_OUT_OF_RANGE` | Requested sector range exceeds volume geometry |
| `0x05` | `ERR_IO_FAILURE` | Underlying block storage read/write error |
| `0x06` | `ERR_UNSUPPORTED_CMD` | Unrecognized or unsupported frame type |

## 4. Protocol Transactions & State Machine

### 4.1 Receiver State Machine (`libopenlnk`)

```
[SEARCH_SYNC] --(0x55, 0xAA)--> [READ_HEADER] --(4 bytes, len <= max)--> [READ_PAYLOAD]
      ^                               |                                        |
      |                               v (len > max)                            v (CRC err)
      +---------------------- [BUFFER_OVERFLOW] <--------------------- [CHECKSUM_MISMATCH]
                                                                               | (CRC ok)
                                                                               v
                                                                          [EXEC_CMD]
```

### 4.2 Golden Trace Verification Scenarios

1. **Handshake & Baud Negotiation (`01_handshake_9600.hexlog`, `02_handshake_115200.hexlog`):** Connection establishment at 9600, 19200, 38400, 57600, and 115200 bps, including `/V` timer/interrupt compatibility mode.
2. **Bootstrap Mode (`03_rcopy_bootstrap.hexlog`):** `INTERSVR /RCOPY` client transfer over COM port at 2400/9600 bps.
3. **Drive Enumeration (`04_bpb_query_fat16.hexlog`):** Retrieval of BIOS Parameter Block (BPB), Drive Parameter Block (DPB), and FAT12/FAT16 geometry.
4. **Sector Read Transactions (`05_read_sector_single.hexlog`):** 1 B, 511 B, 512 B (sector boundary), 4096 B (cluster boundary), and 64 KiB+ (real-mode segment boundary) reads.
5. **Sector Write & Error States (`06_write_sector_multi.hexlog`, `07_media_change_error.hexlog`):** Multi-sector writes, removable media change (`0x01`), write-protect errors, and disk-full handling.

## 5. DOS Block Device Driver Interface Notes (Clean Reference)

Standard documented MS-DOS 5.0/6.22 block device driver header (publicly documented in the MS-DOS Programmer's Reference):

- Offset `0x00` (`u4`): Far pointer to next device driver (`0xFFFFFFFF` for end of chain).
- Offset `0x04` (`u2`): Attribute flags (Bit 15 = `0` for block device, Bit 14 = IOCTL support, Bit 1 = 32-bit sector addressing).
- Offset `0x06` (`u2`): Near offset to Strategy routine.
- Offset `0x08` (`u2`): Near offset to Interrupt routine.
- Offset `0x0A` (`8 bytes`): Number of block units (byte 0) + reserved 7 bytes.
