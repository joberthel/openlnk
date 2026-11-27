/* cleanroom/dos/openlnk_sys_stub.c
 *
 * Clean-room 16-bit Real-Mode MS-DOS Block Device Driver (OPENLNK.SYS) core.
 * Freestanding zero-CRT C99 implementation (compiles with Open Watcom -zl -s):
 *   - No <stdio.h>, no <stdlib.h>, no <ctype.h>, zero dynamic allocation, zero FP.
 *   - Parses CONFIG.SYS arguments (/COM1../COM4, /BAUD:<rate>, /DRIVES:<1..8>).
 *   - Marshals byte-exact MS-DOS 5.0/6.22 Request Headers (Cmds 0x00..0x19)
 *     including 16/32-bit LBA, 4 KiB bounce buffer with 64 KiB wrap safety,
 *     packed 25-byte BPB + INIT BPB pointer array, Media Check volume IDs,
 *     and Generic IOCTL 08h/66h & 08h/46h.
 */
#include "openlnk_sys.h"

#include <string.h>

#define OLNK_SYS_CMDLINE_MAX 128U
#define OLNK_SYS_RH_MAX_LEN  30U

/* Static resident driver state (lives in _BSS; zeroed on first Interrupt call) */
static olnk_client_t g_sys_client;
static olnk_dos_uart_t g_sys_uart;
static olnk_dos_bpb_t g_sys_bpbs[OLNK_SYS_MAX_UNITS];
static uint8_t g_sys_bpb_wire[OLNK_SYS_MAX_UNITS][OLNK_LEN_DOS_BPB];
static uint16_t g_sys_bpb_ptrs[OLNK_SYS_MAX_UNITS];
static char g_sys_vol_labels[OLNK_SYS_MAX_UNITS][12];
static char g_sys_prev_vol_labels[OLNK_SYS_MAX_UNITS][12];
static uint8_t g_sys_unit_map[OLNK_SYS_MAX_UNITS];
static bool g_sys_bpb_valid[OLNK_SYS_MAX_UNITS];
static uint8_t g_sys_bounce_buf[OLNK_SYS_BOUNCE_BYTES];
static char g_sys_init_cmdline[OLNK_SYS_CMDLINE_MAX];
static char g_sys_init_banner[96];
static uint8_t g_sys_unit_count = 0U;
static uint8_t g_sys_active_units = 4U;
static uint8_t g_sys_max_units = 4U;
static uint8_t g_sys_first_drive = 3U; /* Default D: (updated from INIT rh+22) */
static bool g_sys_initialized = false;
static bool g_sys_connected = false;

/* =========================================================================
 * Freestanding ASCII & Far-Pointer Helpers (Zero <ctype.h> / <stdio.h>)
 * ========================================================================= */

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

/* Normalize real-mode seg:off so off is always in 0..15 (prevents 64 KiB wrap) */
static void sys_far_normalize(uint16_t *seg, uint16_t *off) {
    *seg = (uint16_t)(*seg + (uint16_t)(*off >> 4U));
    *off = (uint16_t)(*off & 0x000FU);
}

/* Advance real-mode seg:off by an arbitrary byte count across 64 KiB boundaries */
static void sys_far_advance(uint16_t *seg, uint16_t *off, uint32_t bytes) {
    uint32_t total = (uint32_t)(*off) + bytes;
    *seg = (uint16_t)(*seg + (uint16_t)(total >> 4U));
    *off = (uint16_t)(total & 0x000FUL);
}

/* Valid conservative 10 MiB FAT16 placeholder BPB (prevents DOS boot div-by-0) */
static void sys_fill_default_bpb(olnk_dos_bpb_t *bpb) {
    memset(bpb, 0, sizeof(*bpb));
    bpb->bytes_per_sector = OLNK_SECTOR_SIZE;
    bpb->sectors_per_cluster = 2U;
    bpb->reserved_sectors = 1U;
    bpb->num_fats = 2U;
    bpb->root_entries = 512U;
    bpb->total_sectors_16 = 20480U;
    bpb->media_descriptor = 0xF8U;
    bpb->sectors_per_fat = 40U;
    bpb->sectors_per_track = 32U;
    bpb->num_heads = 2U;
    bpb->hidden_sectors = 0UL;
    bpb->total_sectors_32 = 0UL;
}

static void sys_set_default_label(char dst[12]) {
    memcpy(dst, "NO NAME    ", 11U);
    dst[11] = '\0';
}

/* =========================================================================
 * Driver Table Addressing (Native Near Offsets on DOS; Mirrored on Host)
 * ========================================================================= */

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
    return (uint16_t)(uintptr_t)&g_sys_bpb_wire[unit][0];
#else
    uint16_t base = sys_bpb_array_offset(mem_ops);
    return (uint16_t)(base + (uint16_t)(OLNK_SYS_MAX_UNITS * 2U) +
                      (uint16_t)((uint16_t)unit * (uint16_t)OLNK_LEN_DOS_BPB));
#endif
}

static uint16_t sys_vol_label_offset(const olnk_dos_sys_mem_ops_t *mem_ops, uint8_t unit) {
#if defined(__WATCOMC__) && defined(__DOS__)
    (void)mem_ops;
    return (uint16_t)(uintptr_t)&g_sys_prev_vol_labels[unit][0];
#else
    uint16_t base = sys_bpb_array_offset(mem_ops);
    return (uint16_t)(base + (uint16_t)(OLNK_SYS_MAX_UNITS * 2U) +
                      (uint16_t)(OLNK_SYS_MAX_UNITS * OLNK_LEN_DOS_BPB) +
                      (uint16_t)((uint16_t)unit * 12U));
#endif
}

static void sys_sync_tables_to_drv_seg(
    uint16_t drv_seg,
    const olnk_dos_sys_mem_ops_t *mem_ops
) {
    uint8_t u;
    for (u = 0U; u < OLNK_SYS_MAX_UNITS; ++u) {
        g_sys_bpb_ptrs[u] = sys_bpb_wire_offset(mem_ops, u);
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    /* On 16-bit DOS, g_sys_bpb_ptrs/wire/prev_vol_labels already live in drv_seg (CS==DS) */
    (void)drv_seg;
#else
    if ((mem_ops != NULL) && (mem_ops->far_write != NULL)) {
        uint8_t ptr_raw[OLNK_SYS_MAX_UNITS * 2U];
        uint16_t arr_seg = drv_seg;
        uint16_t arr_off = sys_bpb_array_offset(mem_ops);
        for (u = 0U; u < OLNK_SYS_MAX_UNITS; ++u) {
            olnk_write_u16_le(&ptr_raw[(size_t)u * 2U], g_sys_bpb_ptrs[u]);
        }
        sys_far_normalize(&arr_seg, &arr_off);
        mem_ops->far_write(arr_seg, arr_off, ptr_raw, (uint16_t)sizeof(ptr_raw), mem_ops->ctx);

        for (u = 0U; u < OLNK_SYS_MAX_UNITS; ++u) {
            uint16_t b_seg = drv_seg;
            uint16_t b_off = sys_bpb_wire_offset(mem_ops, u);
            uint16_t v_seg = drv_seg;
            uint16_t v_off = sys_vol_label_offset(mem_ops, u);
            sys_far_normalize(&b_seg, &b_off);
            mem_ops->far_write(b_seg, b_off, g_sys_bpb_wire[u], OLNK_LEN_DOS_BPB, mem_ops->ctx);
            sys_far_normalize(&v_seg, &v_off);
            mem_ops->far_write(v_seg, v_off, g_sys_prev_vol_labels[u], 12U, mem_ops->ctx);
        }
    }
#endif
}

/* =========================================================================
 * CONFIG.SYS Argument Parser (/COM1../COM4, /BAUD:<rate>, /DRIVES:<1..8>)
 * ========================================================================= */

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
            if (ascii_ieq_prefix(sw, "COM")) {
                if ((tok_len == 5U) && (sw[3] >= '1') && (sw[3] <= '4')) {
                    out_cfg->com_number = (uint8_t)(sw[3] - '0');
                } else if ((tok_len == 6U) && (sw[3] == ':') && (sw[4] >= '1') && (sw[4] <= '4')) {
                    out_cfg->com_number = (uint8_t)(sw[4] - '0');
                }
            } else if (ascii_ieq_prefix(sw, "BAUD:")) {
                size_t digits = 0U;
                uint32_t baud = parse_u32_token(&sw[5], &digits);
                uint8_t idx = 0U;
                if ((digits > 0U) && ((5U + digits) == (tok_len - 1U)) &&
                    olnk_bps_to_baud_index(baud, &idx)) {
                    out_cfg->max_baud = baud;
                }
            } else if (ascii_ieq_prefix(sw, "DRIVES:")) {
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

/* =========================================================================
 * Freestanding Boot Banner Formatter (Zero <stdio.h>, '$'-Terminated)
 * ========================================================================= */

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
    pos = banner_append_str(out, pos, cap, "OPENLNK.SYS v0.2: COM");
    pos = banner_append_u32(out, pos, cap, (uint32_t)com_num);
    pos = banner_append_str(out, pos, cap, " ");
    pos = banner_append_u32(out, pos, cap, max_baud);
    pos = banner_append_str(out, pos, cap, " bps, ");
    pos = banner_append_u32(out, pos, cap, (uint32_t)unit_count);
    pos = banner_append_str(out, pos, cap, " drive(s) (");
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
    pos = banner_append_str(out, pos, cap, connected ? ") [connected]\r\n$" : ") [standby]\r\n$");
    if (pos < cap) {
        out[pos] = '\0';
    } else if (cap > 0U) {
        out[cap - 1U] = '\0';
    }
}

/* =========================================================================
 * Core Client Connection & Command Dispatcher (0x00..0x19)
 * ========================================================================= */

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
        if (olnk_client_map_config(
                &g_sys_client, g_sys_max_units, g_sys_first_drive, &mresp) != OLNK_OK) {
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
            /* Force DOS to call BUILD_BPB on fresh mapping instead of trusting boot placeholder */
            g_sys_bpb_valid[i] = false;
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
    g_sys_active_units = g_sys_max_units;
    g_sys_unit_count = 0U;
    g_sys_connected = false;

    for (i = 0U; i < OLNK_SYS_MAX_UNITS; ++i) {
        g_sys_unit_map[i] = OLNK_MAP_NONE;
        g_sys_bpb_valid[i] = false;
        sys_fill_default_bpb(&g_sys_bpbs[i]);
        (void)olnk_encode_dos_bpb(&g_sys_bpbs[i], g_sys_bpb_wire[i], OLNK_LEN_DOS_BPB);
        sys_set_default_label(g_sys_vol_labels[i]);
        sys_set_default_label(g_sys_prev_vol_labels[i]);
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
        g_sys_active_units = req->unit;
        req->status = OLNK_DOS_STATUS_DONE;
        return req->status;
    }

    if (req->cmd == OLNK_DOS_CMD_REMOVABLE_MEDIA) {
        if (req->unit >= g_sys_active_units) {
            req->status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
        } else {
            /* BUSY=0 -> removable media so DOS honors MEDIA_CHECK on remote drives */
            req->status = OLNK_DOS_STATUS_DONE;
        }
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
                if (((resp.status & OLNK_DOS_STATUS_ERROR) == 0U) &&
                    (!g_sys_bpb_valid[req->unit]) &&
                    (req->media_status == (uint8_t)OLNK_MEDIA_UNCHANGED)) {
                    /* Force DOS to call BUILD_BPB at least once after offline boot / remap */
                    req->media_status = (uint8_t)OLNK_MEDIA_CHANGED;
                }
                if (req->media_status == (uint8_t)OLNK_MEDIA_CHANGED) {
                    memcpy(g_sys_prev_vol_labels[req->unit], g_sys_vol_labels[req->unit], 12U);
                    memcpy(req->volume_label, g_sys_prev_vol_labels[req->unit], 12U);
                }
                if (resp.volume_label[0] != '\0') {
                    memcpy(g_sys_vol_labels[req->unit], resp.volume_label, 12U);
                    g_sys_vol_labels[req->unit][11] = '\0';
                }
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
                    (void)olnk_encode_dos_bpb(
                        &resp.bpb, g_sys_bpb_wire[req->unit], OLNK_LEN_DOS_BPB);
                    g_sys_bpb_valid[req->unit] = true;
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
                if (resp.has_volume_id) {
                    memcpy(g_sys_prev_vol_labels[req->unit], resp.volume_label, 12U);
                    g_sys_prev_vol_labels[req->unit][11] = '\0';
                    memcpy(req->volume_label, g_sys_prev_vol_labels[req->unit], 12U);
                }
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
                if (resp.has_volume_id) {
                    memcpy(g_sys_prev_vol_labels[req->unit], resp.volume_label, 12U);
                    g_sys_prev_vol_labels[req->unit][11] = '\0';
                    memcpy(req->volume_label, g_sys_prev_vol_labels[req->unit], 12U);
                }
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

/* =========================================================================
 * Byte-Exact MS-DOS 5.0/6.22 Request Header Marshaller (Cmds 0x00..0x19)
 * ========================================================================= */

static void sys_rh_write_u8(
    uint16_t rh_seg,
    uint16_t rh_off,
    uint16_t field_off,
    uint8_t val,
    const olnk_dos_sys_mem_ops_t *mem_ops
) {
    uint16_t seg = rh_seg;
    uint16_t off = rh_off;
    sys_far_advance(&seg, &off, (uint32_t)field_off);
    mem_ops->far_write(seg, off, &val, 1U, mem_ops->ctx);
}

static void sys_rh_write_u16(
    uint16_t rh_seg,
    uint16_t rh_off,
    uint16_t field_off,
    uint16_t val,
    const olnk_dos_sys_mem_ops_t *mem_ops
) {
    uint8_t raw[2];
    uint16_t seg = rh_seg;
    uint16_t off = rh_off;
    olnk_write_u16_le(raw, val);
    sys_far_advance(&seg, &off, (uint32_t)field_off);
    mem_ops->far_write(seg, off, raw, 2U, mem_ops->ctx);
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
    uint16_t seg = rh_seg;
    uint16_t off = rh_off;
    olnk_write_u16_le(&raw[0], ptr_off);
    olnk_write_u16_le(&raw[2], ptr_seg);
    sys_far_advance(&seg, &off, (uint32_t)field_off);
    mem_ops->far_write(seg, off, raw, 4U, mem_ops->ctx);
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
        uint16_t c_seg = seg;
        uint16_t c_off = off;
        sys_far_advance(&c_seg, &c_off, (uint32_t)i);
        mem_ops->far_read(c_seg, c_off, &ch, 1U, mem_ops->ctx);
        if ((ch == 0U) || (ch == (uint8_t)'\r') || (ch == (uint8_t)'\n')) {
            break;
        }
        out_buf[i] = (char)ch;
    }
    out_buf[i] = '\0';
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

    if ((mem_ops == NULL) || (mem_ops->far_read == NULL) || (mem_ops->far_write == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }

    sys_far_normalize(&rh_seg, &rh_off);
    memset(rh, 0, sizeof(rh));
    mem_ops->far_read(rh_seg, rh_off, rh, 13U, mem_ops->ctx);

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
        uint16_t tail_seg = rh_seg;
        uint16_t tail_off = rh_off;
        sys_far_advance(&tail_seg, &tail_off, 13UL);
        mem_ops->far_read(tail_seg, tail_off, &rh[13], (uint16_t)(read_len - 13U), mem_ops->ctx);
    }

    memset(&req, 0, sizeof(req));
    req.cmd = cmd;
    req.unit = unit;

    switch (cmd) {
        case OLNK_DOS_CMD_INIT: {
            olnk_dos_sys_cfg_t cfg;
            uint16_t args_off = olnk_read_u16_le(&rh[18]);
            uint16_t args_seg = olnk_read_u16_le(&rh[20]);
            uint8_t first_drv = ((rh_len >= 23U) && (rh[22] < OLNK_MAX_DRIVES)) ? rh[22] : 3U;
            uint8_t u;
            uint8_t units_out;

            if ((args_seg != 0U) || (args_off != 0U)) {
                sys_read_cmdline(args_seg, args_off, g_sys_init_cmdline, mem_ops);
            } else {
                g_sys_init_cmdline[0] = '\0';
            }
            olnk_dos_sys_parse_args(g_sys_init_cmdline, &cfg);
            g_sys_first_drive = first_drv;

            if (olnk_dos_sys_init(cfg.com_number, cfg.max_baud, cfg.max_units, custom_io) !=
                OLNK_OK) {
                if (mem_ops->set_header_units != NULL) {
                    mem_ops->set_header_units(0U, mem_ops->ctx);
                }
                sys_rh_write_u8(rh_seg, rh_off, 13U, 0U, mem_ops);
                sys_rh_write_far_ptr(rh_seg, rh_off, 14U, 0U, drv_seg, mem_ops);
                status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
                sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
                return status;
            }

            status = olnk_dos_sys_dispatch(&req);
            units_out = ((req.unit > 0U) && (req.unit <= OLNK_SYS_MAX_UNITS))
                            ? req.unit
                            : cfg.max_units;
            g_sys_active_units = units_out;

            /* If server responded at boot, prefetch live remote BPBs for each mapped unit */
            if (g_sys_connected) {
                for (u = 0U; u < units_out; ++u) {
                    olnk_dos_sys_req_t bpb_req;
                    memset(&bpb_req, 0, sizeof(bpb_req));
                    bpb_req.cmd = OLNK_DOS_CMD_BUILD_BPB;
                    bpb_req.unit = u;
                    bpb_req.media_descriptor = 0xF8U;
                    (void)olnk_dos_sys_dispatch(&bpb_req);
                }
            }

            sys_sync_tables_to_drv_seg(drv_seg, mem_ops);

            if (mem_ops->set_header_units != NULL) {
                mem_ops->set_header_units(units_out, mem_ops->ctx);
            }
            sys_rh_write_u8(rh_seg, rh_off, 13U, units_out, mem_ops);
            sys_rh_write_far_ptr(rh_seg, rh_off, 14U, resident_end, drv_seg, mem_ops);
            sys_rh_write_far_ptr(
                rh_seg, rh_off, 18U, sys_bpb_array_offset(mem_ops), drv_seg, mem_ops);
            sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);

            if (mem_ops->print_init_msg != NULL) {
                sys_format_init_banner(
                    g_sys_init_banner,
                    sizeof(g_sys_init_banner),
                    cfg.com_number,
                    cfg.max_baud,
                    units_out,
                    first_drv,
                    g_sys_connected
                );
                mem_ops->print_init_msg(g_sys_init_banner, mem_ops->ctx);
            }
            return status;
        }

        case OLNK_DOS_CMD_MEDIA_CHECK: {
            req.media_descriptor = rh[13];
            status = olnk_dos_sys_dispatch(&req);
            sys_rh_write_u8(rh_seg, rh_off, 14U, req.media_status, mem_ops);
            if ((req.media_status == (uint8_t)OLNK_MEDIA_CHANGED) &&
                (unit < OLNK_SYS_MAX_UNITS) && (rh_len >= 19U)) {
                sys_sync_tables_to_drv_seg(drv_seg, mem_ops);
                sys_rh_write_far_ptr(
                    rh_seg, rh_off, 15U, sys_vol_label_offset(mem_ops, unit), drv_seg, mem_ops);
            }
            sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
            return status;
        }

        case OLNK_DOS_CMD_BUILD_BPB: {
            req.media_descriptor = rh[13];
            status = olnk_dos_sys_dispatch(&req);
            if (((status & OLNK_DOS_STATUS_ERROR) == 0U) && req.has_bpb &&
                (unit < OLNK_SYS_MAX_UNITS)) {
                sys_sync_tables_to_drv_seg(drv_seg, mem_ops);
                sys_rh_write_far_ptr(
                    rh_seg, rh_off, 18U, sys_bpb_wire_offset(mem_ops, unit), drv_seg, mem_ops);
            }
            sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
            return status;
        }

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
            uint16_t bps = ((unit < OLNK_SYS_MAX_UNITS) &&
                            (g_sys_bpbs[unit].bytes_per_sector != 0U))
                               ? g_sys_bpbs[unit].bytes_per_sector
                               : OLNK_SECTOR_SIZE;
            uint16_t max_chunk = (bps > 0U) ? (uint16_t)(sizeof(g_sys_bounce_buf) / bps) : 0U;
            uint16_t done_sectors = 0U;
            uint16_t last_status = OLNK_DOS_STATUS_DONE;

            if (total_sectors == 0U) {
                sys_rh_write_u16(rh_seg, rh_off, 18U, 0U, mem_ops);
                sys_rh_write_u16(rh_seg, rh_off, 3U, OLNK_DOS_STATUS_DONE, mem_ops);
                return OLNK_DOS_STATUS_DONE;
            }
            if (max_chunk == 0U) {
                status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_MEDIA);
                sys_rh_write_u16(rh_seg, rh_off, 18U, 0U, mem_ops);
                sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
                return status;
            }

            sys_far_normalize(&xfer_seg, &xfer_off);

            while (done_sectors < total_sectors) {
                uint16_t remain = (uint16_t)(total_sectors - done_sectors);
                uint16_t chunk = (remain > max_chunk) ? max_chunk : remain;
                uint16_t chunk_bytes = (uint16_t)(chunk * bps);

                if (cmd != OLNK_DOS_CMD_READ) {
                    mem_ops->far_read(
                        xfer_seg, xfer_off, g_sys_bounce_buf, chunk_bytes, mem_ops->ctx);
                }

                memset(&req, 0, sizeof(req));
                req.cmd = cmd;
                req.unit = unit;
                req.media_descriptor = media_desc;
                req.start_lba = start_lba + (uint32_t)done_sectors;
                req.sector_count = chunk;
                req.xfer_buf = g_sys_bounce_buf;
                req.xfer_cap = (uint32_t)chunk_bytes;

                last_status = olnk_dos_sys_dispatch(&req);

                if ((cmd == OLNK_DOS_CMD_READ) && (req.sector_count > 0U)) {
                    uint16_t got_bytes = (uint16_t)(req.sector_count * bps);
                    mem_ops->far_write(
                        xfer_seg, xfer_off, g_sys_bounce_buf, got_bytes, mem_ops->ctx);
                }

                done_sectors = (uint16_t)(done_sectors + req.sector_count);

                if (((last_status & OLNK_DOS_STATUS_ERROR) != 0U) || (req.sector_count == 0U)) {
                    if (((last_status & 0x00FFU) == (uint16_t)OLNK_DOS_ERR_INVALID_DISK_CHG) &&
                        (unit < OLNK_SYS_MAX_UNITS) && (rh_len >= 26U)) {
                        sys_sync_tables_to_drv_seg(drv_seg, mem_ops);
                        sys_rh_write_far_ptr(
                            rh_seg,
                            rh_off,
                            22U,
                            sys_vol_label_offset(mem_ops, unit),
                            drv_seg,
                            mem_ops
                        );
                    }
                    break;
                }

                sys_far_advance(
                    &xfer_seg, &xfer_off, (uint32_t)req.sector_count * (uint32_t)bps);

                if (req.sector_count < chunk) {
                    break;
                }
            }

            sys_rh_write_u16(rh_seg, rh_off, 18U, done_sectors, mem_ops);
            sys_rh_write_u16(rh_seg, rh_off, 3U, last_status, mem_ops);
            return last_status;
        }

        case OLNK_DOS_CMD_OPEN:
        case OLNK_DOS_CMD_CLOSE:
        case OLNK_DOS_CMD_REMOVABLE_MEDIA: {
            status = olnk_dos_sys_dispatch(&req);
            sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
            return status;
        }

        case OLNK_DOS_CMD_GET_LOGICAL:
        case OLNK_DOS_CMD_SET_LOGICAL: {
            status = olnk_dos_sys_dispatch(&req);
            sys_rh_write_u8(rh_seg, rh_off, 1U, req.unit, mem_ops);
            sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
            return status;
        }

        case OLNK_DOS_CMD_IOCTL_QUERY: {
            req.ioctl_category = rh[13];
            req.ioctl_function = rh[14];
            status = olnk_dos_sys_dispatch(&req);
            sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
            return status;
        }

        case OLNK_DOS_CMD_GENERIC_IOCTL: {
            uint8_t mid_buf[OLNK_LEN_GENERIC_IOCTL_MID];
            uint16_t pb_off = olnk_read_u16_le(&rh[19]);
            uint16_t pb_seg = olnk_read_u16_le(&rh[21]);
            size_t mid_len = 0U;

            req.ioctl_category = rh[13];
            req.ioctl_function = rh[14];
            sys_far_normalize(&pb_seg, &pb_off);

            if ((req.ioctl_category == OLNK_IOCTL_CAT_DISK) &&
                (req.ioctl_function == OLNK_IOCTL_FN_SET_MEDIA_ID)) {
                mem_ops->far_read(pb_seg, pb_off, mid_buf, OLNK_LEN_GENERIC_IOCTL_MID, mem_ops->ctx);
                (void)olnk_decode_generic_ioctl_mid(mid_buf, OLNK_LEN_GENERIC_IOCTL_MID, &req.mid);
            }

            status = olnk_dos_sys_dispatch(&req);

            if ((req.ioctl_category == OLNK_IOCTL_CAT_DISK) &&
                (req.ioctl_function == OLNK_IOCTL_FN_GET_MEDIA_ID) &&
                ((status & OLNK_DOS_STATUS_ERROR) == 0U)) {
                if (olnk_encode_generic_ioctl_mid(&req.mid, mid_buf, sizeof(mid_buf), &mid_len) ==
                    OLNK_OK) {
                    mem_ops->far_write(
                        pb_seg, pb_off, mid_buf, OLNK_LEN_GENERIC_IOCTL_MID, mem_ops->ctx);
                }
            }

            sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
            return status;
        }

        default: {
            status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);
            sys_rh_write_u16(rh_seg, rh_off, 3U, status, mem_ops);
            return status;
        }
    }
}

/* =========================================================================
 * Diagnostic Helpers & Real-Mode 16-Bit Assembly Bridge
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

static void dos_mem_far_read(uint16_t seg, uint16_t off, void *dst, uint16_t len, void *ctx) {
    (void)ctx;
    sys_asm_far_read(seg, off, dst, len);
}

static void dos_mem_far_write(uint16_t seg, uint16_t off, const void *src, uint16_t len, void *ctx) {
    (void)ctx;
    sys_asm_far_write(seg, off, src, len);
}

static void dos_mem_set_units(uint8_t count, void *ctx) {
    (void)ctx;
    sys_asm_set_header_units(count);
}

static void dos_mem_print_init(const char *dollar_msg, void *ctx) {
    (void)ctx;
    sys_asm_dos_print(dollar_msg);
}

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
    olnk_dos_sys_mem_ops_t mem_ops;
    mem_ops.far_read = dos_mem_far_read;
    mem_ops.far_write = dos_mem_far_write;
    mem_ops.set_header_units = dos_mem_set_units;
    mem_ops.print_init_msg = dos_mem_print_init;
    mem_ops.host_table_base_off = 0U;
    mem_ops.ctx = NULL;

    (void)olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, NULL);
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
