/* esp32/examples/dos_client_idf/main/main.c
 *
 * Reference ESP-IDF 5.x OpenLNK MS-DOS Client & FatFs POSIX VFS (/dos) Example.
 *
 * Connects over a 3.3V MAX3232 RS-232 null-modem link to MS-DOS INTERSVR.EXE,
 * clean-room OPENSVR.EXE, or desktop/ESP32 opensvr, starts a lightweight
 * background keepalive task, mounts a remote FAT12/FAT16 drive at "/dos",
 * lists files via standard POSIX opendir()/readdir()/stat(), and optionally
 * reads a sample file via fopen()/fread().
 *
 * All large protocol contexts are placed in static BSS storage (never on a
 * FreeRTOS task stack).
 */
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "openlnk_esp32.h"

static const char *TAG = "openlnk_dos_cli";

/* Caller-owned static contexts (NEVER allocate ~8.9 KiB olnk_esp32_client_t on stack!) */
static olnk_esp32_uart_t g_uart;
static olnk_esp32_client_t g_client;
static olnk_esp32_vfs_mount_t g_vfs_mount;
static char g_path_buf[320];
static char g_file_buf[257];

static uint8_t configured_max_baud_idx(void) {
#ifdef CONFIG_OPENLNK_CLIENT_MAX_BAUD_9600
    return (uint8_t)OLNK_BAUD_9600;
#elif defined(CONFIG_OPENLNK_CLIENT_MAX_BAUD_19200)
    return (uint8_t)OLNK_BAUD_19200;
#elif defined(CONFIG_OPENLNK_CLIENT_MAX_BAUD_38400)
    return (uint8_t)OLNK_BAUD_38400;
#elif defined(CONFIG_OPENLNK_CLIENT_MAX_BAUD_57600)
    return (uint8_t)OLNK_BAUD_57600;
#else
    return (uint8_t)OLNK_BAUD_115200;
#endif
}

static void log_server_info(void) {
    size_t i;

    ESP_LOGI(
        TAG,
        "Connected @ %" PRIu32 " bps | server_id=0x%08" PRIX32 " | max_block=%u | DOS %u.%02u (proto %u.%u)",
        olnk_baud_index_to_bps(g_client.client.link.current_baud_idx),
        g_client.client.link.server_id,
        (unsigned)g_client.client.link.max_block_size,
        (unsigned)g_client.client.map.server_dos_major,
        (unsigned)g_client.client.map.server_dos_minor,
        (unsigned)g_client.client.map.server_proto_major,
        (unsigned)g_client.client.map.server_proto_minor
    );

    ESP_LOGI(TAG, "Server reported %u drive(s):", (unsigned)g_client.client.map.server_drive_count);
    for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
        uint8_t entry = g_client.client.map.drive_map_resp[i];
        uint8_t u;
        olnk_get_drive_info_resp_t ginfo;

        if ((entry == OLNK_MAP_AUTO) || (entry == OLNK_MAP_NONE)) {
            continue;
        }
        u = (uint8_t)(entry & 0x7FU);
        if (u >= OLNK_MAX_DRIVES) {
            continue;
        }
        memset(&ginfo, 0, sizeof(ginfo));
        (void)olnk_esp32_client_get_drive_info(&g_client, u, &ginfo);
        ESP_LOGI(
            TAG,
            "  Slot %u -> Server %c: (unit=%u, attr=0x%04X%s) size='%s' label='%s'",
            (unsigned)i,
            (char)('A' + (int)u),
            (unsigned)u,
            (unsigned)g_client.client.map.server_drive_attrs[u],
            ((entry & OLNK_MAP_READONLY_BIT) != 0U) ? ", RO" : "",
            ginfo.size_string,
            ginfo.volume_label
        );
    }
}

static void list_mounted_root(const char *mount_path) {
    DIR *dir = opendir(mount_path);
    struct dirent *ent;
    uint32_t count = 0UL;

    if (dir == NULL) {
        ESP_LOGE(TAG, "opendir('%s') failed: errno=%d (%s)", mount_path, errno, strerror(errno));
        return;
    }

    ESP_LOGI(TAG, "POSIX directory listing of %s:", mount_path);
    while ((ent = readdir(dir)) != NULL) {
        struct stat st_info;
        int n = snprintf(g_path_buf, sizeof(g_path_buf), "%s/%s", mount_path, ent->d_name);
        if ((n < 0) || ((size_t)n >= sizeof(g_path_buf))) {
            continue;
        }
        if (stat(g_path_buf, &st_info) == 0) {
            if (S_ISDIR(st_info.st_mode)) {
                ESP_LOGI(TAG, "  [DIR]  %s", ent->d_name);
            } else {
                ESP_LOGI(TAG, "  [FILE] %-13s (%lu bytes)", ent->d_name, (unsigned long)st_info.st_size);
            }
        } else {
            ESP_LOGI(TAG, "  [ITEM] %s", ent->d_name);
        }
        count += 1UL;
    }

    (void)closedir(dir);
    ESP_LOGI(TAG, "Listed %" PRIu32 " entry(ies) in %s", count, mount_path);
}

static void read_sample_file(const char *mount_path, const char *rel_name) {
    FILE *fp;
    size_t nread;
    int n;

    if ((rel_name == NULL) || (rel_name[0] == '\0')) {
        return;
    }

    n = snprintf(g_path_buf, sizeof(g_path_buf), "%s/%s", mount_path, rel_name);
    if ((n < 0) || ((size_t)n >= sizeof(g_path_buf))) {
        ESP_LOGW(TAG, "Sample file path too long: %s/%s", mount_path, rel_name);
        return;
    }

    fp = fopen(g_path_buf, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "Sample file '%s' not found (errno=%d)", g_path_buf, errno);
        return;
    }

    nread = fread(g_file_buf, 1U, sizeof(g_file_buf) - 1U, fp);
    g_file_buf[nread] = '\0';
    (void)fclose(fp);

    ESP_LOGI(TAG, "Read %u byte(s) from '%s':", (unsigned)nread, g_path_buf);
    ESP_LOGI(TAG, "--- BEGIN %s ---\n%s\n--- END %s ---", rel_name, g_file_buf, rel_name);
}

void app_main(void) {
    olnk_esp32_uart_cfg_t ucfg;
    olnk_transport_t transport;
    olnk_esp32_client_cfg_t ccfg;
    olnk_esp32_vfs_cfg_t vcfg;
    uint8_t target_unit;
    olnk_status_t st;

    ESP_LOGI(TAG, "OpenLNK ESP32 MS-DOS Client & FatFs VFS Example v%s", OLNK_VERSION_STRING);
    ESP_LOGI(TAG, "Static client footprint: %u bytes", (unsigned)sizeof(g_client));

    olnk_esp32_uart_cfg_default(
        &ucfg,
        CONFIG_OPENLNK_CLIENT_UART_PORT,
        CONFIG_OPENLNK_CLIENT_UART_TX_GPIO,
        CONFIG_OPENLNK_CLIENT_UART_RX_GPIO
    );
#ifdef CONFIG_OPENLNK_CLIENT_UART_HW_FLOW_CTRL
    ucfg.use_hw_flow_control = true;
    ucfg.rts_pin = CONFIG_OPENLNK_CLIENT_UART_RTS_GPIO;
    ucfg.cts_pin = CONFIG_OPENLNK_CLIENT_UART_CTS_GPIO;
#endif

    st = olnk_esp32_uart_init(&g_uart, &ucfg, NULL, &transport);
    if (st != OLNK_OK) {
        ESP_LOGE(TAG, "UART%d init failed (status=%d)", ucfg.uart_port, (int)st);
        return;
    }

    olnk_esp32_client_cfg_default(&ccfg);
    ccfg.max_baud_idx = configured_max_baud_idx();
#ifdef CONFIG_OPENLNK_CLIENT_ECHO_PACED_RX
    ccfg.local_requires_echo = true;
#else
    ccfg.local_requires_echo = false;
#endif
#ifdef CONFIG_OPENLNK_CLIENT_FORCE_TX_ECHO
    ccfg.force_tx_echo = true;
#else
    ccfg.force_tx_echo = false;
#endif
    ccfg.keepalive_interval_ms = (uint32_t)CONFIG_OPENLNK_CLIENT_KEEPALIVE_MS;

    st = olnk_esp32_client_init(&g_client, transport, &ccfg);
    if (st != OLNK_OK) {
        ESP_LOGE(TAG, "Client init failed (status=%d)", (int)st);
        (void)olnk_esp32_uart_deinit(&g_uart);
        return;
    }

    ESP_LOGI(
        TAG,
        "Waiting for INTERSVR.EXE / OPENSVR.EXE on UART%d (TX=%d RX=%d, 9600..%" PRIu32 " bps)...",
        ucfg.uart_port,
        ucfg.tx_pin,
        ucfg.rx_pin,
        olnk_baud_index_to_bps(ccfg.max_baud_idx)
    );

    while ((st = olnk_esp32_client_connect_session(&g_client)) != OLNK_OK) {
        ESP_LOGW(TAG, "Connect attempt failed (status=%d); retrying...", (int)st);
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }

    if (ccfg.keepalive_interval_ms > 0UL) {
        st = olnk_esp32_client_start_keepalive_task(
            &g_client,
            "openlnk_ka",
            OLNK_ESP32_CLIENT_DEFAULT_TASK_STACK_BYTES,
            OLNK_ESP32_CLIENT_DEFAULT_TASK_PRIORITY,
            -1
        );
        if (st != OLNK_OK) {
            ESP_LOGW(TAG, "Background keepalive task start failed (status=%d)", (int)st);
        }
    }

    log_server_info();

    target_unit = (CONFIG_OPENLNK_CLIENT_REMOTE_UNIT < 0)
                      ? OLNK_ESP32_UNIT_AUTO
                      : (uint8_t)CONFIG_OPENLNK_CLIENT_REMOTE_UNIT;

    olnk_esp32_vfs_cfg_default(&vcfg, CONFIG_OPENLNK_CLIENT_MOUNT_POINT, target_unit);
#ifdef CONFIG_OPENLNK_CLIENT_READ_ONLY
    vcfg.read_only = true;
#else
    vcfg.read_only = false;
#endif
#ifdef CONFIG_OPENLNK_CLIENT_WRITE_VERIFY
    vcfg.write_verify = true;
#else
    vcfg.write_verify = false;
#endif

    st = olnk_esp32_vfs_mount(&g_vfs_mount, &g_client, &vcfg);
    if (st != OLNK_OK) {
        ESP_LOGE(
            TAG,
            "Failed to mount remote drive at '%s' (status=%d)",
            vcfg.base_path,
            (int)st
        );
        (void)olnk_esp32_client_deinit(&g_client);
        (void)olnk_esp32_uart_deinit(&g_uart);
        return;
    }

    ESP_LOGI(
        TAG,
        "Mounted remote unit %c: at '%s' (pdrv=%u, sectors=%" PRIu32 ", spc=%u%s)",
        (char)('A' + (int)g_vfs_mount.unit),
        g_vfs_mount.base_path,
        (unsigned)g_vfs_mount.pdrv,
        g_vfs_mount.total_sectors,
        (unsigned)g_vfs_mount.sectors_per_cluster,
        g_vfs_mount.read_only ? ", RO" : ", RW"
    );

    list_mounted_root(g_vfs_mount.base_path);
    read_sample_file(g_vfs_mount.base_path, CONFIG_OPENLNK_CLIENT_SAMPLE_FILE);

#ifdef CONFIG_OPENLNK_CLIENT_UNMOUNT_ON_DONE
    ESP_LOGI(TAG, "Unmounting '%s' and shutting down cleanly...", g_vfs_mount.base_path);
    (void)olnk_esp32_vfs_unmount(&g_vfs_mount);
    (void)olnk_esp32_client_deinit(&g_client);
    (void)olnk_esp32_uart_deinit(&g_uart);
    ESP_LOGI(TAG, "Done.");
#else
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000U));
        ESP_LOGI(
            TAG,
            "stats: baud=%" PRIu32 " rpcs=%" PRIu32 " rd_sec=%" PRIu32 " wr_sec=%" PRIu32 " ka=%" PRIu32 " reconn=%" PRIu32 " vfs_rd=%" PRIu32 " vfs_wr=%" PRIu32,
            olnk_baud_index_to_bps(g_client.client.link.current_baud_idx),
            g_client.client.rpc_count,
            g_client.client.sectors_read_total,
            g_client.client.sectors_written_total,
            g_client.client.link.keepalives_ok,
            g_client.reconnect_count,
            g_vfs_mount.disk_reads,
            g_vfs_mount.disk_writes
        );
    }
#endif
}
