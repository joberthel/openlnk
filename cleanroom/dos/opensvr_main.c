/* cleanroom/dos/opensvr_main.c
 *
 * 16-bit Real-Mode MS-DOS OPENSVR.EXE clean-room server entry target
 * compiled with Open Watcom v2 (-ms small memory model, 8086/8088 safe).
 */
#include "openlnk/openlnk.h"
#include "openlnk/storage.h"

#include <stdio.h>
#include <string.h>

static int dos_null_read(uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data) {
    (void)buf;
    (void)len;
    (void)timeout_ms;
    (void)user_data;
    return 0;
}

static int dos_null_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data) {
    (void)buf;
    (void)timeout_ms;
    (void)user_data;
    return (int)len;
}

static bool dos_null_set_baud(uint32_t baudrate, void *user_data) {
    (void)baudrate;
    (void)user_data;
    return true;
}

int main(void) {
    static olnk_ctx_t ctx;
    static uint8_t tx_buf[64];
    olnk_transport_t tr;
    olnk_frame_t ack;

    tr.read = dos_null_read;
    tr.write = dos_null_write;
    tr.set_baud = dos_null_set_baud;
    tr.user_data = NULL;

    if (olnk_init(&ctx, tr) != OLNK_OK) {
        return 1;
    }

    memset(&ack, 0, sizeof(ack));
    ack.frame_type = OLNK_FRAME_RESP_INIT_ACK;
    ack.sequence_number = 0U;
    ack.payload.init_ack.server_version = 0x0622U;
    ack.payload.init_ack.drive_count = 1U;
    ack.payload.init_ack.status = 0U;

    if (olnk_send_frame(&ctx, &ack, tx_buf, sizeof(tx_buf), 100U) != OLNK_OK) {
        return 2;
    }

    printf("OPENSVR (libopenlnk 16-bit Real-Mode C99 server ready)\n");
    return 0;
}
