/* cleanroom/tests/test_libopenlnk.c
 *
 * Comprehensive unit, link-layer state-machine, end-to-end RPC loopback,
 * and fault-injection test suite for libopenlnk (MS-DOS 6.22 protocol).
 *
 * Strictly ISO C99, zero dynamic memory allocation, zero floating-point.
 */
#include "openlnk/crc16.h"
#include "openlnk/openlnk.h"
#include "openlnk/protocol.h"
#include "openlnk/server.h"
#include "openlnk/storage.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures += 1; \
    } \
} while (0)

#define EXPECT_EQ_U32(actual, expected) do { \
    uint32_t _a = (uint32_t)(actual); \
    uint32_t _e = (uint32_t)(expected); \
    if (_a != _e) { \
        fprintf(stderr, "FAIL %s:%d: %s == %lu (0x%lX), expected %lu (0x%lX)\n", \
                __FILE__, __LINE__, #actual, \
                (unsigned long)_a, (unsigned long)_a, \
                (unsigned long)_e, (unsigned long)_e); \
        g_failures += 1; \
    } \
} while (0)

/* =========================================================================
 * Reusable Fault-Injecting Loopback Harness (Zero-Alloc, Single-Threaded)
 * ========================================================================= */

#define LB_RING_CAP 20480U

typedef struct {
    uint8_t buf[LB_RING_CAP];
    size_t head;
    size_t tail;
    size_t count;
} lb_ring_t;

typedef struct {
    lb_ring_t s2c;
    olnk_server_t *srv;
    uint32_t cli_baud;
    uint32_t srv_baud;

    /* Fault-injection controls */
    bool drop_all_c2s;
    bool flip_c2s_once;
    size_t flip_c2s_countdown;
    uint8_t flip_c2s_mask;
    uint32_t c2s_bytes_total;
    uint32_t s2c_bytes_total;

    /* Wire observer for echo verification */
    bool record_wire;
    uint8_t last_c2s_bytes[64];
    size_t last_c2s_len;
    uint8_t last_s2c_bytes[64];
    size_t last_s2c_len;
} lb_bus_t;

static void lb_ring_push(lb_ring_t *r, const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0U; i < len; ++i) {
        if (r->count < LB_RING_CAP) {
            r->buf[r->tail] = data[i];
            r->tail = (r->tail + 1U) % LB_RING_CAP;
            r->count += 1U;
        }
    }
}

static size_t lb_ring_pop(lb_ring_t *r, uint8_t *out, size_t len) {
    size_t n = 0U;
    while ((n < len) && (r->count > 0U)) {
        out[n++] = r->buf[r->head];
        r->head = (r->head + 1U) % LB_RING_CAP;
        r->count -= 1U;
    }
    return n;
}

static int lb_srv_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    lb_bus_t *bus = (lb_bus_t *)ud;
    (void)timeout_ms;
    bus->s2c_bytes_total += (uint32_t)len;
    if (bus->record_wire) {
        size_t i;
        for (i = 0U; (i < len) && (bus->last_s2c_len < sizeof(bus->last_s2c_bytes)); ++i) {
            bus->last_s2c_bytes[bus->last_s2c_len++] = buf[i];
        }
    }
    lb_ring_push(&bus->s2c, buf, len);
    return (int)len;
}

static bool lb_srv_set_baud(uint32_t baud, void *ud) {
    lb_bus_t *bus = (lb_bus_t *)ud;
    bus->srv_baud = baud;
    return true;
}

static int lb_cli_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    lb_bus_t *bus = (lb_bus_t *)ud;
    size_t i;
    (void)timeout_ms;

    for (i = 0U; i < len; ++i) {
        uint8_t b = buf[i];
        bus->c2s_bytes_total += 1U;
        if (bus->record_wire && (bus->last_c2s_len < sizeof(bus->last_c2s_bytes))) {
            bus->last_c2s_bytes[bus->last_c2s_len++] = b;
        }
        if (bus->drop_all_c2s) {
            continue;
        }
        if (bus->flip_c2s_once) {
            if (bus->flip_c2s_countdown == 0U) {
                b = (uint8_t)(b ^ bus->flip_c2s_mask);
                bus->flip_c2s_once = false;
            } else {
                bus->flip_c2s_countdown -= 1U;
            }
        }
        if (bus->cli_baud == bus->srv_baud) {
            olnk_server_feed_bytes(bus->srv, &b, 1U);
        }
    }
    return (int)len;
}

static int lb_cli_read(uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    lb_bus_t *bus = (lb_bus_t *)ud;
    size_t n = lb_ring_pop(&bus->s2c, buf, len);
    if ((n == 0U) && (timeout_ms > 0U)) {
        /* Advance server timers on client read timeout (for silent drain / beacon expiry) */
        olnk_server_tick(bus->srv, timeout_ms);
        n = lb_ring_pop(&bus->s2c, buf, len);
    }
    return (int)n;
}

static bool lb_cli_set_baud(uint32_t baud, void *ud) {
    lb_bus_t *bus = (lb_bus_t *)ud;
    bus->cli_baud = baud;
    return true;
}

static void lb_setup(
    lb_bus_t *bus,
    olnk_server_t *srv,
    olnk_ctx_t *cli,
    olnk_storage_ops_t storage
) {
    olnk_transport_t srv_tr;
    olnk_transport_t cli_tr;

    memset(bus, 0, sizeof(*bus));
    bus->srv = srv;
    bus->cli_baud = 9600UL;
    bus->srv_baud = 9600UL;

    memset(&srv_tr, 0, sizeof(srv_tr));
    srv_tr.write = lb_srv_write;
    srv_tr.set_baud = lb_srv_set_baud;
    srv_tr.user_data = bus;

    memset(&cli_tr, 0, sizeof(cli_tr));
    cli_tr.read = lb_cli_read;
    cli_tr.write = lb_cli_write;
    cli_tr.set_baud = lb_cli_set_baud;
    cli_tr.user_data = bus;

    EXPECT_EQ_U32(olnk_server_init(srv, srv_tr, storage), OLNK_OK);
    EXPECT_EQ_U32(olnk_init(cli, cli_tr), OLNK_OK);
}

/* =========================================================================
 * Test 1: CRC-16/GENIBUS & Check Residue 0x1D0F
 * ========================================================================= */

static void test_crc16_genibus_and_residue(void) {
    /* 1. Standard ASCII "123456789" -> CCITT-FALSE 0x29B1, GENIBUS 0xD64E */
    const uint8_t std_msg[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    const uint8_t std_with_crc[11] = {
        '1', '2', '3', '4', '5', '6', '7', '8', '9', 0xD6U, 0x4EU
    };
    /* 2. Sync Trigger (0x00, len=2): 02 00 00 -> 0x5D03 */
    const uint8_t sync_trig[5] = { 0x02U, 0x00U, 0x00U, 0x5DU, 0x03U };
    /* 3. Get Drive Info C: (0x1C, len=2): 02 1C 02 -> 0x3B5F */
    const uint8_t get_info[5] = { 0x02U, 0x1CU, 0x02U, 0x3BU, 0x5FU };
    /* 4. Read Sector LBA 0x53 (0x04, len=11) -> 0x704E */
    const uint8_t read_req[14] = {
        0x0BU, 0x04U, 0x02U, 0xF8U, 0x01U, 0x00U, 0xFFU, 0xFFU,
        0x53U, 0x00U, 0x00U, 0x00U, 0x70U, 0x4EU
    };
    uint8_t corrupt[14];

    EXPECT_EQ_U32(olnk_crc16_ccitt(std_msg, sizeof(std_msg)), 0x29B1U);
    EXPECT_EQ_U32(olnk_crc16_genibus(std_msg, sizeof(std_msg)), 0xD64EU);
    EXPECT_EQ_U32(olnk_crc16_ccitt(std_with_crc, sizeof(std_with_crc)), OLNK_CRC16_RESIDUE_GOOD);
    EXPECT_TRUE(olnk_crc16_check_residue(std_with_crc, sizeof(std_with_crc)));

    EXPECT_EQ_U32(olnk_crc16_genibus(sync_trig, 3U), 0x5D03U);
    EXPECT_TRUE(olnk_crc16_check_residue(sync_trig, sizeof(sync_trig)));

    EXPECT_EQ_U32(olnk_crc16_genibus(get_info, 3U), 0x3B5FU);
    EXPECT_TRUE(olnk_crc16_check_residue(get_info, sizeof(get_info)));

    EXPECT_EQ_U32(olnk_crc16_genibus(read_req, 12U), 0x704EU);
    EXPECT_TRUE(olnk_crc16_check_residue(read_req, sizeof(read_req)));

    memcpy(corrupt, read_req, sizeof(corrupt));
    corrupt[13] ^= 0x01U; /* Matches Fixture 22 */
    EXPECT_TRUE(!olnk_crc16_check_residue(corrupt, sizeof(corrupt)));
}

/* =========================================================================
 * Test 2: Header Bits, ACK Calculation, Keepalive, Turnaround, Baud Table
 * ========================================================================= */

static void test_protocol_tokens_and_wire_framing(void) {
    static uint8_t wire[OLNK_FRAME_BUFFER_SIZE];
    static uint8_t payload_big[8192];
    size_t wire_len = 0U;
    size_t consumed = 0U;
    olnk_wire_frame_t f;
    bool echo = false;
    uint8_t idx = 0U;
    size_t i;

    /* Header ACK Echo vs Burst */
    EXPECT_EQ_U32(olnk_hdr_compute_ack(0x80U, true), 0x7FU);
    EXPECT_EQ_U32(olnk_hdr_compute_ack(0x80U, false), 0x7BU);
    EXPECT_EQ_U32(olnk_hdr_compute_ack(0x84U, false), 0x7FU);
    EXPECT_TRUE(olnk_hdr_verify_ack(0x80U, 0x7FU, &echo) && echo);
    EXPECT_TRUE(olnk_hdr_verify_ack(0x80U, 0x7BU, &echo) && (!echo));
    EXPECT_TRUE(!olnk_hdr_verify_ack(0x80U, 0x7EU, &echo));

    /* Keepalive bytes for seq 0..3 -> 0xFD, 0xFC, 0xFF, 0xFE */
    EXPECT_EQ_U32(olnk_keepalive_byte(0U), 0xFDU);
    EXPECT_EQ_U32(olnk_keepalive_byte(1U), 0xFCU);
    EXPECT_EQ_U32(olnk_keepalive_byte(2U), 0xFFU);
    EXPECT_EQ_U32(olnk_keepalive_byte(3U), 0xFEU);

    /* Baud request bytes never collide with keepalive bytes for any seq */
    for (i = 0U; i < 4U; ++i) {
        EXPECT_TRUE(olnk_baud_req_byte((uint8_t)i) != olnk_keepalive_byte((uint8_t)i));
    }

    /* Baud index table */
    EXPECT_EQ_U32(olnk_baud_index_to_bps(OLNK_BAUD_9600), 9600UL);
    EXPECT_EQ_U32(olnk_baud_index_to_divisor(OLNK_BAUD_9600), 12U);
    EXPECT_EQ_U32(olnk_baud_index_to_bps(OLNK_BAUD_115200), 115200UL);
    EXPECT_EQ_U32(olnk_baud_index_to_divisor(OLNK_BAUD_115200), 1U);
    EXPECT_TRUE(olnk_bps_to_baud_index(38400UL, &idx) && (idx == OLNK_BAUD_38400));
    EXPECT_TRUE(!olnk_bps_to_baud_index(14400UL, &idx));

    /* Exact wire encoding match for Fixture 01: 80 02 00 00 5d 03 */
    {
        const uint8_t trig[2] = { 0x00U, 0x00U };
        const uint8_t expect[6] = { 0x80U, 0x02U, 0x00U, 0x00U, 0x5DU, 0x03U };
        EXPECT_EQ_U32(olnk_wire_frame_encode(0U, false, false, trig, 2U, wire, sizeof(wire), &wire_len), OLNK_OK);
        EXPECT_EQ_U32(wire_len, 6U);
        EXPECT_TRUE(memcmp(wire, expect, 6U) == 0);
        EXPECT_EQ_U32(olnk_wire_frame_decode(wire, wire_len, &f, &consumed), OLNK_OK);
        EXPECT_EQ_U32(consumed, 6U);
        EXPECT_EQ_U32(f.crc16, 0x5D03U);
        EXPECT_TRUE(f.is_short_len && !f.more_fragments && !f.echo_request);
    }

    /* Max-size 8192 B fragmented frame (8197 B on wire) */
    for (i = 0U; i < sizeof(payload_big); ++i) {
        payload_big[i] = (uint8_t)((i * 13U + 5U) & 0xFFU);
    }
    EXPECT_EQ_U32(olnk_wire_frame_encode(2U, true, false, payload_big, 8192U, wire, sizeof(wire), &wire_len), OLNK_OK);
    EXPECT_EQ_U32(wire_len, OLNK_FRAME_BUFFER_SIZE);
    EXPECT_EQ_U32(wire[0], 0x42U);
    EXPECT_EQ_U32(olnk_wire_frame_decode(wire, wire_len, &f, &consumed), OLNK_OK);
    EXPECT_EQ_U32(f.payload_length, 8192U);
    EXPECT_EQ_U32(f.sequence_number, 2U);
    EXPECT_TRUE(!f.is_short_len && f.more_fragments);
    EXPECT_TRUE(memcmp(f.payload, payload_big, 8192U) == 0);

    /* Reserved header bits (Fixture 24: 0x98) -> OLNK_ERR_INVALID_FRAME */
    {
        const uint8_t bad_hdr[6] = { 0x98U, 0x02U, 0x00U, 0x00U, 0x5DU, 0x03U };
        EXPECT_EQ_U32(olnk_wire_frame_decode(bad_hdr, sizeof(bad_hdr), &f, &consumed), OLNK_ERR_INVALID_FRAME);
    }

    /* Truncated frame -> OLNK_ERR_NEED_DATA */
    EXPECT_EQ_U32(olnk_wire_frame_decode(wire, 100U, &f, &consumed), OLNK_ERR_NEED_DATA);

    /* Oversized length (> 8192) -> OLNK_ERR_BUFFER_OVERFLOW */
    {
        uint8_t over[5] = { 0x00U, 0x01U, 0x20U, 0x00U, 0x00U }; /* len = 8193 */
        EXPECT_EQ_U32(olnk_wire_frame_decode(over, sizeof(over), &f, &consumed), OLNK_ERR_BUFFER_OVERFLOW);
    }
}

/* =========================================================================
 * Test 3: Streaming Canonical Frame Parser & Fault Recovery
 * ========================================================================= */

typedef struct {
    uint32_t count;
    olnk_wire_frame_t last;
} parser_sink_t;

static void on_parsed_frame(const olnk_wire_frame_t *frame, void *ud) {
    parser_sink_t *s = (parser_sink_t *)ud;
    s->count += 1U;
    s->last = *frame;
}

static void test_streaming_parser_and_faults(void) {
    static olnk_ctx_t ctx;
    olnk_transport_t tr;
    parser_sink_t sink;
    const uint8_t valid_read_req[15] = {
        0x83U, 0x0BU, 0x04U, 0x02U, 0xF8U, 0x01U, 0x00U, 0xFFU,
        0xFFU, 0x53U, 0x00U, 0x00U, 0x00U, 0x70U, 0x4EU
    };
    uint8_t bad_crc[15];
    const uint8_t bad_rsvd[6] = { 0x98U, 0x02U, 0x00U, 0x00U, 0x5DU, 0x03U };
    size_t i;
    size_t consumed = 0U;

    memset(&tr, 0, sizeof(tr));
    memset(&sink, 0, sizeof(sink));
    EXPECT_EQ_U32(olnk_init(&ctx, tr), OLNK_OK);
    olnk_set_frame_handler(&ctx, on_parsed_frame, &sink);

    /* Byte-by-byte valid frame (Fixture 13) */
    for (i = 0U; i < sizeof(valid_read_req); ++i) {
        olnk_status_t st = olnk_feed_bytes(&ctx, &valid_read_req[i], 1U, &consumed);
        EXPECT_EQ_U32(consumed, 1U);
        if (i + 1U < sizeof(valid_read_req)) {
            EXPECT_EQ_U32(st, OLNK_ERR_NEED_DATA);
        } else {
            EXPECT_EQ_U32(st, OLNK_OK);
        }
    }
    EXPECT_EQ_U32(sink.count, 1U);
    EXPECT_EQ_U32(sink.last.sequence_number, 3U);
    EXPECT_EQ_U32(sink.last.crc16, 0x704EU);

    /* Bitflip -> CHECKSUM_MISMATCH */
    memcpy(bad_crc, valid_read_req, sizeof(bad_crc));
    bad_crc[14] ^= 0x01U;
    EXPECT_EQ_U32(olnk_feed_bytes(&ctx, bad_crc, sizeof(bad_crc), &consumed), OLNK_ERR_CHECKSUM_MISMATCH);
    EXPECT_EQ_U32(ctx.frames_rx_err, 1U);

    /* Reserved header bits -> immediate OLNK_ERR_INVALID_FRAME on byte 0 */
    EXPECT_EQ_U32(olnk_feed_bytes(&ctx, bad_rsvd, sizeof(bad_rsvd), &consumed), OLNK_ERR_INVALID_FRAME);
    EXPECT_EQ_U32(consumed, 1U);
    EXPECT_EQ_U32(ctx.frames_rx_err, 2U);

    /* Clean recovery on next valid frame */
    EXPECT_EQ_U32(olnk_feed_bytes(&ctx, valid_read_req, sizeof(valid_read_req), &consumed), OLNK_OK);
    EXPECT_EQ_U32(sink.count, 2U);
}

/* =========================================================================
 * Test 4: Phase A Sync, Phase B Baud Negotiation (3->7) & Fallback
 * ========================================================================= */

static void test_sync_and_baud_negotiation_and_fallback(void) {
    static olnk_server_t srv;
    static olnk_ctx_t cli;
    static lb_bus_t bus;
    static uint8_t disk[OLNK_SECTOR_SIZE * 4U];
    olnk_mem_drive_t drv;

    olnk_fat_build_synthetic_vbr(disk, 4UL, 0xF0U, "BAUD_TEST", 0x12345678UL);
    olnk_mem_drive_init(&drv, disk, 4UL);

    /* 4a. Full step-up 9600 -> 19200 -> 38400 -> 57600 -> 115200 */
    lb_setup(&bus, &srv, &cli, olnk_mem_drive_ops(&drv));
    EXPECT_EQ_U32(olnk_link_connect_client(&cli), OLNK_OK);
    EXPECT_EQ_U32(cli.current_baud_idx, OLNK_BAUD_115200);
    EXPECT_EQ_U32(srv.link.current_baud_idx, OLNK_BAUD_115200);
    EXPECT_EQ_U32(bus.cli_baud, 115200UL);
    EXPECT_EQ_U32(bus.srv_baud, 115200UL);
    EXPECT_EQ_U32(cli.baud_switches, 4U);
    EXPECT_EQ_U32(srv.link.baud_switches, 4U);

    /* 4b. Server ceiling cap at 19200 -> client negotiates 19200 & clamps safely */
    lb_setup(&bus, &srv, &cli, olnk_mem_drive_ops(&drv));
    srv.link.max_baud_idx = OLNK_BAUD_19200;
    cli.max_baud_idx = OLNK_BAUD_115200;
    EXPECT_EQ_U32(olnk_link_connect_client(&cli), OLNK_OK);
    EXPECT_EQ_U32(cli.current_baud_idx, OLNK_BAUD_19200);
    EXPECT_EQ_U32(cli.max_baud_idx, OLNK_BAUD_19200);
    EXPECT_EQ_U32(srv.link.current_baud_idx, OLNK_BAUD_19200);
    EXPECT_EQ_U32(bus.cli_baud, 19200UL);
    EXPECT_EQ_U32(bus.srv_baud, 19200UL);

    /* 4c. Line quality failure during 38400 test -> both sides revert to 19200 */
    lb_setup(&bus, &srv, &cli, olnk_mem_drive_ops(&drv));
    cli.max_baud_idx = OLNK_BAUD_19200;
    EXPECT_EQ_U32(olnk_link_connect_client(&cli), OLNK_OK);
    EXPECT_EQ_U32(bus.cli_baud, 19200UL);

    /* Corrupt 5th TX byte during 19200->38400 negotiation (hits TEST_TX) */
    bus.flip_c2s_once = true;
    bus.flip_c2s_countdown = 5U;
    bus.flip_c2s_mask = 0x80U;
    EXPECT_TRUE(olnk_link_negotiate_baud_initiator(&cli, OLNK_BAUD_38400) != OLNK_OK);
    EXPECT_EQ_U32(cli.current_baud_idx, OLNK_BAUD_19200);
    EXPECT_EQ_U32(srv.link.current_baud_idx, OLNK_BAUD_19200);
    EXPECT_EQ_U32(bus.cli_baud, 19200UL);
    EXPECT_EQ_U32(bus.srv_baud, 19200UL);
}

/* =========================================================================
 * Test 5: Echo Mode (crc_lo NOT echoed) vs Burst Mode & Burst->Echo Recovery
 * ========================================================================= */

static void test_echo_vs_burst_and_error_recovery(void) {
    static olnk_server_t srv;
    static olnk_ctx_t cli;
    static lb_bus_t bus;
    static uint8_t disk[OLNK_SECTOR_SIZE * 4U];
    olnk_mem_drive_t drv;
    uint8_t msg[32];
    size_t msg_len = 0U;
    uint16_t rx_len = 0U;
    bool more = false;
    olnk_get_drive_info_req_t req = { OLNK_OP_GET_DRIVE_INFO, 0U };

    olnk_fat_build_synthetic_vbr(disk, 4UL, 0xF0U, "ECHO_TEST", 0xAABBCCDDUL);
    olnk_mem_drive_init(&drv, disk, 4UL);

    /* 5a. Echo-Paced Mode: verify exact wire bytes (crc_lo NOT echoed!) */
    lb_setup(&bus, &srv, &cli, olnk_mem_drive_ops(&drv));
    cli.max_baud_idx = OLNK_BAUD_9600;
    srv.link.local_requires_echo = true;
    cli.local_requires_echo = true;
    EXPECT_EQ_U32(olnk_link_connect_client(&cli), OLNK_OK);

    (void)olnk_encode_get_drive_info_req(&req, msg, sizeof(msg), &msg_len);
    bus.record_wire = true;
    bus.last_c2s_len = 0U;
    bus.last_s2c_len = 0U;
    EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
    bus.record_wire = false;

    /* Frame: hdr(80), len(02), op(1C), unit(00), crc_hi, crc_lo (6 bytes C->S).
     * Echo S->C must be: ack_hdr(7F), len(02), op(1C), unit(00), crc_hi, final_ack(80)
     * — exactly 6 bytes (crc_lo is NOT echoed, directly final_ack == 0x80)! */
    EXPECT_EQ_U32(bus.last_c2s_len, 6U);
    EXPECT_EQ_U32(bus.last_s2c_len, 6U);
    EXPECT_EQ_U32(bus.last_s2c_bytes[0], 0x7FU); /* ~0x80 => Echo mode */
    EXPECT_EQ_U32(bus.last_s2c_bytes[1], 0x02U);
    EXPECT_EQ_U32(bus.last_s2c_bytes[2], OLNK_OP_GET_DRIVE_INFO);
    EXPECT_EQ_U32(bus.last_s2c_bytes[3], 0x00U);
    EXPECT_EQ_U32(bus.last_s2c_bytes[4], bus.last_c2s_bytes[4]); /* crc_hi echoed */
    EXPECT_EQ_U32(bus.last_s2c_bytes[5], 0x80U); /* final_ack == hdr (NOT crc_lo!) */

    EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
    EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
    EXPECT_EQ_U32(rx_len, OLNK_LEN_GET_DRIVE_INFO_RSP);
    EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);

    /* 5b. Burst (No-Echo) Mode: verify ack_hdr = (~hdr) & 0xFB and only 2 bytes S->C! */
    srv.link.local_requires_echo = false;
    cli.local_requires_echo = false;
    (void)olnk_encode_get_drive_info_req(&req, msg, sizeof(msg), &msg_len);
    bus.record_wire = true;
    bus.last_c2s_len = 0U;
    bus.last_s2c_len = 0U;
    EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
    bus.record_wire = false;

    EXPECT_EQ_U32(bus.last_c2s_len, 6U);
    EXPECT_EQ_U32(bus.last_s2c_len, 2U); /* Only ack_hdr and final_ack! */
    EXPECT_EQ_U32(bus.last_s2c_bytes[0], (uint8_t)((~(unsigned int)bus.last_c2s_bytes[0]) & 0xFBU));
    EXPECT_EQ_U32(bus.last_s2c_bytes[1], bus.last_c2s_bytes[0]);

    EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
    EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
    EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);

    /* 5c. Fault Injection in Burst Mode: bitflip -> silent drain -> auto Echo retry succeeds! */
    (void)olnk_encode_get_drive_info_req(&req, msg, sizeof(msg), &msg_len);
    bus.flip_c2s_once = true;
    bus.flip_c2s_countdown = 3U; /* Flip payload byte inside Burst frame */
    bus.flip_c2s_mask = 0x01U;
    EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
    EXPECT_TRUE(cli.peer_echo_forced);
    EXPECT_EQ_U32(cli.frames_tx_retry, 1U);
    EXPECT_EQ_U32(srv.link.frames_rx_err, 1U);

    EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
    EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
    EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);
}

/* =========================================================================
 * Test 6: Full Multi-Drive End-to-End RPC Suite (MBR + FAT12/FAT16 + Keepalive)
 * ========================================================================= */

static void test_end_to_end_rpc_and_session_resumption(void) {
    static olnk_server_t srv;
    static olnk_ctx_t cli;
    static lb_bus_t bus;
    static uint8_t floppy_a[OLNK_SECTOR_SIZE * 16U];
    static uint8_t floppy_b[OLNK_SECTOR_SIZE * 16U];
    /* Partitioned disk image: MBR at LBA 0 + FAT16 volume starting at LBA 17 (48 sectors total) */
    static uint8_t hdd_c[OLNK_SECTOR_SIZE * 65U];
    static uint8_t pattern_20k[OLNK_SECTOR_SIZE * 40U]; /* 20,480 B = 2x 8192 + 4096 B! */
    static uint8_t readback_20k[OLNK_SECTOR_SIZE * 40U];

    olnk_mem_drive_t drv_a;
    olnk_mem_drive_t drv_c;
    olnk_mem_drive_set_t dset;
    uint8_t msg[256];
    size_t msg_len = 0U;
    uint16_t rx_len = 0U;
    uint32_t total_rx = 0UL;
    bool more = false;
    uint32_t saved_server_id = 0UL;
    size_t i;

    memset(floppy_a, 0, sizeof(floppy_a));
    memset(floppy_b, 0, sizeof(floppy_b));
    memset(hdd_c, 0, sizeof(hdd_c));

    olnk_fat_build_synthetic_vbr(floppy_a, 16UL, 0xF0U, "DISK_A", 0xFACA1E25UL);
    olnk_fat_build_synthetic_vbr(floppy_b, 16UL, 0xF0U, "DISK_B", 0x41011E25UL);

    /* Build MBR at sector 0 pointing to FAT16 partition at LBA 17 (48 sectors) */
    hdd_c[0x1BEU + 0U] = 0x80U;
    hdd_c[0x1BEU + 4U] = 0x04U; /* FAT16 < 32MB */
    olnk_write_u32_le(&hdd_c[0x1BEU + 8U], 17UL);
    olnk_write_u32_le(&hdd_c[0x1BEU + 12U], 48UL);
    hdd_c[510] = 0x55U;
    hdd_c[511] = 0xAAU;
    olnk_fat_build_synthetic_vbr(&hdd_c[17U * OLNK_SECTOR_SIZE], 48UL, 0xF8U, "DOS622_SRV", 0x29661CF8UL);

    olnk_mem_drive_init(&drv_a, floppy_a, 16UL);
    olnk_mem_drive_init(&drv_c, hdd_c, 65UL);
    EXPECT_EQ_U32(drv_c.partition_lba, 17UL);
    EXPECT_EQ_U32(drv_c.volume_sectors, 48UL);

    memset(&dset, 0, sizeof(dset));
    dset.drives[0] = &drv_a; /* Server A: */
    dset.drives[2] = &drv_c; /* Server C: */
    dset.drive_count = 3U;

    lb_setup(&bus, &srv, &cli, olnk_mem_drive_set_ops(&dset));
    srv.idle_disconnect_ms = 1000UL;
    EXPECT_EQ_U32(olnk_link_connect_client(&cli), OLNK_OK);

    /* 1. 0x00 SESSION_SYNC (initial) */
    {
        olnk_sync_trigger_t trig = { OLNK_OP_SESSION_SYNC, 0U };
        olnk_session_sync_req_t req;
        olnk_session_sync_resp_t resp;
        memset(&req, 0, sizeof(req));
        req.client_version = OLNK_CLIENT_VERSION_DOS622;
        req.proto_major = 1U;
        req.capability = 1U;
        req.max_block_size = 8192U;
        req.client_id = 0xAD37AFF7UL;
        req.server_id = 0UL;

        (void)olnk_encode_sync_trigger(&trig, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        (void)olnk_encode_session_sync_req(&req, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);
        EXPECT_EQ_U32(rx_len, OLNK_LEN_SESSION_SYNC_RESP);
        EXPECT_EQ_U32(olnk_decode_session_sync_resp(msg, rx_len, &resp), OLNK_OK);
        EXPECT_EQ_U32(resp.server_version, OLNK_SERVER_VERSION_DOS622);
        EXPECT_EQ_U32(resp.ready_flag, 1U);
        EXPECT_EQ_U32(resp.prev_client_id, 0UL);
        EXPECT_TRUE(resp.server_id != 0UL);
        saved_server_id = resp.server_id;
    }

    /* 2. 0x01 MAP_CONFIG (auto-assigns A:=0 and C:=2 into slots 0 & 1!) */
    {
        olnk_map_config_req_t req;
        olnk_map_config_resp_t resp;
        memset(&req, 0, sizeof(req));
        req.opcode = OLNK_OP_MAP_CONFIG;
        req.client_proto_ver = OLNK_MAP_PROTO_VER;
        req.client_dos_minor = 22U;
        req.client_dos_major = 6U;
        req.max_client_drives = 4U;
        req.first_client_drive = 3U;
        req.lpt_map_req[0] = OLNK_MAP_NONE;
        req.lpt_map_req[1] = OLNK_MAP_AUTO;
        req.lpt_map_req[2] = OLNK_MAP_AUTO;
        memset(req.drive_map_req, OLNK_MAP_AUTO, sizeof(req.drive_map_req));

        (void)olnk_encode_map_config_req(&req, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_decode_map_config_resp(msg, rx_len, &resp), OLNK_OK);
        EXPECT_EQ_U32(resp.server_drive_count, 3U);
        EXPECT_EQ_U32(resp.drive_map_resp[0], 0U);
        EXPECT_EQ_U32(resp.drive_map_resp[1], 2U);
        EXPECT_EQ_U32(resp.drive_map_resp[2], OLNK_MAP_AUTO);
        EXPECT_EQ_U32(resp.server_drive_attrs[0], OLNK_DRIVE_ATTR_DEFAULT);
        EXPECT_EQ_U32(resp.server_drive_attrs[2], OLNK_DRIVE_ATTR_DEFAULT);
    }

    /* 3. 0x16 / 0x17 BLK_DEVICE_OPEN & CLOSE + 0x07 DRIVE_INCOMPAT (one-way) */
    {
        olnk_unit_ctrl_req_t ureq = { OLNK_OP_BLK_DEVICE_OPEN, 2U };
        olnk_drive_incompat_t inc = { OLNK_OP_DRIVE_INCOMPAT, 0xFFU, 1U, 0x00U, 5U };
        uint16_t st = 0U;

        (void)olnk_encode_unit_ctrl_req(&ureq, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_decode_status_only_resp(msg, rx_len, &st), OLNK_OK);
        EXPECT_EQ_U32(st, OLNK_DOS_STATUS_DONE);

        /* One-way 0x07 DRIVE_INCOMPAT: no turnaround, no server response */
        (void)olnk_encode_drive_incompat(&inc, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
    }

    /* 4. 0x1D GENERIC_IOCTL Get Media ID (08h/66h) + Set Media ID (08h/46h) + Unsupported */
    {
        olnk_generic_ioctl_req_t ireq = { OLNK_OP_GENERIC_IOCTL, 2U, OLNK_IOCTL_CAT_DISK, OLNK_IOCTL_FN_GET_MEDIA_ID };
        olnk_generic_ioctl_mid_t mid;
        uint16_t st = 0U;

        (void)olnk_encode_generic_ioctl_req(&ireq, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_decode_status_only_resp(msg, rx_len, &st), OLNK_OK);
        EXPECT_EQ_U32(st, OLNK_DOS_STATUS_DONE);
        EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_decode_generic_ioctl_mid(msg, rx_len, &mid), OLNK_OK);
        EXPECT_EQ_U32(mid.serial_number, 0x29661CF8UL);
        EXPECT_TRUE(memcmp(mid.volume_label, "DOS622_SRV ", 11U) == 0);

        /* Set Media ID (08h/46h) -> verify updated serial */
        ireq.function = OLNK_IOCTL_FN_SET_MEDIA_ID;
        mid.serial_number = 0xDEADBEEFUL;
        memcpy(mid.volume_label, "NEW_LABEL  ", 11U);
        (void)olnk_encode_generic_ioctl_req(&ireq, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        (void)olnk_encode_generic_ioctl_mid(&mid, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_decode_status_only_resp(msg, rx_len, &st), OLNK_OK);
        EXPECT_EQ_U32(st, OLNK_DOS_STATUS_DONE);
        EXPECT_EQ_U32(drv_c.info.volume_serial, 0xDEADBEEFUL);
    }

    /* 5. Multi-Fragment 20 KiB (40 sectors = 8192 + 8192 + 4096 B) WRITE_VERIFY & READ on C: */
    for (i = 0U; i < sizeof(pattern_20k); ++i) {
        pattern_20k[i] = (uint8_t)((i * 73U + 19U) & 0xFFU);
    }
    {
        olnk_sector_io_req_t wreq = { OLNK_OP_WRITE_VERIFY, 2U, 0xF8U, 40U, 0xFFFFU, 4UL };
        olnk_sector_io_resp_t wresp;
        (void)olnk_encode_sector_io_req(&wreq, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_send_message(&cli, pattern_20k, (uint32_t)sizeof(pattern_20k)), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_decode_sector_io_resp(msg, rx_len, &wresp), OLNK_OK);
        EXPECT_EQ_U32(wresp.status, OLNK_DOS_STATUS_DONE);
        EXPECT_EQ_U32(wresp.sectors_transferred, 40U);

        /* Verify write landed at physical LBA = partition_lba(17) + 4 = 21! */
        EXPECT_TRUE(memcmp(&hdd_c[21U * OLNK_SECTOR_SIZE], pattern_20k, sizeof(pattern_20k)) == 0);

        /* Read back 40 sectors via 3 fragments */
        wreq.opcode = OLNK_OP_READ_SECTORS;
        (void)olnk_encode_sector_io_req(&wreq, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_decode_sector_io_resp(msg, rx_len, &wresp), OLNK_OK);
        EXPECT_EQ_U32(wresp.status, OLNK_DOS_STATUS_DONE);
        EXPECT_EQ_U32(wresp.sectors_transferred, 40U);
        EXPECT_EQ_U32(olnk_link_recv_message(&cli, readback_20k, (uint32_t)sizeof(readback_20k), &total_rx, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);
        EXPECT_EQ_U32(total_rx, (uint32_t)sizeof(pattern_20k));
        EXPECT_TRUE(memcmp(readback_20k, pattern_20k, sizeof(pattern_20k)) == 0);
    }

    /* 6. Invalid Disk Change (0x810F with 20-byte Volume ID payload) on A: */
    drv_a.invalid_disk_change = true;
    {
        olnk_sector_io_req_t rreq = { OLNK_OP_READ_SECTORS, 0U, 0xF0U, 1U, 1U, 1UL };
        olnk_sector_io_resp_t rresp;
        (void)olnk_encode_sector_io_req(&rreq, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);
        EXPECT_EQ_U32(rx_len, OLNK_LEN_SECTOR_IO_RESP_CHG);
        EXPECT_EQ_U32(olnk_decode_sector_io_resp(msg, rx_len, &rresp), OLNK_OK);
        EXPECT_EQ_U32(rresp.status, OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_INVALID_DISK_CHG));
        EXPECT_TRUE(rresp.has_volume_id);
        EXPECT_TRUE(strcmp(rresp.volume_label, "DISK_A") == 0);
        EXPECT_EQ_U32(rresp.volume_serial, 0xFACA1E25UL);
    }

    /* 7. Idle Keepalives, Blackout Disconnect (>10 misses), Reconnect & Session Resumption! */
    EXPECT_EQ_U32(olnk_link_send_keepalive(&cli), OLNK_OK);
    EXPECT_EQ_U32(olnk_link_send_keepalive(&cli), OLNK_OK);
    EXPECT_TRUE(srv.link.keepalives_ok >= 2U);

    bus.drop_all_c2s = true;
    for (i = 0U; i < OLNK_KEEPALIVE_MAX_MISSED; ++i) {
        EXPECT_EQ_U32(olnk_link_send_keepalive(&cli), OLNK_ERR_HAL_TIMEOUT);
    }
    /* 11th miss drops link to 9600 bps */
    EXPECT_EQ_U32(olnk_link_send_keepalive(&cli), OLNK_ERR_DISCONNECTED);
    EXPECT_EQ_U32(cli.link_state, OLNK_LINK_DISCONNECTED);
    EXPECT_EQ_U32(bus.cli_baud, 9600UL);

    /* Server detects idle silence and drops back to 9600 bps, preserving session IDs */
    olnk_server_tick(&srv, 1000UL);
    EXPECT_EQ_U32(srv.srv_state, OLNK_SRV_SYNC_WAIT_AA);
    EXPECT_EQ_U32(bus.srv_baud, 9600UL);

    /* Restore line, reconnect, and verify transparent Session Resumption! */
    bus.drop_all_c2s = false;
    EXPECT_EQ_U32(olnk_link_connect_client(&cli), OLNK_OK);
    EXPECT_EQ_U32(bus.cli_baud, 115200UL);
    {
        olnk_sync_trigger_t trig = { OLNK_OP_SESSION_SYNC, 0U };
        olnk_session_sync_req_t req;
        olnk_session_sync_resp_t resp;
        memset(&req, 0, sizeof(req));
        req.client_version = OLNK_CLIENT_VERSION_DOS622;
        req.proto_major = 1U;
        req.capability = 1U;
        req.max_block_size = 8192U;
        req.client_id = 0xAD37AFF7UL;
        req.server_id = saved_server_id;

        (void)olnk_encode_sync_trigger(&trig, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        (void)olnk_encode_session_sync_req(&req, msg, sizeof(msg), &msg_len);
        EXPECT_EQ_U32(olnk_link_send_frame(&cli, msg, (uint16_t)msg_len, false), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_send_turnaround(&cli), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_recv_frame(&cli, msg, sizeof(msg), &rx_len, &more, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_link_wait_turnaround(&cli, 500U), OLNK_OK);
        EXPECT_EQ_U32(olnk_decode_session_sync_resp(msg, rx_len, &resp), OLNK_OK);
        EXPECT_EQ_U32(resp.prev_client_id, 0xAD37AFF7UL);
        EXPECT_EQ_U32(resp.server_id, saved_server_id);
        EXPECT_TRUE(srv.link.session_resumed);
    }
}

/* =========================================================================
 * Test 7: Storage Helpers (VHD Footer, Size Formatting, FAT12/FAT16 Parsing)
 * ========================================================================= */

static void test_storage_helpers(void) {
    uint8_t tail[OLNK_SECTOR_SIZE];
    char size_str[10];

    memset(tail, 0, sizeof(tail));
    EXPECT_TRUE(!olnk_vhd_has_fixed_footer(tail));
    memcpy(tail, "conectix", 8U);
    EXPECT_TRUE(olnk_vhd_has_fixed_footer(tail));

    olnk_fat_format_size_string(20791UL, 512U, size_str);
    EXPECT_TRUE(strcmp(size_str, "(10Mb)") == 0);

    olnk_fat_format_size_string(2880UL, 512U, size_str);
    EXPECT_TRUE(strcmp(size_str, "(1440Kb)") == 0);
}

int main(void) {
    test_crc16_genibus_and_residue();
    test_protocol_tokens_and_wire_framing();
    test_streaming_parser_and_faults();
    test_sync_and_baud_negotiation_and_fallback();
    test_echo_vs_burst_and_error_recovery();
    test_end_to_end_rpc_and_session_resumption();
    test_storage_helpers();

    if (g_failures == 0) {
        printf("All libopenlnk C99 MS-DOS 6.22 protocol tests PASSED.\n");
        return 0;
    }
    fprintf(stderr, "%d test assertion(s) FAILED.\n", g_failures);
    return 1;
}
