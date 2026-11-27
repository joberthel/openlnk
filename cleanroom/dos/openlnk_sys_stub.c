/* cleanroom/dos/openlnk_sys_stub.c
 *
 * Clean-room 16-bit Real-Mode MS-DOS Block Device Driver (OPENLNK.SYS) core.
 * Freestanding zero-CRT C99 implementation (compiles with Open Watcom -zl -s):
 *   - No <stdio.h>, no <stdlib.h>, no <ctype.h>, zero dynamic allocation, zero FP.
 *   - Parses CONFIG.SYS arguments (/COM1../COM4, /BAUD:<rate>, /DRIVES:<1..8>).
 *   - Marshals byte-exact MS-DOS 5.0/6.22 Request Headers (Cmds 0x00..0x19)
 *     including 16/32-bit LBA, bounce buffer with 64 KiB wrap safety,
 *     packed 25-byte BPB + INIT BPB pointer array, Media Check volume IDs,
 *     and Generic IOCTL 08h/66h & 08h/46h.
 *   - On 16-bit MS-DOS, places INIT-only code, arg parser, banner formatter,
 *     and INIT strings in discardable INIT_TEXT/INIT_DATA segments after
 *     _UNIT_BSS so MS-DOS reclaims all INIT memory via the Cmd 0x00 Break Address.
 */
#include "openlnk_sys.h"
#include "openlnk/version.h"

#include <string.h>

#define OLNK_SYS_CMDLINE_MAX 128U
#define OLNK_SYS_BANNER_MAX  96U
#define OLNK_SYS_RH_MAX_LEN  30U
#define OLNK_SYS_BOUNCE_CAP \
    ((OLNK_SYS_BOUNCE_BYTES <= OLNK_MAX_PAYLOAD_SIZE) ? OLNK_SYS_BOUNCE_BYTES : OLNK_MAX_PAYLOAD_SIZE)

/* Compact per-unit resident state (38 B/unit; no redundant unpacked BPB copy). */
typedef struct {
    uint8_t bpb_wire[OLNK_LEN_DOS_BPB];
    char vol_label[12];
    uint8_t srv_unit;
} olnk_sys_unit_t;

/* =========================================================================
 * Static Resident Driver State (_BSS + tail _UNIT_BSS)
 *
 * Half-duplex sector I/O and INIT scratch reuse g_sys_client.link.rx_buffer
 * (olnk_client_* always receives directly into caller buffers or ctrl_buf,
 * never into link.rx_buffer).
 * ========================================================================= */

static olnk_client_t g_sys_client;
static olnk_dos_uart_t g_sys_uart;
static uint16_t g_sys_bpb_ptrs[OLNK_SYS_MAX_UNITS];
static char g_sys_ret_vol_label[12];
static uint8_t g_sys_bpb_valid_mask = 0U;
static uint8_t g_sys_unit_count = 0U;
static uint8_t g_sys_active_units = 4U;
static uint8_t g_sys_max_units = 4U;
static uint8_t g_sys_first_drive = 3U; /* Default D: (updated from INIT rh+22) */
static bool g_sys_initialized = false;
static bool g_sys_connected = false;

/* Placed immediately after _DRVSTK in _UNIT_BSS so unused slots [units_out..7]
 * are reclaimed by MS-DOS alongside INIT_TEXT/INIT_DATA on Cmd 0x00 return. */
#if defined(__WATCOMC__) && defined(__DOS__)
#pragma data_seg("_UNIT_BSS", "UNIT_BSS")
static olnk_sys_unit_t g_sys_units[OLNK_SYS_MAX_UNITS] = { { { 0U } } };
#pragma data_seg("_DATA", "DATA")
#else
static olnk_sys_unit_t g_sys_units[OLNK_SYS_MAX_UNITS];
#endif

/* Forward declaration of discardable INIT handler (lives in INIT_TEXT on DOS) */
static uint16_t sys_handle_init(
    uint16_t rh_seg,
    uint16_t rh_off,
    const uint8_t *rh,
    uint8_t rh_len,
    uint16_t drv_seg,
    uint16_t resident_end,
    const olnk_dos_sys_mem_ops_t *mem_ops,
    const olnk_dos_io_ops_t *custom_io,
    olnk_dos_sys_req_t *req
);

#if defined(__WATCOMC__) && defined(__DOS__)
extern void sys_asm_far_read(uint16_t src_seg, uint16_t src_off, void *dst_near, uint16_t len);
#pragma aux sys_asm_far_read "_sys_asm_far_read" \
    parm [ax] [si] [di] [cx]                     \
    modify [];

extern void sys_asm_far_write(uint16_t dst_seg, uint16_t dst_off, const void *src_near, uint16_t len);
#pragma aux sys_asm_far_write "_sys_asm_far_write" \
    parm [ax] [di] [si] [cx]                       \
    modify [];

extern void sys_asm_set_header_units(uint8_t count);
#pragma aux sys_asm_set_header_units "_sys_asm_set_header_units" \
    parm [ax]                                                    \
    modify [];

extern void sys_asm_dos_print(const char *dollar_msg);
#pragma aux sys_asm_dos_print "_sys_asm_dos_print" \
    parm [dx]                                      \
    modify [];

#define SYS_FAR_READ(ops, seg, off, dst, len) \
    ((void)(ops), sys_asm_far_read((seg), (off), (dst), (len)))
#define SYS_FAR_WRITE(ops, seg, off, src, len) \
    ((void)(ops), sys_asm_far_write((seg), (off), (src), (len)))
#define SYS_SET_UNITS(ops, cnt) ((void)(ops), sys_asm_set_header_units(cnt))
#define SYS_PRINT_INIT(ops, msg) ((void)(ops), sys_asm_dos_print(msg))
#else
#define SYS_FAR_READ(ops, seg, off, dst, len) \
    (ops)->far_read((seg), (off), (dst), (len), (ops)->ctx)
#define SYS_FAR_WRITE(ops, seg, off, src, len) \
    (ops)->far_write((seg), (off), (src), (len), (ops)->ctx)
#define SYS_SET_UNITS(ops, cnt) \
    do { if ((ops)->set_header_units != NULL) { (ops)->set_header_units((cnt), (ops)->ctx); } } while (0)
#define SYS_PRINT_INIT(ops, msg) \
    do { if ((ops)->print_init_msg != NULL) { (ops)->print_init_msg((msg), (ops)->ctx); } } while (0)
#endif

/* =========================================================================
 * Resident Far-Pointer & Table Addressing Helpers
 * ========================================================================= */

/* Normalize real-mode seg:off so off is always in 0..15 (prevents 64 KiB wrap) */
static void sys_far_normalize(uint16_t *seg, uint16_t *off) {
    *seg = (uint16_t)(*seg + (uint16_t)(*off >> 4U));
    *off = (uint16_t)(*off & 0x000FU);
}

/* Advance normalized real-mode seg:off (off <= 15) by <= 65520 bytes without 32-bit math */
static void sys_far_advance_u16(uint16_t *seg, uint16_t *off, uint16_t bytes) {
    uint16_t total = (uint16_t)(*off + bytes);
    *seg = (uint16_t)(*seg + (uint16_t)(total >> 4U));
    *off = (uint16_t)(total & 0x000FU);
}

static uint16_t sys_unit_bps(uint8_t unit) {
    if (unit < OLNK_SYS_MAX_UNITS) {
        uint16_t bps = olnk_read_u16_le(g_sys_units[unit].bpb_wire);
        if (bps != 0U) {
            return bps;
        }
    }
    return OLNK_SECTOR_SIZE;
}

static uint16_t sys_bpb_array_offset(const olnk_dos_sys_mem_ops_t *mem_ops) {
#if defined(__WATCOMC__) && defined(__DOS__)
    (void)mem_ops;
    return (uint16_t)(uintptr_t)&g_sys_bpb_ptrs[0];
#else
    if ((mem_ops != NULL) && (mem_ops->host_table_base_off != 0U)) {
        return mem_ops->host_table_base_off;
    }
    return 0x0400U;
#endif
}

static uint16_t sys_bpb_wire_offset(const olnk_dos_sys_mem_ops_t *mem_ops, uint8_t unit) {
#if defined(__WATCOMC__) && defined(__DOS__)
    (void)mem_ops;
    return (uint16_t)(uintptr_t)&g_sys_units[unit].bpb_wire[0];
#else
    uint16_t base = sys_bpb_array_offset(mem_ops);
    return (uint16_t)(base + (uint16_t)(OLNK_SYS_MAX_UNITS * 2U) +
                      (uint16_t)((uint16_t)unit * (uint16_t)OLNK_LEN_DOS_BPB));
#endif
}

static uint16_t sys_vol_label_offset(const olnk_dos_sys_mem_ops_t *mem_ops) {
#if defined(__WATCOMC__) && defined(__DOS__)
    (void)mem_ops;
    return (uint16_t)(uintptr_t)&g_sys_ret_vol_label[0];
#else
    uint16_t base = sys_bpb_array_offset(mem_ops);
    return (uint16_t)(base + (uint16_t)(OLNK_SYS_MAX_UNITS * 2U) +
                      (uint16_t)(OLNK_SYS_MAX_UNITS * OLNK_LEN_DOS_BPB));
#endif
}

static void sys_sync_tables_to_drv_seg(
    uint16_t drv_seg,
    const olnk_dos_sys_mem_ops_t *mem_ops
) {
#if defined(__WATCOMC__) && defined(__DOS__)
    /* On 16-bit DOS, g_sys_bpb_ptrs/units/ret_vol_label already live in drv_seg (CS==DS) */
    (void)drv_seg;
    (void)mem_ops;
#else
    uint8_t u;
    for (u = 0U; u < g_sys_active_units; ++u) {
        g_sys_bpb_ptrs[u] = sys_bpb_wire_offset(mem_ops, u);
    }
    if ((mem_ops != NULL) && (mem_ops->far_write != NULL)) {
        uint8_t ptr_raw[OLNK_SYS_MAX_UNITS * 2U];
        uint16_t arr_seg = drv_seg;
        uint16_t arr_off = sys_bpb_array_offset(mem_ops);
        uint16_t v_seg = drv_seg;
        uint16_t v_off = sys_vol_label_offset(mem_ops);
        memset(ptr_raw, 0, sizeof(ptr_raw));
        for (u = 0U; u < g_sys_active_units; ++u) {
            olnk_write_u16_le(&ptr_raw[(size_t)u * 2U], g_sys_bpb_ptrs[u]);
        }
        sys_far_normalize(&arr_seg, &arr_off);
        mem_ops->far_write(arr_seg, arr_off, ptr_raw, (uint16_t)sizeof(ptr_raw), mem_ops->ctx);

        for (u = 0U; u < g_sys_active_units; ++u) {
            uint16_t b_seg = drv_seg;
            uint16_t b_off = sys_bpb_wire_offset(mem_ops, u);
            sys_far_normalize(&b_seg, &b_off);
            mem_ops->far_write(b_seg, b_off, g_sys_units[u].bpb_wire, OLNK_LEN_DOS_BPB, mem_ops->ctx);
        }
        sys_far_normalize(&v_seg, &v_off);
        mem_ops->far_write(v_seg, v_off, g_sys_ret_vol_label, 12U, mem_ops->ctx);
    }
#endif
}

/* =========================================================================
 * Core Client Connection & Command Dispatcher (0x00..0x19)
 * ========================================================================= */

static bool sys_ensure_connected(void) {
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
    if (olnk_client_session_sync(&g_sys_client, false, NULL) != OLNK_OK) {
        g_sys_connected = false;
        return false;
    }
    if ((!g_sys_client.link.session_resumed) || (g_sys_unit_count == 0U)) {
        if (olnk_client_map_config(
                &g_sys_client, g_sys_max_units, g_sys_first_drive, NULL) != OLNK_OK) {
            g_sys_connected = false;
            return false;
        }
        g_sys_unit_count = 0U;
        g_sys_bpb_valid_mask = 0U;
        for (i = 0U; i < g_sys_max_units; ++i) {
            uint8_t entry = g_sys_client.map.drive_map_resp[i];
            if ((entry != OLNK_MAP_AUTO) && (entry != OLNK_MAP_NONE)) {
                g_sys_units[i].srv_unit = (uint8_t)(entry & 0x7FU);
                g_sys_unit_count = (uint8_t)(i + 1U);
            } else {
                g_sys_units[i].srv_unit = OLNK_MAP_NONE;
            }
        }
    }

    g_sys_connected = true;
    return true;
}

static bool sys_resolve_server_unit(uint8_t client_unit, uint8_t *out_srv_unit) {
    if ((client_unit >= g_sys_max_units) || (g_sys_units[client_unit].srv_unit == OLNK_MAP_NONE)) {
        return false;
    }
    *out_srv_unit = g_sys_units[client_unit].srv_unit;
    return true;
}

uint16_t olnk_dos_sys_dispatch(olnk_dos_sys_req_t *req) {
    uint8_t srv_unit = 0U;
    olnk_status_t st;
    /* Overlap mutually exclusive RPC response structs to minimize 16-bit stack depth */
    union {
        olnk_media_check_resp_t mc;
        olnk_build_bpb_resp_t bpb;
        olnk_sector_io_resp_t io;
        uint16_t dos_st;
    } u;

    if (req == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }

    if (req->cmd == OLNK_DOS_CMD_INIT) {
        (void)sys_ensure_connected();
        req->unit = (g_sys_unit_count > 0U) ? g_sys_unit_count : g_sys_max_units;
        g_sys_active_units = req->unit;
        req->status = OLNK_DOS_STATUS_DONE;
        return req->status;
    }

    if (req->cmd == OLNK_DOS_CMD_REMOVABLE_MEDIA) {
        req->status = (req->unit >= g_sys_active_units)
                          ? OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT)
                          : OLNK_DOS_STATUS_DONE;
        return req->status;
    }

    if ((req->cmd == OLNK_DOS_CMD_GET_LOGICAL) || (req->cmd == OLNK_DOS_CMD_SET_LOGICAL)) {
        req->unit = 0U; /* Single logical letter per unit */
        req->status = OLNK_DOS_STATUS_DONE;
        return req->status;
    }

    if (req->cmd == OLNK_DOS_CMD_IOCTL_QUERY) {
        if ((req->ioctl_category == OLNK_IOCTL_CAT_DISK) &&
            ((req->ioctl_function == OLNK_IOCTL_FN_GET_MEDIA_ID) ||
             (req->ioctl_function == OLNK_IOCTL_FN_SET_MEDIA_ID))) {
            req->status = OLNK_DOS_STATUS_DONE;
        } else {
            req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);
        }
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
        case OLNK_DOS_CMD_MEDIA_CHECK:
            st = olnk_client_media_check(&g_sys_client, srv_unit, req->media_descriptor, &u.mc);
            if (st != OLNK_OK) {
                req->media_status = (uint8_t)OLNK_MEDIA_UNSURE;
                goto link_err;
            }
            req->media_status = u.mc.media_status;
            req->status = u.mc.status;
            if (((u.mc.status & OLNK_DOS_STATUS_ERROR) == 0U) &&
                ((g_sys_bpb_valid_mask & (uint8_t)(1U << req->unit)) == 0U) &&
                (req->media_status == (uint8_t)OLNK_MEDIA_UNCHANGED)) {
                /* Force DOS to call BUILD_BPB at least once after offline boot / remap */
                req->media_status = (uint8_t)OLNK_MEDIA_CHANGED;
            }
            if (req->media_status == (uint8_t)OLNK_MEDIA_CHANGED) {
                memcpy(g_sys_ret_vol_label, g_sys_units[req->unit].vol_label, 12U);
                memcpy(req->volume_label, g_sys_ret_vol_label, 12U);
            }
            if (u.mc.volume_label[0] != '\0') {
                memcpy(g_sys_units[req->unit].vol_label, u.mc.volume_label, 12U);
                g_sys_units[req->unit].vol_label[11] = '\0';
            }
            return req->status;

        case OLNK_DOS_CMD_BUILD_BPB:
            st = olnk_client_build_bpb(&g_sys_client, srv_unit, req->media_descriptor, &u.bpb);
            if (st != OLNK_OK) {
                req->has_bpb = false;
                goto link_err;
            }
            req->has_bpb = u.bpb.has_bpb;
            if (u.bpb.has_bpb) {
                /* ctrl_buf[2..26] holds the exact 25-byte packed DOS BPB from the server */
                memcpy(g_sys_units[req->unit].bpb_wire, &g_sys_client.ctrl_buf[2], OLNK_LEN_DOS_BPB);
                g_sys_bpb_valid_mask = (uint8_t)(g_sys_bpb_valid_mask | (uint8_t)(1U << req->unit));
                req->bpb = u.bpb.bpb;
            }
            req->status = u.bpb.status;
            return req->status;

        case OLNK_DOS_CMD_READ:
        case OLNK_DOS_CMD_WRITE:
        case OLNK_DOS_CMD_WRITE_VERIFY: {
            uint16_t bps = sys_unit_bps(req->unit);
            if (req->sector_count == 0U) {
                req->status = OLNK_DOS_STATUS_DONE;
                return req->status;
            }
            if (req->xfer_buf == NULL) {
                req->sector_count = 0U;
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
                return req->status;
            }
            if (req->cmd == OLNK_DOS_CMD_READ) {
                st = olnk_client_read_sectors(
                    &g_sys_client,
                    srv_unit,
                    req->media_descriptor,
                    req->start_lba,
                    req->sector_count,
                    bps,
                    req->xfer_buf,
                    req->xfer_cap,
                    &u.io
                );
            } else {
                st = olnk_client_write_sectors(
                    &g_sys_client,
                    srv_unit,
                    req->media_descriptor,
                    req->start_lba,
                    req->sector_count,
                    bps,
                    req->xfer_buf,
                    (req->cmd == OLNK_DOS_CMD_WRITE_VERIFY),
                    &u.io
                );
            }
            if (st != OLNK_OK) {
                req->sector_count = 0U;
                goto link_err;
            }
            req->sector_count = u.io.sectors_transferred;
            req->status = u.io.status;
            if (u.io.has_volume_id) {
                memcpy(g_sys_ret_vol_label, u.io.volume_label, 12U);
                g_sys_ret_vol_label[11] = '\0';
                memcpy(req->volume_label, g_sys_ret_vol_label, 12U);
            }
            return req->status;
        }

        case OLNK_DOS_CMD_OPEN:
        case OLNK_DOS_CMD_CLOSE: {
            uint8_t op = (req->cmd == OLNK_DOS_CMD_OPEN)
                             ? (uint8_t)OLNK_OP_BLK_DEVICE_OPEN
                             : (uint8_t)OLNK_OP_BLK_DEVICE_CLOSE;
            u.dos_st = 0U;
            st = olnk_client_blk_ctrl(&g_sys_client, op, srv_unit, &u.dos_st);
            if (st != OLNK_OK) {
                goto link_err;
            }
            req->status = u.dos_st;
            return req->status;
        }

        case OLNK_DOS_CMD_GENERIC_IOCTL:
            u.dos_st = 0U;
            if (req->ioctl_category != OLNK_IOCTL_CAT_DISK) {
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);
                return req->status;
            }
            if (req->ioctl_function == OLNK_IOCTL_FN_GET_MEDIA_ID) {
                st = olnk_client_get_media_id(&g_sys_client, srv_unit, &u.dos_st, &req->mid);
            } else if (req->ioctl_function == OLNK_IOCTL_FN_SET_MEDIA_ID) {
                st = olnk_client_set_media_id(&g_sys_client, srv_unit, &req->mid, &u.dos_st);
            } else {
                req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);
                return req->status;
            }
            if (st != OLNK_OK) {
                goto link_err;
            }
            req->status = u.dos_st;
            return req->status;

        default:
            req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);
            return req->status;
    }

link_err:
    g_sys_connected = false;
    req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    return req->status;
}

/* =========================================================================
 * Byte-Exact MS-DOS 5.0/6.22 Request Header Marshaller (Cmds 0x00..0x19)
 * ========================================================================= */

/* rh_seg:rh_off is pre-normalized (rh_off <= 15) and field_off <= 30, so
 * rh_off + field_off <= 45 never crosses a 64 KiB boundary. */
static void sys_rh_write_u8(
    uint16_t rh_seg,
    uint16_t rh_off,
    uint16_t field_off,
    uint8_t val,
    const olnk_dos_sys_mem_ops_t *mem_ops
) {
    SYS_FAR_WRITE(mem_ops, rh_seg, (uint16_t)(rh_off + field_off), &val, 1U);
}

static void sys_rh_write_u16(
    uint16_t rh_seg,
    uint16_t rh_off,
    uint16_t field_off,
    uint16_t val,
    const olnk_dos_sys_mem_ops_t *mem_ops
) {
    uint8_t raw[2];
    olnk_write_u16_le(raw, val);
    SYS_FAR_WRITE(mem_ops, rh_seg, (uint16_t)(rh_off + field_off), raw, 2U);
}

static void sys_rh_write_far_ptr(
    uint16_t rh_seg,
    uint16_t rh_off,
    uint16_t field_off,
    uint16_t ptr_off,
    uint16_t ptr_seg,
    const olnk_dos_sys_mem_ops_t *mem_ops
) {
    uint8_t raw[4];
    olnk_write_u16_le(&raw[0], ptr_off);
    olnk_write_u16_le(&raw[2], ptr_seg);
    SYS_FAR_WRITE(mem_ops, rh_seg, (uint16_t)(rh_off + field_off), raw, 4U);
}

uint16_t olnk_dos_sys_handle_rh(
    uint16_t rh_seg,
    uint16_t rh_off,
    uint16_t drv_seg,
    uint16_t resident_end,
    const olnk_dos_sys_mem_ops_t *mem_ops,
    const olnk_dos_io_ops_t *custom_io
) {
    uint8_t rh[OLNK_SYS_RH_MAX_LEN];
    uint8_t rh_len;
    uint8_t unit;
    uint8_t cmd;
    uint16_t read_len;
    uint16_t status;
    olnk_dos_sys_req_t req;

#if !defined(__WATCOMC__) || !defined(__DOS__)
    if ((mem_ops == NULL) || (mem_ops->far_read == NULL) || (mem_ops->far_write == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }
#endif

    sys_far_normalize(&rh_seg, &rh_off);
    memset(rh, 0, sizeof(rh));
    SYS_FAR_READ(mem_ops, rh_seg, rh_off, rh, 13U);

    rh_len = rh[0];
    unit = rh[1];
    cmd = rh[2];

    if (rh_len < 13U) {
        status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_BAD_REQ_LENGTH);
        sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
        return status;
    }

    read_len = (rh_len > OLNK_SYS_RH_MAX_LEN) ? OLNK_SYS_RH_MAX_LEN : (uint16_t)rh_len;
    if (read_len > 13U) {
        SYS_FAR_READ(
            mem_ops, rh_seg, (uint16_t)(rh_off + 13U), &rh[13], (uint16_t)(read_len - 13U));
    }

    if (cmd == OLNK_DOS_CMD_INIT) {
#if defined(__WATCOMC__) && defined(__DOS__)
        /* On real MS-DOS, INIT_TEXT is reclaimed by DOS right after Cmd 0x00 returns */
        if (g_sys_initialized) {
            status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);
            sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
            return status;
        }
#endif
        return sys_handle_init(
            rh_seg, rh_off, rh, rh_len, drv_seg, resident_end, mem_ops, custom_io, &req);
    }

    memset(&req, 0, sizeof(req));
    req.cmd = cmd;
    req.unit = unit;

    switch (cmd) {
        case OLNK_DOS_CMD_MEDIA_CHECK:
            req.media_descriptor = rh[13];
            status = olnk_dos_sys_dispatch(&req);
            sys_rh_write_u8(rh_seg, rh_off, 14U, req.media_status, mem_ops);
            if ((req.media_status == (uint8_t)OLNK_MEDIA_CHANGED) &&
                (unit < OLNK_SYS_MAX_UNITS) && (rh_len >= 19U)) {
                sys_sync_tables_to_drv_seg(drv_seg, mem_ops);
                sys_rh_write_far_ptr(
                    rh_seg, rh_off, 15U, sys_vol_label_offset(mem_ops), drv_seg, mem_ops);
            }
            break;

        case OLNK_DOS_CMD_BUILD_BPB:
            req.media_descriptor = rh[13];
            status = olnk_dos_sys_dispatch(&req);
            if (((status & OLNK_DOS_STATUS_ERROR) == 0U) && req.has_bpb &&
                (unit < OLNK_SYS_MAX_UNITS)) {
                sys_sync_tables_to_drv_seg(drv_seg, mem_ops);
                sys_rh_write_far_ptr(
                    rh_seg, rh_off, 18U, sys_bpb_wire_offset(mem_ops, unit), drv_seg, mem_ops);
            }
            break;

        case OLNK_DOS_CMD_READ:
        case OLNK_DOS_CMD_WRITE:
        case OLNK_DOS_CMD_WRITE_VERIFY: {
            uint8_t media_desc = rh[13];
            uint16_t xfer_off = olnk_read_u16_le(&rh[14]);
            uint16_t xfer_seg = olnk_read_u16_le(&rh[16]);
            uint16_t total_sectors = olnk_read_u16_le(&rh[18]);
            uint16_t start16 = olnk_read_u16_le(&rh[20]);
            uint32_t start_lba = ((start16 == 0xFFFFU) && (rh_len >= 30U))
                                     ? olnk_read_u32_le(&rh[26])
                                     : (uint32_t)start16;
            uint16_t bps = sys_unit_bps(unit);
            uint16_t max_chunk = (bps > 0U) ? (uint16_t)(OLNK_SYS_BOUNCE_CAP / bps) : 0U;
            uint16_t done_sectors = 0U;

            if (total_sectors == 0U) {
                status = OLNK_DOS_STATUS_DONE;
                sys_rh_write_u16(rh_seg, rh_off, 18U, 0U, mem_ops);
                break;
            }
            if (max_chunk == 0U) {
                status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_MEDIA);
                sys_rh_write_u16(rh_seg, rh_off, 18U, 0U, mem_ops);
                break;
            }

            sys_far_normalize(&xfer_seg, &xfer_off);
            status = OLNK_DOS_STATUS_DONE;

            while (done_sectors < total_sectors) {
                uint16_t remain = (uint16_t)(total_sectors - done_sectors);
                uint16_t chunk = (remain > max_chunk) ? max_chunk : remain;
                uint16_t chunk_bytes = (uint16_t)(chunk * bps);

                if (cmd != OLNK_DOS_CMD_READ) {
                    SYS_FAR_READ(
                        mem_ops, xfer_seg, xfer_off, g_sys_client.link.rx_buffer, chunk_bytes);
                }

                req.cmd = cmd;
                req.unit = unit;
                req.media_descriptor = media_desc;
                req.start_lba = start_lba + (uint32_t)done_sectors;
                req.sector_count = chunk;
                req.xfer_buf = g_sys_client.link.rx_buffer;
                req.xfer_cap = (uint32_t)chunk_bytes;

                status = olnk_dos_sys_dispatch(&req);

                if ((cmd == OLNK_DOS_CMD_READ) && (req.sector_count > 0U)) {
                    uint16_t got_bytes = (uint16_t)(req.sector_count * bps);
                    SYS_FAR_WRITE(
                        mem_ops, xfer_seg, xfer_off, g_sys_client.link.rx_buffer, got_bytes);
                }

                done_sectors = (uint16_t)(done_sectors + req.sector_count);

                if (((status & OLNK_DOS_STATUS_ERROR) != 0U) || (req.sector_count == 0U)) {
                    if (((status & 0x00FFU) == (uint16_t)OLNK_DOS_ERR_INVALID_DISK_CHG) &&
                        (unit < OLNK_SYS_MAX_UNITS) && (rh_len >= 26U)) {
                        sys_sync_tables_to_drv_seg(drv_seg, mem_ops);
                        sys_rh_write_far_ptr(
                            rh_seg,
                            rh_off,
                            22U,
                            sys_vol_label_offset(mem_ops),
                            drv_seg,
                            mem_ops
                        );
                    }
                    break;
                }

                sys_far_advance_u16(
                    &xfer_seg, &xfer_off, (uint16_t)(req.sector_count * bps));

                if (req.sector_count < chunk) {
                    break;
                }
            }

            sys_rh_write_u16(rh_seg, rh_off, 18U, done_sectors, mem_ops);
            break;
        }

        case OLNK_DOS_CMD_OPEN:
        case OLNK_DOS_CMD_CLOSE:
        case OLNK_DOS_CMD_REMOVABLE_MEDIA:
            status = olnk_dos_sys_dispatch(&req);
            break;

        case OLNK_DOS_CMD_GET_LOGICAL:
        case OLNK_DOS_CMD_SET_LOGICAL:
            status = olnk_dos_sys_dispatch(&req);
            sys_rh_write_u8(rh_seg, rh_off, 1U, req.unit, mem_ops);
            break;

        case OLNK_DOS_CMD_IOCTL_QUERY:
            req.ioctl_category = rh[13];
            req.ioctl_function = rh[14];
            status = olnk_dos_sys_dispatch(&req);
            break;

        case OLNK_DOS_CMD_GENERIC_IOCTL: {
            uint8_t *mid_buf = g_sys_client.link.rx_buffer;
            uint16_t pb_off = olnk_read_u16_le(&rh[19]);
            uint16_t pb_seg = olnk_read_u16_le(&rh[21]);
            size_t mid_len = 0U;

            req.ioctl_category = rh[13];
            req.ioctl_function = rh[14];
            sys_far_normalize(&pb_seg, &pb_off);

            if ((req.ioctl_category == OLNK_IOCTL_CAT_DISK) &&
                (req.ioctl_function == OLNK_IOCTL_FN_SET_MEDIA_ID)) {
                SYS_FAR_READ(mem_ops, pb_seg, pb_off, mid_buf, OLNK_LEN_GENERIC_IOCTL_MID);
                (void)olnk_decode_generic_ioctl_mid(mid_buf, OLNK_LEN_GENERIC_IOCTL_MID, &req.mid);
            }

            status = olnk_dos_sys_dispatch(&req);

            if ((req.ioctl_category == OLNK_IOCTL_CAT_DISK) &&
                (req.ioctl_function == OLNK_IOCTL_FN_GET_MEDIA_ID) &&
                ((status & OLNK_DOS_STATUS_ERROR) == 0U)) {
                if (olnk_encode_generic_ioctl_mid(
                        &req.mid, mid_buf, OLNK_LEN_GENERIC_IOCTL_MID, &mid_len) == OLNK_OK) {
                    SYS_FAR_WRITE(mem_ops, pb_seg, pb_off, mid_buf, OLNK_LEN_GENERIC_IOCTL_MID);
                }
            }
            break;
        }

        default:
            status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);
            break;
    }

    sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
    return status;
}

/* =========================================================================
 * Diagnostic Helpers & Real-Mode 16-Bit Assembly Bridge (Resident)
 * ========================================================================= */

const olnk_client_t *olnk_dos_sys_client(void) {
    return &g_sys_client;
}

const olnk_dos_uart_t *olnk_dos_sys_uart(void) {
    return &g_sys_uart;
}

uint8_t olnk_dos_sys_unit_count(void) {
    return g_sys_unit_count;
}

#if defined(__WATCOMC__) && defined(__DOS__)

void olnk_dos_sys_interrupt_c(
    uint16_t rh_seg,
    uint16_t rh_off,
    uint16_t drv_seg,
    uint16_t resident_end
);
#pragma aux olnk_dos_sys_interrupt_c "_olnk_dos_sys_interrupt_c" \
    parm [ax] [dx] [bx] [cx];

void olnk_dos_sys_interrupt_c(
    uint16_t rh_seg,
    uint16_t rh_off,
    uint16_t drv_seg,
    uint16_t resident_end
) {
    (void)olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, NULL, NULL);
}

#else

void olnk_dos_strategy(void) {
    /* Linked with 16-bit Real-Mode openlnk_sys_entry.asm on MS-DOS */
}

void olnk_dos_interrupt(void) {
    /* Linked with 16-bit Real-Mode openlnk_sys_entry.asm on MS-DOS */
}

const dos_block_driver_header_t olnk_dos_header = {
    0xFFFFFFFFUL,             /* Last driver in chain */
    OLNK_DOS_SYS_HEADER_ATTR, /* Block | Non-IBM | Open/Close/RM | Generic IOCTL | 32-bit LBA */
    0x0000U,                  /* Strategy offset (linked in openlnk_sys_entry.asm) */
    0x0000U,                  /* Interrupt offset (linked in openlnk_sys_entry.asm) */
    1U,                       /* Redirected remote block unit count (updated on INIT) */
    { 'O', 'P', 'E', 'N', 'L', 'N', 'K' }
};

#endif

/* =========================================================================
 * DISCARDABLE INIT-ONLY SECTION (INIT_TEXT / INIT_DATA)
 *
 * Everything below this point sits after _sys_resident_end on 16-bit MS-DOS
 * and is reclaimed immediately by MS-DOS when Cmd 0x00 (INIT) returns the
 * Break Address at rh+14..17.
 * ========================================================================= */

#if defined(__WATCOMC__) && defined(__DOS__)
#pragma code_seg("INIT_TEXT", "INIT_CODE")
#pragma data_seg("INIT_DATA", "INIT_DATA")
#endif

/* Valid conservative 10 MiB FAT16 placeholder BPB (25 B wire format; prevents DOS boot div-by-0) */
static const uint8_t k_default_bpb_wire[OLNK_LEN_DOS_BPB] = {
    0x00U, 0x02U,               /* bytes_per_sector    = 512 */
    0x02U,                      /* sectors_per_cluster = 2 */
    0x01U, 0x00U,               /* reserved_sectors    = 1 */
    0x02U,                      /* num_fats            = 2 */
    0x00U, 0x02U,               /* root_entries        = 512 */
    0x00U, 0x50U,               /* total_sectors_16    = 20480 */
    0xF8U,                      /* media_descriptor    = 0xF8 */
    0x28U, 0x00U,               /* sectors_per_fat     = 40 */
    0x20U, 0x00U,               /* sectors_per_track   = 32 */
    0x02U, 0x00U,               /* num_heads           = 2 */
    0x00U, 0x00U, 0x00U, 0x00U, /* hidden_sectors      = 0 */
    0x00U, 0x00U, 0x00U, 0x00U  /* total_sectors_32    = 0 */
};

static const char k_default_label[12] = "NO NAME    ";
static const char k_sw_com[] = "COM";
static const char k_sw_baud[] = "BAUD:";
static const char k_sw_drives[] = "DRIVES:";
static const char k_ban_hdr[] = "OPENLNK.SYS v" OLNK_VERSION_STRING ": COM";
static const char k_ban_space[] = " ";
static const char k_ban_bps[] = " bps, ";
static const char k_ban_drv[] = " drive(s) (";
static const char k_ban_conn[] = ") [connected]\r\n$";
static const char k_ban_stby[] = ") [standby]\r\n$";

static char ascii_upper(char ch) {
    if ((ch >= 'a') && (ch <= 'z')) {
        return (char)(ch - ('a' - 'A'));
    }
    return ch;
}

static bool ascii_is_space(char ch) {
    return (ch == ' ') || (ch == '\t') || (ch == '\r') || (ch == '\n');
}

static bool ascii_ieq_prefix(const char *s, const char *prefix) {
    while (*prefix != '\0') {
        if (ascii_upper(*s) != ascii_upper(*prefix)) {
            return false;
        }
        ++s;
        ++prefix;
    }
    return true;
}

static uint32_t parse_u32_token(const char *s, size_t *out_digits) {
    uint32_t val = 0UL;
    size_t digits = 0U;
    while ((*s >= '0') && (*s <= '9')) {
        if (digits < 9U) {
            val = (val * 10UL) + (uint32_t)(*s - '0');
        }
        ++digits;
        ++s;
    }
    if (out_digits != NULL) {
        *out_digits = digits;
    }
    return val;
}

void olnk_dos_sys_parse_args(const char *cmdline, olnk_dos_sys_cfg_t *out_cfg) {
    const char *p;
    bool first_token = true;

    if (out_cfg == NULL) {
        return;
    }

    out_cfg->com_number = 1U;
    out_cfg->max_baud = 115200UL;
    out_cfg->max_units = 4U;

    if (cmdline == NULL) {
        return;
    }

    p = cmdline;
    while ((*p != '\0') && (*p != '\r') && (*p != '\n')) {
        const char *tok_start;
        size_t tok_len = 0U;

        while ((*p != '\0') && (*p != '\r') && (*p != '\n') && ascii_is_space(*p)) {
            ++p;
        }
        if ((*p == '\0') || (*p == '\r') || (*p == '\n')) {
            break;
        }

        tok_start = p;
        while ((*p != '\0') && (*p != '\r') && (*p != '\n') && (!ascii_is_space(*p))) {
            ++p;
            ++tok_len;
        }

        /* Skip leading driver path token (e.g. C:\OPENLNK.SYS) if present */
        if (first_token && (tok_start[0] != '/') && (tok_start[0] != '-')) {
            first_token = false;
            continue;
        }
        first_token = false;

        if ((tok_len >= 5U) && ((tok_start[0] == '/') || (tok_start[0] == '-'))) {
            const char *sw = &tok_start[1];
            if (ascii_ieq_prefix(sw, k_sw_com)) {
                if ((tok_len == 5U) && (sw[3] >= '1') && (sw[3] <= '4')) {
                    out_cfg->com_number = (uint8_t)(sw[3] - '0');
                } else if ((tok_len == 6U) && (sw[3] == ':') && (sw[4] >= '1') && (sw[4] <= '4')) {
                    out_cfg->com_number = (uint8_t)(sw[4] - '0');
                }
            } else if (ascii_ieq_prefix(sw, k_sw_baud)) {
                size_t digits = 0U;
                uint32_t baud = parse_u32_token(&sw[5], &digits);
                uint8_t idx = 0U;
                if ((digits > 0U) && ((5U + digits) == (tok_len - 1U)) &&
                    olnk_bps_to_baud_index(baud, &idx)) {
                    out_cfg->max_baud = baud;
                }
            } else if (ascii_ieq_prefix(sw, k_sw_drives)) {
                size_t digits = 0U;
                uint32_t drv = parse_u32_token(&sw[7], &digits);
                if ((digits > 0U) && ((7U + digits) == (tok_len - 1U)) &&
                    (drv >= 1UL) && (drv <= (uint32_t)OLNK_SYS_MAX_UNITS)) {
                    out_cfg->max_units = (uint8_t)drv;
                }
            }
        }
    }
}

static size_t banner_append_str(char *dst, size_t pos, size_t cap, const char *s) {
    while ((*s != '\0') && ((pos + 1U) < cap)) {
        dst[pos++] = *s++;
    }
    return pos;
}

static size_t banner_append_u32(char *dst, size_t pos, size_t cap, uint32_t val) {
    char tmp[12];
    size_t n = 0U;
    if (val == 0UL) {
        if ((pos + 1U) < cap) {
            dst[pos++] = '0';
        }
        return pos;
    }
    while ((val > 0UL) && (n < sizeof(tmp))) {
        tmp[n++] = (char)('0' + (char)(val % 10UL));
        val /= 10UL;
    }
    while ((n > 0U) && ((pos + 1U) < cap)) {
        dst[pos++] = tmp[--n];
    }
    return pos;
}

static void sys_format_init_banner(
    char *out,
    size_t cap,
    uint8_t com_num,
    uint32_t max_baud,
    uint8_t unit_count,
    uint8_t first_drive,
    bool connected
) {
    size_t pos = 0U;
    pos = banner_append_str(out, pos, cap, k_ban_hdr);
    pos = banner_append_u32(out, pos, cap, (uint32_t)com_num);
    pos = banner_append_str(out, pos, cap, k_ban_space);
    pos = banner_append_u32(out, pos, cap, max_baud);
    pos = banner_append_str(out, pos, cap, k_ban_bps);
    pos = banner_append_u32(out, pos, cap, (uint32_t)unit_count);
    pos = banner_append_str(out, pos, cap, k_ban_drv);
    if ((unit_count > 0U) && ((uint16_t)first_drive < OLNK_MAX_DRIVES)) {
        char first_ch = (char)('A' + (char)first_drive);
        uint8_t last_idx = (uint8_t)(first_drive + unit_count - 1U);
        if (last_idx >= OLNK_MAX_DRIVES) {
            last_idx = (uint8_t)(OLNK_MAX_DRIVES - 1U);
        }
        if ((pos + 2U) < cap) {
            out[pos++] = first_ch;
            out[pos++] = ':';
        }
        if (unit_count > 1U) {
            if ((pos + 3U) < cap) {
                out[pos++] = '-';
                out[pos++] = (char)('A' + (char)last_idx);
                out[pos++] = ':';
            }
        }
    }
    pos = banner_append_str(out, pos, cap, connected ? k_ban_conn : k_ban_stby);
    if (pos < cap) {
        out[pos] = '\0';
    } else if (cap > 0U) {
        out[cap - 1U] = '\0';
    }
}

static void sys_read_cmdline(
    uint16_t seg,
    uint16_t off,
    char out_buf[OLNK_SYS_CMDLINE_MAX],
    const olnk_dos_sys_mem_ops_t *mem_ops
) {
    uint16_t i;
    sys_far_normalize(&seg, &off);
    for (i = 0U; i < (uint16_t)(OLNK_SYS_CMDLINE_MAX - 1U); ++i) {
        uint8_t ch = 0U;
        SYS_FAR_READ(mem_ops, seg, (uint16_t)(off + i), &ch, 1U);
        if ((ch == 0U) || (ch == (uint8_t)'\r') || (ch == (uint8_t)'\n')) {
            break;
        }
        out_buf[i] = (char)ch;
    }
    out_buf[i] = '\0';
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
    g_sys_active_units = g_sys_max_units;
    g_sys_unit_count = 0U;
    g_sys_bpb_valid_mask = 0U;
    g_sys_connected = false;
    memcpy(g_sys_ret_vol_label, k_default_label, 12U);

    for (i = 0U; i < g_sys_max_units; ++i) {
        g_sys_units[i].srv_unit = OLNK_MAP_NONE;
        memcpy(g_sys_units[i].bpb_wire, k_default_bpb_wire, OLNK_LEN_DOS_BPB);
        memcpy(g_sys_units[i].vol_label, k_default_label, 12U);
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

static uint16_t sys_handle_init(
    uint16_t rh_seg,
    uint16_t rh_off,
    const uint8_t *rh,
    uint8_t rh_len,
    uint16_t drv_seg,
    uint16_t resident_end,
    const olnk_dos_sys_mem_ops_t *mem_ops,
    const olnk_dos_io_ops_t *custom_io,
    olnk_dos_sys_req_t *req
) {
    olnk_dos_sys_cfg_t cfg;
    uint16_t args_off = olnk_read_u16_le(&rh[18]);
    uint16_t args_seg = olnk_read_u16_le(&rh[20]);
    uint8_t first_drv = ((rh_len >= 23U) && (rh[22] < OLNK_MAX_DRIVES)) ? rh[22] : 3U;
    uint8_t u;
    uint8_t units_out;
    uint16_t break_off;
    uint16_t status;
    char *scratch = (char *)g_sys_client.link.rx_buffer;

    if ((args_seg != 0U) || (args_off != 0U)) {
        sys_read_cmdline(args_seg, args_off, scratch, mem_ops);
    } else {
        scratch[0] = '\0';
    }
    olnk_dos_sys_parse_args(scratch, &cfg);
    g_sys_first_drive = first_drv;

    if (olnk_dos_sys_init(cfg.com_number, cfg.max_baud, cfg.max_units, custom_io) != OLNK_OK) {
        SYS_SET_UNITS(mem_ops, 0U);
        sys_rh_write_u8(rh_seg, rh_off, 13U, 0U, mem_ops);
        sys_rh_write_far_ptr(rh_seg, rh_off, 14U, 0U, drv_seg, mem_ops);
        status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
        sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
        return status;
    }

    (void)sys_ensure_connected();
    units_out = (g_sys_unit_count > 0U) ? g_sys_unit_count : cfg.max_units;
    if ((units_out == 0U) || (units_out > OLNK_SYS_MAX_UNITS)) {
        units_out = cfg.max_units;
    }
    g_sys_active_units = units_out;
    g_sys_max_units = units_out;
    status = OLNK_DOS_STATUS_DONE;

    /* If server responded at boot, prefetch live remote BPBs for each mapped unit */
    if (g_sys_connected) {
        for (u = 0U; u < units_out; ++u) {
            memset(req, 0, sizeof(*req));
            req->cmd = OLNK_DOS_CMD_BUILD_BPB;
            req->unit = u;
            req->media_descriptor = 0xF8U;
            (void)olnk_dos_sys_dispatch(req);
        }
    }

    for (u = 0U; u < units_out; ++u) {
        g_sys_bpb_ptrs[u] = sys_bpb_wire_offset(mem_ops, u);
    }
    sys_sync_tables_to_drv_seg(drv_seg, mem_ops);

#if defined(__WATCOMC__) && defined(__DOS__)
    /* Trim unused trailing g_sys_units[units_out..7] slots + all INIT_TEXT/INIT_DATA */
    break_off = (uint16_t)(uintptr_t)&g_sys_units[units_out];
    if ((resident_end != 0U) && (break_off > resident_end)) {
        break_off = resident_end;
    }
#else
    break_off = resident_end;
#endif

    SYS_SET_UNITS(mem_ops, units_out);
    sys_rh_write_u8(rh_seg, rh_off, 13U, units_out, mem_ops);
    sys_rh_write_far_ptr(rh_seg, rh_off, 14U, break_off, drv_seg, mem_ops);
    sys_rh_write_far_ptr(rh_seg, rh_off, 18U, sys_bpb_array_offset(mem_ops), drv_seg, mem_ops);
    sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);

    sys_format_init_banner(
        scratch,
        OLNK_SYS_BANNER_MAX,
        cfg.com_number,
        cfg.max_baud,
        units_out,
        first_drv,
        g_sys_connected
    );
    SYS_PRINT_INIT(mem_ops, scratch);
    return status;
}

#if defined(__WATCOMC__) && defined(__DOS__)
#pragma code_seg("_TEXT", "CODE")
#pragma data_seg("_DATA", "DATA")
#endif
