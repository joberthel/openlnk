/* esp32/include/openlnk_esp32.h
 *
 * Production-ready ESP32 HAL, Server Engine, Client Wrapper & FatFs VFS Mount
 * Bridge for OpenLNK (libopenlnk).
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
 *   5. Caller-owned Thread-Safe High-Level Client Wrapper around olnk_client_t
 *      with static FreeRTOS mutex, auto-reconnect / session resumption, raw sector
 *      streaming backup helper, and idle background keepalive task.
 *   6. FatFs DiskIO & ESP-IDF POSIX VFS Mount Bridge (e.g. "/dos") so remote
 *      MS-DOS INTERSVR.EXE / OPENSVR.EXE / opensvr drives can be accessed via
 *      standard fopen/fread/fwrite/opendir/mkdir/unlink.
 *
 * Strictly ISO C99, zero dynamic allocation (malloc/free), zero floating-point.
 * IMPORTANT: olnk_esp32_server_t (~9.3 KiB) and olnk_esp32_client_t (~8.9 KiB)
 * embed large static frame buffers; ALWAYS allocate statically or in caller-owned
 * global/BSS storage — never on a FreeRTOS task stack.
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
#include "freertos/semphr.h"
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

/* =========================================================================
 * 5. High-Level Thread-Safe ESP32 Client Wrapper & Keepalive Task
 * ========================================================================= */

#define OLNK_ESP32_CLIENT_DEFAULT_SYNC_RETRIES     15U
#define OLNK_ESP32_CLIENT_DEFAULT_MAX_DRIVES       8U
#define OLNK_ESP32_CLIENT_DEFAULT_FIRST_DRIVE      3U   /* D: */
#define OLNK_ESP32_CLIENT_DEFAULT_KEEPALIVE_MS     200U
#define OLNK_ESP32_CLIENT_DEFAULT_TASK_STACK_BYTES 3072U
#define OLNK_ESP32_CLIENT_DEFAULT_TASK_PRIORITY    4U

typedef bool (*olnk_esp32_lock_fn_t)(void *ctx);
typedef void (*olnk_esp32_unlock_fn_t)(void *ctx);

/**
 * Streaming sink callback for olnk_esp32_client_dump_sectors().
 * Return true to continue streaming, false to abort cleanly.
 */
typedef bool (*olnk_esp32_dump_write_fn_t)(
    uint32_t lba,
    const uint8_t *data,
    uint32_t byte_len,
    void *ctx
);

typedef struct {
    uint8_t max_baud_idx;           /* OLNK_BAUD_1200..OLNK_BAUD_115200 (default 7) */
    bool local_requires_echo;       /* Default true (safe for 3-wire MAX3232) */
    bool force_tx_echo;             /* Default false; sets cli.link.peer_echo_forced after connect */
    uint16_t sync_retries;          /* Default 15 */
    uint32_t rpc_timeout_ms;        /* Default OLNK_CLIENT_DEFAULT_RESP_MS (5000 ms) */
    uint8_t max_client_drives;      /* Default 8 */
    uint8_t first_client_drive;     /* Default 3 (D:) */
    bool auto_reconnect;            /* Default true: transparently reconnect & retry RPC once */
    uint32_t keepalive_interval_ms; /* Default 200 ms (0 disables background keepalive) */
    /* Optional injectable mutex callbacks (NULL -> static FreeRTOS mutex on ESP-IDF). */
    olnk_esp32_lock_fn_t lock;
    olnk_esp32_unlock_fn_t unlock;
    void *lock_ctx;
    /* Optional monotonic ms clock override (NULL -> esp_timer_get_time()/1000 on ESP-IDF). */
    olnk_esp32_time_ms_fn_t get_time_ms;
    void *time_ctx;
} olnk_esp32_client_cfg_t;

typedef struct {
    olnk_client_t client; /* ~8.7 KiB: MUST be static / caller-owned BSS, NEVER on task stack! */
    olnk_esp32_client_cfg_t cfg;
    olnk_session_sync_resp_t last_sync;
    bool has_sync;
    bool initialized;

    /* Last RPC/keepalive monotonic timestamp for idle keepalive scheduling */
    uint64_t last_activity_ms;
    bool has_last_activity;

    /* Telemetry counters */
    uint32_t reconnect_count;
    uint32_t keepalive_fail_count;

    /* Mutex & background keepalive task state */
    olnk_esp32_lock_fn_t lock_fn;
    olnk_esp32_unlock_fn_t unlock_fn;
    void *lock_ctx;
#ifdef ESP_PLATFORM
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t mutex_handle;
    TaskHandle_t keepalive_task_handle;
#else
    void *mutex_handle;
    void *keepalive_task_handle;
#endif
    volatile bool keepalive_stop_requested;
    volatile bool keepalive_running;
} olnk_esp32_client_t;

/**
 * Populate cfg with recommended ESP32 client defaults (115200 bps max,
 * echo-paced RX enabled, auto-reconnect enabled, 200 ms keepalive interval).
 */
void olnk_esp32_client_cfg_default(olnk_esp32_client_cfg_t *cfg);

/**
 * Initialize a caller-owned thread-safe ESP32 client wrapper around olnk_client_t.
 * Creates a static FreeRTOS mutex (zero malloc) on ESP-IDF when cfg->lock == NULL.
 */
olnk_status_t olnk_esp32_client_init(
    olnk_esp32_client_t *ecli,
    olnk_transport_t transport,
    const olnk_esp32_client_cfg_t *cfg
);

/**
 * Stop any background keepalive task, disconnect the link cleanly, and release
 * client state.
 */
olnk_status_t olnk_esp32_client_deinit(olnk_esp32_client_t *ecli);

/**
 * Execute Phase A/B connect -> 0x00 SESSION_SYNC -> 0x01 MAP_CONFIG (if fresh
 * session or no cached map), preserving client_id/server_id for resumption.
 */
olnk_status_t olnk_esp32_client_connect_session(olnk_esp32_client_t *ecli);

/**
 * Drop the active link back to 9600 bps (preserving session IDs).
 */
olnk_status_t olnk_esp32_client_disconnect(olnk_esp32_client_t *ecli);

/**
 * Select the first mapped & ready remote server unit (mirrors desktop
 * select_default_unit). Falls back to first mapped unit if none return a BPB.
 */
olnk_status_t olnk_esp32_client_select_default_unit(
    olnk_esp32_client_t *ecli,
    uint8_t *out_unit
);

/**
 * Return true if the server advertised OLNK_MAP_READONLY_BIT (0x80) for unit
 * in the cached MAP_CONFIG drive_map_resp table.
 */
bool olnk_esp32_client_unit_is_readonly(olnk_esp32_client_t *ecli, uint8_t unit);

/**
 * Thread-safe getter for negotiated max_block_size (clamped to sector..8192 B).
 */
uint16_t olnk_esp32_client_max_block_size(olnk_esp32_client_t *ecli);

/* Thread-safe RPC wrappers (with optional auto-reconnect retry on link drop) */
olnk_status_t olnk_esp32_client_get_drive_info(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    olnk_get_drive_info_resp_t *out_resp
);

olnk_status_t olnk_esp32_client_media_check(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint8_t media_descriptor,
    olnk_media_check_resp_t *out_resp
);

olnk_status_t olnk_esp32_client_build_bpb(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint8_t media_descriptor,
    olnk_build_bpb_resp_t *out_resp
);

olnk_status_t olnk_esp32_client_read_sectors(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint8_t media_descriptor,
    uint32_t start_lba,
    uint16_t sector_count,
    uint16_t bytes_per_sector,
    uint8_t *out_buf,
    uint32_t out_cap,
    olnk_sector_io_resp_t *out_resp
);

olnk_status_t olnk_esp32_client_write_sectors(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint8_t media_descriptor,
    uint32_t start_lba,
    uint16_t sector_count,
    uint16_t bytes_per_sector,
    const uint8_t *in_buf,
    bool verify,
    olnk_sector_io_resp_t *out_resp
);

olnk_status_t olnk_esp32_client_get_media_id(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint16_t *out_status,
    olnk_generic_ioctl_mid_t *out_mid
);

olnk_status_t olnk_esp32_client_set_media_id(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    const olnk_generic_ioctl_mid_t *mid,
    uint16_t *out_status
);

olnk_status_t olnk_esp32_client_keepalive(olnk_esp32_client_t *ecli);

/**
 * Single idle-aware keepalive check: sends a keepalive only if at least
 * cfg.keepalive_interval_ms have elapsed since the last RPC/keepalive.
 * Invoked periodically by the background keepalive task and testable on host.
 */
olnk_status_t olnk_esp32_client_keepalive_step(olnk_esp32_client_t *ecli);

/**
 * Stream remote sectors in <= max_block_size chunks (reusing internal static
 * frame storage, zero malloc) to write_cb (e.g. SD card .img backup or HTTP).
 * Pass sector_count == 0 to dump from start_lba to the end of the volume.
 */
olnk_status_t olnk_esp32_client_dump_sectors(
    olnk_esp32_client_t *ecli,
    uint8_t unit,
    uint32_t start_lba,
    uint32_t sector_count,
    olnk_esp32_dump_write_fn_t write_cb,
    void *write_ctx,
    uint32_t *out_sectors_dumped
);

/**
 * Start a lightweight background FreeRTOS keepalive task that periodically
 * calls olnk_esp32_client_keepalive_step() so INTERSVR.EXE / opensvr stays
 * alive during idle periods without starving active VFS I/O.
 * Pass core_id < 0 for tskNO_AFFINITY. Returns OLNK_ERR_PROTOCOL on host builds.
 */
olnk_status_t olnk_esp32_client_start_keepalive_task(
    olnk_esp32_client_t *ecli,
    const char *task_name,
    uint32_t stack_bytes,
    uint32_t priority,
    int core_id
);

/**
 * Request the background keepalive task to stop and optionally wait up to
 * wait_timeout_ms for clean termination.
 */
olnk_status_t olnk_esp32_client_stop_keepalive_task(
    olnk_esp32_client_t *ecli,
    uint32_t wait_timeout_ms
);

/* =========================================================================
 * 6. FatFs DiskIO Core & ESP-IDF POSIX VFS Mount Bridge (e.g. "/dos")
 * ========================================================================= */

#define OLNK_ESP32_VFS_MAX_PATH_LEN      32U
#define OLNK_ESP32_VFS_DEFAULT_MAX_FILES 4U
#define OLNK_ESP32_VFS_PDRV_NONE         0xFFU
#define OLNK_ESP32_UNIT_AUTO             0xFFU

/* Platform-independent FatFs-compatible status flags (match ChaN FatFs STA_*) */
#define OLNK_ESP32_DISK_STA_OK           0x00U
#define OLNK_ESP32_DISK_STA_NOINIT       0x01U
#define OLNK_ESP32_DISK_STA_NODISK       0x02U
#define OLNK_ESP32_DISK_STA_PROTECT      0x04U

/* Platform-independent FatFs-compatible result codes (match ChaN FatFs DRESULT) */
typedef enum {
    OLNK_ESP32_DISK_RES_OK     = 0U,
    OLNK_ESP32_DISK_RES_ERROR  = 1U,
    OLNK_ESP32_DISK_RES_WRPRT  = 2U,
    OLNK_ESP32_DISK_RES_NOTRDY = 3U,
    OLNK_ESP32_DISK_RES_PARERR = 4U
} olnk_esp32_disk_res_t;

/* Platform-independent FatFs-compatible ioctl commands (match ChaN FatFs) */
#define OLNK_ESP32_DISK_CTRL_SYNC        0U
#define OLNK_ESP32_DISK_GET_SECTOR_COUNT 1U
#define OLNK_ESP32_DISK_GET_SECTOR_SIZE  2U
#define OLNK_ESP32_DISK_GET_BLOCK_SIZE   3U

typedef struct {
    const char *base_path; /* VFS mount prefix, e.g. "/dos" (NULL -> "/dos") */
    uint8_t unit;          /* Remote server unit (0=A:, 2=C:, ...), or OLNK_ESP32_UNIT_AUTO */
    bool read_only;        /* Force software read-only mount (default false) */
    bool write_verify;     /* Use opcode 0x06 WRITE_VERIFY for writes (default false) */
    uint8_t max_files;     /* Max simultaneously open POSIX files (0 -> 4) */
} olnk_esp32_vfs_cfg_t;

typedef struct {
    olnk_esp32_client_t *client;
    uint8_t unit;
    bool cfg_read_only;
    bool read_only;        /* Effective RO: cfg_read_only || server OLNK_MAP_READONLY_BIT */
    bool write_verify;
    uint8_t max_files;
    bool disk_ready;
    bool vfs_mounted;

    /* Cached partition-relative BPB geometry from MEDIA_CHECK + BUILD_BPB */
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint8_t media_descriptor;
    uint32_t total_sectors;
    olnk_dos_bpb_t bpb;

    /* ESP-IDF FatFs / VFS binding metadata */
    uint8_t pdrv;
    char base_path[OLNK_ESP32_VFS_MAX_PATH_LEN];
    char fat_drv_str[4];   /* e.g. "0:" or "1:" */
    void *fatfs;           /* Internally FATFS * returned by esp_vfs_fat_register* */

    /* Telemetry counters */
    uint32_t disk_reads;
    uint32_t disk_writes;
    uint32_t disk_errors;
} olnk_esp32_vfs_mount_t;

/**
 * Populate cfg with safe defaults ("/dos", specified unit or OLNK_ESP32_UNIT_AUTO,
 * read_only=false, write_verify=false, max_files=4).
 */
void olnk_esp32_vfs_cfg_default(
    olnk_esp32_vfs_cfg_t *cfg,
    const char *base_path,
    uint8_t unit
);

/**
 * Attach a caller-owned olnk_esp32_vfs_mount_t to a client and config without
 * registering ESP-IDF VFS yet (for direct DiskIO host testing or custom setups).
 */
olnk_status_t olnk_esp32_vfs_disk_attach(
    olnk_esp32_vfs_mount_t *mnt,
    olnk_esp32_client_t *client,
    const olnk_esp32_vfs_cfg_t *cfg
);

/**
 * Platform-independent FatFs DiskIO core entrypoints (called by ESP-IDF
 * ff_diskio_impl_t glue and directly by host CTest unit tests).
 */
uint8_t olnk_esp32_vfs_disk_init(olnk_esp32_vfs_mount_t *mnt);
uint8_t olnk_esp32_vfs_disk_status(const olnk_esp32_vfs_mount_t *mnt);
olnk_esp32_disk_res_t olnk_esp32_vfs_disk_read(
    olnk_esp32_vfs_mount_t *mnt,
    uint8_t *buff,
    uint32_t sector,
    uint32_t count
);
olnk_esp32_disk_res_t olnk_esp32_vfs_disk_write(
    olnk_esp32_vfs_mount_t *mnt,
    const uint8_t *buff,
    uint32_t sector,
    uint32_t count
);
olnk_esp32_disk_res_t olnk_esp32_vfs_disk_ioctl(
    olnk_esp32_vfs_mount_t *mnt,
    uint8_t cmd,
    void *buff
);

/**
 * Mount a remote MS-DOS drive at cfg->base_path (e.g. "/dos") via ESP-IDF
 * FatFs DiskIO + esp_vfs_fat_register_cfg() + f_mount(). Dynamically allocates
 * a free FatFs pdrv so "/sdcard" and "/dos" can coexist simultaneously.
 * On non-ESP host builds, returns OLNK_ERR_PROTOCOL.
 */
olnk_status_t olnk_esp32_vfs_mount(
    olnk_esp32_vfs_mount_t *mnt,
    olnk_esp32_client_t *client,
    const olnk_esp32_vfs_cfg_t *cfg
);

/**
 * Unmount a mounted remote drive cleanly (f_mount(NULL), VFS unregister,
 * ff_diskio_unregister) and reset mount state.
 */
olnk_status_t olnk_esp32_vfs_unmount(olnk_esp32_vfs_mount_t *mnt);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_ESP32_H */
