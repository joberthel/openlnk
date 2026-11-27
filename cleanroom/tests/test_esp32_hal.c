/* cleanroom/tests/test_esp32_hal.c
 *
 * Host-side C99 unit & end-to-end loopback test suite for the ESP32 HAL:
 *   - esp32/src/esp32_hal_uart.c   (multi-instance, TX-drain barrier, sub-tick clamp)
 *   - esp32/src/esp32_hal_sd.c     (MBR/superfloppy FAT12/16, multi-drive, verify,
 *                                   write-protect, OOB, hot-swap, IOCTL 08h/46h)
 *   - esp32/src/esp32_hal_img.c    (raw .img & Fixed-Size .vhd footer stripping)
 *   - esp32/src/esp32_server.c     (monotonic timer, E2E loopback with olnk_client_t)
 *   - esp32/src/esp32_client.c     (mutex balance, E2E loopback, dump, keepalive,
 *                                   auto-reconnect & session resumption)
 *   - esp32/src/esp32_client_vfs.c (FatFs DiskIO geometry, ioctl, chunked R/W,
 *                                   RO/OLNK_MAP_READONLY_BIT, error mapping, unmount)
 *
 * Strictly ISO C99, zero dynamic allocation (malloc/free), zero floating-point.
 */
#include "openlnk_esp32.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            g_pass++;                                                         \
        } else {                                                              \
            g_fail++;                                                         \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        }                                                                     \
    } while (0)

/* =========================================================================
 * Mock ESP32 UART Driver with Strict Chronological Call Ordering & Baud Match
 * ========================================================================= */

#define MOCK_UART_RING_CAP 16384U
#define MOCK_UART_LOG_CAP  256U

typedef enum {
    EV_NONE = 0,
    EV_INSTALL,
    EV_UNINSTALL,
    EV_WRITE,
    EV_WAIT_TX_DONE,
    EV_SET_BAUD,
    EV_FLUSH_INPUT
} mock_uart_event_t;

typedef struct mock_uart mock_uart_t;

struct mock_uart {
    int port;
    uint32_t baudrate;
    bool installed;
    bool fail_wait_tx_done;
    bool fail_set_baud;

    /* RX ring buffer */
    uint8_t rx_ring[MOCK_UART_RING_CAP];
    size_t rx_head;
    size_t rx_tail;

    /* Recorded wait_ticks from read_bytes calls */
    uint32_t last_read_wait_ticks;
    size_t last_read_req_len;
    uint32_t read_calls;

    /* Chronological event log for verifying wait_tx_done BEFORE set_baudrate */
    mock_uart_event_t events[MOCK_UART_LOG_CAP];
    size_t event_count;
    bool baud_switch_order_violated;

    /* Optional null-modem peer for E2E loopback */
    mock_uart_t *peer;
    void (*on_tx_pump)(void *ctx);
    void (*on_idle_wait)(uint32_t wait_ticks, void *ctx);
    void *pump_ctx;
    bool pumping;
    uint32_t baud_mismatch_drops;
};

static void mock_uart_log(mock_uart_t *u, mock_uart_event_t ev) {
    if (u->event_count < MOCK_UART_LOG_CAP) {
        u->events[u->event_count++] = ev;
    }
}

static size_t mock_uart_rx_count(const mock_uart_t *u) {
    if (u->rx_head >= u->rx_tail) {
        return u->rx_head - u->rx_tail;
    }
    return (MOCK_UART_RING_CAP - u->rx_tail) + u->rx_head;
}

static void mock_uart_rx_push(mock_uart_t *u, uint8_t b) {
    size_t next = (u->rx_head + 1U) % MOCK_UART_RING_CAP;
    if (next != u->rx_tail) {
        u->rx_ring[u->rx_head] = b;
        u->rx_head = next;
    }
}

static size_t mock_uart_rx_pop(mock_uart_t *u, uint8_t *dst, size_t max_len) {
    size_t n = 0U;
    while ((n < max_len) && (u->rx_head != u->rx_tail)) {
        dst[n++] = u->rx_ring[u->rx_tail];
        u->rx_tail = (u->rx_tail + 1U) % MOCK_UART_RING_CAP;
    }
    return n;
}

static int mock_uart_install(
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
    mock_uart_t *u = (mock_uart_t *)ctx;
    (void)tx_pin;
    (void)rx_pin;
    (void)rts_pin;
    (void)cts_pin;
    (void)use_hw_flow_control;
    (void)rx_buf_size;
    (void)tx_buf_size;
    u->port = uart_port;
    u->baudrate = baudrate;
    u->installed = true;
    mock_uart_log(u, EV_INSTALL);
    return 0;
}

static int mock_uart_uninstall(int uart_port, void *ctx) {
    mock_uart_t *u = (mock_uart_t *)ctx;
    (void)uart_port;
    u->installed = false;
    mock_uart_log(u, EV_UNINSTALL);
    return 0;
}

static int mock_uart_rx_buffered(int uart_port, size_t *out_len, void *ctx) {
    mock_uart_t *u = (mock_uart_t *)ctx;
    (void)uart_port;
    if ((mock_uart_rx_count(u) == 0U) && (u->on_tx_pump != NULL) && (!u->pumping)) {
        u->pumping = true;
        u->on_tx_pump(u->pump_ctx);
        u->pumping = false;
    }
    *out_len = mock_uart_rx_count(u);
    return 0;
}

static int mock_uart_read_bytes(
    int uart_port,
    uint8_t *buf,
    size_t len,
    uint32_t wait_ticks,
    void *ctx
) {
    mock_uart_t *u = (mock_uart_t *)ctx;
    size_t n;
    (void)uart_port;

    u->last_read_wait_ticks = wait_ticks;
    u->last_read_req_len = len;
    u->read_calls += 1UL;

    if ((mock_uart_rx_count(u) == 0U) && (u->on_tx_pump != NULL) && (!u->pumping)) {
        u->pumping = true;
        u->on_tx_pump(u->pump_ctx);
        u->pumping = false;
    }

    if ((mock_uart_rx_count(u) == 0U) && (wait_ticks > 0UL) &&
        (u->on_idle_wait != NULL) && (!u->pumping)) {
        uint32_t step;
        u->pumping = true;
        for (step = 0UL; (step < wait_ticks) && (mock_uart_rx_count(u) == 0U); ++step) {
            u->on_idle_wait(1UL, u->pump_ctx);
        }
        u->pumping = false;
    }

    n = mock_uart_rx_pop(u, buf, len);
    return (int)n;
}

static int mock_uart_write_bytes(
    int uart_port,
    const uint8_t *buf,
    size_t len,
    void *ctx
) {
    mock_uart_t *u = (mock_uart_t *)ctx;
    size_t i;
    (void)uart_port;

    mock_uart_log(u, EV_WRITE);
    if (u->peer != NULL) {
        for (i = 0U; i < len; ++i) {
            if (u->baudrate == u->peer->baudrate) {
                mock_uart_rx_push(u->peer, buf[i]);
            } else {
                u->baud_mismatch_drops += 1UL;
            }
        }
        if ((u->on_tx_pump != NULL) && (!u->pumping)) {
            u->pumping = true;
            u->on_tx_pump(u->pump_ctx);
            u->pumping = false;
        }
    }
    return (int)len;
}

static int mock_uart_wait_tx_done(int uart_port, uint32_t wait_ticks, void *ctx) {
    mock_uart_t *u = (mock_uart_t *)ctx;
    (void)uart_port;
    (void)wait_ticks;
    mock_uart_log(u, EV_WAIT_TX_DONE);
    return u->fail_wait_tx_done ? -1 : 0;
}

static int mock_uart_set_baudrate(int uart_port, uint32_t baudrate, void *ctx) {
    mock_uart_t *u = (mock_uart_t *)ctx;
    (void)uart_port;

    /* Enforce that wait_tx_done was ALWAYS the immediately preceding event */
    if ((u->event_count == 0U) || (u->events[u->event_count - 1U] != EV_WAIT_TX_DONE)) {
        u->baud_switch_order_violated = true;
    }

    mock_uart_log(u, EV_SET_BAUD);
    if (u->fail_set_baud) {
        return -1;
    }
    u->baudrate = baudrate;
    return 0;
}

static int mock_uart_flush_input(int uart_port, void *ctx) {
    mock_uart_t *u = (mock_uart_t *)ctx;
    (void)uart_port;
    u->rx_head = 0U;
    u->rx_tail = 0U;
    mock_uart_log(u, EV_FLUSH_INPUT);
    return 0;
}

static olnk_esp32_uart_ops_t make_mock_uart_ops(mock_uart_t *u, uint32_t tick_ms) {
    olnk_esp32_uart_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.install = mock_uart_install;
    ops.uninstall = mock_uart_uninstall;
    ops.read_bytes = mock_uart_read_bytes;
    ops.rx_buffered_len = mock_uart_rx_buffered;
    ops.write_bytes = mock_uart_write_bytes;
    ops.wait_tx_done = mock_uart_wait_tx_done;
    ops.set_baudrate = mock_uart_set_baudrate;
    ops.flush_input = mock_uart_flush_input;
    ops.tick_period_ms = tick_ms;
    ops.ctx = u;
    return ops;
}

/* =========================================================================
 * Mock SD Card Block Storage Backend
 * ========================================================================= */

#define SIM_SD_MAX_BLOCKS 320U

typedef struct {
    uint8_t blocks[SIM_SD_MAX_BLOCKS][OLNK_SECTOR_SIZE];
    uint32_t total_blocks;
    bool present;
    bool corrupt_next_write;
    uint32_t read_ops;
    uint32_t write_ops;
} sim_sd_card_t;

static bool sim_sd_read(uint8_t *dst, uint32_t start_block, uint32_t block_count, void *ctx) {
    sim_sd_card_t *c = (sim_sd_card_t *)ctx;
    if ((c == NULL) || (!c->present) || (dst == NULL) || (block_count == 0UL)) {
        return false;
    }
    if ((start_block >= c->total_blocks) ||
        ((start_block + block_count) < start_block) ||
        ((start_block + block_count) > c->total_blocks)) {
        return false;
    }
    memcpy(dst, c->blocks[start_block], (size_t)block_count * OLNK_SECTOR_SIZE);
    c->read_ops += 1UL;
    return true;
}

static bool sim_sd_write(const uint8_t *src, uint32_t start_block, uint32_t block_count, void *ctx) {
    sim_sd_card_t *c = (sim_sd_card_t *)ctx;
    if ((c == NULL) || (!c->present) || (src == NULL) || (block_count == 0UL)) {
        return false;
    }
    if ((start_block >= c->total_blocks) ||
        ((start_block + block_count) < start_block) ||
        ((start_block + block_count) > c->total_blocks)) {
        return false;
    }
    memcpy(c->blocks[start_block], src, (size_t)block_count * OLNK_SECTOR_SIZE);
    if (c->corrupt_next_write) {
        c->blocks[start_block][17] ^= 0xFFU;
        c->corrupt_next_write = false;
    }
    c->write_ops += 1UL;
    return true;
}

static bool sim_sd_is_present(void *ctx) {
    const sim_sd_card_t *c = (const sim_sd_card_t *)ctx;
    return (c != NULL) && c->present;
}

static olnk_esp32_sd_block_ops_t make_sim_sd_ops(sim_sd_card_t *c) {
    olnk_esp32_sd_block_ops_t ops;
    ops.read_blocks = sim_sd_read;
    ops.write_blocks = sim_sd_write;
    ops.is_present = sim_sd_is_present;
    ops.total_blocks = c->total_blocks;
    ops.ctx = c;
    return ops;
}

static void write_mbr_entry(
    uint8_t mbr[OLNK_SECTOR_SIZE],
    size_t slot,
    uint8_t ptype,
    uint32_t start_lba,
    uint32_t num_sectors
) {
    uint8_t *ent = &mbr[0x1BEU + slot * 16U];
    memset(ent, 0, 16U);
    ent[0] = (slot == 0U) ? 0x80U : 0x00U;
    ent[4] = ptype;
    olnk_write_u32_le(&ent[8], start_lba);
    olnk_write_u32_le(&ent[12], num_sectors);
    mbr[510] = 0x55U;
    mbr[511] = 0xAAU;
}

/* =========================================================================
 * Test 1: Hardened Multi-Instance ESP32 UART HAL
 * ========================================================================= */

static mock_uart_t g_mu1;
static mock_uart_t g_mu2;

static void test_uart_hal(void) {
    olnk_esp32_uart_t u1;
    olnk_esp32_uart_t u2;
    olnk_esp32_uart_cfg_t cfg1;
    olnk_esp32_uart_cfg_t cfg2;
    olnk_esp32_uart_ops_t ops1;
    olnk_esp32_uart_ops_t ops2;
    olnk_transport_t tr1;
    olnk_transport_t tr2;
    uint8_t buf[16];
    int n;

    /* 1. Sub-tick timeout conversion rounding (100 Hz -> 10 ms/tick, 1000 Hz -> 1 ms/tick) */
    CHECK(olnk_esp32_ms_to_ticks(0UL, 10UL) == 0UL, "0 ms -> 0 ticks");
    CHECK(olnk_esp32_ms_to_ticks(1UL, 10UL) == 1UL, "1 ms -> 1 tick (never truncated to 0)");
    CHECK(olnk_esp32_ms_to_ticks(5UL, 10UL) == 1UL, "5 ms -> 1 tick at 100 Hz");
    CHECK(olnk_esp32_ms_to_ticks(10UL, 10UL) == 1UL, "10 ms -> 1 tick at 100 Hz");
    CHECK(olnk_esp32_ms_to_ticks(11UL, 10UL) == 2UL, "11 ms -> 2 ticks at 100 Hz");
    CHECK(olnk_esp32_ms_to_ticks(55UL, 10UL) == 6UL, "55 ms -> 6 ticks at 100 Hz");
    CHECK(olnk_esp32_ms_to_ticks(275UL, 10UL) == 28UL, "275 ms -> 28 ticks at 100 Hz");
    CHECK(olnk_esp32_ms_to_ticks(5UL, 1UL) == 5UL, "5 ms -> 5 ticks at 1000 Hz");
    CHECK(olnk_esp32_ms_to_ticks(UINT32_MAX, 10UL) == (UINT32_MAX / 10UL), "UINT32_MAX overflow-safe");

    /* 2. Invalid argument validation */
    olnk_esp32_uart_cfg_default(&cfg1, 1, 17, 16);
    CHECK((cfg1.baudrate == 9600UL) && (!cfg1.use_hw_flow_control) &&
              (cfg1.rx_buf_size == 4096U) && (cfg1.tx_buf_size == 1024U) &&
              (cfg1.rts_pin == OLNK_ESP32_PIN_UNUSED) && (cfg1.cts_pin == OLNK_ESP32_PIN_UNUSED),
          "uart_cfg_default sets 3-wire 9600 4096/1024 defaults");

    memset(&g_mu1, 0, sizeof(g_mu1));
    ops1 = make_mock_uart_ops(&g_mu1, 10UL);
    CHECK(olnk_esp32_uart_init(NULL, &cfg1, &ops1, &tr1) == OLNK_ERR_INVALID_ARG, "NULL uart rejected");
    CHECK(olnk_esp32_uart_init(&u1, NULL, &ops1, &tr1) == OLNK_ERR_INVALID_ARG, "NULL cfg rejected");
    CHECK(olnk_esp32_uart_init(&u1, &cfg1, &ops1, NULL) == OLNK_ERR_INVALID_ARG, "NULL transport rejected");
    cfg1.baudrate = 12345UL;
    CHECK(olnk_esp32_uart_init(&u1, &cfg1, &ops1, &tr1) == OLNK_ERR_INVALID_ARG, "invalid baud rejected");
    cfg1.baudrate = 9600UL;
    cfg1.uart_port = -1;
    CHECK(olnk_esp32_uart_init(&u1, &cfg1, &ops1, &tr1) == OLNK_ERR_INVALID_ARG, "negative port rejected");
    cfg1.uart_port = 1;

    /* 3. Multi-instance initialization (port 1 and port 2 simultaneously) */
    memset(&g_mu1, 0, sizeof(g_mu1));
    memset(&g_mu2, 0, sizeof(g_mu2));
    ops1 = make_mock_uart_ops(&g_mu1, 10UL);
    ops2 = make_mock_uart_ops(&g_mu2, 10UL);
    olnk_esp32_uart_cfg_default(&cfg1, 1, 17, 16);
    olnk_esp32_uart_cfg_default(&cfg2, 2, 4, 5);

    CHECK(olnk_esp32_uart_init(&u1, &cfg1, &ops1, &tr1) == OLNK_OK, "UART1 init ok");
    CHECK(olnk_esp32_uart_init(&u2, &cfg2, &ops2, &tr2) == OLNK_OK, "UART2 init ok");
    CHECK(g_mu1.installed && g_mu2.installed && (u1.uart_port == 1) && (u2.uart_port == 2),
          "two UART instances coexist independently");

    /* 4. Live sub-tick timeout clamping on empty RX */
    n = tr1.read(buf, sizeof(buf), 5UL, tr1.user_data);
    CHECK((n == 0) && (g_mu1.last_read_wait_ticks == 1UL) && (g_mu1.last_read_req_len == 1U),
          "5 ms read at 100 Hz waits 1 full tick for 1st byte (never 0-tick poll)");

    /* 5. Fast-path buffered read + flush_rx */
    mock_uart_rx_push(&g_mu1, 0xAAU);
    mock_uart_rx_push(&g_mu1, 0x55U);
    mock_uart_rx_push(&g_mu2, 0x77U);
    n = tr1.read(buf, sizeof(buf), 50UL, tr1.user_data);
    CHECK((n == 2) && (buf[0] == 0xAAU) && (buf[1] == 0x55U), "UART1 reads own 2 bytes immediately");
    n = tr2.read(buf, sizeof(buf), 50UL, tr2.user_data);
    CHECK((n == 1) && (buf[0] == 0x77U), "UART2 reads own isolated byte");

    mock_uart_rx_push(&g_mu1, 0xEEU);
    CHECK(olnk_esp32_uart_flush_rx(&u1) == OLNK_OK, "flush_rx returns OLNK_OK");
    CHECK(mock_uart_rx_count(&g_mu1) == 0U, "flush_rx clears RX ring buffer");

    /* 6. Phase-B baud switching barrier: wait_tx_done strictly BEFORE set_baudrate */
    buf[0] = 0x04U;
    CHECK(tr1.write(buf, 1U, 100UL, tr1.user_data) == 1, "write target baud index");
    CHECK(tr1.set_baud(19200UL, tr1.user_data), "switch UART1 to 19200 bps");
    CHECK(!g_mu1.baud_switch_order_violated, "wait_tx_done strictly preceded set_baudrate");
    CHECK((u1.current_baud == 19200UL) && (g_mu1.baudrate == 19200UL) &&
              (u1.baud_switches == 1UL) && (u1.tx_flush_calls == 1UL),
          "UART1 baud & telemetry updated");
    CHECK(u2.current_baud == 9600UL, "UART2 baud unaffected by UART1 switch");

    /* Verify wait_tx_done failure aborts before set_baudrate */
    g_mu1.fail_wait_tx_done = true;
    CHECK(!tr1.set_baud(38400UL, tr1.user_data), "wait_tx_done failure aborts baud switch");
    CHECK((u1.current_baud == 19200UL) && (g_mu1.baudrate == 19200UL),
          "baud unchanged when TX drain fails");
    g_mu1.fail_wait_tx_done = false;

    /* Unsupported baud rejected without touching hardware */
    CHECK(!tr1.set_baud(77777UL, tr1.user_data), "unsupported baud rejected");
    CHECK(u1.current_baud == 19200UL, "current_baud preserved on invalid baud");

    /* 7. Deinit drains TX, uninstalls, and blocks subsequent I/O */
    CHECK(olnk_esp32_uart_deinit(&u1) == OLNK_OK, "UART1 deinit ok");
    CHECK((!u1.initialized) && (!g_mu1.installed), "UART1 marked uninitialized and uninstalled");
    CHECK(tr1.read(buf, 1U, 10UL, tr1.user_data) < 0, "read fails after deinit");
    CHECK(tr1.write(buf, 1U, 10UL, tr1.user_data) < 0, "write fails after deinit");
    CHECK(!tr1.set_baud(9600UL, tr1.user_data), "set_baud fails after deinit");
    CHECK(olnk_esp32_uart_flush_rx(&u1) == OLNK_ERR_INVALID_ARG, "flush_rx fails after deinit");
    CHECK(olnk_esp32_uart_deinit(&u2) == OLNK_OK, "UART2 deinit ok");
}

/* =========================================================================
 * Test 2: Raw SD-Card Block Storage HAL (MBR, Superfloppy, Multi-Drive, Errors)
 * ========================================================================= */

static sim_sd_card_t g_sd_mbr_card;
static sim_sd_card_t g_sd_floppy_card;
static olnk_esp32_sd_drive_t g_sd_part0;
static olnk_esp32_sd_drive_t g_sd_part1;
static olnk_esp32_sd_drive_t g_sd_floppy;
static olnk_esp32_drive_set_t g_sd_set;

static void test_sd_block_hal(void) {
    olnk_esp32_sd_block_ops_t mbr_ops;
    olnk_esp32_sd_block_ops_t flp_ops;
    olnk_esp32_sd_opts_t opts;
    olnk_storage_ops_t ops;
    olnk_drive_info_t info;
    olnk_dos_bpb_t bpb;
    olnk_generic_ioctl_mid_t mid;
    uint8_t media_status = 0U;
    uint8_t sec_w[OLNK_SECTOR_SIZE * 2U];
    uint8_t sec_r[OLNK_SECTOR_SIZE * 2U];
    uint16_t xfer = 0U;
    size_t i;

    /* Prepare MBR card: Partition 0 (LBA 63, 128 sectors) + Partition 1 (LBA 200, 96 sectors) */
    memset(&g_sd_mbr_card, 0, sizeof(g_sd_mbr_card));
    g_sd_mbr_card.total_blocks = SIM_SD_MAX_BLOCKS;
    g_sd_mbr_card.present = true;
    write_mbr_entry(g_sd_mbr_card.blocks[0], 0U, 0x06U, 63UL, 128UL);
    write_mbr_entry(g_sd_mbr_card.blocks[0], 1U, 0x04U, 200UL, 96UL);
    olnk_fat_build_synthetic_vbr(g_sd_mbr_card.blocks[63], 128UL, 0xF8U, "SD_PART0", 0x11112222UL);
    olnk_fat_build_synthetic_vbr(g_sd_mbr_card.blocks[200], 96UL, 0xF8U, "SD_PART1", 0x33334444UL);

    /* Prepare superfloppy FAT12 card (LBA 0, 64 sectors) */
    memset(&g_sd_floppy_card, 0, sizeof(g_sd_floppy_card));
    g_sd_floppy_card.total_blocks = 64UL;
    g_sd_floppy_card.present = true;
    olnk_fat_build_synthetic_vbr(g_sd_floppy_card.blocks[0], 64UL, 0xF0U, "FLOPPY12", 0xAAAA5555UL);

    mbr_ops = make_sim_sd_ops(&g_sd_mbr_card);
    flp_ops = make_sim_sd_ops(&g_sd_floppy_card);

    /* Init Partition 0 (auto-detect MBR), Partition 1 (explicit slot 1), and Superfloppy */
    olnk_esp32_sd_opts_default(&opts);
    CHECK(olnk_esp32_sd_init_custom(&g_sd_part0, &mbr_ops, &opts) == OLNK_OK, "SD MBR auto init");
    CHECK((g_sd_part0.partition_lba == 63UL) && (g_sd_part0.volume_sectors == 128UL),
          "MBR Partition 0 auto-detected at LBA 63");

    opts.partition_index = 1;
    CHECK(olnk_esp32_sd_init_custom(&g_sd_part1, &mbr_ops, &opts) == OLNK_OK, "SD MBR slot 1 init");
    CHECK((g_sd_part1.partition_lba == 200UL) && (g_sd_part1.volume_sectors == 96UL),
          "Explicit MBR Partition 1 attached at LBA 200");

    olnk_esp32_sd_opts_default(&opts);
    CHECK(olnk_esp32_sd_init_custom(&g_sd_floppy, &flp_ops, &opts) == OLNK_OK, "SD superfloppy init");
    CHECK((g_sd_floppy.partition_lba == 0UL) && (g_sd_floppy.volume_sectors == 64UL),
          "Superfloppy FAT12 detected at LBA 0");

    /* Assemble multi-drive set: A:=floppy, C:=part0, D:=part1 */
    olnk_esp32_drive_set_init(&g_sd_set);
    CHECK(olnk_esp32_drive_set_attach(&g_sd_set, 0U, &g_sd_floppy), "attach A:");
    CHECK(olnk_esp32_drive_set_attach(&g_sd_set, 2U, &g_sd_part0), "attach C:");
    CHECK(olnk_esp32_drive_set_attach(&g_sd_set, 3U, &g_sd_part1), "attach D:");
    ops = olnk_esp32_drive_set_ops(&g_sd_set);

    CHECK(ops.drive_count(ops.user_data) == 4U, "multi-drive set drive_count == 4");
    CHECK(ops.get_drive_info(1U, &info, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT),
          "unmapped B: returns 0x8101 UNKNOWN_UNIT");

    CHECK(ops.get_drive_info(0U, &info, ops.user_data) == OLNK_DOS_STATUS_DONE, "get_drive_info A:");
    CHECK((info.volume_serial == 0xAAAA5555UL) && (memcmp(info.filesystem_type, "FAT12", 5U) == 0),
          "A: FAT12 VBR metadata matches");
    CHECK(ops.get_drive_info(2U, &info, ops.user_data) == OLNK_DOS_STATUS_DONE, "get_drive_info C:");
    CHECK(info.volume_serial == 0x11112222UL, "C: MBR Partition 0 serial matches");
    CHECK(ops.get_drive_info(3U, &info, ops.user_data) == OLNK_DOS_STATUS_DONE, "get_drive_info D:");
    CHECK(info.volume_serial == 0x33334444UL, "D: MBR Partition 1 serial matches");

    CHECK(ops.build_bpb(2U, 0xF8U, &bpb, ops.user_data) == OLNK_DOS_STATUS_DONE, "build_bpb C:");
    CHECK((bpb.bytes_per_sector == 512U) && (bpb.total_sectors_16 == 128U), "C: BPB geometry exact");

    /* Multi-sector write + verify + readback */
    for (i = 0U; i < sizeof(sec_w); ++i) {
        sec_w[i] = (uint8_t)((i * 29U + 5U) & 0xFFU);
    }
    CHECK(ops.write_sectors(2U, 10UL, 2U, sec_w, &xfer, true, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "SD WRITE_VERIFY succeeds on faithful media");
    CHECK(xfer == 2U, "2 sectors written");
    CHECK(memcmp(g_sd_mbr_card.blocks[63U + 10U], sec_w, sizeof(sec_w)) == 0,
          "Partition-relative LBA 10 landed at physical block 73");

    memset(sec_r, 0, sizeof(sec_r));
    CHECK(ops.read_sectors(2U, 10UL, 2U, sec_r, &xfer, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "SD READ_SECTORS succeeds");
    CHECK((xfer == 2U) && (memcmp(sec_w, sec_r, sizeof(sec_w)) == 0), "SD readback exact");

    /* Write-verify mismatch detection */
    g_sd_mbr_card.corrupt_next_write = true;
    CHECK(ops.write_sectors(2U, 12UL, 1U, sec_w, &xfer, true, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT),
          "WRITE_VERIFY detects read-back bit corruption -> 0x810A");

    /* Out-of-bounds LBA & wraparound */
    CHECK(ops.read_sectors(2U, 128UL, 1U, sec_r, &xfer, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND),
          "LBA == volume_sectors returns 0x8108");
    CHECK(ops.read_sectors(2U, 127UL, 2U, sec_r, &xfer, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND),
          "LBA crossing partition end returns 0x8108");
    CHECK(ops.read_sectors(2U, 0xFFFFFFFFUL, 2U, sec_r, &xfer, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND),
          "uint32 LBA wraparound returns 0x8108");

    /* Write-protect enforcement (0x8100) */
    g_sd_part0.opts.write_protected = true;
    CHECK(ops.write_sectors(2U, 10UL, 1U, sec_w, &xfer, false, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT),
          "write-protected drive returns 0x8100 WRITE_PROTECT");
    memset(&mid, 0, sizeof(mid));
    mid.serial_number = 0xDEADC0DEUL;
    CHECK(ops.set_media_id(2U, &mid, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT),
          "set_media_id on write-protected drive returns 0x8100");
    g_sd_part0.opts.write_protected = false;

    /* Generic IOCTL 08h/46h Set Media ID (with extended boot sig 0x29) */
    mid.info_level = 0U;
    mid.serial_number = 0x87654321UL;
    olnk_label_pad_copy(mid.volume_label, 11U, "NEW_ESP_SD");
    olnk_label_pad_copy(mid.filesystem_type, 8U, "FAT16");
    CHECK(ops.set_media_id(2U, &mid, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "IOCTL 08h/46h Set Media ID succeeds");
    CHECK(olnk_read_u32_le(&g_sd_mbr_card.blocks[63][39]) == 0x87654321UL,
          "Set Media ID persisted serial into VBR on card");
    CHECK(memcmp(&g_sd_mbr_card.blocks[63][43], "NEW_ESP_SD ", 11U) == 0,
          "Set Media ID persisted 11-byte volume label into VBR");

    /* Media check unchanged after Set Media ID */
    CHECK(ops.media_check(2U, 0xF8U, &media_status, &info, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "media_check after Set Media ID");
    CHECK((media_status == (uint8_t)OLNK_MEDIA_UNCHANGED) && (info.volume_serial == 0x87654321UL),
          "media_check stays UNCHANGED (0x01) with updated serial");

    /* Card removal -> 0x8102 DRIVE_NOT_READY */
    g_sd_mbr_card.present = false;
    CHECK(ops.media_check(2U, 0xF8U, &media_status, &info, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY),
          "removed card media_check returns 0x8102");
    CHECK(ops.read_sectors(2U, 0UL, 1U, sec_r, &xfer, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY),
          "removed card read returns 0x8102");
    CHECK(ops.write_sectors(2U, 0UL, 1U, sec_w, &xfer, false, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY),
          "removed card write returns 0x8102");
    CHECK(ops.get_drive_info(2U, &info, ops.user_data) == OLNK_DOS_STATUS_DONE && (!info.ready),
          "removed card get_drive_info marks ready=false");

    /* Card re-inserted with new FAT volume -> MEDIA_CHANGED (0xFF) ONCE, then UNCHANGED (0x01) */
    olnk_fat_build_synthetic_vbr(g_sd_mbr_card.blocks[63], 128UL, 0xF8U, "SWAPPED_SD", 0xCAFEBABEUL);
    g_sd_mbr_card.present = true;
    CHECK(ops.media_check(2U, 0xF8U, &media_status, &info, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "re-inserted card media_check returns DONE");
    CHECK((media_status == (uint8_t)OLNK_MEDIA_CHANGED) && (info.volume_serial == 0xCAFEBABEUL),
          "re-inserted card reports MEDIA_CHANGED (0xFF) with new VBR serial");

    CHECK(ops.media_check(2U, 0xF8U, &media_status, &info, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "second media_check after re-insert");
    CHECK(media_status == (uint8_t)OLNK_MEDIA_UNCHANGED,
          "subsequent media_check clears back to UNCHANGED (0x01)");

    /* Non-FAT (unformatted) card -> 0x8107 UNKNOWN_MEDIA */
    memset(g_sd_floppy_card.blocks[0], 0, OLNK_SECTOR_SIZE);
    CHECK(ops.build_bpb(0U, 0xF0U, &bpb, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_MEDIA),
          "unformatted card build_bpb returns 0x8107 UNKNOWN_MEDIA");
}

/* =========================================================================
 * Test 3: Disk Image File Backend (.img & Fixed-Size .vhd)
 * ========================================================================= */

static olnk_esp32_img_drive_t g_img_flp;
static olnk_esp32_img_drive_t g_img_vhd;
static olnk_esp32_img_drive_set_t g_img_set;

static bool write_temp_image_file(
    const char *path,
    uint32_t total_sectors,
    uint32_t part_lba,
    uint32_t part_sectors,
    const char *label,
    uint32_t serial,
    bool append_vhd_footer
) {
    FILE *fp = fopen(path, "wb");
    uint8_t sec[OLNK_SECTOR_SIZE];
    uint32_t s;

    if (fp == NULL) {
        return false;
    }

    for (s = 0UL; s < total_sectors; ++s) {
        memset(sec, 0, sizeof(sec));
        if ((part_lba > 0UL) && (s == 0UL)) {
            write_mbr_entry(sec, 0U, 0x06U, part_lba, part_sectors);
        } else if (s == part_lba) {
            olnk_fat_build_synthetic_vbr(
                sec,
                (part_lba > 0UL) ? part_sectors : total_sectors,
                (part_lba > 0UL) ? 0xF8U : 0xF0U,
                label,
                serial
            );
        }
        if (fwrite(sec, 1U, OLNK_SECTOR_SIZE, fp) != OLNK_SECTOR_SIZE) {
            (void)fclose(fp);
            return false;
        }
    }

    if (append_vhd_footer) {
        memset(sec, 0, sizeof(sec));
        memcpy(sec, "conectix", 8U);
        if (fwrite(sec, 1U, OLNK_SECTOR_SIZE, fp) != OLNK_SECTOR_SIZE) {
            (void)fclose(fp);
            return false;
        }
    }

    (void)fclose(fp);
    return true;
}

static void test_img_file_hal(void) {
    const char *img_path = "test_esp32_tmp_floppy.img";
    const char *vhd_path = "test_esp32_tmp_disk.vhd";
    olnk_storage_ops_t ops;
    olnk_drive_info_t info;
    olnk_generic_ioctl_mid_t mid;
    uint8_t sec_w[OLNK_SECTOR_SIZE * 2U];
    uint8_t sec_r[OLNK_SECTOR_SIZE * 2U];
    uint8_t ms = 0U;
    uint16_t xfer = 0U;
    size_t i;

    CHECK(write_temp_image_file(img_path, 64UL, 0UL, 0UL, "IMG_FLOPPY", 0x10203040UL, false),
          "create temp raw FAT12 .img");
    CHECK(write_temp_image_file(vhd_path, 256UL, 32UL, 192UL, "VHD_DISK_C", 0x50607080UL, true),
          "create temp Fixed-Size .vhd with conectix footer");

    CHECK(olnk_esp32_img_open(&g_img_flp, img_path, OLNK_ESP32_PART_AUTO, false) == OLNK_OK,
          "open raw .img");
    CHECK((!g_img_flp.is_vhd) && (g_img_flp.raw_sectors == 64UL) &&
              (g_img_flp.partition_lba == 0UL) && (g_img_flp.volume_sectors == 64UL),
          "raw .img geometry parsed");

    CHECK(olnk_esp32_img_open(&g_img_vhd, vhd_path, OLNK_ESP32_PART_AUTO, false) == OLNK_OK,
          "open Fixed-Size .vhd");
    CHECK(g_img_vhd.is_vhd && (g_img_vhd.raw_sectors == 256UL) &&
              (g_img_vhd.partition_lba == 32UL) && (g_img_vhd.volume_sectors == 192UL),
          "Fixed-Size .vhd conectix footer stripped & MBR partition LBA 32 detected");

    olnk_esp32_img_drive_set_init(&g_img_set);
    CHECK(olnk_esp32_img_drive_set_attach(&g_img_set, 0U, &g_img_flp), "attach .img as A:");
    CHECK(olnk_esp32_img_drive_set_attach(&g_img_set, 2U, &g_img_vhd), "attach .vhd as C:");
    ops = olnk_esp32_img_drive_set_ops(&g_img_set);

    CHECK(ops.drive_count(ops.user_data) == 3U, "img drive set count == 3");

    /* Write + Verify + Readback on Fixed-VHD partition */
    for (i = 0U; i < sizeof(sec_w); ++i) {
        sec_w[i] = (uint8_t)((i * 41U + 9U) ^ 0x5AU);
    }
    CHECK(ops.write_sectors(2U, 8UL, 2U, sec_w, &xfer, true, ops.user_data) == OLNK_DOS_STATUS_DONE,
          ".vhd WRITE_VERIFY succeeds");
    CHECK(xfer == 2U, ".vhd wrote 2 sectors");

    memset(sec_r, 0, sizeof(sec_r));
    CHECK(ops.read_sectors(2U, 8UL, 2U, sec_r, &xfer, ops.user_data) == OLNK_DOS_STATUS_DONE,
          ".vhd READ_SECTORS succeeds");
    CHECK((xfer == 2U) && (memcmp(sec_w, sec_r, sizeof(sec_w)) == 0), ".vhd readback exact");

    /* Out-of-bounds cannot touch conectix footer */
    CHECK(ops.write_sectors(2U, 192UL, 1U, sec_w, &xfer, false, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND),
          ".vhd OOB write returns 0x8108");

    /* IOCTL 08h/46h Set Media ID persists across close + reopen */
    memset(&mid, 0, sizeof(mid));
    mid.serial_number = 0x99887766UL;
    olnk_label_pad_copy(mid.volume_label, 11U, "VHD_RENAMED");
    olnk_label_pad_copy(mid.filesystem_type, 8U, "FAT16");
    CHECK(ops.set_media_id(2U, &mid, ops.user_data) == OLNK_DOS_STATUS_DONE,
          ".vhd Set Media ID succeeds");

    olnk_esp32_img_close(&g_img_vhd);
    CHECK(olnk_esp32_img_open(&g_img_vhd, vhd_path, OLNK_ESP32_PART_AUTO, true) == OLNK_OK,
          "reopen .vhd read-only");
    CHECK(ops.get_drive_info(2U, &info, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "get_drive_info reopened .vhd");
    CHECK((info.volume_serial == 0x99887766UL) && info.write_protected && g_img_vhd.is_vhd,
          "Set Media ID persisted to .vhd and conectix footer remains intact");
    CHECK(ops.write_sectors(2U, 8UL, 1U, sec_w, &xfer, false, ops.user_data) ==
              OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT),
          "read-only .vhd returns 0x8100 WRITE_PROTECT");

    /* Swap image -> reports MEDIA_CHANGED (0xFF) once, then UNCHANGED (0x01) */
    CHECK(olnk_esp32_img_swap(&g_img_flp, vhd_path, OLNK_ESP32_PART_AUTO, true) == OLNK_OK,
          "img_swap succeeds");
    CHECK(ops.media_check(0U, 0xF0U, &ms, &info, ops.user_data) == OLNK_DOS_STATUS_DONE,
          "media_check after img_swap");
    CHECK((ms == (uint8_t)OLNK_MEDIA_CHANGED) && (info.volume_serial == 0x99887766UL),
          "img_swap reports MEDIA_CHANGED (0xFF)");
    CHECK(ops.media_check(0U, 0xF0U, &ms, &info, ops.user_data) == OLNK_DOS_STATUS_DONE &&
              (ms == (uint8_t)OLNK_MEDIA_UNCHANGED),
          "second media_check after img_swap returns UNCHANGED (0x01)");

    olnk_esp32_img_close(&g_img_flp);
    olnk_esp32_img_close(&g_img_vhd);
    (void)remove(img_path);
    (void)remove(vhd_path);
}

/* =========================================================================
 * Test 4: Full E2E Loopback (olnk_client_t <-> olnk_esp32_server_t)
 * ========================================================================= */

static mock_uart_t g_e2e_cli_hw;
static mock_uart_t g_e2e_srv_hw;
static olnk_esp32_uart_t g_e2e_cli_uart;
static olnk_esp32_uart_t g_e2e_srv_uart;
static sim_sd_card_t g_e2e_sd_card;
static olnk_esp32_sd_drive_t g_e2e_sd_drv;
static olnk_esp32_drive_set_t g_e2e_sd_set;
static olnk_esp32_server_t g_e2e_server;
static olnk_client_t g_e2e_client;

static uint8_t g_tx_20k[40U * OLNK_SECTOR_SIZE]; /* 20 KiB = 3 fragments (8192+8192+4096) */
static uint8_t g_rx_20k[40U * OLNK_SECTOR_SIZE];

static uint64_t g_virt_time_ms = 1000ULL;
static uint32_t g_ev_sync_ok = 0UL;
static uint32_t g_ev_baud_switches = 0UL;
static uint32_t g_ev_disconnects = 0UL;

static uint64_t virt_clock_ms(void *ctx) {
    (void)ctx;
    return g_virt_time_ms;
}

static void e2e_server_event(const char *name, uint32_t a1, uint32_t a2, void *ud) {
    (void)a1;
    (void)a2;
    (void)ud;
    if (strcmp(name, "SYNC_OK") == 0) {
        g_ev_sync_ok += 1UL;
    } else if (strcmp(name, "BAUD_SWITCH") == 0) {
        g_ev_baud_switches += 1UL;
    } else if (strcmp(name, "DISCONNECT") == 0) {
        g_ev_disconnects += 1UL;
    }
}

static void e2e_pump_server(void *ctx) {
    olnk_esp32_server_t *esrv = (olnk_esp32_server_t *)ctx;
    uint32_t guard;
    for (guard = 0UL; guard < 128UL; ++guard) {
        if (mock_uart_rx_count(&g_e2e_srv_hw) == 0U) {
            break;
        }
        (void)olnk_esp32_server_step(esrv, 0UL);
    }
}

static void e2e_idle_tick_server(uint32_t wait_ticks, void *ctx) {
    olnk_esp32_server_t *esrv = (olnk_esp32_server_t *)ctx;
    g_virt_time_ms += (uint64_t)wait_ticks * 10ULL; /* 100 Hz FreeRTOS tick */
    (void)olnk_esp32_server_step(esrv, 0UL);
}

static void test_e2e_client_esp32_server_loopback(void) {
    olnk_esp32_uart_cfg_t cli_ucfg;
    olnk_esp32_uart_cfg_t srv_ucfg;
    olnk_esp32_uart_ops_t cli_uops;
    olnk_esp32_uart_ops_t srv_uops;
    olnk_transport_t cli_tr;
    olnk_transport_t srv_tr;
    olnk_esp32_sd_block_ops_t sd_ops;
    olnk_esp32_sd_opts_t sd_opts;
    olnk_esp32_server_cfg_t srv_cfg;
    olnk_session_sync_resp_t sresp;
    olnk_map_config_resp_t mresp;
    olnk_get_drive_info_resp_t gresp;
    olnk_media_check_resp_t mcresp;
    olnk_build_bpb_resp_t bpbresp;
    olnk_sector_io_resp_t ioresp;
    olnk_generic_ioctl_mid_t mid_in;
    olnk_generic_ioctl_mid_t mid_out;
    uint16_t ioctl_st = 0U;
    size_t i;

    g_virt_time_ms = 1000ULL;
    g_ev_sync_ok = 0UL;
    g_ev_baud_switches = 0UL;
    g_ev_disconnects = 0UL;

    /* Wire two mock ESP32 UARTs back-to-back with strict baud-rate enforcement */
    memset(&g_e2e_cli_hw, 0, sizeof(g_e2e_cli_hw));
    memset(&g_e2e_srv_hw, 0, sizeof(g_e2e_srv_hw));
    g_e2e_cli_hw.peer = &g_e2e_srv_hw;
    g_e2e_cli_hw.on_tx_pump = e2e_pump_server;
    g_e2e_cli_hw.on_idle_wait = e2e_idle_tick_server;
    g_e2e_cli_hw.pump_ctx = &g_e2e_server;
    g_e2e_srv_hw.peer = &g_e2e_cli_hw;

    olnk_esp32_uart_cfg_default(&cli_ucfg, 1, 17, 16);
    olnk_esp32_uart_cfg_default(&srv_ucfg, 2, 4, 5);
    cli_uops = make_mock_uart_ops(&g_e2e_cli_hw, 10UL);
    srv_uops = make_mock_uart_ops(&g_e2e_srv_hw, 10UL);

    CHECK(olnk_esp32_uart_init(&g_e2e_cli_uart, &cli_ucfg, &cli_uops, &cli_tr) == OLNK_OK,
          "E2E client ESP32 UART init");
    CHECK(olnk_esp32_uart_init(&g_e2e_srv_uart, &srv_ucfg, &srv_uops, &srv_tr) == OLNK_OK,
          "E2E server ESP32 UART init");

    /* Prepare mock SD card (MBR + 192-sector FAT16 partition at LBA 63 -> C:) */
    memset(&g_e2e_sd_card, 0, sizeof(g_e2e_sd_card));
    g_e2e_sd_card.total_blocks = SIM_SD_MAX_BLOCKS;
    g_e2e_sd_card.present = true;
    write_mbr_entry(g_e2e_sd_card.blocks[0], 0U, 0x06U, 63UL, 192UL);
    olnk_fat_build_synthetic_vbr(g_e2e_sd_card.blocks[63], 192UL, 0xF8U, "ESP32_SD_C", 0x20261127UL);

    sd_ops = make_sim_sd_ops(&g_e2e_sd_card);
    olnk_esp32_sd_opts_default(&sd_opts);
    CHECK(olnk_esp32_sd_init_custom(&g_e2e_sd_drv, &sd_ops, &sd_opts) == OLNK_OK,
          "E2E SD drive init");
    olnk_esp32_drive_set_init(&g_e2e_sd_set);
    CHECK(olnk_esp32_drive_set_attach(&g_e2e_sd_set, 2U, &g_e2e_sd_drv), "E2E attach C:");

    /* Init ESP32 server & client */
    olnk_esp32_server_cfg_default(&srv_cfg);
    srv_cfg.max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    srv_cfg.local_requires_echo = true;
    srv_cfg.idle_disconnect_ms = 3000UL;
    srv_cfg.on_event = e2e_server_event;
    srv_cfg.get_time_ms = virt_clock_ms;

    CHECK(olnk_esp32_server_init(
              &g_e2e_server,
              srv_tr,
              olnk_esp32_drive_set_ops(&g_e2e_sd_set),
              &srv_cfg) == OLNK_OK,
          "E2E esp32_server_init");
    CHECK(olnk_client_init(&g_e2e_client, cli_tr) == OLNK_OK, "E2E client_init");
    g_e2e_client.link.max_baud_idx = (uint8_t)OLNK_BAUD_115200;

    /* 1. Phase A Sync -> Phase B 9600..115200 baud step-up */
    CHECK(olnk_client_connect(&g_e2e_client, 3U) == OLNK_OK, "E2E Phase A+B connect");
    CHECK((g_e2e_cli_hw.baudrate == 115200UL) && (g_e2e_srv_hw.baudrate == 115200UL),
          "both ESP32 UARTs stepped up to 115200 bps");
    CHECK((!g_e2e_cli_hw.baud_switch_order_violated) &&
              (!g_e2e_srv_hw.baud_switch_order_violated),
          "wait_tx_done strictly preceded every set_baudrate on both peers");
    CHECK((g_ev_sync_ok == 1UL) && (g_ev_baud_switches == 4UL),
          "server logged SYNC_OK + 4 stepwise BAUD_SWITCH events");

    /* 2. SESSION_SYNC + MAP_CONFIG + GET_DRIVE_INFO */
    CHECK(olnk_client_session_sync(&g_e2e_client, false, &sresp) == OLNK_OK, "E2E SESSION_SYNC");
    CHECK((sresp.ready_flag == 1U) && (sresp.max_block_size == 8192U), "max_block_size == 8192");

    CHECK(olnk_client_map_config(&g_e2e_client, 4U, 3U, &mresp) == OLNK_OK, "E2E MAP_CONFIG");
    CHECK((mresp.server_drive_count == 3U) && (mresp.drive_map_resp[0] == 2U) &&
              (mresp.server_drive_attrs[2] == OLNK_DRIVE_ATTR_DEFAULT),
          "MAP_CONFIG mapped server C: (unit 2) with 0x08C2 attrs");

    CHECK(olnk_client_get_drive_info(&g_e2e_client, 2U, &gresp) == OLNK_OK, "E2E GET_DRIVE_INFO");
    CHECK(strcmp(gresp.volume_label, "ESP32_SD_C") == 0, "GET_DRIVE_INFO label matches");

    /* 3. MEDIA_CHECK + BUILD_BPB */
    CHECK(olnk_client_media_check(&g_e2e_client, 2U, 0xF8U, &mcresp) == OLNK_OK, "E2E MEDIA_CHECK");
    CHECK((mcresp.status == OLNK_DOS_STATUS_DONE) &&
              (mcresp.media_status == (uint8_t)OLNK_MEDIA_UNCHANGED) &&
              (mcresp.volume_serial == 0x20261127UL),
          "initial MEDIA_CHECK returns UNCHANGED (0x01)");

    CHECK(olnk_client_build_bpb(&g_e2e_client, 2U, 0xF8U, &bpbresp) == OLNK_OK, "E2E BUILD_BPB");
    CHECK(bpbresp.has_bpb && (bpbresp.bpb.bytes_per_sector == 512U) &&
              (bpbresp.bpb.total_sectors_16 == 192U),
          "BUILD_BPB returned 192-sector FAT16 BPB");

    /* 4. 20 KiB (40 sectors = 3 fragments) WRITE_VERIFY + READ_SECTORS */
    for (i = 0U; i < sizeof(g_tx_20k); ++i) {
        g_tx_20k[i] = (uint8_t)((i * 53U + 17U) ^ 0x3CU);
    }
    CHECK(olnk_client_write_sectors(
              &g_e2e_client,
              2U,
              0xF8U,
              16UL,
              40U,
              OLNK_SECTOR_SIZE,
              g_tx_20k,
              true,
              &ioresp) == OLNK_OK,
          "E2E 20 KiB multi-fragment WRITE_VERIFY");
    CHECK((ioresp.status == OLNK_DOS_STATUS_DONE) && (ioresp.sectors_transferred == 40U),
          "all 40 sectors written & verified");

    memset(g_rx_20k, 0, sizeof(g_rx_20k));
    CHECK(olnk_client_read_sectors(
              &g_e2e_client,
              2U,
              0xF8U,
              16UL,
              40U,
              OLNK_SECTOR_SIZE,
              g_rx_20k,
              (uint32_t)sizeof(g_rx_20k),
              &ioresp) == OLNK_OK,
          "E2E 20 KiB multi-fragment READ_SECTORS");
    CHECK((ioresp.status == OLNK_DOS_STATUS_DONE) && (ioresp.sectors_transferred == 40U) &&
              (memcmp(g_tx_20k, g_rx_20k, sizeof(g_tx_20k)) == 0),
          "20 KiB WRITE_VERIFY -> READ_SECTORS bit-for-bit identical");

    /* 5. Generic IOCTL 08h/46h Set Media ID + 08h/66h Get Media ID */
    memset(&mid_in, 0, sizeof(mid_in));
    mid_in.serial_number = 0x13572468UL;
    olnk_label_pad_copy(mid_in.volume_label, 11U, "ESP32_LIVE");
    olnk_label_pad_copy(mid_in.filesystem_type, 8U, "FAT16");
    CHECK(olnk_client_set_media_id(&g_e2e_client, 2U, &mid_in, &ioctl_st) == OLNK_OK,
          "E2E IOCTL 08h/46h Set Media ID");
    CHECK(ioctl_st == OLNK_DOS_STATUS_DONE, "Set Media ID status DONE");

    CHECK(olnk_client_get_media_id(&g_e2e_client, 2U, &ioctl_st, &mid_out) == OLNK_OK,
          "E2E IOCTL 08h/66h Get Media ID");
    CHECK((ioctl_st == OLNK_DOS_STATUS_DONE) && (mid_out.serial_number == 0x13572468UL) &&
              (memcmp(mid_out.volume_label, "ESP32_LIVE ", 11U) == 0),
          "Get Media ID reflects updated serial & label");

    /* 6. Hot-swap SD card over live link */
    g_e2e_sd_card.present = false;
    CHECK(olnk_client_media_check(&g_e2e_client, 2U, 0xF8U, &mcresp) == OLNK_OK,
          "E2E MEDIA_CHECK while card removed");
    CHECK(mcresp.status == OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY),
          "removed SD card returns 0x8102 over wire");

    olnk_fat_build_synthetic_vbr(g_e2e_sd_card.blocks[63], 192UL, 0xF8U, "CARD_TWO", 0xFACEB00CUL);
    g_e2e_sd_card.present = true;
    CHECK(olnk_client_media_check(&g_e2e_client, 2U, 0xF8U, &mcresp) == OLNK_OK,
          "E2E MEDIA_CHECK after hot-swap");
    CHECK((mcresp.status == OLNK_DOS_STATUS_DONE) &&
              (mcresp.media_status == (uint8_t)OLNK_MEDIA_CHANGED) &&
              (mcresp.volume_serial == 0xFACEB00CUL),
          "hot-swap returns MEDIA_CHANGED (0xFF) with new serial over wire");

    CHECK(olnk_client_media_check(&g_e2e_client, 2U, 0xF8U, &mcresp) == OLNK_OK &&
              (mcresp.media_status == (uint8_t)OLNK_MEDIA_UNCHANGED),
          "next MEDIA_CHECK clears back to UNCHANGED (0x01)");

    /* 7. Keepalives */
    CHECK(olnk_client_keepalive(&g_e2e_client) == OLNK_OK, "E2E keepalive #1");
    CHECK(olnk_client_keepalive(&g_e2e_client) == OLNK_OK, "E2E keepalive #2");
    CHECK(olnk_client_keepalive(&g_e2e_client) == OLNK_OK, "E2E keepalive #3");
    CHECK(g_e2e_server.server.link.keepalives_ok >= 3UL, "server echoed all keepalives");

    /* 8. Monotonic idle_disconnect_ms drop -> transparent session resumption */
    g_virt_time_ms += 1500ULL;
    CHECK(olnk_esp32_server_step(&g_e2e_server, 0UL) == OLNK_OK, "idle step 1500 ms");
    CHECK(g_e2e_srv_hw.baudrate == 115200UL, "still connected at 1500 ms (< 3000 ms)");

    g_virt_time_ms += 1600ULL;
    CHECK(olnk_esp32_server_step(&g_e2e_server, 0UL) == OLNK_OK, "idle step 3100 ms");
    CHECK((g_e2e_srv_hw.baudrate == 9600UL) && (g_ev_disconnects == 1UL),
          "monotonic idle_disconnect_ms dropped link back to 9600 bps");

    olnk_client_disconnect(&g_e2e_client);
    CHECK(olnk_client_connect(&g_e2e_client, 3U) == OLNK_OK, "E2E reconnect after idle drop");
    CHECK(olnk_client_session_sync(&g_e2e_client, false, &sresp) == OLNK_OK, "E2E resume SESSION_SYNC");
    CHECK(g_e2e_client.link.session_resumed && g_e2e_server.server.link.session_resumed,
          "transparent session resumption succeeded on both sides");

    CHECK(olnk_esp32_uart_deinit(&g_e2e_cli_uart) == OLNK_OK, "E2E client UART deinit");
    CHECK(olnk_esp32_uart_deinit(&g_e2e_srv_uart) == OLNK_OK, "E2E server UART deinit");
}

/* =========================================================================
 * Test 5 & 6: Thread-Safe ESP32 Client Wrapper & FatFs DiskIO/VFS Bridge
 * ========================================================================= */

typedef struct {
    uint32_t lock_count;
    uint32_t unlock_count;
    uint32_t depth;
    uint32_t max_depth;
    bool double_lock_err;
    bool unlock_underflow_err;
} spy_mutex_t;

typedef struct {
    olnk_transport_t inner;
    const spy_mutex_t *mtx;
    uint32_t io_calls_total;
    uint32_t io_calls_unlocked;
} spy_transport_ctx_t;

static bool spy_mutex_lock(void *ctx) {
    spy_mutex_t *m = (spy_mutex_t *)ctx;
    m->lock_count += 1UL;
    m->depth += 1UL;
    if (m->depth > m->max_depth) {
        m->max_depth = m->depth;
    }
    if (m->depth > 1UL) {
        m->double_lock_err = true;
    }
    return true;
}

static void spy_mutex_unlock(void *ctx) {
    spy_mutex_t *m = (spy_mutex_t *)ctx;
    m->unlock_count += 1UL;
    if (m->depth == 0UL) {
        m->unlock_underflow_err = true;
    } else {
        m->depth -= 1UL;
    }
}

static int spy_tr_read(uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    spy_transport_ctx_t *sc = (spy_transport_ctx_t *)ud;
    sc->io_calls_total += 1UL;
    if (sc->mtx->depth != 1UL) {
        sc->io_calls_unlocked += 1UL;
    }
    return sc->inner.read(buf, len, timeout_ms, sc->inner.user_data);
}

static int spy_tr_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    spy_transport_ctx_t *sc = (spy_transport_ctx_t *)ud;
    sc->io_calls_total += 1UL;
    if (sc->mtx->depth != 1UL) {
        sc->io_calls_unlocked += 1UL;
    }
    return sc->inner.write(buf, len, timeout_ms, sc->inner.user_data);
}

static bool spy_tr_set_baud(uint32_t baudrate, void *ud) {
    spy_transport_ctx_t *sc = (spy_transport_ctx_t *)ud;
    sc->io_calls_total += 1UL;
    if (sc->mtx->depth != 1UL) {
        sc->io_calls_unlocked += 1UL;
    }
    return sc->inner.set_baud(baudrate, sc->inner.user_data);
}

/* Streaming backup sink for olnk_esp32_client_dump_sectors() */
typedef struct {
    uint8_t buf[192U * OLNK_SECTOR_SIZE];
    uint32_t bytes_written;
    uint32_t call_count;
    uint32_t expected_next_lba;
    bool lba_order_ok;
    uint32_t abort_after_calls; /* 0 = never abort */
} dump_sink_t;

static bool dump_sink_cb(uint32_t lba, const uint8_t *data, uint32_t byte_len, void *ctx) {
    dump_sink_t *s = (dump_sink_t *)ctx;
    if (lba != s->expected_next_lba) {
        s->lba_order_ok = false;
    }
    if ((s->bytes_written + byte_len) <= (uint32_t)sizeof(s->buf)) {
        memcpy(&s->buf[s->bytes_written], data, (size_t)byte_len);
        s->bytes_written += byte_len;
    } else {
        return false;
    }
    s->expected_next_lba += (byte_len / OLNK_SECTOR_SIZE);
    s->call_count += 1UL;
    if ((s->abort_after_calls > 0UL) && (s->call_count >= s->abort_after_calls)) {
        return false;
    }
    return true;
}

/* Bad-geometry storage mock for testing non-512B and 0-sector VFS rejection */
typedef struct {
    uint16_t bps;
    uint32_t total_sectors;
} bad_geom_ctx_t;

static uint8_t bad_geom_drive_count(void *ud) {
    (void)ud;
    return 1U;
}

static uint16_t bad_geom_get_info(uint8_t unit, olnk_drive_info_t *out, void *ud) {
    const bad_geom_ctx_t *g = (const bad_geom_ctx_t *)ud;
    if (unit != 0U) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    memset(out, 0, sizeof(*out));
    out->present = true;
    out->ready = true;
    out->drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
    out->bpb.bytes_per_sector = g->bps;
    out->bpb.sectors_per_cluster = 1U;
    out->bpb.media_descriptor = 0xF8U;
    out->bpb.total_sectors_32 = g->total_sectors;
    out->total_sectors = g->total_sectors;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t bad_geom_media_check(
    uint8_t unit,
    uint8_t md,
    uint8_t *out_ms,
    olnk_drive_info_t *out_info,
    void *ud
) {
    (void)md;
    if (unit != 0U) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    *out_ms = (uint8_t)OLNK_MEDIA_UNCHANGED;
    return bad_geom_get_info(unit, out_info, ud);
}

static uint16_t bad_geom_build_bpb(uint8_t unit, uint8_t md, olnk_dos_bpb_t *out_bpb, void *ud) {
    olnk_drive_info_t info;
    uint16_t st = bad_geom_get_info(unit, &info, ud);
    (void)md;
    if ((st & OLNK_DOS_STATUS_ERROR) == 0U) {
        *out_bpb = info.bpb;
    }
    return st;
}

static olnk_esp32_client_t g_esp32_client;
static olnk_esp32_vfs_mount_t g_vfs_mnt;
static sim_sd_card_t g_cli_sd_bad_floppy;
static olnk_esp32_sd_drive_t g_cli_sd_drv_a;
static dump_sink_t g_dump_sink;
static spy_mutex_t g_spy_mtx;
static spy_transport_ctx_t g_spy_tr_ctx;
static uint64_t g_cli_virt_time_ms = 5000ULL;

static uint64_t cli_virt_clock_ms(void *ctx) {
    (void)ctx;
    return g_cli_virt_time_ms;
}

static void test_esp32_client_and_vfs_loopback(void) {
    olnk_esp32_uart_cfg_t cli_ucfg;
    olnk_esp32_uart_cfg_t srv_ucfg;
    olnk_esp32_uart_ops_t cli_uops;
    olnk_esp32_uart_ops_t srv_uops;
    olnk_transport_t raw_cli_tr;
    olnk_transport_t spy_cli_tr;
    olnk_transport_t srv_tr;
    olnk_esp32_sd_block_ops_t sd_ops_c;
    olnk_esp32_sd_block_ops_t sd_ops_a;
    olnk_esp32_sd_opts_t sd_opts;
    olnk_esp32_server_cfg_t srv_cfg;
    olnk_esp32_client_cfg_t cli_cfg;
    olnk_esp32_vfs_cfg_t vfs_cfg;
    olnk_get_drive_info_resp_t gresp;
    olnk_media_check_resp_t mcresp;
    olnk_build_bpb_resp_t bpbresp;
    olnk_sector_io_resp_t ioresp;
    olnk_generic_ioctl_mid_t mid_in;
    olnk_generic_ioctl_mid_t mid_out;
    uint16_t ioctl_st = 0U;
    uint8_t sel_unit = 0xFFU;
    uint32_t dumped = 0UL;
    uint32_t sec_count = 0UL;
    uint16_t sec_size = 0U;
    uint32_t blk_size = 0UL;
    uint8_t single_w[OLNK_SECTOR_SIZE];
    uint8_t single_r[OLNK_SECTOR_SIZE];
    uint32_t srv_rpcs_before;
    uint32_t ka_before;
    size_t i;

    g_virt_time_ms = 1000ULL;
    g_cli_virt_time_ms = 5000ULL;
    g_ev_sync_ok = 0UL;
    g_ev_baud_switches = 0UL;
    g_ev_disconnects = 0UL;
    memset(&g_spy_mtx, 0, sizeof(g_spy_mtx));
    memset(&g_spy_tr_ctx, 0, sizeof(g_spy_tr_ctx));

    /* 1. Validate client config defaults and invalid argument rejection */
    olnk_esp32_client_cfg_default(&cli_cfg);
    CHECK((cli_cfg.max_baud_idx == (uint8_t)OLNK_BAUD_115200) &&
              cli_cfg.local_requires_echo &&
              (!cli_cfg.force_tx_echo) &&
              (cli_cfg.sync_retries == OLNK_ESP32_CLIENT_DEFAULT_SYNC_RETRIES) &&
              (cli_cfg.rpc_timeout_ms == OLNK_CLIENT_DEFAULT_RESP_MS) &&
              (cli_cfg.max_client_drives == OLNK_ESP32_CLIENT_DEFAULT_MAX_DRIVES) &&
              (cli_cfg.first_client_drive == OLNK_ESP32_CLIENT_DEFAULT_FIRST_DRIVE) &&
              cli_cfg.auto_reconnect &&
              (cli_cfg.keepalive_interval_ms == OLNK_ESP32_CLIENT_DEFAULT_KEEPALIVE_MS),
          "client_cfg_default populates expected defaults");

    memset(&raw_cli_tr, 0, sizeof(raw_cli_tr));
    CHECK(olnk_esp32_client_init(NULL, raw_cli_tr, &cli_cfg) == OLNK_ERR_INVALID_ARG,
          "client_init rejects NULL ecli");
    cli_cfg.max_baud_idx = OLNK_BAUD_INDEX_COUNT;
    CHECK(olnk_esp32_client_init(&g_esp32_client, raw_cli_tr, &cli_cfg) == OLNK_ERR_INVALID_ARG,
          "client_init rejects invalid max_baud_idx");
    olnk_esp32_client_cfg_default(&cli_cfg);
    cli_cfg.lock = spy_mutex_lock;
    cli_cfg.unlock = NULL;
    CHECK(olnk_esp32_client_init(&g_esp32_client, raw_cli_tr, &cli_cfg) == OLNK_ERR_INVALID_ARG,
          "client_init rejects asymmetric lock/unlock callbacks");
    olnk_esp32_client_cfg_default(&cli_cfg);
    cli_cfg.first_client_drive = OLNK_MAX_DRIVES;
    CHECK(olnk_esp32_client_init(&g_esp32_client, raw_cli_tr, &cli_cfg) == OLNK_ERR_INVALID_ARG,
          "client_init rejects out-of-range first_client_drive");
    olnk_esp32_client_cfg_default(&cli_cfg);

    /* 2. Wire dual mock ESP32 UARTs + spy mutex transport wrapper */
    memset(&g_e2e_cli_hw, 0, sizeof(g_e2e_cli_hw));
    memset(&g_e2e_srv_hw, 0, sizeof(g_e2e_srv_hw));
    g_e2e_cli_hw.peer = &g_e2e_srv_hw;
    g_e2e_cli_hw.on_tx_pump = e2e_pump_server;
    g_e2e_cli_hw.on_idle_wait = e2e_idle_tick_server;
    g_e2e_cli_hw.pump_ctx = &g_e2e_server;
    g_e2e_srv_hw.peer = &g_e2e_cli_hw;

    olnk_esp32_uart_cfg_default(&cli_ucfg, 1, 17, 16);
    olnk_esp32_uart_cfg_default(&srv_ucfg, 2, 4, 5);
    cli_uops = make_mock_uart_ops(&g_e2e_cli_hw, 10UL);
    srv_uops = make_mock_uart_ops(&g_e2e_srv_hw, 10UL);

    CHECK(olnk_esp32_uart_init(&g_e2e_cli_uart, &cli_ucfg, &cli_uops, &raw_cli_tr) == OLNK_OK,
          "Client loopback UART init");
    CHECK(olnk_esp32_uart_init(&g_e2e_srv_uart, &srv_ucfg, &srv_uops, &srv_tr) == OLNK_OK,
          "Server loopback UART init");

    g_spy_tr_ctx.inner = raw_cli_tr;
    g_spy_tr_ctx.mtx = &g_spy_mtx;
    spy_cli_tr.read = spy_tr_read;
    spy_cli_tr.write = spy_tr_write;
    spy_cli_tr.set_baud = spy_tr_set_baud;
    spy_cli_tr.user_data = &g_spy_tr_ctx;

    /* Prepare server drives: A: (unformatted/unready) and C: (192-sector MBR FAT16 at LBA 63) */
    memset(&g_cli_sd_bad_floppy, 0, sizeof(g_cli_sd_bad_floppy));
    g_cli_sd_bad_floppy.total_blocks = 64UL;
    g_cli_sd_bad_floppy.present = true;
    /* Leave sector 0 zeroed so A: is unformatted -> BUILD_BPB returns 0x8107 */
    sd_ops_a = make_sim_sd_ops(&g_cli_sd_bad_floppy);
    olnk_esp32_sd_opts_default(&sd_opts);
    CHECK(olnk_esp32_sd_init_custom(&g_cli_sd_drv_a, &sd_ops_a, &sd_opts) == OLNK_OK,
          "Attach unformatted A: drive for select_default_unit skip test");

    memset(&g_e2e_sd_card, 0, sizeof(g_e2e_sd_card));
    g_e2e_sd_card.total_blocks = SIM_SD_MAX_BLOCKS;
    g_e2e_sd_card.present = true;
    write_mbr_entry(g_e2e_sd_card.blocks[0], 0U, 0x06U, 63UL, 192UL);
    olnk_fat_build_synthetic_vbr(g_e2e_sd_card.blocks[63], 192UL, 0xF8U, "DOS_REMOTE", 0xCAFE1234UL);
    /* Populate deterministic pattern across partition sectors 1..191 for dump verification */
    for (i = 1U; i < 192U; ++i) {
        size_t b;
        for (b = 0U; b < OLNK_SECTOR_SIZE; ++b) {
            g_e2e_sd_card.blocks[63U + i][b] = (uint8_t)(((i * 31U) + b) & 0xFFU);
        }
    }
    sd_ops_c = make_sim_sd_ops(&g_e2e_sd_card);
    CHECK(olnk_esp32_sd_init_custom(&g_e2e_sd_drv, &sd_ops_c, &sd_opts) == OLNK_OK,
          "Init valid 192-sector FAT16 C: drive");

    olnk_esp32_drive_set_init(&g_e2e_sd_set);
    CHECK(olnk_esp32_drive_set_attach(&g_e2e_sd_set, 0U, &g_cli_sd_drv_a), "Server attach A:");
    CHECK(olnk_esp32_drive_set_attach(&g_e2e_sd_set, 2U, &g_e2e_sd_drv), "Server attach C:");

    olnk_esp32_server_cfg_default(&srv_cfg);
    srv_cfg.max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    srv_cfg.local_requires_echo = true;
    srv_cfg.idle_disconnect_ms = 3000UL;
    srv_cfg.on_event = e2e_server_event;
    srv_cfg.get_time_ms = virt_clock_ms;

    CHECK(olnk_esp32_server_init(
              &g_e2e_server,
              srv_tr,
              olnk_esp32_drive_set_ops(&g_e2e_sd_set),
              &srv_cfg) == OLNK_OK,
          "Init ESP32 server for client loopback");

    /* Init ESP32 client with spy mutex, force_tx_echo, and virtual clock */
    olnk_esp32_client_cfg_default(&cli_cfg);
    cli_cfg.max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    cli_cfg.local_requires_echo = true;
    cli_cfg.force_tx_echo = true;
    cli_cfg.sync_retries = OLNK_ESP32_CLIENT_DEFAULT_SYNC_RETRIES;
    cli_cfg.auto_reconnect = true;
    cli_cfg.keepalive_interval_ms = 200UL;
    cli_cfg.lock = spy_mutex_lock;
    cli_cfg.unlock = spy_mutex_unlock;
    cli_cfg.lock_ctx = &g_spy_mtx;
    cli_cfg.get_time_ms = cli_virt_clock_ms;

    CHECK(olnk_esp32_client_init(&g_esp32_client, spy_cli_tr, &cli_cfg) == OLNK_OK,
          "olnk_esp32_client_init succeeds");
    CHECK(olnk_esp32_client_start_keepalive_task(&g_esp32_client, "ka", 3072UL, 4UL, -1) ==
              OLNK_ERR_PROTOCOL,
          "start_keepalive_task returns OLNK_ERR_PROTOCOL on non-ESP host");

    /* 3. Connect session (9600 -> 115200, SESSION_SYNC, MAP_CONFIG) */
    CHECK(olnk_esp32_client_connect_session(&g_esp32_client) == OLNK_OK,
          "esp32_client_connect_session succeeds");
    CHECK((g_e2e_cli_hw.baudrate == 115200UL) && (g_e2e_srv_hw.baudrate == 115200UL),
          "ESP32 client <-> server negotiated 115200 bps");
    CHECK(g_esp32_client.client.link.peer_echo_forced,
          "force_tx_echo applied cli.link.peer_echo_forced after connect");
    CHECK(g_esp32_client.has_sync && g_esp32_client.client.has_map &&
              (!g_esp32_client.client.link.session_resumed),
          "initial connect cached sync + drive map (fresh session)");

    /* 4. select_default_unit skips unformatted A: and picks ready C: (unit 2) */
    CHECK(olnk_esp32_client_select_default_unit(&g_esp32_client, &sel_unit) == OLNK_OK,
          "select_default_unit succeeds");
    CHECK(sel_unit == 2U, "select_default_unit skipped unready A: and selected C: (unit 2)");
    CHECK(!olnk_esp32_client_unit_is_readonly(&g_esp32_client, 2U),
          "unit 2 is initially read-write in MAP_CONFIG");

    /* 5. Thread-safe RPC wrappers: get_drive_info, media_check, build_bpb, media ID */
    CHECK(olnk_esp32_client_get_drive_info(&g_esp32_client, 2U, &gresp) == OLNK_OK,
          "client_get_drive_info ok");
    CHECK(strcmp(gresp.volume_label, "DOS_REMOTE") == 0, "client_get_drive_info label matches");

    CHECK(olnk_esp32_client_media_check(&g_esp32_client, 2U, 0xF8U, &mcresp) == OLNK_OK,
          "client_media_check ok");
    CHECK((mcresp.status == OLNK_DOS_STATUS_DONE) &&
              (mcresp.media_status == (uint8_t)OLNK_MEDIA_UNCHANGED) &&
              (mcresp.volume_serial == 0xCAFE1234UL),
          "client_media_check reports UNCHANGED + expected serial");

    CHECK(olnk_esp32_client_build_bpb(&g_esp32_client, 2U, 0xF8U, &bpbresp) == OLNK_OK,
          "client_build_bpb ok");
    CHECK(bpbresp.has_bpb && (bpbresp.bpb.bytes_per_sector == 512U) &&
              (bpbresp.bpb.total_sectors_16 == 192U),
          "client_build_bpb returned 192-sector BPB");

    memset(&mid_in, 0, sizeof(mid_in));
    mid_in.serial_number = 0x76543210UL;
    olnk_label_pad_copy(mid_in.volume_label, 11U, "ESP_CLIENT");
    olnk_label_pad_copy(mid_in.filesystem_type, 8U, "FAT16");
    CHECK(olnk_esp32_client_set_media_id(&g_esp32_client, 2U, &mid_in, &ioctl_st) == OLNK_OK,
          "client_set_media_id ok");
    CHECK(ioctl_st == OLNK_DOS_STATUS_DONE, "client_set_media_id status DONE");

    CHECK(olnk_esp32_client_get_media_id(&g_esp32_client, 2U, &ioctl_st, &mid_out) == OLNK_OK,
          "client_get_media_id ok");
    CHECK((ioctl_st == OLNK_DOS_STATUS_DONE) &&
              (mid_out.serial_number == 0x76543210UL) &&
              (memcmp(mid_out.volume_label, "ESP_CLIENT ", 11U) == 0),
          "client_get_media_id matches updated serial & label");

    /* 6. Multi-fragment 20 KiB (>8192 B) write_sectors (verify=true) & read_sectors */
    for (i = 0U; i < sizeof(g_tx_20k); ++i) {
        g_tx_20k[i] = (uint8_t)((i * 67U + 11U) ^ 0xA5U);
    }
    CHECK(olnk_esp32_client_write_sectors(
              &g_esp32_client,
              2U,
              0xF8U,
              20UL,
              40U,
              OLNK_SECTOR_SIZE,
              g_tx_20k,
              true,
              &ioresp) == OLNK_OK,
          "client_write_sectors 20 KiB WRITE_VERIFY ok");
    CHECK((ioresp.status == OLNK_DOS_STATUS_DONE) && (ioresp.sectors_transferred == 40U),
          "client_write_sectors wrote all 40 sectors");

    memset(g_rx_20k, 0, sizeof(g_rx_20k));
    CHECK(olnk_esp32_client_read_sectors(
              &g_esp32_client,
              2U,
              0xF8U,
              20UL,
              40U,
              OLNK_SECTOR_SIZE,
              g_rx_20k,
              (uint32_t)sizeof(g_rx_20k),
              &ioresp) == OLNK_OK,
          "client_read_sectors 20 KiB ok");
    CHECK((ioresp.status == OLNK_DOS_STATUS_DONE) && (ioresp.sectors_transferred == 40U) &&
              (memcmp(g_tx_20k, g_rx_20k, sizeof(g_tx_20k)) == 0),
          "client 20 KiB write->read bit-for-bit identical");

    /* 7. Streaming raw drive backup helper: olnk_esp32_client_dump_sectors() */
    memset(&g_dump_sink, 0, sizeof(g_dump_sink));
    g_dump_sink.expected_next_lba = 0UL;
    g_dump_sink.lba_order_ok = true;
    CHECK(olnk_esp32_client_dump_sectors(
              &g_esp32_client,
              2U,
              0UL,
              0UL, /* 0 -> dump entire 192-sector partition */
              dump_sink_cb,
              &g_dump_sink,
              &dumped) == OLNK_OK,
          "dump_sectors full 192-sector backup ok");
    CHECK((dumped == 192UL) &&
              (g_dump_sink.bytes_written == (192UL * OLNK_SECTOR_SIZE)) &&
              (g_dump_sink.call_count == 12UL) &&
              g_dump_sink.lba_order_ok &&
              (memcmp(g_dump_sink.buf, g_e2e_sd_card.blocks[63], 192U * OLNK_SECTOR_SIZE) == 0),
          "dump_sectors streamed 192 sectors in 12x8KiB chunks byte-for-byte identical to SD");

    /* Verify early callback abort stops cleanly and reports exact sectors dumped */
    memset(&g_dump_sink, 0, sizeof(g_dump_sink));
    g_dump_sink.expected_next_lba = 10UL;
    g_dump_sink.lba_order_ok = true;
    g_dump_sink.abort_after_calls = 2UL;
    CHECK(olnk_esp32_client_dump_sectors(
              &g_esp32_client,
              2U,
              10UL,
              64UL,
              dump_sink_cb,
              &g_dump_sink,
              &dumped) == OLNK_ERR_HAL_IO,
          "dump_sectors aborts cleanly when write_cb returns false");
    CHECK((dumped == 32UL) && (g_dump_sink.call_count == 2UL) && g_dump_sink.lba_order_ok,
          "dump_sectors reports exact 32 sectors dumped before abort");

    /* 8. Idle-aware keepalive_step & direct keepalive */
    ka_before = g_esp32_client.client.link.keepalives_ok;
    g_cli_virt_time_ms += 50ULL; /* < 200 ms interval */
    CHECK(olnk_esp32_client_keepalive_step(&g_esp32_client) == OLNK_OK,
          "keepalive_step within interval ok");
    CHECK(g_esp32_client.client.link.keepalives_ok == ka_before,
          "keepalive_step skipped ping while idle < keepalive_interval_ms");

    g_cli_virt_time_ms += 200ULL; /* >= 200 ms interval */
    CHECK(olnk_esp32_client_keepalive_step(&g_esp32_client) == OLNK_OK,
          "keepalive_step past interval ok");
    CHECK(g_esp32_client.client.link.keepalives_ok == (ka_before + 1UL),
          "keepalive_step sent keepalive once idle >= keepalive_interval_ms");

    CHECK(olnk_esp32_client_keepalive(&g_esp32_client) == OLNK_OK,
          "direct client_keepalive ok");
    CHECK(g_esp32_client.client.link.keepalives_ok == (ka_before + 2UL),
          "direct client_keepalive incremented keepalives_ok");

    /* 9. Auto-reconnect & transparent session resumption after server link drop */
    olnk_server_drop_link(&g_e2e_server.server);
    CHECK(g_e2e_srv_hw.baudrate == 9600UL, "server link dropped back to 9600 bps");
    CHECK(!g_esp32_client.client.link.session_resumed, "not yet resumed before drop recovery");

    memset(single_r, 0, sizeof(single_r));
    CHECK(olnk_esp32_client_read_sectors(
              &g_esp32_client,
              2U,
              0xF8U,
              20UL,
              1U,
              OLNK_SECTOR_SIZE,
              single_r,
              (uint32_t)sizeof(single_r),
              &ioresp) == OLNK_OK,
          "read_sectors transparently auto-reconnected after olnk_server_drop_link");
    CHECK((ioresp.status == OLNK_DOS_STATUS_DONE) &&
              (ioresp.sectors_transferred == 1U) &&
              (memcmp(single_r, g_tx_20k, OLNK_SECTOR_SIZE) == 0),
          "auto-reconnected read_sectors returned byte-exact sector data");
    CHECK(g_esp32_client.client.link.session_resumed &&
              g_e2e_server.server.link.session_resumed &&
              (g_esp32_client.reconnect_count == 1UL) &&
              (g_e2e_cli_hw.baudrate == 115200UL) &&
              (g_e2e_srv_hw.baudrate == 115200UL),
          "transparent session resumption succeeded (session_resumed=true, 115200 bps)");

    /* Fresh server restart (different server_id) re-runs MAP_CONFIG cleanly */
    olnk_server_drop_link(&g_e2e_server.server);
    g_e2e_server.server.link.server_id = 0x55443322UL;
    CHECK(olnk_esp32_client_media_check(&g_esp32_client, 2U, 0xF8U, &mcresp) == OLNK_OK,
          "auto-reconnect against restarted server (new server_id) succeeds");
    CHECK((!g_esp32_client.client.link.session_resumed) &&
              (g_esp32_client.client.link.server_id == 0x55443322UL) &&
              g_esp32_client.client.has_map &&
              (g_esp32_client.reconnect_count == 2UL),
          "fresh server_id cleared session_resumed and refreshed MAP_CONFIG");

    /* Verify auto_reconnect=false fails cleanly when disconnected */
    g_esp32_client.cfg.auto_reconnect = false;
    CHECK(olnk_esp32_client_disconnect(&g_esp32_client) == OLNK_OK, "explicit client_disconnect ok");
    CHECK(olnk_esp32_client_media_check(&g_esp32_client, 2U, 0xF8U, &mcresp) ==
              OLNK_ERR_DISCONNECTED,
          "RPC fails with OLNK_ERR_DISCONNECTED when auto_reconnect=false");
    g_esp32_client.cfg.auto_reconnect = true;
    CHECK(olnk_esp32_client_connect_session(&g_esp32_client) == OLNK_OK,
          "reconnect before VFS tests ok");

    /* =====================================================================
     * Test 6: FatFs DiskIO Core & VFS Bridge (esp32_client_vfs.c)
     * ===================================================================== */

    /* 6a. Argument validation & host-only vfs_mount guard */
    olnk_esp32_vfs_cfg_default(&vfs_cfg, "/dos", OLNK_ESP32_UNIT_AUTO);
    CHECK(olnk_esp32_vfs_mount(&g_vfs_mnt, &g_esp32_client, &vfs_cfg) == OLNK_ERR_PROTOCOL,
          "vfs_mount returns OLNK_ERR_PROTOCOL on non-ESP host build");
    CHECK(olnk_esp32_vfs_disk_attach(NULL, &g_esp32_client, &vfs_cfg) == OLNK_ERR_INVALID_ARG,
          "vfs_disk_attach rejects NULL mnt");
    CHECK(olnk_esp32_vfs_disk_attach(&g_vfs_mnt, NULL, &vfs_cfg) == OLNK_ERR_INVALID_ARG,
          "vfs_disk_attach rejects NULL client");
    vfs_cfg.base_path = "dos_no_slash";
    CHECK(olnk_esp32_vfs_disk_attach(&g_vfs_mnt, &g_esp32_client, &vfs_cfg) == OLNK_ERR_INVALID_ARG,
          "vfs_disk_attach rejects base_path without leading '/'");
    olnk_esp32_vfs_cfg_default(&vfs_cfg, "/dos", 26U);
    CHECK(olnk_esp32_vfs_disk_attach(&g_vfs_mnt, &g_esp32_client, &vfs_cfg) == OLNK_ERR_INVALID_ARG,
          "vfs_disk_attach rejects invalid unit >= 26");

    /* 6b. Attach (RW + write_verify) and verify pre-init NOTRDY / STA_NOINIT */
    olnk_esp32_vfs_cfg_default(&vfs_cfg, "/dos", OLNK_ESP32_UNIT_AUTO);
    vfs_cfg.read_only = false;
    vfs_cfg.write_verify = true;
    CHECK(olnk_esp32_vfs_disk_attach(&g_vfs_mnt, &g_esp32_client, &vfs_cfg) == OLNK_OK,
          "vfs_disk_attach succeeds");
    CHECK(olnk_esp32_vfs_disk_status(&g_vfs_mnt) == OLNK_ESP32_DISK_STA_NOINIT,
          "disk_status returns STA_NOINIT before disk_init");
    CHECK(olnk_esp32_vfs_disk_read(&g_vfs_mnt, single_r, 0UL, 1UL) == OLNK_ESP32_DISK_RES_NOTRDY,
          "disk_read returns RES_NOTRDY before disk_init");
    CHECK(olnk_esp32_vfs_disk_ioctl(&g_vfs_mnt, OLNK_ESP32_DISK_CTRL_SYNC, NULL) ==
              OLNK_ESP32_DISK_RES_NOTRDY,
          "disk_ioctl returns RES_NOTRDY before disk_init");

    /* 6c. disk_init auto-selects unit 2 and caches BPB geometry */
    CHECK(olnk_esp32_vfs_disk_init(&g_vfs_mnt) == OLNK_ESP32_DISK_STA_OK,
          "disk_init returns STA_OK on RW remote FAT16 drive");
    CHECK(g_vfs_mnt.disk_ready && (g_vfs_mnt.unit == 2U) &&
              (g_vfs_mnt.bytes_per_sector == 512U) &&
              (g_vfs_mnt.total_sectors == 192UL) &&
              (g_vfs_mnt.sectors_per_cluster >= 1U) &&
              (g_vfs_mnt.media_descriptor == 0xF8U) &&
              (!g_vfs_mnt.read_only),
          "disk_init cached BPB geometry accurately");
    CHECK(olnk_esp32_vfs_disk_status(&g_vfs_mnt) == OLNK_ESP32_DISK_STA_OK,
          "disk_status returns STA_OK after disk_init");

    /* 6d. disk_ioctl queries & invalid command / NULL buff handling */
    CHECK(olnk_esp32_vfs_disk_ioctl(&g_vfs_mnt, OLNK_ESP32_DISK_CTRL_SYNC, NULL) ==
              OLNK_ESP32_DISK_RES_OK,
          "ioctl CTRL_SYNC returns RES_OK (NULL buff permitted)");
    CHECK(olnk_esp32_vfs_disk_ioctl(&g_vfs_mnt, OLNK_ESP32_DISK_GET_SECTOR_COUNT, &sec_count) ==
              OLNK_ESP32_DISK_RES_OK &&
              (sec_count == 192UL),
          "ioctl GET_SECTOR_COUNT returns 192");
    CHECK(olnk_esp32_vfs_disk_ioctl(&g_vfs_mnt, OLNK_ESP32_DISK_GET_SECTOR_SIZE, &sec_size) ==
              OLNK_ESP32_DISK_RES_OK &&
              (sec_size == 512U),
          "ioctl GET_SECTOR_SIZE returns 512");
    CHECK(olnk_esp32_vfs_disk_ioctl(&g_vfs_mnt, OLNK_ESP32_DISK_GET_BLOCK_SIZE, &blk_size) ==
              OLNK_ESP32_DISK_RES_OK &&
              (blk_size == (uint32_t)g_vfs_mnt.sectors_per_cluster),
          "ioctl GET_BLOCK_SIZE returns sectors_per_cluster");
    CHECK(olnk_esp32_vfs_disk_ioctl(&g_vfs_mnt, OLNK_ESP32_DISK_GET_SECTOR_COUNT, NULL) ==
              OLNK_ESP32_DISK_RES_PARERR,
          "ioctl GET_SECTOR_COUNT rejects NULL buff");
    CHECK(olnk_esp32_vfs_disk_ioctl(&g_vfs_mnt, 0x99U, &blk_size) == OLNK_ESP32_DISK_RES_PARERR,
          "ioctl unknown command returns RES_PARERR");

    /* 6e. Partition-relative LBA 0 read (must be VBR, not MBR) + single-sector R/W */
    memset(single_r, 0, sizeof(single_r));
    CHECK(olnk_esp32_vfs_disk_read(&g_vfs_mnt, single_r, 0UL, 1UL) == OLNK_ESP32_DISK_RES_OK,
          "disk_read LBA 0 (VBR) succeeds");
    CHECK((single_r[510] == 0x55U) && (single_r[511] == 0xAAU) &&
              (memcmp(single_r, g_e2e_sd_card.blocks[63], OLNK_SECTOR_SIZE) == 0),
          "disk_read LBA 0 returns partition-relative FAT VBR (physical block 63)");

    for (i = 0U; i < sizeof(single_w); ++i) {
        single_w[i] = (uint8_t)((i * 19U + 7U) ^ 0x5CU);
    }
    CHECK(olnk_esp32_vfs_disk_write(&g_vfs_mnt, single_w, 5UL, 1UL) == OLNK_ESP32_DISK_RES_OK,
          "disk_write single sector LBA 5 succeeds");
    CHECK(memcmp(g_e2e_sd_card.blocks[63U + 5U], single_w, OLNK_SECTOR_SIZE) == 0,
          "disk_write LBA 5 landed at physical block 68");
    memset(single_r, 0, sizeof(single_r));
    CHECK(olnk_esp32_vfs_disk_read(&g_vfs_mnt, single_r, 5UL, 1UL) == OLNK_ESP32_DISK_RES_OK &&
              (memcmp(single_w, single_r, OLNK_SECTOR_SIZE) == 0),
          "disk_read LBA 5 matches written sector");

    /* 6f. Multi-sector >8192 B (20 KiB = 40 sectors, chunked 16+16+8) R/W */
    for (i = 0U; i < sizeof(g_tx_20k); ++i) {
        g_tx_20k[i] = (uint8_t)((i * 73U + 3U) ^ 0x91U);
    }
    CHECK(olnk_esp32_vfs_disk_write(&g_vfs_mnt, g_tx_20k, 40UL, 40UL) == OLNK_ESP32_DISK_RES_OK,
          "disk_write 20 KiB (40 sectors) chunked transfer ok");
    memset(g_rx_20k, 0, sizeof(g_rx_20k));
    CHECK(olnk_esp32_vfs_disk_read(&g_vfs_mnt, g_rx_20k, 40UL, 40UL) == OLNK_ESP32_DISK_RES_OK &&
              (memcmp(g_tx_20k, g_rx_20k, sizeof(g_tx_20k)) == 0),
          "disk_read 20 KiB chunked transfer bit-for-bit identical");

    /* 6g. Out-of-bounds & parameter checks */
    CHECK(olnk_esp32_vfs_disk_read(&g_vfs_mnt, single_r, 192UL, 1UL) == OLNK_ESP32_DISK_RES_PARERR,
          "disk_read LBA == total_sectors returns RES_PARERR");
    CHECK(olnk_esp32_vfs_disk_read(&g_vfs_mnt, single_r, 190UL, 4UL) == OLNK_ESP32_DISK_RES_PARERR,
          "disk_read crossing end returns RES_PARERR");
    CHECK(olnk_esp32_vfs_disk_read(&g_vfs_mnt, single_r, 0xFFFFFFFFUL, 2UL) ==
              OLNK_ESP32_DISK_RES_PARERR,
          "disk_read uint32 wraparound returns RES_PARERR");
    CHECK(olnk_esp32_vfs_disk_write(&g_vfs_mnt, single_w, 192UL, 1UL) == OLNK_ESP32_DISK_RES_PARERR,
          "disk_write OOB returns RES_PARERR");
    CHECK(olnk_esp32_vfs_disk_read(&g_vfs_mnt, single_r, 0UL, 0UL) == OLNK_ESP32_DISK_RES_PARERR,
          "disk_read count=0 returns RES_PARERR");

    /* 6h. Server-side DOS error mapping (WRITE_PROTECT -> RES_WRPRT, NOT_READY -> RES_NOTRDY) */
    g_e2e_sd_drv.opts.write_protected = true;
    CHECK(olnk_esp32_vfs_disk_write(&g_vfs_mnt, single_w, 5UL, 1UL) == OLNK_ESP32_DISK_RES_WRPRT,
          "server 0x8100 WRITE_PROTECT maps to RES_WRPRT");
    g_e2e_sd_drv.opts.write_protected = false;

    g_e2e_sd_card.present = false;
    CHECK(olnk_esp32_vfs_disk_read(&g_vfs_mnt, single_r, 5UL, 1UL) == OLNK_ESP32_DISK_RES_NOTRDY,
          "server 0x8102 DRIVE_NOT_READY maps to RES_NOTRDY");
    CHECK(olnk_esp32_vfs_disk_init(&g_vfs_mnt) ==
              (uint8_t)(OLNK_ESP32_DISK_STA_NOINIT | OLNK_ESP32_DISK_STA_NODISK),
          "disk_init on removed card returns STA_NOINIT | STA_NODISK");
    g_e2e_sd_card.present = true;

    /* 6i. Software read-only mode (cfg.read_only=true) blocks writes before RPC */
    olnk_esp32_vfs_cfg_default(&vfs_cfg, "/dos", 2U);
    vfs_cfg.read_only = true;
    CHECK(olnk_esp32_vfs_disk_attach(&g_vfs_mnt, &g_esp32_client, &vfs_cfg) == OLNK_OK,
          "re-attach VFS mount read-only");
    CHECK(olnk_esp32_vfs_disk_init(&g_vfs_mnt) == OLNK_ESP32_DISK_STA_PROTECT,
          "RO disk_init returns STA_PROTECT");
    CHECK(olnk_esp32_vfs_disk_status(&g_vfs_mnt) == OLNK_ESP32_DISK_STA_PROTECT,
          "RO disk_status returns STA_PROTECT");
    srv_rpcs_before = g_e2e_server.server.rpc_count;
    CHECK(olnk_esp32_vfs_disk_write(&g_vfs_mnt, single_w, 5UL, 1UL) == OLNK_ESP32_DISK_RES_WRPRT,
          "RO disk_write returns RES_WRPRT");
    CHECK(g_e2e_server.server.rpc_count == srv_rpcs_before,
          "RO disk_write transmitted ZERO write RPCs to server");

    /* 6j. Server-advertised OLNK_MAP_READONLY_BIT enforces RO even with cfg.read_only=false */
    for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
        if ((g_esp32_client.client.map.drive_map_resp[i] & 0x7FU) == 2U) {
            g_esp32_client.client.map.drive_map_resp[i] |= OLNK_MAP_READONLY_BIT;
            break;
        }
    }
    CHECK(olnk_esp32_client_unit_is_readonly(&g_esp32_client, 2U),
          "unit_is_readonly detects OLNK_MAP_READONLY_BIT");
    olnk_esp32_vfs_cfg_default(&vfs_cfg, "/dos", 2U);
    vfs_cfg.read_only = false;
    CHECK(olnk_esp32_vfs_disk_attach(&g_vfs_mnt, &g_esp32_client, &vfs_cfg) == OLNK_OK,
          "attach RW cfg against server-RO unit");
    CHECK(olnk_esp32_vfs_disk_init(&g_vfs_mnt) == OLNK_ESP32_DISK_STA_PROTECT,
          "server OLNK_MAP_READONLY_BIT makes disk_init return STA_PROTECT");
    srv_rpcs_before = g_e2e_server.server.rpc_count;
    CHECK(olnk_esp32_vfs_disk_write(&g_vfs_mnt, single_w, 5UL, 1UL) == OLNK_ESP32_DISK_RES_WRPRT,
          "server-RO disk_write returns RES_WRPRT");
    CHECK(g_e2e_server.server.rpc_count == srv_rpcs_before,
          "server-RO disk_write transmitted ZERO write RPCs");

    /* 6k. Unmount / re-attach cleanup */
    CHECK(olnk_esp32_vfs_unmount(&g_vfs_mnt) == OLNK_OK, "vfs_unmount succeeds");
    CHECK((!g_vfs_mnt.disk_ready) && (!g_vfs_mnt.vfs_mounted) &&
              (g_vfs_mnt.client == NULL) && (g_vfs_mnt.pdrv == OLNK_ESP32_VFS_PDRV_NONE),
          "vfs_unmount cleared mount state cleanly");
    CHECK(olnk_esp32_vfs_disk_status(&g_vfs_mnt) == OLNK_ESP32_DISK_STA_NOINIT,
          "disk_status returns STA_NOINIT after unmount");

    /* 6l. Reject non-512B sector size and 0-sector volumes */
    {
        bad_geom_ctx_t bad_ctx;
        olnk_storage_ops_t bad_ops;

        memset(&bad_ops, 0, sizeof(bad_ops));
        bad_ops.drive_count = bad_geom_drive_count;
        bad_ops.get_drive_info = bad_geom_get_info;
        bad_ops.media_check = bad_geom_media_check;
        bad_ops.build_bpb = bad_geom_build_bpb;
        bad_ops.user_data = &bad_ctx;

        g_e2e_server.server.storage = bad_ops;

        bad_ctx.bps = 1024U;
        bad_ctx.total_sectors = 128UL;
        olnk_esp32_vfs_cfg_default(&vfs_cfg, "/dos", 0U);
        CHECK(olnk_esp32_vfs_disk_attach(&g_vfs_mnt, &g_esp32_client, &vfs_cfg) == OLNK_OK,
              "attach non-512B test volume");
        CHECK(olnk_esp32_vfs_disk_init(&g_vfs_mnt) == OLNK_ESP32_DISK_STA_NOINIT,
              "disk_init rejects non-512B sector size (1024 B)");

        bad_ctx.bps = 512U;
        bad_ctx.total_sectors = 0UL;
        CHECK(olnk_esp32_vfs_disk_init(&g_vfs_mnt) == OLNK_ESP32_DISK_STA_NOINIT,
              "disk_init rejects 0-sector volume");
    }

    /* 10. Clean client deinit & strict mutex balance audit */
    CHECK(olnk_esp32_client_deinit(&g_esp32_client) == OLNK_OK, "esp32_client_deinit ok");
    CHECK(!g_esp32_client.initialized, "client marked uninitialized after deinit");
    CHECK((g_spy_mtx.lock_count > 0UL) &&
              (g_spy_mtx.lock_count == g_spy_mtx.unlock_count) &&
              (g_spy_mtx.depth == 0UL) &&
              (g_spy_mtx.max_depth == 1UL) &&
              (!g_spy_mtx.double_lock_err) &&
              (!g_spy_mtx.unlock_underflow_err),
          "mutex lock/unlock strictly balanced with zero nesting or leaks");
    CHECK((g_spy_tr_ctx.io_calls_total > 0UL) && (g_spy_tr_ctx.io_calls_unlocked == 0UL),
          "100% of transport I/O calls executed while client mutex was held");

    CHECK(olnk_esp32_uart_deinit(&g_e2e_cli_uart) == OLNK_OK, "Client loopback UART deinit");
    CHECK(olnk_esp32_uart_deinit(&g_e2e_srv_uart) == OLNK_OK, "Server loopback UART deinit");
}

int main(void) {
    test_uart_hal();
    test_sd_block_hal();
    test_img_file_hal();
    test_e2e_client_esp32_server_loopback();
    test_esp32_client_and_vfs_loopback();

    printf("[test_esp32_hal] Passed: %d, Failed: %d\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
