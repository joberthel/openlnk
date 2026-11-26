/* esp32/include/interlnk_esp32.h */
#ifndef INTERLNK_ESP32_H
#define INTERLNK_ESP32_H

#include <stdbool.h>
#include <stdint.h>

#include "interlnk/interlnk.h"
#include "interlnk/storage.h"

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
} ilnk_esp32_uart_cfg_t;

/**
 * Initialize hardware UART (for MAX3232 RS-232 transceiver) with internal
 * ring buffer and optional RTS/CTS flow control at up to 115200 bps.
 */
ilnk_status_t ilnk_esp32_uart_init(
    const ilnk_esp32_uart_cfg_t *cfg,
    ilnk_transport_t *out_transport
);

#ifdef __cplusplus
}
#endif

#endif /* INTERLNK_ESP32_H */
