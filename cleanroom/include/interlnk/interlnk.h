/* cleanroom/include/interlnk/interlnk.h */
#ifndef INTERLNK_H
#define INTERLNK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interlnk/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ILNK_STATE_SEARCH_SYNC = 0U,
    ILNK_STATE_READ_HEADER = 1U,
    ILNK_STATE_READ_PAYLOAD = 2U,
    ILNK_STATE_EXEC_CMD = 3U
} ilnk_state_t;

typedef struct ilnk_transport {
    /* Return >0 bytes read, 0 on timeout/no-data, <0 on HAL error. */
    int (*read)(uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data);
    /* Return >0 bytes written, 0 on timeout, <0 on HAL error. */
    int (*write)(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data);
    bool (*set_baud)(uint32_t baudrate, void *user_data);
    void *user_data;
} ilnk_transport_t;

typedef void (*ilnk_frame_handler_t)(const ilnk_frame_t *frame, void *user_data);

typedef struct {
    uint8_t rx_buffer[ILNK_RX_BUFFER_SIZE];
    size_t rx_len;
    uint8_t state;
    uint8_t seq_num;
    ilnk_transport_t transport;
    uint16_t expected_frame_len;
    ilnk_frame_handler_t on_frame;
    void *on_frame_user_data;
    uint32_t frames_rx_ok;
    uint32_t frames_rx_err;
} ilnk_ctx_t;

/**
 * Initialize a caller-allocated libinterlnk state machine context.
 * Performs zero dynamic memory allocation.
 */
ilnk_status_t ilnk_init(ilnk_ctx_t *ctx, ilnk_transport_t transport);

/**
 * Reset receiver buffer and return parser state machine to SEARCH_SYNC.
 */
void ilnk_reset(ilnk_ctx_t *ctx);

/**
 * Register a callback invoked whenever a complete, checksum-valid frame arrives.
 */
void ilnk_set_frame_handler(
    ilnk_ctx_t *ctx,
    ilnk_frame_handler_t handler,
    void *user_data
);

/**
 * Poll the attached transport HAL once and feed received bytes into the parser.
 */
ilnk_status_t ilnk_poll(ilnk_ctx_t *ctx);

/**
 * Feed raw wire bytes into the streaming frame state machine.
 * Stops as soon as one complete frame is decoded (returning ILNK_OK) or on error,
 * setting *bytes_processed to the exact number of input bytes consumed so the
 * caller can resume on the remaining slice.
 */
ilnk_status_t ilnk_feed_bytes(
    ilnk_ctx_t *ctx,
    const uint8_t *data,
    size_t len,
    size_t *bytes_processed
);

/**
 * Encode and transmit a frame using the configured transport HAL.
 * Uses a caller-supplied scratch buffer (zero dynamic allocation).
 */
ilnk_status_t ilnk_send_frame(
    ilnk_ctx_t *ctx,
    const ilnk_frame_t *frame,
    uint8_t *tx_scratch,
    size_t tx_scratch_cap,
    uint32_t timeout_ms
);

#ifdef __cplusplus
}
#endif

#endif /* INTERLNK_H */
