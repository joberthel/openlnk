/* cleanroom/dos/dos_hal_disk.c
 *
 * Clean-room 16-bit Real-Mode MS-DOS INT 25h / INT 26h Storage Backend.
 * Supports DOS 4.0+/6.22 32-bit extended packet I/O (CX=0xFFFF) with
 * automatic fallback to 16-bit LBA for DOS 3.x volumes <= 32 MiB.
 */
#include "dos_hal_disk.h"

#include <string.h>

#if defined(__WATCOMC__) && defined(__DOS__)
#include <i86.h>

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

static uint16_t dos_get_ds(void) {
    struct SREGS sregs;
    segread(&sregs);
    return sregs.ds;
}
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

static uint16_t probe_unit_vbr(olnk_dos_disk_set_t *set, uint8_t unit) {
    uint16_t st = hw_abs_read(set, unit, 0UL, 1U, set->vbr_scratch);
    if ((st & OLNK_DOS_STATUS_ERROR) != 0U) {
        set->info[unit].present = true;
        set->info[unit].ready = false;
        set->info[unit].drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        set->info[unit].write_protected = set->read_only;
        return st;
    }

    if (!olnk_fat_parse_vbr(set->vbr_scratch, &set->info[unit])) {
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
