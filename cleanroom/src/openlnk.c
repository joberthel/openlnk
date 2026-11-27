/* cleanroom/src/openlnk.c */
#include "openlnk/openlnk.h"
#include "openlnk/crc16.h"

#include <string.h>

#define OLNK_POLL_CHUNK_SIZE 64U
#define OLNK_DEFAULT_POLL_MS 10U

/* =========================================================================
 * Internal Transport Byte Helpers
 * ========================================================================= */

static olnk_status_t hal_write_all(
    olnk_ctx_t *ctx,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms
) {
    size_t sent = 0U;
    if (ctx->transport.write == NULL) {
        return OLNK_ERR_HAL_TIMEOUT;
    }
    while (sent < len) {
        int n = ctx->transport.write(&buf[sent], len - sent, timeout_ms, ctx->transport.user_data);
        if (n < 0) {
            return OLNK_ERR_HAL_IO;
        }
        if (n == 0) {
            return OLNK_ERR_HAL_TIMEOUT;
        }
        sent += (size_t)n;
    }
    return OLNK_OK;
}

static olnk_status_t hal_write_byte(olnk_ctx_t *ctx, uint8_t byte, uint32_t timeout_ms) {
    return hal_write_all(ctx, &byte, 1U, timeout_ms);
}

static olnk_status_t hal_read_byte(olnk_ctx_t *ctx, uint8_t *out_byte, uint32_t timeout_ms) {
    int n;
    if (ctx->transport.read == NULL) {
        return OLNK_ERR_HAL_TIMEOUT;
    }
    n = ctx->transport.read(out_byte, 1U, timeout_ms, ctx->transport.user_data);
    if (n < 0) {
        return OLNK_ERR_HAL_IO;
    }
    if (n == 0) {
        return OLNK_ERR_HAL_TIMEOUT;
    }
    return OLNK_OK;
}

static bool hal_set_baud_idx(olnk_ctx_t *ctx, uint8_t baud_idx) {
    uint32_t bps = olnk_baud_index_to_bps(baud_idx);
    if (bps == 0UL) {
        return false;
    }
    if (ctx->transport.set_baud == NULL) {
        return true;
    }
    return ctx->transport.set_baud(bps, ctx->transport.user_data);
}

static void hal_silent_drain(olnk_ctx_t *ctx, uint32_t silence_ms) {
    uint8_t discard;
    uint32_t guard = 0UL;
    while (guard < 16384UL) {
        olnk_status_t st = hal_read_byte(ctx, &discard, silence_ms);
        if (st != OLNK_OK) {
            break;
        }
        guard += 1UL;
    }
}

/* =========================================================================
 * Initialization & Streaming Canonical Frame Parser
 * ========================================================================= */

olnk_status_t olnk_init(olnk_ctx_t *ctx, olnk_transport_t transport) {
    if (ctx == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->transport = transport;
    ctx->parse_state = (uint8_t)OLNK_PARSE_HEADER;
    ctx->link_state = (uint8_t)OLNK_LINK_DISCONNECTED;
    ctx->seq = 0U;
    ctx->current_baud_idx = (uint8_t)OLNK_BAUD_9600;
    ctx->max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    ctx->max_block_size = (uint16_t)OLNK_MAX_PAYLOAD_SIZE;
    ctx->local_requires_echo = true;
    ctx->peer_echo_forced = false;
    ctx->sync_timeout_ms = OLNK_TIMEOUT_SYNC_MS;
    ctx->rx_byte_timeout_ms = OLNK_TIMEOUT_RX_BYTE_MS;
    ctx->tx_timeout_ms = OLNK_TIMEOUT_TX_ACK_MS;
    return OLNK_OK;
}

void olnk_reset(olnk_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->rx_len = 0U;
    ctx->expected_frame_len = 0U;
    ctx->parse_state = (uint8_t)OLNK_PARSE_HEADER;
}

void olnk_set_frame_handler(
    olnk_ctx_t *ctx,
    olnk_frame_handler_t handler,
    void *user_data
) {
    if (ctx == NULL) {
        return;
    }
    ctx->on_frame = handler;
    ctx->on_frame_user_data = user_data;
}

olnk_status_t olnk_feed_bytes(
    olnk_ctx_t *ctx,
    const uint8_t *data,
    size_t len,
    size_t *bytes_processed
) {
    size_t i = 0U;

    if ((ctx == NULL) || ((data == NULL) && (len > 0U))) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (bytes_processed != NULL) {
        *bytes_processed = 0U;
    }

    while (i < len) {
        uint8_t byte = data[i];
        ++i;

        switch ((olnk_parse_state_t)ctx->parse_state) {
            case OLNK_PARSE_HEADER:
                if ((byte & OLNK_HDR_RESERVED_MASK) != 0U) {
                    ctx->frames_rx_err += 1U;
                    olnk_reset(ctx);
                    if (bytes_processed != NULL) {
                        *bytes_processed = i;
                    }
                    return OLNK_ERR_INVALID_FRAME;
                }
                ctx->rx_buffer[0] = byte;
                ctx->rx_len = 1U;
                if ((byte & OLNK_HDR_SHORT_LEN) != 0U) {
                    ctx->parse_state = (uint8_t)OLNK_PARSE_LEN_SHORT;
                } else {
                    ctx->parse_state = (uint8_t)OLNK_PARSE_LEN_LO;
                }
                break;

            case OLNK_PARSE_LEN_SHORT:
                ctx->rx_buffer[1] = byte;
                ctx->rx_len = 2U;
                if (byte == 0U) {
                    ctx->frames_rx_err += 1U;
                    olnk_reset(ctx);
                    if (bytes_processed != NULL) {
                        *bytes_processed = i;
                    }
                    return OLNK_ERR_INVALID_FRAME;
                }
                ctx->expected_frame_len = (uint16_t)(1U + 1U + (uint16_t)byte + 2U);
                ctx->parse_state = (uint8_t)OLNK_PARSE_BODY_CRC;
                break;

            case OLNK_PARSE_LEN_LO:
                ctx->rx_buffer[1] = byte;
                ctx->rx_len = 2U;
                ctx->parse_state = (uint8_t)OLNK_PARSE_LEN_HI;
                break;

            case OLNK_PARSE_LEN_HI: {
                uint16_t payload_len;
                ctx->rx_buffer[2] = byte;
                ctx->rx_len = 3U;
                payload_len = olnk_read_u16_le(&ctx->rx_buffer[1]);
                if (payload_len == 0U) {
                    ctx->frames_rx_err += 1U;
                    olnk_reset(ctx);
                    if (bytes_processed != NULL) {
                        *bytes_processed = i;
                    }
                    return OLNK_ERR_INVALID_FRAME;
                }
                if (payload_len > OLNK_MAX_PAYLOAD_SIZE) {
                    ctx->frames_rx_err += 1U;
                    olnk_reset(ctx);
                    if (bytes_processed != NULL) {
                        *bytes_processed = i;
                    }
                    return OLNK_ERR_BUFFER_OVERFLOW;
                }
                ctx->expected_frame_len = (uint16_t)(1U + 2U + payload_len + 2U);
                ctx->parse_state = (uint8_t)OLNK_PARSE_BODY_CRC;
                break;
            }

            case OLNK_PARSE_BODY_CRC:
                if (ctx->rx_len >= OLNK_FRAME_BUFFER_SIZE) {
                    ctx->frames_rx_err += 1U;
                    olnk_reset(ctx);
                    if (bytes_processed != NULL) {
                        *bytes_processed = i;
                    }
                    return OLNK_ERR_BUFFER_OVERFLOW;
                }
                ctx->rx_buffer[ctx->rx_len] = byte;
                ctx->rx_len += 1U;

                if (ctx->rx_len == (size_t)ctx->expected_frame_len) {
                    olnk_wire_frame_t frame;
                    olnk_status_t st = olnk_wire_frame_decode(
                        ctx->rx_buffer,
                        ctx->rx_len,
                        &frame,
                        NULL
                    );
                    if (st == OLNK_OK) {
                        ctx->seq = frame.sequence_number;
                        ctx->frames_rx_ok += 1U;
                        if (ctx->on_frame != NULL) {
                            ctx->on_frame(&frame, ctx->on_frame_user_data);
                        }
                    } else {
                        ctx->frames_rx_err += 1U;
                    }
                    olnk_reset(ctx);
                    if (bytes_processed != NULL) {
                        *bytes_processed = i;
                    }
                    return st;
                }
                break;

            default:
                olnk_reset(ctx);
                break;
        }
    }

    if (bytes_processed != NULL) {
        *bytes_processed = i;
    }
    return OLNK_ERR_NEED_DATA;
}

olnk_status_t olnk_poll(olnk_ctx_t *ctx) {
    uint8_t chunk[OLNK_POLL_CHUNK_SIZE];
    int nread;
    size_t offset = 0U;
    olnk_status_t last_status = OLNK_ERR_NEED_DATA;

    if (ctx == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (ctx->transport.read == NULL) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    nread = ctx->transport.read(
        chunk,
        sizeof(chunk),
        OLNK_DEFAULT_POLL_MS,
        ctx->transport.user_data
    );
    if (nread <= 0) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    while (offset < (size_t)nread) {
        size_t consumed = 0U;
        last_status = olnk_feed_bytes(
            ctx,
            &chunk[offset],
            (size_t)nread - offset,
            &consumed
        );
        if (consumed == 0U) {
            break;
        }
        offset += consumed;
        if (last_status == OLNK_OK) {
            return OLNK_OK;
        }
    }

    return last_status;
}

/* =========================================================================
 * Active Link-Layer State Machine (Phase A Sync, Phase B Baud, Phase C Frame)
 * ========================================================================= */

void olnk_link_disconnect(olnk_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->link_state = (uint8_t)OLNK_LINK_DISCONNECTED;
    ctx->seq = 0U;
    ctx->current_baud_idx = (uint8_t)OLNK_BAUD_9600;
    ctx->peer_echo_forced = false;
    ctx->keepalive_missed = 0U;
    (void)hal_set_baud_idx(ctx, (uint8_t)OLNK_BAUD_9600);
}

olnk_status_t olnk_link_negotiate_baud_initiator(
    olnk_ctx_t *ctx,
    uint8_t target_baud_idx
) {
    uint8_t prev_baud_idx;
    uint8_t req_byte;
    uint8_t rx_byte = 0U;
    uint8_t beacon;
    uint8_t inv_beacon;
    uint8_t k;
    uint8_t tries;
    olnk_status_t st;

    if ((ctx == NULL) || (target_baud_idx >= OLNK_BAUD_INDEX_COUNT)) {
        return OLNK_ERR_INVALID_ARG;
    }

    prev_baud_idx = ctx->current_baud_idx;
    req_byte = olnk_baud_req_byte(ctx->seq);

    /* 1. Send baud-change request (~seq) and wait for seq ACK */
    st = hal_write_byte(ctx, req_byte, ctx->tx_timeout_ms);
    if (st != OLNK_OK) {
        return st;
    }
    st = hal_read_byte(ctx, &rx_byte, ctx->tx_timeout_ms);
    if ((st != OLNK_OK) || (rx_byte != (ctx->seq & OLNK_HDR_SEQ_MASK))) {
        return (st == OLNK_OK) ? OLNK_ERR_PROTOCOL : st;
    }

    /* 2. Send target baud index and switch own UART */
    st = hal_write_byte(ctx, target_baud_idx, ctx->tx_timeout_ms);
    if (st != OLNK_OK) {
        return st;
    }
    if (!hal_set_baud_idx(ctx, target_baud_idx)) {
        (void)hal_set_baud_idx(ctx, prev_baud_idx);
        return OLNK_ERR_HAL_IO;
    }

    /* 3. Lock onto responder beacon at new baud rate */
    beacon = olnk_baud_beacon_byte(target_baud_idx, ctx->seq);
    inv_beacon = (uint8_t)(~beacon);

    st = OLNK_ERR_HAL_TIMEOUT;
    for (tries = 0U; tries < 8U; ++tries) {
        st = hal_read_byte(ctx, &rx_byte, ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            break;
        }
        if (rx_byte == beacon) {
            break;
        }
    }
    if ((st != OLNK_OK) || (rx_byte != beacon)) {
        (void)hal_set_baud_idx(ctx, prev_baud_idx);
        return OLNK_ERR_HAL_TIMEOUT;
    }

    /* Echo beacon back to responder */
    st = hal_write_byte(ctx, beacon, ctx->tx_timeout_ms);
    if (st != OLNK_OK) {
        (void)hal_set_baud_idx(ctx, prev_baud_idx);
        return st;
    }

    /* Wait for ~beacon (tolerate any duplicate in-flight beacon) */
    for (tries = 0U; tries < 6U; ++tries) {
        st = hal_read_byte(ctx, &rx_byte, ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            (void)hal_set_baud_idx(ctx, prev_baud_idx);
            return st;
        }
        if (rx_byte == inv_beacon) {
            break;
        }
        if (rx_byte != beacon) {
            (void)hal_set_baud_idx(ctx, prev_baud_idx);
            return OLNK_ERR_PROTOCOL;
        }
    }
    if (rx_byte != inv_beacon) {
        (void)hal_set_baud_idx(ctx, prev_baud_idx);
        return OLNK_ERR_HAL_TIMEOUT;
    }

    /* 4. 15-pair bidirectional line quality test */
    for (k = 0U; k < OLNK_BAUD_TEST_LEN; ++k) {
        st = hal_write_byte(ctx, olnk_baud_test_tx[k], ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            (void)hal_set_baud_idx(ctx, prev_baud_idx);
            return st;
        }
        st = hal_read_byte(ctx, &rx_byte, ctx->tx_timeout_ms);
        if ((st != OLNK_OK) || (rx_byte != olnk_baud_test_rx[k])) {
            (void)hal_set_baud_idx(ctx, prev_baud_idx);
            return (st == OLNK_OK) ? OLNK_ERR_PROTOCOL : st;
        }
    }

    ctx->current_baud_idx = target_baud_idx;
    ctx->baud_switches += 1U;
    return OLNK_OK;
}

olnk_status_t olnk_link_handle_baud_responder(olnk_ctx_t *ctx) {
    uint8_t prev_baud_idx;
    uint8_t target_baud_idx = 0U;
    uint8_t beacon;
    uint8_t rx_byte = 0U;
    uint8_t retry;
    uint8_t k;
    bool locked = false;
    olnk_status_t st;

    if (ctx == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

    prev_baud_idx = ctx->current_baud_idx;

    /* 1. ACK baud request with seq */
    st = hal_write_byte(ctx, (uint8_t)(ctx->seq & OLNK_HDR_SEQ_MASK), ctx->tx_timeout_ms);
    if (st != OLNK_OK) {
        return st;
    }

    /* 2. Receive target baud index */
    st = hal_read_byte(ctx, &target_baud_idx, ctx->rx_byte_timeout_ms);
    if (st != OLNK_OK) {
        return st;
    }
    if ((target_baud_idx >= OLNK_BAUD_INDEX_COUNT) ||
        (target_baud_idx > ctx->max_baud_idx)) {
        return OLNK_ERR_PROTOCOL;
    }

    /* Switch own UART */
    if (!hal_set_baud_idx(ctx, target_baud_idx)) {
        (void)hal_set_baud_idx(ctx, prev_baud_idx);
        return OLNK_ERR_HAL_IO;
    }

    /* 3. Transmit beacon up to 5 ticks waiting for echo */
    beacon = olnk_baud_beacon_byte(target_baud_idx, ctx->seq);
    for (retry = 0U; retry < OLNK_BEACON_MAX_RETRIES; ++retry) {
        st = hal_write_byte(ctx, beacon, ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            (void)hal_set_baud_idx(ctx, prev_baud_idx);
            return st;
        }
        st = hal_read_byte(ctx, &rx_byte, OLNK_TIMEOUT_BEACON_TICK_MS);
        if ((st == OLNK_OK) && (rx_byte == beacon)) {
            locked = true;
            break;
        }
    }

    if (!locked) {
        (void)hal_set_baud_idx(ctx, prev_baud_idx);
        return OLNK_ERR_HAL_TIMEOUT;
    }

    /* Send ~beacon */
    st = hal_write_byte(ctx, (uint8_t)(~beacon), ctx->tx_timeout_ms);
    if (st != OLNK_OK) {
        (void)hal_set_baud_idx(ctx, prev_baud_idx);
        return st;
    }

    /* 4. 15-pair line quality test */
    for (k = 0U; k < OLNK_BAUD_TEST_LEN; ++k) {
        st = hal_read_byte(ctx, &rx_byte, ctx->rx_byte_timeout_ms);
        if ((st != OLNK_OK) || (rx_byte != olnk_baud_test_tx[k])) {
            (void)hal_set_baud_idx(ctx, prev_baud_idx);
            return (st == OLNK_OK) ? OLNK_ERR_PROTOCOL : st;
        }
        st = hal_write_byte(ctx, olnk_baud_test_rx[k], ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            (void)hal_set_baud_idx(ctx, prev_baud_idx);
            return st;
        }
    }

    ctx->current_baud_idx = target_baud_idx;
    ctx->baud_switches += 1U;
    return OLNK_OK;
}

olnk_status_t olnk_link_connect_client(olnk_ctx_t *ctx) {
    uint8_t rx = 0U;
    olnk_status_t st;

    if (ctx == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

    ctx->link_state = (uint8_t)OLNK_LINK_SYNCING;
    ctx->current_baud_idx = (uint8_t)OLNK_BAUD_9600;
    if (!hal_set_baud_idx(ctx, (uint8_t)OLNK_BAUD_9600)) {
        ctx->link_state = (uint8_t)OLNK_LINK_DISCONNECTED;
        return OLNK_ERR_HAL_IO;
    }

    /* Phase A: 3-step Sync Handshake (0xAA->0x00, 0x55->0xFF, 0x5A->0x11) */
    st = hal_write_byte(ctx, OLNK_SYNC_C1, ctx->sync_timeout_ms);
    if (st != OLNK_OK) {
        ctx->link_state = (uint8_t)OLNK_LINK_DISCONNECTED;
        return st;
    }
    st = hal_read_byte(ctx, &rx, ctx->sync_timeout_ms);
    if ((st != OLNK_OK) || (rx != OLNK_SYNC_S1)) {
        ctx->link_state = (uint8_t)OLNK_LINK_DISCONNECTED;
        return (st == OLNK_OK) ? OLNK_ERR_PROTOCOL : st;
    }

    st = hal_write_byte(ctx, OLNK_SYNC_C2, ctx->sync_timeout_ms);
    if (st != OLNK_OK) {
        ctx->link_state = (uint8_t)OLNK_LINK_DISCONNECTED;
        return st;
    }
    st = hal_read_byte(ctx, &rx, ctx->sync_timeout_ms);
    if ((st != OLNK_OK) || (rx != OLNK_SYNC_S2)) {
        ctx->link_state = (uint8_t)OLNK_LINK_DISCONNECTED;
        return (st == OLNK_OK) ? OLNK_ERR_PROTOCOL : st;
    }

    st = hal_write_byte(ctx, OLNK_SYNC_C3, ctx->sync_timeout_ms);
    if (st != OLNK_OK) {
        ctx->link_state = (uint8_t)OLNK_LINK_DISCONNECTED;
        return st;
    }
    st = hal_read_byte(ctx, &rx, ctx->sync_timeout_ms);
    if ((st != OLNK_OK) || (rx != OLNK_SYNC_S3)) {
        ctx->link_state = (uint8_t)OLNK_LINK_DISCONNECTED;
        return (st == OLNK_OK) ? OLNK_ERR_PROTOCOL : st;
    }

    /* Link established at 9600 bps, seq = 0 */
    ctx->link_state = (uint8_t)OLNK_LINK_CONNECTED;
    ctx->seq = 0U;
    ctx->peer_echo_forced = false;
    ctx->keepalive_missed = 0U;

    /* Phase B: stepwise baud step-up (3 -> 4 -> ... -> max_baud_idx) */
    while (ctx->current_baud_idx < ctx->max_baud_idx) {
        uint8_t next_idx = (uint8_t)(ctx->current_baud_idx + 1U);
        st = olnk_link_negotiate_baud_initiator(ctx, next_idx);
        if (st != OLNK_OK) {
            /* Fallback: clamp session ceiling to last stable baud rate */
            ctx->max_baud_idx = ctx->current_baud_idx;
            break;
        }
    }

    return OLNK_OK;
}

static olnk_status_t send_frame_once(
    olnk_ctx_t *ctx,
    const uint8_t *payload,
    uint16_t payload_len,
    bool more_fragments,
    bool echo_request,
    bool *out_used_echo
) {
    bool is_short = (payload_len <= 255U);
    uint8_t hdr = olnk_hdr_make(is_short, more_fragments, echo_request, ctx->seq);
    uint8_t len_bytes[2];
    size_t len_sz = is_short ? 1U : 2U;
    uint16_t crc = OLNK_CRC16_INIT;
    uint8_t crc_be[2];
    uint8_t ack_hdr = 0U;
    uint8_t final_ack = 0U;
    bool echo_mode = false;
    size_t i;
    olnk_status_t st;

    if (is_short) {
        len_bytes[0] = (uint8_t)payload_len;
    } else {
        olnk_write_u16_le(len_bytes, payload_len);
    }

    for (i = 0U; i < len_sz; ++i) {
        crc = olnk_crc16_update(crc, len_bytes[i]);
    }
    for (i = 0U; i < (size_t)payload_len; ++i) {
        crc = olnk_crc16_update(crc, payload[i]);
    }
    crc = (uint16_t)(crc ^ OLNK_CRC16_XOROUT);
    olnk_write_u16_be(crc_be, crc);

    /* 1. Send header and wait for header ACK */
    st = hal_write_byte(ctx, hdr, ctx->tx_timeout_ms);
    if (st != OLNK_OK) {
        return st;
    }
    st = hal_read_byte(ctx, &ack_hdr, ctx->tx_timeout_ms);
    if (st != OLNK_OK) {
        return st;
    }
    if (!olnk_hdr_verify_ack(hdr, ack_hdr, &echo_mode)) {
        return OLNK_ERR_PROTOCOL;
    }
    if (out_used_echo != NULL) {
        *out_used_echo = echo_mode;
    }

    /* 2. Send length, payload, crc_hi, crc_lo */
    if (echo_mode) {
        uint8_t echoed = 0U;

        for (i = 0U; i < len_sz; ++i) {
            st = hal_write_byte(ctx, len_bytes[i], ctx->tx_timeout_ms);
            if (st != OLNK_OK) {
                return st;
            }
            st = hal_read_byte(ctx, &echoed, ctx->tx_timeout_ms);
            if ((st != OLNK_OK) || (echoed != len_bytes[i])) {
                return (st == OLNK_OK) ? OLNK_ERR_PROTOCOL : st;
            }
        }

        for (i = 0U; i < (size_t)payload_len; ++i) {
            st = hal_write_byte(ctx, payload[i], ctx->tx_timeout_ms);
            if (st != OLNK_OK) {
                return st;
            }
            st = hal_read_byte(ctx, &echoed, ctx->tx_timeout_ms);
            if ((st != OLNK_OK) || (echoed != payload[i])) {
                return (st == OLNK_OK) ? OLNK_ERR_PROTOCOL : st;
            }
        }

        /* Echo crc_hi */
        st = hal_write_byte(ctx, crc_be[0], ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            return st;
        }
        st = hal_read_byte(ctx, &echoed, ctx->tx_timeout_ms);
        if ((st != OLNK_OK) || (echoed != crc_be[0])) {
            return (st == OLNK_OK) ? OLNK_ERR_PROTOCOL : st;
        }

        /* crc_lo is NOT echoed — receiver evaluates CRC and sends final_ack directly */
        st = hal_write_byte(ctx, crc_be[1], ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            return st;
        }
    } else {
        /* Burst mode: stream length + payload + crc_be consecutively */
        st = hal_write_all(ctx, len_bytes, len_sz, ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            return st;
        }
        st = hal_write_all(ctx, payload, (size_t)payload_len, ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            return st;
        }
        st = hal_write_all(ctx, crc_be, 2U, ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            return st;
        }
    }

    /* 3. Wait for final_ack == hdr (in Burst mode, allow wire TX serialization time) */
    {
        uint32_t ack_wait_ms = ctx->tx_timeout_ms;
        if (!echo_mode) {
            uint32_t bps = olnk_baud_index_to_bps(ctx->current_baud_idx);
            uint32_t wire_bytes = (uint32_t)len_sz + (uint32_t)payload_len + 2UL;
            if (bps == 0UL) {
                bps = 9600UL;
            }
            ack_wait_ms += (wire_bytes * 12000UL) / bps;
        }
        st = hal_read_byte(ctx, &final_ack, ack_wait_ms);
    }
    if (st != OLNK_OK) {
        return st;
    }
    if (final_ack != hdr) {
        return OLNK_ERR_PROTOCOL;
    }

    return OLNK_OK;
}

olnk_status_t olnk_link_send_frame(
    olnk_ctx_t *ctx,
    const uint8_t *payload,
    uint16_t payload_len,
    bool more_fragments
) {
    uint8_t echo_failures = 0U;

    if ((ctx == NULL) || (payload == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if ((payload_len == 0U) || (payload_len > OLNK_MAX_PAYLOAD_SIZE)) {
        return OLNK_ERR_INVALID_ARG;
    }

    for (;;) {
        bool used_echo = ctx->peer_echo_forced;
        olnk_status_t st = send_frame_once(
            ctx,
            payload,
            payload_len,
            more_fragments,
            ctx->peer_echo_forced,
            &used_echo
        );
        if (st == OLNK_OK) {
            ctx->seq = (uint8_t)((ctx->seq + 1U) & OLNK_HDR_SEQ_MASK);
            ctx->frames_tx_ok += 1U;
            return OLNK_OK;
        }
        if (st == OLNK_ERR_HAL_IO) {
            olnk_link_disconnect(ctx);
            return OLNK_ERR_HAL_IO;
        }

        ctx->frames_tx_retry += 1U;

        if (!used_echo) {
            /* Burst failed: immediately escalate to Echo mode at current baud */
            ctx->peer_echo_forced = true;
            echo_failures = 0U;
            continue;
        }

        echo_failures += 1U;
        if (echo_failures < 2U) {
            continue;
        }

        /* 2 consecutive failures in Echo mode: step down baud if > 9600 bps */
        if (ctx->current_baud_idx > (uint8_t)OLNK_BAUD_9600) {
            uint8_t lower_idx = (uint8_t)(ctx->current_baud_idx - 1U);
            if (olnk_link_negotiate_baud_initiator(ctx, lower_idx) == OLNK_OK) {
                ctx->peer_echo_forced = false;
                echo_failures = 0U;
                continue;
            }
        }

        olnk_link_disconnect(ctx);
        return OLNK_ERR_DISCONNECTED;
    }
}

olnk_status_t olnk_link_send_message(
    olnk_ctx_t *ctx,
    const uint8_t *data,
    uint32_t total_len
) {
    uint32_t offset = 0UL;
    uint16_t max_blk;

    if ((ctx == NULL) || (data == NULL) || (total_len == 0UL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    max_blk = ctx->max_block_size;
    if ((max_blk == 0U) || (max_blk > OLNK_MAX_PAYLOAD_SIZE)) {
        max_blk = (uint16_t)OLNK_MAX_PAYLOAD_SIZE;
    }

    while (offset < total_len) {
        uint32_t remain = total_len - offset;
        uint16_t chunk = (remain > (uint32_t)max_blk) ? max_blk : (uint16_t)remain;
        bool more = (remain > (uint32_t)chunk);
        olnk_status_t st = olnk_link_send_frame(ctx, &data[offset], chunk, more);
        if (st != OLNK_OK) {
            return st;
        }
        offset += (uint32_t)chunk;
    }

    return OLNK_OK;
}

olnk_status_t olnk_link_recv_frame(
    olnk_ctx_t *ctx,
    uint8_t *out_payload,
    size_t out_cap,
    uint16_t *out_len,
    bool *out_more_fragments,
    uint32_t first_byte_timeout_ms
) {
    uint8_t *dst;
    size_t dst_cap;
    uint8_t hdr = 0U;
    uint8_t ack_hdr;
    bool echo_mode;
    bool is_short;
    uint16_t payload_len = 0U;
    uint16_t crc = OLNK_CRC16_INIT;
    uint8_t b = 0U;
    uint16_t i;
    olnk_status_t st;

    if ((ctx == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    dst = (out_payload != NULL) ? out_payload : ctx->rx_buffer;
    dst_cap = (out_payload != NULL) ? out_cap : OLNK_MAX_PAYLOAD_SIZE;

    /* 1. Wait for frame header (also handle peer baud-change step-down if requested) */
    for (;;) {
        st = hal_read_byte(ctx, &hdr, first_byte_timeout_ms);
        if (st != OLNK_OK) {
            return st;
        }
        if (hdr == olnk_baud_req_byte(ctx->seq)) {
            (void)olnk_link_handle_baud_responder(ctx);
            continue;
        }
        if (hdr == olnk_keepalive_byte(ctx->seq)) {
            (void)hal_write_byte(ctx, hdr, ctx->tx_timeout_ms);
            ctx->keepalives_ok += 1U;
            continue;
        }
        if (((hdr & OLNK_HDR_RESERVED_MASK) != 0U) ||
            ((hdr & OLNK_HDR_SEQ_MASK) != (ctx->seq & OLNK_HDR_SEQ_MASK))) {
            ctx->frames_rx_err += 1U;
            hal_silent_drain(ctx, ctx->rx_byte_timeout_ms);
            return OLNK_ERR_INVALID_FRAME;
        }
        break;
    }

    /* 2. Send header ACK negotiating Echo vs Burst mode */
    ack_hdr = olnk_hdr_compute_ack(hdr, ctx->local_requires_echo);
    echo_mode = ((ack_hdr & OLNK_HDR_ECHO_REQUEST) != 0U);
    st = hal_write_byte(ctx, ack_hdr, ctx->tx_timeout_ms);
    if (st != OLNK_OK) {
        return st;
    }

    /* 3. Receive length field */
    is_short = ((hdr & OLNK_HDR_SHORT_LEN) != 0U);
    st = hal_read_byte(ctx, &b, ctx->rx_byte_timeout_ms);
    if (st != OLNK_OK) {
        ctx->frames_rx_err += 1U;
        return st;
    }
    crc = olnk_crc16_update(crc, b);
    if (echo_mode) {
        st = hal_write_byte(ctx, b, ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            return st;
        }
    }

    if (is_short) {
        payload_len = (uint16_t)b;
    } else {
        uint8_t hi = 0U;
        uint8_t lo = b;
        st = hal_read_byte(ctx, &hi, ctx->rx_byte_timeout_ms);
        if (st != OLNK_OK) {
            ctx->frames_rx_err += 1U;
            return st;
        }
        crc = olnk_crc16_update(crc, hi);
        if (echo_mode) {
            st = hal_write_byte(ctx, hi, ctx->tx_timeout_ms);
            if (st != OLNK_OK) {
                return st;
            }
        }
        payload_len = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8U));
    }

    if ((payload_len == 0U) || (payload_len > OLNK_MAX_PAYLOAD_SIZE) || ((size_t)payload_len > dst_cap)) {
        ctx->frames_rx_err += 1U;
        hal_silent_drain(ctx, ctx->rx_byte_timeout_ms);
        return (payload_len == 0U) ? OLNK_ERR_INVALID_FRAME : OLNK_ERR_BUFFER_OVERFLOW;
    }

    /* 4. Receive payload bytes */
    for (i = 0U; i < payload_len; ++i) {
        st = hal_read_byte(ctx, &b, ctx->rx_byte_timeout_ms);
        if (st != OLNK_OK) {
            ctx->frames_rx_err += 1U;
            return st;
        }
        dst[i] = b;
        crc = olnk_crc16_update(crc, b);
        if (echo_mode) {
            st = hal_write_byte(ctx, b, ctx->tx_timeout_ms);
            if (st != OLNK_OK) {
                return st;
            }
        }
    }

    /* 5. Receive crc_hi (echoed in Echo mode) */
    st = hal_read_byte(ctx, &b, ctx->rx_byte_timeout_ms);
    if (st != OLNK_OK) {
        ctx->frames_rx_err += 1U;
        return st;
    }
    crc = olnk_crc16_update(crc, b);
    if (echo_mode) {
        st = hal_write_byte(ctx, b, ctx->tx_timeout_ms);
        if (st != OLNK_OK) {
            return st;
        }
    }

    /* 6. Receive crc_lo (NEVER echoed!) */
    st = hal_read_byte(ctx, &b, ctx->rx_byte_timeout_ms);
    if (st != OLNK_OK) {
        ctx->frames_rx_err += 1U;
        return st;
    }
    crc = olnk_crc16_update(crc, b);

    /* 7. Validate CRC-16/GENIBUS residue == 0x1D0F */
    if (crc != OLNK_CRC16_RESIDUE_GOOD) {
        ctx->frames_rx_err += 1U;
        hal_silent_drain(ctx, ctx->rx_byte_timeout_ms);
        return OLNK_ERR_CHECKSUM_MISMATCH;
    }

    /* 8. Send final_ack == hdr and advance sequence number */
    st = hal_write_byte(ctx, hdr, ctx->tx_timeout_ms);
    if (st != OLNK_OK) {
        return st;
    }

    ctx->seq = (uint8_t)((ctx->seq + 1U) & OLNK_HDR_SEQ_MASK);
    ctx->frames_rx_ok += 1U;
    *out_len = payload_len;
    if (out_more_fragments != NULL) {
        *out_more_fragments = ((hdr & OLNK_HDR_MORE_FRAGMENTS) != 0U);
    }
    return OLNK_OK;
}

olnk_status_t olnk_link_recv_message(
    olnk_ctx_t *ctx,
    uint8_t *out_buf,
    uint32_t out_cap,
    uint32_t *out_len,
    uint32_t first_byte_timeout_ms
) {
    uint32_t total = 0UL;
    bool more = true;
    uint32_t timeout_ms = first_byte_timeout_ms;

    if ((ctx == NULL) || (out_buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    while (more) {
        uint16_t chunk_len = 0U;
        uint32_t remain = out_cap - total;
        size_t chunk_cap = (remain > (uint32_t)OLNK_MAX_PAYLOAD_SIZE)
                               ? (size_t)OLNK_MAX_PAYLOAD_SIZE
                               : (size_t)remain;
        olnk_status_t st = olnk_link_recv_frame(
            ctx,
            &out_buf[total],
            chunk_cap,
            &chunk_len,
            &more,
            timeout_ms
        );
        if (st == OLNK_ERR_CHECKSUM_MISMATCH) {
            /* Wait for peer retransmission of same seq */
            timeout_ms = ctx->tx_timeout_ms * 2U;
            continue;
        }
        if (st != OLNK_OK) {
            return st;
        }
        total += (uint32_t)chunk_len;
        timeout_ms = 2000U;
    }

    *out_len = total;
    return OLNK_OK;
}

olnk_status_t olnk_link_send_turnaround(olnk_ctx_t *ctx) {
    if (ctx == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    return hal_write_byte(ctx, olnk_turnaround_byte(ctx->seq), ctx->tx_timeout_ms);
}

olnk_status_t olnk_link_wait_turnaround(olnk_ctx_t *ctx, uint32_t timeout_ms) {
    uint8_t expected;
    uint8_t b = 0U;
    olnk_status_t st;

    if (ctx == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    expected = olnk_turnaround_byte(ctx->seq);
    st = hal_read_byte(ctx, &b, timeout_ms);
    if (st != OLNK_OK) {
        return st;
    }
    if (b != expected) {
        return OLNK_ERR_SEQ_MISMATCH;
    }
    return OLNK_OK;
}

olnk_status_t olnk_link_send_keepalive(olnk_ctx_t *ctx) {
    uint8_t ka;
    uint8_t rx = 0U;
    olnk_status_t st;

    if (ctx == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

    ka = olnk_keepalive_byte(ctx->seq);
    st = hal_write_byte(ctx, ka, OLNK_BIOS_TICK_MS);
    if (st == OLNK_OK) {
        st = hal_read_byte(ctx, &rx, OLNK_BIOS_TICK_MS);
    }

    if ((st == OLNK_OK) && (rx == ka)) {
        ctx->keepalive_missed = 0U;
        ctx->keepalives_ok += 1U;
        return OLNK_OK;
    }

    ctx->keepalive_missed += 1U;
    if (ctx->keepalive_missed > OLNK_KEEPALIVE_MAX_MISSED) {
        olnk_link_disconnect(ctx);
        return OLNK_ERR_DISCONNECTED;
    }
    return OLNK_ERR_HAL_TIMEOUT;
}
