/* cleanroom/tests/test_dos_hal.c
 *
 * Host-side C99 verification suite for the 16-bit Real-Mode MS-DOS HAL:
 *   - cleanroom/dos/dos_hal_uart.c   (8250/16450/16550A COM1..COM4 + BIOS 18.2 Hz timer)
 *   - cleanroom/dos/dos_hal_disk.c   (MS-DOS INT 25h/26h logical sector storage backend)
 *   - cleanroom/dos/openlnk_sys_stub.c (OPENLNK.SYS Block Device Driver commands 0x00..0x13)
 *
 * Simulates two real 16550A UART register blocks wired back-to-back with strict
 * divisor-latch baud matching, verifying full end-to-end OPENLNK.SYS <-> olnk_server_t
 * transfers (Phase A sync, Phase B 9600->115200 step-up, BPB, IOCTL, 16 KiB multi-fragment
 * write/read, media change, write-protect 0x8100, and transparent session resumption).
 *
 * Strictly ISO C99, zero dynamic memory allocation, zero floating-point.
 */
#include "dos_hal_disk.h"
#include "dos_hal_uart.h"
#include "openlnk/server.h"
#include "openlnk_sys.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                           \
    do {                                                                           \
        if (cond) {                                                                \
            g_pass++;                                                              \
        } else {                                                                   \
            g_fail++;                                                              \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg));      \
        }                                                                          \
    } while (0)

/* =========================================================================
 * Simulated 8250 / 16550 / 16550A PC UART Register Block
 * ========================================================================= */

#define SIM_RING_CAP 16384U

typedef enum {
    SIM_CHIP_8250 = 0,
    SIM_CHIP_16550_BUGGY = 1,
    SIM_CHIP_16550A = 2
} sim_uart_chip_t;

typedef struct sim_uart sim_uart_t;

struct sim_uart {
    uint16_t base_port;
    sim_uart_chip_t chip;

    /* Hardware register latches */
    uint8_t ier;
    uint8_t fcr_last;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t dll;
    uint8_t dlm;
    uint8_t injected_lsr_err;
    bool fifo_enabled;
    uint32_t baud_reprograms;

    /* Receive ring buffer */
    uint8_t rx_ring[SIM_RING_CAP];
    size_t rx_head;
    size_t rx_tail;

    /* BIOS 18.2 Hz tick counter */
    uint32_t ticks;
    bool auto_tick_on_idle;
    uint32_t idle_polls;

    /* Optional null-modem peer + server pump/tick callbacks */
    sim_uart_t *peer;
    void (*on_tx_pump)(void *ctx);
    void (*on_idle_tick)(void *ctx);
    void *pump_ctx;
    bool pumping;
    uint32_t baud_mismatch_drops;
};

static uint16_t sim_uart_divisor(const sim_uart_t *u) {
    return (uint16_t)(((uint16_t)u->dlm << 8U) | (uint16_t)u->dll);
}

static size_t sim_uart_rx_count(const sim_uart_t *u) {
    if (u->rx_head >= u->rx_tail) {
        return u->rx_head - u->rx_tail;
    }
    return (SIM_RING_CAP - u->rx_tail) + u->rx_head;
}

static void sim_uart_rx_push(sim_uart_t *u, uint8_t b) {
    size_t next = (u->rx_head + 1U) % SIM_RING_CAP;
    if (next != u->rx_tail) {
        u->rx_ring[u->rx_head] = b;
        u->rx_head = next;
    }
}

static uint8_t sim_uart_rx_pop(sim_uart_t *u) {
    uint8_t b = 0U;
    if (u->rx_head != u->rx_tail) {
        b = u->rx_ring[u->rx_tail];
        u->rx_tail = (u->rx_tail + 1U) % SIM_RING_CAP;
    }
    return b;
}

static uint8_t sim_in8(uint16_t port, void *ctx) {
    sim_uart_t *u = (sim_uart_t *)ctx;
    uint16_t reg;
    if ((u == NULL) || (port < u->base_port) || (port > (uint16_t)(u->base_port + 6U))) {
        return 0xFFU;
    }
    reg = (uint16_t)(port - u->base_port);

    switch (reg) {
        case OLNK_UART_REG_RBR_THR:
            if ((u->lcr & OLNK_UART_LCR_DLAB) != 0U) {
                return u->dll;
            }
            return sim_uart_rx_pop(u);

        case OLNK_UART_REG_IER:
            if ((u->lcr & OLNK_UART_LCR_DLAB) != 0U) {
                return u->dlm;
            }
            return u->ier;

        case OLNK_UART_REG_IIR_FCR:
            if ((u->chip == SIM_CHIP_16550A) && u->fifo_enabled) {
                return 0xC1U;
            }
            if ((u->chip == SIM_CHIP_16550_BUGGY) && u->fifo_enabled) {
                return 0x81U;
            }
            return 0x01U;

        case OLNK_UART_REG_LCR:
            return u->lcr;

        case OLNK_UART_REG_MCR:
            return u->mcr;

        case OLNK_UART_REG_LSR: {
            uint8_t lsr = (uint8_t)(OLNK_UART_LSR_THRE | OLNK_UART_LSR_TEMT);
            if ((sim_uart_rx_count(u) == 0U) && (u->on_tx_pump != NULL) && (!u->pumping)) {
                u->pumping = true;
                u->on_tx_pump(u->pump_ctx);
                u->pumping = false;
            }
            if (sim_uart_rx_count(u) > 0U) {
                lsr = (uint8_t)(lsr | OLNK_UART_LSR_DR);
            }
            if (u->injected_lsr_err != 0U) {
                lsr = (uint8_t)(lsr | u->injected_lsr_err);
                u->injected_lsr_err = 0U; /* Reading LSR clears error bits on 8250/16550A */
            }
            return lsr;
        }

        default:
            return 0x00U;
    }
}

static void sim_out8(uint16_t port, uint8_t val, void *ctx) {
    sim_uart_t *u = (sim_uart_t *)ctx;
    uint16_t reg;
    if ((u == NULL) || (port < u->base_port) || (port > (uint16_t)(u->base_port + 6U))) {
        return;
    }
    reg = (uint16_t)(port - u->base_port);

    switch (reg) {
        case OLNK_UART_REG_RBR_THR:
            if ((u->lcr & OLNK_UART_LCR_DLAB) != 0U) {
                u->dll = val;
            } else if (u->peer != NULL) {
                /* Enforce realistic physical RS-232 baud-rate divisor match */
                if ((sim_uart_divisor(u) != 0U) &&
                    (sim_uart_divisor(u) == sim_uart_divisor(u->peer))) {
                    sim_uart_rx_push(u->peer, val);
                } else {
                    u->baud_mismatch_drops += 1UL;
                }
                if ((u->on_tx_pump != NULL) && (!u->pumping)) {
                    u->pumping = true;
                    u->on_tx_pump(u->pump_ctx);
                    u->pumping = false;
                }
            }
            break;

        case OLNK_UART_REG_IER:
            if ((u->lcr & OLNK_UART_LCR_DLAB) != 0U) {
                u->dlm = val;
            } else {
                u->ier = val;
            }
            break;

        case OLNK_UART_REG_IIR_FCR:
            u->fcr_last = val;
            if (u->chip != SIM_CHIP_8250) {
                u->fifo_enabled = ((val & 0x01U) != 0U);
                if ((val & 0x02U) != 0U) {
                    u->rx_head = 0U;
                    u->rx_tail = 0U;
                }
            }
            break;

        case OLNK_UART_REG_LCR:
            if (((u->lcr & OLNK_UART_LCR_DLAB) != 0U) && ((val & OLNK_UART_LCR_DLAB) == 0U)) {
                u->baud_reprograms += 1UL;
            }
            u->lcr = val;
            break;

        case OLNK_UART_REG_MCR:
            u->mcr = val;
            break;

        default:
            break;
    }
}

static uint32_t sim_get_ticks(void *ctx) {
    sim_uart_t *u = (sim_uart_t *)ctx;
    uint32_t cur;
    if (u == NULL) {
        return 0UL;
    }
    cur = u->ticks;
    if (u->auto_tick_on_idle) {
        u->idle_polls += 1UL;
        u->ticks = (u->ticks + 1UL) % 1573040UL;
        if ((u->on_idle_tick != NULL) && (!u->pumping)) {
            u->pumping = true;
            u->on_idle_tick(u->pump_ctx);
            u->pumping = false;
        }
    }
    return cur;
}

static olnk_dos_io_ops_t make_sim_io(sim_uart_t *u) {
    olnk_dos_io_ops_t ops;
    ops.in8 = sim_in8;
    ops.out8 = sim_out8;
    ops.get_ticks = sim_get_ticks;
    ops.ctx = u;
    return ops;
}

/* =========================================================================
 * Simulated FAT16 Disk Behind MS-DOS INT 25h / INT 26h Hooks
 * ========================================================================= */

#define SIM_DISK_SECTORS 128U

typedef struct {
    uint8_t sectors[SIM_DISK_SECTORS][OLNK_SECTOR_SIZE];
    bool present;
    bool ready;
    bool hw_write_protect;
    uint32_t int25_calls;
    uint32_t int26_calls;
} sim_dos_disk_t;

static void build_fat16_vbr(
    uint8_t sec[OLNK_SECTOR_SIZE],
    uint16_t total_sectors,
    uint32_t serial,
    const char label11[11]
) {
    memset(sec, 0, OLNK_SECTOR_SIZE);
    sec[0] = 0xEBU;
    sec[1] = 0x3CU;
    sec[2] = 0x90U;
    memcpy(&sec[3], "OPENLNK ", 8U);
    olnk_write_u16_le(&sec[11], OLNK_SECTOR_SIZE);
    sec[13] = 2U;  /* sectors per cluster */
    olnk_write_u16_le(&sec[14], 1U);  /* reserved */
    sec[16] = 2U;  /* FATs */
    olnk_write_u16_le(&sec[17], 64U); /* root entries */
    olnk_write_u16_le(&sec[19], total_sectors);
    sec[21] = 0xF8U; /* fixed disk */
    olnk_write_u16_le(&sec[22], 2U); /* sectors per FAT */
    olnk_write_u16_le(&sec[24], 32U);
    olnk_write_u16_le(&sec[26], 2U);
    sec[38] = 0x29U;
    olnk_write_u32_le(&sec[39], serial);
    memcpy(&sec[43], label11, 11U);
    memcpy(&sec[54], "FAT16   ", 8U);
    sec[510] = 0x55U;
    sec[511] = 0xAAU;
}

static uint16_t mock_int25_read(
    uint8_t drive,
    uint32_t start_lba,
    uint16_t count,
    uint8_t *buf,
    void *ctx
) {
    sim_dos_disk_t *d = (sim_dos_disk_t *)ctx;
    (void)drive;
    if ((d == NULL) || (!d->present)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (!d->ready) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if ((start_lba >= (uint32_t)SIM_DISK_SECTORS) ||
        ((start_lba + (uint32_t)count) > (uint32_t)SIM_DISK_SECTORS)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }
    d->int25_calls += 1UL;
    memcpy(buf, d->sectors[start_lba], (size_t)count * OLNK_SECTOR_SIZE);
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t mock_int26_write(
    uint8_t drive,
    uint32_t start_lba,
    uint16_t count,
    const uint8_t *buf,
    void *ctx
) {
    sim_dos_disk_t *d = (sim_dos_disk_t *)ctx;
    (void)drive;
    if ((d == NULL) || (!d->present)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (!d->ready) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if (d->hw_write_protect) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
    }
    if ((start_lba >= (uint32_t)SIM_DISK_SECTORS) ||
        ((start_lba + (uint32_t)count) > (uint32_t)SIM_DISK_SECTORS)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }
    d->int26_calls += 1UL;
    memcpy(d->sectors[start_lba], buf, (size_t)count * OLNK_SECTOR_SIZE);
    return OLNK_DOS_STATUS_DONE;
}

/* =========================================================================
 * Test 1: 8250 / 16550 / 16550A Register Programming & BIOS 18.2 Hz Timer
 * ========================================================================= */

static sim_uart_t g_u_single;

static void test_uart_register_programming_and_fifo_detect(void) {
    olnk_dos_uart_t uart;
    olnk_transport_t tr;
    olnk_dos_io_ops_t io;
    uint8_t rx_buf[8];
    int n;

    CHECK(olnk_dos_com_port_base(1U) == OLNK_DOS_COM1_BASE, "COM1 base 0x3F8");
    CHECK(olnk_dos_com_port_base(2U) == OLNK_DOS_COM2_BASE, "COM2 base 0x2F8");
    CHECK(olnk_dos_com_port_base(3U) == OLNK_DOS_COM3_BASE, "COM3 base 0x3E8");
    CHECK(olnk_dos_com_port_base(4U) == OLNK_DOS_COM4_BASE, "COM4 base 0x2E8");
    CHECK(olnk_dos_com_port_base(9U) == OLNK_DOS_COM1_BASE, "invalid COM falls back to 0x3F8");

    /* 16550A detection + divisor programming */
    memset(&g_u_single, 0, sizeof(g_u_single));
    g_u_single.base_port = OLNK_DOS_COM1_BASE;
    g_u_single.chip = SIM_CHIP_16550A;
    io = make_sim_io(&g_u_single);

    CHECK(olnk_dos_uart_init(&uart, 1U, 0U, 9600UL, &io, &tr) == OLNK_OK, "16550A init 9600");
    CHECK(uart.has_fifo, "16550A FIFO detected");
    CHECK(sim_uart_divisor(&g_u_single) == 12U, "9600 bps -> divisor 12");
    CHECK(g_u_single.lcr == OLNK_UART_LCR_8N1, "LCR committed 8N1 (0x03)");
    CHECK(g_u_single.ier == 0x00U, "IER disabled (polled mode)");
    CHECK(g_u_single.mcr == OLNK_UART_MCR_DTR_RTS, "MCR asserts DTR+RTS");
    CHECK(g_u_single.fcr_last == OLNK_UART_FCR_ENABLE, "FCR enables 16550A FIFO (0x07)");

    CHECK(tr.set_baud(115200UL, tr.user_data), "switch to 115200");
    CHECK(sim_uart_divisor(&g_u_single) == 1U, "115200 bps -> divisor 1");
    CHECK(tr.set_baud(1200UL, tr.user_data), "switch to 1200");
    CHECK(sim_uart_divisor(&g_u_single) == 96U, "1200 bps -> divisor 96");
    CHECK(!tr.set_baud(99999UL, tr.user_data), "unsupported baud rejected");
    CHECK(uart.current_baud == 1200UL, "current_baud preserved on reject");

    /* 8250 / 16450 (no FIFO) */
    memset(&g_u_single, 0, sizeof(g_u_single));
    g_u_single.base_port = OLNK_DOS_COM2_BASE;
    g_u_single.chip = SIM_CHIP_8250;
    CHECK(olnk_dos_uart_init(&uart, 2U, 0U, 9600UL, &io, &tr) == OLNK_OK, "8250 init");
    CHECK(!uart.has_fifo, "8250 reports has_fifo=false");

    /* Buggy non-A 16550 -> must detect & explicitly disable FIFO (FCR=0x00) */
    memset(&g_u_single, 0, sizeof(g_u_single));
    g_u_single.base_port = OLNK_DOS_COM1_BASE;
    g_u_single.chip = SIM_CHIP_16550_BUGGY;
    CHECK(olnk_dos_uart_init(&uart, 1U, 0U, 9600UL, &io, &tr) == OLNK_OK, "buggy 16550 init");
    CHECK(!uart.has_fifo, "buggy 16550 reports has_fifo=false");
    CHECK(g_u_single.fcr_last == 0x00U, "buggy 16550 FIFO explicitly disabled");

    /* LSR error counting, RX ready, flush, and BIOS midnight rollover timeout */
    sim_uart_rx_push(&g_u_single, 0xA5U);
    sim_uart_rx_push(&g_u_single, 0x5AU);
    CHECK(olnk_dos_uart_rx_ready(&uart), "rx_ready true when bytes queued");
    g_u_single.injected_lsr_err = OLNK_UART_LSR_FE;
    n = tr.read(rx_buf, sizeof(rx_buf), 100U, tr.user_data);
    CHECK((n == 2) && (rx_buf[0] == 0xA5U) && (rx_buf[1] == 0x5AU), "read returns queued bytes");
    CHECK(uart.line_errors == 1UL, "LSR framing error counted");
    CHECK(!olnk_dos_uart_rx_ready(&uart), "rx_ready false after drain");

    sim_uart_rx_push(&g_u_single, 0xFFU);
    olnk_dos_uart_flush_rx(&uart);
    CHECK(!olnk_dos_uart_rx_ready(&uart), "flush_rx empties RBR/FIFO");

    /* Midnight BIOS tick rollover (1573038 -> 1573039 -> 0 -> 1 -> 2) */
    g_u_single.ticks = 1573038UL;
    g_u_single.auto_tick_on_idle = true;
    g_u_single.idle_polls = 0UL;
    n = tr.read(rx_buf, 1U, 110U, tr.user_data); /* ~2 ticks + 1 quantization = 3 ticks */
    CHECK(n == 0, "empty RX times out cleanly across BIOS midnight rollover");
    CHECK((g_u_single.idle_polls >= 3UL) && (g_u_single.idle_polls <= 6UL),
          "BIOS midnight rollover elapsed ticks computed accurately");
}

/* =========================================================================
 * Test 2: MS-DOS INT 25h / INT 26h Storage Backend
 * ========================================================================= */

static sim_dos_disk_t g_mock_disk;
static olnk_dos_disk_set_t g_dos_disk_set;

static void test_dos_disk_hal_int25_26(void) {
    olnk_dos_disk_hooks_t hooks;
    olnk_storage_ops_t ops;
    olnk_drive_info_t info;
    olnk_dos_bpb_t bpb;
    uint8_t media_status = 0U;
    uint8_t sec_w[OLNK_SECTOR_SIZE * 2U];
    uint8_t sec_r[OLNK_SECTOR_SIZE * 2U];
    uint16_t xfer = 0U;
    size_t i;

    memset(&g_mock_disk, 0, sizeof(g_mock_disk));
    g_mock_disk.present = true;
    g_mock_disk.ready = true;
    build_fat16_vbr(g_mock_disk.sectors[0], SIM_DISK_SECTORS, 0x12345678UL, "DOS16VOL   ");

    hooks.abs_read = mock_int25_read;
    hooks.abs_write = mock_int26_write;
    hooks.ctx = &g_mock_disk;

    olnk_dos_disk_init(&g_dos_disk_set, false, &hooks);
    CHECK(olnk_dos_disk_add_unit(&g_dos_disk_set, 2U), "add C: unit=2");
    ops = olnk_dos_disk_ops(&g_dos_disk_set);

    CHECK(ops.drive_count(ops.user_data) == 3U, "drive_count spans through C:");
    CHECK(ops.get_drive_info(2U, &info, ops.user_data) == OLNK_DOS_STATUS_DONE, "get_drive_info C:");
    CHECK(info.ready && (info.volume_serial == 0x12345678UL), "VBR serial parsed via INT 25h");
    CHECK(ops.get_drive_info(0U, &info, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT),
          "unenabled unit A: returns 0x8101");

    CHECK(ops.build_bpb(2U, 0xF8U, &bpb, ops.user_data) == OLNK_DOS_STATUS_DONE, "build_bpb C:");
    CHECK((bpb.bytes_per_sector == 512U) && (bpb.total_sectors_16 == SIM_DISK_SECTORS),
          "FAT16 BPB geometry matches VBR");

    /* Media check unchanged -> swap serial -> changed -> unchanged */
    CHECK(ops.media_check(2U, 0xF8U, &media_status, &info, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "media_check unchanged");
    CHECK(media_status == (uint8_t)OLNK_MEDIA_UNCHANGED, "media_status == 0x01");

    build_fat16_vbr(g_mock_disk.sectors[0], SIM_DISK_SECTORS, 0xCAFEBABEUL, "NEWDISK    ");
    CHECK(ops.media_check(2U, 0xF8U, &media_status, &info, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "media_check after swap");
    CHECK(media_status == (uint8_t)OLNK_MEDIA_CHANGED, "media_status == 0xFF after serial swap");
    CHECK(info.volume_serial == 0xCAFEBABEUL, "updated volume serial reported");

    /* Multi-sector INT 26h write + INT 25h readback */
    for (i = 0U; i < sizeof(sec_w); ++i) {
        sec_w[i] = (uint8_t)((i * 19U + 7U) & 0xFFU);
    }
    CHECK(ops.write_sectors(2U, 10UL, 2U, sec_w, &xfer, true, ops.user_data) ==
              OLNK_DOS_STATUS_DONE,
          "INT 26h multi-sector write");
    CHECK(xfer == 2U, "2 sectors written");
    memset(sec_r, 0, sizeof(sec_r));
    CHECK(ops.read_sectors(2U, 10UL, 2U, sec_r, &xfer, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "INT 25h multi-sector read");
    CHECK((xfer == 2U) && (memcmp(sec_w, sec_r, sizeof(sec_w)) == 0), "INT 25h/26h round-trip exact");

    /* Zero-sector no-op */
    CHECK(ops.read_sectors(2U, 10UL, 0U, sec_r, &xfer, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "zero-sector read returns DONE");

    /* Read-only write protect (0x8100) */
    g_dos_disk_set.read_only = true;
    CHECK(ops.write_sectors(2U, 10UL, 1U, sec_w, &xfer, false, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT),
          "read_only returns 0x8100 WRITE_PROTECT");
    g_dos_disk_set.read_only = false;

    /* Not-ready propagation (0x8102) */
    g_mock_disk.ready = false;
    CHECK(ops.read_sectors(2U, 0UL, 1U, sec_r, &xfer, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY),
          "not-ready propagates 0x8102");
}

/* =========================================================================
 * Test 3: End-to-End OPENLNK.SYS <-> olnk_server_t over Dual 16550A UARTs
 * ========================================================================= */

static sim_uart_t g_cli_uart_hw;
static sim_uart_t g_srv_uart_hw;
static olnk_dos_uart_t g_srv_uart;
static olnk_server_t g_srv;
static sim_dos_disk_t g_e2e_disk;
static olnk_dos_disk_set_t g_e2e_disk_set;
static uint8_t g_big_tx[32U * OLNK_SECTOR_SIZE]; /* 16 KiB (>8192 B multi-fragment!) */
static uint8_t g_big_rx[32U * OLNK_SECTOR_SIZE];

static void pump_dos_server(void *ctx) {
    olnk_server_t *srv = (olnk_server_t *)ctx;
    uint32_t guard;
    for (guard = 0UL; guard < 128UL; ++guard) {
        if (sim_uart_rx_count(&g_srv_uart_hw) == 0U) {
            break;
        }
        (void)olnk_server_poll(srv, 0U);
    }
}

static void tick_dos_server(void *ctx) {
    olnk_server_t *srv = (olnk_server_t *)ctx;
    g_srv_uart_hw.ticks = (g_srv_uart_hw.ticks + 1UL) % 1573040UL;
    olnk_server_tick(srv, OLNK_BIOS_TICK_MS);
}

static void test_openlnk_sys_over_simulated_16550a_uart(void) {
    olnk_dos_io_ops_t cli_io;
    olnk_dos_io_ops_t srv_io;
    olnk_dos_disk_hooks_t disk_hooks;
    olnk_transport_t srv_tr;
    olnk_dos_sys_req_t req;
    size_t i;

    /* Wire two simulated 16550A UARTs back-to-back (COM1 <-> COM2) */
    memset(&g_cli_uart_hw, 0, sizeof(g_cli_uart_hw));
    memset(&g_srv_uart_hw, 0, sizeof(g_srv_uart_hw));
    g_cli_uart_hw.base_port = OLNK_DOS_COM1_BASE;
    g_cli_uart_hw.chip = SIM_CHIP_16550A;
    g_cli_uart_hw.peer = &g_srv_uart_hw;
    g_cli_uart_hw.on_tx_pump = pump_dos_server;
    g_cli_uart_hw.on_idle_tick = tick_dos_server;
    g_cli_uart_hw.pump_ctx = &g_srv;
    g_cli_uart_hw.auto_tick_on_idle = true;

    g_srv_uart_hw.base_port = OLNK_DOS_COM2_BASE;
    g_srv_uart_hw.chip = SIM_CHIP_16550A;
    g_srv_uart_hw.peer = &g_cli_uart_hw;
    g_srv_uart_hw.auto_tick_on_idle = false;

    /* Prepare mock FAT16 drive C: (unit 2) behind INT 25h/26h HAL */
    memset(&g_e2e_disk, 0, sizeof(g_e2e_disk));
    g_e2e_disk.present = true;
    g_e2e_disk.ready = true;
    build_fat16_vbr(g_e2e_disk.sectors[0], SIM_DISK_SECTORS, 0x12345678UL, "OPENLNKDOS ");

    disk_hooks.abs_read = mock_int25_read;
    disk_hooks.abs_write = mock_int26_write;
    disk_hooks.ctx = &g_e2e_disk;
    olnk_dos_disk_init(&g_e2e_disk_set, false, &disk_hooks);
    CHECK(olnk_dos_disk_add_unit(&g_e2e_disk_set, 2U), "e2e server adds C:");

    /* Init server on simulated COM2 @ 9600 bps (max 115200 bps) */
    srv_io = make_sim_io(&g_srv_uart_hw);
    CHECK(olnk_dos_uart_init(&g_srv_uart, 2U, 0U, 9600UL, &srv_io, &srv_tr) == OLNK_OK,
          "e2e server COM2 init");
    CHECK(olnk_server_init(&g_srv, srv_tr, olnk_dos_disk_ops(&g_e2e_disk_set)) == OLNK_OK,
          "e2e server engine init");
    g_srv.link.max_baud_idx = (uint8_t)OLNK_BAUD_115200;

    /* Init freestanding OPENLNK.SYS client on simulated COM1 @ max 115200 bps */
    cli_io = make_sim_io(&g_cli_uart_hw);
    CHECK(olnk_dos_sys_init(1U, 115200UL, 4U, &cli_io) == OLNK_OK, "OPENLNK.SYS init COM1");
    CHECK(olnk_dos_sys_uart()->has_fifo, "OPENLNK.SYS detected 16550A FIFO");

    /* Cmd 0x00 INIT -> Sync + Baud step-up (9600->115200) + SESSION_SYNC + MAP_CONFIG */
    memset(&req, 0, sizeof(req));
    req.cmd = OLNK_DOS_CMD_INIT;
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_STATUS_DONE, "OPENLNK.SYS Cmd 0x00 INIT");
    CHECK(olnk_dos_sys_unit_count() >= 1U, "OPENLNK.SYS mapped >= 1 remote unit");
    CHECK(sim_uart_divisor(&g_cli_uart_hw) == 1U, "client 16550A latched divisor=1 (115200 bps)");
    CHECK(sim_uart_divisor(&g_srv_uart_hw) == 1U, "server 16550A latched divisor=1 (115200 bps)");
    CHECK(g_cli_uart_hw.baud_mismatch_drops == 0UL, "zero RS-232 baud mismatch drops (client)");
    CHECK(g_srv_uart_hw.baud_mismatch_drops == 4UL,
          "all 4 asynchronous early baud-switch beacons recovered via 55 ms server retry");
    CHECK((g_srv.link.baud_switches == 4UL) && (olnk_dos_sys_client()->link.baud_switches == 4UL),
          "both endpoints completed all 4 stepwise baud switches (9600->115200)");

    /* Cmd 0x02 BUILD_BPB */
    memset(&req, 0, sizeof(req));
    req.cmd = OLNK_DOS_CMD_BUILD_BPB;
    req.unit = 0U;
    req.media_descriptor = 0xF8U;
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_STATUS_DONE, "OPENLNK.SYS Cmd 0x02 BUILD_BPB");
    CHECK(req.has_bpb && (req.bpb.bytes_per_sector == 512U) &&
              (req.bpb.total_sectors_16 == SIM_DISK_SECTORS),
          "OPENLNK.SYS received valid FAT16 BPB over simulated 16550A");

    /* Cmd 0x0D OPEN & 0x0E CLOSE */
    memset(&req, 0, sizeof(req));
    req.cmd = OLNK_DOS_CMD_OPEN;
    req.unit = 0U;
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_STATUS_DONE, "OPENLNK.SYS Cmd 0x0D OPEN");
    req.cmd = OLNK_DOS_CMD_CLOSE;
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_STATUS_DONE, "OPENLNK.SYS Cmd 0x0E CLOSE");

    /* Cmd 0x13 GENERIC_IOCTL 08h/66h (Get Media ID) & unsupported 08h/99h */
    memset(&req, 0, sizeof(req));
    req.cmd = OLNK_DOS_CMD_GENERIC_IOCTL;
    req.unit = 0U;
    req.ioctl_category = OLNK_IOCTL_CAT_DISK;
    req.ioctl_function = OLNK_IOCTL_FN_GET_MEDIA_ID;
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_STATUS_DONE, "OPENLNK.SYS Cmd 0x13 IOCTL 08/66");
    CHECK((req.mid.serial_number == 0x12345678UL) &&
              (memcmp(req.mid.volume_label, "OPENLNKDOS ", 11U) == 0),
          "IOCTL 08h/66h returned exact volume serial & label");

    req.ioctl_function = 0x99U;
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND),
          "unsupported IOCTL returns 0x8103");

    /* Cmd 0x09 WRITE_VERIFY + Cmd 0x04 READ: 16 KiB (32 sectors = multi-fragment!) */
    for (i = 0U; i < sizeof(g_big_tx); ++i) {
        g_big_tx[i] = (uint8_t)((i * 37U + 13U) ^ 0xA5U);
    }
    memset(&req, 0, sizeof(req));
    req.cmd = OLNK_DOS_CMD_WRITE_VERIFY;
    req.unit = 0U;
    req.media_descriptor = 0xF8U;
    req.start_lba = 16UL;
    req.sector_count = 32U;
    req.xfer_buf = g_big_tx;
    req.xfer_cap = (uint32_t)sizeof(g_big_tx);
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_STATUS_DONE,
          "OPENLNK.SYS Cmd 0x09 16 KiB multi-fragment WRITE_VERIFY");
    CHECK(req.sector_count == 32U, "all 32 sectors written through INT 26h");

    memset(g_big_rx, 0, sizeof(g_big_rx));
    memset(&req, 0, sizeof(req));
    req.cmd = OLNK_DOS_CMD_READ;
    req.unit = 0U;
    req.media_descriptor = 0xF8U;
    req.start_lba = 16UL;
    req.sector_count = 32U;
    req.xfer_buf = g_big_rx;
    req.xfer_cap = (uint32_t)sizeof(g_big_rx);
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_STATUS_DONE,
          "OPENLNK.SYS Cmd 0x04 16 KiB multi-fragment READ");
    CHECK((req.sector_count == 32U) && (memcmp(g_big_tx, g_big_rx, sizeof(g_big_tx)) == 0),
          "16 KiB multi-fragment 16550A WRITE->READ bit-for-bit identical");

    /* Cmd 0x01 MEDIA_CHECK -> unchanged, swap serial -> changed (0xFF) */
    memset(&req, 0, sizeof(req));
    req.cmd = OLNK_DOS_CMD_MEDIA_CHECK;
    req.unit = 0U;
    req.media_descriptor = 0xF8U;
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_STATUS_DONE, "MEDIA_CHECK unchanged");
    CHECK(req.media_status == (uint8_t)OLNK_MEDIA_UNCHANGED, "media unchanged == 0x01");

    build_fat16_vbr(g_e2e_disk.sectors[0], SIM_DISK_SECTORS, 0xDEADBEEFUL, "SWAPPED    ");
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_STATUS_DONE, "MEDIA_CHECK after swap");
    CHECK(req.media_status == (uint8_t)OLNK_MEDIA_CHANGED, "media changed == 0xFF");

    /* Write-protect (0x8100) & unknown unit (0x8101) */
    g_e2e_disk_set.read_only = true;
    memset(&req, 0, sizeof(req));
    req.cmd = OLNK_DOS_CMD_WRITE;
    req.unit = 0U;
    req.media_descriptor = 0xF8U;
    req.start_lba = 16UL;
    req.sector_count = 1U;
    req.xfer_buf = g_big_tx;
    req.xfer_cap = OLNK_SECTOR_SIZE;
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT),
          "OPENLNK.SYS propagates 0x8100 WRITE_PROTECT");
    g_e2e_disk_set.read_only = false;

    req.cmd = OLNK_DOS_CMD_READ;
    req.unit = 7U;
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT),
          "OPENLNK.SYS unmapped unit returns 0x8101");

    /* Link drop + transparent reconnect & session resumption! */
    olnk_server_drop_link(&g_srv); /* Server drops back to 9600 bps (divisor=12) */
    memset(&req, 0, sizeof(req));
    req.cmd = OLNK_DOS_CMD_READ;
    req.unit = 0U;
    req.media_descriptor = 0xF8U;
    req.start_lba = 16UL;
    req.sector_count = 1U;
    req.xfer_buf = g_big_rx;
    req.xfer_cap = OLNK_SECTOR_SIZE;
    /* First read sees baud mismatch / timeout -> marks link down (0x8102) */
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY),
          "link drop cleanly reports 0x8102 DRIVE_NOT_READY");

    /* Next read lazily reconnects @ 9600 -> steps up to 115200 -> resumes session! */
    memset(g_big_rx, 0, OLNK_SECTOR_SIZE);
    req.sector_count = 1U;
    CHECK(olnk_dos_sys_dispatch(&req) == OLNK_DOS_STATUS_DONE,
          "OPENLNK.SYS transparently reconnects & completes read");
    CHECK(olnk_dos_sys_client()->link.session_resumed, "session_resumed == true after reconnect");
    CHECK((req.sector_count == 1U) && (memcmp(g_big_tx, g_big_rx, OLNK_SECTOR_SIZE) == 0),
          "data read after session resumption matches");
    CHECK(sim_uart_divisor(&g_cli_uart_hw) == 1U && sim_uart_divisor(&g_srv_uart_hw) == 1U,
          "both 16550A UARTs re-negotiated back to 115200 bps (divisor=1)");
}

int main(void) {
    test_uart_register_programming_and_fifo_detect();
    test_dos_disk_hal_int25_26();
    test_openlnk_sys_over_simulated_16550a_uart();

    printf("[test_dos_hal] PASSED=%d FAILED=%d\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
