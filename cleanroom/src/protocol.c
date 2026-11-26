/* cleanroom/src/protocol.c */
#include "openlnk/protocol.h"
#include "openlnk/crc16.h"

#include <string.h>

const uint8_t olnk_baud_test_tx[OLNK_BAUD_TEST_LEN] = {
    0xFFU, 0x00U, 0x5AU, 0x55U, 0xAAU,
    0xF0U, 0x0FU, 0xE7U, 0x7EU, 0xC3U,
    0x3CU, 0x81U, 0x18U, 0x00U, 0xFFU
};

const uint8_t olnk_baud_test_rx[OLNK_BAUD_TEST_LEN] = {
    0x00U, 0xFFU, 0xA5U, 0xAAU, 0x55U,
    0xF0U, 0x0FU, 0xFFU, 0x00U, 0x18U,
    0x81U, 0x3CU, 0xC3U, 0x7EU, 0xE7U
};

static const uint32_t g_baud_bps_table[OLNK_BAUD_INDEX_COUNT] = {
    1200UL, 2400UL, 4800UL, 9600UL,
    19200UL, 38400UL, 57600UL, 115200UL
};

static const uint16_t g_baud_divisor_table[OLNK_BAUD_INDEX_COUNT] = {
    96U, 48U, 24U, 12U, 6U, 3U, 2U, 1U
};

/* =========================================================================
 * Endianness Helpers
 * ========================================================================= */

uint16_t olnk_read_u16_le(const uint8_t *buf) {
    return (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8U));
}

uint16_t olnk_read_u16_be(const uint8_t *buf) {
    return (uint16_t)(((uint16_t)buf[0] << 8U) | (uint16_t)buf[1]);
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

void olnk_write_u16_be(uint8_t *buf, uint16_t value) {
    buf[0] = (uint8_t)((value >> 8U) & 0xFFU);
    buf[1] = (uint8_t)(value & 0xFFU);
}

void olnk_write_u32_le(uint8_t *buf, uint32_t value) {
    buf[0] = (uint8_t)(value & 0xFFU);
    buf[1] = (uint8_t)((value >> 8U) & 0xFFU);
    buf[2] = (uint8_t)((value >> 16U) & 0xFFU);
    buf[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

/* =========================================================================
 * Link-Layer Header, Control Token & Baud Helpers
 * ========================================================================= */

uint8_t olnk_hdr_make(bool is_short_len, bool more_fragments, bool echo_request, uint8_t seq) {
    uint8_t hdr = (uint8_t)(seq & OLNK_HDR_SEQ_MASK);
    if (is_short_len) {
        hdr = (uint8_t)(hdr | OLNK_HDR_SHORT_LEN);
    }
    if (more_fragments) {
        hdr = (uint8_t)(hdr | OLNK_HDR_MORE_FRAGMENTS);
    }
    if (echo_request) {
        hdr = (uint8_t)(hdr | OLNK_HDR_ECHO_REQUEST);
    }
    return hdr;
}

uint8_t olnk_hdr_compute_ack(uint8_t hdr, bool local_requires_echo) {
    uint8_t echo_bit = (uint8_t)((hdr & OLNK_HDR_ECHO_REQUEST) |
                                 (local_requires_echo ? OLNK_HDR_ECHO_REQUEST : 0x00U));
    return (uint8_t)(((uint8_t)(~hdr) & 0xFBU) | echo_bit);
}

bool olnk_hdr_verify_ack(uint8_t hdr, uint8_t ack_hdr, bool *out_echo_mode) {
    if ((ack_hdr & 0xFBU) != ((uint8_t)(~hdr) & 0xFBU)) {
        return false;
    }
    if (out_echo_mode != NULL) {
        *out_echo_mode = ((ack_hdr & OLNK_HDR_ECHO_REQUEST) != 0U);
    }
    return true;
}

uint8_t olnk_keepalive_byte(uint8_t expected_seq) {
    return (uint8_t)(0xFCU | ((expected_seq & OLNK_HDR_SEQ_MASK) ^ 0x01U));
}

uint8_t olnk_turnaround_byte(uint8_t next_seq) {
    return (uint8_t)(next_seq & OLNK_HDR_SEQ_MASK);
}

uint8_t olnk_baud_req_byte(uint8_t seq) {
    return (uint8_t)(~(seq & OLNK_HDR_SEQ_MASK));
}

uint8_t olnk_baud_beacon_byte(uint8_t baud_idx, uint8_t seq) {
    return (uint8_t)((uint8_t)((baud_idx & 0x0FU) << 4U) | (seq & OLNK_HDR_SEQ_MASK));
}

uint32_t olnk_baud_index_to_bps(uint8_t baud_idx) {
    if (baud_idx >= OLNK_BAUD_INDEX_COUNT) {
        return 0UL;
    }
    return g_baud_bps_table[baud_idx];
}

uint16_t olnk_baud_index_to_divisor(uint8_t baud_idx) {
    if (baud_idx >= OLNK_BAUD_INDEX_COUNT) {
        return 0U;
    }
    return g_baud_divisor_table[baud_idx];
}

bool olnk_bps_to_baud_index(uint32_t bps, uint8_t *out_idx) {
    uint8_t i;
    for (i = 0U; i < OLNK_BAUD_INDEX_COUNT; ++i) {
        if (g_baud_bps_table[i] == bps) {
            if (out_idx != NULL) {
                *out_idx = i;
            }
            return true;
        }
    }
    return false;
}

/* =========================================================================
 * Canonical Wire-Frame Encode / Decode
 * ========================================================================= */

olnk_status_t olnk_wire_frame_encode(
    uint8_t seq,
    bool more_fragments,
    bool echo_request,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out_buf,
    size_t out_cap,
    size_t *out_len
) {
    bool is_short;
    size_t len_field_sz;
    size_t total_len;
    uint16_t crc;

    if ((payload == NULL) || (out_buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (payload_len == 0U) {
        return OLNK_ERR_INVALID_FRAME;
    }
    if (payload_len > OLNK_MAX_PAYLOAD_SIZE) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }

    is_short = (payload_len <= 255U);
    len_field_sz = is_short ? 1U : 2U;
    total_len = 1U + len_field_sz + (size_t)payload_len + 2U;
    if (total_len > out_cap) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }

    out_buf[0] = olnk_hdr_make(is_short, more_fragments, echo_request, seq);
    if (is_short) {
        out_buf[1] = (uint8_t)payload_len;
    } else {
        olnk_write_u16_le(&out_buf[1], payload_len);
    }

    memcpy(&out_buf[1U + len_field_sz], payload, (size_t)payload_len);

    /* CRC-16/GENIBUS covers [length_bytes || payload], stored big-endian */
    crc = olnk_crc16_genibus(&out_buf[1], len_field_sz + (size_t)payload_len);
    olnk_write_u16_be(&out_buf[1U + len_field_sz + (size_t)payload_len], crc);

    *out_len = total_len;
    return OLNK_OK;
}

olnk_status_t olnk_wire_frame_decode(
    const uint8_t *raw,
    size_t raw_len,
    olnk_wire_frame_t *out_frame,
    size_t *out_consumed
) {
    uint8_t hdr;
    bool is_short;
    size_t len_field_sz;
    uint16_t payload_len;
    size_t total_len;
    uint16_t wire_crc;

    if ((raw == NULL) || (out_frame == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (out_consumed != NULL) {
        *out_consumed = 0U;
    }
    if (raw_len < 1U) {
        return OLNK_ERR_NEED_DATA;
    }

    hdr = raw[0];
    if ((hdr & OLNK_HDR_RESERVED_MASK) != 0U) {
        return OLNK_ERR_INVALID_FRAME;
    }

    is_short = ((hdr & OLNK_HDR_SHORT_LEN) != 0U);
    len_field_sz = is_short ? 1U : 2U;
    if (raw_len < (1U + len_field_sz)) {
        return OLNK_ERR_NEED_DATA;
    }

    if (is_short) {
        payload_len = (uint16_t)raw[1];
    } else {
        payload_len = olnk_read_u16_le(&raw[1]);
    }

    if (payload_len == 0U) {
        return OLNK_ERR_INVALID_FRAME;
    }
    if (payload_len > OLNK_MAX_PAYLOAD_SIZE) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }

    total_len = 1U + len_field_sz + (size_t)payload_len + 2U;
    if (raw_len < total_len) {
        return OLNK_ERR_NEED_DATA;
    }

    /* Check CRC-16/GENIBUS via CCITT-FALSE residue == 0x1D0F */
    if (!olnk_crc16_check_residue(&raw[1], len_field_sz + (size_t)payload_len + 2U)) {
        return OLNK_ERR_CHECKSUM_MISMATCH;
    }

    wire_crc = olnk_read_u16_be(&raw[1U + len_field_sz + (size_t)payload_len]);

    out_frame->header = hdr;
    out_frame->is_short_len = is_short;
    out_frame->more_fragments = ((hdr & OLNK_HDR_MORE_FRAGMENTS) != 0U);
    out_frame->echo_request = ((hdr & OLNK_HDR_ECHO_REQUEST) != 0U);
    out_frame->sequence_number = (uint8_t)(hdr & OLNK_HDR_SEQ_MASK);
    out_frame->payload_length = payload_len;
    out_frame->payload = &raw[1U + len_field_sz];
    out_frame->crc16 = wire_crc;

    if (out_consumed != NULL) {
        *out_consumed = total_len;
    }
    return OLNK_OK;
}

/* =========================================================================
 * RPC Message Encoders & Decoders
 * ========================================================================= */

olnk_status_t olnk_encode_sync_trigger(const olnk_sync_trigger_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_SYNC_TRIGGER) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    buf[0] = msg->opcode;
    buf[1] = msg->subcode;
    *out_len = OLNK_LEN_SYNC_TRIGGER;
    return OLNK_OK;
}

olnk_status_t olnk_decode_sync_trigger(const uint8_t *buf, size_t len, olnk_sync_trigger_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_SYNC_TRIGGER) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->opcode = buf[0];
    out->subcode = buf[1];
    return OLNK_OK;
}

olnk_status_t olnk_encode_session_sync_req(const olnk_session_sync_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_SESSION_SYNC_REQ) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    memset(buf, 0, OLNK_LEN_SESSION_SYNC_REQ);
    olnk_write_u16_le(&buf[6], msg->client_version);
    buf[8] = msg->proto_major;
    buf[9] = msg->ext_info_flag;
    buf[10] = 0U;
    buf[11] = msg->capability;
    olnk_write_u16_le(&buf[12], msg->max_block_size);
    olnk_write_u32_le(&buf[26], msg->client_id);
    olnk_write_u32_le(&buf[30], msg->server_id);
    *out_len = OLNK_LEN_SESSION_SYNC_REQ;
    return OLNK_OK;
}

olnk_status_t olnk_decode_session_sync_req(const uint8_t *buf, size_t len, olnk_session_sync_req_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_SESSION_SYNC_REQ) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->client_version = olnk_read_u16_le(&buf[6]);
    out->proto_major = buf[8];
    out->ext_info_flag = buf[9];
    out->capability = buf[11];
    out->max_block_size = olnk_read_u16_le(&buf[12]);
    out->client_id = olnk_read_u32_le(&buf[26]);
    out->server_id = olnk_read_u32_le(&buf[30]);
    return OLNK_OK;
}

olnk_status_t olnk_encode_session_sync_resp(const olnk_session_sync_resp_t *msg, bool ext_info, uint8_t *buf, size_t cap, size_t *out_len) {
    size_t target_len = ext_info ? OLNK_LEN_SESSION_SYNC_EXT : OLNK_LEN_SESSION_SYNC_RESP;
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < target_len) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    memset(buf, 0, target_len);
    olnk_write_u16_le(&buf[6], msg->server_version);
    buf[8] = msg->ready_flag;
    buf[9] = 0U;
    buf[10] = msg->capability;
    olnk_write_u16_le(&buf[11], msg->max_block_size);
    olnk_write_u32_le(&buf[25], msg->prev_client_id);
    olnk_write_u32_le(&buf[29], msg->server_id);
    *out_len = target_len;
    return OLNK_OK;
}

olnk_status_t olnk_decode_session_sync_resp(const uint8_t *buf, size_t len, olnk_session_sync_resp_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len < OLNK_LEN_SESSION_SYNC_RESP) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->server_version = olnk_read_u16_le(&buf[6]);
    out->ready_flag = buf[8];
    out->capability = buf[10];
    out->max_block_size = olnk_read_u16_le(&buf[11]);
    out->prev_client_id = olnk_read_u32_le(&buf[25]);
    out->server_id = olnk_read_u32_le(&buf[29]);
    return OLNK_OK;
}

olnk_status_t olnk_encode_map_config_req(const olnk_map_config_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_MAP_CONFIG_REQ) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    buf[0] = msg->opcode;
    olnk_write_u16_le(&buf[1], msg->client_proto_ver);
    buf[3] = msg->client_dos_minor;
    buf[4] = msg->client_dos_major;
    buf[5] = msg->max_client_drives;
    buf[6] = msg->first_client_drive;
    memcpy(&buf[7], msg->lpt_map_req, OLNK_MAX_LPTS);
    memcpy(&buf[10], msg->drive_map_req, OLNK_MAX_DRIVES);
    *out_len = OLNK_LEN_MAP_CONFIG_REQ;
    return OLNK_OK;
}

olnk_status_t olnk_decode_map_config_req(const uint8_t *buf, size_t len, olnk_map_config_req_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_MAP_CONFIG_REQ) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->opcode = buf[0];
    out->client_proto_ver = olnk_read_u16_le(&buf[1]);
    out->client_dos_minor = buf[3];
    out->client_dos_major = buf[4];
    out->max_client_drives = buf[5];
    out->first_client_drive = buf[6];
    memcpy(out->lpt_map_req, &buf[7], OLNK_MAX_LPTS);
    memcpy(out->drive_map_req, &buf[10], OLNK_MAX_DRIVES);
    return OLNK_OK;
}

olnk_status_t olnk_encode_map_config_resp(const olnk_map_config_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    size_t i;
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_MAP_CONFIG_RESP) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    buf[0] = msg->server_proto_major;
    buf[1] = msg->server_proto_minor;
    buf[2] = msg->server_dos_minor;
    buf[3] = msg->server_dos_major;
    buf[4] = msg->server_drive_count;
    memcpy(&buf[5], msg->lpt_map_resp, OLNK_MAX_LPTS);
    memcpy(&buf[8], msg->drive_map_resp, OLNK_MAX_DRIVES);
    for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
        olnk_write_u16_le(&buf[34U + i * 2U], msg->server_drive_attrs[i]);
    }
    buf[86] = msg->server_flags;
    *out_len = OLNK_LEN_MAP_CONFIG_RESP;
    return OLNK_OK;
}

olnk_status_t olnk_decode_map_config_resp(const uint8_t *buf, size_t len, olnk_map_config_resp_t *out) {
    size_t i;
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_MAP_CONFIG_RESP) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->server_proto_major = buf[0];
    out->server_proto_minor = buf[1];
    out->server_dos_minor = buf[2];
    out->server_dos_major = buf[3];
    out->server_drive_count = buf[4];
    memcpy(out->lpt_map_resp, &buf[5], OLNK_MAX_LPTS);
    memcpy(out->drive_map_resp, &buf[8], OLNK_MAX_DRIVES);
    for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
        out->server_drive_attrs[i] = olnk_read_u16_le(&buf[34U + i * 2U]);
    }
    out->server_flags = buf[86];
    return OLNK_OK;
}

olnk_status_t olnk_encode_media_check_req(const olnk_media_check_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_MEDIA_CHECK_REQ) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    buf[0] = msg->opcode;
    buf[1] = msg->unit;
    buf[2] = msg->media_descriptor;
    *out_len = OLNK_LEN_MEDIA_CHECK_REQ;
    return OLNK_OK;
}

olnk_status_t olnk_decode_media_check_req(const uint8_t *buf, size_t len, olnk_media_check_req_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_MEDIA_CHECK_REQ) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->opcode = buf[0];
    out->unit = buf[1];
    out->media_descriptor = buf[2];
    return OLNK_OK;
}

olnk_status_t olnk_encode_media_check_resp(const olnk_media_check_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_MEDIA_CHECK_RESP) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    memset(buf, 0, OLNK_LEN_MEDIA_CHECK_RESP);
    olnk_write_u16_le(&buf[0], msg->status);
    buf[2] = msg->media_status;
    memcpy(&buf[3], msg->volume_label, 11U);
    buf[14] = 0U;
    olnk_write_u32_le(&buf[15], msg->volume_serial);
    *out_len = OLNK_LEN_MEDIA_CHECK_RESP;
    return OLNK_OK;
}

olnk_status_t olnk_decode_media_check_resp(const uint8_t *buf, size_t len, olnk_media_check_resp_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_MEDIA_CHECK_RESP) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->status = olnk_read_u16_le(&buf[0]);
    out->media_status = buf[2];
    memcpy(out->volume_label, &buf[3], 12U);
    out->volume_label[11] = '\0';
    out->volume_serial = olnk_read_u32_le(&buf[15]);
    return OLNK_OK;
}

olnk_status_t olnk_encode_dos_bpb(const olnk_dos_bpb_t *bpb, uint8_t *buf, size_t cap) {
    if ((bpb == NULL) || (buf == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_DOS_BPB) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    olnk_write_u16_le(&buf[0], bpb->bytes_per_sector);
    buf[2] = bpb->sectors_per_cluster;
    olnk_write_u16_le(&buf[3], bpb->reserved_sectors);
    buf[5] = bpb->num_fats;
    olnk_write_u16_le(&buf[6], bpb->root_entries);
    olnk_write_u16_le(&buf[8], bpb->total_sectors_16);
    buf[10] = bpb->media_descriptor;
    olnk_write_u16_le(&buf[11], bpb->sectors_per_fat);
    olnk_write_u16_le(&buf[13], bpb->sectors_per_track);
    olnk_write_u16_le(&buf[15], bpb->num_heads);
    olnk_write_u32_le(&buf[17], bpb->hidden_sectors);
    olnk_write_u32_le(&buf[21], bpb->total_sectors_32);
    return OLNK_OK;
}

olnk_status_t olnk_decode_dos_bpb(const uint8_t *buf, size_t len, olnk_dos_bpb_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len < OLNK_LEN_DOS_BPB) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->bytes_per_sector = olnk_read_u16_le(&buf[0]);
    out->sectors_per_cluster = buf[2];
    out->reserved_sectors = olnk_read_u16_le(&buf[3]);
    out->num_fats = buf[5];
    out->root_entries = olnk_read_u16_le(&buf[6]);
    out->total_sectors_16 = olnk_read_u16_le(&buf[8]);
    out->media_descriptor = buf[10];
    out->sectors_per_fat = olnk_read_u16_le(&buf[11]);
    out->sectors_per_track = olnk_read_u16_le(&buf[13]);
    out->num_heads = olnk_read_u16_le(&buf[15]);
    out->hidden_sectors = olnk_read_u32_le(&buf[17]);
    out->total_sectors_32 = olnk_read_u32_le(&buf[21]);
    return OLNK_OK;
}

olnk_status_t olnk_encode_build_bpb_req(const olnk_build_bpb_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_BUILD_BPB_REQ) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    buf[0] = msg->opcode;
    buf[1] = msg->unit;
    buf[2] = msg->media_descriptor;
    *out_len = OLNK_LEN_BUILD_BPB_REQ;
    return OLNK_OK;
}

olnk_status_t olnk_decode_build_bpb_req(const uint8_t *buf, size_t len, olnk_build_bpb_req_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_BUILD_BPB_REQ) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->opcode = buf[0];
    out->unit = buf[1];
    out->media_descriptor = buf[2];
    return OLNK_OK;
}

olnk_status_t olnk_encode_build_bpb_resp(const olnk_build_bpb_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (((msg->status & OLNK_DOS_STATUS_ERROR) != 0U) || (!msg->has_bpb)) {
        if (cap < OLNK_LEN_BUILD_BPB_RESP_ERR) {
            return OLNK_ERR_BUFFER_OVERFLOW;
        }
        olnk_write_u16_le(&buf[0], msg->status);
        *out_len = OLNK_LEN_BUILD_BPB_RESP_ERR;
        return OLNK_OK;
    }
    if (cap < OLNK_LEN_BUILD_BPB_RESP_OK) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    olnk_write_u16_le(&buf[0], msg->status);
    (void)olnk_encode_dos_bpb(&msg->bpb, &buf[2], OLNK_LEN_DOS_BPB);
    memcpy(&buf[27], msg->bpb_ext, 6U);
    *out_len = OLNK_LEN_BUILD_BPB_RESP_OK;
    return OLNK_OK;
}

olnk_status_t olnk_decode_build_bpb_resp(const uint8_t *buf, size_t len, olnk_build_bpb_resp_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len < OLNK_LEN_BUILD_BPB_RESP_ERR) {
        return OLNK_ERR_INVALID_FRAME;
    }
    memset(out, 0, sizeof(*out));
    out->status = olnk_read_u16_le(&buf[0]);
    if (((out->status & OLNK_DOS_STATUS_ERROR) == 0U) && (len >= (2U + OLNK_LEN_DOS_BPB))) {
        out->has_bpb = true;
        (void)olnk_decode_dos_bpb(&buf[2], OLNK_LEN_DOS_BPB, &out->bpb);
        if (len >= OLNK_LEN_BUILD_BPB_RESP_OK) {
            memcpy(out->bpb_ext, &buf[27], 6U);
        }
    }
    return OLNK_OK;
}

uint32_t olnk_sector_io_effective_lba(const olnk_sector_io_req_t *req) {
    if (req == NULL) {
        return 0UL;
    }
    if (req->start_sector_16 == 0xFFFFU) {
        return req->start_sector_32;
    }
    return (uint32_t)req->start_sector_16;
}

olnk_status_t olnk_encode_sector_io_req(const olnk_sector_io_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_SECTOR_IO_REQ) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    buf[0] = msg->opcode;
    buf[1] = msg->unit;
    buf[2] = msg->media_descriptor;
    olnk_write_u16_le(&buf[3], msg->sector_count);
    olnk_write_u16_le(&buf[5], msg->start_sector_16);
    olnk_write_u32_le(&buf[7], msg->start_sector_32);
    *out_len = OLNK_LEN_SECTOR_IO_REQ;
    return OLNK_OK;
}

olnk_status_t olnk_decode_sector_io_req(const uint8_t *buf, size_t len, olnk_sector_io_req_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_SECTOR_IO_REQ) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->opcode = buf[0];
    out->unit = buf[1];
    out->media_descriptor = buf[2];
    out->sector_count = olnk_read_u16_le(&buf[3]);
    out->start_sector_16 = olnk_read_u16_le(&buf[5]);
    out->start_sector_32 = olnk_read_u32_le(&buf[7]);
    return OLNK_OK;
}

olnk_status_t olnk_encode_sector_io_resp(const olnk_sector_io_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    size_t target_len;
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    target_len = msg->has_volume_id ? OLNK_LEN_SECTOR_IO_RESP_CHG : OLNK_LEN_SECTOR_IO_RESP;
    if (cap < target_len) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    olnk_write_u16_le(&buf[0], msg->status);
    olnk_write_u16_le(&buf[2], msg->sectors_transferred);
    if (msg->has_volume_id) {
        memset(&buf[4], 0, 12U);
        memcpy(&buf[4], msg->volume_label, 11U);
        olnk_write_u32_le(&buf[16], msg->volume_serial);
    }
    *out_len = target_len;
    return OLNK_OK;
}

olnk_status_t olnk_decode_sector_io_resp(const uint8_t *buf, size_t len, olnk_sector_io_resp_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len < OLNK_LEN_SECTOR_IO_RESP) {
        return OLNK_ERR_INVALID_FRAME;
    }
    memset(out, 0, sizeof(*out));
    out->status = olnk_read_u16_le(&buf[0]);
    out->sectors_transferred = olnk_read_u16_le(&buf[2]);
    if (len >= OLNK_LEN_SECTOR_IO_RESP_CHG) {
        out->has_volume_id = true;
        memcpy(out->volume_label, &buf[4], 12U);
        out->volume_label[11] = '\0';
        out->volume_serial = olnk_read_u32_le(&buf[16]);
    }
    return OLNK_OK;
}

olnk_status_t olnk_encode_drive_incompat(const olnk_drive_incompat_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_DRIVE_INCOMPAT) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    buf[0] = msg->opcode;
    buf[1] = msg->flag;
    buf[2] = msg->client_unit;
    buf[3] = msg->reason;
    buf[4] = msg->server_unit;
    *out_len = OLNK_LEN_DRIVE_INCOMPAT;
    return OLNK_OK;
}

olnk_status_t olnk_decode_drive_incompat(const uint8_t *buf, size_t len, olnk_drive_incompat_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_DRIVE_INCOMPAT) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->opcode = buf[0];
    out->flag = buf[1];
    out->client_unit = buf[2];
    out->reason = buf[3];
    out->server_unit = buf[4];
    return OLNK_OK;
}

olnk_status_t olnk_encode_lpt_write_req(const olnk_lpt_write_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_LPT_WRITE_REQ) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    buf[0] = msg->opcode;
    buf[1] = msg->lpt_index;
    olnk_write_u16_le(&buf[2], msg->byte_count);
    buf[4] = msg->inline_byte;
    *out_len = OLNK_LEN_LPT_WRITE_REQ;
    return OLNK_OK;
}

olnk_status_t olnk_decode_lpt_write_req(const uint8_t *buf, size_t len, olnk_lpt_write_req_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_LPT_WRITE_REQ) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->opcode = buf[0];
    out->lpt_index = buf[1];
    out->byte_count = olnk_read_u16_le(&buf[2]);
    out->inline_byte = buf[4];
    return OLNK_OK;
}

olnk_status_t olnk_encode_lpt_write_resp(const olnk_lpt_write_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_LPT_WRITE_RESP) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    olnk_write_u16_le(&buf[0], msg->status);
    olnk_write_u16_le(&buf[2], msg->bytes_written);
    *out_len = OLNK_LEN_LPT_WRITE_RESP;
    return OLNK_OK;
}

olnk_status_t olnk_decode_lpt_write_resp(const uint8_t *buf, size_t len, olnk_lpt_write_resp_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_LPT_WRITE_RESP) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->status = olnk_read_u16_le(&buf[0]);
    out->bytes_written = olnk_read_u16_le(&buf[2]);
    return OLNK_OK;
}

olnk_status_t olnk_encode_unit_ctrl_req(const olnk_unit_ctrl_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_UNIT_CTRL_REQ) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    buf[0] = msg->opcode;
    buf[1] = msg->unit;
    *out_len = OLNK_LEN_UNIT_CTRL_REQ;
    return OLNK_OK;
}

olnk_status_t olnk_decode_unit_ctrl_req(const uint8_t *buf, size_t len, olnk_unit_ctrl_req_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_UNIT_CTRL_REQ) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->opcode = buf[0];
    out->unit = buf[1];
    return OLNK_OK;
}

olnk_status_t olnk_encode_status_only_resp(uint16_t status, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_STATUS_ONLY_RESP) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    olnk_write_u16_le(&buf[0], status);
    *out_len = OLNK_LEN_STATUS_ONLY_RESP;
    return OLNK_OK;
}

olnk_status_t olnk_decode_status_only_resp(const uint8_t *buf, size_t len, uint16_t *out_status) {
    if ((buf == NULL) || (out_status == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_STATUS_ONLY_RESP) {
        return OLNK_ERR_INVALID_FRAME;
    }
    *out_status = olnk_read_u16_le(&buf[0]);
    return OLNK_OK;
}

olnk_status_t olnk_encode_get_drive_info_req(const olnk_get_drive_info_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_GET_DRIVE_INFO_REQ) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    buf[0] = msg->opcode;
    buf[1] = msg->unit;
    *out_len = OLNK_LEN_GET_DRIVE_INFO_REQ;
    return OLNK_OK;
}

olnk_status_t olnk_decode_get_drive_info_req(const uint8_t *buf, size_t len, olnk_get_drive_info_req_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_GET_DRIVE_INFO_REQ) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->opcode = buf[0];
    out->unit = buf[1];
    return OLNK_OK;
}

olnk_status_t olnk_encode_get_drive_info_resp(const olnk_get_drive_info_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_GET_DRIVE_INFO_RSP) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    memset(buf, 0, OLNK_LEN_GET_DRIVE_INFO_RSP);
    memcpy(&buf[0], msg->size_string, 9U);
    buf[9] = 0U;
    memcpy(&buf[10], msg->volume_label, 12U);
    buf[22] = 0U;
    *out_len = OLNK_LEN_GET_DRIVE_INFO_RSP;
    return OLNK_OK;
}

olnk_status_t olnk_decode_get_drive_info_resp(const uint8_t *buf, size_t len, olnk_get_drive_info_resp_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_GET_DRIVE_INFO_RSP) {
        return OLNK_ERR_INVALID_FRAME;
    }
    memcpy(out->size_string, &buf[0], 10U);
    out->size_string[9] = '\0';
    memcpy(out->volume_label, &buf[10], 13U);
    out->volume_label[12] = '\0';
    return OLNK_OK;
}

olnk_status_t olnk_encode_generic_ioctl_req(const olnk_generic_ioctl_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_GENERIC_IOCTL_REQ) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    buf[0] = msg->opcode;
    buf[1] = msg->unit;
    buf[2] = msg->category;
    buf[3] = msg->function;
    *out_len = OLNK_LEN_GENERIC_IOCTL_REQ;
    return OLNK_OK;
}

olnk_status_t olnk_decode_generic_ioctl_req(const uint8_t *buf, size_t len, olnk_generic_ioctl_req_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_GENERIC_IOCTL_REQ) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->opcode = buf[0];
    out->unit = buf[1];
    out->category = buf[2];
    out->function = buf[3];
    return OLNK_OK;
}

olnk_status_t olnk_encode_generic_ioctl_mid(const olnk_generic_ioctl_mid_t *msg, uint8_t *buf, size_t cap, size_t *out_len) {
    if ((msg == NULL) || (buf == NULL) || (out_len == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cap < OLNK_LEN_GENERIC_IOCTL_MID) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }
    olnk_write_u16_le(&buf[0], msg->info_level);
    olnk_write_u32_le(&buf[2], msg->serial_number);
    memcpy(&buf[6], msg->volume_label, 11U);
    memcpy(&buf[17], msg->filesystem_type, 8U);
    *out_len = OLNK_LEN_GENERIC_IOCTL_MID;
    return OLNK_OK;
}

olnk_status_t olnk_decode_generic_ioctl_mid(const uint8_t *buf, size_t len, olnk_generic_ioctl_mid_t *out) {
    if ((buf == NULL) || (out == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (len != OLNK_LEN_GENERIC_IOCTL_MID) {
        return OLNK_ERR_INVALID_FRAME;
    }
    out->info_level = olnk_read_u16_le(&buf[0]);
    out->serial_number = olnk_read_u32_le(&buf[2]);
    memcpy(out->volume_label, &buf[6], 11U);
    memcpy(out->filesystem_type, &buf[17], 8U);
    return OLNK_OK;
}
