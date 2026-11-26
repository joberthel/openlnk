# Golden Traces Directory

This directory stores sanitized, verified Golden Traces captured by Agent A in the 86Box + `serial_mitm.py` laboratory.

## Required Golden Trace Matrix

| Trace File | Baud Rate | Scenario |
| :--- | :--- | :--- |
| `01_handshake_9600.hexlog.gz` | `9600` | Initial connect & baud negotiation at 9600 bps |
| `02_handshake_115200.hexlog.gz` | `115200` | High-speed handshake at 115200 bps (including `/V` INT 2Fh hook mode) |
| `03_rcopy_bootstrap.hexlog.gz` | `2400`/`115200` | `INTERSVR /RCOPY` bare-PC bootstrap transfer |
| `04_bpb_query_fat16.hexlog.gz` | `115200` | Drive enumeration, BPB/DPB query, FAT12/FAT16 identification |
| `05_read_sector_single.hexlog.gz` | `115200` | Sector reads across 1 B, 511 B, 512 B, 4096 B, and 64 KiB+ boundaries |
| `06_write_sector_multi.hexlog.gz` | `115200` | Multi-sector write operations and read-back verification |
| `07_media_change_error.hexlog.gz` | `115200` | Media change (`0x01`), write-protect, and not-ready error responses |

Uncompressed `.hexlog`, `.bin`, and `.pcap` files are gitignored; decompress with:

```bash
gzip -dk tests/traces/*.hexlog.gz
```

## Line Format (`.hexlog`)

```text
[<unix_timestamp_ns>] [CLIENT->SERVER] 55aa01000300220604abcd
[<unix_timestamp_ns>] [SERVER->CLIENT] 55aa81000400220601001234
```
