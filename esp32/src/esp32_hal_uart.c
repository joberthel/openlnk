/* esp32/src/esp32_hal_uart.c */
#include "openlnk_esp32.h"

#ifdef ESP_PLATFORM
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OLNK_ESP32_UART_RX_RING_SIZE 4096

static int esp32_uart_read(uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data) {
    int port = *(const int *)user_data;
    return uart_read_bytes((uart_port_t)port, buf, len, pdMS_TO_TICKS(timeout_ms));
}

static int esp32_uart_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data) {
    int port = *(const int *)user_data;
    (void)timeout_ms;
    return uart_write_bytes((uart_port_t)port, (const char *)buf, len);
}

static bool esp32_uart_set_baud(uint32_t baudrate, void *user_data) {
    int port = *(const int *)user_data;
    return uart_set_baudrate((uart_port_t)port, baudrate) == ESP_OK;
}

static int g_esp32_uart_port = 1;

olnk_status_t olnk_esp32_uart_init(
    const olnk_esp32_uart_cfg_t *cfg,
    olnk_transport_t *out_transport
) {
    uart_config_t ucfg;
    if ((cfg == NULL) || (out_transport == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    ucfg.baud_rate = (int)cfg->baudrate;
    ucfg.data_bits = UART_DATA_8_BITS;
    ucfg.parity = UART_PARITY_DISABLE;
    ucfg.stop_bits = UART_STOP_BITS_1;
    ucfg.flow_ctrl = cfg->use_hw_flow_control ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE;
    ucfg.rx_flow_ctrl_thresh = 112;
    ucfg.source_clk = UART_SCLK_DEFAULT;

    g_esp32_uart_port = cfg->uart_port;
    if (uart_driver_install((uart_port_t)cfg->uart_port, OLNK_ESP32_UART_RX_RING_SIZE, 0, 0, NULL, 0) != ESP_OK) {
        return OLNK_ERR_HAL_TIMEOUT;
    }
    if (uart_param_config((uart_port_t)cfg->uart_port, &ucfg) != ESP_OK) {
        return OLNK_ERR_INVALID_ARG;
    }
    if (uart_set_pin((uart_port_t)cfg->uart_port, cfg->tx_pin, cfg->rx_pin, cfg->rts_pin, cfg->cts_pin) != ESP_OK) {
        return OLNK_ERR_INVALID_ARG;
    }

    out_transport->read = esp32_uart_read;
    out_transport->write = esp32_uart_write;
    out_transport->set_baud = esp32_uart_set_baud;
    out_transport->user_data = &g_esp32_uart_port;
    return OLNK_OK;
}
#else
olnk_status_t olnk_esp32_uart_init(
    const olnk_esp32_uart_cfg_t *cfg,
    olnk_transport_t *out_transport
) {
    (void)cfg;
    (void)out_transport;
    return OLNK_ERR_HAL_TIMEOUT;
}
#endif
