/* cleanroom/dos/opensvr_main.c
 *
 * 16-bit Real-Mode MS-DOS OPENSVR.EXE clean-room server entry target
 * compiled with Open Watcom v2 (-ms small memory model, 8086/8088 safe).
 */
#include "openlnk/server.h"
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

/* Keep large server/drive buffers static (never on 16-bit Real-Mode stack) */
static olnk_server_t g_dos_server;
static uint8_t g_dos_ramdisk[OLNK_SECTOR_SIZE * 2U];

int main(void) {
    olnk_transport_t tr;
    olnk_mem_drive_t drv;
    const uint8_t sync_probe[3] = { OLNK_SYNC_C1, OLNK_SYNC_C2, OLNK_SYNC_C3 };

    tr.read = dos_null_read;
    tr.write = dos_null_write;
    tr.set_baud = dos_null_set_baud;
    tr.user_data = NULL;

    olnk_fat_build_synthetic_vbr(g_dos_ramdisk, 2UL, 0xF0U, "OPENSVR_DOS", 0x06220001UL);
    olnk_mem_drive_init(&drv, g_dos_ramdisk, 2UL);

    if (olnk_server_init(&g_dos_server, tr, olnk_mem_drive_ops(&drv)) != OLNK_OK) {
        return 1;
    }

    /* Verify Phase A sync state machine transitions cleanly on 16-bit target */
    olnk_server_feed_bytes(&g_dos_server, sync_probe, sizeof(sync_probe));
    if (g_dos_server.link.link_state != (uint8_t)OLNK_LINK_CONNECTED) {
        return 2;
    }

    printf("OPENSVR (libopenlnk 16-bit Real-Mode MS-DOS 6.22 server ready)\n");
    return 0;
}
