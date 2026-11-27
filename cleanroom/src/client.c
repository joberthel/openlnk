/* cleanroom/src/client.c */
#include "openlnk/client.h"

#include <string.h>

static void cli_drain_rx(olnk_client_t *cli, uint32_t quiet_ms) {
    uint8_t discard[32];
    uint16_t guard;
    if (cli->link.transport.read == NULL) {
        return;
    }
    for (guard = 0U; guard < 128U; ++guard) {
        int n = cli->link.transport.read(discard, sizeof(discard), quiet_ms, cli->link.transport.user_data);
        if (n <= 0) {
            break;
        }
    }
}

static olnk_status_t cli_send_and_turn(
    olnk_client_t *cli,
    const uint8_t *buf,
    uint16_t len
) {
    olnk_status_t st = olnk_link_send_frame(&cli->link, buf, len, false);
    if (st != OLNK_OK) {
        return st;
    }
    return olnk_link_send_turnaround(&cli->link);
}

static olnk_status_t cli_recv_ctrl(olnk_client_t *cli, uint16_t *out_resp_len) {
    bool more = false;
    return olnk_link_recv_frame(
        &cli->link,
        cli->ctrl_buf,
        sizeof(cli->ctrl_buf),
        out_resp_len,
        &more,
        cli->rpc_timeout_ms
    );
}

static olnk_status_t cli_recv_ctrl_and_turn(olnk_client_t *cli, uint16_t *out_resp_len) {
    olnk_status_t st = cli_recv_ctrl(cli, out_resp_len);
    if (st != OLNK_OK) {
        return st;
    }
    st = olnk_link_wait_turnaround(&cli->link, cli->rpc_timeout_ms);
    if (st == OLNK_OK) {
        cli->rpc_count += 1U;
    }
    return st;
}

static olnk_status_t cli_exchange_single(
    olnk_client_t *cli,
    const uint8_t *req_buf,
    uint16_t req_len,
    uint16_t *out_resp_len
) {
    olnk_status_t st = cli_send_and_turn(cli, req_buf, req_len);
    if (st != OLNK_OK) {
        return st;
    }
    return cli_recv_ctrl_and_turn(cli, out_resp_len);
}

static olnk_status_t cli_send_sector_io_hdr(
    olnk_client_t *cli,
    uint8_t opcode,
    uint8_t unit,
    uint8_t media_descriptor,
    uint32_t start_lba,
    uint16_t sector_count
) {
    uint8_t req_buf[OLNK_LEN_SECTOR_IO_REQ];
    bool use_lba32 = olnk_client_unit_supports_lba32(cli, unit) || (start_lba >= 0xFFFFUL);

    req_buf[0] = opcode;
    req_buf[1] = unit;
    req_buf[2] = media_descriptor;
    olnk_write_u16_le(&req_buf[3], sector_count);
    olnk_write_u16_le(&req_buf[5], use_lba32 ? 0xFFFFU : (uint16_t)start_lba);
    olnk_write_u32_le(&req_buf[7], start_lba);
    return olnk_link_send_frame(&cli->link, req_buf, OLNK_LEN_SECTOR_IO_REQ, false);
}

olnk_status_t olnk_client_init(olnk_client_t *cli, olnk_transport_t transport) {
    olnk_status_t st;
    if (cli == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    memset(cli, 0, sizeof(*cli));
    st = olnk_init(&cli->link, transport);
    if (st != OLNK_OK) {
        return st;
    }
    cli->link.client_id = OLNK_CLIENT_DEFAULT_ID;
    cli->link.local_requires_echo = true;
    cli->rpc_timeout_ms = OLNK_CLIENT_DEFAULT_RESP_MS;
    return OLNK_OK;
}

void olnk_client_disconnect(olnk_client_t *cli) {
    if (cli == NULL) {
        return;
    }
    olnk_link_disconnect(&cli->link);
}

olnk_status_t olnk_client_connect(olnk_client_t *cli, uint16_t sync_retries) {
    uint16_t attempts;
    uint16_t i;
    olnk_status_t st = OLNK_ERR_HAL_TIMEOUT;

    if (cli == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

    attempts = (sync_retries == 0U) ? 1U : sync_retries;
    for (i = 0U; i < attempts; ++i) {
        if (i > 0U) {
            /* If the peer echoed mismatched-baud bytes (OLNK_ERR_PROTOCOL) and
             * returned immediately, wait a full sync_timeout_ms quiet gap so
             * the peer's RX_ERROR_DRAIN completes and probes stay spaced apart. */
            uint32_t quiet_ms = (st == OLNK_ERR_PROTOCOL) ? cli->link.sync_timeout_ms : 5U;
            cli_drain_rx(cli, quiet_ms);
        }
        st = olnk_link_connect_client(&cli->link);
        if ((st == OLNK_OK) || (st == OLNK_ERR_HAL_IO)) {
            return st;
        }
    }
    return st;
}

olnk_status_t olnk_client_session_sync(
    olnk_client_t *cli,
    bool ext_info,
    olnk_session_sync_resp_t *out_resp
) {
    olnk_session_sync_resp_t resp;
    uint8_t req_buf[OLNK_LEN_SESSION_SYNC_REQ];
    uint16_t resp_len = 0U;
    olnk_status_t st;

    if (cli == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

    if (cli->link.client_id == 0UL) {
        cli->link.client_id = OLNK_CLIENT_DEFAULT_ID;
    }

    /* Frame 1: 2-byte trigger (00 00), NO turnaround after */
    req_buf[0] = (uint8_t)OLNK_OP_SESSION_SYNC;
    req_buf[1] = 0U;
    st = olnk_link_send_frame(&cli->link, req_buf, OLNK_LEN_SYNC_TRIGGER, false);
    if (st != OLNK_OK) {
        return st;
    }

    /* Frame 2: 34-byte session sync request */
    memset(req_buf, 0, sizeof(req_buf));
    olnk_write_u16_le(&req_buf[6], OLNK_CLIENT_VERSION_DOS622);
    req_buf[8] = 1U;
    req_buf[9] = ext_info ? 1U : 0U;
    req_buf[11] = 1U;
    olnk_write_u16_le(&req_buf[12], (uint16_t)OLNK_MAX_PAYLOAD_SIZE);
    olnk_write_u32_le(&req_buf[26], cli->link.client_id);
    olnk_write_u32_le(&req_buf[30], cli->link.server_id);

    st = cli_exchange_single(cli, req_buf, OLNK_LEN_SESSION_SYNC_REQ, &resp_len);
    if (st != OLNK_OK) {
        return st;
    }

    st = olnk_decode_session_sync_resp(cli->ctrl_buf, (size_t)resp_len, &resp);
    if (st != OLNK_OK) {
        return st;
    }
    if (resp.ready_flag == 0U) {
        return OLNK_ERR_PROTOCOL;
    }

    if ((resp.max_block_size >= OLNK_SECTOR_SIZE) &&
        (resp.max_block_size <= OLNK_MAX_PAYLOAD_SIZE)) {
        cli->link.max_block_size = resp.max_block_size;
    } else {
        cli->link.max_block_size = (uint16_t)OLNK_MAX_PAYLOAD_SIZE;
    }

    cli->link.session_resumed =
        (resp.prev_client_id == cli->link.client_id) &&
        (cli->link.server_id != 0UL) &&
        (resp.server_id == cli->link.server_id);
    cli->link.prev_client_id = resp.prev_client_id;
    cli->link.server_id = resp.server_id;

    if (out_resp != NULL) {
        *out_resp = resp;
    }
    return OLNK_OK;
}

olnk_status_t olnk_client_map_config(
    olnk_client_t *cli,
    uint8_t max_client_drives,
    uint8_t first_client_drive,
    olnk_map_config_resp_t *out_resp
) {
    uint8_t req_buf[OLNK_LEN_MAP_CONFIG_REQ];
    uint16_t resp_len = 0U;
    olnk_status_t st;

    if (cli == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

    req_buf[0] = (uint8_t)OLNK_OP_MAP_CONFIG;
    olnk_write_u16_le(&req_buf[1], OLNK_MAP_PROTO_VER);
    req_buf[3] = OLNK_DOS_VER_MINOR;
    req_buf[4] = OLNK_DOS_VER_MAJOR;
    req_buf[5] = max_client_drives;
    req_buf[6] = first_client_drive;
    memset(&req_buf[7], OLNK_MAP_NONE, OLNK_MAX_LPTS);
    memset(&req_buf[10], OLNK_MAP_AUTO, OLNK_MAX_DRIVES);

    st = cli_exchange_single(cli, req_buf, OLNK_LEN_MAP_CONFIG_REQ, &resp_len);
    if (st != OLNK_OK) {
        return st;
    }

    /* Decode directly into cli->map (olnk_decode_map_config_resp validates length before writes) */
    st = olnk_decode_map_config_resp(cli->ctrl_buf, (size_t)resp_len, &cli->map);
    if (st != OLNK_OK) {
        return st;
    }

    cli->has_map = true;
    if (cli->map.server_flags != 0U) {
        /* Server requested timer/echo compatibility mode */
        cli->link.local_requires_echo = true;
    }
    if (out_resp != NULL) {
        *out_resp = cli->map;
    }
    return OLNK_OK;
}

bool olnk_client_unit_supports_lba32(const olnk_client_t *cli, uint8_t unit) {
    if ((cli == NULL) || (!cli->has_map) || (unit >= OLNK_MAX_DRIVES)) {
        return true;
    }
    return (cli->map.server_drive_attrs[unit] & 0x0002U) != 0U;
}

olnk_status_t olnk_client_get_drive_info(
    olnk_client_t *cli,
    uint8_t unit,
    olnk_get_drive_info_resp_t *out_resp
) {
    uint8_t req_buf[OLNK_LEN_GET_DRIVE_INFO_REQ];
    uint16_t resp_len = 0U;
    olnk_status_t st;

    if ((cli == NULL) || (out_resp == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    req_buf[0] = (uint8_t)OLNK_OP_GET_DRIVE_INFO;
    req_buf[1] = unit;
    st = cli_exchange_single(cli, req_buf, OLNK_LEN_GET_DRIVE_INFO_REQ, &resp_len);
    if (st != OLNK_OK) {
        return st;
    }
    return olnk_decode_get_drive_info_resp(cli->ctrl_buf, (size_t)resp_len, out_resp);
}

olnk_status_t olnk_client_media_check(
    olnk_client_t *cli,
    uint8_t unit,
    uint8_t media_descriptor,
    olnk_media_check_resp_t *out_resp
) {
    uint8_t req_buf[OLNK_LEN_MEDIA_CHECK_REQ];
    uint16_t resp_len = 0U;
    olnk_status_t st;

    if ((cli == NULL) || (out_resp == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    req_buf[0] = (uint8_t)OLNK_OP_MEDIA_CHECK;
    req_buf[1] = unit;
    req_buf[2] = media_descriptor;
    st = cli_exchange_single(cli, req_buf, OLNK_LEN_MEDIA_CHECK_REQ, &resp_len);
    if (st != OLNK_OK) {
        return st;
    }
    return olnk_decode_media_check_resp(cli->ctrl_buf, (size_t)resp_len, out_resp);
}

olnk_status_t olnk_client_build_bpb(
    olnk_client_t *cli,
    uint8_t unit,
    uint8_t media_descriptor,
    olnk_build_bpb_resp_t *out_resp
) {
    uint8_t req_buf[OLNK_LEN_BUILD_BPB_REQ];
    uint16_t resp_len = 0U;
    olnk_status_t st;

    if ((cli == NULL) || (out_resp == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    req_buf[0] = (uint8_t)OLNK_OP_BUILD_BPB;
    req_buf[1] = unit;
    req_buf[2] = media_descriptor;
    st = cli_exchange_single(cli, req_buf, OLNK_LEN_BUILD_BPB_REQ, &resp_len);
    if (st != OLNK_OK) {
        return st;
    }
    return olnk_decode_build_bpb_resp(cli->ctrl_buf, (size_t)resp_len, out_resp);
}

olnk_status_t olnk_client_read_sectors(
    olnk_client_t *cli,
    uint8_t unit,
    uint8_t media_descriptor,
    uint32_t start_lba,
    uint16_t sector_count,
    uint16_t bytes_per_sector,
    uint8_t *out_buf,
    uint32_t out_cap,
    olnk_sector_io_resp_t *out_resp
) {
    olnk_sector_io_resp_t resp;
    uint16_t resp_len = 0U;
    uint16_t bps = (bytes_per_sector != 0U) ? bytes_per_sector : OLNK_SECTOR_SIZE;
    olnk_status_t st;

    if ((cli == NULL) || (out_resp == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if ((sector_count > 0U) && (out_buf == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (((uint32_t)sector_count * (uint32_t)bps) > out_cap) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }

    st = cli_send_sector_io_hdr(
        cli, (uint8_t)OLNK_OP_READ_SECTORS, unit, media_descriptor, start_lba, sector_count);
    if (st != OLNK_OK) {
        return st;
    }
    st = olnk_link_send_turnaround(&cli->link);
    if (st != OLNK_OK) {
        return st;
    }

    /* Receive 4B (or 20B on 0x0F) status header frame */
    st = cli_recv_ctrl(cli, &resp_len);
    if (st != OLNK_OK) {
        return st;
    }
    st = olnk_decode_sector_io_resp(cli->ctrl_buf, (size_t)resp_len, &resp);
    if (st != OLNK_OK) {
        return st;
    }

    /* Receive sector data frame(s) iff bit 15 (ERROR) == 0 AND sectors_transferred > 0 */
    if (((resp.status & OLNK_DOS_STATUS_ERROR) == 0U) && (resp.sectors_transferred > 0U)) {
        uint32_t expected_bytes = (uint32_t)resp.sectors_transferred * (uint32_t)bps;
        uint32_t got_bytes = 0UL;
        if (expected_bytes > out_cap) {
            return OLNK_ERR_BUFFER_OVERFLOW;
        }
        st = olnk_link_recv_message(&cli->link, out_buf, out_cap, &got_bytes, cli->rpc_timeout_ms);
        if (st != OLNK_OK) {
            return st;
        }
        if (got_bytes != expected_bytes) {
            return OLNK_ERR_PROTOCOL;
        }
        cli->sectors_read_total += (uint32_t)resp.sectors_transferred;
    }

    st = olnk_link_wait_turnaround(&cli->link, cli->rpc_timeout_ms);
    if (st != OLNK_OK) {
        return st;
    }

    cli->rpc_count += 1U;
    *out_resp = resp;
    return OLNK_OK;
}

olnk_status_t olnk_client_write_sectors(
    olnk_client_t *cli,
    uint8_t unit,
    uint8_t media_descriptor,
    uint32_t start_lba,
    uint16_t sector_count,
    uint16_t bytes_per_sector,
    const uint8_t *in_buf,
    bool verify,
    olnk_sector_io_resp_t *out_resp
) {
    olnk_sector_io_resp_t resp;
    uint16_t resp_len = 0U;
    uint16_t bps = (bytes_per_sector != 0U) ? bytes_per_sector : OLNK_SECTOR_SIZE;
    olnk_status_t st;

    if ((cli == NULL) || (out_resp == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if ((sector_count > 0U) && (in_buf == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    /* Frame 1: 11-byte write header (NO turnaround) */
    st = cli_send_sector_io_hdr(
        cli,
        (uint8_t)(verify ? OLNK_OP_WRITE_VERIFY : OLNK_OP_WRITE_SECTORS),
        unit,
        media_descriptor,
        start_lba,
        sector_count
    );
    if (st != OLNK_OK) {
        return st;
    }

    /* Frame 2+: sector data fragmented into <= max_block_size chunks */
    if (sector_count > 0U) {
        uint32_t total_bytes = (uint32_t)sector_count * (uint32_t)bps;
        st = olnk_link_send_message(&cli->link, in_buf, total_bytes);
        if (st != OLNK_OK) {
            return st;
        }
    }

    st = olnk_link_send_turnaround(&cli->link);
    if (st != OLNK_OK) {
        return st;
    }

    st = cli_recv_ctrl_and_turn(cli, &resp_len);
    if (st != OLNK_OK) {
        return st;
    }

    st = olnk_decode_sector_io_resp(cli->ctrl_buf, (size_t)resp_len, &resp);
    if (st != OLNK_OK) {
        return st;
    }

    if ((resp.status & OLNK_DOS_STATUS_ERROR) == 0U) {
        cli->sectors_written_total += (uint32_t)resp.sectors_transferred;
    }
    *out_resp = resp;
    return OLNK_OK;
}

olnk_status_t olnk_client_blk_ctrl(
    olnk_client_t *cli,
    uint8_t opcode,
    uint8_t unit,
    uint16_t *out_status
) {
    uint8_t req_buf[OLNK_LEN_UNIT_CTRL_REQ];
    uint16_t resp_len = 0U;
    olnk_status_t st;

    if ((cli == NULL) || (out_status == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    req_buf[0] = opcode;
    req_buf[1] = unit;
    st = cli_exchange_single(cli, req_buf, OLNK_LEN_UNIT_CTRL_REQ, &resp_len);
    if (st != OLNK_OK) {
        return st;
    }
    return olnk_decode_status_only_resp(cli->ctrl_buf, (size_t)resp_len, out_status);
}

olnk_status_t olnk_client_drive_incompat(
    olnk_client_t *cli,
    uint8_t client_unit,
    uint8_t server_unit
) {
    uint8_t req_buf[OLNK_LEN_DRIVE_INCOMPAT];
    olnk_status_t st;

    if (cli == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

    req_buf[0] = (uint8_t)OLNK_OP_DRIVE_INCOMPAT;
    req_buf[1] = 0xFFU;
    req_buf[2] = client_unit;
    req_buf[3] = 0x00U;
    req_buf[4] = server_unit;
    /* One-way notification: no turnaround, no server response */
    st = olnk_link_send_frame(&cli->link, req_buf, OLNK_LEN_DRIVE_INCOMPAT, false);
    if (st == OLNK_OK) {
        cli->rpc_count += 1U;
    }
    return st;
}

olnk_status_t olnk_client_get_media_id(
    olnk_client_t *cli,
    uint8_t unit,
    uint16_t *out_status,
    olnk_generic_ioctl_mid_t *out_mid
) {
    uint8_t req_buf[OLNK_LEN_GENERIC_IOCTL_REQ];
    uint16_t resp_len = 0U;
    uint16_t dos_status = 0U;
    olnk_status_t st;

    if ((cli == NULL) || (out_status == NULL) || (out_mid == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    memset(out_mid, 0, sizeof(*out_mid));
    req_buf[0] = (uint8_t)OLNK_OP_GENERIC_IOCTL;
    req_buf[1] = unit;
    req_buf[2] = OLNK_IOCTL_CAT_DISK;
    req_buf[3] = OLNK_IOCTL_FN_GET_MEDIA_ID;

    st = cli_send_and_turn(cli, req_buf, OLNK_LEN_GENERIC_IOCTL_REQ);
    if (st != OLNK_OK) {
        return st;
    }

    st = cli_recv_ctrl(cli, &resp_len);
    if (st != OLNK_OK) {
        return st;
    }
    st = olnk_decode_status_only_resp(cli->ctrl_buf, (size_t)resp_len, &dos_status);
    if (st != OLNK_OK) {
        return st;
    }

    if ((dos_status & OLNK_DOS_STATUS_ERROR) == 0U) {
        st = cli_recv_ctrl(cli, &resp_len);
        if (st != OLNK_OK) {
            return st;
        }
        st = olnk_decode_generic_ioctl_mid(cli->ctrl_buf, (size_t)resp_len, out_mid);
        if (st != OLNK_OK) {
            return st;
        }
    }

    st = olnk_link_wait_turnaround(&cli->link, cli->rpc_timeout_ms);
    if (st != OLNK_OK) {
        return st;
    }

    cli->rpc_count += 1U;
    *out_status = dos_status;
    return OLNK_OK;
}

olnk_status_t olnk_client_set_media_id(
    olnk_client_t *cli,
    uint8_t unit,
    const olnk_generic_ioctl_mid_t *mid,
    uint16_t *out_status
) {
    uint8_t hdr_buf[OLNK_LEN_GENERIC_IOCTL_REQ];
    uint8_t mid_buf[OLNK_LEN_GENERIC_IOCTL_MID];
    size_t mid_len = 0U;
    uint16_t resp_len = 0U;
    olnk_status_t st;

    if ((cli == NULL) || (mid == NULL) || (out_status == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    hdr_buf[0] = (uint8_t)OLNK_OP_GENERIC_IOCTL;
    hdr_buf[1] = unit;
    hdr_buf[2] = OLNK_IOCTL_CAT_DISK;
    hdr_buf[3] = OLNK_IOCTL_FN_SET_MEDIA_ID;

    st = olnk_encode_generic_ioctl_mid(mid, mid_buf, sizeof(mid_buf), &mid_len);
    if (st != OLNK_OK) {
        return st;
    }

    /* Frame 1: 4B IOCTL header (NO turnaround), Frame 2: 25B MID + turnaround */
    st = olnk_link_send_frame(&cli->link, hdr_buf, OLNK_LEN_GENERIC_IOCTL_REQ, false);
    if (st != OLNK_OK) {
        return st;
    }
    st = cli_exchange_single(cli, mid_buf, (uint16_t)mid_len, &resp_len);
    if (st != OLNK_OK) {
        return st;
    }
    return olnk_decode_status_only_resp(cli->ctrl_buf, (size_t)resp_len, out_status);
}

olnk_status_t olnk_client_keepalive(olnk_client_t *cli) {
    if (cli == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    return olnk_link_send_keepalive(&cli->link);
}
