/* cleanroom/include/openlnk/openlnk.h
 *
 * Clean-room C99 link-layer state machine & streaming wire parser for
 * libopenlnk (MS-DOS 6.22 INTERLNK / INTERSVR serial protocol).
 *
 * Strictly ISO C99, zero dynamic memory allocation, zero floating-point,
 * 16-bit Real-Mode safe (keep olnk_ctx_t static or heap-free caller-owned).
 */
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
    OLNK_PARSE_HEADER    = 0U,
    OLNK_PARSE_LEN_SHORT = 1U,
    OLNK_PARSE_LEN_LO    = 2U,
    OLNK_PARSE_LEN_HI    = 3U,
    OLNK_PARSE_BODY_CRC  = 4U
} olnk_parse_state_t;

typedef enum {
    OLNK_LINK_DISCONNECTED = 0U,
    OLNK_LINK_SYNCING      = 1U,
    OLNK_LINK_CONNECTED    = 2U
} olnk_link_state_t;

typedef struct olnk_transport {
    /* Return >0 bytes read, 0 on timeout/no-data, <0 on fatal HAL I/O error. */
    int (*read)(uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data);
    /* Return >0 bytes written, 0 on timeout, <0 on fatal HAL I/O error. */
    int (*write)(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data);
    /* Reconfigure serial UART baud rate (1200..115200 bps). Optional (may be NULL). */
    bool (*set_baud)(uint32_t baudrate, void *user_data);
    void *user_data;
} olnk_transport_t;

typedef void (*olnk_frame_handler_t)(const olnk_wire_frame_t *frame, void *user_data);

typedef struct {
    /* Static wire frame buffer (8192 + 5 = 8197 B by default) */
    uint8_t rx_buffer[OLNK_FRAME_BUFFER_SIZE];
    size_t rx_len;
    uint16_t expected_frame_len;
    uint8_t parse_state;

    /* Link-layer & session state */
    olnk_transport_t transport;
    uint8_t link_state;
    uint8_t seq;
    uint8_t current_baud_idx;
    uint8_t max_baud_idx;
    uint16_t max_block_size;
    bool local_requires_echo;
    bool peer_echo_forced;
    uint8_t keepalive_missed;

    /* Session IDs for transparent resumption after disconnect */
    uint32_t client_id;
    uint32_t server_id;
    uint32_t prev_client_id;
    bool session_resumed;

    /* Configurable timeouts (ms) */
    uint32_t sync_timeout_ms;
    uint32_t rx_byte_timeout_ms;
    uint32_t tx_timeout_ms;

    /* Streaming parser callback */
    olnk_frame_handler_t on_frame;
    void *on_frame_user_data;

    /* Telemetry counters */
    uint32_t frames_rx_ok;
    uint32_t frames_rx_err;
    uint32_t frames_tx_ok;
    uint32_t frames_tx_retry;
    uint32_t keepalives_ok;
    uint32_t baud_switches;
} olnk_ctx_t;

/* =========================================================================
 * Initialization & Streaming Canonical Frame Parser
 * ========================================================================= */

/**
 * Initialize a caller-allocated libopenlnk context (zero dynamic allocation).
 */
olnk_status_t olnk_init(olnk_ctx_t *ctx, olnk_transport_t transport);

/**
 * Reset streaming canonical frame parser buffer and state.
 */
void olnk_reset(olnk_ctx_t *ctx);

/**
 * Register callback invoked by olnk_feed_bytes / olnk_poll on valid frame.
 */
void olnk_set_frame_handler(
    olnk_ctx_t *ctx,
    olnk_frame_handler_t handler,
    void *user_data
);

/**
 * Feed raw canonical wire-frame bytes into the streaming parser.
 * Stops as soon as one complete frame is decoded (OLNK_OK) or on frame error,
 * reporting exact consumed bytes via *bytes_processed.
 */
olnk_status_t olnk_feed_bytes(
    olnk_ctx_t *ctx,
    const uint8_t *data,
    size_t len,
    size_t *bytes_processed
);

/**
 * Poll transport HAL once and feed received bytes into the streaming parser.
 */
olnk_status_t olnk_poll(olnk_ctx_t *ctx);

/* =========================================================================
 * Active Half-Duplex Link-Layer Protocol Engine (Phases A, B, C)
 * ========================================================================= */

/**
 * Drop link to OLNK_LINK_DISCONNECTED at 9600 bps (preserves session IDs).
 */
void olnk_link_disconnect(olnk_ctx_t *ctx);

/**
 * Phase A (9600 bps 3-step sync) + Phase B (stepwise baud step-up to max_baud_idx)
 * from the client (initiator) side.
 */
olnk_status_t olnk_link_connect_client(olnk_ctx_t *ctx);

/**
 * Phase B single-step baud negotiation as initiator (current -> target_baud_idx),
 * including beacon lock-on and 15-pair line quality test. Reverts on failure.
 */
olnk_status_t olnk_link_negotiate_baud_initiator(
    olnk_ctx_t *ctx,
    uint8_t target_baud_idx
);

/**
 * Phase B single-step baud negotiation as responder (called after receiving ~seq).
 * Reverts to previous baud rate on timeout or line quality test failure.
 */
olnk_status_t olnk_link_handle_baud_responder(olnk_ctx_t *ctx);

/**
 * Phase C single-frame transmit with header ACK pacing negotiation,
 * Echo-Paced or Burst mode, final_ack check, and adaptive retry / baud step-down.
 */
olnk_status_t olnk_link_send_frame(
    olnk_ctx_t *ctx,
    const uint8_t *payload,
    uint16_t payload_len,
    bool more_fragments
);

/**
 * Transmit an arbitrarily sized logical message by segmenting into <= max_block_size
 * frames using header bit 6 (more_fragments).
 */
olnk_status_t olnk_link_send_message(
    olnk_ctx_t *ctx,
    const uint8_t *data,
    uint32_t total_len
);

/**
 * Phase C single-frame receive with header ACK, Echo or Burst pacing (crc_lo not echoed),
 * CRC-16/GENIBUS residue (0x1D0F) validation, final_ack == hdr, and silent drain on error.
 * If out_payload is NULL, payload is stored in ctx->rx_buffer[0..*out_len-1].
 */
olnk_status_t olnk_link_recv_frame(
    olnk_ctx_t *ctx,
    uint8_t *out_payload,
    size_t out_cap,
    uint16_t *out_len,
    bool *out_more_fragments,
    uint32_t first_byte_timeout_ms
);

/**
 * Receive and reassemble a potentially multi-fragment logical message into out_buf,
 * tolerating peer retransmissions on transient CRC errors.
 */
olnk_status_t olnk_link_recv_message(
    olnk_ctx_t *ctx,
    uint8_t *out_buf,
    uint32_t out_cap,
    uint32_t *out_len,
    uint32_t first_byte_timeout_ms
);

/**
 * Send 1-byte half-duplex turnaround token (next_seq = ctx->seq & 0x03).
 */
olnk_status_t olnk_link_send_turnaround(olnk_ctx_t *ctx);

/**
 * Wait for 1-byte half-duplex turnaround token matching ctx->seq.
 */
olnk_status_t olnk_link_wait_turnaround(olnk_ctx_t *ctx, uint32_t timeout_ms);

/**
 * Send one idle keepalive poll (0xFC | (seq ^ 1)) and verify exact echo.
 * Drops link after > OLNK_KEEPALIVE_MAX_MISSED consecutive misses.
 */
olnk_status_t olnk_link_send_keepalive(olnk_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_H */
