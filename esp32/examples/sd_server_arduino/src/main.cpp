/* esp32/examples/sd_server_arduino/src/main.cpp
 *
 * PlatformIO / Arduino-ESP32 example: Serve a FAT12/FAT16 SD card over
 * MAX3232 RS-232 to an MS-DOS 5.0/6.22 INTERLNK.EXE or OPENLNK.SYS client.
 *
 * All large OpenLNK structures are declared static in BSS — never on the stack.
 */
#include <Arduino.h>
#include <openlnk_esp32.h>

#ifndef OPENLNK_UART_PORT
#define OPENLNK_UART_PORT 1
#endif
#ifndef OPENLNK_UART_TX_PIN
#define OPENLNK_UART_TX_PIN 17
#endif
#ifndef OPENLNK_UART_RX_PIN
#define OPENLNK_UART_RX_PIN 16
#endif

#ifndef OPENLNK_SD_MOSI_PIN
#define OPENLNK_SD_MOSI_PIN 23
#endif
#ifndef OPENLNK_SD_MISO_PIN
#define OPENLNK_SD_MISO_PIN 19
#endif
#ifndef OPENLNK_SD_CLK_PIN
#define OPENLNK_SD_CLK_PIN 18
#endif
#ifndef OPENLNK_SD_CS_PIN
#define OPENLNK_SD_CS_PIN 5
#endif

/* Static BSS storage (~9.8 KiB total; never put olnk_esp32_server_t on stack) */
static olnk_esp32_uart_t g_uart;
static olnk_esp32_sd_drive_t g_sd_drive;
static olnk_esp32_drive_set_t g_drive_set;
static olnk_esp32_server_t g_server;
static bool g_ready = false;

static void on_openlnk_event(
    const char *event_name,
    uint32_t arg1,
    uint32_t arg2,
    void *user_data
) {
    (void)user_data;
    if (strcmp(event_name, "RPC") == 0) {
        return;
    }
    Serial.printf("[openlnk] %s (%lu, %lu)\n",
                  event_name,
                  (unsigned long)arg1,
                  (unsigned long)arg2);
}

void setup() {
    Serial.begin(115200);
    delay(250);
    Serial.printf("\nOpenLNK Arduino SD Server v%s\n", OLNK_VERSION_STRING);

    /* 1. Initialize raw FAT12/FAT16 SD card over SDSPI */
    olnk_esp32_sdspi_cfg_t spi_cfg;
    olnk_esp32_sd_opts_t sd_opts;
    olnk_esp32_sdspi_cfg_default(
        &spi_cfg,
        OPENLNK_SD_MOSI_PIN,
        OPENLNK_SD_MISO_PIN,
        OPENLNK_SD_CLK_PIN,
        OPENLNK_SD_CS_PIN
    );
    olnk_esp32_sd_opts_default(&sd_opts);

    if (olnk_esp32_sd_init_sdspi(&g_sd_drive, &spi_cfg, &sd_opts) != OLNK_OK) {
        Serial.println("[openlnk] ERROR: SD card init failed!");
        return;
    }

    olnk_esp32_drive_set_init(&g_drive_set);
    olnk_esp32_drive_set_attach(&g_drive_set, 2U /* C: */, &g_sd_drive);

    /* 2. Initialize UART1 wired to MAX3232 (3-wire null-modem, 9600..115200 bps) */
    olnk_esp32_uart_cfg_t ucfg;
    olnk_transport_t transport;
    olnk_esp32_uart_cfg_default(
        &ucfg,
        OPENLNK_UART_PORT,
        OPENLNK_UART_TX_PIN,
        OPENLNK_UART_RX_PIN
    );
    if (olnk_esp32_uart_init(&g_uart, &ucfg, NULL, &transport) != OLNK_OK) {
        Serial.println("[openlnk] ERROR: UART init failed!");
        return;
    }

    /* 3. Initialize server engine */
    olnk_esp32_server_cfg_t scfg;
    olnk_esp32_server_cfg_default(&scfg);
    scfg.max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    scfg.local_requires_echo = true;
    scfg.idle_disconnect_ms = 3000U;
    scfg.on_event = on_openlnk_event;

    if (olnk_esp32_server_init(
            &g_server,
            transport,
            olnk_esp32_drive_set_ops(&g_drive_set),
            &scfg) != OLNK_OK) {
        Serial.println("[openlnk] ERROR: Server init failed!");
        return;
    }

    g_ready = true;
    Serial.println("[openlnk] Ready — waiting for MS-DOS INTERLNK.EXE / OPENLNK.SYS...");
}

void loop() {
    if (!g_ready) {
        delay(1000);
        return;
    }
    (void)olnk_esp32_server_step(&g_server, 10U);
}
