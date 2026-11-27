/* cleanroom/dos/openlnk_sys_stub.c
 *
 * Clean-room 16-bit Real-Mode MS-DOS Block Device Driver (OPENLNK.SYS) core.
 * Freestanding zero-CRT C99 implementation (compiles with Open Watcom -zl -s):
 *   - No <stdio.h>, no <stdlib.h>, zero dynamic allocation, zero floating point.
 *   - Maps MS-DOS Block Device Driver commands (0x00..0x13) cleanly to
 *     libopenlnk's olnk_client_* RPCs over 8250/16550A COM1..COM4.
 */
#include "openlnk_sys.h"

#include <string.h>

static olnk_client_t g_sys_client;
static olnk_dos_uart_t g_sys_uart;
static olnk_dos_bpb_t g_sys_bpbs[OLNK_SYS_MAX_UNITS];
static uint8_t g_sys_unit_map[OLNK_SYS_MAX_UNITS];
static uint8_t g_sys_unit_count = 0U;
static uint8_t g_sys_max_units = 4U;
static bool g_sys_initialized = false;
static bool g_sys_connected = false;

static bool sys_ensure_connected(void) {
    olnk_session_sync_resp_t sresp;
    olnk_map_config_resp_t mresp;
    uint8_t i;

    if (!g_sys_initialized) {
        return false;
    }
    if (g_sys_connected && (g_sys_client.link.link_state == (uint8_t)OLNK_LINK_CONNECTED)) {
        return true;
    }

    if (olnk_client_connect(&g_sys_client, 3U) != OLNK_OK) {
        g_sys_connected = false;
        return false;
    }
    if (olnk_client_session_sync(&g_sys_client, false, &sresp) != OLNK_OK) {
        g_sys_connected = false;
        return false;
    }
    if ((!g_sys_client.link.session_resumed) || (g_sys_unit_count == 0U)) {
        if (olnk_client_map_config(&g_sys_client, g_sys_max_units, 3U, &mresp) != OLNK_OK) {
            g_sys_connected = false;
            return false;
        }
        g_sys_unit_count = 0U;
        for (i = 0U; i < g_sys_max_units; ++i) {
            uint8_t entry = mresp.drive_map_resp[i];
            if ((entry != OLNK_MAP_AUTO) && (entry != OLNK_MAP_NONE)) {
                g_sys_unit_map[i] = (uint8_t)(entry & 0x7FU);
                g_sys_unit_count = (uint8_t)(i + 1U);
            } else {
                g_sys_unit_map[i] = OLNK_MAP_NONE;
            }
        }
    }

    g_sys_connected = true;
    return true;
}

static bool sys_resolve_server_unit(uint8_t client_unit, uint8_t *out_srv_unit) {
    if ((client_unit >= g_sys_max_units) || (g_sys_unit_map[client_unit] == OLNK_MAP_NONE)) {
        return false;
    }
    *out_srv_unit = g_sys_unit_map[client_unit];
    return true;
}

olnk_status_t olnk_dos_sys_init(
    uint8_t com_number,
    uint32_t max_baud,
    uint8_t max_units,
    const olnk_dos_io_ops_t *custom_io
) {
    olnk_transport_t tr;
    uint8_t baud_idx = (uint8_t)OLNK_BAUD_115200;
    uint8_t i;
    olnk_status_t st;

    if (!olnk_bps_to_baud_index(max_baud, &baud_idx)) {
        baud_idx = (uint8_t)OLNK_BAUD_9600;
    }

    g_sys_max_units = ((max_units > 0U) && (max_units <= OLNK_SYS_MAX_UNITS))
                          ? max_units
                          : 4U;
    g_sys_unit_count = 0U;
    g_sys_connected = false;
    memset(g_sys_bpbs, 0, sizeof(g_sys_bpbs));
    for (i = 0U; i < OLNK_SYS_MAX_UNITS; ++i) {
        g_sys_unit_map[i] = OLNK_MAP_NONE;
    }

    st = olnk_dos_uart_init(&g_sys_uart, com_number, 0U, 9600UL, custom_io, &tr);
    if (st != OLNK_OK) {
        return st;
    }
    st = olnk_client_init(&g_sys_client, tr);
    if (st != OLNK_OK) {
        return st;
    }
    g_sys_client.link.max_baud_idx = baud_idx;
    g_sys_initialized = true;
    return OLNK_OK;
}

uint16_t olnk_dos_sys_dispatch(olnk_dos_sys_req_t *req) {
    uint8_t srv_unit = 0U;
    olnk_status_t st;

    if (req == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }

    if (req->cmd == OLNK_DOS_CMD_INIT) {
        (void)sys_ensure_connected();
        req->unit = (g_sys_unit_count > 0U) ? g_sys_unit_count : g_sys_max_units;
        req->status = OLNK_DOS_STATUS_DONE;
        return req->status;
    }

    if (!sys_ensure_connected()) {
        req->sector_count = 0U;
        req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
        return req->status;
    }

    if (!sys_resolve_server_unit(req->unit, &srv_unit)) {
        req->sector_count = 0U;
        req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
        return req->status;
    }

    switch (req->cmd) {
        case OLNK_DOS_CMD_MEDIA_CHECK: {
            olnk_media_check_resp_t resp;
            st = olnk_client_media_check(&g_sys_client, srv_unit, req->media_descriptor, &resp);
            if (st != OLNK_OK) {
                g_sys_connected = false;
                req->media_status = (uint8_t)OLNK_MEDIA_UNSURE;
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
            } else {
                req->media_status = resp.media_status;
                req->status = resp.status;
            }
            return req->status;
        }

        case OLNK_DOS_CMD_BUILD_BPB: {
            olnk_build_bpb_resp_t resp;
            st = olnk_client_build_bpb(&g_sys_client, srv_unit, req->media_descriptor, &resp);
            if (st != OLNK_OK) {
                g_sys_connected = false;
                req->has_bpb = false;
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
            } else {
                req->has_bpb = resp.has_bpb;
                if (resp.has_bpb) {
                    g_sys_bpbs[req->unit] = resp.bpb;
                    req->bpb = resp.bpb;
                }
                req->status = resp.status;
            }
            return req->status;
        }

        case OLNK_DOS_CMD_READ: {
            olnk_sector_io_resp_t resp;
            uint16_t bps = (g_sys_bpbs[req->unit].bytes_per_sector != 0U)
                               ? g_sys_bpbs[req->unit].bytes_per_sector
                               : OLNK_SECTOR_SIZE;
            if (req->sector_count == 0U) {
                req->status = OLNK_DOS_STATUS_DONE;
                return req->status;
            }
            if (req->xfer_buf == NULL) {
                req->sector_count = 0U;
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
                return req->status;
            }
            st = olnk_client_read_sectors(
                &g_sys_client,
                srv_unit,
                req->media_descriptor,
                req->start_lba,
                req->sector_count,
                bps,
                req->xfer_buf,
                req->xfer_cap,
                &resp
            );
            if (st != OLNK_OK) {
                g_sys_connected = false;
                req->sector_count = 0U;
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
            } else {
                req->sector_count = resp.sectors_transferred;
                req->status = resp.status;
            }
            return req->status;
        }

        case OLNK_DOS_CMD_WRITE:
        case OLNK_DOS_CMD_WRITE_VERIFY: {
            olnk_sector_io_resp_t resp;
            uint16_t bps = (g_sys_bpbs[req->unit].bytes_per_sector != 0U)
                               ? g_sys_bpbs[req->unit].bytes_per_sector
                               : OLNK_SECTOR_SIZE;
            if (req->sector_count == 0U) {
                req->status = OLNK_DOS_STATUS_DONE;
                return req->status;
            }
            if (req->xfer_buf == NULL) {
                req->sector_count = 0U;
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
                return req->status;
            }
            st = olnk_client_write_sectors(
                &g_sys_client,
                srv_unit,
                req->media_descriptor,
                req->start_lba,
                req->sector_count,
                bps,
                req->xfer_buf,
                (req->cmd == OLNK_DOS_CMD_WRITE_VERIFY),
                &resp
            );
            if (st != OLNK_OK) {
                g_sys_connected = false;
                req->sector_count = 0U;
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
            } else {
                req->sector_count = resp.sectors_transferred;
                req->status = resp.status;
            }
            return req->status;
        }

        case OLNK_DOS_CMD_OPEN:
        case OLNK_DOS_CMD_CLOSE: {
            uint16_t dos_st = 0U;
            uint8_t op = (req->cmd == OLNK_DOS_CMD_OPEN)
                             ? (uint8_t)OLNK_OP_BLK_DEVICE_OPEN
                             : (uint8_t)OLNK_OP_BLK_DEVICE_CLOSE;
            st = olnk_client_blk_ctrl(&g_sys_client, op, srv_unit, &dos_st);
            if (st != OLNK_OK) {
                g_sys_connected = false;
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
            } else {
                req->status = dos_st;
            }
            return req->status;
        }

        case OLNK_DOS_CMD_GENERIC_IOCTL: {
            uint16_t dos_st = 0U;
            if (req->ioctl_category != OLNK_IOCTL_CAT_DISK) {
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);
                return req->status;
            }
            if (req->ioctl_function == OLNK_IOCTL_FN_GET_MEDIA_ID) {
                st = olnk_client_get_media_id(&g_sys_client, srv_unit, &dos_st, &req->mid);
            } else if (req->ioctl_function == OLNK_IOCTL_FN_SET_MEDIA_ID) {
                st = olnk_client_set_media_id(&g_sys_client, srv_unit, &req->mid, &dos_st);
            } else {
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);
                return req->status;
            }
            if (st != OLNK_OK) {
                g_sys_connected = false;
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
            } else {
                req->status = dos_st;
            }
            return req->status;
        }

        default:
            req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);
            return req->status;
    }
}

const olnk_client_t *olnk_dos_sys_client(void) {
    return &g_sys_client;
}

const olnk_dos_uart_t *olnk_dos_sys_uart(void) {
    return &g_sys_uart;
}

uint8_t olnk_dos_sys_unit_count(void) {
    return g_sys_unit_count;
}

void olnk_dos_strategy(void) {
    /* Linked with 16-bit Real-Mode ES:BX Request Header capture stub */
}

void olnk_dos_interrupt(void) {
    /* Linked with 16-bit Real-Mode Request Header marshaller -> olnk_dos_sys_dispatch */
}

const dos_block_driver_header_t olnk_dos_header = {
    0xFFFFFFFFUL,            /* Last driver in chain */
    OLNK_DRIVE_ATTR_DEFAULT, /* Block device, Open/Close/RM + Generic IOCTL + 32-bit LBA */
    0x0000U,                 /* Strategy offset (linked in assembly stub) */
    0x0000U,                 /* Interrupt offset (linked in assembly stub) */
    1U,                      /* Redirected remote block unit count (updated on INIT) */
    { 'O', 'P', 'E', 'N', 'L', 'N', 'K' }
};
