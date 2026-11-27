/* esp32/src/esp32_server.c
 *
 * Caller-owned high-level ESP32 server wrapper and optional FreeRTOS task
 * runner around olnk_server_t.
 *
 * Uses real monotonic elapsed milliseconds (esp_timer_get_time() / 1000 on
 * ESP-IDF, or injectable clock callback on host tests) so olnk_server_tick()
 * timing remains accurate even when uart_read_bytes() returns early.
 *
 * Strictly ISO C99, zero dynamic allocation (malloc/free), zero floating-point.
 */
#include "openlnk_esp32.h"

#include <limits.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define OLNK_ESP32_SERVER_RX_CHUNK 64U

/* =========================================================================
 * Monotonic Millisecond Clock Abstraction
 * ========================================================================= */

#ifdef ESP_PLATFORM
static uint64_t idf_monotonic_ms(void *ctx) {
    int64_t us = esp_timer_get_time();
    (void)ctx;
    return (us > 0LL) ? (uint64_t)(us / 1000LL) : 0ULL;
}
#endif

static bool server_read_time_ms(const olnk_esp32_server_t *esrv, uint64_t *out_ms) {
    if (esrv->get_time_ms != NULL) {
        *out_ms = esrv->get_time_ms(esrv->time_ctx);
        return true;
    }
#ifdef ESP_PLATFORM
    *out_ms = idf_monotonic_ms(NULL);
    return true;
#else
    (void)out_ms;
    return false;
#endif
}

/* =========================================================================
 * Public High-Level Server API
 * ========================================================================= */

void olnk_esp32_server_cfg_default(olnk_esp32_server_cfg_t *cfg) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    cfg->local_requires_echo = true;
    cfg->idle_disconnect_ms = OLNK_ESP32_SERVER_DEFAULT_IDLE_DISCONNECT_MS;
    cfg->poll_timeout_ms = OLNK_ESP32_SERVER_DEFAULT_POLL_TIMEOUT_MS;
    cfg->on_event = NULL;
    cfg->on_event_user_data = NULL;
    cfg->get_time_ms = NULL;
    cfg->time_ctx = NULL;
}

olnk_status_t olnk_esp32_server_init(
    olnk_esp32_server_t *esrv,
    olnk_transport_t transport,
    olnk_storage_ops_t storage,
    const olnk_esp32_server_cfg_t *cfg
) {
    olnk_esp32_server_cfg_t eff_cfg;
    olnk_status_t st;
    uint64_t now_ms = 0ULL;

    if (esrv == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

    olnk_esp32_server_cfg_default(&eff_cfg);
    if (cfg != NULL) {
        eff_cfg = *cfg;
    }
    if (eff_cfg.max_baud_idx >= OLNK_BAUD_INDEX_COUNT) {
        return OLNK_ERR_INVALID_ARG;
    }

    memset(esrv, 0, sizeof(*esrv));

    st = olnk_server_init(&esrv->server, transport, storage);
    if (st != OLNK_OK) {
        return st;
    }

    esrv->server.link.max_baud_idx = eff_cfg.max_baud_idx;
    esrv->server.link.local_requires_echo = eff_cfg.local_requires_echo;
    esrv->server.idle_disconnect_ms = eff_cfg.idle_disconnect_ms;
    olnk_server_set_event_callback(
        &esrv->server,
        eff_cfg.on_event,
        eff_cfg.on_event_user_data
    );

    esrv->poll_timeout_ms = (eff_cfg.poll_timeout_ms != 0UL)
                                ? eff_cfg.poll_timeout_ms
                                : OLNK_ESP32_SERVER_DEFAULT_POLL_TIMEOUT_MS;
    esrv->get_time_ms = eff_cfg.get_time_ms;
    esrv->time_ctx = eff_cfg.time_ctx;
    esrv->stop_requested = false;
    esrv->running = false;
    esrv->task_handle = NULL;

    if (server_read_time_ms(esrv, &now_ms)) {
        esrv->last_tick_ms = now_ms;
        esrv->has_last_tick = true;
    }
    return OLNK_OK;
}

olnk_status_t olnk_esp32_server_step(
    olnk_esp32_server_t *esrv,
    uint32_t poll_timeout_ms
) {
    uint8_t rx_buf[OLNK_ESP32_SERVER_RX_CHUNK];
    uint64_t t_before = 0ULL;
    uint64_t t_after = 0ULL;
    bool have_clock;
    int n;

    if (esrv == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (esrv->server.link.transport.read == NULL) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    have_clock = server_read_time_ms(esrv, &t_before);
    if (have_clock && (!esrv->has_last_tick)) {
        esrv->last_tick_ms = t_before;
        esrv->has_last_tick = true;
    }

    n = esrv->server.link.transport.read(
        rx_buf,
        sizeof(rx_buf),
        poll_timeout_ms,
        esrv->server.link.transport.user_data
    );
    if (n < 0) {
        return OLNK_ERR_HAL_IO;
    }

    if (have_clock) {
        (void)server_read_time_ms(esrv, &t_after);
    }

    if (n > 0) {
        olnk_server_feed_bytes(&esrv->server, rx_buf, (size_t)n);
        if (have_clock) {
            esrv->last_tick_ms = t_after;
        }
        return OLNK_OK;
    }

    /* No bytes arrived: advance protocol timers by real monotonic elapsed ms */
    if (have_clock) {
        uint32_t elapsed_ms = 0UL;
        if (t_after >= esrv->last_tick_ms) {
            uint64_t diff = t_after - esrv->last_tick_ms;
            elapsed_ms = (diff > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)diff;
        }
        esrv->last_tick_ms = t_after;
        if (elapsed_ms > 0UL) {
            olnk_server_tick(&esrv->server, elapsed_ms);
        }
    } else if (poll_timeout_ms > 0UL) {
        olnk_server_tick(&esrv->server, poll_timeout_ms);
    }

    return OLNK_OK;
}

olnk_status_t olnk_esp32_server_run(olnk_esp32_server_t *esrv) {
    olnk_status_t st = OLNK_OK;

    if (esrv == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

    esrv->stop_requested = false;
    esrv->running = true;

    while (!esrv->stop_requested) {
        st = olnk_esp32_server_step(esrv, esrv->poll_timeout_ms);
        if (st == OLNK_ERR_HAL_IO) {
            break;
        }
    }

    esrv->running = false;
    return st;
}

#ifdef ESP_PLATFORM
static void esp32_server_task_entry(void *arg) {
    olnk_esp32_server_t *esrv = (olnk_esp32_server_t *)arg;
    (void)olnk_esp32_server_run(esrv);
    esrv->task_handle = NULL;
    vTaskDelete(NULL);
}
#endif

olnk_status_t olnk_esp32_server_start_task(
    olnk_esp32_server_t *esrv,
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

    if (esrv == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (esrv->running || (esrv->task_handle != NULL)) {
        return OLNK_ERR_PROTOCOL;
    }

    name = (task_name != NULL) ? task_name : "openlnk_srv";
    stack_sz = (stack_bytes != 0UL)
                   ? stack_bytes
                   : OLNK_ESP32_SERVER_DEFAULT_TASK_STACK_BYTES;
    core = (core_id < 0) ? tskNO_AFFINITY : (BaseType_t)core_id;

    esrv->stop_requested = false;
    rc = xTaskCreatePinnedToCore(
        esp32_server_task_entry,
        name,
        stack_sz,
        esrv,
        (UBaseType_t)priority,
        &esrv->task_handle,
        core
    );
    if (rc != pdPASS) {
        esrv->task_handle = NULL;
        return OLNK_ERR_HAL_IO;
    }
    return OLNK_OK;
#else
    (void)esrv;
    (void)task_name;
    (void)stack_bytes;
    (void)priority;
    (void)core_id;
    return OLNK_ERR_PROTOCOL;
#endif
}

olnk_status_t olnk_esp32_server_stop(
    olnk_esp32_server_t *esrv,
    uint32_t wait_timeout_ms
) {
    if (esrv == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    esrv->stop_requested = true;

#ifdef ESP_PLATFORM
    if (wait_timeout_ms > 0UL) {
        uint64_t deadline = idf_monotonic_ms(NULL) + (uint64_t)wait_timeout_ms;
        while ((esrv->running || (esrv->task_handle != NULL)) &&
               (idf_monotonic_ms(NULL) < deadline)) {
            vTaskDelay(1);
        }
        if (esrv->running || (esrv->task_handle != NULL)) {
            return OLNK_ERR_HAL_TIMEOUT;
        }
    }
#else
    (void)wait_timeout_ms;
#endif
    return OLNK_OK;
}
