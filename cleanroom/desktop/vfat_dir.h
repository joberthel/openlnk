/* cleanroom/desktop/vfat_dir.h
 *
 * Desktop Virtual FAT16 (Read-Write & Read-Only) directory storage backend
 * for opensvr (--dir <path>).
 *
 * Exposes a host directory (including subdirectories) as a valid MS-DOS 6.22
 * FAT16 block device behind olnk_storage_ops_t:
 *   - Synthesizes VBR/BPB, FAT1/FAT2, and 8.3 directory tables on the fly.
 *   - Streams unmodified file clusters directly from host files on demand.
 *   - Copy-on-Write dirty cluster overlay reconciles DOS writes (create, edit,
 *     rename, delete, mkdir, rmdir) back into the host directory safely.
 *   - Detects live host directory changes on MEDIA_CHECK and signals 0xFF
 *     (MEDIA_CHANGED) so MS-DOS invalidates caches and sees new host files.
 *
 * Lives strictly in cleanroom/desktop/ so libopenlnk core remains zero-alloc.
 */
#ifndef OPENLNK_DESKTOP_VFAT_DIR_H
#define OPENLNK_DESKTOP_VFAT_DIR_H

#include <stdbool.h>
#include <stdint.h>

#include "openlnk/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vfat_dir_drive vfat_dir_drive_t;

/**
 * Open and index a host directory as a Virtual FAT16 drive.
 * Returns NULL on failure (or on non-POSIX platforms).
 */
vfat_dir_drive_t *vfat_dir_open(
    const char *root_path,
    bool read_only,
    const char *volume_label
);

/**
 * Flush any pending writes to the host directory and release all resources.
 */
void vfat_dir_close(vfat_dir_drive_t *drv);

/**
 * Fill drive metadata (BPB, total_sectors, label, serial, attrs, RO flag).
 */
uint16_t vfat_dir_get_info(vfat_dir_drive_t *drv, olnk_drive_info_t *out_info);

/**
 * Perform MEDIA_CHECK: checks host directory fingerprint (rate-limited) and
 * rebuilds virtual FAT16 + returns OLNK_MEDIA_CHANGED (0xFF) if host files
 * changed externally.
 */
uint16_t vfat_dir_media_check(
    vfat_dir_drive_t *drv,
    uint8_t media_descriptor,
    uint8_t *out_media_status,
    olnk_drive_info_t *out_info
);

/**
 * Build BPB for the virtual FAT16 volume.
 */
uint16_t vfat_dir_build_bpb(
    vfat_dir_drive_t *drv,
    uint8_t media_descriptor,
    olnk_dos_bpb_t *out_bpb
);

/**
 * Read logical partition sectors (LBA 0 = VBR).
 */
uint16_t vfat_dir_read_sectors(
    vfat_dir_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    uint8_t *out_buf,
    uint16_t *out_transferred
);

/**
 * Write logical partition sectors (returns 0x8100 when read_only).
 * Reconciles consistent FAT16 file/directory changes back to the host folder.
 */
uint16_t vfat_dir_write_sectors(
    vfat_dir_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    const uint8_t *in_buf,
    uint16_t *out_transferred,
    bool verify
);

/**
 * Update virtual volume serial / label via Generic IOCTL 08h/46h.
 */
uint16_t vfat_dir_set_media_id(
    vfat_dir_drive_t *drv,
    const olnk_generic_ioctl_mid_t *mid
);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_DESKTOP_VFAT_DIR_H */
