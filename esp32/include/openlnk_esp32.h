/* esp32/include/openlnk_esp32.h
 *
 * Production-ready ESP32 HAL & Server Engine for OpenLNK (libopenlnk).
 *
 * Provides:
 *   1. Multi-instance RS-232 / MAX3232 UART transport HAL (1200..115200 bps, 8N1)
 *      with Phase-B TX-drain barrier before baud switches and sub-tick timeout rounding.
 *   2. Zero-alloc Raw SD-Card Block Storage HAL (SDMMC 1/4-bit + SDSPI) with
 *      MBR/superfloppy FAT12/FAT16 auto-detection, write-verify, hot-swap detection,
 *      write-protect, card-detect GPIO, and Generic IOCTL 08h/46h Set Media ID.
 *   3. Disk Image File Storage HAL (.img / Fixed-Size .vhd over POSIX VFS stdio).
 *   4. Caller-owned High-Level Server Engine & optional FreeRTOS task runner
 *      using monotonic esp_timer_get_time() elapsed timing.
 *
 * Strictly ISO C99, zero dynamic allocation (malloc/free), zero floating-point.
 * IMPORTANT: olnk_esp32_server_t embeds olnk_server_t (~9.2 KiB); ALWAYS allocate
 * statically or in caller-owned global/BSS storage — never on a FreeRTOS task stack.
 */
#ifndef OPENLNK_ESP32_H
#define OPENLNK_ESP32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openlnk/openlnk.h"
#include "openlnk/client.h"
#include "openlnk/server.h"
#include "openlnk/storage.h"
#include "openlnk/version.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. Multi-Instance ESP32 UART HAL (MAX3232 RS-232, 1200..115200 bps, 8N1)
 * ========================================================================= */

#define OLNK_ESP32_PIN_UNUSED               (-1)
#define OLNK_ESP32_UART_DEFAULT_RX_BUF_SIZE 4096U
#define OLNK_ESP32_UART_DEFAULT_TX_BUF_SIZE 1024U
#define OLNK_ESP32_UART_TX_FLUSH_TIMEOUT_MS 500U

typedef struct {
    int uart_port;
    int tx_pin;
    int rx_pin;
    int rts_pin;              /* Set OLNK_ESP32_PIN_UNUSED (-1) for 3-wire null-modem */
    int cts_pin;              /* Set OLNK_ESP32_PIN_UNUSED (-1) for 3-wire null-modem */
    uint32_t baudrate;        /* Initial baud rate (0 -> 9600 bps) */
    bool use_hw_flow_control; /* Default false (3-wire TX/RX/GND per protocol spec) */
    uint32_t rx_buf_size;     /* RX ring buffer bytes (0 -> 4096 B) */
    uint32_t tx_buf_size;     /* TX ring buffer bytes (0 -> 1024 B) */
} olnk_esp32_uart_cfg_t;

/**
 * Optional injectable low-level UART driver ops for deterministic host CTest
 * verification without ESP-IDF hardware. Pass NULL on ESP32 to use ESP-IDF UART.
 */
typedef struct {
    int (*install)(
        int uart_port,
        uint32_t baudrate,
        int tx_pin,
        int rx_pin,
        int rts_pin,
        int cts_pin,
        bool use_hw_flow_control,
        uint32_t rx_buf_size,
        uint32_t tx_buf_size,
        void *ctx
    );
    int (*uninstall)(int uart_port, void *ctx);
    /* Return >0 bytes read, 0 on timeout, <0 on fatal error. */
    int (*read_bytes)(
        int uart_port,
        uint8_t *buf,
        size_t len,
        uint32_t wait_ticks,
        void *ctx
    );
    /* Optional: query currently buffered RX byte count (0 on success, <0 on error). */
    int (*rx_buffered_len)(
        int uart_port,
        size_t *out_len,
        void *ctx
    );
    /* Return >=0 bytes written, <0 on fatal error. */
    int (*write_bytes)(
        int uart_port,
        const uint8_t *buf,
        size_t len,
        void *ctx
    );
    /* Block until TX FIFO + shift register are completely empty (0 on ok, <0 on err). */
    int (*wait_tx_done)(
        int uart_port,
        uint32_t wait_ticks,
        void *ctx
    );
    /* Reconfigure UART hardware baud divider (0 on ok, <0 on err). */
    int (*set_baudrate)(
        int uart_port,
        uint32_t baudrate,
        void *ctx
    );
    /* Discard buffered RX bytes (0 on ok, <0 on err). */
    int (*flush_input)(int uart_port, void *ctx);
    /* FreeRTOS tick period in ms (0 -> portTICK_PERIOD_MS on ESP-IDF, 10 ms on host). */
    uint32_t tick_period_ms;
    void *ctx;
} olnk_esp32_uart_ops_t;

typedef struct {
    int uart_port;
    uint32_t current_baud;
    uint32_t rx_buf_size;
    uint32_t tx_buf_size;
    uint32_t tick_period_ms;
    bool use_hw_flow_control;
    bool initialized;
    olnk_esp32_uart_ops_t ops;
    uint32_t tx_bytes_total;
    uint32_t rx_bytes_total;
    uint32_t baud_switches;
    uint32_t tx_flush_calls;
} olnk_esp32_uart_t;

/**
 * Populate cfg with safe 3-wire RS-232 defaults (9600 bps, 8N1, no HW flow ctrl,
 * 4096 B RX / 1024 B TX ring buffers).
 */
void olnk_esp32_uart_cfg_default(
    olnk_esp32_uart_cfg_t *cfg,
    int uart_port,
    int tx_pin,
    int rx_pin
);

/**
 * Convert milliseconds to FreeRTOS ticks with upward rounding so any
 * timeout_ms > 0 yields at least 1 tick (never truncated to 0-tick poll).
 */
uint32_t olnk_esp32_ms_to_ticks(uint32_t timeout_ms, uint32_t tick_period_ms);

/**
 * Initialize a caller-owned ESP32 UART instance and bind it to out_transport.
 * Pass custom_ops == NULL on ESP-IDF to use the native driver/uart backend.
 */
olnk_status_t olnk_esp32_uart_init(
    olnk_esp32_uart_t *uart,
    const olnk_esp32_uart_cfg_t *cfg,
    const olnk_esp32_uart_ops_t *custom_ops,
    olnk_transport_t *out_transport
);

/**
 * Flush pending TX bytes (wait_tx_done) and uninstall the UART driver instance.
 */
olnk_status_t olnk_esp32_uart_deinit(olnk_esp32_uart_t *uart);

/**
 * Discard any unread bytes queued in the UART RX ring buffer.
 */
olnk_status_t olnk_esp32_uart_flush_rx(olnk_esp32_uart_t *uart);

/* =========================================================================
 * 2. Zero-Alloc Raw SD-Card Block Storage HAL (SDMMC / SDSPI / Mockable)
 * ========================================================================= */

#define OLNK_ESP32_PART_AUTO (-1) /* Auto-detect superfloppy VBR or first FAT12/16 MBR partition */

typedef enum {
    OLNK_ESP32_SD_HOST_NONE   = 0U,
    OLNK_ESP32_SD_HOST_EXT    = 1U, /* Caller-owned sdmmc_card_t * */
    OLNK_ESP32_SD_HOST_SDMMC  = 2U, /* Initialized via olnk_esp32_sd_init_sdmmc() */
    OLNK_ESP32_SD_HOST_SDSPI  = 3U, /* Initialized via olnk_esp32_sd_init_sdspi() */
    OLNK_ESP32_SD_HOST_CUSTOM = 4U  /* Injectable block callbacks (host tests) */
} olnk_esp32_sd_host_type_t;

typedef struct {
    int8_t partition_index; /* OLNK_ESP32_PART_AUTO (-1) or explicit MBR slot 0..3 */
    bool write_protected;   /* Software read-only lock (-> 0x8100 WRITE_PROTECT) */
    int cd_gpio;            /* Optional card-detect GPIO (-1 = disabled) */
    bool cd_active_low;     /* True if CD switch pulls GPIO low when card inserted */
    int wp_gpio;            /* Optional write-protect switch GPIO (-1 = disabled) */
    bool wp_active_high;    /* True if WP switch pulls GPIO high when write-protected */
} olnk_esp32_sd_opts_t;

/**
 * Injectable raw 512-byte block I/O callbacks for host testing or custom media.
 */
typedef struct {
    bool (*read_blocks)(
        uint8_t *dst,
        uint32_t start_block,
        uint32_t block_count,
        void *ctx
    );
    bool (*write_blocks)(
        const uint8_t *src,
        uint32_t start_block,
        uint32_t block_count,
        void *ctx
    );
    bool (*is_present)(void *ctx); /* Optional; NULL -> always present */
    uint32_t total_blocks;
    void *ctx;
} olnk_esp32_sd_block_ops_t;

typedef struct {
    int slot;          /* SDMMC_HOST_SLOT_1 (default) or SDMMC_HOST_SLOT_0 */
    uint8_t bus_width; /* 1 or 4 (default 1) */
    int max_freq_khz;  /* 0 -> SDMMC_FREQ_DEFAULT (20000 kHz) */
    int clk_pin;       /* Used only on SoCs with SOC_SDMMC_USE_GPIO_MATRIX (e.g. ESP32-S3) */
    int cmd_pin;
    int d0_pin;
    int d1_pin;
    int d2_pin;
    int d3_pin;
    bool internal_pullups;
} olnk_esp32_sdmmc_cfg_t;

typedef struct {
    int spi_host;      /* SPI2_HOST / SPI3_HOST (0 -> default SPI2_HOST) */
    int mosi_pin;
    int miso_pin;
    int clk_pin;
    int cs_pin;
    int max_freq_khz;  /* 0 -> SDMMC_FREQ_DEFAULT (20000 kHz) */
    bool skip_bus_init; /* Set true if SPI bus is already initialized by caller */
} olnk_esp32_sdspi_cfg_t;

typedef struct {
    olnk_esp32_sd_host_type_t host_type;
    olnk_esp32_sd_block_ops_t custom_ops;
    olnk_esp32_sd_opts_t opts;

#ifdef ESP_PLATFORM
    sdmmc_card_t *card;
    sdmmc_card_t owned_card;
    int sdspi_dev_handle;
    int spi_host_id;
    bool owns_spi_bus;
#else
    void *card;
#endif

    uint32_t raw_sectors;
    uint32_t partition_lba;
    uint32_t volume_sectors;
    olnk_drive_info_t info;

    uint32_t last_partition_lba;
    uint32_t last_volume_serial;
    char last_volume_label[12];
    bool has_last_vbr;
    bool was_absent;
    bool media_changed_pending;
    bool initialized;

    /* Static 512-byte scratch buffer for VBR parsing & WRITE_VERIFY readback */
    uint8_t scratch[OLNK_SECTOR_SIZE];
} olnk_esp32_sd_drive_t;

typedef struct {
    olnk_esp32_sd_drive_t *drives[OLNK_MAX_DRIVES];
    uint8_t drive_count;
} olnk_esp32_drive_set_t;

/**
 * Fill opts with defaults (auto-detect partition, read-write, no CD/WP GPIO).
 */
void olnk_esp32_sd_opts_default(olnk_esp32_sd_opts_t *opts);

/**
 * Fill sdmmc_cfg with safe defaults (Slot 1, 1-bit mode, internal pull-ups,
 * unused matrix pins set to OLNK_ESP32_PIN_UNUSED).
 */
void olnk_esp32_sdmmc_cfg_default(olnk_esp32_sdmmc_cfg_t *cfg);

/**
 * Fill sdspi_cfg with safe defaults (SPI2_HOST, 20 MHz, caller GPIO pins).
 */
void olnk_esp32_sdspi_cfg_default(
    olnk_esp32_sdspi_cfg_t *cfg,
    int mosi_pin,
    int miso_pin,
    int clk_pin,
    int cs_pin
);

/**
 * Initialize an SD block drive using custom/mockable block callbacks (works on
 * both ESP32 and host CTest without ESP-IDF hardware).
 */
olnk_status_t olnk_esp32_sd_init_custom(
    olnk_esp32_sd_drive_t *drv,
    const olnk_esp32_sd_block_ops_t *block_ops,
    const olnk_esp32_sd_opts_t *opts
);

#ifdef ESP_PLATFORM
/**
 * Attach an already-initialized ESP-IDF sdmmc_card_t * (caller retains ownership).
 */
olnk_status_t olnk_esp32_sd_attach_card(
    olnk_esp32_sd_drive_t *drv,
    sdmmc_card_t *card,
    const olnk_esp32_sd_opts_t *opts
);

/**
 * Initialize a raw SD card over SDMMC 1-bit or 4-bit host (ESP32 / ESP32-S3).
 * Returns OLNK_ERR_PROTOCOL on SoCs without SDMMC hardware (e.g. ESP32-C3/C6/S2).
 */
olnk_status_t olnk_esp32_sd_init_sdmmc(
    olnk_esp32_sd_drive_t *drv,
    const olnk_esp32_sdmmc_cfg_t *sdmmc_cfg,
    const olnk_esp32_sd_opts_t *opts
);

/**
 * Initialize a raw SD card over SDSPI (supported on ESP32/S2/S3/C3/C6).
 */
olnk_status_t olnk_esp32_sd_init_sdspi(
    olnk_esp32_sd_drive_t *drv,
    const olnk_esp32_sdspi_cfg_t *sdspi_cfg,
    const olnk_esp32_sd_opts_t *opts
);
#endif /* ESP_PLATFORM */

/**
 * Release any SDMMC/SDSPI resources owned by drv.
 */
void olnk_esp32_sd_deinit(olnk_esp32_sd_drive_t *drv);

/**
 * Manually flag a pending media change (causes next MEDIA_CHECK to re-parse
 * VBR and report OLNK_MEDIA_CHANGED 0xFF once).
 */
void olnk_esp32_sd_notify_media_changed(olnk_esp32_sd_drive_t *drv);

/**
 * Single-drive storage vtable exposing drv as server unit 0 (A:).
 */
olnk_storage_ops_t olnk_esp32_sd_drive_ops(olnk_esp32_sd_drive_t *drv);

/**
 * Multi-drive set helpers (expose up to 26 SD drives/partitions at units 0..25).
 */
void olnk_esp32_drive_set_init(olnk_esp32_drive_set_t *set);
bool olnk_esp32_drive_set_attach(
    olnk_esp32_drive_set_t *set,
    uint8_t unit,
    olnk_esp32_sd_drive_t *drv
);
olnk_storage_ops_t olnk_esp32_drive_set_ops(olnk_esp32_drive_set_t *set);

/* =========================================================================
 * 3. Disk Image File Storage HAL (.img / Fixed-Size .vhd via POSIX stdio)
 * ========================================================================= */

typedef struct {
    void *file_handle;      /* Internally FILE * (kept opaque to avoid stdio.h here) */
    uint32_t raw_sectors;
    uint32_t partition_lba;
    uint32_t volume_sectors;
    int8_t partition_index;
    bool is_vhd;
    bool write_protected;
    bool present;
    bool media_changed_pending;
    olnk_drive_info_t info;
    uint8_t scratch[OLNK_SECTOR_SIZE];
} olnk_esp32_img_drive_t;

typedef struct {
    olnk_esp32_img_drive_t *drives[OLNK_MAX_DRIVES];
    uint8_t drive_count;
} olnk_esp32_img_drive_set_t;

/**
 * Open a raw FAT12/FAT16 .img or Fixed-Size .vhd file from a mounted filesystem
 * (e.g. "/sdcard/dos_c.vhd" on ESP-IDF VFS or any host path in CTest).
 * Automatically strips a trailing 512-byte Fixed-VHD "conectix" footer and
 * detects MBR partition vs. superfloppy VBR.
 */
olnk_status_t olnk_esp32_img_open(
    olnk_esp32_img_drive_t *drv,
    const char *path,
    int8_t partition_index,
    bool read_only
);

/**
 * Swap the backing disk image file on an existing drive slot, re-parse its VBR,
 * and mark OLNK_MEDIA_CHANGED (0xFF) for the next MEDIA_CHECK.
 */
olnk_status_t olnk_esp32_img_swap(
    olnk_esp32_img_drive_t *drv,
    const char *new_path,
    int8_t partition_index,
    bool read_only
);

/**
 * Flush and close the underlying disk image file.
 */
void olnk_esp32_img_close(olnk_esp32_img_drive_t *drv);

/**
 * Single-drive and multi-drive storage vtables for disk image files.
 */
olnk_storage_ops_t olnk_esp32_img_drive_ops(olnk_esp32_img_drive_t *drv);
void olnk_esp32_img_drive_set_init(olnk_esp32_img_drive_set_t *set);
bool olnk_esp32_img_drive_set_attach(
    olnk_esp32_img_drive_set_t *set,
    uint8_t unit,
    olnk_esp32_img_drive_t *drv
);
olnk_storage_ops_t olnk_esp32_img_drive_set_ops(olnk_esp32_img_drive_set_t *set);

/* =========================================================================
 * 4. High-Level ESP32 Server Engine & Optional FreeRTOS Task Runner
 * ========================================================================= */

#define OLNK_ESP32_SERVER_DEFAULT_IDLE_DISCONNECT_MS 3000U
#define OLNK_ESP32_SERVER_DEFAULT_POLL_TIMEOUT_MS    10U
#define OLNK_ESP32_SERVER_DEFAULT_TASK_STACK_BYTES   4096U
#define OLNK_ESP32_SERVER_DEFAULT_TASK_PRIORITY      5U

typedef uint64_t (*olnk_esp32_time_ms_fn_t)(void *ctx);

typedef struct {
    uint8_t max_baud_idx;         /* OLNK_BAUD_1200..OLNK_BAUD_115200 (default 7) */
    bool local_requires_echo;     /* Default true (recommended for 3-wire MAX3232) */
    uint32_t idle_disconnect_ms;  /* Default 3000 ms (0 disables idle drop) */
    uint32_t poll_timeout_ms;     /* Default 10 ms */
    olnk_server_event_cb_t on_event;
    void *on_event_user_data;
    /* Optional monotonic ms clock override (NULL -> esp_timer_get_time()/1000 on ESP32). */
    olnk_esp32_time_ms_fn_t get_time_ms;
    void *time_ctx;
} olnk_esp32_server_cfg_t;

typedef struct {
    olnk_server_t server; /* ~9.2 KiB: MUST be static / caller-owned, NEVER on task stack! */
    uint32_t poll_timeout_ms;
    olnk_esp32_time_ms_fn_t get_time_ms;
    void *time_ctx;
    uint64_t last_tick_ms;
    bool has_last_tick;
    volatile bool stop_requested;
    volatile bool running;
#ifdef ESP_PLATFORM
    TaskHandle_t task_handle;
#else
    void *task_handle;
#endif
} olnk_esp32_server_t;

/**
 * Populate cfg with recommended ESP32 server defaults (115200 bps max,
 * echo-paced RX enabled, 3000 ms idle disconnect, 10 ms poll interval).
 */
void olnk_esp32_server_cfg_default(olnk_esp32_server_cfg_t *cfg);

/**
 * Initialize a caller-owned olnk_esp32_server_t wrapper around olnk_server_t.
 */
olnk_status_t olnk_esp32_server_init(
    olnk_esp32_server_t *esrv,
    olnk_transport_t transport,
    olnk_storage_ops_t storage,
    const olnk_esp32_server_cfg_t *cfg
);

/**
 * Execute one poll/dispatch/monotonic-timer step.
 * Suitable for cooperative loops (e.g. Arduino loop()) or deterministic tests.
 */
olnk_status_t olnk_esp32_server_step(
    olnk_esp32_server_t *esrv,
    uint32_t poll_timeout_ms
);

/**
 * Blocking server loop until olnk_esp32_server_stop() is called or a fatal
 * transport error occurs.
 */
olnk_status_t olnk_esp32_server_run(olnk_esp32_server_t *esrv);

/**
 * Launch a FreeRTOS task running olnk_esp32_server_run(esrv).
 * Passes caller-owned esrv as the task parameter (never allocates olnk_server_t
 * on the task stack). Pass core_id < 0 for tskNO_AFFINITY.
 * On non-ESP-IDF host builds, returns OLNK_ERR_PROTOCOL.
 */
olnk_status_t olnk_esp32_server_start_task(
    olnk_esp32_server_t *esrv,
    const char *task_name,
    uint32_t stack_bytes,
    uint32_t priority,
    int core_id
);

/**
 * Request the running server loop/task to stop and optionally wait up to
 * wait_timeout_ms for clean termination.
 */
olnk_status_t olnk_esp32_server_stop(
    olnk_esp32_server_t *esrv,
    uint32_t wait_timeout_ms
);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_ESP32_H */
