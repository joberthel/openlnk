/* cleanroom/dos/dos_hal_uart.h
 *
 * Clean-room 16-bit Real-Mode PC 8250 / 16450 / 16550A UART + BIOS 18.2 Hz
 * Timer HAL for OpenLNK (OPENSVR.EXE, OPENLNK.EXE, OPENLNK.SYS).
 *
 * Register layout follows IBM PC Technical Reference & docs/protocol.md §2.1:
 *   base + 0 : THR (W) / RBR (R) / DLL (DLAB=1)
 *   base + 1 : IER (R/W) / DLM (DLAB=1)
 *   base + 2 : IIR (R) / FCR (W)
 *   base + 3 : LCR (0x83 DLAB latch, 0x03 8N1)
 *   base + 4 : MCR (0x03 DTR|RTS)
 *   base + 5 : LSR (DR=0x01, OE=0x02, PE=0x04, FE=0x08, THRE=0x20, TEMT=0x40)
 *
 * Strictly ISO C99, zero dynamic memory allocation, zero floating-point,
 * 8086/8088 safe (Open Watcom v2 -ms -0 -za99 -wx -we).
 */
#ifndef OPENLNK_DOS_HAL_UART_H
#define OPENLNK_DOS_HAL_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openlnk/openlnk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard IBM PC COM1..COM4 I/O base addresses */
#define OLNK_DOS_COM1_BASE 0x03F8U
#define OLNK_DOS_COM2_BASE 0x02F8U
#define OLNK_DOS_COM3_BASE 0x03E8U
#define OLNK_DOS_COM4_BASE 0x02E8U

/* 8250 / 16550A Register Offsets */
#define OLNK_UART_REG_RBR_THR 0U
#define OLNK_UART_REG_DLL     0U
#define OLNK_UART_REG_IER     1U
#define OLNK_UART_REG_DLM     1U
#define OLNK_UART_REG_IIR_FCR 2U
#define OLNK_UART_REG_LCR     3U
#define OLNK_UART_REG_MCR     4U
#define OLNK_UART_REG_LSR     5U
#define OLNK_UART_REG_MSR     6U

/* Line Control / FIFO / Modem / Line Status Bit Masks */
#define OLNK_UART_LCR_8N1     0x03U
#define OLNK_UART_LCR_DLAB    0x80U
#define OLNK_UART_FCR_ENABLE  0x07U /* Enable 16550A FIFO + clear RX/TX + 1B trigger */
#define OLNK_UART_MCR_DTR_RTS 0x03U /* Assert DTR + RTS (polled mode, OUT2=0) */
#define OLNK_UART_LSR_DR      0x01U /* Data Ready */
#define OLNK_UART_LSR_OE      0x02U /* Overrun Error */
#define OLNK_UART_LSR_PE      0x04U /* Parity Error */
#define OLNK_UART_LSR_FE      0x08U /* Framing Error */
#define OLNK_UART_LSR_ERRMASK 0x0EU /* OE | PE | FE */
#define OLNK_UART_LSR_THRE    0x20U /* Transmit Holding Register Empty */
#define OLNK_UART_LSR_TEMT    0x40U /* Transmitter Empty */

/* Injectable low-level port I/O & 18.2 Hz timer ops (for host unit testing) */
typedef struct {
    uint8_t (*in8)(uint16_t port, void *ctx);
    void (*out8)(uint16_t port, uint8_t val, void *ctx);
    uint32_t (*get_ticks)(void *ctx);
    void *ctx;
} olnk_dos_io_ops_t;

typedef struct {
    uint16_t io_base;
    uint32_t current_baud;
    uint8_t com_number;      /* 1..4 */
    bool has_fifo;           /* True if 16550A FIFO detected */
    uint32_t line_errors;    /* Count of LSR OE/PE/FE observations */
    olnk_dos_io_ops_t io;    /* Uses hardware in/out/INT 1Ah when callbacks NULL */
} olnk_dos_uart_t;

/**
 * Return standard I/O base port for COM1..COM4 (reads BIOS Data Area on DOS
 * if non-zero, else standard PC fallback 0x3F8/0x2F8/0x3E8/0x2E8).
 */
uint16_t olnk_dos_com_port_base(uint8_t com_number);

/**
 * Query BIOS 18.2 Hz timer tick count (INT 1Ah AH=00h on DOS, or mock callback).
 */
uint32_t olnk_dos_get_ticks(const olnk_dos_uart_t *uart);

/**
 * Initialize a 16-bit PC UART at initial_baud (typically 9600 bps) 8N1 and
 * populate out_transport for libopenlnk (olnk_server_t / olnk_client_t).
 * Pass custom_io = NULL on real MS-DOS hardware.
 */
olnk_status_t olnk_dos_uart_init(
    olnk_dos_uart_t *uart,
    uint8_t com_number,
    uint16_t io_base_override,
    uint32_t initial_baud,
    const olnk_dos_io_ops_t *custom_io,
    olnk_transport_t *out_transport
);

/**
 * Reprogram 8250/16550A UART divisor latch to target baudrate (1200..115200 bps).
 */
bool olnk_dos_uart_set_baud(olnk_dos_uart_t *uart, uint32_t baudrate);

/**
 * Non-blocking check whether at least one byte is waiting in RBR (LSR DR=1).
 */
bool olnk_dos_uart_rx_ready(const olnk_dos_uart_t *uart);

/**
 * Flush any stale bytes waiting in the UART receive register/FIFO.
 */
void olnk_dos_uart_flush_rx(olnk_dos_uart_t *uart);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_DOS_HAL_UART_H */
