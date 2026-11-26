/* cleanroom/include/openlnk/storage.h */
#ifndef OPENLNK_STORAGE_H
#define OPENLNK_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openlnk/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct olnk_storage_ops {
    olnk_proto_error_t (*read_sectors)(
        uint8_t drive_index,
        uint32_t start_sector,
        uint16_t sector_count,
        uint8_t *out_buf,
        void *user_data
    );
    olnk_proto_error_t (*write_sectors)(
        uint8_t drive_index,
        uint32_t start_sector,
        uint16_t sector_count,
        const uint8_t *in_buf,
        void *user_data
    );
    bool (*media_changed)(uint8_t drive_index, void *user_data);
    void *user_data;
} olnk_storage_ops_t;

/**
 * Phase 1 MVP In-Memory / Raw Sector Image Backend (zero malloc).
 * Caller supplies a statically allocated sector buffer.
 */
typedef struct {
    uint8_t *data;
    uint32_t total_sectors;
    bool write_protected;
    bool media_changed_flag;
} olnk_mem_drive_t;

olnk_storage_ops_t olnk_mem_drive_ops(olnk_mem_drive_t *drive);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_STORAGE_H */
