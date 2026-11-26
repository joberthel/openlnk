/* cleanroom/desktop/main_server.c */
#include "interlnk/interlnk.h"
#include "interlnk/storage.h"

#include <stdio.h>
#include <string.h>

#define SERVER_PIPE_CAP 2048U

typedef struct {
    uint8_t buf[SERVER_PIPE_CAP];
    size_t head;
    size_t tail;
    size_t count;
} loopback_pipe_t;

typedef struct {
    loopback_pipe_t *rx;
    loopback_pipe_t *tx;
    uint32_t baud;
} loopback_hal_t;

typedef struct {
    ilnk_ctx_t *ctx;
    ilnk_storage_ops_t storage;
    uint8_t tx_scratch[ILNK_RX_BUFFER_SIZE];
    uint8_t sector_buf[ILNK_SECTOR_SIZE];
    uint32_t handled_cmds;
} server_state_t;

static int pipe_read(uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data) {
    loopback_hal_t *hal = (loopback_hal_t *)user_data;
    size_t n = 0U;
    (void)timeout_ms;

    while ((n < len) && (hal->rx->count > 0U)) {
        buf[n] = hal->rx->buf[hal->rx->head];
        hal->rx->head = (hal->rx->head + 1U) % SERVER_PIPE_CAP;
        hal->rx->count -= 1U;
        n += 1U;
    }
    return (int)n;
}

static int pipe_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data) {
    loopback_hal_t *hal = (loopback_hal_t *)user_data;
    size_t n = 0U;
    (void)timeout_ms;

    while ((n < len) && (hal->tx->count < SERVER_PIPE_CAP)) {
        hal->tx->buf[hal->tx->tail] = buf[n];
        hal->tx->tail = (hal->tx->tail + 1U) % SERVER_PIPE_CAP;
        hal->tx->count += 1U;
        n += 1U;
    }
    return (int)n;
}

static bool pipe_set_baud(uint32_t baudrate, void *user_data) {
    loopback_hal_t *hal = (loopback_hal_t *)user_data;
    hal->baud = baudrate;
    return true;
}

static void on_server_frame(const ilnk_frame_t *frame, void *user_data) {
    server_state_t *srv = (server_state_t *)user_data;
    ilnk_frame_t resp;

    memset(&resp, 0, sizeof(resp));
    resp.sequence_number = frame->sequence_number;
    srv->handled_cmds += 1U;

    if (frame->frame_type == ILNK_FRAME_CMD_INIT) {
        resp.frame_type = ILNK_FRAME_RESP_INIT_ACK;
        resp.payload.init_ack.server_version = 0x0622U;
        resp.payload.init_ack.drive_count = 1U;
        resp.payload.init_ack.status = 0U;
    } else if (frame->frame_type == ILNK_FRAME_CMD_READ_SECTOR) {
        ilnk_proto_error_t err = srv->storage.read_sectors(
            frame->payload.read_sector.drive_index,
            frame->payload.read_sector.start_sector,
            1U,
            srv->sector_buf,
            srv->storage.user_data
        );
        if (err == ILNK_PROTO_ERR_NONE) {
            resp.frame_type = ILNK_FRAME_RESP_READ_DATA;
            resp.payload.read_data.drive_index = frame->payload.read_sector.drive_index;
            resp.payload.read_data.start_sector = frame->payload.read_sector.start_sector;
            resp.payload.read_data.data = srv->sector_buf;
            resp.payload.read_data.data_len = ILNK_SECTOR_SIZE;
        } else {
            resp.frame_type = ILNK_FRAME_RESP_ERROR;
            resp.payload.error.error_code = (uint8_t)err;
            resp.payload.error.detail = 0U;
        }
    } else {
        resp.frame_type = ILNK_FRAME_RESP_ERROR;
        resp.payload.error.error_code = (uint8_t)ILNK_PROTO_ERR_UNSUPPORTED_CMD;
        resp.payload.error.detail = frame->frame_type;
    }

    (void)ilnk_send_frame(srv->ctx, &resp, srv->tx_scratch, sizeof(srv->tx_scratch), 100U);
}

static void on_client_verify(const ilnk_frame_t *frame, void *user_data) {
    ilnk_frame_t *captured = (ilnk_frame_t *)user_data;
    *captured = *frame;
}

static int run_self_test(void) {
    static uint8_t disk_image[ILNK_SECTOR_SIZE * 4U];
    static uint8_t tx_buf[ILNK_RX_BUFFER_SIZE];
    loopback_pipe_t c2s;
    loopback_pipe_t s2c;
    loopback_hal_t server_hal;
    loopback_hal_t client_hal;
    ilnk_transport_t srv_tr;
    ilnk_transport_t cli_tr;
    ilnk_ctx_t srv_ctx;
    ilnk_ctx_t cli_ctx;
    ilnk_mem_drive_t drive;
    server_state_t srv_state;
    ilnk_frame_t req;
    ilnk_frame_t got;

    memset(disk_image, 0xA5, sizeof(disk_image));
    memset(&c2s, 0, sizeof(c2s));
    memset(&s2c, 0, sizeof(s2c));
    memset(&got, 0, sizeof(got));

    server_hal.rx = &c2s;
    server_hal.tx = &s2c;
    server_hal.baud = 115200U;
    client_hal.rx = &s2c;
    client_hal.tx = &c2s;
    client_hal.baud = 115200U;

    srv_tr.read = pipe_read;
    srv_tr.write = pipe_write;
    srv_tr.set_baud = pipe_set_baud;
    srv_tr.user_data = &server_hal;

    cli_tr.read = pipe_read;
    cli_tr.write = pipe_write;
    cli_tr.set_baud = pipe_set_baud;
    cli_tr.user_data = &client_hal;

    drive.data = disk_image;
    drive.total_sectors = 4U;
    drive.write_protected = false;
    drive.media_changed_flag = false;

    if ((ilnk_init(&srv_ctx, srv_tr) != ILNK_OK) ||
        (ilnk_init(&cli_ctx, cli_tr) != ILNK_OK)) {
        return 1;
    }

    memset(&srv_state, 0, sizeof(srv_state));
    srv_state.ctx = &srv_ctx;
    srv_state.storage = ilnk_mem_drive_ops(&drive);
    ilnk_set_frame_handler(&srv_ctx, on_server_frame, &srv_state);
    ilnk_set_frame_handler(&cli_ctx, on_client_verify, &got);

    memset(&req, 0, sizeof(req));
    req.frame_type = ILNK_FRAME_CMD_INIT;
    req.sequence_number = 1U;
    req.payload.init.client_version = 0x0622U;
    req.payload.init.drive_count = 4U;

    if (ilnk_send_frame(&cli_ctx, &req, tx_buf, sizeof(tx_buf), 100U) != ILNK_OK) {
        return 2;
    }
    if (ilnk_poll(&srv_ctx) != ILNK_OK) {
        return 3;
    }
    if (ilnk_poll(&cli_ctx) != ILNK_OK) {
        return 4;
    }
    if ((got.frame_type != ILNK_FRAME_RESP_INIT_ACK) ||
        (got.payload.init_ack.server_version != 0x0622U)) {
        return 5;
    }

    printf("ilnk-server self-test PASSED (handled=%lu)\n", (unsigned long)srv_state.handled_cmds);
    return 0;
}

static void print_usage(void) {
    printf("ilnk-server (libinterlnk 0.1.0)\n");
    printf("Usage:\n");
    printf("  ilnk-server --version\n");
    printf("  ilnk-server --self-test\n");
    printf("  ilnk-server --port <device> --baud <rate> --image <disk.img>\n");
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("ilnk-server 0.1.0 (C99 zero-alloc core)\n");
            return 0;
        }
        if (strcmp(argv[i], "--self-test") == 0) {
            return run_self_test();
        }
        if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
            print_usage();
            return 0;
        }
    }

    print_usage();
    return 0;
}
