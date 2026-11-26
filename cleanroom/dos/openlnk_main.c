/* cleanroom/dos/openlnk_main.c
 *
 * 16-bit Real-Mode MS-DOS OPENLNK.EXE clean-room client CLI target
 * compiled with Open Watcom v2 (-ms small memory model, 8086/8088 safe).
 */
#include "openlnk/openlnk.h"

#include <stdio.h>
#include <string.h>

static int dos_cli_read(uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data) {
    (void)buf;
    (void)len;
    (void)timeout_ms;
    (void)user_data;
    return 0;
}

static int dos_cli_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data) {
    (void)buf;
    (void)timeout_ms;
    (void)user_data;
    return (int)len;
}

static bool dos_cli_set_baud(uint32_t baudrate, void *user_data) {
    (void)baudrate;
    (void)user_data;
    return true;
}

int main(void) {
    static olnk_ctx_t ctx;
    static uint8_t tx_buf[64];
    olnk_transport_t tr;
    olnk_frame_t req;

    tr.read = dos_cli_read;
    tr.write = dos_cli_write;
    tr.set_baud = dos_cli_set_baud;
    tr.user_data = NULL;

    if (olnk_init(&ctx, tr) != OLNK_OK) {
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.frame_type = OLNK_FRAME_CMD_INIT;
    req.sequence_number = 0U;
    req.payload.init.client_version = 0x0622U;
    req.payload.init.drive_count = 4U;

    if (olnk_send_frame(&ctx, &req, tx_buf, sizeof(tx_buf), 100U) != OLNK_OK) {
        return 2;
    }

    printf("OPENLNK (libopenlnk 16-bit Real-Mode C99 client ready)\n");
    return 0;
}
