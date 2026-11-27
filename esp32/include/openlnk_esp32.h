/* esp32/include/openlnk_esp32.h */
#ifndef OPENLNK_ESP32_H
#define OPENLNK_ESP32_H

#include <stdbool.h>
#include <stdint.h>

#include "openlnk/openlnk.h"
#include "openlnk/client.h"
#include "openlnk/server.h"
#include "openlnk/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_port;
    int tx_pin;
    int rx_pin;
    int rts_pin;
    int cts_pin;
    uint32_t baudrate;
    bool use_hw_flow_control;
} olnk_esp32_uart_cfg_t;

/**
 * Initialize hardware UART (for MAX3232 RS-232 transceiver) with internal
 * ring buffer and optional RTS/CTS flow control at up to 115200 bps.
 */
olnk_status_t olnk_esp32_uart_init(
    const olnk_esp32_uart_cfg_t *cfg,
    olnk_transport_t *out_transport
);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_ESP32_H */
