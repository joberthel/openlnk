# Golden Traces Directory

This directory stores sanitized, verified Golden Traces captured in the 86Box + `serial_mitm.py` laboratory. All 7 traces are 100% deterministically decoded (**774,590 / 774,590 bytes**, **923 / 923 frames**, **0 CRC-16/GENIBUS errors**, **4,127 keepalives**) and formalized into [docs/protocol.md](../../docs/protocol.md), [specs/interlnk_wire.ksy](../../specs/interlnk_wire.ksy), and 24 differential JSON test vectors in `tests/fixtures/*.json`.

See [COMPATIBILITY.md](../../COMPATIBILITY.md) for per-trace byte/frame verification counts.

## Golden Trace Matrix

| Trace File | Baud Rate | Scenario |
| :--- | :---: | :--- |
| `01_handshake_9600.hexlog.gz` | `9600` | Initial connect & sync (`AA/00 55/FF 5A/11`), burst vs echo pacing, `0x00`/`0x01`/`0x1C` |
| `02_handshake_115200.hexlog.gz` | `115200` | Stepwise baud negotiation (`9600->115200`, 15-pair line test), `/V` echo pacing |
| `03_rcopy_bootstrap.hexlog.gz` | `2400`/`38400` | `INTERSVR /RCOPY` bare-PC bootstrap (`CTTY COM1`, `FXB.COM`, `CTTY CON`) |
| `04_bpb_query_fat16.hexlog.gz` | `115200` | Drive enumeration, `DIR D:`, `CHKDSK D:` (FAT16 BPB, Generic IOCTL `08h/66h` MID) |
| `05_read_sector_single.hexlog.gz` | `115200` | Sector reads (`512 B`, `16 KiB`, `65,024 B` / 127 sectors across 8× `0x40`-fragmented `8192 B` frames) |
| `06_write_sector_multi.hexlog.gz` | `115200` | Multi-sector writes (`16 KiB` `COPY` via 2× `8192 B` fragmented frames, `FC /B`, `DEL`) |
| `07_media_change_error.hexlog.gz` | `115200` | FAT12 BPB, media change (`0xFF`), write-protect (`0x8100`), not-ready (`0x8102`), keepalive timeout & re-sync |

Uncompressed `.hexlog`, `.bin`, and `.pcap` files are gitignored; decompress with:

```bash
gzip -dk tests/traces/*.hexlog.gz
```

## Line Format (`.hexlog`)

```text
[<unix_timestamp_ns>] [CLIENT->SERVER] aa
[<unix_timestamp_ns>] [SERVER->CLIENT] 00
[<unix_timestamp_ns>] [CLIENT->SERVER] 55
[<unix_timestamp_ns>] [SERVER->CLIENT] ff
[<unix_timestamp_ns>] [CLIENT->SERVER] 5a
[<unix_timestamp_ns>] [SERVER->CLIENT] 11
```
