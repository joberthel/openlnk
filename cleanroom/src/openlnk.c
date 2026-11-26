/* cleanroom/src/openlnk.c */
#include "openlnk/openlnk.h"

#include <string.h>

#define OLNK_POLL_CHUNK_SIZE   64U
#define OLNK_DEFAULT_POLL_MS   10U

olnk_status_t olnk_init(olnk_ctx_t *ctx, olnk_transport_t transport) {
    if (ctx == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->transport = transport;
    ctx->state = (uint8_t)OLNK_STATE_SEARCH_SYNC;
    return OLNK_OK;
}

void olnk_reset(olnk_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->rx_len = 0U;
    ctx->expected_frame_len = 0U;
    ctx->state = (uint8_t)OLNK_STATE_SEARCH_SYNC;
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

        switch ((olnk_state_t)ctx->state) {
            case OLNK_STATE_SEARCH_SYNC:
                if (ctx->rx_len == 0U) {
                    if (byte == OLNK_SYNC_BYTE_0) {
                        ctx->rx_buffer[0] = byte;
                        ctx->rx_len = 1U;
                    }
                } else {
                    if (byte == OLNK_SYNC_BYTE_1) {
                        ctx->rx_buffer[1] = byte;
                        ctx->rx_len = 2U;
                        ctx->state = (uint8_t)OLNK_STATE_READ_HEADER;
                    } else if (byte == OLNK_SYNC_BYTE_0) {
                        /* Overlapping sync preamble (e.g. 0x55, 0x55, 0xAA) */
                        ctx->rx_buffer[0] = byte;
                        ctx->rx_len = 1U;
                    } else {
                        ctx->rx_len = 0U;
                    }
                }
                break;

            case OLNK_STATE_READ_HEADER:
                ctx->rx_buffer[ctx->rx_len] = byte;
                ctx->rx_len += 1U;

                if (ctx->rx_len == OLNK_HEADER_SIZE) {
                    uint16_t payload_len = olnk_read_u16_le(&ctx->rx_buffer[4]);
                    if (payload_len > OLNK_MAX_PAYLOAD_SIZE) {
                        ctx->frames_rx_err += 1U;
                        olnk_reset(ctx);
                        if (bytes_processed != NULL) {
                            *bytes_processed = i;
                        }
                        return OLNK_ERR_BUFFER_OVERFLOW;
                    }
                    ctx->expected_frame_len =
                        (uint16_t)(OLNK_HEADER_SIZE + payload_len + OLNK_CHECKSUM_SIZE);
                    ctx->state = (uint8_t)OLNK_STATE_READ_PAYLOAD;
                }
                break;

            case OLNK_STATE_READ_PAYLOAD:
                if (ctx->rx_len >= OLNK_RX_BUFFER_SIZE) {
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
                    olnk_frame_t frame;
                    olnk_status_t st;

                    ctx->state = (uint8_t)OLNK_STATE_EXEC_CMD;
                    st = olnk_frame_decode(ctx->rx_buffer, ctx->rx_len, &frame);
                    if (st == OLNK_OK) {
                        ctx->seq_num = frame.sequence_number;
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

            case OLNK_STATE_EXEC_CMD:
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

olnk_status_t olnk_send_frame(
    olnk_ctx_t *ctx,
    const olnk_frame_t *frame,
    uint8_t *tx_scratch,
    size_t tx_scratch_cap,
    uint32_t timeout_ms
) {
    size_t wire_len = 0U;
    size_t sent = 0U;
    olnk_status_t st;

    if ((ctx == NULL) || (frame == NULL) || (tx_scratch == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (ctx->transport.write == NULL) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    st = olnk_frame_encode(frame, tx_scratch, tx_scratch_cap, &wire_len);
    if (st != OLNK_OK) {
        return st;
    }

    while (sent < wire_len) {
        int written = ctx->transport.write(
            &tx_scratch[sent],
            wire_len - sent,
            timeout_ms,
            ctx->transport.user_data
        );
        if (written <= 0) {
            return OLNK_ERR_HAL_TIMEOUT;
        }
        sent += (size_t)written;
    }

    return OLNK_OK;
}
