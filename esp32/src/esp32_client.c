/* esp32/src/esp32_client.c
 *
 * Caller-owned, thread-safe high-level ESP32 client wrapper around
 * olnk_client_t with static FreeRTOS mutex (zero malloc), optional
 * auto-reconnect with transparent session resumption, streaming raw drive
 * backup helper, and idle-aware background keepalive task.
 *
 * Strictly ISO C99, zero dynamic allocation (malloc/free), zero floating-point.
 */
#include "openlnk_esp32.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#define OLNK_ESP32_CLIENT_KA_POLL_SLICE_MS 50U
#define OLNK_ESP32_CLIENT_DEINIT_STOP_MS   1000U

/* =========================================================================
 * Monotonic Clock & Mutex Helpers
 * ========================================================================= */

#ifdef ESP_PLATFORM
static uint64_t client_idf_monotonic_ms(void *ctx) {
    int64_t us = esp_timer_get_time();
    (void)ctx;
    return (us > 0LL) ? (uint64_t)(us / 1000LL) : 0ULL;
}
#endif

static bool client_read_time_ms(const olnk_esp32_client_t *ecli, uint64_t *out_ms) {
    if (ecli->cfg.get_time_ms != NULL) {
        *out_ms = ecli->cfg.get_time_ms(ecli->cfg.time_ctx);
        return true;
    }
#ifdef ESP_PLATFORM
    *out_ms = client_idf_monotonic_ms(NULL);
    return true;
#else
    (void)out_ms;
    return false;
#endif
}

static void client_mark_activity_locked(olnk_esp32_client_t *ecli) {
    uint64_t now_ms = 0ULL;
    if (client_read_time_ms(ecli, &now_ms)) {
        ecli->last_activity_ms = now_ms;
        ecli->has_last_activity = true;
    }
}

static bool client_lock(olnk_esp32_client_t *ecli) {
    if (ecli->lock_fn != NULL) {
        return ecli->lock_fn(ecli->lock_ctx);
    }
#ifdef ESP_PLATFORM
    if (ecli->mutex_handle != NULL) {
        return xSemaphoreTake(ecli->mutex_handle, portMAX_DELAY) == pdTRUE;
    }
#endif
    return true;
}

static void client_unlock(olnk_esp32_client_t *ecli) {
    if (ecli->unlock_fn != NULL) {
        ecli->unlock_fn(ecli->lock_ctx);
        return;
    }
#ifdef ESP_PLATFORM
    if (ecli->mutex_handle != NULL) {
        (void)xSemaphoreGive(ecli->mutex_handle);
    }
#endif
}

/* =========================================================================
 * Internal Locked Connection, Session & Auto-Reconnect Helpers
 * ========================================================================= */

static void client_drain_rx_nonblock_locked(olnk_esp32_client_t *ecli) {
    uint8_t discard[32];
    uint16_t guard;

    if (ecli->client.link.transport.read == NULL) {
        return;
    }
    for (guard = 0U; guard < 64U; ++guard) {
        int n = ecli->client.link.transport.read(
            discard,
            sizeof(discard),
            0UL,
            ecli->client.link.transport.user_data
        );
        if (n <= 0) {
            break;
        }
    }
}

static bool client_status_is_recoverable(olnk_status_t st) {
    switch (st) {
        case OLNK_ERR_DISCONNECTED:
        case OLNK_ERR_HAL_TIMEOUT:
        case OLNK_ERR_SEQ_MISMATCH:
        case OLNK_ERR_INVALID_FRAME:
        case OLNK_ERR_PROTOCOL:
            return true;
        default:
            return false;
    }
}

static olnk_status_t client_connect_session_locked(
    olnk_esp32_client_t *ecli,
    uint16_t sync_retries,
    bool is_reconnect
) {
    olnk_session_sync_resp_t sresp;
    olnk_status_t st;

    /* Reset link to 9600 bps (preserving client_id & server_id for resumption) */
    olnk_client_disconnect(&ecli->client);
    ecli->client.link.max_baud_idx = ecli->cfg.max_baud_idx;
    ecli->client.link.local_requires_echo = ecli->cfg.local_requires_echo;
    ecli->client.rpc_timeout_ms = ecli->cfg.rpc_timeout_ms;

    client_drain_rx_nonblock_locked(ecli);

    st = olnk_client_connect(&ecli->client, sync_retries);
    if (st != OLNK_OK) {
        return st;
    }

    if (ecli->cfg.force_tx_echo) {
        ecli->client.link.peer_echo_forced = true;
    }

    memset(&sresp, 0, sizeof(sresp));
    st = olnk_client_session_sync(&ecli->client, false, &sresp);
    if (st != OLNK_OK) {
        olnk_client_disconnect(&ecli->client);
        return st;
    }

    ecli->last_sync = sresp;
    ecli->has_sync = true;

    if ((!ecli->client.link.session_resumed) || (!ecli->client.has_map)) {
        st = olnk_client_map_config(
            &ecli->client,
            ecli->cfg.max_client_drives,
            ecli->cfg.first_client_drive,
            NULL
        );
        if (st != OLNK_OK) {
            olnk_client_disconnect(&ecli->client);
            return st;
        }
    }

    if (is_reconnect) {
        ecli->reconnect_count += 1UL;
    }
    client_mark_activity_locked(ecli);
    return OLNK_OK;
}

static olnk_status_t client_ensure_connected_locked(olnk_esp32_client_t *ecli) {
    if (ecli->client.link.link_state == (uint8_t)OLNK_LINK_CONNECTED) {
        return OLNK_OK;
    }
    if (!ecli->cfg.auto_reconnect) {
        return OLNK_ERR_DISCONNECTED;
    }
    return client_connect_session_locked(ecli, ecli->cfg.sync_retries, ecli->has_sync);
}

static olnk_status_t client_build_bpb_locked(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint8_t media_descriptor,
    olnk_build_bpb_resp_t *out_resp
) {
    olnk_status_t st;

    st = client_ensure_connected_locked(ecli);
    if (st != OLNK_OK) {
        return st;
    }

    st = olnk_client_build_bpb(&ecli->client, unit, media_descriptor, out_resp);
    if ((st != OLNK_OK) && ecli->cfg.auto_reconnect && client_status_is_recoverable(st)) {
        st = client_connect_session_locked(ecli, ecli->cfg.sync_retries, true);
        if (st == OLNK_OK) {
            st = olnk_client_build_bpb(&ecli->client, unit, media_descriptor, out_resp);
        }
    }
    if (st == OLNK_OK) {
        client_mark_activity_locked(ecli);
    }
    return st;
}

static olnk_status_t client_media_check_locked(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint8_t media_descriptor,
    olnk_media_check_resp_t *out_resp
) {
    olnk_status_t st;

    st = client_ensure_connected_locked(ecli);
    if (st != OLNK_OK) {
        return st;
    }

    st = olnk_client_media_check(&ecli->client, unit, media_descriptor, out_resp);
    if ((st != OLNK_OK) && ecli->cfg.auto_reconnect && client_status_is_recoverable(st)) {
        st = client_connect_session_locked(ecli, ecli->cfg.sync_retries, true);
        if (st == OLNK_OK) {
            st = olnk_client_media_check(&ecli->client, unit, media_descriptor, out_resp);
        }
    }
    if (st == OLNK_OK) {
        client_mark_activity_locked(ecli);
    }
    return st;
}

static olnk_status_t client_read_sectors_locked(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint8_t media_descriptor,
    uint32_t start_lba,
    uint16_t sector_count,
    uint16_t bytes_per_sector,
    uint8_t *out_buf,
    uint32_t out_cap,
    olnk_sector_io_resp_t *out_resp
) {
    olnk_status_t st;

    st = client_ensure_connected_locked(ecli);
    if (st != OLNK_OK) {
        return st;
    }

    st = olnk_client_read_sectors(
        &ecli->client,
        unit,
        media_descriptor,
        start_lba,
        sector_count,
        bytes_per_sector,
        out_buf,
        out_cap,
        out_resp
    );
    if ((st != OLNK_OK) && ecli->cfg.auto_reconnect && client_status_is_recoverable(st)) {
        st = client_connect_session_locked(ecli, ecli->cfg.sync_retries, true);
        if (st == OLNK_OK) {
            st = olnk_client_read_sectors(
                &ecli->client,
                unit,
                media_descriptor,
                start_lba,
                sector_count,
                bytes_per_sector,
                out_buf,
                out_cap,
                out_resp
            );
        }
    }
    if (st == OLNK_OK) {
        client_mark_activity_locked(ecli);
    }
    return st;
}

/* =========================================================================
 * Public Configuration, Init & Session Lifecycle API
 * ========================================================================= */

void olnk_esp32_client_cfg_default(olnk_esp32_client_cfg_t *cfg) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    cfg->local_requires_echo = true;
    cfg->force_tx_echo = false;
    cfg->sync_retries = OLNK_ESP32_CLIENT_DEFAULT_SYNC_RETRIES;
    cfg->rpc_timeout_ms = OLNK_CLIENT_DEFAULT_RESP_MS;
    cfg->max_client_drives = OLNK_ESP32_CLIENT_DEFAULT_MAX_DRIVES;
    cfg->first_client_drive = OLNK_ESP32_CLIENT_DEFAULT_FIRST_DRIVE;
    cfg->auto_reconnect = true;
    cfg->keepalive_interval_ms = OLNK_ESP32_CLIENT_DEFAULT_KEEPALIVE_MS;
    cfg->lock = NULL;
    cfg->unlock = NULL;
    cfg->lock_ctx = NULL;
    cfg->get_time_ms = NULL;
    cfg->time_ctx = NULL;
}

olnk_status_t olnk_esp32_client_init(
    olnk_esp32_client_t *ecli,
    olnk_transport_t transport,
    const olnk_esp32_client_cfg_t *cfg
) {
    olnk_esp32_client_cfg_t eff_cfg;
    olnk_status_t st;

    if (ecli == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

    olnk_esp32_client_cfg_default(&eff_cfg);
    if (cfg != NULL) {
        eff_cfg = *cfg;
    }
    if (eff_cfg.max_baud_idx >= OLNK_BAUD_INDEX_COUNT) {
        return OLNK_ERR_INVALID_ARG;
    }
    if ((eff_cfg.lock == NULL) != (eff_cfg.unlock == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if ((eff_cfg.first_client_drive >= OLNK_MAX_DRIVES) ||
        (eff_cfg.max_client_drives > OLNK_MAX_DRIVES)) {
        return OLNK_ERR_INVALID_ARG;
    }

    if (eff_cfg.sync_retries == 0U) {
        eff_cfg.sync_retries = 1U;
    }
    if (eff_cfg.rpc_timeout_ms == 0UL) {
        eff_cfg.rpc_timeout_ms = OLNK_CLIENT_DEFAULT_RESP_MS;
    }
    if (eff_cfg.max_client_drives == 0U) {
        eff_cfg.max_client_drives = OLNK_ESP32_CLIENT_DEFAULT_MAX_DRIVES;
    }

    memset(ecli, 0, sizeof(*ecli));

    st = olnk_client_init(&ecli->client, transport);
    if (st != OLNK_OK) {
        return st;
    }

    ecli->cfg = eff_cfg;
    ecli->client.link.max_baud_idx = eff_cfg.max_baud_idx;
    ecli->client.link.local_requires_echo = eff_cfg.local_requires_echo;
    ecli->client.rpc_timeout_ms = eff_cfg.rpc_timeout_ms;

    ecli->lock_fn = eff_cfg.lock;
    ecli->unlock_fn = eff_cfg.unlock;
    ecli->lock_ctx = eff_cfg.lock_ctx;

#ifdef ESP_PLATFORM
    if (ecli->lock_fn == NULL) {
        ecli->mutex_handle = xSemaphoreCreateMutexStatic(&ecli->mutex_storage);
        if (ecli->mutex_handle == NULL) {
            return OLNK_ERR_HAL_IO;
        }
    }
#endif

    ecli->initialized = true;
    return OLNK_OK;
}

olnk_status_t olnk_esp32_client_deinit(olnk_esp32_client_t *ecli) {
    if (ecli == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!ecli->initialized) {
        return OLNK_OK;
    }

    (void)olnk_esp32_client_stop_keepalive_task(ecli, OLNK_ESP32_CLIENT_DEINIT_STOP_MS);

    if (client_lock(ecli)) {
        olnk_client_disconnect(&ecli->client);
        ecli->initialized = false;
        client_unlock(ecli);
    } else {
        ecli->initialized = false;
    }

#ifdef ESP_PLATFORM
    if (ecli->mutex_handle != NULL) {
        vSemaphoreDelete(ecli->mutex_handle);
        ecli->mutex_handle = NULL;
    }
#endif
    return OLNK_OK;
}

olnk_status_t olnk_esp32_client_connect_session(olnk_esp32_client_t *ecli) {
    olnk_status_t st;

    if ((ecli == NULL) || (!ecli->initialized)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    st = client_connect_session_locked(ecli, ecli->cfg.sync_retries, false);

    client_unlock(ecli);
    return st;
}

olnk_status_t olnk_esp32_client_disconnect(olnk_esp32_client_t *ecli) {
    if ((ecli == NULL) || (!ecli->initialized)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    olnk_client_disconnect(&ecli->client);

    client_unlock(ecli);
    return OLNK_OK;
}

olnk_status_t olnk_esp32_client_select_default_unit(
    olnk_esp32_client_t *ecli,
    uint8_t *out_unit
) {
    size_t i;
    uint8_t first_mapped = 0xFFU;
    olnk_status_t st;

    if ((ecli == NULL) || (!ecli->initialized) || (out_unit == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    st = client_ensure_connected_locked(ecli);
    if (st != OLNK_OK) {
        client_unlock(ecli);
        return st;
    }

    if (!ecli->client.has_map) {
        client_unlock(ecli);
        return OLNK_ERR_PROTOCOL;
    }

    for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
        uint8_t entry = ecli->client.map.drive_map_resp[i];
        uint8_t u;
        olnk_build_bpb_resp_t bpb;

        if ((entry == OLNK_MAP_AUTO) || (entry == OLNK_MAP_NONE)) {
            continue;
        }
        u = (uint8_t)(entry & 0x7FU);
        if (u >= OLNK_MAX_DRIVES) {
            continue;
        }
        if (first_mapped == 0xFFU) {
            first_mapped = u;
        }

        memset(&bpb, 0, sizeof(bpb));
        if ((client_build_bpb_locked(ecli, u, 0xF8U, &bpb) == OLNK_OK) &&
            ((bpb.status & OLNK_DOS_STATUS_ERROR) == 0U) &&
            bpb.has_bpb) {
            *out_unit = u;
            client_unlock(ecli);
            return OLNK_OK;
        }
    }

    if (first_mapped != 0xFFU) {
        *out_unit = first_mapped;
        client_unlock(ecli);
        return OLNK_OK;
    }

    client_unlock(ecli);
    return OLNK_ERR_PROTOCOL;
}

bool olnk_esp32_client_unit_is_readonly(olnk_esp32_client_t *ecli, uint8_t unit) {
    size_t i;
    bool ro = false;

    if ((ecli == NULL) || (!ecli->initialized) || (unit >= OLNK_MAX_DRIVES)) {
        return false;
    }
    if (!client_lock(ecli)) {
        return false;
    }

    if (ecli->client.has_map) {
        for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
            uint8_t entry = ecli->client.map.drive_map_resp[i];
            if ((entry == OLNK_MAP_AUTO) || (entry == OLNK_MAP_NONE)) {
                continue;
            }
            if ((uint8_t)(entry & 0x7FU) == unit) {
                ro = ((entry & OLNK_MAP_READONLY_BIT) != 0U);
                break;
            }
        }
    }

    client_unlock(ecli);
    return ro;
}

uint16_t olnk_esp32_client_max_block_size(olnk_esp32_client_t *ecli) {
    uint16_t blk = (uint16_t)OLNK_MAX_PAYLOAD_SIZE;

    if ((ecli == NULL) || (!ecli->initialized)) {
        return (uint16_t)OLNK_MAX_PAYLOAD_SIZE;
    }
    if (!client_lock(ecli)) {
        return (uint16_t)OLNK_MAX_PAYLOAD_SIZE;
    }

    blk = ecli->client.link.max_block_size;
    if ((blk < OLNK_SECTOR_SIZE) || (blk > OLNK_MAX_PAYLOAD_SIZE)) {
        blk = (uint16_t)OLNK_MAX_PAYLOAD_SIZE;
    }

    client_unlock(ecli);
    return blk;
}

/* =========================================================================
 * Thread-Safe RPC Wrappers (with Auto-Reconnect Retry)
 * ========================================================================= */

olnk_status_t olnk_esp32_client_get_drive_info(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    olnk_get_drive_info_resp_t *out_resp
) {
    olnk_status_t st;

    if ((ecli == NULL) || (!ecli->initialized) || (out_resp == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    st = client_ensure_connected_locked(ecli);
    if (st == OLNK_OK) {
        st = olnk_client_get_drive_info(&ecli->client, unit, out_resp);
        if ((st != OLNK_OK) && ecli->cfg.auto_reconnect && client_status_is_recoverable(st)) {
            st = client_connect_session_locked(ecli, ecli->cfg.sync_retries, true);
            if (st == OLNK_OK) {
                st = olnk_client_get_drive_info(&ecli->client, unit, out_resp);
            }
        }
        if (st == OLNK_OK) {
            client_mark_activity_locked(ecli);
        }
    }

    client_unlock(ecli);
    return st;
}

olnk_status_t olnk_esp32_client_media_check(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint8_t media_descriptor,
    olnk_media_check_resp_t *out_resp
) {
    olnk_status_t st;

    if ((ecli == NULL) || (!ecli->initialized) || (out_resp == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    st = client_media_check_locked(ecli, unit, media_descriptor, out_resp);

    client_unlock(ecli);
    return st;
}

olnk_status_t olnk_esp32_client_build_bpb(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint8_t media_descriptor,
    olnk_build_bpb_resp_t *out_resp
) {
    olnk_status_t st;

    if ((ecli == NULL) || (!ecli->initialized) || (out_resp == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    st = client_build_bpb_locked(ecli, unit, media_descriptor, out_resp);

    client_unlock(ecli);
    return st;
}

olnk_status_t olnk_esp32_client_read_sectors(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint8_t media_descriptor,
    uint32_t start_lba,
    uint16_t sector_count,
    uint16_t bytes_per_sector,
    uint8_t *out_buf,
    uint32_t out_cap,
    olnk_sector_io_resp_t *out_resp
) {
    olnk_status_t st;

    if ((ecli == NULL) || (!ecli->initialized) || (out_resp == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if ((sector_count > 0U) && (out_buf == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    st = client_read_sectors_locked(
        ecli,
        unit,
        media_descriptor,
        start_lba,
        sector_count,
        bytes_per_sector,
        out_buf,
        out_cap,
        out_resp
    );

    client_unlock(ecli);
    return st;
}

olnk_status_t olnk_esp32_client_write_sectors(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint8_t media_descriptor,
    uint32_t start_lba,
    uint16_t sector_count,
    uint16_t bytes_per_sector,
    const uint8_t *in_buf,
    bool verify,
    olnk_sector_io_resp_t *out_resp
) {
    olnk_status_t st;

    if ((ecli == NULL) || (!ecli->initialized) || (out_resp == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if ((sector_count > 0U) && (in_buf == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    st = client_ensure_connected_locked(ecli);
    if (st == OLNK_OK) {
        st = olnk_client_write_sectors(
            &ecli->client,
            unit,
            media_descriptor,
            start_lba,
            sector_count,
            bytes_per_sector,
            in_buf,
            verify,
            out_resp
        );
        if ((st != OLNK_OK) && ecli->cfg.auto_reconnect && client_status_is_recoverable(st)) {
            st = client_connect_session_locked(ecli, ecli->cfg.sync_retries, true);
            if (st == OLNK_OK) {
                st = olnk_client_write_sectors(
                    &ecli->client,
                    unit,
                    media_descriptor,
                    start_lba,
                    sector_count,
                    bytes_per_sector,
                    in_buf,
                    verify,
                    out_resp
                );
            }
        }
        if (st == OLNK_OK) {
            client_mark_activity_locked(ecli);
        }
    }

    client_unlock(ecli);
    return st;
}

olnk_status_t olnk_esp32_client_get_media_id(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint16_t *out_status,
    olnk_generic_ioctl_mid_t *out_mid
) {
    olnk_status_t st;

    if ((ecli == NULL) || (!ecli->initialized) || (out_status == NULL) || (out_mid == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    st = client_ensure_connected_locked(ecli);
    if (st == OLNK_OK) {
        st = olnk_client_get_media_id(&ecli->client, unit, out_status, out_mid);
        if ((st != OLNK_OK) && ecli->cfg.auto_reconnect && client_status_is_recoverable(st)) {
            st = client_connect_session_locked(ecli, ecli->cfg.sync_retries, true);
            if (st == OLNK_OK) {
                st = olnk_client_get_media_id(&ecli->client, unit, out_status, out_mid);
            }
        }
        if (st == OLNK_OK) {
            client_mark_activity_locked(ecli);
        }
    }

    client_unlock(ecli);
    return st;
}

olnk_status_t olnk_esp32_client_set_media_id(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    const olnk_generic_ioctl_mid_t *mid,
    uint16_t *out_status
) {
    olnk_status_t st;

    if ((ecli == NULL) || (!ecli->initialized) || (mid == NULL) || (out_status == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    st = client_ensure_connected_locked(ecli);
    if (st == OLNK_OK) {
        st = olnk_client_set_media_id(&ecli->client, unit, mid, out_status);
        if ((st != OLNK_OK) && ecli->cfg.auto_reconnect && client_status_is_recoverable(st)) {
            st = client_connect_session_locked(ecli, ecli->cfg.sync_retries, true);
            if (st == OLNK_OK) {
                st = olnk_client_set_media_id(&ecli->client, unit, mid, out_status);
            }
        }
        if (st == OLNK_OK) {
            client_mark_activity_locked(ecli);
        }
    }

    client_unlock(ecli);
    return st;
}

olnk_status_t olnk_esp32_client_keepalive(olnk_esp32_client_t *ecli) {
    olnk_status_t st;

    if ((ecli == NULL) || (!ecli->initialized)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    if (ecli->client.link.link_state != (uint8_t)OLNK_LINK_CONNECTED) {
        if (ecli->cfg.auto_reconnect) {
            st = client_connect_session_locked(ecli, ecli->cfg.sync_retries, ecli->has_sync);
        } else {
            st = OLNK_ERR_DISCONNECTED;
        }
        client_unlock(ecli);
        return st;
    }

    st = olnk_client_keepalive(&ecli->client);
    if (st == OLNK_OK) {
        client_mark_activity_locked(ecli);
    } else {
        ecli->keepalive_fail_count += 1UL;
        if ((st == OLNK_ERR_DISCONNECTED) && ecli->cfg.auto_reconnect) {
            st = client_connect_session_locked(ecli, ecli->cfg.sync_retries, true);
        }
    }

    client_unlock(ecli);
    return st;
}

olnk_status_t olnk_esp32_client_keepalive_step(olnk_esp32_client_t *ecli) {
    uint64_t now_ms = 0ULL;
    bool have_clock;
    olnk_status_t st = OLNK_OK;

    if ((ecli == NULL) || (!ecli->initialized)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (ecli->cfg.keepalive_interval_ms == 0UL) {
        return OLNK_OK;
    }

    /* Check elapsed idle time before taking mutex when possible, then re-verify
     * under lock so active VFS I/O is never needlessly contended. */
    have_clock = client_read_time_ms(ecli, &now_ms);
    if (have_clock && ecli->has_last_activity &&
        (now_ms >= ecli->last_activity_ms) &&
        ((now_ms - ecli->last_activity_ms) < (uint64_t)ecli->cfg.keepalive_interval_ms)) {
        return OLNK_OK;
    }

    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    have_clock = client_read_time_ms(ecli, &now_ms);
    if (have_clock && ecli->has_last_activity &&
        (now_ms >= ecli->last_activity_ms) &&
        ((now_ms - ecli->last_activity_ms) < (uint64_t)ecli->cfg.keepalive_interval_ms)) {
        client_unlock(ecli);
        return OLNK_OK;
    }

    if (ecli->client.link.link_state == (uint8_t)OLNK_LINK_CONNECTED) {
        st = olnk_client_keepalive(&ecli->client);
        client_mark_activity_locked(ecli);
        if (st != OLNK_OK) {
            ecli->keepalive_fail_count += 1UL;
            if ((st == OLNK_ERR_DISCONNECTED) && ecli->cfg.auto_reconnect) {
                /* Single-probe reconnect so keepalive task never blocks mutex for seconds */
                st = client_connect_session_locked(ecli, 1U, true);
            }
        }
    } else if (ecli->has_sync && ecli->cfg.auto_reconnect) {
        client_mark_activity_locked(ecli);
        st = client_connect_session_locked(ecli, 1U, true);
    }

    client_unlock(ecli);
    return st;
}

/* =========================================================================
 * Streaming Raw Drive Backup / Dump Helper (Zero-Alloc)
 * ========================================================================= */

olnk_status_t olnk_esp32_client_dump_sectors(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint32_t start_lba,
    uint32_t sector_count,
    olnk_esp32_dump_write_fn_t write_cb,
    void *write_ctx,
    uint32_t *out_sectors_dumped
) {
    olnk_media_check_resp_t mcresp;
    olnk_build_bpb_resp_t bpbresp;
    uint16_t bps;
    uint8_t media_desc;
    uint32_t total_sectors;
    uint32_t target_sectors;
    uint32_t dumped = 0UL;
    uint16_t max_blk;
    uint16_t max_chunk_secs;
    olnk_status_t st;

    if (out_sectors_dumped != NULL) {
        *out_sectors_dumped = 0UL;
    }
    if ((ecli == NULL) || (!ecli->initialized) || (write_cb == NULL) || (unit >= OLNK_MAX_DRIVES)) {
        return OLNK_ERR_INVALID_ARG;
    }

    if (!client_lock(ecli)) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    memset(&mcresp, 0, sizeof(mcresp));
    memset(&bpbresp, 0, sizeof(bpbresp));
    (void)client_media_check_locked(ecli, unit, 0xF8U, &mcresp);
    st = client_build_bpb_locked(ecli, unit, 0xF8U, &bpbresp);
    if (st != OLNK_OK) {
        client_unlock(ecli);
        return st;
    }
    if (((bpbresp.status & OLNK_DOS_STATUS_ERROR) != 0U) || (!bpbresp.has_bpb)) {
        client_unlock(ecli);
        return OLNK_ERR_HAL_IO;
    }

    bps = bpbresp.bpb.bytes_per_sector;
    total_sectors = (bpbresp.bpb.total_sectors_16 != 0U)
                        ? (uint32_t)bpbresp.bpb.total_sectors_16
                        : bpbresp.bpb.total_sectors_32;
    media_desc = (bpbresp.bpb.media_descriptor != 0U) ? bpbresp.bpb.media_descriptor : 0xF8U;

    if ((bps == 0U) || (bps > OLNK_MAX_PAYLOAD_SIZE) || (total_sectors == 0UL)) {
        client_unlock(ecli);
        return OLNK_ERR_PROTOCOL;
    }
    if (start_lba >= total_sectors) {
        client_unlock(ecli);
        return OLNK_ERR_INVALID_ARG;
    }

    target_sectors = (sector_count == 0UL) ? (total_sectors - start_lba) : sector_count;
    if (target_sectors > (total_sectors - start_lba)) {
        client_unlock(ecli);
        return OLNK_ERR_INVALID_ARG;
    }

    max_blk = ecli->client.link.max_block_size;
    if ((max_blk < bps) || (max_blk > OLNK_MAX_PAYLOAD_SIZE)) {
        max_blk = (uint16_t)OLNK_MAX_PAYLOAD_SIZE;
    }
    max_chunk_secs = (uint16_t)(max_blk / bps);
    if (max_chunk_secs == 0U) {
        max_chunk_secs = 1U;
    }

    client_unlock(ecli);

    while (dumped < target_sectors) {
        uint32_t rem = target_sectors - dumped;
        uint16_t chunk = (rem > (uint32_t)max_chunk_secs) ? max_chunk_secs : (uint16_t)rem;
        olnk_sector_io_resp_t ior;
        uint32_t byte_len;
        bool cont;

        if (!client_lock(ecli)) {
            if (out_sectors_dumped != NULL) {
                *out_sectors_dumped = dumped;
            }
            return OLNK_ERR_HAL_TIMEOUT;
        }

        /* Reuse ecli->client.link.rx_buffer (8197 B, unused by client RPCs when
         * out_buf != NULL) while holding the client mutex — zero malloc, zero stack. */
        memset(&ior, 0, sizeof(ior));
        st = client_read_sectors_locked(
            ecli,
            unit,
            media_desc,
            start_lba + dumped,
            chunk,
            bps,
            ecli->client.link.rx_buffer,
            (uint32_t)OLNK_MAX_PAYLOAD_SIZE,
            &ior
        );
        if (st != OLNK_OK) {
            client_unlock(ecli);
            if (out_sectors_dumped != NULL) {
                *out_sectors_dumped = dumped;
            }
            return st;
        }
        if (((ior.status & OLNK_DOS_STATUS_ERROR) != 0U) || (ior.sectors_transferred == 0U)) {
            client_unlock(ecli);
            if (out_sectors_dumped != NULL) {
                *out_sectors_dumped = dumped;
            }
            return OLNK_ERR_HAL_IO;
        }

        byte_len = (uint32_t)ior.sectors_transferred * (uint32_t)bps;
        cont = write_cb(start_lba + dumped, ecli->client.link.rx_buffer, byte_len, write_ctx);
        dumped += (uint32_t)ior.sectors_transferred;

        client_unlock(ecli);

        if (!cont) {
            if (out_sectors_dumped != NULL) {
                *out_sectors_dumped = dumped;
            }
            return OLNK_ERR_HAL_IO;
        }
    }

    if (out_sectors_dumped != NULL) {
        *out_sectors_dumped = dumped;
    }
    return OLNK_OK;
}

/* =========================================================================
 * Optional Background Keepalive FreeRTOS Task (ESP-IDF)
 * ========================================================================= */

#ifdef ESP_PLATFORM
static void esp32_client_keepalive_task_entry(void *arg) {
    olnk_esp32_client_t *ecli = (olnk_esp32_client_t *)arg;
    uint32_t sleep_ms;

    ecli->keepalive_running = true;

    while (!ecli->keepalive_stop_requested) {
        sleep_ms = ecli->cfg.keepalive_interval_ms;
        if ((sleep_ms == 0UL) || (sleep_ms > OLNK_ESP32_CLIENT_KA_POLL_SLICE_MS)) {
            sleep_ms = OLNK_ESP32_CLIENT_KA_POLL_SLICE_MS;
        }
        vTaskDelay(olnk_esp32_ms_to_ticks(sleep_ms, (uint32_t)portTICK_PERIOD_MS));
        if (ecli->keepalive_stop_requested) {
            break;
        }
        (void)olnk_esp32_client_keepalive_step(ecli);
    }

    ecli->keepalive_running = false;
    ecli->keepalive_task_handle = NULL;
    vTaskDelete(NULL);
}
#endif

olnk_status_t olnk_esp32_client_start_keepalive_task(
    olnk_esp32_client_t *ecli,
    const char *task_name,
    uint32_t stack_bytes,
    uint32_t priority,
    int core_id
) {
#ifdef ESP_PLATFORM
    const char *name;
    uint32_t stack_sz;
    BaseType_t core;
    BaseType_t rc;

    if ((ecli == NULL) || (!ecli->initialized)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (ecli->cfg.keepalive_interval_ms == 0UL) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (ecli->keepalive_running || (ecli->keepalive_task_handle != NULL)) {
        return OLNK_ERR_PROTOCOL;
    }

    name = (task_name != NULL) ? task_name : "openlnk_ka";
    stack_sz = (stack_bytes != 0UL)
                   ? stack_bytes
                   : OLNK_ESP32_CLIENT_DEFAULT_TASK_STACK_BYTES;
    core = (core_id < 0) ? tskNO_AFFINITY : (BaseType_t)core_id;

    ecli->keepalive_stop_requested = false;
    rc = xTaskCreatePinnedToCore(
        esp32_client_keepalive_task_entry,
        name,
        stack_sz,
        ecli,
        (UBaseType_t)priority,
        &ecli->keepalive_task_handle,
        core
    );
    if (rc != pdPASS) {
        ecli->keepalive_task_handle = NULL;
        return OLNK_ERR_HAL_IO;
    }
    return OLNK_OK;
#else
    (void)ecli;
    (void)task_name;
    (void)stack_bytes;
    (void)priority;
    (void)core_id;
    return OLNK_ERR_PROTOCOL;
#endif
}

olnk_status_t olnk_esp32_client_stop_keepalive_task(
    olnk_esp32_client_t *ecli,
    uint32_t wait_timeout_ms
) {
    if (ecli == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    ecli->keepalive_stop_requested = true;

#ifdef ESP_PLATFORM
    if (wait_timeout_ms > 0UL) {
        uint64_t deadline = client_idf_monotonic_ms(NULL) + (uint64_t)wait_timeout_ms;
        while ((ecli->keepalive_running || (ecli->keepalive_task_handle != NULL)) &&
               (client_idf_monotonic_ms(NULL) < deadline)) {
            vTaskDelay(1);
        }
        if (ecli->keepalive_running || (ecli->keepalive_task_handle != NULL)) {
            return OLNK_ERR_HAL_TIMEOUT;
        }
    }
#else
    (void)wait_timeout_ms;
#endif
    return OLNK_OK;
}
