/* cleanroom/src/protocol.c */
#include "openlnk/protocol.h"
#include "openlnk/crc16.h"

#include <string.h>

uint16_t olnk_read_u16_le(const uint8_t *buf) {
    return (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8U));
}

uint32_t olnk_read_u32_le(const uint8_t *buf) {
    return ((uint32_t)buf[0]) |
           ((uint32_t)buf[1] << 8U) |
           ((uint32_t)buf[2] << 16U) |
           ((uint32_t)buf[3] << 24U);
}

void olnk_write_u16_le(uint8_t *buf, uint16_t value) {
    buf[0] = (uint8_t)(value & 0xFFU);
    buf[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

void olnk_write_u32_le(uint8_t *buf, uint32_t value) {
    buf[0] = (uint8_t)(value & 0xFFU);
    buf[1] = (uint8_t)((value >> 8U) & 0xFFU);
    buf[2] = (uint8_t)((value >> 16U) & 0xFFU);
    buf[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

olnk_status_t olnk_frame_encode(
    const olnk_frame_t *frame,
    uint8_t *out_buf,
    size_t out_cap,
    size_t *out_len
) {
    size_t payload_len = 0U;
    size_t total_len = 0U;
    uint8_t *payload_ptr;
    uint16_t crc;

    if ((frame == NULL) || (out_buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    switch (frame->frame_type) {
        case OLNK_FRAME_CMD_INIT:
            payload_len = 3U;
            break;
        case OLNK_FRAME_CMD_READ_SECTOR:
            payload_len = 7U;
            break;
        case OLNK_FRAME_CMD_WRITE_SECTOR: {
            size_t expected_bytes;
            if (frame->payload.write_sector.sector_count >
                (uint16_t)((OLNK_MAX_PAYLOAD_SIZE - 7U) / OLNK_SECTOR_SIZE)) {
                return OLNK_ERR_BUFFER_OVERFLOW;
            }
            expected_bytes = (size_t)frame->payload.write_sector.sector_count * OLNK_SECTOR_SIZE;
            if ((expected_bytes > 0U) && (frame->payload.write_sector.data == NULL)) {
                return OLNK_ERR_INVALID_ARG;
            }
            if ((frame->payload.write_sector.data_len != 0U) &&
                (frame->payload.write_sector.data_len != expected_bytes)) {
                return OLNK_ERR_INVALID_ARG;
            }
            payload_len = 7U + expected_bytes;
            break;
        }
        case OLNK_FRAME_RESP_INIT_ACK:
            payload_len = 4U;
            break;
        case OLNK_FRAME_RESP_READ_DATA:
            if (frame->payload.read_data.data_len > (OLNK_MAX_PAYLOAD_SIZE - 5U)) {
                return OLNK_ERR_BUFFER_OVERFLOW;
            }
            if ((frame->payload.read_data.data_len > 0U) &&
                (frame->payload.read_data.data == NULL)) {
                return OLNK_ERR_INVALID_ARG;
            }
            payload_len = 5U + frame->payload.read_data.data_len;
            break;
        case OLNK_FRAME_RESP_ERROR:
            payload_len = 3U;
            break;
        default:
            return OLNK_ERR_INVALID_FRAME;
    }

    if (payload_len > OLNK_MAX_PAYLOAD_SIZE) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }

    total_len = OLNK_HEADER_SIZE + payload_len + OLNK_CHECKSUM_SIZE;
    if (total_len > out_cap) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }

    out_buf[0] = OLNK_SYNC_BYTE_0;
    out_buf[1] = OLNK_SYNC_BYTE_1;
    out_buf[2] = frame->frame_type;
    out_buf[3] = frame->sequence_number;
    olnk_write_u16_le(&out_buf[4], (uint16_t)payload_len);

    payload_ptr = &out_buf[OLNK_HEADER_SIZE];
    switch (frame->frame_type) {
        case OLNK_FRAME_CMD_INIT:
            olnk_write_u16_le(&payload_ptr[0], frame->payload.init.client_version);
            payload_ptr[2] = frame->payload.init.drive_count;
            break;
        case OLNK_FRAME_CMD_READ_SECTOR:
            payload_ptr[0] = frame->payload.read_sector.drive_index;
            olnk_write_u32_le(&payload_ptr[1], frame->payload.read_sector.start_sector);
            olnk_write_u16_le(&payload_ptr[5], frame->payload.read_sector.sector_count);
            break;
        case OLNK_FRAME_CMD_WRITE_SECTOR:
            payload_ptr[0] = frame->payload.write_sector.drive_index;
            olnk_write_u32_le(&payload_ptr[1], frame->payload.write_sector.start_sector);
            olnk_write_u16_le(&payload_ptr[5], frame->payload.write_sector.sector_count);
            if (payload_len > 7U) {
                memcpy(&payload_ptr[7], frame->payload.write_sector.data, payload_len - 7U);
            }
            break;
        case OLNK_FRAME_RESP_INIT_ACK:
            olnk_write_u16_le(&payload_ptr[0], frame->payload.init_ack.server_version);
            payload_ptr[2] = frame->payload.init_ack.drive_count;
            payload_ptr[3] = frame->payload.init_ack.status;
            break;
        case OLNK_FRAME_RESP_READ_DATA:
            payload_ptr[0] = frame->payload.read_data.drive_index;
            olnk_write_u32_le(&payload_ptr[1], frame->payload.read_data.start_sector);
            if (frame->payload.read_data.data_len > 0U) {
                memcpy(&payload_ptr[5], frame->payload.read_data.data, frame->payload.read_data.data_len);
            }
            break;
        case OLNK_FRAME_RESP_ERROR:
            payload_ptr[0] = frame->payload.error.error_code;
            olnk_write_u16_le(&payload_ptr[1], frame->payload.error.detail);
            break;
        default:
            return OLNK_ERR_INVALID_FRAME;
    }

    /* CRC16 covers frame_type .. end of payload (bytes [2 .. 6 + payload_len)) */
    crc = olnk_crc16_ccitt(&out_buf[2], 4U + payload_len);
    olnk_write_u16_le(&out_buf[OLNK_HEADER_SIZE + payload_len], crc);

    *out_len = total_len;
    return OLNK_OK;
}

olnk_status_t olnk_frame_decode(
    const uint8_t *raw,
    size_t raw_len,
    olnk_frame_t *out_frame
) {
    uint16_t payload_len;
    size_t expected_len;
    uint16_t wire_crc;
    uint16_t calc_crc;
    const uint8_t *payload_ptr;

    if ((raw == NULL) || (out_frame == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    if (raw_len < OLNK_FRAME_OVERHEAD) {
        return OLNK_ERR_NEED_DATA;
    }

    if ((raw[0] != OLNK_SYNC_BYTE_0) || (raw[1] != OLNK_SYNC_BYTE_1)) {
        return OLNK_ERR_INVALID_FRAME;
    }

    payload_len = olnk_read_u16_le(&raw[4]);
    if (payload_len > OLNK_MAX_PAYLOAD_SIZE) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }

    expected_len = OLNK_HEADER_SIZE + (size_t)payload_len + OLNK_CHECKSUM_SIZE;
    if (raw_len < expected_len) {
        return OLNK_ERR_NEED_DATA;
    }

    wire_crc = olnk_read_u16_le(&raw[OLNK_HEADER_SIZE + payload_len]);
    calc_crc = olnk_crc16_ccitt(&raw[2], 4U + (size_t)payload_len);
    if (wire_crc != calc_crc) {
        return OLNK_ERR_CHECKSUM_MISMATCH;
    }

    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->frame_type = raw[2];
    out_frame->sequence_number = raw[3];
    out_frame->payload_length = payload_len;
    out_frame->checksum = wire_crc;

    payload_ptr = &raw[OLNK_HEADER_SIZE];
    switch (out_frame->frame_type) {
        case OLNK_FRAME_CMD_INIT:
            if (payload_len != 3U) {
                return OLNK_ERR_INVALID_FRAME;
            }
            out_frame->payload.init.client_version = olnk_read_u16_le(&payload_ptr[0]);
            out_frame->payload.init.drive_count = payload_ptr[2];
            break;

        case OLNK_FRAME_CMD_READ_SECTOR:
            if (payload_len != 7U) {
                return OLNK_ERR_INVALID_FRAME;
            }
            out_frame->payload.read_sector.drive_index = payload_ptr[0];
            out_frame->payload.read_sector.start_sector = olnk_read_u32_le(&payload_ptr[1]);
            out_frame->payload.read_sector.sector_count = olnk_read_u16_le(&payload_ptr[5]);
            break;

        case OLNK_FRAME_CMD_WRITE_SECTOR: {
            uint16_t sector_count;
            size_t data_bytes;
            if (payload_len < 7U) {
                return OLNK_ERR_INVALID_FRAME;
            }
            sector_count = olnk_read_u16_le(&payload_ptr[5]);
            data_bytes = (size_t)payload_len - 7U;
            if (data_bytes != ((size_t)sector_count * OLNK_SECTOR_SIZE)) {
                return OLNK_ERR_INVALID_FRAME;
            }
            out_frame->payload.write_sector.drive_index = payload_ptr[0];
            out_frame->payload.write_sector.start_sector = olnk_read_u32_le(&payload_ptr[1]);
            out_frame->payload.write_sector.sector_count = sector_count;
            out_frame->payload.write_sector.data = (data_bytes > 0U) ? &payload_ptr[7] : NULL;
            out_frame->payload.write_sector.data_len = data_bytes;
            break;
        }

        case OLNK_FRAME_RESP_INIT_ACK:
            if (payload_len != 4U) {
                return OLNK_ERR_INVALID_FRAME;
            }
            out_frame->payload.init_ack.server_version = olnk_read_u16_le(&payload_ptr[0]);
            out_frame->payload.init_ack.drive_count = payload_ptr[2];
            out_frame->payload.init_ack.status = payload_ptr[3];
            break;

        case OLNK_FRAME_RESP_READ_DATA:
            if (payload_len < 5U) {
                return OLNK_ERR_INVALID_FRAME;
            }
            out_frame->payload.read_data.drive_index = payload_ptr[0];
            out_frame->payload.read_data.start_sector = olnk_read_u32_le(&payload_ptr[1]);
            out_frame->payload.read_data.data_len = (size_t)payload_len - 5U;
            out_frame->payload.read_data.data =
                (out_frame->payload.read_data.data_len > 0U) ? &payload_ptr[5] : NULL;
            break;

        case OLNK_FRAME_RESP_ERROR:
            if (payload_len != 3U) {
                return OLNK_ERR_INVALID_FRAME;
            }
            out_frame->payload.error.error_code = payload_ptr[0];
            out_frame->payload.error.detail = olnk_read_u16_le(&payload_ptr[1]);
            break;

        default:
            return OLNK_ERR_INVALID_FRAME;
    }

    return OLNK_OK;
}
