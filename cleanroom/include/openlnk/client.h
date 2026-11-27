/* cleanroom/include/openlnk/client.h
 *
 * Platform-independent, zero-allocation MS-DOS 6.22 INTERLNK-compatible
 * client RPC engine built on libopenlnk.
 *
 * Interoperates directly with original MS-DOS 6.22 INTERSVR.EXE and
 * clean-room opensvr across POSIX serial/pipe, Windows, 16-bit DOS, and ESP32.
 *
 * Strictly ISO C99, zero dynamic memory allocation, zero floating-point.
 */
#ifndef OPENLNK_CLIENT_H
#define OPENLNK_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openlnk/openlnk.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OLNK_CLIENT_DEFAULT_ID       0x4F4C4331UL /* "OLC1" */
#define OLNK_CLIENT_DEFAULT_RESP_MS  5000U        /* Wait for server RPC execution */
#define OLNK_CLIENT_SYNC_POLL_MS     275U         /* 5 BIOS ticks per sync attempt */

#ifndef OLNK_CLIENT_CTRL_BUF_SIZE
#define OLNK_CLIENT_CTRL_BUF_SIZE    256U
#endif

typedef struct {
    olnk_ctx_t link;
    uint8_t ctrl_buf[OLNK_CLIENT_CTRL_BUF_SIZE];

    /* Last negotiated drive & printer mapping from opcode 0x01 */
    olnk_map_config_resp_t map;
    bool has_map;

    /* Configurable RPC response wait timeout (ms) */
    uint32_t rpc_timeout_ms;

    /* Telemetry counters */
    uint32_t rpc_count;
    uint32_t sectors_read_total;
    uint32_t sectors_written_total;
} olnk_client_t;

/**
 * Initialize caller-allocated client context (keep static on 16-bit DOS!).
 */
olnk_status_t olnk_client_init(olnk_client_t *cli, olnk_transport_t transport);

/**
 * Drop link back to OLNK_LINK_DISCONNECTED at 9600 bps (preserves session IDs).
 */
void olnk_client_disconnect(olnk_client_t *cli);

/**
 * Phase A Sync (with up to sync_retries attempts, >= 1) + Phase B stepwise
 * baud negotiation up to cli->link.max_baud_idx.
 */
olnk_status_t olnk_client_connect(olnk_client_t *cli, uint16_t sync_retries);

/**
 * Opcode 0x00 SESSION_SYNC (2B trigger + 34B request -> 33B/161B response).
 * Automatically updates negotiated max_block_size, server_id, and session_resumed.
 */
olnk_status_t olnk_client_session_sync(
    olnk_client_t *cli,
    bool ext_info,
    olnk_session_sync_resp_t *out_resp
);

/**
 * Opcode 0x01 MAP_CONFIG (auto-assigns available remote server drives).
 * Stores the resulting map inside cli->map and optionally copies to out_resp.
 */
olnk_status_t olnk_client_map_config(
    olnk_client_t *cli,
    uint8_t max_client_drives,
    uint8_t first_client_drive,
    olnk_map_config_resp_t *out_resp
);

/**
 * Return true if the server advertised 32-bit LBA support (attr bit 1 = 0x0002)
 * for the given 0-based server unit index (or when no map has been queried yet).
 */
bool olnk_client_unit_supports_lba32(const olnk_client_t *cli, uint8_t unit);

/**
 * Opcode 0x1C GET_DRIVE_INFO (size string + volume label).
 */
olnk_status_t olnk_client_get_drive_info(
    olnk_client_t *cli,
    uint8_t unit,
    olnk_get_drive_info_resp_t *out_resp
);

/**
 * Opcode 0x02 MEDIA_CHECK (status + 0x01/0x00/0xFF + label + serial).
 */
olnk_status_t olnk_client_media_check(
    olnk_client_t *cli,
    uint8_t unit,
    uint8_t media_descriptor,
    olnk_media_check_resp_t *out_resp
);

/**
 * Opcode 0x03 BUILD_BPB (2B error or 33B DOS BPB + 6B ext).
 */
olnk_status_t olnk_client_build_bpb(
    olnk_client_t *cli,
    uint8_t unit,
    uint8_t media_descriptor,
    olnk_build_bpb_resp_t *out_resp
);

/**
 * Opcode 0x04 READ_SECTORS (supports 16/32-bit LBA and >8192 B multi-fragment RX).
 * Note: Checks (out_resp->status & OLNK_DOS_STATUS_ERROR) == 0 before receiving data
 * frames, preserving compatibility with non-zero residual status low bytes from MS-DOS.
 */
olnk_status_t olnk_client_read_sectors(
    olnk_client_t *cli,
    uint8_t unit,
    uint8_t media_descriptor,
    uint32_t start_lba,
    uint16_t sector_count,
    uint16_t bytes_per_sector,
    uint8_t *out_buf,
    uint32_t out_cap,
    olnk_sector_io_resp_t *out_resp
);

/**
 * Opcode 0x05 WRITE_SECTORS or 0x06 WRITE_VERIFY (supports >8192 B fragmentation).
 */
olnk_status_t olnk_client_write_sectors(
    olnk_client_t *cli,
    uint8_t unit,
    uint8_t media_descriptor,
    uint32_t start_lba,
    uint16_t sector_count,
    uint16_t bytes_per_sector,
    const uint8_t *in_buf,
    bool verify,
    olnk_sector_io_resp_t *out_resp
);

/**
 * Opcodes 0x16 (BLK_DEVICE_OPEN), 0x17/0x18 (BLK_DEVICE_CLOSE).
 */
olnk_status_t olnk_client_blk_ctrl(
    olnk_client_t *cli,
    uint8_t opcode,
    uint8_t unit,
    uint16_t *out_status
);

/**
 * Opcode 0x07 DRIVE_INCOMPAT (one-way 5-byte notification, no turnaround/response).
 */
olnk_status_t olnk_client_drive_incompat(
    olnk_client_t *cli,
    uint8_t client_unit,
    uint8_t server_unit
);

/**
 * Opcode 0x1D GENERIC_IOCTL 08h/66h (Get Media ID).
 */
olnk_status_t olnk_client_get_media_id(
    olnk_client_t *cli,
    uint8_t unit,
    uint16_t *out_status,
    olnk_generic_ioctl_mid_t *out_mid
);

/**
 * Opcode 0x1D GENERIC_IOCTL 08h/46h (Set Media ID).
 */
olnk_status_t olnk_client_set_media_id(
    olnk_client_t *cli,
    uint8_t unit,
    const olnk_generic_ioctl_mid_t *mid,
    uint16_t *out_status
);

/**
 * Send one idle keepalive poll and verify echo (drops link after >10 misses).
 */
olnk_status_t olnk_client_keepalive(olnk_client_t *cli);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_CLIENT_H */
