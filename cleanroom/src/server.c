/* cleanroom/src/server.c */
#include "openlnk/server.h"
#include "openlnk/crc16.h"

#include <string.h>

#define OLNK_SERVER_DEFAULT_ID 0x4F4C4E4BUL /* "OLNK" */

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

static void srv_emit_event(olnk_server_t *srv, const char *name, uint32_t a1, uint32_t a2) {
    if (srv->on_event != NULL) {
        srv->on_event(name, a1, a2, srv->on_event_user_data);
    }
}

static bool srv_write_all(olnk_server_t *srv, const uint8_t *buf, size_t len) {
    size_t sent = 0U;
    if (srv->link.transport.write == NULL) {
        return false;
    }
    while (sent < len) {
        int n = srv->link.transport.write(
            &buf[sent],
            len - sent,
            srv->link.tx_timeout_ms,
            srv->link.transport.user_data
        );
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static void srv_write_byte(olnk_server_t *srv, uint8_t b) {
    (void)srv_write_all(srv, &b, 1U);
}

static bool srv_set_baud_idx(olnk_server_t *srv, uint8_t baud_idx) {
    uint32_t bps = olnk_baud_index_to_bps(baud_idx);
    if (bps == 0UL) {
        return false;
    }
    if (srv->link.transport.set_baud == NULL) {
        return true;
    }
    return srv->link.transport.set_baud(bps, srv->link.transport.user_data);
}

static uint16_t srv_get_drive_info(olnk_server_t *srv, uint8_t unit, olnk_drive_info_t *info) {
    memset(info, 0, sizeof(*info));
    if (srv->storage.get_drive_info == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    return srv->storage.get_drive_info(unit, info, srv->storage.user_data);
}

/* =========================================================================
 * Phase C Response TX Engine (Zero-Alloc Streaming)
 * ========================================================================= */

static uint8_t srv_tx_stream_byte(const olnk_server_t *srv, uint16_t pos) {
    if (pos < (uint16_t)srv->tx_len_sz) {
        return srv->tx_len_bytes[pos];
    }
    pos = (uint16_t)(pos - (uint16_t)srv->tx_len_sz);
    if (pos < srv->tx_payload_len) {
        return srv->tx_payload_ptr[pos];
    }
    pos = (uint16_t)(pos - srv->tx_payload_len);
    return srv->tx_crc_be[pos & 1U];
}

static void srv_start_tx_frame(
    olnk_server_t *srv,
    const uint8_t *payload,
    uint16_t payload_len,
    bool more_fragments
) {
    bool is_short = (payload_len <= 255U);
    uint16_t crc = OLNK_CRC16_INIT;
    uint16_t i;

    srv->tx_payload_ptr = payload;
    srv->tx_payload_len = payload_len;
    srv->tx_more_frags = more_fragments;
    srv->tx_hdr = olnk_hdr_make(is_short, more_fragments, srv->tx_echo_request, srv->link.seq);

    if (is_short) {
        srv->tx_len_bytes[0] = (uint8_t)payload_len;
        srv->tx_len_sz = 1U;
    } else {
        olnk_write_u16_le(srv->tx_len_bytes, payload_len);
        srv->tx_len_sz = 2U;
    }

    for (i = 0U; i < (uint16_t)srv->tx_len_sz; ++i) {
        crc = olnk_crc16_update(crc, srv->tx_len_bytes[i]);
    }
    for (i = 0U; i < payload_len; ++i) {
        crc = olnk_crc16_update(crc, payload[i]);
    }
    crc = (uint16_t)(crc ^ OLNK_CRC16_XOROUT);
    olnk_write_u16_be(srv->tx_crc_be, crc);

    srv->tx_stream_pos = 0U;
    srv->tx_stream_total = (uint16_t)((uint16_t)srv->tx_len_sz + payload_len + 2U);
    srv->state_elapsed_ms = 0UL;
    srv->srv_state = (uint8_t)OLNK_SRV_TX_WAIT_HDR_ACK;

    srv_write_byte(srv, srv->tx_hdr);
}

static void srv_retry_tx_frame(olnk_server_t *srv) {
    srv->link.frames_tx_retry += 1U;
    if (!srv->tx_echo_mode) {
        /* Burst failed -> force Echo pacing and retry */
        srv->tx_echo_request = true;
        srv->tx_retries = 0U;
        srv_start_tx_frame(srv, srv->tx_payload_ptr, srv->tx_payload_len, srv->tx_more_frags);
        return;
    }
    srv->tx_retries += 1U;
    if (srv->tx_retries < 2U) {
        srv_start_tx_frame(srv, srv->tx_payload_ptr, srv->tx_payload_len, srv->tx_more_frags);
        return;
    }
    /* Unrecoverable TX failure: drop back to 9600 bps sync */
    olnk_server_drop_link(srv);
}

static void srv_finish_response_turnaround(olnk_server_t *srv) {
    srv_write_byte(srv, olnk_turnaround_byte(srv->link.seq));
    srv->resp_phase = (uint8_t)OLNK_RESP_NONE;
    srv->req_phase = (uint8_t)OLNK_REQ_PHASE_CMD;
    srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
    srv->state_elapsed_ms = 0UL;
    srv->idle_elapsed_ms = 0UL;
}

static void srv_send_next_read_chunk(olnk_server_t *srv) {
    uint16_t max_blk = srv->link.max_block_size;
    uint16_t bps = (srv->read_bps != 0U) ? srv->read_bps : OLNK_SECTOR_SIZE;
    uint16_t max_secs_per_frame;
    uint16_t chunk_secs;
    uint16_t chunk_bytes;
    bool more;

    if (srv->read_prefetched_valid) {
        uint16_t len = srv->read_prefetched_len;
        srv->read_prefetched_valid = false;
        more = (srv->read_sectors_remaining > 0U);
        if (!more) {
            srv->resp_phase = (uint8_t)OLNK_RESP_SINGLE;
        }
        srv_start_tx_frame(srv, srv->tx_data_buf, len, more);
        return;
    }

    if (srv->read_sectors_remaining == 0U) {
        srv_finish_response_turnaround(srv);
        return;
    }

    if ((max_blk == 0U) || (max_blk > OLNK_MAX_PAYLOAD_SIZE)) {
        max_blk = (uint16_t)OLNK_MAX_PAYLOAD_SIZE;
    }
    max_secs_per_frame = (uint16_t)(max_blk / bps);
    if (max_secs_per_frame == 0U) {
        max_secs_per_frame = 1U;
    }
    chunk_secs = (srv->read_sectors_remaining > max_secs_per_frame)
                     ? max_secs_per_frame
                     : srv->read_sectors_remaining;
    chunk_bytes = (uint16_t)(chunk_secs * bps);

    memset(srv->tx_data_buf, 0, (size_t)chunk_bytes);
    if (srv->storage.read_sectors != NULL) {
        uint16_t xfer = 0U;
        (void)srv->storage.read_sectors(
            srv->read_unit,
            srv->read_next_lba,
            chunk_secs,
            srv->tx_data_buf,
            &xfer,
            srv->storage.user_data
        );
        srv->sectors_read_total += (uint32_t)xfer;
    }

    srv->read_next_lba += (uint32_t)chunk_secs;
    srv->read_sectors_remaining = (uint16_t)(srv->read_sectors_remaining - chunk_secs);
    more = (srv->read_sectors_remaining > 0U);
    if (!more) {
        srv->resp_phase = (uint8_t)OLNK_RESP_SINGLE;
    }
    srv_start_tx_frame(srv, srv->tx_data_buf, chunk_bytes, more);
}

static void srv_on_tx_frame_acked(olnk_server_t *srv) {
    srv->link.seq = (uint8_t)((srv->link.seq + 1U) & OLNK_HDR_SEQ_MASK);
    srv->link.frames_tx_ok += 1U;
    srv->tx_retries = 0U;
    srv->tx_echo_request = false;

    if (srv->resp_phase == (uint8_t)OLNK_RESP_READ_HDR_THEN_DATA) {
        srv_send_next_read_chunk(srv);
        return;
    }
    if (srv->resp_phase == (uint8_t)OLNK_RESP_IOCTL_HDR_THEN_MID) {
        srv->resp_phase = (uint8_t)OLNK_RESP_SINGLE;
        srv_start_tx_frame(srv, srv->tx_data_buf, OLNK_LEN_GENERIC_IOCTL_MID, false);
        return;
    }

    srv_finish_response_turnaround(srv);
}

static void srv_queue_single_response(olnk_server_t *srv, uint16_t ctrl_len) {
    srv->tx_payload_ptr = srv->tx_ctrl_buf;
    srv->tx_payload_len = ctrl_len;
    srv->tx_more_frags = false;
    srv->resp_phase = (uint8_t)OLNK_RESP_SINGLE;
    srv->req_phase = (uint8_t)OLNK_REQ_PHASE_CMD;
    srv->srv_state = (uint8_t)OLNK_SRV_WAIT_TURNAROUND;
    srv->state_elapsed_ms = 0UL;
}

/* =========================================================================
 * Application-Layer RPC Handlers (0x00 .. 0x1D)
 * ========================================================================= */

static void handle_session_sync_body(olnk_server_t *srv, const uint8_t *buf, uint16_t len) {
    olnk_session_sync_req_t req;
    olnk_session_sync_resp_t resp;
    uint16_t negotiated_max;
    bool resumed;
    size_t out_len = 0U;

    if (olnk_decode_session_sync_req(buf, (size_t)len, &req) != OLNK_OK) {
        size_t slen = 0U;
        (void)olnk_encode_status_only_resp(
            OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_BAD_REQ_LENGTH),
            srv->tx_ctrl_buf,
            sizeof(srv->tx_ctrl_buf),
            &slen
        );
        srv_queue_single_response(srv, (uint16_t)slen);
        return;
    }

    negotiated_max = req.max_block_size;
    if ((negotiated_max == 0U) || (negotiated_max > OLNK_MAX_PAYLOAD_SIZE)) {
        negotiated_max = (uint16_t)OLNK_MAX_PAYLOAD_SIZE;
    }
    srv->link.max_block_size = negotiated_max;

    if (srv->link.server_id == 0UL) {
        srv->link.server_id = OLNK_SERVER_DEFAULT_ID;
    }

    resumed = (req.client_id != 0UL) &&
              (req.client_id == srv->link.prev_client_id) &&
              (req.server_id == srv->link.server_id);

    memset(&resp, 0, sizeof(resp));
    resp.server_version = OLNK_SERVER_VERSION_DOS622;
    resp.ready_flag = 1U;
    resp.capability = 1U;
    resp.max_block_size = negotiated_max;
    resp.prev_client_id = srv->link.prev_client_id;
    resp.server_id = srv->link.server_id;

    srv->link.prev_client_id = req.client_id;
    srv->link.client_id = req.client_id;
    srv->link.session_resumed = resumed;

    srv_emit_event(srv, "SESSION_SYNC", req.client_id, resumed ? 1UL : 0UL);

    (void)olnk_encode_session_sync_resp(
        &resp,
        (req.ext_info_flag == 1U),
        srv->tx_ctrl_buf,
        sizeof(srv->tx_ctrl_buf),
        &out_len
    );
    srv_queue_single_response(srv, (uint16_t)out_len);
}

static void handle_map_config(olnk_server_t *srv, const uint8_t *buf, uint16_t len) {
    olnk_map_config_req_t req;
    olnk_map_config_resp_t resp;
    bool used[OLNK_MAX_DRIVES];
    uint8_t srv_drives = 0U;
    uint8_t i;
    size_t out_len = 0U;

    if (olnk_decode_map_config_req(buf, (size_t)len, &req) != OLNK_OK) {
        (void)olnk_encode_status_only_resp(
            OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_BAD_REQ_LENGTH),
            srv->tx_ctrl_buf,
            sizeof(srv->tx_ctrl_buf),
            &out_len
        );
        srv_queue_single_response(srv, (uint16_t)out_len);
        return;
    }

    memset(&resp, 0, sizeof(resp));
    memset(used, 0, sizeof(used));

    if (srv->storage.drive_count != NULL) {
        srv_drives = srv->storage.drive_count(srv->storage.user_data);
        if (srv_drives > OLNK_MAX_DRIVES) {
            srv_drives = OLNK_MAX_DRIVES;
        }
    }

    resp.server_proto_major = OLNK_SERVER_PROTO_MAJOR;
    resp.server_proto_minor = OLNK_SERVER_PROTO_MINOR;
    resp.server_dos_minor = OLNK_DOS_VER_MINOR;
    resp.server_dos_major = OLNK_DOS_VER_MAJOR;
    resp.server_drive_count = srv_drives;
    resp.server_flags = srv->server_flags;

    for (i = 0U; i < OLNK_MAX_LPTS; ++i) {
        resp.lpt_map_resp[i] = OLNK_MAP_NONE;
    }

    for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
        resp.drive_map_resp[i] = OLNK_MAP_AUTO;
        if (i < srv_drives) {
            olnk_drive_info_t info;
            uint16_t st = srv_get_drive_info(srv, i, &info);
            if (((st & OLNK_DOS_STATUS_ERROR) == 0U) && info.present) {
                resp.server_drive_attrs[i] =
                    (info.drive_attrs != 0U) ? info.drive_attrs : OLNK_DRIVE_ATTR_DEFAULT;
            }
        }
    }

    /* Assign explicit mappings first */
    for (i = 0U; (i < OLNK_MAX_DRIVES) && (i < req.max_client_drives); ++i) {
        uint8_t wanted = req.drive_map_req[i];
        if (wanted == OLNK_MAP_NONE) {
            resp.drive_map_resp[i] = OLNK_MAP_NONE;
        } else if (wanted < srv_drives) {
            olnk_drive_info_t info;
            uint16_t st = srv_get_drive_info(srv, wanted, &info);
            if (((st & OLNK_DOS_STATUS_ERROR) == 0U) && info.present) {
                resp.drive_map_resp[i] = wanted;
                used[wanted] = true;
            } else {
                resp.drive_map_resp[i] = OLNK_MAP_AUTO;
            }
        }
    }

    /* Auto-assign remaining slots marked 0xFE */
    for (i = 0U; (i < OLNK_MAX_DRIVES) && (i < req.max_client_drives); ++i) {
        if (req.drive_map_req[i] == OLNK_MAP_AUTO) {
            uint8_t u;
            for (u = 0U; u < srv_drives; ++u) {
                if (!used[u]) {
                    olnk_drive_info_t info;
                    uint16_t st = srv_get_drive_info(srv, u, &info);
                    if (((st & OLNK_DOS_STATUS_ERROR) == 0U) && info.present) {
                        resp.drive_map_resp[i] = u;
                        used[u] = true;
                        break;
                    }
                }
            }
        }
    }

    srv_emit_event(srv, "MAP_CONFIG", (uint32_t)req.max_client_drives, (uint32_t)srv_drives);

    (void)olnk_encode_map_config_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
    srv_queue_single_response(srv, (uint16_t)out_len);
}

static void handle_media_check(olnk_server_t *srv, const uint8_t *buf, uint16_t len) {
    olnk_media_check_req_t req;
    olnk_media_check_resp_t resp;
    olnk_drive_info_t info;
    uint8_t media_st = (uint8_t)OLNK_MEDIA_UNSURE;
    size_t out_len = 0U;

    memset(&resp, 0, sizeof(resp));
    memset(&info, 0, sizeof(info));

    if (olnk_decode_media_check_req(buf, (size_t)len, &req) != OLNK_OK) {
        resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_BAD_REQ_LENGTH);
    } else if (srv->storage.media_check == NULL) {
        resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    } else {
        resp.status = srv->storage.media_check(
            req.unit,
            req.media_descriptor,
            &media_st,
            &info,
            srv->storage.user_data
        );
        resp.media_status = media_st;
        olnk_label_trim_copy(resp.volume_label, sizeof(resp.volume_label), info.volume_label, 11U);
        resp.volume_serial = info.volume_serial;
    }

    (void)olnk_encode_media_check_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
    srv_queue_single_response(srv, (uint16_t)out_len);
}

static void handle_build_bpb(olnk_server_t *srv, const uint8_t *buf, uint16_t len) {
    olnk_build_bpb_req_t req;
    olnk_build_bpb_resp_t resp;
    size_t out_len = 0U;

    memset(&resp, 0, sizeof(resp));
    if (olnk_decode_build_bpb_req(buf, (size_t)len, &req) != OLNK_OK) {
        resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_BAD_REQ_LENGTH);
        resp.has_bpb = false;
    } else if (srv->storage.build_bpb == NULL) {
        resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
        resp.has_bpb = false;
    } else {
        resp.status = srv->storage.build_bpb(
            req.unit,
            req.media_descriptor,
            &resp.bpb,
            srv->storage.user_data
        );
        resp.has_bpb = ((resp.status & OLNK_DOS_STATUS_ERROR) == 0U);
    }

    (void)olnk_encode_build_bpb_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
    srv_queue_single_response(srv, (uint16_t)out_len);
}

static void handle_read_sectors(olnk_server_t *srv, const uint8_t *buf, uint16_t len) {
    olnk_sector_io_req_t req;
    olnk_sector_io_resp_t resp;
    olnk_drive_info_t info;
    uint32_t lba;
    uint32_t end_lba;
    uint16_t st;
    uint16_t bps;
    uint16_t max_blk;
    uint16_t max_secs_per_frame;
    uint16_t first_secs;
    uint16_t xfer = 0U;
    size_t out_len = 0U;

    memset(&resp, 0, sizeof(resp));

    if (olnk_decode_sector_io_req(buf, (size_t)len, &req) != OLNK_OK) {
        resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_BAD_REQ_LENGTH);
        (void)olnk_encode_sector_io_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
        srv_queue_single_response(srv, (uint16_t)out_len);
        return;
    }

    st = srv_get_drive_info(srv, req.unit, &info);
    if (((st & OLNK_DOS_STATUS_ERROR) != 0U) || (!info.present)) {
        resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
        (void)olnk_encode_sector_io_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
        srv_queue_single_response(srv, (uint16_t)out_len);
        return;
    }
    if (!info.ready) {
        resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
        (void)olnk_encode_sector_io_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
        srv_queue_single_response(srv, (uint16_t)out_len);
        return;
    }
    if ((req.sector_count == 0U) || (srv->storage.read_sectors == NULL)) {
        resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
        (void)olnk_encode_sector_io_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
        srv_queue_single_response(srv, (uint16_t)out_len);
        return;
    }

    lba = olnk_sector_io_effective_lba(&req);
    end_lba = lba + (uint32_t)req.sector_count;
    if ((end_lba < lba) || (end_lba > info.total_sectors)) {
        resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
        (void)olnk_encode_sector_io_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
        srv_queue_single_response(srv, (uint16_t)out_len);
        return;
    }

    bps = (info.bpb.bytes_per_sector != 0U) ? info.bpb.bytes_per_sector : OLNK_SECTOR_SIZE;
    max_blk = srv->link.max_block_size;
    if ((max_blk == 0U) || (max_blk > OLNK_MAX_PAYLOAD_SIZE)) {
        max_blk = (uint16_t)OLNK_MAX_PAYLOAD_SIZE;
    }
    max_secs_per_frame = (uint16_t)(max_blk / bps);
    if (max_secs_per_frame == 0U) {
        max_secs_per_frame = 1U;
    }
    first_secs = (req.sector_count > max_secs_per_frame) ? max_secs_per_frame : req.sector_count;

    /* Pre-read first chunk before committing to status == DONE */
    st = srv->storage.read_sectors(
        req.unit,
        lba,
        first_secs,
        srv->tx_data_buf,
        &xfer,
        srv->storage.user_data
    );
    if ((st & OLNK_DOS_STATUS_ERROR) != 0U) {
        resp.status = st;
        resp.sectors_transferred = 0U;
        if ((st & 0x00FFU) == (uint16_t)OLNK_DOS_ERR_INVALID_DISK_CHG) {
            resp.has_volume_id = true;
            olnk_label_trim_copy(resp.volume_label, sizeof(resp.volume_label), info.volume_label, 11U);
            resp.volume_serial = info.volume_serial;
        }
        (void)olnk_encode_sector_io_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
        srv_queue_single_response(srv, (uint16_t)out_len);
        return;
    }

    srv->sectors_read_total += (uint32_t)xfer;
    srv->read_unit = req.unit;
    srv->read_bps = bps;
    srv->read_next_lba = lba + (uint32_t)first_secs;
    srv->read_sectors_remaining = (uint16_t)(req.sector_count - first_secs);
    srv->read_prefetched_len = (uint16_t)(first_secs * bps);
    srv->read_prefetched_valid = true;

    resp.status = OLNK_DOS_STATUS_DONE;
    resp.sectors_transferred = req.sector_count;
    (void)olnk_encode_sector_io_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);

    srv->tx_payload_ptr = srv->tx_ctrl_buf;
    srv->tx_payload_len = (uint16_t)out_len;
    srv->tx_more_frags = false;
    srv->resp_phase = (uint8_t)OLNK_RESP_READ_HDR_THEN_DATA;
    srv->req_phase = (uint8_t)OLNK_REQ_PHASE_CMD;
    srv->srv_state = (uint8_t)OLNK_SRV_WAIT_TURNAROUND;
    srv->state_elapsed_ms = 0UL;
}

static void handle_write_sectors_header(olnk_server_t *srv, const uint8_t *buf, uint16_t len, bool verify) {
    olnk_sector_io_req_t req;
    olnk_drive_info_t info;
    uint32_t lba;
    uint32_t end_lba;
    uint16_t st;
    uint16_t bps = OLNK_SECTOR_SIZE;

    if (olnk_decode_sector_io_req(buf, (size_t)len, &req) != OLNK_OK) {
        olnk_sector_io_resp_t resp;
        size_t out_len = 0U;
        memset(&resp, 0, sizeof(resp));
        resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_BAD_REQ_LENGTH);
        (void)olnk_encode_sector_io_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
        srv_queue_single_response(srv, (uint16_t)out_len);
        return;
    }

    lba = olnk_sector_io_effective_lba(&req);
    srv->write_unit = req.unit;
    srv->write_verify = verify;
    srv->write_next_lba = lba;
    srv->write_bytes_received = 0UL;
    srv->write_sectors_written = 0U;
    srv->write_spill_len = 0U;
    srv->write_error_status = 0U;

    st = srv_get_drive_info(srv, req.unit, &info);
    if (((st & OLNK_DOS_STATUS_ERROR) != 0U) || (!info.present)) {
        srv->write_error_status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    } else if (!info.ready) {
        srv->write_error_status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    } else {
        if (info.bpb.bytes_per_sector != 0U) {
            bps = info.bpb.bytes_per_sector;
        }
        if (info.write_protected) {
            srv->write_error_status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
        } else if (req.sector_count == 0U) {
            srv->write_error_status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
        } else {
            end_lba = lba + (uint32_t)req.sector_count;
            if ((end_lba < lba) || (end_lba > info.total_sectors)) {
                srv->write_error_status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
            }
        }
    }

    srv->write_bps = bps;
    srv->write_bytes_expected = (uint32_t)req.sector_count * (uint32_t)bps;

    if (req.sector_count == 0U) {
        olnk_sector_io_resp_t resp;
        size_t out_len = 0U;
        memset(&resp, 0, sizeof(resp));
        resp.status = (srv->write_error_status != 0U)
                          ? srv->write_error_status
                          : OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
        (void)olnk_encode_sector_io_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
        srv_queue_single_response(srv, (uint16_t)out_len);
        return;
    }

    /* Expect follow-on sector data frame(s) immediately without turnaround */
    srv->req_phase = (uint8_t)OLNK_REQ_PHASE_WRITE_DATA;
    srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
    srv->state_elapsed_ms = 0UL;
}

static void handle_write_sectors_data(
    olnk_server_t *srv,
    const uint8_t *buf,
    uint16_t len,
    bool more_fragments
) {
    uint16_t bps = (srv->write_bps != 0U) ? srv->write_bps : OLNK_SECTOR_SIZE;
    uint16_t offset = 0U;

    srv->write_bytes_received += (uint32_t)len;

    if ((srv->write_error_status == 0U) && (srv->storage.write_sectors != NULL) &&
        (bps <= OLNK_SECTOR_SIZE)) {
        /* Complete any partial spill sector from a prior unaligned fragment */
        if (srv->write_spill_len > 0U) {
            uint16_t need = (uint16_t)(bps - srv->write_spill_len);
            uint16_t take = (len < need) ? len : need;
            memcpy(&srv->write_spill[srv->write_spill_len], buf, (size_t)take);
            srv->write_spill_len = (uint16_t)(srv->write_spill_len + take);
            offset = (uint16_t)(offset + take);

            if (srv->write_spill_len == bps) {
                uint16_t xfer = 0U;
                uint16_t st = srv->storage.write_sectors(
                    srv->write_unit,
                    srv->write_next_lba,
                    1U,
                    srv->write_spill,
                    &xfer,
                    srv->write_verify,
                    srv->storage.user_data
                );
                srv->write_spill_len = 0U;
                if ((st & OLNK_DOS_STATUS_ERROR) != 0U) {
                    srv->write_error_status = st;
                } else {
                    srv->write_next_lba += 1UL;
                    srv->write_sectors_written += xfer;
                    srv->sectors_written_total += (uint32_t)xfer;
                }
            }
        }

        /* Write whole aligned sectors directly from RX buffer */
        if ((srv->write_error_status == 0U) && (offset < len)) {
            uint16_t avail = (uint16_t)(len - offset);
            uint16_t whole_secs = (uint16_t)(avail / bps);
            if (whole_secs > 0U) {
                uint16_t xfer = 0U;
                uint16_t st = srv->storage.write_sectors(
                    srv->write_unit,
                    srv->write_next_lba,
                    whole_secs,
                    &buf[offset],
                    &xfer,
                    srv->write_verify,
                    srv->storage.user_data
                );
                if ((st & OLNK_DOS_STATUS_ERROR) != 0U) {
                    srv->write_error_status = st;
                } else {
                    srv->write_next_lba += (uint32_t)whole_secs;
                    srv->write_sectors_written = (uint16_t)(srv->write_sectors_written + xfer);
                    srv->sectors_written_total += (uint32_t)xfer;
                }
                offset = (uint16_t)(offset + (uint16_t)(whole_secs * bps));
            }

            /* Save any trailing partial sector */
            if ((srv->write_error_status == 0U) && (offset < len)) {
                uint16_t rem = (uint16_t)(len - offset);
                memcpy(srv->write_spill, &buf[offset], (size_t)rem);
                srv->write_spill_len = rem;
            }
        }
    }

    if (more_fragments && (srv->write_bytes_received < srv->write_bytes_expected)) {
        srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
        srv->state_elapsed_ms = 0UL;
        return;
    }

    /* Final write fragment received: assemble status response */
    {
        olnk_sector_io_resp_t resp;
        size_t out_len = 0U;
        memset(&resp, 0, sizeof(resp));

        if (srv->write_error_status != 0U) {
            resp.status = srv->write_error_status;
            resp.sectors_transferred = 0U;
            if ((resp.status & 0x00FFU) == (uint16_t)OLNK_DOS_ERR_INVALID_DISK_CHG) {
                olnk_drive_info_t info;
                (void)srv_get_drive_info(srv, srv->write_unit, &info);
                resp.has_volume_id = true;
                olnk_label_trim_copy(resp.volume_label, sizeof(resp.volume_label), info.volume_label, 11U);
                resp.volume_serial = info.volume_serial;
            }
        } else if (srv->write_bytes_received != srv->write_bytes_expected) {
            resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
            resp.sectors_transferred = srv->write_sectors_written;
        } else {
            resp.status = OLNK_DOS_STATUS_DONE;
            resp.sectors_transferred = srv->write_sectors_written;
        }

        (void)olnk_encode_sector_io_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
        srv_queue_single_response(srv, (uint16_t)out_len);
    }
}

static void handle_get_drive_info(olnk_server_t *srv, const uint8_t *buf, uint16_t len) {
    olnk_get_drive_info_req_t req;
    olnk_get_drive_info_resp_t resp;
    olnk_drive_info_t info;
    size_t out_len = 0U;

    memset(&resp, 0, sizeof(resp));
    if (olnk_decode_get_drive_info_req(buf, (size_t)len, &req) == OLNK_OK) {
        uint16_t st = srv_get_drive_info(srv, req.unit, &info);
        if (((st & OLNK_DOS_STATUS_ERROR) == 0U) && info.present && info.ready) {
            uint16_t bps = (info.bpb.bytes_per_sector != 0U)
                               ? info.bpb.bytes_per_sector
                               : OLNK_SECTOR_SIZE;
            olnk_fat_format_size_string(info.total_sectors, bps, resp.size_string);
            olnk_label_trim_copy(resp.volume_label, sizeof(resp.volume_label), info.volume_label, 11U);
        }
    }

    (void)olnk_encode_get_drive_info_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
    srv_queue_single_response(srv, (uint16_t)out_len);
}

static void handle_generic_ioctl_header(olnk_server_t *srv, const uint8_t *buf, uint16_t len) {
    olnk_generic_ioctl_req_t req;
    size_t out_len = 0U;

    if (olnk_decode_generic_ioctl_req(buf, (size_t)len, &req) != OLNK_OK) {
        (void)olnk_encode_status_only_resp(
            OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_BAD_REQ_LENGTH),
            srv->tx_ctrl_buf,
            sizeof(srv->tx_ctrl_buf),
            &out_len
        );
        srv_queue_single_response(srv, (uint16_t)out_len);
        return;
    }

    if (req.category != OLNK_IOCTL_CAT_DISK) {
        (void)olnk_encode_status_only_resp(
            OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND),
            srv->tx_ctrl_buf,
            sizeof(srv->tx_ctrl_buf),
            &out_len
        );
        srv_queue_single_response(srv, (uint16_t)out_len);
        return;
    }

    if (req.function == OLNK_IOCTL_FN_GET_MEDIA_ID) {
        olnk_drive_info_t info;
        uint16_t st = srv_get_drive_info(srv, req.unit, &info);
        if (((st & OLNK_DOS_STATUS_ERROR) != 0U) || (!info.present)) {
            (void)olnk_encode_status_only_resp(
                OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT),
                srv->tx_ctrl_buf,
                sizeof(srv->tx_ctrl_buf),
                &out_len
            );
            srv_queue_single_response(srv, (uint16_t)out_len);
            return;
        }
        if (!info.ready) {
            (void)olnk_encode_status_only_resp(
                OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY),
                srv->tx_ctrl_buf,
                sizeof(srv->tx_ctrl_buf),
                &out_len
            );
            srv_queue_single_response(srv, (uint16_t)out_len);
            return;
        }
        {
            olnk_generic_ioctl_mid_t mid;
            size_t mid_len = 0U;
            memset(&mid, 0, sizeof(mid));
            mid.info_level = 0U;
            mid.serial_number = info.volume_serial;
            olnk_label_pad_copy(mid.volume_label, sizeof(mid.volume_label), info.volume_label);
            olnk_label_pad_copy(mid.filesystem_type, sizeof(mid.filesystem_type), info.filesystem_type);
            (void)olnk_encode_generic_ioctl_mid(&mid, srv->tx_data_buf, sizeof(srv->tx_data_buf), &mid_len);
            (void)olnk_encode_status_only_resp(
                OLNK_DOS_STATUS_DONE,
                srv->tx_ctrl_buf,
                sizeof(srv->tx_ctrl_buf),
                &out_len
            );
            srv->tx_payload_ptr = srv->tx_ctrl_buf;
            srv->tx_payload_len = (uint16_t)out_len;
            srv->tx_more_frags = false;
            srv->resp_phase = (uint8_t)OLNK_RESP_IOCTL_HDR_THEN_MID;
            srv->req_phase = (uint8_t)OLNK_REQ_PHASE_CMD;
            srv->srv_state = (uint8_t)OLNK_SRV_WAIT_TURNAROUND;
            srv->state_elapsed_ms = 0UL;
        }
        return;
    }

    if (req.function == OLNK_IOCTL_FN_SET_MEDIA_ID) {
        srv->ioctl_unit = req.unit;
        srv->req_phase = (uint8_t)OLNK_REQ_PHASE_IOCTL_MID;
        srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
        srv->state_elapsed_ms = 0UL;
        return;
    }

    (void)olnk_encode_status_only_resp(
        OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND),
        srv->tx_ctrl_buf,
        sizeof(srv->tx_ctrl_buf),
        &out_len
    );
    srv_queue_single_response(srv, (uint16_t)out_len);
}

static void handle_generic_ioctl_mid(olnk_server_t *srv, const uint8_t *buf, uint16_t len) {
    olnk_generic_ioctl_mid_t mid;
    uint16_t st;
    size_t out_len = 0U;

    if (olnk_decode_generic_ioctl_mid(buf, (size_t)len, &mid) != OLNK_OK) {
        st = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_BAD_REQ_LENGTH);
    } else if (srv->storage.set_media_id == NULL) {
        st = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);
    } else {
        st = srv->storage.set_media_id(srv->ioctl_unit, &mid, srv->storage.user_data);
    }

    (void)olnk_encode_status_only_resp(st, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
    srv_queue_single_response(srv, (uint16_t)out_len);
}

static void handle_rpc_frame(
    olnk_server_t *srv,
    const uint8_t *buf,
    uint16_t len,
    bool more_fragments
) {
    if (srv->req_phase == (uint8_t)OLNK_REQ_PHASE_SYNC_BODY) {
        handle_session_sync_body(srv, buf, len);
        return;
    }
    if (srv->req_phase == (uint8_t)OLNK_REQ_PHASE_WRITE_DATA) {
        handle_write_sectors_data(srv, buf, len, more_fragments);
        return;
    }
    if (srv->req_phase == (uint8_t)OLNK_REQ_PHASE_IOCTL_MID) {
        handle_generic_ioctl_mid(srv, buf, len);
        return;
    }
    if (srv->req_phase == (uint8_t)OLNK_REQ_PHASE_LPT_DATA) {
        srv->lpt_bytes_received = (uint16_t)(srv->lpt_bytes_received + len);
        if (more_fragments && (srv->lpt_bytes_received < srv->lpt_bytes_expected)) {
            srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
            return;
        }
        {
            olnk_lpt_write_resp_t resp;
            size_t out_len = 0U;
            resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
            resp.bytes_written = 0U;
            (void)olnk_encode_lpt_write_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
            srv_queue_single_response(srv, (uint16_t)out_len);
        }
        return;
    }

    if (len == 0U) {
        srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
        return;
    }

    srv->rpc_count += 1U;
    srv_emit_event(srv, "RPC", (uint32_t)buf[0], (uint32_t)len);

    switch (buf[0]) {
        case OLNK_OP_SESSION_SYNC:
            /* Frame 1 is 2B trigger (00 00); expect 34B body next with no turnaround */
            srv->req_phase = (uint8_t)OLNK_REQ_PHASE_SYNC_BODY;
            srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
            srv->state_elapsed_ms = 0UL;
            break;

        case OLNK_OP_MAP_CONFIG:
            handle_map_config(srv, buf, len);
            break;

        case OLNK_OP_MEDIA_CHECK:
            handle_media_check(srv, buf, len);
            break;

        case OLNK_OP_BUILD_BPB:
            handle_build_bpb(srv, buf, len);
            break;

        case OLNK_OP_READ_SECTORS:
            handle_read_sectors(srv, buf, len);
            break;

        case OLNK_OP_WRITE_SECTORS:
            handle_write_sectors_header(srv, buf, len, false);
            break;

        case OLNK_OP_WRITE_VERIFY:
            handle_write_sectors_header(srv, buf, len, true);
            break;

        case OLNK_OP_DRIVE_INCOMPAT:
            /* One-way 5-byte notification: no response, no turnaround */
            srv->req_phase = (uint8_t)OLNK_REQ_PHASE_CMD;
            srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
            srv->state_elapsed_ms = 0UL;
            break;

        case OLNK_OP_BLK_DEVICE_OPEN:
        case OLNK_OP_BLK_DEVICE_CLOSE:
        case OLNK_OP_BLK_DEVICE_CLOSE_ALT: {
            olnk_unit_ctrl_req_t req;
            olnk_drive_info_t info;
            uint16_t st;
            size_t out_len = 0U;
            if (olnk_decode_unit_ctrl_req(buf, (size_t)len, &req) != OLNK_OK) {
                st = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_BAD_REQ_LENGTH);
            } else {
                st = srv_get_drive_info(srv, req.unit, &info);
                if (((st & OLNK_DOS_STATUS_ERROR) == 0U) && info.present) {
                    st = OLNK_DOS_STATUS_DONE;
                } else {
                    st = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
                }
            }
            (void)olnk_encode_status_only_resp(st, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
            srv_queue_single_response(srv, (uint16_t)out_len);
            break;
        }

        case OLNK_OP_GET_DRIVE_INFO:
            handle_get_drive_info(srv, buf, len);
            break;

        case OLNK_OP_GENERIC_IOCTL:
            handle_generic_ioctl_header(srv, buf, len);
            break;

        case OLNK_OP_LPT_WRITE:
        case OLNK_OP_LPT_WRITE_VERIFY:
        case OLNK_OP_LPT_AUX_WRITE:
        case OLNK_OP_LPT_OUTPUT_BUSY: {
            olnk_lpt_write_req_t req;
            if ((olnk_decode_lpt_write_req(buf, (size_t)len, &req) == OLNK_OK) &&
                (req.byte_count > 1U)) {
                srv->lpt_bytes_expected = req.byte_count;
                srv->lpt_bytes_received = 0U;
                srv->req_phase = (uint8_t)OLNK_REQ_PHASE_LPT_DATA;
                srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                srv->state_elapsed_ms = 0UL;
            } else {
                olnk_lpt_write_resp_t resp;
                size_t out_len = 0U;
                resp.status = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
                resp.bytes_written = 0U;
                (void)olnk_encode_lpt_write_resp(&resp, srv->tx_ctrl_buf, sizeof(srv->tx_ctrl_buf), &out_len);
                srv_queue_single_response(srv, (uint16_t)out_len);
            }
            break;
        }

        case OLNK_OP_LPT_OUTPUT_STATUS:
        case OLNK_OP_LPT_OUTPUT_FLUSH:
        case OLNK_OP_LPT_DEVICE_OPEN:
        case OLNK_OP_LPT_DEVICE_CLOSE: {
            size_t out_len = 0U;
            (void)olnk_encode_status_only_resp(
                OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT),
                srv->tx_ctrl_buf,
                sizeof(srv->tx_ctrl_buf),
                &out_len
            );
            srv_queue_single_response(srv, (uint16_t)out_len);
            break;
        }

        case OLNK_OP_BIOS_PRN_WRITE:
        case OLNK_OP_BIOS_PRN_INIT:
        case OLNK_OP_BIOS_PRN_STATUS:
            srv->tx_ctrl_buf[0] = 0x29U; /* Out of paper / I/O error / timeout */
            srv_queue_single_response(srv, OLNK_LEN_BIOS_PRN_RESP);
            break;

        default: {
            size_t out_len = 0U;
            (void)olnk_encode_status_only_resp(
                OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND),
                srv->tx_ctrl_buf,
                sizeof(srv->tx_ctrl_buf),
                &out_len
            );
            srv_queue_single_response(srv, (uint16_t)out_len);
            break;
        }
    }
}

/* =========================================================================
 * Byte-Driven Non-Blocking State Machine
 * ========================================================================= */

static void srv_handle_byte(olnk_server_t *srv, uint8_t b) {
    switch ((olnk_server_state_t)srv->srv_state) {
        case OLNK_SRV_SYNC_WAIT_AA:
            if (b == OLNK_SYNC_C1) {
                srv_write_byte(srv, OLNK_SYNC_S1);
                srv->srv_state = (uint8_t)OLNK_SRV_SYNC_WAIT_55;
                srv->state_elapsed_ms = 0UL;
            }
            break;

        case OLNK_SRV_SYNC_WAIT_55:
            if (b == OLNK_SYNC_C2) {
                srv_write_byte(srv, OLNK_SYNC_S2);
                srv->srv_state = (uint8_t)OLNK_SRV_SYNC_WAIT_5A;
                srv->state_elapsed_ms = 0UL;
            } else if (b == OLNK_SYNC_C1) {
                srv_write_byte(srv, OLNK_SYNC_S1);
                srv->state_elapsed_ms = 0UL;
            } else {
                srv->srv_state = (uint8_t)OLNK_SRV_SYNC_WAIT_AA;
            }
            break;

        case OLNK_SRV_SYNC_WAIT_5A:
            if (b == OLNK_SYNC_C3) {
                srv_write_byte(srv, OLNK_SYNC_S3);
                srv->link.link_state = (uint8_t)OLNK_LINK_CONNECTED;
                srv->link.seq = 0U;
                srv->link.current_baud_idx = (uint8_t)OLNK_BAUD_9600;
                srv->req_phase = (uint8_t)OLNK_REQ_PHASE_CMD;
                srv->resp_phase = (uint8_t)OLNK_RESP_NONE;
                srv->idle_garbage_count = 0U;
                srv->idle_elapsed_ms = 0UL;
                srv->state_elapsed_ms = 0UL;
                srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                srv_emit_event(srv, "SYNC_OK", 9600UL, 0UL);
            } else if (b == OLNK_SYNC_C1) {
                srv_write_byte(srv, OLNK_SYNC_S1);
                srv->srv_state = (uint8_t)OLNK_SRV_SYNC_WAIT_55;
                srv->state_elapsed_ms = 0UL;
            } else {
                srv->srv_state = (uint8_t)OLNK_SRV_SYNC_WAIT_AA;
            }
            break;

        case OLNK_SRV_IDLE_RX:
            srv->idle_elapsed_ms = 0UL;
            if (b == olnk_keepalive_byte(srv->link.seq)) {
                srv_write_byte(srv, b);
                srv->link.keepalives_ok += 1U;
                srv->idle_garbage_count = 0U;
                break;
            }
            if (b == olnk_baud_req_byte(srv->link.seq)) {
                srv_write_byte(srv, (uint8_t)(srv->link.seq & OLNK_HDR_SEQ_MASK));
                srv->idle_garbage_count = 0U;
                srv->state_elapsed_ms = 0UL;
                srv->srv_state = (uint8_t)OLNK_SRV_BAUD_WAIT_IDX;
                break;
            }
            if (((b & OLNK_HDR_RESERVED_MASK) == 0U) &&
                ((b & OLNK_HDR_SEQ_MASK) == (srv->link.seq & OLNK_HDR_SEQ_MASK))) {
                uint8_t ack_hdr = olnk_hdr_compute_ack(b, srv->link.local_requires_echo);
                srv->rx_hdr = b;
                srv->rx_is_short = ((b & OLNK_HDR_SHORT_LEN) != 0U);
                srv->rx_more_frags = ((b & OLNK_HDR_MORE_FRAGMENTS) != 0U);
                srv->rx_echo_mode = ((ack_hdr & OLNK_HDR_ECHO_REQUEST) != 0U);
                srv->rx_crc = OLNK_CRC16_INIT;
                srv->rx_pos = 0U;
                srv->rx_payload_len = 0U;
                srv->idle_garbage_count = 0U;
                srv->state_elapsed_ms = 0UL;
                srv->srv_state = (uint8_t)OLNK_SRV_RX_LEN_LO;
                srv_write_byte(srv, ack_hdr);
                break;
            }
            if ((b == OLNK_SYNC_C1) &&
                (srv->link.current_baud_idx == (uint8_t)OLNK_BAUD_9600) &&
                (srv->req_phase == (uint8_t)OLNK_REQ_PHASE_CMD)) {
                srv_write_byte(srv, OLNK_SYNC_S1);
                srv->srv_state = (uint8_t)OLNK_SRV_SYNC_WAIT_55;
                srv->state_elapsed_ms = 0UL;
                break;
            }
            /* Invalid header or line noise */
            srv->link.frames_rx_err += 1U;
            if ((srv->link.current_baud_idx > (uint8_t)OLNK_BAUD_9600) &&
                (srv->req_phase == (uint8_t)OLNK_REQ_PHASE_CMD)) {
                srv->idle_garbage_count += 1U;
                if (srv->idle_garbage_count >= 4U) {
                    olnk_server_drop_link(srv);
                    break;
                }
            }
            srv->state_elapsed_ms = 0UL;
            srv->srv_state = (uint8_t)OLNK_SRV_RX_ERROR_DRAIN;
            break;

        case OLNK_SRV_BAUD_WAIT_IDX:
            if ((b >= OLNK_BAUD_INDEX_COUNT) || (b > srv->link.max_baud_idx)) {
                /* Requested baud exceeds local ceiling: ignore/abort */
                srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                srv->state_elapsed_ms = 0UL;
                break;
            }
            srv->baud_prev_idx = srv->link.current_baud_idx;
            srv->baud_target_idx = b;
            if (!srv_set_baud_idx(srv, b)) {
                (void)srv_set_baud_idx(srv, srv->baud_prev_idx);
                srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                srv->state_elapsed_ms = 0UL;
                break;
            }
            srv->baud_beacon = olnk_baud_beacon_byte(b, srv->link.seq);
            srv->baud_beacon_sent = 1U;
            srv->state_elapsed_ms = 0UL;
            srv->srv_state = (uint8_t)OLNK_SRV_BAUD_WAIT_BEACON;
            srv_write_byte(srv, srv->baud_beacon);
            break;

        case OLNK_SRV_BAUD_WAIT_BEACON:
            if (b == srv->baud_beacon) {
                srv_write_byte(srv, (uint8_t)(~srv->baud_beacon));
                srv->baud_test_idx = 0U;
                srv->state_elapsed_ms = 0UL;
                srv->srv_state = (uint8_t)OLNK_SRV_BAUD_TEST_PAIR;
            }
            break;

        case OLNK_SRV_BAUD_TEST_PAIR:
            if (b == olnk_baud_test_tx[srv->baud_test_idx]) {
                srv_write_byte(srv, olnk_baud_test_rx[srv->baud_test_idx]);
                srv->baud_test_idx += 1U;
                srv->state_elapsed_ms = 0UL;
                if (srv->baud_test_idx >= OLNK_BAUD_TEST_LEN) {
                    srv->link.current_baud_idx = srv->baud_target_idx;
                    srv->link.baud_switches += 1U;
                    srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                    srv_emit_event(
                        srv,
                        "BAUD_SWITCH",
                        olnk_baud_index_to_bps(srv->baud_target_idx),
                        (uint32_t)srv->baud_target_idx
                    );
                }
            } else {
                (void)srv_set_baud_idx(srv, srv->baud_prev_idx);
                srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                srv->state_elapsed_ms = 0UL;
            }
            break;

        case OLNK_SRV_RX_LEN_LO:
            srv->rx_crc = olnk_crc16_update(srv->rx_crc, b);
            srv->state_elapsed_ms = 0UL;
            if (srv->rx_echo_mode) {
                srv_write_byte(srv, b);
            }
            if (srv->rx_is_short) {
                srv->rx_payload_len = (uint16_t)b;
                if (srv->rx_payload_len == 0U) {
                    srv->link.frames_rx_err += 1U;
                    srv->srv_state = (uint8_t)OLNK_SRV_RX_ERROR_DRAIN;
                } else {
                    srv->rx_pos = 0U;
                    srv->srv_state = (uint8_t)OLNK_SRV_RX_PAYLOAD;
                }
            } else {
                srv->rx_len_lo = b;
                srv->srv_state = (uint8_t)OLNK_SRV_RX_LEN_HI;
            }
            break;

        case OLNK_SRV_RX_LEN_HI:
            srv->rx_crc = olnk_crc16_update(srv->rx_crc, b);
            srv->state_elapsed_ms = 0UL;
            if (srv->rx_echo_mode) {
                srv_write_byte(srv, b);
            }
            srv->rx_payload_len = (uint16_t)((uint16_t)srv->rx_len_lo | ((uint16_t)b << 8U));
            if ((srv->rx_payload_len == 0U) || (srv->rx_payload_len > OLNK_MAX_PAYLOAD_SIZE)) {
                srv->link.frames_rx_err += 1U;
                srv->srv_state = (uint8_t)OLNK_SRV_RX_ERROR_DRAIN;
            } else {
                srv->rx_pos = 0U;
                srv->srv_state = (uint8_t)OLNK_SRV_RX_PAYLOAD;
            }
            break;

        case OLNK_SRV_RX_PAYLOAD:
            srv->link.rx_buffer[srv->rx_pos] = b;
            srv->rx_pos += 1U;
            srv->rx_crc = olnk_crc16_update(srv->rx_crc, b);
            srv->state_elapsed_ms = 0UL;
            if (srv->rx_echo_mode) {
                srv_write_byte(srv, b);
            }
            if (srv->rx_pos >= srv->rx_payload_len) {
                srv->srv_state = (uint8_t)OLNK_SRV_RX_CRC_HI;
            }
            break;

        case OLNK_SRV_RX_CRC_HI:
            srv->rx_crc = olnk_crc16_update(srv->rx_crc, b);
            srv->state_elapsed_ms = 0UL;
            if (srv->rx_echo_mode) {
                srv_write_byte(srv, b);
            }
            srv->srv_state = (uint8_t)OLNK_SRV_RX_CRC_LO;
            break;

        case OLNK_SRV_RX_CRC_LO:
            /* crc_lo is NEVER echoed: evaluate CRC residue == 0x1D0F directly */
            srv->rx_crc = olnk_crc16_update(srv->rx_crc, b);
            srv->state_elapsed_ms = 0UL;
            if (srv->rx_crc != OLNK_CRC16_RESIDUE_GOOD) {
                srv->link.frames_rx_err += 1U;
                srv->srv_state = (uint8_t)OLNK_SRV_RX_ERROR_DRAIN;
                break;
            }
            srv_write_byte(srv, srv->rx_hdr);
            srv->link.seq = (uint8_t)((srv->link.seq + 1U) & OLNK_HDR_SEQ_MASK);
            srv->link.frames_rx_ok += 1U;
            handle_rpc_frame(srv, srv->link.rx_buffer, srv->rx_payload_len, srv->rx_more_frags);
            break;

        case OLNK_SRV_RX_ERROR_DRAIN:
            srv->state_elapsed_ms = 0UL;
            break;

        case OLNK_SRV_WAIT_TURNAROUND:
            if (b == olnk_turnaround_byte(srv->link.seq)) {
                srv->tx_retries = 0U;
                srv->tx_echo_request = false;
                srv_start_tx_frame(srv, srv->tx_payload_ptr, srv->tx_payload_len, srv->tx_more_frags);
            } else if (b == olnk_keepalive_byte(srv->link.seq)) {
                srv_write_byte(srv, b);
                srv->link.keepalives_ok += 1U;
            }
            break;

        case OLNK_SRV_TX_WAIT_HDR_ACK:
            if (olnk_hdr_verify_ack(srv->tx_hdr, b, &srv->tx_echo_mode)) {
                srv->state_elapsed_ms = 0UL;
                if (srv->tx_echo_mode) {
                    srv->tx_stream_pos = 0U;
                    srv->tx_last_sent_byte = srv_tx_stream_byte(srv, 0U);
                    srv->srv_state = (uint8_t)OLNK_SRV_TX_ECHO_BYTE;
                    srv_write_byte(srv, srv->tx_last_sent_byte);
                } else {
                    (void)srv_write_all(srv, srv->tx_len_bytes, (size_t)srv->tx_len_sz);
                    (void)srv_write_all(srv, srv->tx_payload_ptr, (size_t)srv->tx_payload_len);
                    (void)srv_write_all(srv, srv->tx_crc_be, 2U);
                    srv->srv_state = (uint8_t)OLNK_SRV_TX_WAIT_FINAL_ACK;
                }
            } else {
                srv_retry_tx_frame(srv);
            }
            break;

        case OLNK_SRV_TX_ECHO_BYTE:
            if (b != srv->tx_last_sent_byte) {
                srv_retry_tx_frame(srv);
                break;
            }
            srv->state_elapsed_ms = 0UL;
            srv->tx_stream_pos += 1U;
            srv->tx_last_sent_byte = srv_tx_stream_byte(srv, srv->tx_stream_pos);
            if ((uint16_t)(srv->tx_stream_pos + 1U) >= srv->tx_stream_total) {
                /* Sending crc_lo: receiver does not echo crc_lo, expects final_ack next */
                srv->srv_state = (uint8_t)OLNK_SRV_TX_WAIT_FINAL_ACK;
            }
            srv_write_byte(srv, srv->tx_last_sent_byte);
            break;

        case OLNK_SRV_TX_WAIT_FINAL_ACK:
            if (b == srv->tx_hdr) {
                srv_on_tx_frame_acked(srv);
            } else {
                srv_retry_tx_frame(srv);
            }
            break;

        default:
            olnk_server_drop_link(srv);
            break;
    }
}

/* =========================================================================
 * Public Server API
 * ========================================================================= */

olnk_status_t olnk_server_init(
    olnk_server_t *srv,
    olnk_transport_t transport,
    olnk_storage_ops_t storage
) {
    olnk_status_t st;
    if (srv == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    memset(srv, 0, sizeof(*srv));
    st = olnk_init(&srv->link, transport);
    if (st != OLNK_OK) {
        return st;
    }
    srv->storage = storage;
    srv->link.server_id = OLNK_SERVER_DEFAULT_ID;
    srv->link.local_requires_echo = true;
    srv->srv_state = (uint8_t)OLNK_SRV_SYNC_WAIT_AA;
    srv->req_phase = (uint8_t)OLNK_REQ_PHASE_CMD;
    srv->resp_phase = (uint8_t)OLNK_RESP_NONE;
    (void)srv_set_baud_idx(srv, (uint8_t)OLNK_BAUD_9600);
    return OLNK_OK;
}

void olnk_server_set_event_callback(
    olnk_server_t *srv,
    olnk_server_event_cb_t cb,
    void *user_data
) {
    if (srv == NULL) {
        return;
    }
    srv->on_event = cb;
    srv->on_event_user_data = user_data;
}

void olnk_server_drop_link(olnk_server_t *srv) {
    if (srv == NULL) {
        return;
    }
    olnk_link_disconnect(&srv->link);
    srv->srv_state = (uint8_t)OLNK_SRV_SYNC_WAIT_AA;
    srv->req_phase = (uint8_t)OLNK_REQ_PHASE_CMD;
    srv->resp_phase = (uint8_t)OLNK_RESP_NONE;
    srv->state_elapsed_ms = 0UL;
    srv->idle_elapsed_ms = 0UL;
    srv->idle_garbage_count = 0U;
    srv->read_prefetched_valid = false;
    srv_emit_event(srv, "DISCONNECT", 9600UL, 0UL);
}

void olnk_server_feed_bytes(olnk_server_t *srv, const uint8_t *data, size_t len) {
    size_t i;
    if ((srv == NULL) || (data == NULL)) {
        return;
    }
    for (i = 0U; i < len; ++i) {
        srv_handle_byte(srv, data[i]);
    }
}

void olnk_server_tick(olnk_server_t *srv, uint32_t elapsed_ms) {
    if (srv == NULL) {
        return;
    }
    srv->state_elapsed_ms += elapsed_ms;

    switch ((olnk_server_state_t)srv->srv_state) {
        case OLNK_SRV_SYNC_WAIT_55:
        case OLNK_SRV_SYNC_WAIT_5A:
            if (srv->state_elapsed_ms >= srv->link.sync_timeout_ms) {
                srv->srv_state = (uint8_t)OLNK_SRV_SYNC_WAIT_AA;
                srv->state_elapsed_ms = 0UL;
            }
            break;

        case OLNK_SRV_IDLE_RX:
            srv->idle_elapsed_ms += elapsed_ms;
            if ((srv->idle_disconnect_ms > 0UL) &&
                (srv->link.current_baud_idx > (uint8_t)OLNK_BAUD_9600) &&
                (srv->idle_elapsed_ms >= srv->idle_disconnect_ms)) {
                olnk_server_drop_link(srv);
            }
            break;

        case OLNK_SRV_BAUD_WAIT_IDX:
            if (srv->state_elapsed_ms >= srv->link.rx_byte_timeout_ms) {
                srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                srv->state_elapsed_ms = 0UL;
            }
            break;

        case OLNK_SRV_BAUD_WAIT_BEACON:
            if (srv->state_elapsed_ms >= OLNK_TIMEOUT_BEACON_TICK_MS) {
                srv->state_elapsed_ms = 0UL;
                if (srv->baud_beacon_sent < OLNK_BEACON_MAX_RETRIES) {
                    srv->baud_beacon_sent += 1U;
                    srv_write_byte(srv, srv->baud_beacon);
                } else {
                    (void)srv_set_baud_idx(srv, srv->baud_prev_idx);
                    srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                }
            }
            break;

        case OLNK_SRV_BAUD_TEST_PAIR:
            if (srv->state_elapsed_ms >= srv->link.rx_byte_timeout_ms) {
                (void)srv_set_baud_idx(srv, srv->baud_prev_idx);
                srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                srv->state_elapsed_ms = 0UL;
            }
            break;

        case OLNK_SRV_RX_LEN_LO:
        case OLNK_SRV_RX_LEN_HI:
        case OLNK_SRV_RX_PAYLOAD:
        case OLNK_SRV_RX_CRC_HI:
        case OLNK_SRV_RX_CRC_LO:
            if (srv->state_elapsed_ms >= srv->link.rx_byte_timeout_ms) {
                srv->link.frames_rx_err += 1U;
                srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                srv->state_elapsed_ms = 0UL;
            }
            break;

        case OLNK_SRV_RX_ERROR_DRAIN:
            if (srv->state_elapsed_ms >= srv->link.rx_byte_timeout_ms) {
                srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                srv->state_elapsed_ms = 0UL;
            }
            break;

        case OLNK_SRV_WAIT_TURNAROUND:
            if (srv->state_elapsed_ms >= 2000UL) {
                srv->resp_phase = (uint8_t)OLNK_RESP_NONE;
                srv->req_phase = (uint8_t)OLNK_REQ_PHASE_CMD;
                srv->srv_state = (uint8_t)OLNK_SRV_IDLE_RX;
                srv->state_elapsed_ms = 0UL;
            }
            break;

        case OLNK_SRV_TX_WAIT_HDR_ACK:
        case OLNK_SRV_TX_ECHO_BYTE:
        case OLNK_SRV_TX_WAIT_FINAL_ACK:
            if (srv->state_elapsed_ms >= srv->link.tx_timeout_ms) {
                srv_retry_tx_frame(srv);
            }
            break;

        case OLNK_SRV_SYNC_WAIT_AA:
        default:
            break;
    }
}

olnk_status_t olnk_server_poll(olnk_server_t *srv, uint32_t timeout_ms) {
    uint8_t buf[64];
    int n;

    if (srv == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (srv->link.transport.read == NULL) {
        return OLNK_ERR_HAL_TIMEOUT;
    }

    n = srv->link.transport.read(buf, sizeof(buf), timeout_ms, srv->link.transport.user_data);
    if (n < 0) {
        return OLNK_ERR_HAL_IO;
    }
    if (n == 0) {
        olnk_server_tick(srv, timeout_ms);
        return OLNK_OK;
    }

    olnk_server_feed_bytes(srv, buf, (size_t)n);
    return OLNK_OK;
}
