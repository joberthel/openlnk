/* cleanroom/dos/dos_hal_disk.h
 *
 * Clean-room 16-bit Real-Mode MS-DOS Logical Sector Storage Backend
 * (INT 25h Absolute Disk Read / INT 26h Absolute Disk Write) for OPENSVR.EXE.
 *
 * Strictly ISO C99, zero dynamic memory allocation, zero floating-point,
 * 8086/8088 safe (Open Watcom v2 -ms -0 -za99 -wx -we).
 */
#ifndef OPENLNK_DOS_HAL_DISK_H
#define OPENLNK_DOS_HAL_DISK_H

#include <stdbool.h>
#include <stdint.h>

#include "openlnk/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Injectable raw INT 25h/26h hooks for host unit testing */
typedef struct {
    uint16_t (*abs_read)(uint8_t drive, uint32_t start_lba, uint16_t count, uint8_t *buf, void *ctx);
    uint16_t (*abs_write)(uint8_t drive, uint32_t start_lba, uint16_t count, const uint8_t *buf, void *ctx);
    void *ctx;
} olnk_dos_disk_hooks_t;

typedef struct {
    bool enabled[OLNK_MAX_DRIVES];
    olnk_drive_info_t info[OLNK_MAX_DRIVES];
    uint8_t drive_count;
    bool read_only;
    uint8_t vbr_scratch[OLNK_SECTOR_SIZE];
    olnk_dos_disk_hooks_t hooks;
} olnk_dos_disk_set_t;

/**
 * Initialize a DOS disk set. Pass custom_hooks = NULL on real MS-DOS hardware
 * to use native 8086 INT 25h / INT 26h extended 32-bit sector I/O.
 */
void olnk_dos_disk_init(
    olnk_dos_disk_set_t *set,
    bool read_only,
    const olnk_dos_disk_hooks_t *custom_hooks
);

/**
 * Enable and probe a 0-based DOS drive unit (0=A:, 2=C:, ...).
 * Reads sector 0 (VBR) to populate FAT12/FAT16 BPB metadata if media is ready.
 */
bool olnk_dos_disk_add_unit(olnk_dos_disk_set_t *set, uint8_t unit);

/**
 * Return an olnk_storage_ops_t vtable backed by MS-DOS INT 25h / INT 26h.
 */
olnk_storage_ops_t olnk_dos_disk_ops(olnk_dos_disk_set_t *set);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_DOS_HAL_DISK_H */
