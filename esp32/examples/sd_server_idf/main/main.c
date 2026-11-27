/* esp32/examples/sd_server_idf/main/main.c
 *
 * Reference ESP-IDF 5.x OpenLNK (MS-DOS 6.22 INTERSVR-compatible) SD-card
 * serial block server over MAX3232 RS-232.
 *
 * All large protocol/storage contexts are placed in static BSS storage
 * (never on a FreeRTOS task stack).
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "openlnk_esp32.h"

#ifdef CONFIG_OPENLNK_STORAGE_MODE_IMG
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_host.h"
#endif
#endif

static const char *TAG = "openlnk_sd_srv";

/* Caller-owned static contexts (NEVER allocate ~9.2 KiB olnk_esp32_server_t on stack!) */
static olnk_esp32_uart_t g_uart;
static olnk_esp32_server_t g_server;

#ifdef CONFIG_OPENLNK_STORAGE_MODE_RAW
static olnk_esp32_sd_drive_t g_sd_drive;
static olnk_esp32_drive_set_t g_drive_set;
#else
static olnk_esp32_img_drive_t g_img_drive;
static olnk_esp32_img_drive_set_t g_img_set;
static sdmmc_card_t *g_vfs_card = NULL;
#endif

static uint8_t configured_max_baud_idx(void) {
#ifdef CONFIG_OPENLNK_MAX_BAUD_9600
    return (uint8_t)OLNK_BAUD_9600;
#elif defined(CONFIG_OPENLNK_MAX_BAUD_19200)
    return (uint8_t)OLNK_BAUD_19200;
#elif defined(CONFIG_OPENLNK_MAX_BAUD_38400)
    return (uint8_t)OLNK_BAUD_38400;
#elif defined(CONFIG_OPENLNK_MAX_BAUD_57600)
    return (uint8_t)OLNK_BAUD_57600;
#else
    return (uint8_t)OLNK_BAUD_115200;
#endif
}

static void on_server_event(
    const char *event_name,
    uint32_t arg1,
    uint32_t arg2,
    void *user_data
) {
    (void)user_data;
    if (strcmp(event_name, "RPC") == 0) {
        ESP_LOGD(TAG, "RPC op=0x%02" PRIX32 " len=%" PRIu32, arg1, arg2);
        return;
    }
    ESP_LOGI(
        TAG,
        "event=%s arg1=%" PRIu32 " arg2=%" PRIu32,
        event_name,
        arg1,
        arg2
    );
}

#ifdef CONFIG_OPENLNK_STORAGE_MODE_RAW
static bool init_storage(olnk_storage_ops_t *out_ops) {
    olnk_esp32_sd_opts_t opts;
    olnk_status_t st;
    olnk_drive_info_t info;
    char size_str[10];
    char lbl[12];

    olnk_esp32_sd_opts_default(&opts);
    opts.partition_index = (int8_t)CONFIG_OPENLNK_RAW_PARTITION_INDEX;
#ifdef CONFIG_OPENLNK_READ_ONLY
    opts.write_protected = true;
#else
    opts.write_protected = false;
#endif
    opts.cd_gpio = CONFIG_OPENLNK_RAW_CD_GPIO;

#if defined(CONFIG_OPENLNK_SD_BUS_SDMMC) && SOC_SDMMC_HOST_SUPPORTED
    {
        olnk_esp32_sdmmc_cfg_t mmc_cfg;
        olnk_esp32_sdmmc_cfg_default(&mmc_cfg);
#ifdef CONFIG_OPENLNK_SDMMC_4BIT
        mmc_cfg.bus_width = 4U;
#else
        mmc_cfg.bus_width = 1U;
#endif
#if SOC_SDMMC_USE_GPIO_MATRIX
        mmc_cfg.clk_pin = CONFIG_OPENLNK_SDMMC_CLK_GPIO;
        mmc_cfg.cmd_pin = CONFIG_OPENLNK_SDMMC_CMD_GPIO;
        mmc_cfg.d0_pin = CONFIG_OPENLNK_SDMMC_D0_GPIO;
#ifdef CONFIG_OPENLNK_SDMMC_4BIT
        mmc_cfg.d1_pin = CONFIG_OPENLNK_SDMMC_D1_GPIO;
        mmc_cfg.d2_pin = CONFIG_OPENLNK_SDMMC_D2_GPIO;
        mmc_cfg.d3_pin = CONFIG_OPENLNK_SDMMC_D3_GPIO;
#endif
#endif
        ESP_LOGI(TAG, "Initializing raw SD card via SDMMC (%u-bit)...", (unsigned)mmc_cfg.bus_width);
        st = olnk_esp32_sd_init_sdmmc(&g_sd_drive, &mmc_cfg, &opts);
    }
#else
    {
        olnk_esp32_sdspi_cfg_t spi_cfg;
        olnk_esp32_sdspi_cfg_default(
            &spi_cfg,
            CONFIG_OPENLNK_SDSPI_MOSI_GPIO,
            CONFIG_OPENLNK_SDSPI_MISO_GPIO,
            CONFIG_OPENLNK_SDSPI_CLK_GPIO,
            CONFIG_OPENLNK_SDSPI_CS_GPIO
        );
        ESP_LOGI(
            TAG,
            "Initializing raw SD card via SDSPI (MOSI=%d MISO=%d CLK=%d CS=%d)...",
            spi_cfg.mosi_pin,
            spi_cfg.miso_pin,
            spi_cfg.clk_pin,
            spi_cfg.cs_pin
        );
        st = olnk_esp32_sd_init_sdspi(&g_sd_drive, &spi_cfg, &opts);
    }
#endif

    if (st != OLNK_OK) {
        ESP_LOGE(TAG, "SD card initialization failed (status=%d)", (int)st);
        return false;
    }

    olnk_esp32_drive_set_init(&g_drive_set);
    if (!olnk_esp32_drive_set_attach(&g_drive_set, (uint8_t)CONFIG_OPENLNK_DRIVE_UNIT, &g_sd_drive)) {
        ESP_LOGE(TAG, "Failed to attach SD drive to unit %d", CONFIG_OPENLNK_DRIVE_UNIT);
        return false;
    }

    *out_ops = olnk_esp32_drive_set_ops(&g_drive_set);

    if ((out_ops->get_drive_info((uint8_t)CONFIG_OPENLNK_DRIVE_UNIT, &info, out_ops->user_data) &
         OLNK_DOS_STATUS_ERROR) == 0U &&
        info.ready) {
        olnk_fat_format_size_string(info.total_sectors, info.bpb.bytes_per_sector, size_str);
        olnk_label_trim_copy(lbl, sizeof(lbl), info.volume_label, 11U);
        ESP_LOGI(
            TAG,
            "Unit %c: ready | part_lba=%" PRIu32 " sectors=%" PRIu32 " %s | fs=%.8s label='%s' ser=%08" PRIX32 "%s",
            (char)('A' + CONFIG_OPENLNK_DRIVE_UNIT),
            g_sd_drive.partition_lba,
            g_sd_drive.volume_sectors,
            size_str,
            info.filesystem_type,
            lbl,
            info.volume_serial,
            info.write_protected ? " [RO]" : ""
        );
    } else {
        ESP_LOGW(TAG, "SD card initialized, waiting for valid FAT12/FAT16 volume");
    }
    return true;
}
#else
static bool init_storage(olnk_storage_ops_t *out_ops) {
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg;
    esp_err_t ret;
    olnk_status_t st;
    char size_str[10];
    char lbl[12];

    memset(&mount_cfg, 0, sizeof(mount_cfg));
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 4;
    mount_cfg.allocation_unit_size = 16 * 1024;

#if defined(CONFIG_OPENLNK_SD_BUS_SDMMC) && SOC_SDMMC_HOST_SUPPORTED
    {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
#ifdef CONFIG_OPENLNK_SDMMC_4BIT
        slot_cfg.width = 4U;
#else
        slot_cfg.width = 1U;
#endif
        slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#if SOC_SDMMC_USE_GPIO_MATRIX
        if (CONFIG_OPENLNK_SDMMC_CLK_GPIO >= 0) {
            slot_cfg.clk = (gpio_num_t)CONFIG_OPENLNK_SDMMC_CLK_GPIO;
        }
        if (CONFIG_OPENLNK_SDMMC_CMD_GPIO >= 0) {
            slot_cfg.cmd = (gpio_num_t)CONFIG_OPENLNK_SDMMC_CMD_GPIO;
        }
        if (CONFIG_OPENLNK_SDMMC_D0_GPIO >= 0) {
            slot_cfg.d0 = (gpio_num_t)CONFIG_OPENLNK_SDMMC_D0_GPIO;
        }
#ifdef CONFIG_OPENLNK_SDMMC_4BIT
        if (CONFIG_OPENLNK_SDMMC_D1_GPIO >= 0) {
            slot_cfg.d1 = (gpio_num_t)CONFIG_OPENLNK_SDMMC_D1_GPIO;
        }
        if (CONFIG_OPENLNK_SDMMC_D2_GPIO >= 0) {
            slot_cfg.d2 = (gpio_num_t)CONFIG_OPENLNK_SDMMC_D2_GPIO;
        }
        if (CONFIG_OPENLNK_SDMMC_D3_GPIO >= 0) {
            slot_cfg.d3 = (gpio_num_t)CONFIG_OPENLNK_SDMMC_D3_GPIO;
        }
#endif
#endif
        ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &g_vfs_card);
    }
#else
    {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        spi_bus_config_t bus_cfg;
        sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();

        memset(&bus_cfg, 0, sizeof(bus_cfg));
        bus_cfg.mosi_io_num = CONFIG_OPENLNK_SDSPI_MOSI_GPIO;
        bus_cfg.miso_io_num = CONFIG_OPENLNK_SDSPI_MISO_GPIO;
        bus_cfg.sclk_io_num = CONFIG_OPENLNK_SDSPI_CLK_GPIO;
        bus_cfg.quadwp_io_num = -1;
        bus_cfg.quadhd_io_num = -1;
        bus_cfg.max_transfer_sz = 4096;

        ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
            return false;
        }
        slot_cfg.gpio_cs = (gpio_num_t)CONFIG_OPENLNK_SDSPI_CS_GPIO;
        slot_cfg.host_id = (spi_host_device_t)host.slot;
        ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &g_vfs_card);
    }
#endif

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount VFS /sdcard: %s", esp_err_to_name(ret));
        return false;
    }

    st = olnk_esp32_img_open(
        &g_img_drive,
        CONFIG_OPENLNK_IMG_FILE_PATH,
        (int8_t)OLNK_ESP32_PART_AUTO,
#ifdef CONFIG_OPENLNK_READ_ONLY
        true
#else
        false
#endif
    );
    if (st != OLNK_OK) {
        ESP_LOGE(TAG, "Failed to open image '%s' (status=%d)", CONFIG_OPENLNK_IMG_FILE_PATH, (int)st);
        return false;
    }

    olnk_esp32_img_drive_set_init(&g_img_set);
    if (!olnk_esp32_img_drive_set_attach(&g_img_set, (uint8_t)CONFIG_OPENLNK_DRIVE_UNIT, &g_img_drive)) {
        ESP_LOGE(TAG, "Failed to attach image drive to unit %d", CONFIG_OPENLNK_DRIVE_UNIT);
        return false;
    }

    *out_ops = olnk_esp32_img_drive_set_ops(&g_img_set);

    olnk_fat_format_size_string(
        g_img_drive.info.total_sectors,
        g_img_drive.info.bpb.bytes_per_sector,
        size_str
    );
    olnk_label_trim_copy(lbl, sizeof(lbl), g_img_drive.info.volume_label, 11U);
    ESP_LOGI(
        TAG,
        "Unit %c: image '%s'%s | sectors=%" PRIu32 " %s | label='%s' ser=%08" PRIX32 "%s",
        (char)('A' + CONFIG_OPENLNK_DRIVE_UNIT),
        CONFIG_OPENLNK_IMG_FILE_PATH,
        g_img_drive.is_vhd ? " [VHD]" : " [RAW]",
        g_img_drive.volume_sectors,
        size_str,
        lbl,
        g_img_drive.info.volume_serial,
        g_img_drive.write_protected ? " [RO]" : ""
    );
    return true;
}
#endif

void app_main(void) {
    olnk_esp32_uart_cfg_t ucfg;
    olnk_transport_t transport;
    olnk_storage_ops_t storage;
    olnk_esp32_server_cfg_t scfg;
    olnk_status_t st;

    ESP_LOGI(TAG, "OpenLNK ESP32 SD Block Server v%s", OLNK_VERSION_STRING);
    ESP_LOGI(TAG, "Static server footprint: %u bytes", (unsigned)sizeof(g_server));

    if (!init_storage(&storage)) {
        ESP_LOGE(TAG, "Storage startup failed; halting.");
        return;
    }

    olnk_esp32_uart_cfg_default(
        &ucfg,
        CONFIG_OPENLNK_UART_PORT,
        CONFIG_OPENLNK_UART_TX_GPIO,
        CONFIG_OPENLNK_UART_RX_GPIO
    );
#ifdef CONFIG_OPENLNK_UART_HW_FLOW_CTRL
    ucfg.use_hw_flow_control = true;
    ucfg.rts_pin = CONFIG_OPENLNK_UART_RTS_GPIO;
    ucfg.cts_pin = CONFIG_OPENLNK_UART_CTS_GPIO;
#endif

    st = olnk_esp32_uart_init(&g_uart, &ucfg, NULL, &transport);
    if (st != OLNK_OK) {
        ESP_LOGE(TAG, "UART%d init failed (status=%d)", ucfg.uart_port, (int)st);
        return;
    }

    olnk_esp32_server_cfg_default(&scfg);
    scfg.max_baud_idx = configured_max_baud_idx();
#ifdef CONFIG_OPENLNK_ECHO_PACED_RX
    scfg.local_requires_echo = true;
#else
    scfg.local_requires_echo = false;
#endif
    scfg.idle_disconnect_ms = (uint32_t)CONFIG_OPENLNK_IDLE_DISCONNECT_MS;
    scfg.on_event = on_server_event;

    st = olnk_esp32_server_init(&g_server, transport, storage, &scfg);
    if (st != OLNK_OK) {
        ESP_LOGE(TAG, "Server init failed (status=%d)", (int)st);
        return;
    }

    st = olnk_esp32_server_start_task(
        &g_server,
        "openlnk_srv",
        OLNK_ESP32_SERVER_DEFAULT_TASK_STACK_BYTES,
        OLNK_ESP32_SERVER_DEFAULT_TASK_PRIORITY,
        -1
    );
    if (st != OLNK_OK) {
        ESP_LOGE(TAG, "Failed to start server task (status=%d)", (int)st);
        return;
    }

    ESP_LOGI(
        TAG,
        "Listening on UART%d (TX=%d RX=%d, 9600..%" PRIu32 " bps, echo=%s)",
        ucfg.uart_port,
        ucfg.tx_pin,
        ucfg.rx_pin,
        olnk_baud_index_to_bps(scfg.max_baud_idx),
        scfg.local_requires_echo ? "on" : "off"
    );

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000U));
        ESP_LOGI(
            TAG,
            "stats: baud=%" PRIu32 " rpcs=%" PRIu32 " rd_sec=%" PRIu32 " wr_sec=%" PRIu32 " ka=%" PRIu32 " err=%" PRIu32,
            olnk_baud_index_to_bps(g_server.server.link.current_baud_idx),
            g_server.server.rpc_count,
            g_server.server.sectors_read_total,
            g_server.server.sectors_written_total,
            g_server.server.link.keepalives_ok,
            g_server.server.link.frames_rx_err
        );
    }
}
