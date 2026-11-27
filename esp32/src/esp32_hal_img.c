/* esp32/src/esp32_hal_img.c
 *
 * Disk Image File Storage HAL (.img / Fixed-Size .vhd) over POSIX stdio VFS
 * (e.g. FAT32/exFAT SD card mounted at /sdcard on ESP-IDF, or local files on host).
 *
 * Cleanly separated from esp32_hal_sd.c so applications that only need
 * zero-alloc raw SD block mode do not pay for VFS/FATFS linkage.
 *
 * Strictly ISO C99, zero dynamic allocation (malloc/free), zero floating-point.
 */
#include "openlnk_esp32.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Pure ISO C99 File Seek / Size Helpers (Safe Up to LONG_MAX / 2 GiB FAT16)
 * ========================================================================= */

static bool img_seek_sector(FILE *fp, uint32_t phys_lba) {
    uint64_t byte_off = (uint64_t)phys_lba * (uint64_t)OLNK_SECTOR_SIZE;
    if ((fp == NULL) || (byte_off > (uint64_t)LONG_MAX)) {
        return false;
    }
    return fseek(fp, (long)byte_off, SEEK_SET) == 0;
}

static uint64_t img_get_file_size(FILE *fp) {
    long sz;
    if (fp == NULL) {
        return 0ULL;
    }
    if (fseek(fp, 0L, SEEK_END) != 0) {
        return 0ULL;
    }
    sz = ftell(fp);
    return (sz > 0L) ? (uint64_t)sz : 0ULL;
}

static bool img_is_fat12_or_fat16_ptype(uint8_t ptype) {
    return (ptype == 0x01U) || (ptype == 0x04U) || (ptype == 0x06U) || (ptype == 0x0EU);
}

static bool img_find_explicit_mbr_part(
    const uint8_t sec0[OLNK_SECTOR_SIZE],
    int8_t part_idx,
    uint32_t *out_start_lba,
    uint32_t *out_num_sectors
) {
    olnk_drive_info_t probe;
    const uint8_t *ent;
    uint8_t ptype;
    uint32_t start_lba;
    uint32_t num_sec;

    if ((part_idx < 0) || (part_idx >= 4)) {
        return false;
    }
    if ((sec0[510] != 0x55U) || (sec0[511] != 0xAAU)) {
        return false;
    }
    if (olnk_fat_parse_vbr(sec0, &probe)) {
        return false;
    }

    ent = &sec0[0x1BEU + ((size_t)(uint8_t)part_idx * 16U)];
    ptype = ent[4];
    start_lba = olnk_read_u32_le(&ent[8]);
    num_sec = olnk_read_u32_le(&ent[12]);

    if ((!img_is_fat12_or_fat16_ptype(ptype)) || (start_lba == 0UL) || (num_sec == 0UL)) {
        return false;
    }

    *out_start_lba = start_lba;
    *out_num_sectors = num_sec;
    return true;
}

static bool img_refresh_vbr(olnk_esp32_img_drive_t *drv) {
    FILE *fp;
    uint32_t part_lba = 0UL;
    uint32_t part_sec = 0UL;
    uint32_t end_part;
    olnk_drive_info_t parsed;

    if ((drv == NULL) || (drv->file_handle == NULL) || (drv->raw_sectors == 0UL)) {
        return false;
    }
    fp = (FILE *)drv->file_handle;

    if (!img_seek_sector(fp, 0UL) ||
        (fread(drv->scratch, 1U, OLNK_SECTOR_SIZE, fp) != OLNK_SECTOR_SIZE)) {
        return false;
    }

    drv->partition_lba = 0UL;
    drv->volume_sectors = drv->raw_sectors;

    if (drv->partition_index >= 0) {
        if (!img_find_explicit_mbr_part(drv->scratch, drv->partition_index, &part_lba, &part_sec)) {
            return false;
        }
        end_part = part_lba + part_sec;
        if ((end_part < part_lba) || (end_part > drv->raw_sectors)) {
            return false;
        }
        if (!img_seek_sector(fp, part_lba) ||
            (fread(drv->scratch, 1U, OLNK_SECTOR_SIZE, fp) != OLNK_SECTOR_SIZE)) {
            return false;
        }
        drv->partition_lba = part_lba;
        drv->volume_sectors = part_sec;
    } else if (olnk_mbr_find_fat_partition(drv->scratch, &part_lba, &part_sec)) {
        end_part = part_lba + part_sec;
        if ((end_part >= part_lba) && (end_part <= drv->raw_sectors)) {
            if (!img_seek_sector(fp, part_lba) ||
                (fread(drv->scratch, 1U, OLNK_SECTOR_SIZE, fp) != OLNK_SECTOR_SIZE)) {
                return false;
            }
            drv->partition_lba = part_lba;
            drv->volume_sectors = part_sec;
        }
    }

    if (!olnk_fat_parse_vbr(drv->scratch, &parsed)) {
        return false;
    }
    if (parsed.bpb.bytes_per_sector != OLNK_SECTOR_SIZE) {
        return false;
    }

    if (parsed.total_sectors > drv->volume_sectors) {
        parsed.total_sectors = drv->volume_sectors;
    } else if (parsed.total_sectors > 0UL) {
        drv->volume_sectors = parsed.total_sectors;
    }

    parsed.write_protected = drv->write_protected;
    drv->info = parsed;
    return true;
}

/* =========================================================================
 * Public Image Open / Swap / Close API
 * ========================================================================= */

static olnk_status_t img_open_internal(
    olnk_esp32_img_drive_t *drv,
    const char *path,
    int8_t partition_index,
    bool read_only,
    bool mark_changed
) {
    FILE *fp = NULL;
    uint64_t fsize;
    bool ro = read_only;
    bool is_vhd = false;

    if ((drv == NULL) || (path == NULL) || (path[0] == '\0')) {
        return OLNK_ERR_INVALID_ARG;
    }
    if ((partition_index < (int8_t)OLNK_ESP32_PART_AUTO) || (partition_index >= 4)) {
        return OLNK_ERR_INVALID_ARG;
    }

    if (!ro) {
        fp = fopen(path, "r+b");
        if (fp == NULL) {
            fp = fopen(path, "rb");
            ro = true;
        }
    } else {
        fp = fopen(path, "rb");
    }

    if (fp == NULL) {
        return OLNK_ERR_HAL_IO;
    }

    fsize = img_get_file_size(fp);
    if (fsize < (uint64_t)OLNK_SECTOR_SIZE) {
        (void)fclose(fp);
        return OLNK_ERR_INVALID_FRAME;
    }

    /* Strip trailing 512-byte Fixed-Size VHD "conectix" footer if present */
    if ((fsize >= (uint64_t)(OLNK_SECTOR_SIZE * 2U)) &&
        ((fsize % (uint64_t)OLNK_SECTOR_SIZE) == 0ULL)) {
        uint64_t last_sec64 = (fsize / (uint64_t)OLNK_SECTOR_SIZE) - 1ULL;
        if (last_sec64 <= (uint64_t)UINT32_MAX) {
            uint8_t tail[OLNK_SECTOR_SIZE];
            if (img_seek_sector(fp, (uint32_t)last_sec64) &&
                (fread(tail, 1U, OLNK_SECTOR_SIZE, fp) == OLNK_SECTOR_SIZE) &&
                olnk_vhd_has_fixed_footer(tail)) {
                fsize -= (uint64_t)OLNK_SECTOR_SIZE;
                is_vhd = true;
            }
        }
    }

    if ((fsize / (uint64_t)OLNK_SECTOR_SIZE) > (uint64_t)UINT32_MAX) {
        (void)fclose(fp);
        return OLNK_ERR_BUFFER_OVERFLOW;
    }

    /* Close previous handle if swapping */
    if (drv->file_handle != NULL) {
        (void)fflush((FILE *)drv->file_handle);
        (void)fclose((FILE *)drv->file_handle);
    }

    memset(drv, 0, sizeof(*drv));
    drv->file_handle = fp;
    drv->raw_sectors = (uint32_t)(fsize / (uint64_t)OLNK_SECTOR_SIZE);
    drv->partition_index = partition_index;
    drv->is_vhd = is_vhd;
    drv->write_protected = ro;
    drv->media_changed_pending = mark_changed;

    if (!img_refresh_vbr(drv)) {
        (void)fclose(fp);
        memset(drv, 0, sizeof(*drv));
        return OLNK_ERR_INVALID_FRAME;
    }

    drv->present = true;
    return OLNK_OK;
}

olnk_status_t olnk_esp32_img_open(
    olnk_esp32_img_drive_t *drv,
    const char *path,
    int8_t partition_index,
    bool read_only
) {
    if (drv != NULL) {
        memset(drv, 0, sizeof(*drv));
    }
    return img_open_internal(drv, path, partition_index, read_only, false);
}

olnk_status_t olnk_esp32_img_swap(
    olnk_esp32_img_drive_t *drv,
    const char *new_path,
    int8_t partition_index,
    bool read_only
) {
    return img_open_internal(drv, new_path, partition_index, read_only, true);
}

void olnk_esp32_img_close(olnk_esp32_img_drive_t *drv) {
    if (drv == NULL) {
        return;
    }
    if (drv->file_handle != NULL) {
        FILE *fp = (FILE *)drv->file_handle;
        (void)fflush(fp);
        (void)fclose(fp);
        drv->file_handle = NULL;
    }
    drv->present = false;
    drv->info.ready = false;
}

/* =========================================================================
 * olnk_storage_ops_t Image File Backend Implementation
 * ========================================================================= */

static uint16_t img_drv_get_info(olnk_esp32_img_drive_t *drv, olnk_drive_info_t *out_info) {
    if (out_info == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }
    memset(out_info, 0, sizeof(*out_info));
    if ((drv == NULL) || (!drv->present) || (drv->file_handle == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    drv->info.write_protected = drv->write_protected;
    *out_info = drv->info;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t img_drv_media_check(
    olnk_esp32_img_drive_t *drv,
    uint8_t media_descriptor,
    uint8_t *out_media_status,
    olnk_drive_info_t *out_info
) {
    (void)media_descriptor;
    if ((out_media_status == NULL) || (out_info == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }
    memset(out_info, 0, sizeof(*out_info));
    if ((drv == NULL) || (!drv->present) || (drv->file_handle == NULL)) {
        *out_media_status = (uint8_t)OLNK_MEDIA_UNSURE;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    drv->info.write_protected = drv->write_protected;
    *out_info = drv->info;
    if (drv->media_changed_pending) {
        drv->media_changed_pending = false;
        *out_media_status = (uint8_t)OLNK_MEDIA_CHANGED;
    } else {
        *out_media_status = (uint8_t)OLNK_MEDIA_UNCHANGED;
    }
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t img_drv_build_bpb(
    olnk_esp32_img_drive_t *drv,
    uint8_t media_descriptor,
    olnk_dos_bpb_t *out_bpb
) {
    (void)media_descriptor;
    if (out_bpb == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }
    if ((drv == NULL) || (!drv->present) || (drv->file_handle == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (!img_refresh_vbr(drv)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_MEDIA);
    }
    *out_bpb = drv->info.bpb;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t img_drv_read_sectors(
    olnk_esp32_img_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    uint8_t *out_buf,
    uint16_t *out_transferred
) {
    FILE *fp;
    uint32_t end_lba;
    uint32_t phys_lba;
    uint32_t phys_end;
    size_t byte_len;

    if (out_transferred != NULL) {
        *out_transferred = 0U;
    }
    if ((drv == NULL) || (!drv->present) || (drv->file_handle == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if ((out_buf == NULL) || (sector_count == 0U)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
    }

    end_lba = start_lba + (uint32_t)sector_count;
    if ((end_lba < start_lba) || (end_lba > drv->volume_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }

    phys_lba = drv->partition_lba + start_lba;
    phys_end = phys_lba + (uint32_t)sector_count;
    if ((phys_lba < drv->partition_lba) || (phys_end < phys_lba) || (phys_end > drv->raw_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }

    fp = (FILE *)drv->file_handle;
    byte_len = (size_t)sector_count * OLNK_SECTOR_SIZE;
    if (!img_seek_sector(fp, phys_lba) ||
        (fread(out_buf, 1U, byte_len, fp) != byte_len)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
    }

    if (out_transferred != NULL) {
        *out_transferred = sector_count;
    }
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t img_drv_write_sectors(
    olnk_esp32_img_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    const uint8_t *in_buf,
    uint16_t *out_transferred,
    bool verify
) {
    FILE *fp;
    uint32_t end_lba;
    uint32_t phys_lba;
    uint32_t phys_end;
    size_t byte_len;

    if (out_transferred != NULL) {
        *out_transferred = 0U;
    }
    if ((drv == NULL) || (!drv->present) || (drv->file_handle == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (drv->write_protected) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
    }
    if ((in_buf == NULL) || (sector_count == 0U)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
    }

    end_lba = start_lba + (uint32_t)sector_count;
    if ((end_lba < start_lba) || (end_lba > drv->volume_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }

    phys_lba = drv->partition_lba + start_lba;
    phys_end = phys_lba + (uint32_t)sector_count;
    if ((phys_lba < drv->partition_lba) || (phys_end < phys_lba) || (phys_end > drv->raw_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }

    fp = (FILE *)drv->file_handle;
    byte_len = (size_t)sector_count * OLNK_SECTOR_SIZE;
    if (!img_seek_sector(fp, phys_lba) ||
        (fwrite(in_buf, 1U, byte_len, fp) != byte_len) ||
        (fflush(fp) != 0)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
    }

    if (verify) {
        uint16_t i;
        if (!img_seek_sector(fp, phys_lba)) {
            return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
        }
        for (i = 0U; i < sector_count; ++i) {
            const uint8_t *expected = &in_buf[(size_t)i * OLNK_SECTOR_SIZE];
            if ((fread(drv->scratch, 1U, OLNK_SECTOR_SIZE, fp) != OLNK_SECTOR_SIZE) ||
                (memcmp(drv->scratch, expected, OLNK_SECTOR_SIZE) != 0)) {
                return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
            }
        }
    }

    if (start_lba == 0UL) {
        (void)img_refresh_vbr(drv);
    }

    if (out_transferred != NULL) {
        *out_transferred = sector_count;
    }
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t img_drv_set_media_id(
    olnk_esp32_img_drive_t *drv,
    const olnk_generic_ioctl_mid_t *mid
) {
    FILE *fp;

    if ((drv == NULL) || (!drv->present) || (drv->file_handle == NULL) || (mid == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (drv->write_protected) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
    }

    fp = (FILE *)drv->file_handle;
    if (!img_seek_sector(fp, drv->partition_lba) ||
        (fread(drv->scratch, 1U, OLNK_SECTOR_SIZE, fp) != OLNK_SECTOR_SIZE)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
    }

    if (drv->scratch[38] == 0x29U) {
        olnk_write_u32_le(&drv->scratch[39], mid->serial_number);
        memcpy(&drv->scratch[43], mid->volume_label, 11U);
        memcpy(&drv->scratch[54], mid->filesystem_type, 8U);
        if (!img_seek_sector(fp, drv->partition_lba) ||
            (fwrite(drv->scratch, 1U, OLNK_SECTOR_SIZE, fp) != OLNK_SECTOR_SIZE) ||
            (fflush(fp) != 0)) {
            return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
        }
    }

    drv->info.volume_serial = mid->serial_number;
    memcpy(drv->info.volume_label, mid->volume_label, 11U);
    drv->info.volume_label[11] = '\0';
    memcpy(drv->info.filesystem_type, mid->filesystem_type, 8U);
    drv->info.filesystem_type[8] = '\0';
    return OLNK_DOS_STATUS_DONE;
}

/* =========================================================================
 * Single-Drive & Multi-Drive Set Vtables
 * ========================================================================= */

static olnk_esp32_img_drive_t *resolve_img_single(uint8_t unit, void *ud) {
    if (unit != 0U) {
        return NULL;
    }
    return (olnk_esp32_img_drive_t *)ud;
}

static olnk_esp32_img_drive_t *resolve_img_set(uint8_t unit, void *ud) {
    olnk_esp32_img_drive_set_t *set = (olnk_esp32_img_drive_set_t *)ud;
    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES)) {
        return NULL;
    }
    return set->drives[unit];
}

static uint8_t img_single_drive_count(void *ud) {
    return (ud != NULL) ? 1U : 0U;
}

static uint16_t img_single_get_info(uint8_t unit, olnk_drive_info_t *out, void *ud) {
    return img_drv_get_info(resolve_img_single(unit, ud), out);
}

static uint16_t img_single_media_check(
    uint8_t unit,
    uint8_t md,
    uint8_t *ms,
    olnk_drive_info_t *info,
    void *ud
) {
    return img_drv_media_check(resolve_img_single(unit, ud), md, ms, info);
}

static uint16_t img_single_build_bpb(uint8_t unit, uint8_t md, olnk_dos_bpb_t *bpb, void *ud) {
    return img_drv_build_bpb(resolve_img_single(unit, ud), md, bpb);
}

static uint16_t img_single_read(
    uint8_t unit,
    uint32_t lba,
    uint16_t cnt,
    uint8_t *buf,
    uint16_t *xfer,
    void *ud
) {
    return img_drv_read_sectors(resolve_img_single(unit, ud), lba, cnt, buf, xfer);
}

static uint16_t img_single_write(
    uint8_t unit,
    uint32_t lba,
    uint16_t cnt,
    const uint8_t *buf,
    uint16_t *xfer,
    bool ver,
    void *ud
) {
    return img_drv_write_sectors(resolve_img_single(unit, ud), lba, cnt, buf, xfer, ver);
}

static uint16_t img_single_set_mid(
    uint8_t unit,
    const olnk_generic_ioctl_mid_t *mid,
    void *ud
) {
    return img_drv_set_media_id(resolve_img_single(unit, ud), mid);
}

olnk_storage_ops_t olnk_esp32_img_drive_ops(olnk_esp32_img_drive_t *drv) {
    olnk_storage_ops_t ops;
    ops.drive_count = img_single_drive_count;
    ops.get_drive_info = img_single_get_info;
    ops.media_check = img_single_media_check;
    ops.build_bpb = img_single_build_bpb;
    ops.read_sectors = img_single_read;
    ops.write_sectors = img_single_write;
    ops.set_media_id = img_single_set_mid;
    ops.user_data = drv;
    return ops;
}

void olnk_esp32_img_drive_set_init(olnk_esp32_img_drive_set_t *set) {
    if (set == NULL) {
        return;
    }
    memset(set, 0, sizeof(*set));
}

bool olnk_esp32_img_drive_set_attach(
    olnk_esp32_img_drive_set_t *set,
    uint8_t unit,
    olnk_esp32_img_drive_t *drv
) {
    if ((set == NULL) || (drv == NULL) || (unit >= OLNK_MAX_DRIVES)) {
        return false;
    }
    set->drives[unit] = drv;
    if ((uint8_t)(unit + 1U) > set->drive_count) {
        set->drive_count = (uint8_t)(unit + 1U);
    }
    return true;
}

static uint8_t img_set_drive_count(void *ud) {
    const olnk_esp32_img_drive_set_t *set = (const olnk_esp32_img_drive_set_t *)ud;
    return (set != NULL) ? set->drive_count : 0U;
}

static uint16_t img_set_get_info(uint8_t unit, olnk_drive_info_t *out, void *ud) {
    return img_drv_get_info(resolve_img_set(unit, ud), out);
}

static uint16_t img_set_media_check(
    uint8_t unit,
    uint8_t md,
    uint8_t *ms,
    olnk_drive_info_t *info,
    void *ud
) {
    return img_drv_media_check(resolve_img_set(unit, ud), md, ms, info);
}

static uint16_t img_set_build_bpb(uint8_t unit, uint8_t md, olnk_dos_bpb_t *bpb, void *ud) {
    return img_drv_build_bpb(resolve_img_set(unit, ud), md, bpb);
}

static uint16_t img_set_read(
    uint8_t unit,
    uint32_t lba,
    uint16_t cnt,
    uint8_t *buf,
    uint16_t *xfer,
    void *ud
) {
    return img_drv_read_sectors(resolve_img_set(unit, ud), lba, cnt, buf, xfer);
}

static uint16_t img_set_write(
    uint8_t unit,
    uint32_t lba,
    uint16_t cnt,
    const uint8_t *buf,
    uint16_t *xfer,
    bool ver,
    void *ud
) {
    return img_drv_write_sectors(resolve_img_set(unit, ud), lba, cnt, buf, xfer, ver);
}

static uint16_t img_set_set_mid(
    uint8_t unit,
    const olnk_generic_ioctl_mid_t *mid,
    void *ud
) {
    return img_drv_set_media_id(resolve_img_set(unit, ud), mid);
}

olnk_storage_ops_t olnk_esp32_img_drive_set_ops(olnk_esp32_img_drive_set_t *set) {
    olnk_storage_ops_t ops;
    ops.drive_count = img_set_drive_count;
    ops.get_drive_info = img_set_get_info;
    ops.media_check = img_set_media_check;
    ops.build_bpb = img_set_build_bpb;
    ops.read_sectors = img_set_read;
    ops.write_sectors = img_set_write;
    ops.set_media_id = img_set_set_mid;
    ops.user_data = set;
    return ops;
}
