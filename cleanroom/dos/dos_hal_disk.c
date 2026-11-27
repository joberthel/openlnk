/* cleanroom/dos/dos_hal_disk.c
 *
 * Clean-room 16-bit Real-Mode MS-DOS INT 25h / INT 26h Storage Backend.
 * Supports DOS 4.0+/6.22 32-bit extended packet I/O (CX=0xFFFF) with
 * automatic fallback to 16-bit LBA for DOS 3.x volumes <= 32 MiB.
 */
#include "dos_hal_disk.h"

#include <string.h>

#if defined(__WATCOMC__) && defined(__DOS__)

/* MS-DOS 4.0+ Extended Disk I/O Packet (10 bytes, packed) */
#pragma pack(push, 1)
typedef struct {
    uint32_t start_sector;
    uint16_t sector_count;
    uint16_t buf_off;
    uint16_t buf_seg;
} dos_ext_io_pkt_t;
#pragma pack(pop)

/*
 * Branchless, 8086-safe INT 25h extended read:
 *   Input : AL = 0-based drive, BX = near pointer to dos_ext_io_pkt_t (DS = DGROUP)
 *   Output: AX = 0x0100 (DONE) on CF=0, or 0x8100|AL (ERROR|DONE|code) on CF=1.
 *   Note  : INT 25h leaves old FLAGS on stack (popped into DX without altering CF)
 *           and may clobber BP (saved/restored safely around INT 25h).
 */
static uint16_t watcom_int25_ext(uint8_t drive, const dos_ext_io_pkt_t *pkt);
#pragma aux watcom_int25_ext = \
    "push bp"                  \
    "mov cx, 0ffffh"           \
    "int 25h"                  \
    "pop dx"                   \
    "pop bp"                   \
    "sbb dx, dx"               \
    "and ax, 00ffh"            \
    "and ax, dx"               \
    "and dx, 8000h"            \
    "or ax, dx"                \
    "or ax, 0100h"             \
    parm [al] [bx]             \
    value [ax]                 \
    modify [bx cx dx si di];

/* Branchless, 8086-safe INT 26h extended write (same ABI as INT 25h) */
static uint16_t watcom_int26_ext(uint8_t drive, const dos_ext_io_pkt_t *pkt);
#pragma aux watcom_int26_ext = \
    "push bp"                  \
    "mov cx, 0ffffh"           \
    "int 26h"                  \
    "pop dx"                   \
    "pop bp"                   \
    "sbb dx, dx"               \
    "and ax, 00ffh"            \
    "and ax, dx"               \
    "and dx, 8000h"            \
    "or ax, dx"                \
    "or ax, 0100h"             \
    parm [al] [bx]             \
    value [ax]                 \
    modify [bx cx dx si di];

/* Fallback 16-bit LBA INT 25h for DOS 3.x (<= 32 MiB) */
static uint16_t watcom_int25_std(uint8_t drive, uint16_t count, uint16_t lba16, uint8_t *buf);
#pragma aux watcom_int25_std = \
    "push bp"                  \
    "int 25h"                  \
    "pop dx"                   \
    "pop bp"                   \
    "sbb dx, dx"               \
    "and ax, 00ffh"            \
    "and ax, dx"               \
    "and dx, 8000h"            \
    "or ax, dx"                \
    "or ax, 0100h"             \
    parm [al] [cx] [dx] [bx]   \
    value [ax]                 \
    modify [bx cx dx si di];

/* Fallback 16-bit LBA INT 26h for DOS 3.x (<= 32 MiB) */
static uint16_t watcom_int26_std(uint8_t drive, uint16_t count, uint16_t lba16, const uint8_t *buf);
#pragma aux watcom_int26_std = \
    "push bp"                  \
    "int 26h"                  \
    "pop dx"                   \
    "pop bp"                   \
    "sbb dx, dx"               \
    "and ax, 00ffh"            \
    "and ax, dx"               \
    "and dx, 8000h"            \
    "or ax, dx"                \
    "or ax, 0100h"             \
    parm [al] [cx] [dx] [bx]   \
    value [ax]                 \
    modify [bx cx dx si di];

static uint16_t dos_get_ds(void);
#pragma aux dos_get_ds = \
    "mov ax, ds"         \
    value [ax];

/*
 * MS-DOS 3.2+ Generic IOCTL Get Device Parameters (INT 21h AX=440Dh CX=0860h)
 * and Get Media ID (CX=0866h): returns 1 on CF=0 (success), 0 on CF=1 (error).
 */
static uint8_t watcom_ioctl_get_dev_params(uint16_t drive_1based, void *buf);
#pragma aux watcom_ioctl_get_dev_params = \
    "mov ax, 440dh"                       \
    "mov cx, 0860h"                       \
    "int 21h"                             \
    "sbb al, al"                          \
    "inc al"                              \
    parm [bx] [dx]                        \
    value [al]                            \
    modify [ax cx];

static uint8_t watcom_ioctl_get_media_id(uint16_t drive_1based, void *buf);
#pragma aux watcom_ioctl_get_media_id = \
    "mov ax, 440dh"                     \
    "mov cx, 0866h"                     \
    "int 21h"                           \
    "sbb al, al"                        \
    "inc al"                            \
    parm [bx] [dx]                      \
    value [al]                          \
    modify [ax cx];
#endif /* __WATCOMC__ && __DOS__ */

static uint16_t hw_abs_read(
    olnk_dos_disk_set_t *set,
    uint8_t drive,
    uint32_t start_lba,
    uint16_t count,
    uint8_t *buf
) {
    if (set->hooks.abs_read != NULL) {
        return set->hooks.abs_read(drive, start_lba, count, buf, set->hooks.ctx);
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    {
        dos_ext_io_pkt_t pkt;
        uint16_t st;
        pkt.start_sector = start_lba;
        pkt.sector_count = count;
        pkt.buf_off = (uint16_t)(uintptr_t)buf;
        pkt.buf_seg = dos_get_ds();
        st = watcom_int25_ext(drive, &pkt);
        if (((st & OLNK_DOS_STATUS_ERROR) != 0U) &&
            (((st & 0x00FFU) == (uint16_t)OLNK_DOS_ERR_UNKNOWN_MEDIA) ||
             ((st & 0x00FFU) == (uint16_t)OLNK_DOS_ERR_SECTOR_NOT_FOUND)) &&
            (start_lba <= 0xFFFFUL)) {
            st = watcom_int25_std(drive, count, (uint16_t)start_lba, buf);
        }
        return st;
    }
#else
    (void)drive;
    (void)start_lba;
    (void)count;
    (void)buf;
    return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
#endif
}

static uint16_t hw_abs_write(
    olnk_dos_disk_set_t *set,
    uint8_t drive,
    uint32_t start_lba,
    uint16_t count,
    const uint8_t *buf
) {
    if (set->hooks.abs_write != NULL) {
        return set->hooks.abs_write(drive, start_lba, count, buf, set->hooks.ctx);
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    {
        dos_ext_io_pkt_t pkt;
        uint16_t st;
        pkt.start_sector = start_lba;
        pkt.sector_count = count;
        pkt.buf_off = (uint16_t)(uintptr_t)buf;
        pkt.buf_seg = dos_get_ds();
        st = watcom_int26_ext(drive, &pkt);
        if (((st & OLNK_DOS_STATUS_ERROR) != 0U) &&
            (((st & 0x00FFU) == (uint16_t)OLNK_DOS_ERR_UNKNOWN_MEDIA) ||
             ((st & 0x00FFU) == (uint16_t)OLNK_DOS_ERR_SECTOR_NOT_FOUND)) &&
            (start_lba <= 0xFFFFUL)) {
            st = watcom_int26_std(drive, count, (uint16_t)start_lba, buf);
        }
        return st;
    }
#else
    (void)drive;
    (void)start_lba;
    (void)count;
    (void)buf;
    return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
#endif
}

static bool is_pow2_u16(uint16_t v) {
    return (v != 0U) && ((v & (uint16_t)(v - 1U)) == 0U);
}

static bool is_pow2_u8(uint8_t v) {
    return (v != 0U) && ((v & (uint8_t)(v - 1U)) == 0U);
}

/*
 * Populate olnk_drive_info_t from a validated 25-byte MS-DOS BPB (used for
 * non-bootable volumes such as RAMDRIVE.SYS "RDV 1.20" that omit 0x55 0xAA
 * or use a 19-byte DOS 3.x BPB, as well as INT 21h 440Dh/0860h fallback).
 */
static bool fill_drive_info_from_bpb(const olnk_dos_bpb_t *in_bpb, olnk_drive_info_t *out) {
    olnk_dos_bpb_t bpb = *in_bpb;
    uint32_t total_sec;
    uint32_t root_sec;
    uint32_t data_start;
    uint32_t data_sec;
    uint32_t clusters;

    if ((bpb.bytes_per_sector < 128U) || (bpb.bytes_per_sector > 4096U) ||
        (!is_pow2_u16(bpb.bytes_per_sector)) || (!is_pow2_u8(bpb.sectors_per_cluster)) ||
        (bpb.reserved_sectors == 0U) || (bpb.num_fats == 0U) || (bpb.num_fats > 2U) ||
        (bpb.root_entries == 0U) || (bpb.sectors_per_fat == 0U) ||
        (bpb.media_descriptor < 0xF0U)) {
        return false;
    }

    total_sec = (bpb.total_sectors_16 != 0U)
                    ? (uint32_t)bpb.total_sectors_16
                    : bpb.total_sectors_32;
    if (total_sec == 0UL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->bpb = bpb;
    out->total_sectors = total_sec;
    out->drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
    out->present = true;
    out->ready = true;
    out->removable = (bpb.media_descriptor != 0xF8U);

    root_sec = ((uint32_t)bpb.root_entries * 32UL + (uint32_t)bpb.bytes_per_sector - 1UL) /
               (uint32_t)bpb.bytes_per_sector;
    data_start = (uint32_t)bpb.reserved_sectors +
                 ((uint32_t)bpb.num_fats * (uint32_t)bpb.sectors_per_fat) + root_sec;
    data_sec = (total_sec > data_start) ? (total_sec - data_start) : 0UL;
    clusters = data_sec / (uint32_t)bpb.sectors_per_cluster;

    memcpy(out->volume_label, "NO NAME    ", 11U);
    out->volume_label[11] = '\0';
    memcpy(out->filesystem_type, (clusters < 4085UL) ? "FAT12   " : "FAT16   ", 8U);
    out->filesystem_type[8] = '\0';
    return true;
}

static bool parse_relaxed_dos_vbr(const uint8_t *vbr, olnk_drive_info_t *out) {
    olnk_dos_bpb_t bpb;

    if ((vbr[0] != 0xEBU) && (vbr[0] != 0xE9U)) {
        return false;
    }
    if (olnk_decode_dos_bpb(&vbr[11], OLNK_LEN_DOS_BPB, &bpb) != OLNK_OK) {
        return false;
    }
    /* DOS 3.x / RAMDRIVE.SYS 19-byte BPB places boot code at offset 0x1E..0x23 */
    if ((bpb.total_sectors_16 != 0U) && (vbr[38] != 0x29U)) {
        bpb.hidden_sectors = (uint32_t)olnk_read_u16_le(&vbr[28]);
        bpb.total_sectors_32 = 0UL;
    }
    if (!fill_drive_info_from_bpb(&bpb, out)) {
        return false;
    }
    if (vbr[38] == 0x29U) {
        out->volume_serial = olnk_read_u32_le(&vbr[39]);
        memcpy(out->volume_label, &vbr[43], 11U);
        out->volume_label[11] = '\0';
        memcpy(out->filesystem_type, &vbr[54], 8U);
        out->filesystem_type[8] = '\0';
    }
    return true;
}

static uint16_t probe_unit_vbr(olnk_dos_disk_set_t *set, uint8_t unit) {
    uint16_t st = hw_abs_read(set, unit, 0UL, 1U, set->vbr_scratch);
    bool parsed = false;

    if ((st & OLNK_DOS_STATUS_ERROR) != 0U) {
        set->info[unit].present = true;
        set->info[unit].ready = false;
        set->info[unit].drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        set->info[unit].write_protected = set->read_only;
        return st;
    }

    if (olnk_fat_parse_vbr(set->vbr_scratch, &set->info[unit])) {
        parsed = true;
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    if (!parsed) {
        uint8_t dp_buf[64];
        olnk_dos_bpb_t ioctl_bpb;
        memset(dp_buf, 0, sizeof(dp_buf));
        dp_buf[0] = 0x01U; /* Current medium BPB */
        if ((watcom_ioctl_get_dev_params((uint16_t)(unit + 1U), dp_buf) != 0U) &&
            (olnk_decode_dos_bpb(&dp_buf[7], OLNK_LEN_DOS_BPB, &ioctl_bpb) == OLNK_OK)) {
            if (ioctl_bpb.total_sectors_16 != 0U) {
                ioctl_bpb.total_sectors_32 = 0UL;
            }
            if (fill_drive_info_from_bpb(&ioctl_bpb, &set->info[unit])) {
                uint8_t mid_buf[OLNK_LEN_GENERIC_IOCTL_MID];
                olnk_generic_ioctl_mid_t mid;
                memset(mid_buf, 0, sizeof(mid_buf));
                if ((watcom_ioctl_get_media_id((uint16_t)(unit + 1U), mid_buf) != 0U) &&
                    (olnk_decode_generic_ioctl_mid(mid_buf, sizeof(mid_buf), &mid) == OLNK_OK)) {
                    set->info[unit].volume_serial = mid.serial_number;
                    memcpy(set->info[unit].volume_label, mid.volume_label, 11U);
                    set->info[unit].volume_label[11] = '\0';
                    memcpy(set->info[unit].filesystem_type, mid.filesystem_type, 8U);
                    set->info[unit].filesystem_type[8] = '\0';
                }
                parsed = true;
            }
        }
    }
#endif
    if (!parsed) {
        parsed = parse_relaxed_dos_vbr(set->vbr_scratch, &set->info[unit]);
    }

    if (!parsed) {
        set->info[unit].present = true;
        set->info[unit].ready = false;
        set->info[unit].drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_MEDIA);
    }

    set->info[unit].write_protected = set->read_only;
    return OLNK_DOS_STATUS_DONE;
}

void olnk_dos_disk_init(
    olnk_dos_disk_set_t *set,
    bool read_only,
    const olnk_dos_disk_hooks_t *custom_hooks
) {
    if (set == NULL) {
        return;
    }
    memset(set, 0, sizeof(*set));
    set->read_only = read_only;
    if (custom_hooks != NULL) {
        set->hooks = *custom_hooks;
    }
}

bool olnk_dos_disk_add_unit(olnk_dos_disk_set_t *set, uint8_t unit) {
    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES)) {
        return false;
    }
    set->enabled[unit] = true;
    set->info[unit].present = true;
    set->info[unit].drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
    set->info[unit].write_protected = set->read_only;
    if ((uint8_t)(unit + 1U) > set->drive_count) {
        set->drive_count = (uint8_t)(unit + 1U);
    }
    (void)probe_unit_vbr(set, unit);
    return true;
}

/* =========================================================================
 * olnk_storage_ops_t Vtable Implementation
 * ========================================================================= */

static uint8_t ddisk_drive_count(void *ud) {
    const olnk_dos_disk_set_t *set = (const olnk_dos_disk_set_t *)ud;
    return (set != NULL) ? set->drive_count : 0U;
}

static uint16_t ddisk_get_info(uint8_t unit, olnk_drive_info_t *out, void *ud) {
    olnk_dos_disk_set_t *set = (olnk_dos_disk_set_t *)ud;
    if ((set == NULL) || (out == NULL) || (unit >= OLNK_MAX_DRIVES) || (!set->enabled[unit])) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (!set->info[unit].ready) {
        (void)probe_unit_vbr(set, unit);
    }
    *out = set->info[unit];
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t ddisk_media_check(
    uint8_t unit,
    uint8_t media_desc,
    uint8_t *out_ms,
    olnk_drive_info_t *out_info,
    void *ud
) {
    olnk_dos_disk_set_t *set = (olnk_dos_disk_set_t *)ud;
    uint32_t prev_serial;
    bool prev_ready;
    uint16_t st;
    (void)media_desc;

    if ((set == NULL) || (out_ms == NULL) || (out_info == NULL) ||
        (unit >= OLNK_MAX_DRIVES) || (!set->enabled[unit])) {
        if (out_ms != NULL) {
            *out_ms = (uint8_t)OLNK_MEDIA_UNSURE;
        }
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }

    prev_serial = set->info[unit].volume_serial;
    prev_ready = set->info[unit].ready;
    st = probe_unit_vbr(set, unit);
    *out_info = set->info[unit];

    if ((st & OLNK_DOS_STATUS_ERROR) != 0U) {
        *out_ms = (uint8_t)OLNK_MEDIA_UNSURE;
        return st;
    }

    if ((!prev_ready) || (set->info[unit].volume_serial != prev_serial)) {
        *out_ms = (uint8_t)OLNK_MEDIA_CHANGED;
    } else {
        *out_ms = (uint8_t)OLNK_MEDIA_UNCHANGED;
    }
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t ddisk_build_bpb(
    uint8_t unit,
    uint8_t media_desc,
    olnk_dos_bpb_t *out_bpb,
    void *ud
) {
    olnk_dos_disk_set_t *set = (olnk_dos_disk_set_t *)ud;
    uint16_t st;
    (void)media_desc;

    if ((set == NULL) || (out_bpb == NULL) || (unit >= OLNK_MAX_DRIVES) || (!set->enabled[unit])) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    st = probe_unit_vbr(set, unit);
    if ((st & OLNK_DOS_STATUS_ERROR) != 0U) {
        return st;
    }
    *out_bpb = set->info[unit].bpb;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t ddisk_read_sectors(
    uint8_t unit,
    uint32_t start_lba,
    uint16_t sector_count,
    uint8_t *out_buf,
    uint16_t *out_xfer,
    void *ud
) {
    olnk_dos_disk_set_t *set = (olnk_dos_disk_set_t *)ud;
    uint16_t st;

    if (out_xfer != NULL) {
        *out_xfer = 0U;
    }
    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES) || (!set->enabled[unit])) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (sector_count == 0U) {
        return OLNK_DOS_STATUS_DONE;
    }
    st = hw_abs_read(set, unit, start_lba, sector_count, out_buf);
    if (((st & OLNK_DOS_STATUS_ERROR) == 0U) && (out_xfer != NULL)) {
        *out_xfer = sector_count;
    }
    return st;
}

static uint16_t ddisk_write_sectors(
    uint8_t unit,
    uint32_t start_lba,
    uint16_t sector_count,
    const uint8_t *in_buf,
    uint16_t *out_xfer,
    bool verify,
    void *ud
) {
    olnk_dos_disk_set_t *set = (olnk_dos_disk_set_t *)ud;
    uint16_t st;
    (void)verify;

    if (out_xfer != NULL) {
        *out_xfer = 0U;
    }
    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES) || (!set->enabled[unit])) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (set->read_only) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
    }
    if (sector_count == 0U) {
        return OLNK_DOS_STATUS_DONE;
    }
    st = hw_abs_write(set, unit, start_lba, sector_count, in_buf);
    if ((st & OLNK_DOS_STATUS_ERROR) == 0U) {
        if (start_lba == 0UL) {
            (void)probe_unit_vbr(set, unit);
        }
        if (out_xfer != NULL) {
            *out_xfer = sector_count;
        }
    }
    return st;
}

olnk_storage_ops_t olnk_dos_disk_ops(olnk_dos_disk_set_t *set) {
    olnk_storage_ops_t ops;
    ops.drive_count = ddisk_drive_count;
    ops.get_drive_info = ddisk_get_info;
    ops.media_check = ddisk_media_check;
    ops.build_bpb = ddisk_build_bpb;
    ops.read_sectors = ddisk_read_sectors;
    ops.write_sectors = ddisk_write_sectors;
    ops.set_media_id = NULL;
    ops.user_data = set;
    return ops;
}
