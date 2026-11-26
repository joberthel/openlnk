/* cleanroom/include/openlnk/storage.h
 *
 * Clean-room storage backend interface, FAT12/FAT16 BPB/VBR & MBR partition
 * parser, and zero-allocation in-memory drive backend for libopenlnk.
 *
 * Strictly ISO C99, zero dynamic memory allocation, zero floating-point.
 */
#ifndef OPENLNK_STORAGE_H
#define OPENLNK_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openlnk/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    olnk_dos_bpb_t bpb;
    uint32_t total_sectors;
    uint32_t volume_serial;
    char volume_label[12];    /* Null-terminated, up to 11 chars (may be space-padded) */
    char filesystem_type[9];  /* Null-terminated, up to 8 chars (e.g. "FAT16   ") */
    uint16_t drive_attrs;     /* MS-DOS Block Driver attrs (default 0x08C2) */
    bool present;
    bool ready;
    bool write_protected;
    bool removable;
} olnk_drive_info_t;

typedef struct olnk_storage_ops {
    /* Highest present unit index + 1 (0..26). */
    uint8_t (*drive_count)(void *user_data);

    /* Query drive metadata and BPB. Returns DOS status (0x0100 or 0x81xx). */
    uint16_t (*get_drive_info)(
        uint8_t unit,
        olnk_drive_info_t *out_info,
        void *user_data
    );

    /* Media check: sets *out_media_status (0x01/0x00/0xFF) and fills out_info.
     * Returns DOS status word (0x0100 or 0x81xx). */
    uint16_t (*media_check)(
        uint8_t unit,
        uint8_t media_descriptor,
        uint8_t *out_media_status,
        olnk_drive_info_t *out_info,
        void *user_data
    );

    /* Build BPB: fills out_bpb. Returns DOS status word (0x0100 or 0x81xx). */
    uint16_t (*build_bpb)(
        uint8_t unit,
        uint8_t media_descriptor,
        olnk_dos_bpb_t *out_bpb,
        void *user_data
    );

    /* Read logical sectors starting at partition-relative start_lba. */
    uint16_t (*read_sectors)(
        uint8_t unit,
        uint32_t start_lba,
        uint16_t sector_count,
        uint8_t *out_buf,
        uint16_t *out_transferred,
        void *user_data
    );

    /* Write logical sectors starting at partition-relative start_lba. */
    uint16_t (*write_sectors)(
        uint8_t unit,
        uint32_t start_lba,
        uint16_t sector_count,
        const uint8_t *in_buf,
        uint16_t *out_transferred,
        bool verify,
        void *user_data
    );

    /* Optional: Generic IOCTL 08h/46h Set Media ID. May be NULL (-> 0x8103). */
    uint16_t (*set_media_id)(
        uint8_t unit,
        const olnk_generic_ioctl_mid_t *mid,
        void *user_data
    );

    void *user_data;
} olnk_storage_ops_t;

/* =========================================================================
 * Reusable FAT12 / FAT16 / MBR / Fixed-VHD Helpers (Zero-Alloc, Integer-Only)
 * ========================================================================= */

/**
 * Return true if the 512-byte sector is a valid FAT12/FAT16 boot sector (VBR)
 * and fill out_info with parsed BPB, serial, volume label, and filesystem type.
 */
bool olnk_fat_parse_vbr(const uint8_t *vbr_sector, olnk_drive_info_t *out_info);

/**
 * Inspect sector 0 of a disk image. If it is an MBR (not a superfloppy VBR)
 * containing a FAT12/FAT16 partition (types 0x01, 0x04, 0x06, 0x0E), populate
 * out_start_lba and out_num_sectors and return true.
 */
bool olnk_mbr_find_fat_partition(
    const uint8_t *mbr_sector,
    uint32_t *out_start_lba,
    uint32_t *out_num_sectors
);

/**
 * Return true if the final 512 bytes of a disk image start with the Fixed-Size
 * VHD "conectix" signature.
 */
bool olnk_vhd_has_fixed_footer(const uint8_t *last_512_bytes);

/**
 * Format an integer-only drive capacity string (e.g. "(10Mb)" or "(1440Kb)")
 * suitable for opcode 0x1C GET_DRIVE_INFO.
 */
void olnk_fat_format_size_string(
    uint32_t total_sectors,
    uint16_t bytes_per_sector,
    char out_str[10]
);

/**
 * Copy a volume label, trimming trailing spaces (for MEDIA_CHECK / GET_DRIVE_INFO).
 */
void olnk_label_trim_copy(char *dst, size_t dst_cap, const char *src, size_t src_len);

/**
 * Copy a string right-padded with ASCII spaces (for GENERIC_IOCTL MID fields).
 */
void olnk_label_pad_copy(uint8_t *dst, size_t dst_len, const char *src);

/**
 * Build a minimal, spec-valid FAT12/FAT16 Volume Boot Record (512 bytes) for
 * synthetic/in-memory volumes and unit tests.
 */
void olnk_fat_build_synthetic_vbr(
    uint8_t vbr_out[OLNK_SECTOR_SIZE],
    uint32_t total_sectors,
    uint8_t media_descriptor,
    const char *volume_label,
    uint32_t volume_serial
);

/* =========================================================================
 * Zero-Alloc In-Memory Drive Backend (Single Drive & Multi-Drive Set)
 * ========================================================================= */

typedef struct {
    uint8_t *data;
    uint32_t raw_sectors;
    uint32_t partition_lba;
    uint32_t volume_sectors;
    olnk_drive_info_t info;
    bool write_protected;
    bool not_ready;
    bool media_changed_flag;
    bool invalid_disk_change;
} olnk_mem_drive_t;

typedef struct {
    olnk_mem_drive_t *drives[OLNK_MAX_DRIVES];
    uint8_t drive_count;
} olnk_mem_drive_set_t;

/**
 * Initialize an in-memory drive from a caller-supplied static byte buffer.
 * Automatically parses MBR + FAT12/FAT16 VBR if present, or synthesizes BPB metadata.
 */
void olnk_mem_drive_init(
    olnk_mem_drive_t *drv,
    uint8_t *data,
    uint32_t raw_sectors
);

/**
 * Replace the backing buffer of a removable in-memory drive, re-parse its VBR,
 * and mark media_changed_flag = true for the next MEDIA_CHECK.
 */
void olnk_mem_drive_swap_media(
    olnk_mem_drive_t *drv,
    uint8_t *new_data,
    uint32_t new_raw_sectors
);

/**
 * Storage vtable exposing a single olnk_mem_drive_t at server unit 0.
 */
olnk_storage_ops_t olnk_mem_drive_ops(olnk_mem_drive_t *drive);

/**
 * Storage vtable exposing multiple drives mapped to arbitrary server unit indices (0..25).
 */
olnk_storage_ops_t olnk_mem_drive_set_ops(olnk_mem_drive_set_t *set);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_STORAGE_H */
