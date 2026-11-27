/* cleanroom/dos/dos_hal_uart.c
 *
 * Clean-room 16-bit Real-Mode 8250 / 16450 / 16550A UART + BIOS 18.2 Hz
 * Timer HAL for OpenLNK (8086/8088 safe, zero dynamic allocation, zero FP).
 */
#include "dos_hal_uart.h"

#include <string.h>

#if defined(__WATCOMC__) && defined(__DOS__)
/* 8086-safe BIOS Data Area (0040:offset) word read without far keywords (-za99) */
static uint16_t watcom_read_bda_u16(uint16_t bda_off);
#pragma aux watcom_read_bda_u16 = \
    "push es"                     \
    "mov ax, 0040h"               \
    "mov es, ax"                  \
    "mov ax, es:[bx]"             \
    "pop es"                      \
    parm [bx]                     \
    value [ax];

/* 8086-safe zero-CRT port I/O (eliminates clibs inp_/outp_ dependency) */
static uint8_t watcom_in8(uint16_t port);
#pragma aux watcom_in8 = \
    "in al, dx"          \
    parm [dx]            \
    value [al];

static void watcom_out8(uint16_t port, uint8_t val);
#pragma aux watcom_out8 = \
    "out dx, al"          \
    parm [dx] [al];

/*
 * 8086-safe zero-CRT BIOS 18.2 Hz timer tick counter (0040:006C DWORD).
 * Uses a lock-free high/low/high consistency loop so it is safe inside
 * resident device drivers without disturbing the BIOS midnight date flag.
 */
static uint32_t watcom_read_bda_ticks(void);
#pragma aux watcom_read_bda_ticks = \
    "push es"                       \
    "mov ax, 0040h"                 \
    "mov es, ax"                    \
    "L1:"                           \
    "mov dx, es:[006eh]"            \
    "mov ax, es:[006ch]"            \
    "cmp dx, es:[006eh]"            \
    "jne L1"                        \
    "pop es"                        \
    value [dx ax]                   \
    modify [ax dx];
#endif

#define OLNK_UART_TX_SPIN_LIMIT 60000UL
#define OLNK_DOS_TICKS_PER_DAY  1573040UL

/* =========================================================================
 * Low-Level Port I/O & BIOS 18.2 Hz Tick Abstraction
 * ========================================================================= */

static uint8_t hw_in8(const olnk_dos_uart_t *uart, uint16_t port) {
    if (uart->io.in8 != NULL) {
        return uart->io.in8(port, uart->io.ctx);
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    return watcom_in8(port);
#else
    (void)port;
    return 0xFFU;
#endif
}

static void hw_out8(const olnk_dos_uart_t *uart, uint16_t port, uint8_t val) {
    if (uart->io.out8 != NULL) {
        uart->io.out8(port, val, uart->io.ctx);
        return;
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    watcom_out8(port, val);
#else
    (void)port;
    (void)val;
#endif
}

uint32_t olnk_dos_get_ticks(const olnk_dos_uart_t *uart) {
    if ((uart != NULL) && (uart->io.get_ticks != NULL)) {
        return uart->io.get_ticks(uart->io.ctx);
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    return watcom_read_bda_ticks();
#else
    return 0UL;
#endif
}

static uint32_t ticks_elapsed(uint32_t start, uint32_t now) {
    if (now >= start) {
        return now - start;
    }
    /* Handle BIOS midnight rollover cleanly */
    if (start < OLNK_DOS_TICKS_PER_DAY) {
        return (OLNK_DOS_TICKS_PER_DAY - start) + now;
    }
    return 1UL;
}

static uint32_t ms_to_bios_ticks(uint32_t timeout_ms) {
    uint32_t ticks;
    if (timeout_ms == 0UL) {
        return 0UL;
    }
    /* ~55 ms per BIOS tick (18.2 Hz); always round up + 1 tick for quantization */
    ticks = (timeout_ms + (uint32_t)OLNK_BIOS_TICK_MS - 1UL) / (uint32_t)OLNK_BIOS_TICK_MS;
    return ticks + 1UL;
}

uint16_t olnk_dos_com_port_base(uint8_t com_number) {
    static const uint16_t std_bases[4] = {
        OLNK_DOS_COM1_BASE,
        OLNK_DOS_COM2_BASE,
        OLNK_DOS_COM3_BASE,
        OLNK_DOS_COM4_BASE
    };
    if ((com_number < 1U) || (com_number > 4U)) {
        return OLNK_DOS_COM1_BASE;
    }
#if defined(__WATCOMC__) && defined(__DOS__)
    {
        uint16_t bda_port = watcom_read_bda_u16((uint16_t)((uint16_t)(com_number - 1U) * 2U));
        if (bda_port != 0U) {
            return bda_port;
        }
    }
#endif
    return std_bases[com_number - 1U];
}

/* =========================================================================
 * UART Configuration, Baud Switching & Polling Transport Callbacks
 * ========================================================================= */

static void uart_wait_tx_idle(const olnk_dos_uart_t *uart) {
    uint32_t guard;
    uint16_t lsr_port = (uint16_t)(uart->io_base + OLNK_UART_REG_LSR);
    for (guard = 0UL; guard < OLNK_UART_TX_SPIN_LIMIT; ++guard) {
        uint8_t lsr = hw_in8(uart, lsr_port);
        if ((lsr & (uint8_t)(OLNK_UART_LSR_THRE | OLNK_UART_LSR_TEMT)) ==
            (uint8_t)(OLNK_UART_LSR_THRE | OLNK_UART_LSR_TEMT)) {
            break;
        }
    }
}

void olnk_dos_uart_flush_rx(olnk_dos_uart_t *uart) {
    uint16_t guard;
    uint16_t lsr_port;
    uint16_t rbr_port;

    if (uart == NULL) {
        return;
    }
    lsr_port = (uint16_t)(uart->io_base + OLNK_UART_REG_LSR);
    rbr_port = (uint16_t)(uart->io_base + OLNK_UART_REG_RBR_THR);

    for (guard = 0U; guard < 64U; ++guard) {
        if ((hw_in8(uart, lsr_port) & OLNK_UART_LSR_DR) == 0U) {
            break;
        }
        (void)hw_in8(uart, rbr_port);
    }
}

bool olnk_dos_uart_rx_ready(const olnk_dos_uart_t *uart) {
    if (uart == NULL) {
        return false;
    }
    return (hw_in8(uart, (uint16_t)(uart->io_base + OLNK_UART_REG_LSR)) & OLNK_UART_LSR_DR) != 0U;
}

bool olnk_dos_uart_set_baud(olnk_dos_uart_t *uart, uint32_t baudrate) {
    uint8_t baud_idx = 0U;
    uint16_t divisor;
    uint8_t iir;

    if ((uart == NULL) || (!olnk_bps_to_baud_index(baudrate, &baud_idx))) {
        return false;
    }
    divisor = olnk_baud_index_to_divisor(baud_idx);
    if (divisor == 0U) {
        return false;
    }

    /* Let any in-flight byte finish clocking out before changing divisor */
    uart_wait_tx_idle(uart);

    /* Disable UART interrupts (polled mode for deterministic 8088 timing) */
    hw_out8(uart, (uint16_t)(uart->io_base + OLNK_UART_REG_IER), 0x00U);

    /* Latch divisor (LCR = 0x83 -> DLAB=1, 8N1) */
    hw_out8(uart, (uint16_t)(uart->io_base + OLNK_UART_REG_LCR),
            (uint8_t)(OLNK_UART_LCR_DLAB | OLNK_UART_LCR_8N1));
    hw_out8(uart, (uint16_t)(uart->io_base + OLNK_UART_REG_DLL), (uint8_t)(divisor & 0xFFU));
    hw_out8(uart, (uint16_t)(uart->io_base + OLNK_UART_REG_DLM), (uint8_t)((divisor >> 8U) & 0xFFU));

    /* Clear DLAB, commit 8N1 */
    hw_out8(uart, (uint16_t)(uart->io_base + OLNK_UART_REG_LCR), OLNK_UART_LCR_8N1);

    /* Enable & reset 16550A FIFO with 1-byte trigger (disable buggy non-A 16550) */
    hw_out8(uart, (uint16_t)(uart->io_base + OLNK_UART_REG_IIR_FCR), OLNK_UART_FCR_ENABLE);
    iir = hw_in8(uart, (uint16_t)(uart->io_base + OLNK_UART_REG_IIR_FCR));
    if ((iir & 0xC0U) == 0xC0U) {
        uart->has_fifo = true;
    } else {
        if ((iir & 0xC0U) == 0x80U) {
            hw_out8(uart, (uint16_t)(uart->io_base + OLNK_UART_REG_IIR_FCR), 0x00U);
        }
        uart->has_fifo = false;
    }

    /* Assert DTR + RTS */
    hw_out8(uart, (uint16_t)(uart->io_base + OLNK_UART_REG_MCR), OLNK_UART_MCR_DTR_RTS);

    olnk_dos_uart_flush_rx(uart);
    uart->current_baud = baudrate;
    return true;
}

static int dos_transport_read(uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data) {
    olnk_dos_uart_t *uart = (olnk_dos_uart_t *)user_data;
    uint16_t lsr_port;
    uint16_t rbr_port;
    size_t n = 0U;
    size_t max_len;
    uint32_t start_tick = 0UL;
    uint32_t wait_ticks;
    bool have_start = false;

    if ((uart == NULL) || (buf == NULL) || (len == 0U)) {
        return 0;
    }

    lsr_port = (uint16_t)(uart->io_base + OLNK_UART_REG_LSR);
    rbr_port = (uint16_t)(uart->io_base + OLNK_UART_REG_RBR_THR);
    max_len = (len > (size_t)OLNK_MAX_PAYLOAD_SIZE) ? (size_t)OLNK_MAX_PAYLOAD_SIZE : len;
    wait_ticks = ms_to_bios_ticks(timeout_ms);

    while (n < max_len) {
        uint8_t lsr = hw_in8(uart, lsr_port);
        if ((lsr & OLNK_UART_LSR_ERRMASK) != 0U) {
            uart->line_errors += 1UL;
        }
        if ((lsr & OLNK_UART_LSR_DR) != 0U) {
            buf[n++] = hw_in8(uart, rbr_port);
            continue;
        }
        /* Return immediately once we have read at least 1 byte */
        if (n > 0U) {
            break;
        }
        if (wait_ticks == 0UL) {
            break;
        }
        if (!have_start) {
            start_tick = olnk_dos_get_ticks(uart);
            have_start = true;
        } else {
            uint32_t now = olnk_dos_get_ticks(uart);
            if (ticks_elapsed(start_tick, now) >= wait_ticks) {
                break;
            }
        }
    }

    return (int)n;
}

static int dos_transport_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *user_data) {
    olnk_dos_uart_t *uart = (olnk_dos_uart_t *)user_data;
    uint16_t lsr_port;
    uint16_t thr_port;
    size_t sent = 0U;
    size_t max_len;
    uint32_t wait_ticks;

    if ((uart == NULL) || (buf == NULL) || (len == 0U)) {
        return 0;
    }

    lsr_port = (uint16_t)(uart->io_base + OLNK_UART_REG_LSR);
    thr_port = (uint16_t)(uart->io_base + OLNK_UART_REG_RBR_THR);
    max_len = (len > (size_t)OLNK_MAX_PAYLOAD_SIZE) ? (size_t)OLNK_MAX_PAYLOAD_SIZE : len;
    wait_ticks = ms_to_bios_ticks(timeout_ms);

    while (sent < max_len) {
        uint32_t start_tick = 0UL;
        bool have_start = false;
        bool ready = false;

        for (;;) {
            if ((hw_in8(uart, lsr_port) & OLNK_UART_LSR_THRE) != 0U) {
                ready = true;
                break;
            }
            if (wait_ticks == 0UL) {
                break;
            }
            if (!have_start) {
                start_tick = olnk_dos_get_ticks(uart);
                have_start = true;
            } else if (ticks_elapsed(start_tick, olnk_dos_get_ticks(uart)) >= wait_ticks) {
                break;
            }
        }

        if (!ready) {
            break;
        }
        hw_out8(uart, thr_port, buf[sent]);
        sent += 1U;
    }

    return (int)sent;
}

static bool dos_transport_set_baud(uint32_t baudrate, void *user_data) {
    return olnk_dos_uart_set_baud((olnk_dos_uart_t *)user_data, baudrate);
}

olnk_status_t olnk_dos_uart_init(
    olnk_dos_uart_t *uart,
    uint8_t com_number,
    uint16_t io_base_override,
    uint32_t initial_baud,
    const olnk_dos_io_ops_t *custom_io,
    olnk_transport_t *out_transport
) {
    if ((uart == NULL) || (out_transport == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    memset(uart, 0, sizeof(*uart));
    uart->com_number = ((com_number >= 1U) && (com_number <= 4U)) ? com_number : 1U;
    uart->io_base = (io_base_override != 0U)
                        ? io_base_override
                        : olnk_dos_com_port_base(uart->com_number);
    if (custom_io != NULL) {
        uart->io = *custom_io;
    }

    if (!olnk_dos_uart_set_baud(uart, initial_baud)) {
        return OLNK_ERR_INVALID_ARG;
    }

    out_transport->read = dos_transport_read;
    out_transport->write = dos_transport_write;
    out_transport->set_baud = dos_transport_set_baud;
    out_transport->user_data = uart;
    return OLNK_OK;
}
