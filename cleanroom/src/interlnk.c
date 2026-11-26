/* cleanroom/src/interlnk.c */
#include "interlnk/interlnk.h"

#include <string.h>

#define ILNK_POLL_CHUNK_SIZE   64U
#define ILNK_DEFAULT_POLL_MS   10U

ilnk_status_t ilnk_init(ilnk_ctx_t *ctx, ilnk_transport_t transport) {
    if (ctx == NULL) {
        return ILNK_ERR_INVALID_ARG;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->transport = transport;
    ctx->state = (uint8_t)ILNK_STATE_SEARCH_SYNC;
    return ILNK_OK;
}

void ilnk_reset(ilnk_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->rx_len = 0U;
    ctx->expected_frame_len = 0U;
    ctx->state = (uint8_t)ILNK_STATE_SEARCH_SYNC;
}

void ilnk_set_frame_handler(
    ilnk_ctx_t *ctx,
    ilnk_frame_handler_t handler,
    void *user_data
) {
    if (ctx == NULL) {
        return;
    }
    ctx->on_frame = handler;
    ctx->on_frame_user_data = user_data;
}

ilnk_status_t ilnk_feed_bytes(
    ilnk_ctx_t *ctx,
    const uint8_t *data,
    size_t len,
    size_t *bytes_processed
) {
    size_t i = 0U;

    if ((ctx == NULL) || ((data == NULL) && (len > 0U))) {
        return ILNK_ERR_INVALID_ARG;
    }

    if (bytes_processed != NULL) {
        *bytes_processed = 0U;
    }

    while (i < len) {
        uint8_t byte = data[i];
        ++i;

        switch ((ilnk_state_t)ctx->state) {
            case ILNK_STATE_SEARCH_SYNC:
                if (ctx->rx_len == 0U) {
                    if (byte == ILNK_SYNC_BYTE_0) {
                        ctx->rx_buffer[0] = byte;
                        ctx->rx_len = 1U;
                    }
                } else {
                    if (byte == ILNK_SYNC_BYTE_1) {
                        ctx->rx_buffer[1] = byte;
                        ctx->rx_len = 2U;
                        ctx->state = (uint8_t)ILNK_STATE_READ_HEADER;
                    } else if (byte == ILNK_SYNC_BYTE_0) {
                        /* Overlapping sync preamble (e.g. 0x55, 0x55, 0xAA) */
                        ctx->rx_buffer[0] = byte;
                        ctx->rx_len = 1U;
                    } else {
                        ctx->rx_len = 0U;
                    }
                }
                break;

            case ILNK_STATE_READ_HEADER:
                ctx->rx_buffer[ctx->rx_len] = byte;
                ctx->rx_len += 1U;

                if (ctx->rx_len == ILNK_HEADER_SIZE) {
                    uint16_t payload_len = ilnk_read_u16_le(&ctx->rx_buffer[4]);
                    if (payload_len > ILNK_MAX_PAYLOAD_SIZE) {
                        ctx->frames_rx_err += 1U;
                        ilnk_reset(ctx);
                        if (bytes_processed != NULL) {
                            *bytes_processed = i;
                        }
                        return ILNK_ERR_BUFFER_OVERFLOW;
                    }
                    ctx->expected_frame_len =
                        (uint16_t)(ILNK_HEADER_SIZE + payload_len + ILNK_CHECKSUM_SIZE);
                    ctx->state = (uint8_t)ILNK_STATE_READ_PAYLOAD;
                }
                break;

            case ILNK_STATE_READ_PAYLOAD:
                if (ctx->rx_len >= ILNK_RX_BUFFER_SIZE) {
                    ctx->frames_rx_err += 1U;
                    ilnk_reset(ctx);
                    if (bytes_processed != NULL) {
                        *bytes_processed = i;
                    }
                    return ILNK_ERR_BUFFER_OVERFLOW;
                }

                ctx->rx_buffer[ctx->rx_len] = byte;
                ctx->rx_len += 1U;

                if (ctx->rx_len == (size_t)ctx->expected_frame_len) {
                    ilnk_frame_t frame;
                    ilnk_status_t st;

                    ctx->state = (uint8_t)ILNK_STATE_EXEC_CMD;
                    st = ilnk_frame_decode(ctx->rx_buffer, ctx->rx_len, &frame);
                    if (st == ILNK_OK) {
                        ctx->seq_num = frame.sequence_number;
                        ctx->frames_rx_ok += 1U;
                        if (ctx->on_frame != NULL) {
                            ctx->on_frame(&frame, ctx->on_frame_user_data);
                        }
                    } else {
                        ctx->frames_rx_err += 1U;
                    }

                    ilnk_reset(ctx);
                    if (bytes_processed != NULL) {
                        *bytes_processed = i;
                    }
                    return st;
                }
                break;

            case ILNK_STATE_EXEC_CMD:
            default:
                ilnk_reset(ctx);
                break;
        }
    }

    if (bytes_processed != NULL) {
        *bytes_processed = i;
    }
    return ILNK_ERR_NEED_DATA;
}

ilnk_status_t ilnk_poll(ilnk_ctx_t *ctx) {
    uint8_t chunk[ILNK_POLL_CHUNK_SIZE];
    int nread;
    size_t offset = 0U;
    ilnk_status_t last_status = ILNK_ERR_NEED_DATA;

    if (ctx == NULL) {
        return ILNK_ERR_INVALID_ARG;
    }
    if (ctx->transport.read == NULL) {
        return ILNK_ERR_HAL_TIMEOUT;
    }

    nread = ctx->transport.read(
        chunk,
        sizeof(chunk),
        ILNK_DEFAULT_POLL_MS,
        ctx->transport.user_data
    );
    if (nread <= 0) {
        return ILNK_ERR_HAL_TIMEOUT;
    }

    while (offset < (size_t)nread) {
        size_t consumed = 0U;
        last_status = ilnk_feed_bytes(
            ctx,
            &chunk[offset],
            (size_t)nread - offset,
            &consumed
        );
        if (consumed == 0U) {
            break;
        }
        offset += consumed;
        if (last_status == ILNK_OK) {
            return ILNK_OK;
        }
    }

    return last_status;
}

ilnk_status_t ilnk_send_frame(
    ilnk_ctx_t *ctx,
    const ilnk_frame_t *frame,
    uint8_t *tx_scratch,
    size_t tx_scratch_cap,
    uint32_t timeout_ms
) {
    size_t wire_len = 0U;
    size_t sent = 0U;
    ilnk_status_t st;

    if ((ctx == NULL) || (frame == NULL) || (tx_scratch == NULL)) {
        return ILNK_ERR_INVALID_ARG;
    }
    if (ctx->transport.write == NULL) {
        return ILNK_ERR_HAL_TIMEOUT;
    }

    st = ilnk_frame_encode(frame, tx_scratch, tx_scratch_cap, &wire_len);
    if (st != ILNK_OK) {
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
            return ILNK_ERR_HAL_TIMEOUT;
        }
        sent += (size_t)written;
    }

    return ILNK_OK;
}
