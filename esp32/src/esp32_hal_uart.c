/* esp32/src/esp32_hal_uart.c
 *
 * Production-ready, multi-instance ESP32 UART HAL for MAX3232 RS-232 links
 * (1200..115200 bps, 8N1, default 3-wire null-modem).
 *
 * Key invariants:
 *   - Caller-owned olnk_esp32_uart_t (zero static mutable globals, multi-instance).
 *   - Phase-B baud switching race prevention: ALWAYS drains pending TX via
 *     wait_tx_done() strictly BEFORE calling set_baudrate().
 *   - Sub-tick timeout rounding: timeout_ms > 0 always rounds up to >= 1 tick.
 *   - Low-latency 1-byte wakeup + opportunistic burst drain (avoids multi-ms
 *     stalls in MS-DOS INTERLNK Echo-Paced mode).
 *   - Injectable olnk_esp32_uart_ops_t for deterministic host CTest verification.
 *   - Strictly ISO C99, zero dynamic allocation, zero floating-point.
 */
#include "openlnk_esp32.h"

#include <limits.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define OLNK_ESP32_DEFAULT_TICK_MS 10U

/* =========================================================================
 * Tick Conversion Helper (Sub-Tick Safe, Overflow Safe)
 * ========================================================================= */

uint32_t olnk_esp32_ms_to_ticks(uint32_t timeout_ms, uint32_t tick_period_ms) {
    uint32_t period = tick_period_ms;
    uint32_t add;

    if (timeout_ms == 0UL) {
        return 0UL;
    }
    if (period == 0UL) {
#ifdef ESP_PLATFORM
        period = (uint32_t)portTICK_PERIOD_MS;
        if (period == 0UL) {
            period = 1UL;
        }
#else
        period = OLNK_ESP32_DEFAULT_TICK_MS;
#endif
    }

    add = period - 1UL;
    if (timeout_ms > (UINT32_MAX - add)) {
        return (UINT32_MAX / period);
    }
    return (timeout_ms + add) / period;
}

/* =========================================================================
 * Native ESP-IDF 5.x UART Driver Operations
 * ========================================================================= */

#ifdef ESP_PLATFORM
static int idf_uart_install(
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
) {
    uart_config_t ucfg;
    int eff_tx;
    int eff_rx;
    int eff_rts;
    int eff_cts;
    (void)ctx;

    memset(&ucfg, 0, sizeof(ucfg));
    ucfg.baud_rate = (int)baudrate;
    ucfg.data_bits = UART_DATA_8_BITS;
    ucfg.parity = UART_PARITY_DISABLE;
    ucfg.stop_bits = UART_STOP_BITS_1;
    ucfg.flow_ctrl = use_hw_flow_control
                         ? UART_HW_FLOWCTRL_CTS_RTS
                         : UART_HW_FLOWCTRL_DISABLE;
    ucfg.rx_flow_ctrl_thresh = 112U;
    ucfg.source_clk = UART_SCLK_DEFAULT;

    if (uart_driver_install(
            (uart_port_t)uart_port,
            (int)rx_buf_size,
            (int)tx_buf_size,
            0,
            NULL,
            0) != ESP_OK) {
        return -1;
    }

    if (uart_param_config((uart_port_t)uart_port, &ucfg) != ESP_OK) {
        (void)uart_driver_delete((uart_port_t)uart_port);
        return -2;
    }

    eff_tx = (tx_pin < 0) ? UART_PIN_NO_CHANGE : tx_pin;
    eff_rx = (rx_pin < 0) ? UART_PIN_NO_CHANGE : rx_pin;
    eff_rts = (use_hw_flow_control && (rts_pin >= 0)) ? rts_pin : UART_PIN_NO_CHANGE;
    eff_cts = (use_hw_flow_control && (cts_pin >= 0)) ? cts_pin : UART_PIN_NO_CHANGE;

    if (uart_set_pin((uart_port_t)uart_port, eff_tx, eff_rx, eff_rts, eff_cts) != ESP_OK) {
        (void)uart_driver_delete((uart_port_t)uart_port);
        return -3;
    }

    /*
     * Use a 1-byte RX FIFO full threshold so single-byte Echo-Paced transfers
     * and Phase A/B handshakes wake the server immediately without waiting for
     * the hardware RX timeout (~10 symbol periods).
     */
    (void)uart_set_rx_full_threshold((uart_port_t)uart_port, 1);
    (void)uart_flush_input((uart_port_t)uart_port);
    return 0;
}

static int idf_uart_uninstall(int uart_port, void *ctx) {
    (void)ctx;
    return (uart_driver_delete((uart_port_t)uart_port) == ESP_OK) ? 0 : -1;
}

static int idf_uart_read_bytes(
    int uart_port,
    uint8_t *buf,
    size_t len,
    uint32_t wait_ticks,
    void *ctx
) {
    (void)ctx;
    return uart_read_bytes(
        (uart_port_t)uart_port,
        buf,
        (uint32_t)len,
        (TickType_t)wait_ticks
    );
}

static int idf_uart_rx_buffered_len(int uart_port, size_t *out_len, void *ctx) {
    (void)ctx;
    return (uart_get_buffered_data_len((uart_port_t)uart_port, out_len) == ESP_OK) ? 0 : -1;
}

static int idf_uart_write_bytes(
    int uart_port,
    const uint8_t *buf,
    size_t len,
    void *ctx
) {
    (void)ctx;
    return uart_write_bytes((uart_port_t)uart_port, buf, len);
}

static int idf_uart_wait_tx_done(int uart_port, uint32_t wait_ticks, void *ctx) {
    (void)ctx;
    return (uart_wait_tx_done((uart_port_t)uart_port, (TickType_t)wait_ticks) == ESP_OK) ? 0 : -1;
}

static int idf_uart_set_baudrate(int uart_port, uint32_t baudrate, void *ctx) {
    (void)ctx;
    return (uart_set_baudrate((uart_port_t)uart_port, baudrate) == ESP_OK) ? 0 : -1;
}

static int idf_uart_flush_input(int uart_port, void *ctx) {
    (void)ctx;
    return (uart_flush_input((uart_port_t)uart_port) == ESP_OK) ? 0 : -1;
}

static olnk_esp32_uart_ops_t make_idf_uart_ops(void) {
    olnk_esp32_uart_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.install = idf_uart_install;
    ops.uninstall = idf_uart_uninstall;
    ops.read_bytes = idf_uart_read_bytes;
    ops.rx_buffered_len = idf_uart_rx_buffered_len;
    ops.write_bytes = idf_uart_write_bytes;
    ops.wait_tx_done = idf_uart_wait_tx_done;
    ops.set_baudrate = idf_uart_set_baudrate;
    ops.flush_input = idf_uart_flush_input;
    ops.tick_period_ms = (uint32_t)portTICK_PERIOD_MS;
    ops.ctx = NULL;
    return ops;
}
#endif /* ESP_PLATFORM */

/* =========================================================================
 * libopenlnk Transport Callbacks
 * ========================================================================= */

static int esp32_transport_read(
    uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    void *user_data
) {
    olnk_esp32_uart_t *uart = (olnk_esp32_uart_t *)user_data;
    size_t req_len;
    uint32_t wait_ticks;
    int n;

    if ((uart == NULL) || (!uart->initialized) || (uart->ops.read_bytes == NULL)) {
        return -1;
    }
    if ((buf == NULL) || (len == 0U)) {
        return 0;
    }

    req_len = (len > (size_t)OLNK_MAX_PAYLOAD_SIZE)
                  ? (size_t)OLNK_MAX_PAYLOAD_SIZE
                  : len;
    wait_ticks = olnk_esp32_ms_to_ticks(timeout_ms, uart->tick_period_ms);

    /* Non-blocking poll: read whatever is currently queued without blocking */
    if (wait_ticks == 0UL) {
        size_t to_read = req_len;
        if (uart->ops.rx_buffered_len != NULL) {
            size_t avail = 0U;
            if (uart->ops.rx_buffered_len(uart->uart_port, &avail, uart->ops.ctx) == 0) {
                if (avail == 0U) {
                    return 0;
                }
                if (avail < to_read) {
                    to_read = avail;
                }
            }
        }
        n = uart->ops.read_bytes(uart->uart_port, buf, to_read, 0UL, uart->ops.ctx);
        if (n > 0) {
            uart->rx_bytes_total += (uint32_t)n;
        }
        return n;
    }

    /* Fast path: if bytes are already buffered, return immediately without sleeping */
    if (uart->ops.rx_buffered_len != NULL) {
        size_t avail = 0U;
        if ((uart->ops.rx_buffered_len(uart->uart_port, &avail, uart->ops.ctx) == 0) &&
            (avail > 0U)) {
            size_t to_read = (avail < req_len) ? avail : req_len;
            n = uart->ops.read_bytes(uart->uart_port, buf, to_read, 0UL, uart->ops.ctx);
            if (n > 0) {
                uart->rx_bytes_total += (uint32_t)n;
            }
            return n;
        }
    }

    /*
     * Block efficiently for the first byte only so single-byte Echo-Paced /
     * sync / keepalive exchanges never sleep through the remainder of wait_ticks.
     */
    n = uart->ops.read_bytes(uart->uart_port, buf, 1U, wait_ticks, uart->ops.ctx);
    if (n <= 0) {
        return n;
    }

    /* Opportunistically drain any additional already-buffered bytes (0-tick) */
    if (req_len > 1U) {
        size_t extra_req = req_len - 1U;
        bool try_extra = true;

        if (uart->ops.rx_buffered_len != NULL) {
            size_t avail = 0U;
            if (uart->ops.rx_buffered_len(uart->uart_port, &avail, uart->ops.ctx) == 0) {
                if (avail == 0U) {
                    try_extra = false;
                } else if (avail < extra_req) {
                    extra_req = avail;
                }
            }
        }

        if (try_extra) {
            int extra = uart->ops.read_bytes(
                uart->uart_port,
                &buf[1],
                extra_req,
                0UL,
                uart->ops.ctx
            );
            if (extra > 0) {
                n += extra;
            }
        }
    }

    uart->rx_bytes_total += (uint32_t)n;
    return n;
}

static int esp32_transport_write(
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    void *user_data
) {
    olnk_esp32_uart_t *uart = (olnk_esp32_uart_t *)user_data;
    size_t req_len;
    int written;

    if ((uart == NULL) || (!uart->initialized) || (uart->ops.write_bytes == NULL)) {
        return -1;
    }
    if ((buf == NULL) || (len == 0U)) {
        return 0;
    }

    req_len = (len > (size_t)OLNK_MAX_PAYLOAD_SIZE)
                  ? (size_t)OLNK_MAX_PAYLOAD_SIZE
                  : len;
    written = uart->ops.write_bytes(uart->uart_port, buf, req_len, uart->ops.ctx);
    if (written < 0) {
        return -1;
    }
    if (written > 0) {
        uart->tx_bytes_total += (uint32_t)written;
    }

    /* If TX ring buffer is disabled (tx_buf_size == 0) and a timeout was given,
     * ensure blocking drain respects the configured timeout. */
    if ((uart->tx_buf_size == 0U) && (timeout_ms > 0UL) && (uart->ops.wait_tx_done != NULL)) {
        uint32_t wait_ticks = olnk_esp32_ms_to_ticks(timeout_ms, uart->tick_period_ms);
        (void)uart->ops.wait_tx_done(uart->uart_port, wait_ticks, uart->ops.ctx);
    }

    return written;
}

static bool esp32_transport_set_baud(uint32_t baudrate, void *user_data) {
    olnk_esp32_uart_t *uart = (olnk_esp32_uart_t *)user_data;
    uint8_t baud_idx = 0U;

    if ((uart == NULL) || (!uart->initialized) || (uart->ops.set_baudrate == NULL)) {
        return false;
    }
    if (!olnk_bps_to_baud_index(baudrate, &baud_idx)) {
        return false;
    }

    /*
     * Phase-B baud switching barrier: drain all pending TX bytes out of the
     * UART ring buffer + hardware shift register strictly BEFORE switching the
     * baud divider so outgoing target-baud indices and beacons are never
     * corrupted mid-frame.
     */
    if (uart->ops.wait_tx_done != NULL) {
        uint32_t flush_ticks = olnk_esp32_ms_to_ticks(
            OLNK_ESP32_UART_TX_FLUSH_TIMEOUT_MS,
            uart->tick_period_ms
        );
        uart->tx_flush_calls += 1UL;
        if (uart->ops.wait_tx_done(uart->uart_port, flush_ticks, uart->ops.ctx) != 0) {
            return false;
        }
    }

    if (uart->ops.set_baudrate(uart->uart_port, baudrate, uart->ops.ctx) != 0) {
        return false;
    }

    uart->current_baud = baudrate;
    uart->baud_switches += 1UL;
    return true;
}

/* =========================================================================
 * Public ESP32 UART HAL API
 * ========================================================================= */

void olnk_esp32_uart_cfg_default(
    olnk_esp32_uart_cfg_t *cfg,
    int uart_port,
    int tx_pin,
    int rx_pin
) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->uart_port = uart_port;
    cfg->tx_pin = tx_pin;
    cfg->rx_pin = rx_pin;
    cfg->rts_pin = OLNK_ESP32_PIN_UNUSED;
    cfg->cts_pin = OLNK_ESP32_PIN_UNUSED;
    cfg->baudrate = 9600UL;
    cfg->use_hw_flow_control = false;
    cfg->rx_buf_size = OLNK_ESP32_UART_DEFAULT_RX_BUF_SIZE;
    cfg->tx_buf_size = OLNK_ESP32_UART_DEFAULT_TX_BUF_SIZE;
}

olnk_status_t olnk_esp32_uart_init(
    olnk_esp32_uart_t *uart,
    const olnk_esp32_uart_cfg_t *cfg,
    const olnk_esp32_uart_ops_t *custom_ops,
    olnk_transport_t *out_transport
) {
    uint32_t init_baud;
    uint32_t rx_buf;
    uint32_t tx_buf;
    uint8_t baud_idx = 0U;

    if ((uart == NULL) || (cfg == NULL) || (out_transport == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (cfg->uart_port < 0) {
        return OLNK_ERR_INVALID_ARG;
    }

    init_baud = (cfg->baudrate != 0UL) ? cfg->baudrate : 9600UL;
    if (!olnk_bps_to_baud_index(init_baud, &baud_idx)) {
        return OLNK_ERR_INVALID_ARG;
    }

    rx_buf = (cfg->rx_buf_size != 0UL)
                 ? cfg->rx_buf_size
                 : OLNK_ESP32_UART_DEFAULT_RX_BUF_SIZE;
    tx_buf = (cfg->tx_buf_size != 0UL)
                 ? cfg->tx_buf_size
                 : OLNK_ESP32_UART_DEFAULT_TX_BUF_SIZE;

    memset(uart, 0, sizeof(*uart));
    uart->uart_port = cfg->uart_port;
    uart->current_baud = init_baud;
    uart->rx_buf_size = rx_buf;
    uart->tx_buf_size = tx_buf;
    uart->use_hw_flow_control = cfg->use_hw_flow_control;

    if (custom_ops != NULL) {
        uart->ops = *custom_ops;
    } else {
#ifdef ESP_PLATFORM
        uart->ops = make_idf_uart_ops();
#else
        return OLNK_ERR_HAL_IO;
#endif
    }

    if ((uart->ops.read_bytes == NULL) || (uart->ops.write_bytes == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    if (uart->ops.tick_period_ms != 0UL) {
        uart->tick_period_ms = uart->ops.tick_period_ms;
    } else {
#ifdef ESP_PLATFORM
        uart->tick_period_ms = (uint32_t)portTICK_PERIOD_MS;
        if (uart->tick_period_ms == 0UL) {
            uart->tick_period_ms = 1UL;
        }
#else
        uart->tick_period_ms = OLNK_ESP32_DEFAULT_TICK_MS;
#endif
    }

    if (uart->ops.install != NULL) {
        if (uart->ops.install(
                uart->uart_port,
                init_baud,
                cfg->tx_pin,
                cfg->rx_pin,
                cfg->rts_pin,
                cfg->cts_pin,
                cfg->use_hw_flow_control,
                rx_buf,
                tx_buf,
                uart->ops.ctx) != 0) {
            return OLNK_ERR_HAL_IO;
        }
    }

    uart->initialized = true;

    out_transport->read = esp32_transport_read;
    out_transport->write = esp32_transport_write;
    out_transport->set_baud = esp32_transport_set_baud;
    out_transport->user_data = uart;
    return OLNK_OK;
}

olnk_status_t olnk_esp32_uart_deinit(olnk_esp32_uart_t *uart) {
    olnk_status_t st = OLNK_OK;

    if (uart == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (!uart->initialized) {
        return OLNK_OK;
    }

    if (uart->ops.wait_tx_done != NULL) {
        uint32_t flush_ticks = olnk_esp32_ms_to_ticks(
            OLNK_ESP32_UART_TX_FLUSH_TIMEOUT_MS,
            uart->tick_period_ms
        );
        uart->tx_flush_calls += 1UL;
        (void)uart->ops.wait_tx_done(uart->uart_port, flush_ticks, uart->ops.ctx);
    }

    if (uart->ops.uninstall != NULL) {
        if (uart->ops.uninstall(uart->uart_port, uart->ops.ctx) != 0) {
            st = OLNK_ERR_HAL_IO;
        }
    }

    uart->initialized = false;
    return st;
}

olnk_status_t olnk_esp32_uart_flush_rx(olnk_esp32_uart_t *uart) {
    if ((uart == NULL) || (!uart->initialized)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (uart->ops.flush_input != NULL) {
        if (uart->ops.flush_input(uart->uart_port, uart->ops.ctx) != 0) {
            return OLNK_ERR_HAL_IO;
        }
    }
    return OLNK_OK;
}
