/* cleanroom/src/storage_mem.c */
#include "interlnk/storage.h"

#include <string.h>

static ilnk_proto_error_t mem_read_sectors(
    uint8_t drive_index,
    uint32_t start_sector,
    uint16_t sector_count,
    uint8_t *out_buf,
    void *user_data
) {
    ilnk_mem_drive_t *drv = (ilnk_mem_drive_t *)user_data;
    uint32_t end_sector;
    size_t offset;
    size_t byte_len;

    if ((drv == NULL) || (drv->data == NULL) || (out_buf == NULL)) {
        return ILNK_PROTO_ERR_IO_FAILURE;
    }
    if (drive_index != 0U) {
        return ILNK_PROTO_ERR_INVALID_DRIVE;
    }
    if (drv->media_changed_flag) {
        drv->media_changed_flag = false;
        return ILNK_PROTO_ERR_MEDIA_CHANGED;
    }
    end_sector = start_sector + (uint32_t)sector_count;
    if ((end_sector < start_sector) || (end_sector > drv->total_sectors)) {
        return ILNK_PROTO_ERR_SECTOR_RANGE;
    }

    offset = (size_t)start_sector * ILNK_SECTOR_SIZE;
    byte_len = (size_t)sector_count * ILNK_SECTOR_SIZE;
    memcpy(out_buf, &drv->data[offset], byte_len);
    return ILNK_PROTO_ERR_NONE;
}

static ilnk_proto_error_t mem_write_sectors(
    uint8_t drive_index,
    uint32_t start_sector,
    uint16_t sector_count,
    const uint8_t *in_buf,
    void *user_data
) {
    ilnk_mem_drive_t *drv = (ilnk_mem_drive_t *)user_data;
    uint32_t end_sector;
    size_t offset;
    size_t byte_len;

    if ((drv == NULL) || (drv->data == NULL) || (in_buf == NULL)) {
        return ILNK_PROTO_ERR_IO_FAILURE;
    }
    if (drive_index != 0U) {
        return ILNK_PROTO_ERR_INVALID_DRIVE;
    }
    if (drv->media_changed_flag) {
        drv->media_changed_flag = false;
        return ILNK_PROTO_ERR_MEDIA_CHANGED;
    }
    if (drv->write_protected) {
        return ILNK_PROTO_ERR_WRITE_PROTECTED;
    }
    end_sector = start_sector + (uint32_t)sector_count;
    if ((end_sector < start_sector) || (end_sector > drv->total_sectors)) {
        return ILNK_PROTO_ERR_SECTOR_RANGE;
    }

    offset = (size_t)start_sector * ILNK_SECTOR_SIZE;
    byte_len = (size_t)sector_count * ILNK_SECTOR_SIZE;
    memcpy(&drv->data[offset], in_buf, byte_len);
    return ILNK_PROTO_ERR_NONE;
}

static bool mem_media_changed(uint8_t drive_index, void *user_data) {
    ilnk_mem_drive_t *drv = (ilnk_mem_drive_t *)user_data;
    if ((drv == NULL) || (drive_index != 0U)) {
        return false;
    }
    return drv->media_changed_flag;
}

ilnk_storage_ops_t ilnk_mem_drive_ops(ilnk_mem_drive_t *drive) {
    ilnk_storage_ops_t ops;
    ops.read_sectors = mem_read_sectors;
    ops.write_sectors = mem_write_sectors;
    ops.media_changed = mem_media_changed;
    ops.user_data = drive;
    return ops;
}
