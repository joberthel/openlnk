/* cleanroom/tests/test_dos_hal.c
 *
 * Host-side C99 verification suite for the 16-bit Real-Mode MS-DOS HAL:
 *   - cleanroom/dos/dos_hal_uart.c   (8250/16450/16550A COM1..COM4 + BIOS 18.2 Hz timer)
 *   - cleanroom/dos/dos_hal_disk.c   (MS-DOS INT 25h/26h logical sector storage backend)
 *   - cleanroom/dos/dos_hal_io.c     (Zero-stdio INT 21h console/file I/O & cmdline parser)
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
#include "dos_hal_io.h"
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

    /* Non-bootable RAMDRIVE.SYS (DOS 3.x 19-byte BPB, no 0x55 0xAA boot signature!) */
    g_mock_disk.ready = true;
    memset(g_mock_disk.sectors[0], 0, OLNK_SECTOR_SIZE);
    g_mock_disk.sectors[0][0] = 0xEBU;
    g_mock_disk.sectors[0][1] = 0x1DU;
    g_mock_disk.sectors[0][2] = 0x90U;
    memcpy(&g_mock_disk.sectors[0][3], "RDV 1.20", 8U);
    olnk_write_u16_le(&g_mock_disk.sectors[0][11], 512U);
    g_mock_disk.sectors[0][13] = 1U;
    olnk_write_u16_le(&g_mock_disk.sectors[0][14], 1U);
    g_mock_disk.sectors[0][16] = 1U;
    olnk_write_u16_le(&g_mock_disk.sectors[0][17], 256U);
    olnk_write_u16_le(&g_mock_disk.sectors[0][19], SIM_DISK_SECTORS);
    g_mock_disk.sectors[0][21] = 0xF8U;
    olnk_write_u16_le(&g_mock_disk.sectors[0][22], 3U);
    olnk_write_u16_le(&g_mock_disk.sectors[0][24], 1U);
    olnk_write_u16_le(&g_mock_disk.sectors[0][26], 1U);
    /* Simulated DOS 3.x boot code at offset 0x1E..0x23 (must NOT pollute total_sectors_32!) */
    g_mock_disk.sectors[0][30] = 0x08U;
    g_mock_disk.sectors[0][31] = 0xEBU;
    g_mock_disk.sectors[0][32] = 0xFEU;
    g_mock_disk.sectors[0][33] = 0x41U;
    /* Intentionally leave bytes 510..511 == 0x00 0x00 */
    CHECK(ops.build_bpb(2U, 0xF8U, &bpb, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "RAMDRIVE.SYS 19B BPB without 55 AA succeeds in dos_hal_disk");
    CHECK((bpb.bytes_per_sector == 512U) && (bpb.num_fats == 1U) &&
              (bpb.total_sectors_16 == SIM_DISK_SECTORS) && (bpb.total_sectors_32 == 0UL) &&
              (bpb.hidden_sectors == 0UL),
          "RAMDRIVE.SYS BPB geometry exact & boot code does not corrupt 32-bit fields");
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

/* =========================================================================
 * Test 4: CONFIG.SYS Argument Parser & Binary MS-DOS Request Header Marshalling
 * ========================================================================= */

#define SIM_DOS_RAM_SIZE 131072UL /* 128 KiB flat real-mode address space */

typedef struct {
    uint8_t ram[SIM_DOS_RAM_SIZE];
    uint8_t header_units;
    char init_banner[128];
    bool wrap_violation;
    uint32_t far_reads;
    uint32_t far_writes;
} sim_dos_mem_t;

static sim_dos_mem_t g_sim_mem;

static void sim_mem_far_read(
    uint16_t seg,
    uint16_t off,
    void *dst,
    uint16_t len,
    void *ctx
) {
    sim_dos_mem_t *m = (sim_dos_mem_t *)ctx;
    uint32_t lin = (((uint32_t)seg) << 4U) + (uint32_t)off;
    if (((uint32_t)off + (uint32_t)len) > 65536UL) {
        m->wrap_violation = true;
    }
    if ((lin + (uint32_t)len) <= SIM_DOS_RAM_SIZE) {
        memcpy(dst, &m->ram[lin], (size_t)len);
    } else {
        memset(dst, 0, (size_t)len);
    }
    m->far_reads += 1UL;
}

static void sim_mem_far_write(
    uint16_t seg,
    uint16_t off,
    const void *src,
    uint16_t len,
    void *ctx
) {
    sim_dos_mem_t *m = (sim_dos_mem_t *)ctx;
    uint32_t lin = (((uint32_t)seg) << 4U) + (uint32_t)off;
    if (((uint32_t)off + (uint32_t)len) > 65536UL) {
        m->wrap_violation = true;
    }
    if ((lin + (uint32_t)len) <= SIM_DOS_RAM_SIZE) {
        memcpy(&m->ram[lin], src, (size_t)len);
    }
    m->far_writes += 1UL;
}

static void sim_mem_set_units(uint8_t units, void *ctx) {
    sim_dos_mem_t *m = (sim_dos_mem_t *)ctx;
    m->header_units = units;
}

static void sim_mem_print_init(const char *dollar_msg, void *ctx) {
    sim_dos_mem_t *m = (sim_dos_mem_t *)ctx;
    size_t i = 0U;
    while ((dollar_msg[i] != '\0') && (i + 1U < sizeof(m->init_banner))) {
        m->init_banner[i] = dollar_msg[i];
        ++i;
    }
    m->init_banner[i] = '\0';
}

static uint32_t sim_lin(uint16_t seg, uint16_t off) {
    return (((uint32_t)seg) << 4U) + (uint32_t)off;
}

static void test_openlnk_sys_config_parser(void) {
    olnk_dos_sys_cfg_t cfg;

    /* Driver header attributes contract */
    CHECK(olnk_dos_header.next_driver == 0xFFFFFFFFUL, "header next_driver == 0xFFFFFFFF");
    CHECK(olnk_dos_header.attributes == OLNK_DOS_SYS_HEADER_ATTR,
          "header attrs == 0x2842 (Non-IBM|OpenCloseRM|GenIOCTL|LBA32)");
    CHECK(memcmp(olnk_dos_header.reserved, "OPENLNK", 7U) == 0, "header signature == OPENLNK");

    /* Defaults */
    olnk_dos_sys_parse_args(NULL, &cfg);
    CHECK((cfg.com_number == 1U) && (cfg.max_baud == 115200UL) && (cfg.max_units == 4U),
          "NULL cmdline yields defaults (COM1, 115200, 4)");

    /* Typical CONFIG.SYS line with driver path + CR/LF */
    olnk_dos_sys_parse_args("C:\\DOS\\OPENLNK.SYS /COM2 /BAUD:57600 /DRIVES:2\r\n", &cfg);
    CHECK((cfg.com_number == 2U) && (cfg.max_baud == 57600UL) && (cfg.max_units == 2U),
          "CONFIG.SYS /COM2 /BAUD:57600 /DRIVES:2 parsed accurately");

    /* Lowercase bare switches without filename prefix */
    olnk_dos_sys_parse_args("  /com3\t/baud:38400  /drives:6  ", &cfg);
    CHECK((cfg.com_number == 3U) && (cfg.max_baud == 38400UL) && (cfg.max_units == 6U),
          "case-insensitive bare switches parsed accurately");

    /* Alternate /COM:4 syntax + max drives 8 + lowest standard baud 9600 */
    olnk_dos_sys_parse_args("OPENLNK.SYS /COM:4 /BAUD:9600 /DRIVES:8\nIGNORED /COM1", &cfg);
    CHECK((cfg.com_number == 4U) && (cfg.max_baud == 9600UL) && (cfg.max_units == 8U),
          "/COM:4 /BAUD:9600 /DRIVES:8 parsed and stops at newline");

    /* Invalid values ignored cleanly */
    olnk_dos_sys_parse_args("OPENLNK.SYS /COM5 /BAUD:99999 /DRIVES:0 /DRIVES:9 /UNKNOWN", &cfg);
    CHECK((cfg.com_number == 1U) && (cfg.max_baud == 115200UL) && (cfg.max_units == 4U),
          "out-of-range switches fall back cleanly to defaults");
}

static void test_openlnk_sys_rh_marshalling_over_16550a(void) {
    olnk_dos_sys_mem_ops_t mem_ops;
    olnk_dos_io_ops_t cli_io;
    olnk_dos_io_ops_t srv_io;
    olnk_dos_disk_hooks_t disk_hooks;
    olnk_transport_t srv_tr;
    const uint16_t rh_seg = 0x0100U;
    const uint16_t rh_off = 0x0100U;
    const uint16_t drv_seg = 0x0200U;
    const uint16_t resident_end = 0x8450U;
    uint32_t rh_lin = sim_lin(rh_seg, rh_off);
    uint8_t *rh = &g_sim_mem.ram[rh_lin];
    const char *cfg_line = "C:\\OPENLNK.SYS /COM1 /BAUD:115200 /DRIVES:4\r\n";
    uint16_t st;
    uint16_t bpb_arr_off;
    uint16_t bpb_arr_seg;
    uint16_t bpb_off;
    uint16_t bpb_seg;
    uint16_t vol_off;
    uint16_t vol_seg;
    uint8_t u;
    size_t i;

    memset(&g_sim_mem, 0, sizeof(g_sim_mem));
    /* Place sentinel pattern at physical 0x0000..0x0FFF to detect any 64 KiB wraparound! */
    memset(&g_sim_mem.ram[0], 0xEE, 4096U);

    mem_ops.far_read = sim_mem_far_read;
    mem_ops.far_write = sim_mem_far_write;
    mem_ops.set_header_units = sim_mem_set_units;
    mem_ops.print_init_msg = sim_mem_print_init;
    mem_ops.host_table_base_off = 0x0400U;
    mem_ops.ctx = &g_sim_mem;

    /* Prepare server + disk (initially disconnected during offline boot INIT) */
    memset(&g_cli_uart_hw, 0, sizeof(g_cli_uart_hw));
    memset(&g_srv_uart_hw, 0, sizeof(g_srv_uart_hw));
    g_cli_uart_hw.base_port = OLNK_DOS_COM1_BASE;
    g_cli_uart_hw.chip = SIM_CHIP_16550A;
    g_cli_uart_hw.peer = NULL; /* Offline during boot! */
    g_cli_uart_hw.on_tx_pump = pump_dos_server;
    g_cli_uart_hw.on_idle_tick = tick_dos_server;
    g_cli_uart_hw.pump_ctx = &g_srv;
    g_cli_uart_hw.auto_tick_on_idle = true;

    g_srv_uart_hw.base_port = OLNK_DOS_COM2_BASE;
    g_srv_uart_hw.chip = SIM_CHIP_16550A;
    g_srv_uart_hw.peer = &g_cli_uart_hw;
    g_srv_uart_hw.auto_tick_on_idle = false;

    memset(&g_e2e_disk, 0, sizeof(g_e2e_disk));
    g_e2e_disk.present = true;
    g_e2e_disk.ready = true;
    build_fat16_vbr(g_e2e_disk.sectors[0], SIM_DISK_SECTORS, 0x12345678UL, "OPENLNKDOS ");

    disk_hooks.abs_read = mock_int25_read;
    disk_hooks.abs_write = mock_int26_write;
    disk_hooks.ctx = &g_e2e_disk;
    olnk_dos_disk_init(&g_e2e_disk_set, false, &disk_hooks);
    CHECK(olnk_dos_disk_add_unit(&g_e2e_disk_set, 2U), "RH test adds server C:");

    srv_io = make_sim_io(&g_srv_uart_hw);
    CHECK(olnk_dos_uart_init(&g_srv_uart, 2U, 0U, 9600UL, &srv_io, &srv_tr) == OLNK_OK,
          "RH test server COM2 init");
    CHECK(olnk_server_init(&g_srv, srv_tr, olnk_dos_disk_ops(&g_e2e_disk_set)) == OLNK_OK,
          "RH test server engine init");
    g_srv.link.max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    cli_io = make_sim_io(&g_cli_uart_hw);

    /* 1. Cmd 0x00 INIT while server offline: reserves 4 units (D:-G:) with valid default BPBs */
    memcpy(&g_sim_mem.ram[sim_lin(rh_seg, 0x0020U)], cfg_line, strlen(cfg_line) + 1U);
    memset(rh, 0, 32U);
    rh[0] = 23U;
    rh[2] = OLNK_DOS_CMD_INIT;
    olnk_write_u16_le(&rh[18], 0x0020U);
    olnk_write_u16_le(&rh[20], rh_seg);
    rh[22] = 3U; /* First drive = D: */

    st = olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io);
    CHECK(st == OLNK_DOS_STATUS_DONE, "RH Cmd 0x00 INIT offline succeeds (0x0100)");
    CHECK(olnk_read_u16_le(&rh[3]) == OLNK_DOS_STATUS_DONE, "RH INIT status written at rh+3");
    CHECK((rh[13] == 4U) && (g_sim_mem.header_units == 4U),
          "RH INIT reserves 4 units at rh+13 and patches header+0x0A");
    CHECK((olnk_read_u16_le(&rh[14]) == resident_end) && (olnk_read_u16_le(&rh[16]) == drv_seg),
          "RH INIT writes break address drv_seg:resident_end at rh+14");

    bpb_arr_off = olnk_read_u16_le(&rh[18]);
    bpb_arr_seg = olnk_read_u16_le(&rh[20]);
    CHECK(bpb_arr_seg == drv_seg, "RH INIT BPB pointer array segment == drv_seg");
    for (u = 0U; u < 4U; ++u) {
        uint16_t entry_off = olnk_read_u16_le(&g_sim_mem.ram[sim_lin(bpb_arr_seg, (uint16_t)(bpb_arr_off + (uint16_t)(u * 2U)))]);
        olnk_dos_bpb_t decoded;
        memset(&decoded, 0, sizeof(decoded));
        CHECK(olnk_decode_dos_bpb(&g_sim_mem.ram[sim_lin(bpb_arr_seg, entry_off)], OLNK_LEN_DOS_BPB, &decoded) == OLNK_OK,
              "RH INIT BPB array entry decodes cleanly");
        CHECK((decoded.bytes_per_sector == 512U) && (decoded.sectors_per_cluster == 2U) &&
                  (decoded.media_descriptor == 0xF8U) && (decoded.total_sectors_16 == 20480U),
              "RH INIT placeholder BPB is valid non-zero FAT16");
    }
    CHECK((strstr(g_sim_mem.init_banner, "OPENLNK.SYS") != NULL) &&
              (strstr(g_sim_mem.init_banner, "D:-G:") != NULL) &&
              (strstr(g_sim_mem.init_banner, "[standby]") != NULL) &&
              (g_sim_mem.init_banner[strlen(g_sim_mem.init_banner) - 1U] == '$'),
          "RH INIT prints $-terminated standby boot banner with drive range");

    /* 2. Bring server online -> Cmd 0x01 MEDIA_CHECK lazily connects & forces 0xFF for BPB load */
    g_cli_uart_hw.peer = &g_srv_uart_hw;
    memset(rh, 0, 32U);
    rh[0] = 19U;
    rh[1] = 0U; /* unit 0 = D: */
    rh[2] = OLNK_DOS_CMD_MEDIA_CHECK;
    rh[13] = 0xF8U;
    st = olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io);
    CHECK((st == OLNK_DOS_STATUS_DONE) && (olnk_read_u16_le(&rh[3]) == OLNK_DOS_STATUS_DONE),
          "RH Cmd 0x01 MEDIA_CHECK lazily connects after offline boot");
    CHECK(rh[14] == (uint8_t)OLNK_MEDIA_CHANGED,
          "first MEDIA_CHECK after offline boot returns 0xFF so DOS calls BUILD_BPB");
    vol_off = olnk_read_u16_le(&rh[15]);
    vol_seg = olnk_read_u16_le(&rh[17]);
    CHECK((vol_seg == drv_seg) &&
              (memcmp(&g_sim_mem.ram[sim_lin(vol_seg, vol_off)], "NO NAME    ", 12U) == 0),
          "RH MEDIA_CHECK 0xFF writes valid far pointer to previous volume label at rh+15");

    /* 3. Cmd 0x02 BUILD_BPB -> returns far pointer to packed 25-byte live FAT16 BPB at rh+18 */
    memset(rh, 0, 32U);
    rh[0] = 22U;
    rh[1] = 0U;
    rh[2] = OLNK_DOS_CMD_BUILD_BPB;
    rh[13] = 0xF8U;
    st = olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io);
    CHECK((st == OLNK_DOS_STATUS_DONE) && (olnk_read_u16_le(&rh[3]) == OLNK_DOS_STATUS_DONE),
          "RH Cmd 0x02 BUILD_BPB succeeds");
    bpb_off = olnk_read_u16_le(&rh[18]);
    bpb_seg = olnk_read_u16_le(&rh[20]);
    {
        olnk_dos_bpb_t live_bpb;
        memset(&live_bpb, 0, sizeof(live_bpb));
        CHECK((bpb_seg == drv_seg) &&
                  (olnk_decode_dos_bpb(&g_sim_mem.ram[sim_lin(bpb_seg, bpb_off)], OLNK_LEN_DOS_BPB, &live_bpb) == OLNK_OK),
              "RH BUILD_BPB far pointer at rh+18 decodes live packed BPB");
        CHECK((live_bpb.bytes_per_sector == 512U) && (live_bpb.total_sectors_16 == SIM_DISK_SECTORS) &&
                  (live_bpb.sectors_per_fat == 2U) && (live_bpb.media_descriptor == 0xF8U),
              "RH BUILD_BPB live geometry matches remote FAT16 VBR");
    }

    /* 4. Subsequent MEDIA_CHECK unchanged -> swap volume -> changed + previous label */
    memset(rh, 0, 32U);
    rh[0] = 19U;
    rh[1] = 0U;
    rh[2] = OLNK_DOS_CMD_MEDIA_CHECK;
    rh[13] = 0xF8U;
    CHECK(olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io) ==
              OLNK_DOS_STATUS_DONE,
          "RH MEDIA_CHECK steady-state succeeds");
    CHECK(rh[14] == (uint8_t)OLNK_MEDIA_UNCHANGED, "RH MEDIA_CHECK reports 0x01 unchanged");

    build_fat16_vbr(g_e2e_disk.sectors[0], SIM_DISK_SECTORS, 0xABCDEF01UL, "SWAPDISK   ");
    CHECK(olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io) ==
              OLNK_DOS_STATUS_DONE,
          "RH MEDIA_CHECK after swap succeeds");
    CHECK(rh[14] == (uint8_t)OLNK_MEDIA_CHANGED, "RH MEDIA_CHECK after swap reports 0xFF");
    vol_off = olnk_read_u16_le(&rh[15]);
    vol_seg = olnk_read_u16_le(&rh[17]);
    CHECK(strcmp((const char *)&g_sim_mem.ram[sim_lin(vol_seg, vol_off)], "OPENLNKDOS") == 0,
          "RH MEDIA_CHECK swap exposes previous volume label OPENLNKDOS");

    /* 5. Cmds 0x0D OPEN, 0x0E CLOSE, 0x0F REMOVABLE_MEDIA, 0x17/0x18 LOGICAL, 0x19 IOCTL_QUERY */
    memset(rh, 0, 32U);
    rh[0] = 13U;
    rh[1] = 0U;
    rh[2] = OLNK_DOS_CMD_OPEN;
    CHECK(olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io) ==
              OLNK_DOS_STATUS_DONE,
          "RH Cmd 0x0D OPEN");
    rh[2] = OLNK_DOS_CMD_CLOSE;
    CHECK(olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io) ==
              OLNK_DOS_STATUS_DONE,
          "RH Cmd 0x0E CLOSE");
    rh[2] = OLNK_DOS_CMD_REMOVABLE_MEDIA;
    CHECK(olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io) ==
              OLNK_DOS_STATUS_DONE,
          "RH Cmd 0x0F REMOVABLE_MEDIA unit 0");
    rh[1] = 7U;
    CHECK(olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT),
          "RH Cmd 0x0F REMOVABLE_MEDIA invalid unit -> 0x8101");
    rh[1] = 2U;
    rh[2] = OLNK_DOS_CMD_GET_LOGICAL;
    CHECK((olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io) ==
               OLNK_DOS_STATUS_DONE) &&
              (rh[1] == 0U),
          "RH Cmd 0x17 GET_LOGICAL writes 0 to rh+1");

    memset(rh, 0, 32U);
    rh[0] = 23U;
    rh[1] = 0U;
    rh[2] = OLNK_DOS_CMD_IOCTL_QUERY;
    rh[13] = OLNK_IOCTL_CAT_DISK;
    rh[14] = OLNK_IOCTL_FN_GET_MEDIA_ID;
    CHECK(olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io) ==
              OLNK_DOS_STATUS_DONE,
          "RH Cmd 0x19 IOCTL_QUERY supports 08h/66h");
    rh[14] = 0x99U;
    CHECK(olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND),
          "RH Cmd 0x19 IOCTL_QUERY rejects unsupported 08h/99h");

    /* 6. Cmd 0x13 GENERIC_IOCTL 08h/66h (Get Media ID via far param block) */
    memset(rh, 0, 32U);
    rh[0] = 23U;
    rh[1] = 0U;
    rh[2] = OLNK_DOS_CMD_GENERIC_IOCTL;
    rh[13] = OLNK_IOCTL_CAT_DISK;
    rh[14] = OLNK_IOCTL_FN_GET_MEDIA_ID;
    olnk_write_u16_le(&rh[19], 0x0200U);
    olnk_write_u16_le(&rh[21], rh_seg);
    memset(&g_sim_mem.ram[sim_lin(rh_seg, 0x0200U)], 0, OLNK_LEN_GENERIC_IOCTL_MID);
    st = olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io);
    {
        olnk_generic_ioctl_mid_t mid;
        CHECK(st == OLNK_DOS_STATUS_DONE, "RH Cmd 0x13 IOCTL 08h/66h succeeds");
        CHECK(olnk_decode_generic_ioctl_mid(
                  &g_sim_mem.ram[sim_lin(rh_seg, 0x0200U)], OLNK_LEN_GENERIC_IOCTL_MID, &mid) ==
                  OLNK_OK,
              "RH IOCTL 08h/66h copied 25-byte MID to far param block");
        CHECK((mid.serial_number == 0xABCDEF01UL) &&
                  (memcmp(mid.volume_label, "SWAPDISK   ", 11U) == 0) &&
                  (memcmp(mid.filesystem_type, "FAT16   ", 8U) == 0),
              "RH IOCTL 08h/66h serial, label, and FAT16 type exact");

        /* Also exercise Cmd 0x13 IOCTL 08h/46h (Set Media ID far param read & status writeback) */
        rh[14] = OLNK_IOCTL_FN_SET_MEDIA_ID;
        st = olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io);
        CHECK((st == OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND)) &&
                  (olnk_read_u16_le(&rh[3]) == st),
              "RH Cmd 0x13 IOCTL 08h/46h marshals 25B MID & propagates server status");
    }

    /* 7. 16-bit LBA WRITE (0x08) + READ (0x04) of 2 sectors */
    {
        uint32_t tx_lin = sim_lin(0x0300U, 0x0000U);
        uint32_t rx_lin = sim_lin(0x0380U, 0x0000U);
        for (i = 0U; i < (2U * OLNK_SECTOR_SIZE); ++i) {
            g_sim_mem.ram[tx_lin + i] = (uint8_t)((i * 13U + 5U) & 0xFFU);
        }
        memset(rh, 0, 32U);
        rh[0] = 22U;
        rh[1] = 0U;
        rh[2] = OLNK_DOS_CMD_WRITE;
        rh[13] = 0xF8U;
        olnk_write_u16_le(&rh[14], 0x0000U);
        olnk_write_u16_le(&rh[16], 0x0300U);
        olnk_write_u16_le(&rh[18], 2U);
        olnk_write_u16_le(&rh[20], 4U); /* 16-bit LBA = 4 */
        CHECK(olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io) ==
                  OLNK_DOS_STATUS_DONE,
              "RH Cmd 0x08 16-bit LBA WRITE succeeds");
        CHECK(olnk_read_u16_le(&rh[18]) == 2U, "RH WRITE reports 2 sectors transferred");

        memset(&g_sim_mem.ram[rx_lin], 0, 2U * OLNK_SECTOR_SIZE);
        rh[2] = OLNK_DOS_CMD_READ;
        olnk_write_u16_le(&rh[14], 0x0000U);
        olnk_write_u16_le(&rh[16], 0x0380U);
        olnk_write_u16_le(&rh[18], 2U);
        olnk_write_u16_le(&rh[20], 4U);
        CHECK(olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io) ==
                  OLNK_DOS_STATUS_DONE,
              "RH Cmd 0x04 16-bit LBA READ succeeds");
        CHECK((olnk_read_u16_le(&rh[18]) == 2U) &&
                  (memcmp(&g_sim_mem.ram[tx_lin], &g_sim_mem.ram[rx_lin], 2U * OLNK_SECTOR_SIZE) == 0),
              "RH 16-bit LBA WRITE->READ round-trip bit-for-bit identical");
    }

    /* 8. 32-bit LBA (rh+20==0xFFFF, rh+26) + 16 KiB multi-chunk (>4 KiB) + 64 KiB boundary wrap! */
    {
        /* Transfer buffer at 0x0000:0xF800 spans physical 0x0F800..0x137FF (crosses 0x10000!) */
        const uint16_t tx_seg = 0x0000U;
        const uint16_t tx_off = 0xF800U;
        const uint16_t rx_seg = 0x0500U;
        const uint16_t rx_off = 0xF800U; /* spans physical 0x14800..0x187FF */
        uint32_t tx_lin = sim_lin(tx_seg, tx_off);
        uint32_t rx_lin = sim_lin(rx_seg, rx_off);
        uint32_t bytes16k = 32UL * (uint32_t)OLNK_SECTOR_SIZE;
        bool sentinel_ok = true;

        for (i = 0U; i < (size_t)bytes16k; ++i) {
            g_sim_mem.ram[tx_lin + i] = (uint8_t)(((i * 41U) + 19U) ^ 0x5AU);
        }
        memset(&g_sim_mem.ram[rx_lin], 0, (size_t)bytes16k);

        memset(rh, 0, 32U);
        rh[0] = 30U;
        rh[1] = 0U;
        rh[2] = OLNK_DOS_CMD_WRITE_VERIFY;
        rh[13] = 0xF8U;
        olnk_write_u16_le(&rh[14], tx_off);
        olnk_write_u16_le(&rh[16], tx_seg);
        olnk_write_u16_le(&rh[18], 32U);     /* 32 sectors = 4x 4 KiB bounce chunks! */
        olnk_write_u16_le(&rh[20], 0xFFFFU); /* 32-bit LBA indicator */
        olnk_write_u32_le(&rh[26], 20UL);    /* 32-bit start LBA = 20 */
        st = olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io);
        CHECK((st == OLNK_DOS_STATUS_DONE) && (olnk_read_u16_le(&rh[18]) == 32U),
              "RH Cmd 0x09 32-bit LBA 16 KiB multi-chunk WRITE_VERIFY across 64 KiB boundary");

        memset(rh, 0, 32U);
        rh[0] = 30U;
        rh[1] = 0U;
        rh[2] = OLNK_DOS_CMD_READ;
        rh[13] = 0xF8U;
        olnk_write_u16_le(&rh[14], rx_off);
        olnk_write_u16_le(&rh[16], rx_seg);
        olnk_write_u16_le(&rh[18], 32U);
        olnk_write_u16_le(&rh[20], 0xFFFFU);
        olnk_write_u32_le(&rh[26], 20UL);
        st = olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io);
        CHECK((st == OLNK_DOS_STATUS_DONE) && (olnk_read_u16_le(&rh[18]) == 32U),
              "RH Cmd 0x04 32-bit LBA 16 KiB multi-chunk READ across 64 KiB boundary");
        CHECK(memcmp(&g_sim_mem.ram[tx_lin], &g_sim_mem.ram[rx_lin], (size_t)bytes16k) == 0,
              "RH 16 KiB 32-bit LBA WRITE_VERIFY->READ bit-for-bit identical across 64 KiB wrap");
        CHECK(!g_sim_mem.wrap_violation, "zero 16-bit offset wraparound violations in far copies");

        for (i = 0U; i < 4096U; ++i) {
            if (g_sim_mem.ram[i] != 0xEEU) {
                sentinel_ok = false;
                break;
            }
        }
        CHECK(sentinel_ok, "physical 0x0000:0000..0FFF sentinel untouched (no 64 KiB segment wrap)");
    }

    /* 9. Cmd 0x00 INIT while server is already online -> prefetches live BPB at boot */
    olnk_server_drop_link(&g_srv);
    memset(rh, 0, 32U);
    rh[0] = 23U;
    rh[2] = OLNK_DOS_CMD_INIT;
    olnk_write_u16_le(&rh[18], 0x0020U);
    olnk_write_u16_le(&rh[20], rh_seg);
    rh[22] = 3U;
    st = olnk_dos_sys_handle_rh(rh_seg, rh_off, drv_seg, resident_end, &mem_ops, &cli_io);
    CHECK((st == OLNK_DOS_STATUS_DONE) && (rh[13] == 1U) && (g_sim_mem.header_units == 1U),
          "RH INIT while online maps exact 1 live server unit");
    bpb_arr_off = olnk_read_u16_le(&rh[18]);
    bpb_arr_seg = olnk_read_u16_le(&rh[20]);
    {
        uint16_t entry0 = olnk_read_u16_le(&g_sim_mem.ram[sim_lin(bpb_arr_seg, bpb_arr_off)]);
        olnk_dos_bpb_t boot_bpb;
        memset(&boot_bpb, 0, sizeof(boot_bpb));
        CHECK(olnk_decode_dos_bpb(&g_sim_mem.ram[sim_lin(bpb_arr_seg, entry0)], OLNK_LEN_DOS_BPB, &boot_bpb) == OLNK_OK,
              "RH INIT online BPB array entry 0 decodes");
        CHECK((boot_bpb.bytes_per_sector == 512U) && (boot_bpb.total_sectors_16 == SIM_DISK_SECTORS),
              "RH INIT online prefetched live remote FAT16 BPB");
    }
    CHECK((strstr(g_sim_mem.init_banner, "(D:)") != NULL) &&
              (strstr(g_sim_mem.init_banner, "[connected]") != NULL),
          "RH INIT online boot banner reports (D:) [connected]");
}

/* =========================================================================
 * Test 5: Zero-Stdio DOS I/O HAL (dos_hal_io.c) & PSP Command-Line Parser
 * ========================================================================= */

typedef struct {
    char buf[256];
    size_t len;
} io_capture_t;

static void io_capture_sink(const char *data, size_t len, void *ctx) {
    io_capture_t *cap = (io_capture_t *)ctx;
    size_t i;
    for (i = 0U; (i < len) && (cap->len + 1U < sizeof(cap->buf)); ++i) {
        cap->buf[cap->len++] = data[i];
    }
    cap->buf[cap->len] = '\0';
}

static void io_capture_reset(io_capture_t *cap) {
    cap->len = 0U;
    cap->buf[0] = '\0';
}

static void test_dos_hal_io(void) {
    io_capture_t cap;

    /* 1. Minimal freestanding ASCII helpers */
    CHECK(olnk_ascii_isalpha('A') && olnk_ascii_isalpha('Z') &&
              olnk_ascii_isalpha('a') && olnk_ascii_isalpha('z'),
          "olnk_ascii_isalpha accepts ASCII letters");
    CHECK(!olnk_ascii_isalpha('0') && !olnk_ascii_isalpha('@') &&
              !olnk_ascii_isalpha('[') && !olnk_ascii_isalpha('`') &&
              !olnk_ascii_isalpha('{') && !olnk_ascii_isalpha('\0'),
          "olnk_ascii_isalpha rejects non-letters");
    CHECK((olnk_ascii_toupper('a') == 'A') && (olnk_ascii_toupper('z') == 'Z') &&
              (olnk_ascii_toupper('A') == 'A') && (olnk_ascii_toupper('5') == '5') &&
              (olnk_ascii_toupper('/') == '/'),
          "olnk_ascii_toupper uppercases ASCII lowercase only");
    CHECK(olnk_ascii_ieq_prefix("/baud:115200", "/BAUD:") &&
              olnk_ascii_ieq_prefix("/CoM2", "/COM") &&
              !olnk_ascii_ieq_prefix("/CO", "/COM") &&
              !olnk_ascii_ieq_prefix("/DIR", "/GET"),
          "olnk_ascii_ieq_prefix matches case-insensitively");
    CHECK((olnk_ascii_parse_u32("0") == 0UL) &&
              (olnk_ascii_parse_u32("65535") == 65535UL) &&
              (olnk_ascii_parse_u32("115200") == 115200UL) &&
              (olnk_ascii_parse_u32("4294967295") == 4294967295UL) &&
              (olnk_ascii_parse_u32("9600bps") == 9600UL) &&
              (olnk_ascii_parse_u32("") == 0UL) &&
              (olnk_ascii_parse_u32(NULL) == 0UL),
          "olnk_ascii_parse_u32 parses 0..UINT32_MAX cleanly");

    /* 2. Zero-stdio decimal, hex, and padded console output */
    io_capture_reset(&cap);
    olnk_dos_io_set_stdout_sink(io_capture_sink, &cap);

    olnk_dos_put_u32(0UL);
    CHECK(strcmp(cap.buf, "0") == 0, "olnk_dos_put_u32(0) == \"0\"");

    io_capture_reset(&cap);
    olnk_dos_put_u32(65535UL);
    CHECK(strcmp(cap.buf, "65535") == 0, "olnk_dos_put_u32(65535) == \"65535\"");

    io_capture_reset(&cap);
    olnk_dos_put_u32(4294967295UL);
    CHECK(strcmp(cap.buf, "4294967295") == 0, "olnk_dos_put_u32(4294967295) == \"4294967295\"");

    io_capture_reset(&cap);
    olnk_dos_put_hex16(0x0000U);
    olnk_dos_putc(',');
    olnk_dos_put_hex16(0x03F8U);
    olnk_dos_putc(',');
    olnk_dos_put_hex16(0xFFFFU);
    CHECK(strcmp(cap.buf, "0000,03F8,FFFF") == 0, "olnk_dos_put_hex16 formats 4 uppercase digits");

    io_capture_reset(&cap);
    olnk_dos_put_hex32(0x00000000UL);
    olnk_dos_putc(',');
    olnk_dos_put_hex32(0x4F4C4331UL);
    olnk_dos_putc(',');
    olnk_dos_put_hex32(0xDEADBEEFUL);
    olnk_dos_putc(',');
    olnk_dos_put_hex32(0xFFFFFFFFUL);
    CHECK(strcmp(cap.buf, "00000000,4F4C4331,DEADBEEF,FFFFFFFF") == 0,
          "olnk_dos_put_hex32 formats 8 uppercase digits");

    io_capture_reset(&cap);
    olnk_dos_put_str_pad("README.TXT", 14U);
    olnk_dos_putc('|');
    olnk_dos_put_str_pad("EXACT", 5U);
    olnk_dos_putc('|');
    olnk_dos_put_str_pad("TOOLONG", 4U);
    CHECK(strcmp(cap.buf, "README.TXT    |EXACT|TOOLONG") == 0,
          "olnk_dos_put_str_pad right-pads to width without truncating");

    olnk_dos_io_set_stdout_sink(NULL, NULL);

    /* 3. Zero-heap MS-DOS PSP command-line tokenizer */
    {
        char cmd1[] = "  /COM1\t/BAUD:115200   C:  \rEXTRA";
        char *argv1[8];
        int argc1 = olnk_dos_parse_cmdline(cmd1, argv1, 8);
        CHECK((argc1 == 4) && (argv1[0][0] == '\0') &&
                  (strcmp(argv1[1], "/COM1") == 0) &&
                  (strcmp(argv1[2], "/BAUD:115200") == 0) &&
                  (strcmp(argv1[3], "C:") == 0) &&
                  (argv1[4] == NULL),
              "olnk_dos_parse_cmdline splits whitespace and stops at CR");
    }
    {
        char cmd2[] = " /GET \"MY FILE.TXT\" OUT.BIN\r";
        char *argv2[8];
        int argc2 = olnk_dos_parse_cmdline(cmd2, argv2, 8);
        CHECK((argc2 == 4) && (strcmp(argv2[1], "/GET") == 0) &&
                  (strcmp(argv2[2], "MY FILE.TXT") == 0) &&
                  (strcmp(argv2[3], "OUT.BIN") == 0) &&
                  (argv2[4] == NULL),
              "olnk_dos_parse_cmdline handles quoted arguments in-place");
    }
    {
        char cmd3[] = "   \r";
        char *argv3[4];
        int argc3 = olnk_dos_parse_cmdline(cmd3, argv3, 4);
        CHECK((argc3 == 1) && (argv3[1] == NULL),
              "olnk_dos_parse_cmdline handles blank command tail");
    }

    /* 4. Zero-stdio file create / write / close round-trip */
    {
        const char *tmp_path = "openlnk_dos_io_test.tmp";
        const uint8_t payload[12] = { 'O', 'P', 'E', 'N', 'L', 'N', 'K', '!', 0x00U, 0x0DU, 0x0AU, 0xFFU };
        uint8_t readback[12];
        olnk_dos_file_t df;
        FILE *fp;
        size_t rd = 0U;

        memset(&df, 0, sizeof(df));
        memset(readback, 0, sizeof(readback));
        CHECK(olnk_dos_file_create(&df, tmp_path), "olnk_dos_file_create opens binary output file");
        CHECK(olnk_dos_file_write(&df, payload, sizeof(payload)) == sizeof(payload),
              "olnk_dos_file_write writes exact binary byte count");
        CHECK(olnk_dos_file_close(&df), "olnk_dos_file_close closes handle cleanly");

        fp = fopen(tmp_path, "rb");
        CHECK(fp != NULL, "readback fopen succeeds");
        if (fp != NULL) {
            rd = fread(readback, 1U, sizeof(readback), fp);
            (void)fclose(fp);
        }
        (void)remove(tmp_path);
        CHECK((rd == sizeof(payload)) && (memcmp(payload, readback, sizeof(payload)) == 0),
              "olnk_dos_file_create/write/close round-trip matches bit-for-bit");
    }
}

int main(void) {
    test_uart_register_programming_and_fifo_detect();
    test_dos_disk_hal_int25_26();
    test_openlnk_sys_over_simulated_16550a_uart();
    test_openlnk_sys_config_parser();
    test_openlnk_sys_rh_marshalling_over_16550a();
    test_dos_hal_io();

    printf("[test_dos_hal] PASSED=%d FAILED=%d\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
