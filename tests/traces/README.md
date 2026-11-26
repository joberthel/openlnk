# Golden Traces Directory

This directory stores sanitized, verified Golden Traces captured by Agent A in the 86Box + `serial_mitm.py` laboratory.

## Required Golden Trace Matrix

| Trace File | Baud Rate | Scenario |
| :--- | :--- | :--- |
| `01_handshake_9600.hexlog` | `9600` | Initial connect & baud negotiation at 9600 bps |
| `02_handshake_115200.hexlog` | `115200` | High-speed handshake at 115200 bps (including `/V` INT 2Fh hook mode) |
| `03_rcopy_bootstrap.hexlog` | `2400`/`9600` | `INTERSVR /RCOPY` bare-PC bootstrap transfer |
| `04_bpb_query_fat16.hexlog` | `115200` | Drive enumeration, BPB/DPB query, FAT12/FAT16 identification |
| `05_read_sector_single.hexlog` | `115200` | Sector reads across 1 B, 511 B, 512 B, 4096 B, and 64 KiB+ boundaries |
| `06_write_sector_multi.hexlog` | `115200` | Multi-sector write operations and read-back verification |
| `07_media_change_error.hexlog` | `115200` | Media change (`0x01`), write-protect, and disk-full error responses |

## Line Format (`.hexlog`)

```text
[<unix_timestamp_ns>] [CLIENT->SERVER] 55aa01000300220604abcd
[<unix_timestamp_ns>] [SERVER->CLIENT] 55aa81000400220601001234
```
