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

static olnk_ctx_t g_dos_client;
static uint8_t g_dos_wire_buf[64];

int main(void) {
    olnk_transport_t tr;
    olnk_sync_trigger_t trig = { OLNK_OP_SESSION_SYNC, 0U };
    uint8_t payload[OLNK_LEN_SYNC_TRIGGER];
    size_t payload_len = 0U;
    size_t wire_len = 0U;
    olnk_wire_frame_t decoded;

    tr.read = dos_cli_read;
    tr.write = dos_cli_write;
    tr.set_baud = dos_cli_set_baud;
    tr.user_data = NULL;

    if (olnk_init(&g_dos_client, tr) != OLNK_OK) {
        return 1;
    }

    if (olnk_encode_sync_trigger(&trig, payload, sizeof(payload), &payload_len) != OLNK_OK) {
        return 2;
    }
    if (olnk_wire_frame_encode(0U, false, false, payload, (uint16_t)payload_len,
                               g_dos_wire_buf, sizeof(g_dos_wire_buf), &wire_len) != OLNK_OK) {
        return 3;
    }
    if (olnk_wire_frame_decode(g_dos_wire_buf, wire_len, &decoded, NULL) != OLNK_OK) {
        return 4;
    }

    printf("OPENLNK (libopenlnk 16-bit Real-Mode MS-DOS 6.22 client ready)\n");
    return 0;
}
