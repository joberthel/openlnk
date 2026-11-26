/* cleanroom/include/openlnk/openlnk.h */
#ifndef OPENLNK_H
#define OPENLNK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openlnk/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OLNK_STATE_SEARCH_SYNC = 0U,
    OLNK_STATE_READ_HEADER = 1U,
    OLNK_STATE_READ_PAYLOAD = 2U,
    OLNK_STATE_EXEC_CMD = 3U
} olnk_state_t;

typedef struct olnk_transport {
    /* Return >0 bytes read, 0 on timeout/no-data, <0 on HAL error. */
    int (*read)(uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data);
    /* Return >0 bytes written, 0 on timeout, <0 on HAL error. */
    int (*write)(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data);
    bool (*set_baud)(uint32_t baudrate, void *user_data);
    void *user_data;
} olnk_transport_t;

typedef void (*olnk_frame_handler_t)(const olnk_frame_t *frame, void *user_data);

typedef struct {
    uint8_t rx_buffer[OLNK_RX_BUFFER_SIZE];
    size_t rx_len;
    uint8_t state;
    uint8_t seq_num;
    olnk_transport_t transport;
    uint16_t expected_frame_len;
    olnk_frame_handler_t on_frame;
    void *on_frame_user_data;
    uint32_t frames_rx_ok;
    uint32_t frames_rx_err;
} olnk_ctx_t;

/**
 * Initialize a caller-allocated libopenlnk state machine context.
 * Performs zero dynamic memory allocation.
 */
olnk_status_t olnk_init(olnk_ctx_t *ctx, olnk_transport_t transport);

/**
 * Reset receiver buffer and return parser state machine to SEARCH_SYNC.
 */
void olnk_reset(olnk_ctx_t *ctx);

/**
 * Register a callback invoked whenever a complete, checksum-valid frame arrives.
 */
void olnk_set_frame_handler(
    olnk_ctx_t *ctx,
    olnk_frame_handler_t handler,
    void *user_data
);

/**
 * Poll the attached transport HAL once and feed received bytes into the parser.
 */
olnk_status_t olnk_poll(olnk_ctx_t *ctx);

/**
 * Feed raw wire bytes into the streaming frame state machine.
 * Stops as soon as one complete frame is decoded (returning OLNK_OK) or on error,
 * setting *bytes_processed to the exact number of input bytes consumed so the
 * caller can resume on the remaining slice.
 */
olnk_status_t olnk_feed_bytes(
    olnk_ctx_t *ctx,
    const uint8_t *data,
    size_t len,
    size_t *bytes_processed
);

/**
 * Encode and transmit a frame using the configured transport HAL.
 * Uses a caller-supplied scratch buffer (zero dynamic allocation).
 */
olnk_status_t olnk_send_frame(
    olnk_ctx_t *ctx,
    const olnk_frame_t *frame,
    uint8_t *tx_scratch,
    size_t tx_scratch_cap,
    uint32_t timeout_ms
);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_H */
