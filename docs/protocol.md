# MS-DOS 6.22 INTERLNK / INTERSVR Protocol Specification

> **Status:** `VERIFIED`
>
> This document is the formal, clean-room protocol interface specification handed off from Agent A (Analysis) to Agent B (Implementation). Every wire-layer constant, state transition, timing parameter, checksum, and RPC message structure has been deterministically verified against all 7 MS-DOS 6.22 Golden Traces (`tests/traces/01_handshake_9600.hexlog.gz` through `07_media_change_error.hexlog.gz` — **774,590 / 774,590 bytes**, **923 / 923 frames**, **0 CRC errors**).
>
> The authoritative machine-readable wire definition is [specs/interlnk_wire.ksy](../specs/interlnk_wire.ksy).

---

## 1. Scope & Layered Architecture

The MS-DOS 6.22 `INTERLNK` (client device driver) and `INTERSVR` (remote server) protocol consists of three cleanly separated layers:

1. **Physical / Serial UART Layer (RS-232, 8N1):** Point-to-point asynchronous serial transport supporting 1200–115200 bps over a 3-wire null-modem cable (TX, RX, GND; no hardware RTS/CTS required because flow control is handled in-band).
2. **Half-Duplex Link Layer:** Synchronisation handshake, stepwise baud-rate negotiation with 15-byte line quality testing, 2-bit sequence-numbered framing, adaptive echo-paced or burst byte transfer, CRC-16/GENIBUS integrity checking, multi-fragment reassembly (up to 8192 bytes per frame), half-duplex turnaround tokens, and 18.2 Hz idle keepalives.
3. **Application / DOS Device RPC Layer:** Synchronous half-duplex request/response transactions forwarding MS-DOS block device driver commands (`0x00`–`0x1D`), character/printer (`LPT1`–`LPT3`) operations, BIOS `INT 17h` printer calls, and Generic IOCTL Media ID queries.

Separately, **`INTERSVR /RCOPY`** implements a bare-PC bootstrap transfer over `CTTY COM1` + `FXB.COM` that reuses the exact same link-layer framing and CRC-16/GENIBUS algorithm (Section 5).

> **Out of Scope (Future Revision):** 4-bit parallel nibble mode over LapLink-compatible LPT cables is not specified in this revision.

---

## 2. Physical / Serial Transport Layer

### 2.1 Line Parameters & Hardware Reference

- **Framing:** 8 data bits, No parity, 1 stop bit (`8N1`).
- **Initial Connect Baud Rate:** Always `9600` bps (UART divisor `12` = `0x0C`).
- **Flow Control:** In-band byte-by-byte echo pacing (negotiated per frame; Section 3.3). Hardware RTS/CTS is not required on the wire, though embedded/host UART drivers should avoid overflowing 1-byte (8250/16450) or 16-byte (16550A) FIFOs.
- **PC COM Port Register Map (16-bit DOS Reference, e.g. COM1 base `0x3F8`):**
  - `base + 0`: THR (Write) / RBR (Read) / DLL (`DLAB=1`)
  - `base + 1`: IER / DLM (`DLAB=1`, always `0x00` for supported baud indices)
  - `base + 2`: IIR (Read) / FCR (Write)
  - `base + 3`: LCR (`0x83` to latch divisor, `0x03` for `8N1`)
  - `base + 4`: MCR
  - `base + 5`: LSR (Bit 0 = DR Data Ready, Bit 1 = OE Overrun, Bit 2 = PE Parity, Bit 3 = FE Framing, Bit 5 = THRE, Bit 6 = TEMT)

### 2.2 Baud-Rate Index Table

All baud-rate negotiation messages identify rates by a 1-byte `baud_index` (`0`–`7`), corresponding to standard PC 1.8432 MHz UART clock divisors (`divisor = 115200 / baud`):

| `baud_index` | Baud Rate (bps) | UART Divisor (`DLL`, `DLM=0`) | Notes |
| :---: | :---: | :---: | :--- |
| `0` | `1200` | `96` (`0x60`) | Lowest supported speed |
| `1` | `2400` | `48` (`0x30`) | `/RCOPY` `CTTY COM1` shell phase |
| `2` | `4800` | `24` (`0x18`) | — |
| `3` | `9600` | `12` (`0x0C`) | Initial connect & fallback floor |
| `4` | `19200` | `6` (`0x06`) | First step-up target |
| `5` | `38400` | `3` (`0x03`) | Also used by `FXB.COM` during `/RCOPY` |
| `6` | `57600` | `2` (`0x02`) | — |
| `7` | `115200` | `1` (`0x01`) | Maximum supported speed |

### 2.3 Timing Base

All protocol timeouts are derived from the PC BIOS 18.2 Hz timer tick (`~54.925 ms` per tick):
- **Sync step timeout:** `5` ticks (`~275 ms`)
- **Baud switch responder timeout:** `5` ticks (`~275 ms`); initiator timeout: `10` ticks (`~550 ms`)
- **Inter-byte receive timeout (in frame):** `5` ticks (`~275 ms`)
- **Sender byte-echo / final-ACK timeout:** `10` ticks (`~550 ms`)
- **Initial response wait (waiting for server RPC execution):** `401` ticks (`0x0191`, `~22.0 s`), dropping to `36` ticks (`0x0024`, `~2.0 s`) for subsequent fragments
- **Idle keepalive cadence:** Begins after `5` idle ticks (`~275 ms`) following an RPC; thereafter 1 poll per tick (`~54.9 ms`)
- **Keepalive disconnect threshold:** `> 10` consecutive missed echoes (`11–12` probes, `~605–660 ms`)

---

## 3. Link-Layer Protocol & State Machine

### 3.1 Phase A — Initial Sync Handshake (9600 bps)

When disconnected, both peers operate at **9600 bps** (`baud_index = 3`). The client probes the serial port with the 3-exchange synchronization sequence:

| Step | Client -> Server | Server -> Client | Description |
| :---: | :---: | :---: | :--- |
| 1 | `0xAA` | `0x00` | Sync probe 1 |
| 2 | `0x55` | `0xFF` | Sync probe 2 |
| 3 | `0x5A` | `0x11` | Sync probe 3 (link established) |

Upon completion of Step 3:
- Both peers reset the 2-bit link sequence counter `seq = 0`.
- Both peers clear `turnaround_pending = 0` (client holds the initial transmit turn).
- Client clears `echo_force = 0` and prepares to step up the baud rate (if its configured maximum baud index exceeds `3`).

### 3.2 Phase B — Baud-Rate Negotiation (Step-Up & Adaptive Step-Down)

Before transmitting its first data frame after connect (or after repeated link errors requiring a step-down), the active sender (initiator) negotiates a baud-rate change one index step at a time (e.g., `3 -> 4 -> 5 -> 6 -> 7` when connecting at `115200` bps; skipped completely when capped at `9600` bps):

1. **Baud-Change Request (at current baud rate):**
   - Initiator sends `~seq` (`0xFF`, `0xFE`, `0xFD`, or `0xFC`; at initial connect `seq = 0`, so `0xFF`).
   - Responder recognizes `(~byte & 0x03) == seq` and acknowledges with `seq` (`0x00` at initial connect).
2. **Target Baud Index & UART Switch:**
   - Initiator sends `target_baud_idx` (`0x00`..`0x07`, e.g., `0x04` for 19200 bps) and immediately switches its own UART to `target_baud_idx`.
   - Responder verifies `target_baud_idx <= local_max_baud` (if out of range, ignores/aborts), switches its own UART to `target_baud_idx`, and computes the 1-byte beacon:
     ```text
     beacon = (target_baud_idx << 4) | (seq & 0x03)
     ```
     (e.g., `0x40`, `0x50`, `0x60`, `0x70` when `seq = 0`).
3. **New-Baud Lock-On:**
   - Responder transmits `beacon` repeatedly (once per BIOS tick, up to 5 ticks) until it receives `beacon` echoed back from the initiator.
   - Once the initiator receives `beacon` at the new baud rate, it echoes `beacon` back to the responder.
   - Responder replies with `~beacon` (`0xBF`, `0xAF`, `0x9F`, `0x8F` for indices `4..7` at `seq = 0`).
4. **15-Pair Bidirectional Line Quality Test (at new baud rate):**
   Initiator and responder exchange 15 sequential byte pairs (each pair sent by initiator, verified and answered by responder before the next pair):

   | Index `k` | `0` | `1` | `2` | `3` | `4` | `5` | `6` | `7` | `8` | `9` | `10` | `11` | `12` | `13` | `14` |
   | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
   | **Initiator TX (`TEST_TX[k]`)** | `FF` | `00` | `5A` | `55` | `AA` | `F0` | `0F` | `E7` | `7E` | `C3` | `3C` | `81` | `18` | `00` | `FF` |
   | **Responder RX (`TEST_RX[k]`)** | `00` | `FF` | `A5` | `AA` | `55` | `F0` | `0F` | `FF` | `00` | `18` | `81` | `3C` | `C3` | `7E` | `E7` |

5. **Success or Fallback:**
   - If all 15 pairs match within timeout, the new baud rate is committed and the initiator either steps up to `target_baud_idx + 1` (up to `max_baud`) or proceeds directly to frame transmission.
   - If lock-on or any test byte times out during a **step-up**, both sides revert to the previous baud rate and clamp their session ceiling `max_baud = prev_baud_idx`.

### 3.3 Phase C — Frame Layout, Echo Pacing & Fragmentation

#### Canonical Link-Layer Frame Layout

De-interleaved from optional per-byte UART echo pacing, every frame has the exact layout defined in [specs/interlnk_wire.ksy](../specs/interlnk_wire.ksy):

| Offset | Size (Bytes) | Field | Type | Description |
| :--- | :---: | :--- | :--- | :--- |
| `0` | `1` | `header` | `u1` | Frame control flags + 2-bit sequence number |
| `1` | `1` or `2` | `length_short` / `length_long` | `u1` or `u2le` | Payload byte length `N` (`1..255` if bit 7=1; `1..8192` LE if bit 7=0) |
| `2` or `3` | `N` | `payload` | `bytes[N]` | Application RPC payload or data fragment |
| `2+N` or `3+N` | `2` | `crc16` | `u2be` | **Big-endian** CRC-16/GENIBUS over `[length bytes \|\| payload]` |

#### Header Byte Bitfield (`header`)

| Bits | Mask | Name | Meaning |
| :---: | :---: | :--- | :--- |
| `7` | `0x80` | `is_short_len` | `1` = 1-byte length (`u1`, payload `<= 255` B); `0` = 2-byte little-endian length (`u2le`, payload `>= 256` B) |
| `6` | `0x40` | `more_fragments` | `1` = logical message continues in next frame (`seq + 1`); `0` = final (or sole) fragment |
| `5..3` | `0x38` | `reserved` | Must be `0` |
| `2` | `0x04` | `echo_request` | `1` = sender forces byte-by-byte echo pacing (e.g. after a burst failure); `0` = receiver decides |
| `1..0` | `0x03` | `sequence_number` | Rolling 2-bit sequence counter (`0, 1, 2, 3, 0, ...`), incremented after each acknowledged frame |

#### Per-Frame Wire Exchange (Header ACK, Echo vs Burst Pacing, Final ACK)

1. **Header & Pacing Negotiation:**
   - Sender transmits `header`.
   - Receiver validates `(header & 0x03) == expected_seq`, then determines whether byte-by-byte echo pacing is active:
     ```text
     echo_bit = (header & 0x04) | (local_requires_echo ? 0x04 : 0x00)
     ack_hdr  = ((~header) & 0xFB) | echo_bit
     ```
     *Note:* `local_requires_echo` is enabled whenever `/V` (timer/interrupt compatibility) is active, when running under a multitasker/emulator, or when conservative pacing is desired. When `header` has bit 2 = `0`:
     - **Echo-Paced Mode (`echo_bit = 0x04`):** `ack_hdr == ~header` (i.e. `header ^ ack_hdr == 0xFF`).
     - **Burst (No-Echo) Mode (`echo_bit = 0x00`):** `ack_hdr == (~header) & 0xFB` (i.e. `header ^ ack_hdr == 0xFB`).
   - Sender verifies `(ack_hdr & 0xFB) == ((~header) & 0xFB)` and extracts `echo_mode = (ack_hdr & 0x04) != 0`.
2. **Length, Payload, and CRC Transmission:**
   - **Echo-Paced Mode (`echo_mode == true`):** For every byte of `length` (1 or 2 bytes), every byte of `payload` (`N` bytes), and **`crc_hi`** (first CRC byte), the sender transmits 1 byte and waits for the receiver to echo that exact byte before sending the next byte. For **`crc_lo`** (the final byte of the frame), the receiver does **not** echo `crc_lo` — it immediately evaluates the CRC and replies with `final_ack`.
   - **Burst Mode (`echo_mode == false`):** Sender streams all `length`, `payload`, `crc_hi`, and `crc_lo` bytes consecutively without waiting for echoes.
3. **Final Acknowledgement:**
   - If the receiver observes zero UART line errors (OE/PE/FE), valid payload length (`<= rx_buf_size`), and valid CRC residue (`0x1D0F`), the receiver transmits **`final_ack = header`** (exact copy of the received `header` byte) and advances `seq = (seq + 1) & 0x03`.
   - Upon receiving `final_ack == header`, the sender advances `seq = (seq + 1) & 0x03`.

#### Fragmentation (`more_fragments`)

Any logical application message or sector transfer whose byte length exceeds the negotiated `max_block_size` (typically `8192` bytes = `0x2000`) is split across consecutive frames in the same direction without an intervening turnaround byte:
- All chunks except the last set `more_fragments = 1` (`header |= 0x40`) and carry `max_block_size` bytes.
- The final chunk clears `more_fragments = 0` and carries the remaining bytes.
- Example (65,024-byte / 127-sector read from Trace 05): 7 frames of `8192` bytes (`header = 0x40..0x43`) + 1 final frame of `7680` bytes (`header = 0x00..0x03`).

### 3.4 Checksum Algorithm (CRC-16/GENIBUS)

- **Algorithm:** CRC-16/GENIBUS (bitwise complement of CRC-16/CCITT-FALSE)
  - Polynomial: `0x1021` ($x^{16} + x^{12} + x^5 + 1$)
  - Initial value: `0xFFFF`
  - Reflect input (`RefIn`): `false`
  - Reflect output (`RefOut`): `false`
  - Final XOR (`XorOut`): `0xFFFF`
- **Covered Bytes:** The encoded length field (`[length_short]` if `is_short_len`, else `[length_lo, length_hi]`) immediately followed by the `N` payload bytes. The `header` byte and the `crc16` field itself are **excluded**.
- **Wire Byte Order:** **Big-Endian (`u2be`)** — `crc_hi` first, `crc_lo` second (note that all other multi-byte integers in the protocol are Little-Endian!).
- **Receiver Check Residue:** Feeding `[length_bytes || payload || crc_hi || crc_lo]` through uncomplemented CRC-16/CCITT-FALSE (init `0xFFFF`, poly `0x1021`, no final XOR) always yields **`0x1D0F`** for every valid frame.

#### Verified Reference Test Vectors

| Description | Covered Input Bytes (Hex) | `crc16` (`u2be`) | Wire Bytes (Hex) | Residue |
| :--- | :--- | :---: | :---: | :---: |
| Standard ASCII `"123456789"` | `313233343536373839` | `0xD64E` | `d64e` | `0x1D0F` |
| Sync Trigger (`0x00`, `len=2`) | `020000` | `0x5D03` | `5d03` | `0x1D0F` |
| Get Drive Info `C:` (`0x1C`, `len=2`) | `021c02` | `0x3B5F` | `3b5f` | `0x1D0F` |
| Read Sector LBA `0x53` (`0x04`, `len=11`) | `0b0402f80100ffff53000000` | `0x704E` | `704e` | `0x1D0F` |

### 3.5 Half-Duplex Turnaround Token

Because the link is strictly half-duplex, peers synchronize direction reversals with a 1-byte **turnaround token**:
- When a peer has just finished **sending** (last frame acknowledged, `seq` incremented to `next_seq`) and transitions to **receiving**, it transmits a single byte equal to **`next_seq`** (`0x00`, `0x01`, `0x02`, or `0x03`) to signal that it is now listening.
- When the other peer has just finished **receiving** and transitions to **sending**, it first waits to receive that exact **`next_seq`** turnaround byte before sending its next frame `header` (or baud-change request).
- No turnaround byte is sent between consecutive frames in the **same** direction (e.g., between `0x04` Read response header and its sector data frames, between `0x05` Write header and its sector data frames, or between `more_fragments` chunks).

### 3.6 Idle Keepalives & Link Recovery

- **Idle Polling:** While connected and idle (5 BIOS ticks / `~275 ms` after the last RPC completes, after the server has sent its turnaround byte `next_seq`), the client sends a 1-byte keepalive poll once per BIOS timer tick (`~54.9 ms`):
  ```text
  keepalive_byte = 0xFC | ((expected_seq & 0x03) ^ 0x01)
  ```
  | `expected_seq` | `0` | `1` | `2` | `3` |
  | :---: | :---: | :---: | :---: | :---: |
  | **`keepalive_byte`** | `0xFD` | `0xFC` | `0xFF` | `0xFE` |
- **Server Echo:** Whenever the server is idle waiting for the next client command and receives `keepalive_byte`, it immediately echoes the identical byte back to the client without modifying `expected_seq`.
- **Disconnect & Re-Sync:** If the client misses more than 10 consecutive keepalive echoes (`11–12` polls, `~605–660 ms`), it drops the link to disconnected state at 9600 bps. The next DOS device request initiates a full Phase A sync (`AA/00 55/FF 5A/11`), Phase B baud negotiation, and `0x00` Session Sync (which transparently resumes the session if `(client_id, server_id)` still match).

### 3.7 Frame Error Recovery & Adaptive Step-Down

- **Receiver on Error (Silent Drain):** If CRC residue `!= 0x1D0F`, UART reports OE/PE/FE, or `len_payload` exceeds buffer capacity, the receiver sends **no NACK byte**. Instead, it drains all incoming bytes until a 5-tick (`~275 ms`) silence timeout expires, then re-enters frame-header wait with `expected_seq` unchanged.
- **Sender on Timeout (10 ticks / `~550 ms`):**
  1. If the failed attempt used **Burst Mode** (`echo_mode == false`), the sender immediately sets `echo_request = 1` (`0x04`) and retries the same frame (same `seq`) at the current baud rate.
  2. If the failed attempt already used **Echo Mode**, the sender retries once (`retry_count < 2`).
  3. After 2 failures in Echo Mode, if `current_baud_idx > 3` (`> 9600` bps), the sender initiates a Phase B baud step-down to `current_baud_idx - 1`, resets `echo_request = 0`, and retries the frame. If already at `<= 9600` bps, the link is declared lost.

### 3.8 Link-Layer State Machine Diagram

```text
  [DISCONNECTED @ 9600]
          |
          |  C: AA -> S: 00 -> C: 55 -> S: FF -> C: 5A -> S: 11
          v
  [BAUD_NEGOTIATE] <---(2x Echo Fail & baud > 9600: step down)---+
          |  (step up 3->4->5->6->7 via FF/00 + beacon + 15-pair |
          |   test; skipped if max_baud == 9600)                 |
          v                                                      |
  [CONNECTED / IDLE] <====> (C: 0xFC..0xFF <-> S: echo @ 18.2Hz) |
     |         ^                                                 |
     | (RPC    | (Turnaround `next_seq` + >10 missed KA -> drop)  |
     |  start) |                                                 |
     v         |                                                 |
  [TX_FRAME] --+--> [WAIT_FINAL_ACK] --(timeout/CRC err)---------+
     |                     |
     | (multi-frame)       | (final_ack == hdr; seq = (seq+1)&3)
     +---------------------+---> [TURNAROUND: send `seq`] -> [RX_FRAME]
```

---

## 4. Application-Layer RPC Protocol (`0x00`–`0x1D`)

All multi-byte integer fields in application payloads are **Little-Endian (`le`)**.

### 4.1 Complete Opcode Directory

The server dispatch table covers opcodes `0x00` through `0x1D` (30 slots). Opcodes `0x08`, `0x09`, `0x0C`, `0x0D`, `0x0F`, and `0x13` are unimplemented/reserved and cause the server to reject the request.

| Opcode | Name | Request Payload | Response Payload | MS-DOS / BIOS Mapping |
| :---: | :--- | :--- | :--- | :--- |
| `0x00` | `SESSION_SYNC` | `2B` trigger (`00 00`) + `34B` init struct | `33B` (or `161B`) sync response | Session & max-block negotiation |
| `0x01` | `MAP_CONFIG` | `36B` client DOS ver & drive/LPT map | `87B` server drive/LPT map & attrs | Initial device enumeration |
| `0x02` | `MEDIA_CHECK` | `3B` (`op, unit, media_desc`) | `19B` (`status, media_st, label[12], serial`) | Block Driver Cmd `0x01` |
| `0x03` | `BUILD_BPB` | `3B` (`op, unit, media_desc`) | `2B` (error) or `33B` (`status, bpb[25], ext[6]`) | Block Driver Cmd `0x02` |
| `0x04` | `READ_SECTORS` | `11B` (`op, unit, media, cnt, lba16, lba32`) | `4B`/`20B` hdr + `cnt * bps` data frame(s) | Block Driver Cmd `0x04` |
| `0x05` | `WRITE_SECTORS` | `11B` hdr + `cnt * bps` data frame(s) | `4B`/`20B` (`status, written[, vol_id]`) | Block Driver Cmd `0x08` |
| `0x06` | `WRITE_VERIFY` | `11B` hdr + `cnt * bps` data frame(s) | `4B`/`20B` (`status, written[, vol_id]`) | Block Driver Cmd `0x09` |
| `0x07` | `DRIVE_INCOMPAT` | `5B` (`07, FF, cli_unit, 00, srv_unit`) | *(none — one-way notice)* | Incompatible remote drive notice |
| `0x0A` | `LPT_WRITE` | `5B` hdr (+ data frame if `count > 1`) | `4B` (`status, bytes_written`) | Char Driver Cmd `0x08` |
| `0x0B` | `LPT_WRITE_VERIFY` | `5B` hdr (+ data frame if `count > 1`) | `4B` (`status, bytes_written`) | Char Driver Cmd `0x09` |
| `0x0E` | `LPT_OUTPUT_STATUS` | `2B` (`0E, lpt_idx`) | `2B` (`status`) | Char Driver Cmd `0x0A` |
| `0x10` | `LPT_OUTPUT_FLUSH` | `2B` (`10, lpt_idx`) | `2B` (`status`) | Char Driver Cmd `0x0B` |
| `0x11` | `LPT_DEVICE_OPEN` | `2B` (`11, lpt_idx`) | `2B` (`status`) | Char Driver Cmd `0x0D` |
| `0x12` | `LPT_DEVICE_CLOSE` | `2B` (`12, lpt_idx`) | `2B` (`status`) | Char Driver Cmd `0x0E` |
| `0x14` | `LPT_AUX_WRITE` | `5B` hdr (+ data frame if `count > 1`) | `4B` (`status, bytes_written`) | Char output alias |
| `0x15` | `LPT_OUTPUT_BUSY` | `5B` hdr (+ data frame if `count > 1`) | `4B` (`status, bytes_written`) | Char Driver Cmd `0x10` |
| `0x16` | `BLK_DEVICE_OPEN` | `2B` (`16, unit`) | `2B` (`status`) | Block Driver Cmd `0x0D` |
| `0x17` | `BLK_DEVICE_CLOSE` | `2B` (`17, unit`) | `2B` (`status`) | Block Driver Cmd `0x0E` |
| `0x18` | `BLK_DEVICE_CLOSE_ALT` | `2B` (`18, unit`) | `2B` (`status`) | Block Driver Cmd `0x0E` alias |
| `0x19` | `BIOS_PRN_WRITE` | `3B` (`19, lpt_idx, char`) | `1B` (`bios_status`) | BIOS `INT 17h AH=00h` |
| `0x1A` | `BIOS_PRN_INIT` | `2B` (`1A, lpt_idx`) | `1B` (`bios_status`) | BIOS `INT 17h AH=01h` |
| `0x1B` | `BIOS_PRN_STATUS` | `2B` (`1B, lpt_idx`) | `1B` (`bios_status`) | BIOS `INT 17h AH=02h` |
| `0x1C` | `GET_DRIVE_INFO` | `2B` (`1C, unit`) | `23B` (`size_str[10], label[13]`) | Status display query |
| `0x1D` | `GENERIC_IOCTL` | `4B` (`1D, unit, 08, 46\|66`) [+ `25B` MID if `46`] | `2B` (`status`) [+ `25B` MID if `66` & ok] | Block Driver Cmd `0x13` |

---

### 4.2 Detailed Command & Response Layouts

#### `0x00` — Session Sync & Reconnection Handshake

Sent immediately after every Phase A/B link establishment (both initial connect and post-timeout re-sync):

1. **Client Frame 1 (2 bytes, no turnaround after):** `0x00, 0x00` (`msg_sync_trigger`).
2. **Client Frame 2 (34 bytes, followed by turnaround):** `msg_session_sync_req`:

   | Offset | Size | Field | Typical Value |
   | :---: | :---: | :--- | :--- |
   | `0x00` | 6 | `reserved_head` | `00 00 00 00 00 00` |
   | `0x06` | 2 | `client_version` (`u2le`) | `0x0100` (`00 01`) |
   | `0x08` | 1 | `proto_major` (`u1`) | `0x01` |
   | `0x09` | 1 | `ext_info_flag` (`u1`) | `0x00` (if `0x01`, server sends 161 B instead of 33 B) |
   | `0x0A` | 1 | `reserved_mid` (`u1`) | `0x00` |
   | `0x0B` | 1 | `capability` (`u1`) | `0x01` |
   | `0x0C` | 2 | `max_block_size` (`u2le`) | `0x2000` (`8192` bytes) |
   | `0x0E` | 12 | `reserved_pad` | All zeros |
   | `0x1A` | 4 | `client_id` (`u4le`) | Non-zero 32-bit session ID generated by client |
   | `0x1E` | 4 | `server_id` (`u4le`) | `0x00000000` on first connect, or saved server ID on reconnect |

3. **Server Response (33 bytes, or 161 bytes if `ext_info_flag == 1`):** `msg_session_sync_resp`:

   | Offset | Size | Field | Typical Value |
   | :---: | :---: | :--- | :--- |
   | `0x00` | 6 | `reserved_head` | `00 00 00 00 00 00` |
   | `0x06` | 2 | `server_version` (`u2le`) | `0x010A` (`0a 01`) |
   | `0x08` | 1 | `ready_flag` (`u1`) | `0x01` (client aborts if `0x00`) |
   | `0x09` | 1 | `reserved_mid` (`u1`) | `0x00` |
   | `0x0A` | 1 | `capability` (`u1`) | `0x01` |
   | `0x0B` | 2 | `max_block_size` (`u2le`) | `min(client.max_block_size, server.max_block_size)` (`0x2000`) |
   | `0x0D` | 12 | `reserved_pad` | All zeros |
   | `0x19` | 4 | `prev_client_id` (`u4le`) | `0x00000000` on new client, or matching `client_id` on reconnect |
   | `0x1D` | 4 | `server_id` (`u4le`) | Non-zero 32-bit session ID generated by server |

- **Session Resumption Rule:** If `req.client_id == resp.prev_client_id` AND `req.server_id == resp.server_id`, the prior drive/printer mapping remains valid and the client skips `0x01` `MAP_CONFIG`. Otherwise, both sides treat it as a fresh session, store the peer's ID, and the client immediately issues `0x01` `MAP_CONFIG`.

#### `0x01` — Drive & Printer Mapping Negotiation

- **Request (36 bytes, `msg_map_config_req`):**
  - `0x00` (`u1`): `0x01`
  - `0x01` (`u2le`): `client_proto_ver` (`0x0001`)
  - `0x03` (`u1`): `client_dos_minor` (e.g. `22` = `0x16` for MS-DOS 6.22)
  - `0x04` (`u1`): `client_dos_major` (e.g. `6` = `0x06` for MS-DOS 6.22)
  - `0x05` (`u1`): `max_client_drives` (number of redirected drive letters configured on client, e.g. `4`)
  - `0x06` (`u1`): `first_client_drive` (0-based letter of first redirected drive, e.g. `3` = `D:`)
  - `0x07` (`u1[3]`): `lpt_map_req` for `LPT1..LPT3` (`0..2` = explicit server LPT, `0xFE` = auto-assign, `0xFF` = unmapped/local)
  - `0x0A` (`u1[26]`): `drive_map_req` for client slots `0..25` (`0..25` = explicit server drive `A:..Z:`, `0xFE` = auto-assign, `0xFF` = unmapped)
- **Response (87 bytes, `msg_map_config_resp`):**
  - `0x00` (`u1`): `server_proto_major` (`0x01`)
  - `0x01` (`u1`): `server_proto_minor` (`0x0A`)
  - `0x02` (`u1`): `server_dos_minor` (`0x16` for DOS 6.22)
  - `0x03` (`u1`): `server_dos_major` (`0x06` for DOS 6.22)
  - `0x04` (`u1`): `server_drive_count` (total enumerated drives on server, e.g. `3` for `A:, B:, C:`)
  - `0x05` (`u1[3]`): `lpt_map_resp` (`0..2` assigned server LPT, `0xFF` = unmapped)
  - `0x08` (`u1[26]`): `drive_map_resp` — for each client slot `0..25`: bits `6..0` hold the 0-based server drive index (`0=A:`, `2=C:`), bit `7` (`0x80`) marks read-only/write-protected, `0xFE`/`0xFF` = unmapped.
  - `0x22` (`u2le[26]`): `server_drive_attrs` — 26 MS-DOS Block Device Driver attribute words (e.g. `0x08C2` = Open/Close/RM `0x0800` | Generic IOCTL `0x0040` | 32-bit LBA `0x0002` | `0x0080`; unused slots zeroed).
  - `0x56` (`u1`): `server_flags` — non-zero if server operates in timer/echo compatibility mode.

#### `0x02` — Media Check (MS-DOS Block Driver Cmd `0x01`)

- **Request (3 bytes):** `[0x02, unit: u1, media_descriptor: u1]`
- **Response (19 bytes):**
  - `0x00` (`u2le`): `status` (MS-DOS Request Header status word; Section 4.3)
  - `0x02` (`u1`): `media_status`:
    - `0x01` (`+1`): Media unchanged
    - `0x00` (`0`): Unsure (DOS will call `BUILD_BPB`)
    - `0xFF` (`-1`): Media changed (DOS invalidates buffers and calls `BUILD_BPB`)
  - `0x03` (`bytes[12]`): `volume_label` — null-terminated ASCII volume label (up to 11 chars + NUL)
  - `0x0F` (`u4le`): `volume_serial` — 32-bit volume serial number

#### `0x03` — Build BPB (MS-DOS Block Driver Cmd `0x02`)

- **Request (3 bytes):** `[0x03, unit: u1, media_descriptor: u1]`
- **Response:**
  - On error (`(status & 0x8000) != 0`): **2 bytes** `[status: u2le]`.
  - On success: **33 bytes**:
    - `0x00` (`u2le`): `status`
    - `0x02` (`25 bytes`, `dos_bpb`): Standard MS-DOS 5.0/6.22 BIOS Parameter Block:
      - `bytes_per_sector: u2le` (`512`)
      - `sectors_per_cluster: u1` (`1` for 1.44 MB FAT12, `2` for 10 MB FAT16)
      - `reserved_sectors: u2le` (`1`)
      - `num_fats: u1` (`2`)
      - `root_entries: u2le` (`224` for 1.44 MB FAT12, `512` for FAT16)
      - `total_sectors_16: u2le` (`2880` for 1.44 MB, `20791` for 10 MB, or `0` if `> 65535`)
      - `media_descriptor: u1` (`0xF0` 1.44 MB floppy, `0xF8` fixed disk)
      - `sectors_per_fat: u2le` (`9` for 1.44 MB FAT12, `41` for 10 MB FAT16)
      - `sectors_per_track: u2le` (`18` floppy, `17` fixed disk)
      - `num_heads: u2le` (`2` floppy, `6` fixed disk)
      - `hidden_sectors: u4le`
      - `total_sectors_32: u4le` (used when `total_sectors_16 == 0`)
    - `0x1B` (`6 bytes`): `bpb_ext` — 6-byte extended device BPB tail (may be zero-filled by clean-room servers).

#### `0x04` — Read Sectors (MS-DOS Block Driver Cmd `0x04`)

- **Request (11 bytes, `msg_sector_io_req`):**
  - `0x00` (`u1`): `0x04`
  - `0x01` (`u1`): `unit` (server drive index)
  - `0x02` (`u1`): `media_descriptor`
  - `0x03` (`u2le`): `sector_count` (observed up to `127` sectors = `65,024` bytes in a single MS-DOS real-mode request)
  - `0x05` (`u2le`): `start_sector_16` (`0xFFFF` whenever 32-bit sector addressing is enabled via attribute bit 1 on DOS `>= 4.0`)
  - `0x07` (`u4le`): `start_sector_32` (32-bit starting LBA; authoritative when `start_sector_16 == 0xFFFF`)
- **Response (Server -> Client):**
  1. **Status Header Frame (4 bytes, or 20 bytes on error `0x0F`):**
     - `0x00` (`u2le`): `status`
     - `0x02` (`u2le`): `sectors_transferred` (`0` on error)
     - `0x04` (`16 bytes`, optional): `volume_label[12]` + `volume_serial: u4le` — included **only** when `(status & 0x00FF) == 0x0F` (Invalid Disk Change).
  2. **Sector Data Frame(s) (no turnaround between status header and data):**
     - Sent **only** if `(status & 0x8000) == 0` AND `sectors_transferred > 0`.
     - Total length is `sectors_transferred * bytes_per_sector`, fragmented into `<= negotiated_max_block_size` (`8192` B) link frames using `more_fragments` (`0x40`).

#### `0x05` — Write Sectors & `0x06` — Write Sectors with Verify (MS-DOS Cmds `0x08`, `0x09`)

- **Request (Client -> Server):**
  1. **Header Frame (11 bytes, no turnaround after):** Identical layout to `0x04` `msg_sector_io_req`, with opcode `0x05` (Write) or `0x06` (Write with Verify).
  2. **Sector Data Frame(s):** `sector_count * bytes_per_sector` bytes, fragmented into `<= negotiated_max_block_size` (`8192` B) link frames using `more_fragments` (`0x40`).
- **Response (4 bytes, or 20 bytes on error `0x0F`):** Identical to `0x04` `msg_sector_io_resp` (`status: u2le`, `sectors_transferred: u2le`, optional `volume_id[16]`). On a write-protected medium, server returns `status = 0x8100` (`ERROR | DONE | WRITE_PROTECT`) and `sectors_transferred = 0`.

#### `0x1C` — Get Drive Info

- **Request (2 bytes):** `[0x1C, unit: u1]`
- **Response (23 bytes):**
  - `0x00` (`bytes[10]`): `size_string` — null-terminated ASCII formatted drive capacity (e.g. `"(10Mb)\0\0\0\0"`, or all zeros for unmounted/removable drives without cached size).
  - `0x0A` (`bytes[13]`): `volume_label` — null-terminated ASCII volume label (up to 11/12 chars + NUL).

#### `0x1D` — Generic IOCTL (MS-DOS Block Driver Cmd `0x13`)

Only category `0x08` (Disk) and functions `0x46` (Set Media ID) and `0x66` (Get Media ID) are forwarded; all other IOCTL categories/functions return `0x8103` (Unknown Command).
- **Request Header (4 bytes):** `[0x1D, unit: u1, category: u1 (0x08), function: u1 (0x46 | 0x66)]`
  - For `function == 0x46` (**Set Media ID**): Client immediately follows (no turnaround) with a 25-byte `msg_generic_ioctl_mid` frame. Server replies with a 2-byte `status: u2le` frame.
  - For `function == 0x66` (**Get Media ID**): Server replies with a 2-byte `status: u2le` frame; if `(status & 0x8000) == 0`, server immediately follows (no turnaround) with a 25-byte `msg_generic_ioctl_mid` frame:
    - `0x00` (`u2le`): `info_level` (`0x0000`)
    - `0x02` (`u4le`): `serial_number` (32-bit volume serial)
    - `0x06` (`bytes[11]`): `volume_label` (space-padded 11-byte ASCII label, e.g. `"DOS622_CLI "`)
    - `0x11` (`bytes[8]`): `filesystem_type` (space-padded 8-byte ASCII, e.g. `"FAT16   "`, `"FAT12   "`)

#### Additional Block, Printer & BIOS Opcodes

- **`0x07` (`DRIVE_INCOMPAT`, 5 B one-way C->S):** `[0x07, 0xFF, client_unit, 0x00, server_unit]`, no server response.
- **`0x16` / `0x17` / `0x18` (`BLK_DEVICE_OPEN` / `CLOSE`):** `[op, unit]` (2 B) -> `[status: u2le]` (2 B). Only issued when drive attribute bit 11 (`0x0800`) is set.
- **`0x0A` / `0x0B` / `0x14` / `0x15` (`LPT_WRITE`):** `[op, lpt_idx, byte_count: u2le, inline_byte]` (5 B). If `byte_count == 1`, `inline_byte` is the sole byte (no data frame); if `byte_count > 1`, client immediately follows with a `byte_count`-byte data frame. Response: `[status: u2le, bytes_written: u2le]` (4 B).
- **`0x0E` / `0x10` / `0x11` / `0x12` (`LPT_OUTPUT_STATUS` / `FLUSH` / `OPEN` / `CLOSE`):** `[op, lpt_idx]` (2 B) -> `[status: u2le]` (2 B).
- **`0x19` / `0x1A` / `0x1B` (`BIOS_PRN_WRITE` / `INIT` / `STATUS`):** `[0x19, lpt_idx, ch]` (3 B) or `[0x1A|0x1B, lpt_idx]` (2 B) -> `[bios_status: u1]` (1 B, standard BIOS `INT 17h AH` status).

---

### 4.3 MS-DOS Status Word & Error Codes

Every block/character RPC `status` (`u2le`) uses the standard MS-DOS Device Driver Request Header status encoding:

- **Bit 15 (`0x8000`):** `ERROR` (`1` = operation failed; bits `7..0` hold the DOS error code)
- **Bit 9 (`0x0200`):** `BUSY`
- **Bit 8 (`0x0100`):** `DONE` (always set on completion; note that when Bit 15 = `0`, bits `7..0` are ignored by DOS and may carry residual low-byte values such as `0x00`, `0x02`, or `0x09`)

| `status` (Error) | Low Byte | Name | Meaning |
| :---: | :---: | :--- | :--- |
| `0x8100` | `0x00` | `WRITE_PROTECT` | Write attempted on write-protected media or read-only drive |
| `0x8101` | `0x01` | `UNKNOWN_UNIT` | Invalid/unmapped drive or printer unit index |
| `0x8102` | `0x02` | `DRIVE_NOT_READY` | No disk in drive / door open / link lost |
| `0x8103` | `0x03` | `UNKNOWN_COMMAND` | Unsupported command or IOCTL function |
| `0x8104` | `0x04` | `CRC_ERROR` | Media CRC / data error |
| `0x8105` | `0x05` | `BAD_REQUEST_LENGTH` | Invalid request length |
| `0x8106` | `0x06` | `SEEK_ERROR` | Seek failure |
| `0x8107` | `0x07` | `UNKNOWN_MEDIA` | Unsupported/incompatible media or sector size |
| `0x8108` | `0x08` | `SECTOR_NOT_FOUND` | LBA out of range |
| `0x8109` | `0x09` | `PRINTER_OUT_OF_PAPER` | Printer paper out |
| `0x810A` | `0x0A` | `WRITE_FAULT` | Write fault / byte-count mismatch |
| `0x810B` | `0x0B` | `READ_FAULT` | Read fault / zero or overflow sector count |
| `0x810C` | `0x0C` | `GENERAL_FAILURE` | General device failure |
| `0x810F` | `0x0F` | `INVALID_DISK_CHANGE` | Unexpected media swap during I/O (16 B volume ID appended) |

---

## 5. `/RCOPY` Bootstrap Protocol (`FXB`)

`INTERSVR /RCOPY` copies `INTERSVR.EXE` and `INTERLNK.EXE` onto a bare DOS PC that has no INTERLNK software installed yet. It operates in three strictly ordered phases (verified 100% against `tests/traces/03_rcopy_bootstrap.hexlog.gz`):

### Phase 1 — `CTTY COM1` Shell Setup & `FXB.COM` Upload (2400 bps)

1. The user configures the target PC with `MODE COM1:2400,N,8,1` and `CTTY COM1`.
2. The host sends `0x03` (`Ctrl-C`) pulses until the target `COMMAND.COM` echoes `^C` and its prompt (`C:\...>`).
3. The host types:
   - `echo Loading bootstrap > CON\r`
   - `COPY COM1: FXB.COM\r`
4. The host streams the raw 784-byte `FXB.COM` helper executable terminated by ASCII `0x1A` (`Ctrl-Z` EOF) and waits for DOS to print `1 file(s) copied` and the prompt.
5. The host sends `0x03` (`Ctrl-C`) followed by `fxb\r` to execute `FXB.COM`.

### Phase 2 — `FXB` Binary Transfer (38400 bps, Burst Mode)

Upon launch, `FXB.COM` disables Ctrl-Break, switches the UART to **38400 bps** (`divisor = 3`), and receives unidirectional Server -> Client link-layer frames using the **exact same framing and CRC-16/GENIBUS** as Section 3.3, always replying in **Burst (No-Echo) Mode** (`ack_hdr = (~hdr) & 0xFB`, so `hdr ^ ack_hdr == 0xFB`):

| `fxb_op` | Name | Payload Layout | Description |
| :---: | :--- | :--- | :--- |
| `0x05` | `PING_OR_MSG` | `[0x05, ascii_z]` (e.g. `05 00`) | Link check / console message display |
| `0x01` | `OPEN_FILE` | `183 bytes`: `op(1)`, `progress_msg(80)`, `error_msg(80)`, `attr(1)`, `dos_time(2 LE)`, `dos_date(2 LE)`, `file_size(4 LE)`, `filename(13)` | Create target file and display `progress_msg` on CON |
| `0x02` | `WRITE_DATA` | `185 + chunk_len` bytes: same 183 B header + `len_chunk_data(2 LE)` + `chunk_data(bytes)` | Write up to 2048 B (max 8192 B) chunk and print progress update |
| `0x03` | `CLOSE_FILE` | `183 bytes` (same header layout) | Apply `dos_time`/`dos_date` (`INT 21h/5701h`) and close file |
| `0x04` | `DONE` | `1 byte`: `[0x04]` | Exit loop, delete `FXB.COM`, restore **2400 bps** (`divisor = 0x30`), return to `COMMAND.COM` |

### Phase 3 — Console Restoration (2400 bps)

After `FXB.COM` exits and the DOS prompt reappears at 2400 bps, the host sends `0x03` (`Ctrl-C`) followed by `ctty con\r`, returning console control to the target PC's keyboard and screen.

---

## 6. Clean-Room Implementation Notes for Agent B (`libopenlnk` & `opensvr`)

1. **Zero-Allocation Static Buffers:** A single static frame buffer of `8192 + 5 = 8197` bytes (header `1B` + length `2B` + max payload `8192B` + CRC `2B`) accommodates every link-layer frame across the entire protocol (including 64 KiB+ multi-fragment reads/writes, which stream sequentially in `<= 8192`-byte fragments).
2. **Endianness Discipline:** Frame `length_long` and all application-layer RPC fields (`u2le`/`u4le`) are **Little-Endian**. Only **`crc16` (`u2be`)** is transmitted **Big-Endian** (MSB first, LSB second).
3. **Echo-Mode Compatibility:** Clean-room server implementations (`opensvr`) should support both Echo-Paced (`ack_hdr = ~hdr`) and Burst (`ack_hdr = (~hdr) & 0xFB`) reception. Enabling `local_requires_echo = true` (always echoing `~hdr`) guarantees rock-solid interoperability on 115200 bps links, virtual serial ports, and ESP32/USB-UART bridges without requiring hardware flow control.
4. **32-Bit Sector Addressing:** Advertise attribute bit 1 (`0x0002`) in `server_drive_attrs` (e.g. `0x08C2` for DOS `>= 4.0` volumes) and compute `lba = (start_sector_16 == 0xFFFF) ? start_sector_32 : start_sector_16`.
5. **Removable Media & Error Codes:** Return `media_status = 0xFF` on the first `0x02` `MEDIA_CHECK` after a disk swap or volume change, `status = 0x8100` on writes to read-only media, and `status = 0x8102` when a removable medium is absent.
6. **Verified Test Vectors:** Validate `libopenlnk` and `opensvr` against all 24 JSON fixtures in `tests/fixtures/*.json` via `python3 tests/harness/test_runner.py --require-kaitai`.
