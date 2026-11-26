/* cleanroom/include/interlnk/storage.h */
#ifndef INTERLNK_STORAGE_H
#define INTERLNK_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interlnk/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ilnk_storage_ops {
    ilnk_proto_error_t (*read_sectors)(
        uint8_t drive_index,
        uint32_t start_sector,
        uint16_t sector_count,
        uint8_t *out_buf,
        void *user_data
    );
    ilnk_proto_error_t (*write_sectors)(
        uint8_t drive_index,
        uint32_t start_sector,
        uint16_t sector_count,
        const uint8_t *in_buf,
        void *user_data
    );
    bool (*media_changed)(uint8_t drive_index, void *user_data);
    void *user_data;
} ilnk_storage_ops_t;

/**
 * Phase 1 MVP In-Memory / Raw Sector Image Backend (zero malloc).
 * Caller supplies a statically allocated sector buffer.
 */
typedef struct {
    uint8_t *data;
    uint32_t total_sectors;
    bool write_protected;
    bool media_changed_flag;
} ilnk_mem_drive_t;

ilnk_storage_ops_t ilnk_mem_drive_ops(ilnk_mem_drive_t *drive);

#ifdef __cplusplus
}
#endif

#endif /* INTERLNK_STORAGE_H */
