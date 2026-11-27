/* cleanroom/src/storage_mem.c */
#include "openlnk/storage.h"

#include <string.h>

static bool is_power_of_two_u16(uint16_t v) {
    return (v != 0U) && ((v & (uint16_t)(v - 1U)) == 0U);
}

static bool is_power_of_two_u8(uint8_t v) {
    return (v != 0U) && ((v & (uint8_t)(v - 1U)) == 0U);
}

static size_t append_u32_decimal(char *dst, size_t pos, size_t cap, uint32_t val) {
    char tmp[12];
    size_t len = 0U;
    if (val == 0UL) {
        if (pos + 1U < cap) {
            dst[pos++] = '0';
        }
        return pos;
    }
    while ((val > 0UL) && (len < sizeof(tmp))) {
        tmp[len++] = (char)('0' + (char)(val % 10UL));
        val /= 10UL;
    }
    while ((len > 0U) && (pos + 1U < cap)) {
        dst[pos++] = tmp[--len];
    }
    return pos;
}

static size_t append_ascii(char *dst, size_t pos, size_t cap, const char *s) {
    while ((*s != '\0') && (pos + 1U < cap)) {
        dst[pos++] = *s++;
    }
    return pos;
}

/* =========================================================================
 * FAT12 / FAT16 / MBR / Fixed-VHD Helpers
 * ========================================================================= */

void olnk_label_trim_copy(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    size_t len;
    if ((dst == NULL) || (dst_cap == 0U)) {
        return;
    }
    memset(dst, 0, dst_cap);
    if (src == NULL) {
        return;
    }
    len = 0U;
    while ((len < src_len) && (src[len] != '\0')) {
        len += 1U;
    }
    while ((len > 0U) && (src[len - 1U] == ' ')) {
        len -= 1U;
    }
    if (len >= dst_cap) {
        len = dst_cap - 1U;
    }
    if (len > 0U) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

void olnk_label_pad_copy(uint8_t *dst, size_t dst_len, const char *src) {
    size_t i = 0U;
    if (dst == NULL) {
        return;
    }
    memset(dst, ' ', dst_len);
    if (src == NULL) {
        return;
    }
    while ((i < dst_len) && (src[i] != '\0')) {
        dst[i] = (uint8_t)src[i];
        i += 1U;
    }
}

void olnk_fat_format_size_string(
    uint32_t total_sectors,
    uint16_t bytes_per_sector,
    char out_str[10]
) {
    uint32_t kib;
    size_t pos = 0U;

    memset(out_str, 0, 10U);
    if ((total_sectors == 0UL) || (bytes_per_sector == 0U)) {
        return;
    }

    if (bytes_per_sector >= 1024U) {
        kib = total_sectors * (uint32_t)(bytes_per_sector / 1024U);
    } else {
        uint32_t div = (uint32_t)(1024U / bytes_per_sector);
        kib = (div > 0UL) ? (total_sectors / div) : total_sectors;
    }

    pos = append_ascii(out_str, pos, 10U, "(");
    if (kib >= 2048UL) {
        uint32_t mib = kib / 1024UL;
        pos = append_u32_decimal(out_str, pos, 10U, mib);
        pos = append_ascii(out_str, pos, 10U, "Mb)");
    } else {
        pos = append_u32_decimal(out_str, pos, 10U, kib);
        pos = append_ascii(out_str, pos, 10U, "Kb)");
    }
    out_str[pos] = '\0';
}

bool olnk_vhd_has_fixed_footer(const uint8_t *last_512_bytes) {
    static const uint8_t sig[8] = { 'c', 'o', 'n', 'e', 'c', 't', 'i', 'x' };
    if (last_512_bytes == NULL) {
        return false;
    }
    return memcmp(last_512_bytes, sig, sizeof(sig)) == 0;
}

bool olnk_fat_parse_vbr(const uint8_t *vbr_sector, olnk_drive_info_t *out_info) {
    olnk_dos_bpb_t bpb;
    uint32_t total_sec;
    bool valid_jmp;

    if ((vbr_sector == NULL) || (out_info == NULL)) {
        return false;
    }
    if ((vbr_sector[510] != 0x55U) || (vbr_sector[511] != 0xAAU)) {
        return false;
    }

    valid_jmp = ((vbr_sector[0] == 0xEBU) && (vbr_sector[2] == 0x90U)) ||
                (vbr_sector[0] == 0xE9U);
    if (!valid_jmp) {
        return false;
    }

    if (olnk_decode_dos_bpb(&vbr_sector[11], OLNK_LEN_DOS_BPB, &bpb) != OLNK_OK) {
        return false;
    }

    if ((bpb.bytes_per_sector < 128U) || (bpb.bytes_per_sector > 4096U) ||
        (!is_power_of_two_u16(bpb.bytes_per_sector))) {
        return false;
    }
    if (!is_power_of_two_u8(bpb.sectors_per_cluster)) {
        return false;
    }
    if ((bpb.reserved_sectors == 0U) || (bpb.num_fats == 0U) || (bpb.num_fats > 2U)) {
        return false;
    }
    if ((bpb.root_entries == 0U) || (bpb.sectors_per_fat == 0U)) {
        return false;
    }
    if (bpb.media_descriptor < 0xF0U) {
        return false;
    }

    total_sec = (bpb.total_sectors_16 != 0U)
                    ? (uint32_t)bpb.total_sectors_16
                    : bpb.total_sectors_32;
    if (total_sec == 0UL) {
        return false;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->bpb = bpb;
    out_info->total_sectors = total_sec;
    out_info->drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
    out_info->present = true;
    out_info->ready = true;
    out_info->write_protected = false;
    out_info->removable = (bpb.media_descriptor != 0xF8U);

    if (vbr_sector[38] == 0x29U) {
        out_info->volume_serial = olnk_read_u32_le(&vbr_sector[39]);
        memcpy(out_info->volume_label, &vbr_sector[43], 11U);
        out_info->volume_label[11] = '\0';
        memcpy(out_info->filesystem_type, &vbr_sector[54], 8U);
        out_info->filesystem_type[8] = '\0';
    } else {
        uint32_t root_sec = ((uint32_t)bpb.root_entries * UINT32_C(32) +
                             (uint32_t)bpb.bytes_per_sector - UINT32_C(1)) /
                            (uint32_t)bpb.bytes_per_sector;
        uint32_t data_start = (uint32_t)bpb.reserved_sectors +
                              (uint32_t)bpb.num_fats * (uint32_t)bpb.sectors_per_fat +
                              root_sec;
        uint32_t data_sec = (total_sec > data_start) ? (total_sec - data_start) : 0UL;
        uint32_t clusters = data_sec / (uint32_t)bpb.sectors_per_cluster;

        out_info->volume_serial = 0UL;
        memcpy(out_info->volume_label, "NO NAME    ", 11U);
        out_info->volume_label[11] = '\0';
        if (clusters < 4085UL) {
            memcpy(out_info->filesystem_type, "FAT12   ", 8U);
        } else {
            memcpy(out_info->filesystem_type, "FAT16   ", 8U);
        }
        out_info->filesystem_type[8] = '\0';
    }

    return true;
}

bool olnk_mbr_find_fat_partition(
    const uint8_t *mbr_sector,
    uint32_t *out_start_lba,
    uint32_t *out_num_sectors
) {
    olnk_drive_info_t probe;
    size_t i;

    if ((mbr_sector == NULL) || (out_start_lba == NULL) || (out_num_sectors == NULL)) {
        return false;
    }
    if ((mbr_sector[510] != 0x55U) || (mbr_sector[511] != 0xAAU)) {
        return false;
    }
    /* If sector 0 is already a valid FAT VBR (superfloppy), it is not an MBR. */
    if (olnk_fat_parse_vbr(mbr_sector, &probe)) {
        return false;
    }

    for (i = 0U; i < 4U; ++i) {
        const uint8_t *ent = &mbr_sector[0x1BEU + i * 16U];
        uint8_t ptype = ent[4];
        uint32_t start_lba = olnk_read_u32_le(&ent[8]);
        uint32_t num_sec = olnk_read_u32_le(&ent[12]);

        if (((ptype == 0x01U) || (ptype == 0x04U) || (ptype == 0x06U) || (ptype == 0x0EU)) &&
            (start_lba > 0UL) && (num_sec > 0UL)) {
            *out_start_lba = start_lba;
            *out_num_sectors = num_sec;
            return true;
        }
    }

    return false;
}

void olnk_fat_build_synthetic_vbr(
    uint8_t vbr_out[OLNK_SECTOR_SIZE],
    uint32_t total_sectors,
    uint8_t media_descriptor,
    const char *volume_label,
    uint32_t volume_serial
) {
    olnk_dos_bpb_t bpb;
    bool is_fat16 = (total_sectors >= 4200UL);

    memset(vbr_out, 0, OLNK_SECTOR_SIZE);
    memset(&bpb, 0, sizeof(bpb));

    vbr_out[0] = 0xEBU;
    vbr_out[1] = 0x3CU;
    vbr_out[2] = 0x90U;
    memcpy(&vbr_out[3], "OPENLNK ", 8U);

    bpb.bytes_per_sector = OLNK_SECTOR_SIZE;
    bpb.sectors_per_cluster = is_fat16 ? 2U : 1U;
    bpb.reserved_sectors = 1U;
    bpb.num_fats = 2U;
    bpb.root_entries = is_fat16 ? 512U : 224U;
    if (total_sectors <= 65535UL) {
        bpb.total_sectors_16 = (uint16_t)total_sectors;
        bpb.total_sectors_32 = 0UL;
    } else {
        bpb.total_sectors_16 = 0U;
        bpb.total_sectors_32 = total_sectors;
    }
    bpb.media_descriptor = media_descriptor;
    bpb.sectors_per_fat = is_fat16 ? 41U : 9U;
    bpb.sectors_per_track = (media_descriptor == 0xF8U) ? 17U : 18U;
    bpb.num_heads = (media_descriptor == 0xF8U) ? 4U : 2U;
    bpb.hidden_sectors = 0UL;

    (void)olnk_encode_dos_bpb(&bpb, &vbr_out[11], OLNK_LEN_DOS_BPB);

    vbr_out[36] = (media_descriptor == 0xF8U) ? 0x80U : 0x00U;
    vbr_out[37] = 0x00U;
    vbr_out[38] = 0x29U;
    olnk_write_u32_le(&vbr_out[39], volume_serial);
    olnk_label_pad_copy(&vbr_out[43], 11U, (volume_label != NULL) ? volume_label : "OPENLNK");
    olnk_label_pad_copy(&vbr_out[54], 8U, is_fat16 ? "FAT16" : "FAT12");

    vbr_out[510] = 0x55U;
    vbr_out[511] = 0xAAU;
}

/* =========================================================================
 * In-Memory Drive Backend
 * ========================================================================= */

static void mem_drive_refresh_metadata(olnk_mem_drive_t *drv) {
    uint32_t part_lba = 0UL;
    uint32_t part_sec = 0UL;
    const uint8_t *vbr;

    drv->partition_lba = 0UL;
    drv->volume_sectors = drv->raw_sectors;
    memset(&drv->info, 0, sizeof(drv->info));

    if ((drv->data == NULL) || (drv->raw_sectors == 0UL)) {
        drv->info.present = true;
        drv->info.ready = false;
        drv->info.drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        return;
    }

    if (olnk_mbr_find_fat_partition(drv->data, &part_lba, &part_sec)) {
        if ((part_lba < drv->raw_sectors) &&
            ((part_lba + part_sec) <= drv->raw_sectors)) {
            drv->partition_lba = part_lba;
            drv->volume_sectors = part_sec;
        }
    }

    vbr = &drv->data[(size_t)drv->partition_lba * OLNK_SECTOR_SIZE];
    if (!olnk_fat_parse_vbr(vbr, &drv->info)) {
        /* Raw / unformatted buffer fallback */
        drv->info.bpb.bytes_per_sector = OLNK_SECTOR_SIZE;
        drv->info.bpb.sectors_per_cluster = 1U;
        drv->info.bpb.reserved_sectors = 1U;
        drv->info.bpb.num_fats = 2U;
        drv->info.bpb.root_entries = 224U;
        if (drv->volume_sectors <= 65535UL) {
            drv->info.bpb.total_sectors_16 = (uint16_t)drv->volume_sectors;
        } else {
            drv->info.bpb.total_sectors_32 = drv->volume_sectors;
        }
        drv->info.bpb.media_descriptor = 0xF8U;
        drv->info.bpb.sectors_per_fat = 9U;
        drv->info.bpb.sectors_per_track = 18U;
        drv->info.bpb.num_heads = 2U;
        drv->info.total_sectors = drv->volume_sectors;
        drv->info.drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        drv->info.present = true;
        drv->info.ready = true;
        memcpy(drv->info.volume_label, "OPENLNK    ", 12U);
        memcpy(drv->info.filesystem_type, "FAT12   ", 9U);
    }

    if (drv->info.total_sectors > drv->volume_sectors) {
        drv->info.total_sectors = drv->volume_sectors;
    }
    drv->info.write_protected = drv->write_protected;
    drv->info.ready = !drv->not_ready;
}

void olnk_mem_drive_init(
    olnk_mem_drive_t *drv,
    uint8_t *data,
    uint32_t raw_sectors
) {
    if (drv == NULL) {
        return;
    }
    memset(drv, 0, sizeof(*drv));
    drv->data = data;
    drv->raw_sectors = raw_sectors;
    mem_drive_refresh_metadata(drv);
}

void olnk_mem_drive_swap_media(
    olnk_mem_drive_t *drv,
    uint8_t *new_data,
    uint32_t new_raw_sectors
) {
    if (drv == NULL) {
        return;
    }
    drv->data = new_data;
    drv->raw_sectors = new_raw_sectors;
    drv->not_ready = (new_data == NULL) || (new_raw_sectors == 0UL);
    drv->media_changed_flag = true;
    mem_drive_refresh_metadata(drv);
}

static olnk_mem_drive_t *resolve_single(uint8_t unit, void *user_data) {
    if (unit != 0U) {
        return NULL;
    }
    return (olnk_mem_drive_t *)user_data;
}

static olnk_mem_drive_t *resolve_set(uint8_t unit, void *user_data) {
    olnk_mem_drive_set_t *set = (olnk_mem_drive_set_t *)user_data;
    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES)) {
        return NULL;
    }
    return set->drives[unit];
}

static uint16_t mem_drv_get_info(olnk_mem_drive_t *drv, olnk_drive_info_t *out_info) {
    if (out_info == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }
    memset(out_info, 0, sizeof(*out_info));
    if (drv == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    drv->info.write_protected = drv->write_protected;
    drv->info.ready = (!drv->not_ready) && (drv->data != NULL) && (drv->volume_sectors > 0UL);
    *out_info = drv->info;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t mem_drv_media_check(
    olnk_mem_drive_t *drv,
    uint8_t media_descriptor,
    uint8_t *out_media_status,
    olnk_drive_info_t *out_info
) {
    (void)media_descriptor;
    if ((out_media_status == NULL) || (out_info == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }
    memset(out_info, 0, sizeof(*out_info));
    if (drv == NULL) {
        *out_media_status = (uint8_t)OLNK_MEDIA_UNSURE;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    drv->info.write_protected = drv->write_protected;
    drv->info.ready = (!drv->not_ready) && (drv->data != NULL) && (drv->volume_sectors > 0UL);
    *out_info = drv->info;

    if (!drv->info.ready) {
        *out_media_status = (uint8_t)OLNK_MEDIA_UNSURE;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if (drv->media_changed_flag) {
        drv->media_changed_flag = false;
        drv->invalid_disk_change = false;
        *out_media_status = (uint8_t)OLNK_MEDIA_CHANGED;
        return OLNK_DOS_STATUS_DONE;
    }
    *out_media_status = (uint8_t)OLNK_MEDIA_UNCHANGED;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t mem_drv_build_bpb(
    olnk_mem_drive_t *drv,
    uint8_t media_descriptor,
    olnk_dos_bpb_t *out_bpb
) {
    (void)media_descriptor;
    if (out_bpb == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }
    if (drv == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (drv->not_ready || (drv->data == NULL) || (drv->volume_sectors == 0UL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    mem_drive_refresh_metadata(drv);
    *out_bpb = drv->info.bpb;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t mem_drv_read(
    olnk_mem_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    uint8_t *out_buf,
    uint16_t *out_transferred
) {
    uint32_t end_lba;
    uint32_t phys_lba;
    uint16_t i;

    if (out_transferred != NULL) {
        *out_transferred = 0U;
    }
    if (drv == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (drv->not_ready || (drv->data == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if (drv->invalid_disk_change) {
        drv->invalid_disk_change = false;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_INVALID_DISK_CHG);
    }
    if ((out_buf == NULL) || (sector_count == 0U)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
    }
    end_lba = start_lba + (uint32_t)sector_count;
    if ((end_lba < start_lba) || (end_lba > drv->volume_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }

    phys_lba = drv->partition_lba + start_lba;
    for (i = 0U; i < sector_count; ++i) {
        size_t src_off = (size_t)(phys_lba + (uint32_t)i) * OLNK_SECTOR_SIZE;
        size_t dst_off = (size_t)i * OLNK_SECTOR_SIZE;
        memcpy(&out_buf[dst_off], &drv->data[src_off], OLNK_SECTOR_SIZE);
    }

    if (out_transferred != NULL) {
        *out_transferred = sector_count;
    }
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t mem_drv_write(
    olnk_mem_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    const uint8_t *in_buf,
    uint16_t *out_transferred,
    bool verify
) {
    uint32_t end_lba;
    uint32_t phys_lba;
    uint16_t i;

    if (out_transferred != NULL) {
        *out_transferred = 0U;
    }
    if (drv == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (drv->not_ready || (drv->data == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if (drv->invalid_disk_change) {
        drv->invalid_disk_change = false;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_INVALID_DISK_CHG);
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
    for (i = 0U; i < sector_count; ++i) {
        size_t dst_off = (size_t)(phys_lba + (uint32_t)i) * OLNK_SECTOR_SIZE;
        size_t src_off = (size_t)i * OLNK_SECTOR_SIZE;
        memcpy(&drv->data[dst_off], &in_buf[src_off], OLNK_SECTOR_SIZE);
        if (verify && (memcmp(&drv->data[dst_off], &in_buf[src_off], OLNK_SECTOR_SIZE) != 0)) {
            return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
        }
    }

    if (start_lba == 0UL) {
        mem_drive_refresh_metadata(drv);
    }

    if (out_transferred != NULL) {
        *out_transferred = sector_count;
    }
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t mem_drv_set_mid(olnk_mem_drive_t *drv, const olnk_generic_ioctl_mid_t *mid) {
    uint8_t *vbr;
    if ((drv == NULL) || (mid == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (drv->not_ready || (drv->data == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if (drv->write_protected) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
    }
    vbr = &drv->data[(size_t)drv->partition_lba * OLNK_SECTOR_SIZE];
    if (vbr[38] == 0x29U) {
        olnk_write_u32_le(&vbr[39], mid->serial_number);
        memcpy(&vbr[43], mid->volume_label, 11U);
        memcpy(&vbr[54], mid->filesystem_type, 8U);
    }
    drv->info.volume_serial = mid->serial_number;
    memcpy(drv->info.volume_label, mid->volume_label, 11U);
    drv->info.volume_label[11] = '\0';
    memcpy(drv->info.filesystem_type, mid->filesystem_type, 8U);
    drv->info.filesystem_type[8] = '\0';
    return OLNK_DOS_STATUS_DONE;
}

/* Single-drive vtable wrappers */
static uint8_t single_drive_count(void *ud) {
    return (ud != NULL) ? 1U : 0U;
}

static uint16_t single_get_info(uint8_t unit, olnk_drive_info_t *out, void *ud) {
    return mem_drv_get_info(resolve_single(unit, ud), out);
}

static uint16_t single_media_check(uint8_t unit, uint8_t md, uint8_t *ms, olnk_drive_info_t *info, void *ud) {
    return mem_drv_media_check(resolve_single(unit, ud), md, ms, info);
}

static uint16_t single_build_bpb(uint8_t unit, uint8_t md, olnk_dos_bpb_t *bpb, void *ud) {
    return mem_drv_build_bpb(resolve_single(unit, ud), md, bpb);
}

static uint16_t single_read(uint8_t unit, uint32_t lba, uint16_t cnt, uint8_t *buf, uint16_t *xfer, void *ud) {
    return mem_drv_read(resolve_single(unit, ud), lba, cnt, buf, xfer);
}

static uint16_t single_write(uint8_t unit, uint32_t lba, uint16_t cnt, const uint8_t *buf, uint16_t *xfer, bool ver, void *ud) {
    return mem_drv_write(resolve_single(unit, ud), lba, cnt, buf, xfer, ver);
}

static uint16_t single_set_mid(uint8_t unit, const olnk_generic_ioctl_mid_t *mid, void *ud) {
    return mem_drv_set_mid(resolve_single(unit, ud), mid);
}

olnk_storage_ops_t olnk_mem_drive_ops(olnk_mem_drive_t *drive) {
    olnk_storage_ops_t ops;
    ops.drive_count = single_drive_count;
    ops.get_drive_info = single_get_info;
    ops.media_check = single_media_check;
    ops.build_bpb = single_build_bpb;
    ops.read_sectors = single_read;
    ops.write_sectors = single_write;
    ops.set_media_id = single_set_mid;
    ops.user_data = drive;
    return ops;
}

/* Multi-drive set vtable wrappers */
static uint8_t set_drive_count(void *ud) {
    olnk_mem_drive_set_t *set = (olnk_mem_drive_set_t *)ud;
    if (set == NULL) {
        return 0U;
    }
    return set->drive_count;
}

static uint16_t set_get_info(uint8_t unit, olnk_drive_info_t *out, void *ud) {
    return mem_drv_get_info(resolve_set(unit, ud), out);
}

static uint16_t set_media_check(uint8_t unit, uint8_t md, uint8_t *ms, olnk_drive_info_t *info, void *ud) {
    return mem_drv_media_check(resolve_set(unit, ud), md, ms, info);
}

static uint16_t set_build_bpb(uint8_t unit, uint8_t md, olnk_dos_bpb_t *bpb, void *ud) {
    return mem_drv_build_bpb(resolve_set(unit, ud), md, bpb);
}

static uint16_t set_read(uint8_t unit, uint32_t lba, uint16_t cnt, uint8_t *buf, uint16_t *xfer, void *ud) {
    return mem_drv_read(resolve_set(unit, ud), lba, cnt, buf, xfer);
}

static uint16_t set_write(uint8_t unit, uint32_t lba, uint16_t cnt, const uint8_t *buf, uint16_t *xfer, bool ver, void *ud) {
    return mem_drv_write(resolve_set(unit, ud), lba, cnt, buf, xfer, ver);
}

static uint16_t set_set_mid(uint8_t unit, const olnk_generic_ioctl_mid_t *mid, void *ud) {
    return mem_drv_set_mid(resolve_set(unit, ud), mid);
}

olnk_storage_ops_t olnk_mem_drive_set_ops(olnk_mem_drive_set_t *set) {
    olnk_storage_ops_t ops;
    ops.drive_count = set_drive_count;
    ops.get_drive_info = set_get_info;
    ops.media_check = set_media_check;
    ops.build_bpb = set_build_bpb;
    ops.read_sectors = set_read;
    ops.write_sectors = set_write;
    ops.set_media_id = set_set_mid;
    ops.user_data = set;
    return ops;
}
