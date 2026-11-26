/* cleanroom/tests/test_libopenlnk.c
 *
 * Unit, loopback state-machine, and fault-injection test suite for libopenlnk.
 * Strictly C99, zero malloc, zero floating-point arithmetic.
 */
#include "openlnk/crc16.h"
#include "openlnk/openlnk.h"
#include "openlnk/protocol.h"
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
        fprintf(stderr, "FAIL %s:%d: %s == %lu (expected %lu)\n", \
                __FILE__, __LINE__, #actual, (unsigned long)_a, (unsigned long)_e); \
        g_failures += 1; \
    } \
} while (0)

typedef struct {
    bool received;
    uint32_t count;
    olnk_frame_t last_frame;
    uint8_t last_data[OLNK_SECTOR_SIZE];
    size_t last_data_len;
} test_sink_t;

static void sink_cb(const olnk_frame_t *frame, void *user_data) {
    test_sink_t *sink = (test_sink_t *)user_data;
    sink->received = true;
    sink->count += 1U;
    sink->last_frame = *frame;
    sink->last_data_len = 0U;

    if ((frame->frame_type == OLNK_FRAME_RESP_READ_DATA) &&
        (frame->payload.read_data.data != NULL) &&
        (frame->payload.read_data.data_len <= sizeof(sink->last_data))) {
        memcpy(sink->last_data, frame->payload.read_data.data, frame->payload.read_data.data_len);
        sink->last_data_len = frame->payload.read_data.data_len;
    } else if ((frame->frame_type == OLNK_FRAME_CMD_WRITE_SECTOR) &&
               (frame->payload.write_sector.data != NULL) &&
               (frame->payload.write_sector.data_len <= sizeof(sink->last_data))) {
        memcpy(sink->last_data, frame->payload.write_sector.data, frame->payload.write_sector.data_len);
        sink->last_data_len = frame->payload.write_sector.data_len;
    }
}

static void test_crc16_standard_vector(void) {
    /* Standard CRC-16/CCITT-FALSE check value for ASCII "123456789" is 0x29B1 */
    const uint8_t msg[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    uint16_t crc = olnk_crc16_ccitt(msg, sizeof(msg));
    EXPECT_EQ_U32(crc, 0x29B1U);
}

static void test_encode_decode_roundtrip_all_types(void) {
    static uint8_t wire[OLNK_RX_BUFFER_SIZE];
    static uint8_t sector_data[OLNK_SECTOR_SIZE];
    size_t wire_len = 0U;
    size_t i;
    olnk_frame_t in_f;
    olnk_frame_t out_f;

    for (i = 0U; i < sizeof(sector_data); ++i) {
        sector_data[i] = (uint8_t)(i & 0xFFU);
    }

    /* 1. CMD_INIT */
    memset(&in_f, 0, sizeof(in_f));
    in_f.frame_type = OLNK_FRAME_CMD_INIT;
    in_f.sequence_number = 1U;
    in_f.payload.init.client_version = 0x0622U;
    in_f.payload.init.drive_count = 4U;
    EXPECT_EQ_U32(olnk_frame_encode(&in_f, wire, sizeof(wire), &wire_len), OLNK_OK);
    EXPECT_EQ_U32(olnk_frame_decode(wire, wire_len, &out_f), OLNK_OK);
    EXPECT_EQ_U32(out_f.payload.init.client_version, 0x0622U);
    EXPECT_EQ_U32(out_f.payload.init.drive_count, 4U);

    /* 2. CMD_READ_SECTOR */
    memset(&in_f, 0, sizeof(in_f));
    in_f.frame_type = OLNK_FRAME_CMD_READ_SECTOR;
    in_f.sequence_number = 2U;
    in_f.payload.read_sector.drive_index = 1U;
    in_f.payload.read_sector.start_sector = 0x00012345UL;
    in_f.payload.read_sector.sector_count = 1U;
    EXPECT_EQ_U32(olnk_frame_encode(&in_f, wire, sizeof(wire), &wire_len), OLNK_OK);
    EXPECT_EQ_U32(olnk_frame_decode(wire, wire_len, &out_f), OLNK_OK);
    EXPECT_EQ_U32(out_f.payload.read_sector.start_sector, 0x00012345UL);

    /* 3. CMD_WRITE_SECTOR */
    memset(&in_f, 0, sizeof(in_f));
    in_f.frame_type = OLNK_FRAME_CMD_WRITE_SECTOR;
    in_f.sequence_number = 3U;
    in_f.payload.write_sector.drive_index = 0U;
    in_f.payload.write_sector.start_sector = 7U;
    in_f.payload.write_sector.sector_count = 1U;
    in_f.payload.write_sector.data = sector_data;
    in_f.payload.write_sector.data_len = sizeof(sector_data);
    EXPECT_EQ_U32(olnk_frame_encode(&in_f, wire, sizeof(wire), &wire_len), OLNK_OK);
    EXPECT_EQ_U32(olnk_frame_decode(wire, wire_len, &out_f), OLNK_OK);
    EXPECT_EQ_U32(out_f.payload.write_sector.data_len, OLNK_SECTOR_SIZE);
    EXPECT_TRUE(memcmp(out_f.payload.write_sector.data, sector_data, OLNK_SECTOR_SIZE) == 0);

    /* 4. RESP_INIT_ACK */
    memset(&in_f, 0, sizeof(in_f));
    in_f.frame_type = OLNK_FRAME_RESP_INIT_ACK;
    in_f.sequence_number = 4U;
    in_f.payload.init_ack.server_version = 0x0622U;
    in_f.payload.init_ack.drive_count = 3U;
    in_f.payload.init_ack.status = 0U;
    EXPECT_EQ_U32(olnk_frame_encode(&in_f, wire, sizeof(wire), &wire_len), OLNK_OK);
    EXPECT_EQ_U32(olnk_frame_decode(wire, wire_len, &out_f), OLNK_OK);
    EXPECT_EQ_U32(out_f.payload.init_ack.drive_count, 3U);

    /* 5. RESP_READ_DATA */
    memset(&in_f, 0, sizeof(in_f));
    in_f.frame_type = OLNK_FRAME_RESP_READ_DATA;
    in_f.sequence_number = 5U;
    in_f.payload.read_data.drive_index = 0U;
    in_f.payload.read_data.start_sector = 1U;
    in_f.payload.read_data.data = sector_data;
    in_f.payload.read_data.data_len = OLNK_SECTOR_SIZE;
    EXPECT_EQ_U32(olnk_frame_encode(&in_f, wire, sizeof(wire), &wire_len), OLNK_OK);
    EXPECT_EQ_U32(olnk_frame_decode(wire, wire_len, &out_f), OLNK_OK);
    EXPECT_EQ_U32(out_f.payload.read_data.data_len, OLNK_SECTOR_SIZE);
    EXPECT_TRUE(memcmp(out_f.payload.read_data.data, sector_data, OLNK_SECTOR_SIZE) == 0);

    /* 6. RESP_ERROR */
    memset(&in_f, 0, sizeof(in_f));
    in_f.frame_type = OLNK_FRAME_RESP_ERROR;
    in_f.sequence_number = 6U;
    in_f.payload.error.error_code = OLNK_PROTO_ERR_MEDIA_CHANGED;
    in_f.payload.error.detail = 0x0001U;
    EXPECT_EQ_U32(olnk_frame_encode(&in_f, wire, sizeof(wire), &wire_len), OLNK_OK);
    EXPECT_EQ_U32(olnk_frame_decode(wire, wire_len, &out_f), OLNK_OK);
    EXPECT_EQ_U32(out_f.payload.error.error_code, OLNK_PROTO_ERR_MEDIA_CHANGED);
}

static void test_streaming_sync_and_noise_recovery(void) {
    static uint8_t wire[64];
    static uint8_t stream[128];
    size_t wire_len = 0U;
    size_t i;
    size_t pos = 0U;
    olnk_transport_t tr;
    olnk_ctx_t ctx;
    test_sink_t sink;
    olnk_frame_t frame;

    memset(&tr, 0, sizeof(tr));
    memset(&sink, 0, sizeof(sink));
    memset(&frame, 0, sizeof(frame));

    frame.frame_type = OLNK_FRAME_CMD_INIT;
    frame.sequence_number = 42U;
    frame.payload.init.client_version = 0x0600U;
    frame.payload.init.drive_count = 2U;
    EXPECT_EQ_U32(olnk_frame_encode(&frame, wire, sizeof(wire), &wire_len), OLNK_OK);

    /* Prefix with line noise including overlapping 0x55, 0x55 before 0x55 0xAA */
    stream[pos++] = 0x00U;
    stream[pos++] = 0xFFU;
    stream[pos++] = 0x55U;
    stream[pos++] = 0x12U;
    stream[pos++] = 0x55U;
    memcpy(&stream[pos], wire, wire_len);
    pos += wire_len;

    EXPECT_EQ_U32(olnk_init(&ctx, tr), OLNK_OK);
    olnk_set_frame_handler(&ctx, sink_cb, &sink);

    /* Feed byte-by-byte to stress state machine transitions */
    for (i = 0U; i < pos; ++i) {
        size_t consumed = 0U;
        olnk_status_t st = olnk_feed_bytes(&ctx, &stream[i], 1U, &consumed);
        EXPECT_EQ_U32(consumed, 1U);
        if (i + 1U < pos) {
            EXPECT_EQ_U32(st, OLNK_ERR_NEED_DATA);
        } else {
            EXPECT_EQ_U32(st, OLNK_OK);
        }
    }

    EXPECT_TRUE(sink.received);
    EXPECT_EQ_U32(sink.last_frame.sequence_number, 42U);
    EXPECT_EQ_U32(ctx.frames_rx_ok, 1U);
    EXPECT_EQ_U32(ctx.frames_rx_err, 0U);
}

static void test_fault_injection_bitflip_and_overflow(void) {
    static uint8_t wire[64];
    static uint8_t bad_wire[64];
    size_t wire_len = 0U;
    size_t consumed = 0U;
    olnk_transport_t tr;
    olnk_ctx_t ctx;
    test_sink_t sink;
    olnk_frame_t frame;

    memset(&tr, 0, sizeof(tr));
    memset(&sink, 0, sizeof(sink));
    memset(&frame, 0, sizeof(frame));

    frame.frame_type = OLNK_FRAME_CMD_READ_SECTOR;
    frame.sequence_number = 9U;
    frame.payload.read_sector.drive_index = 0U;
    frame.payload.read_sector.start_sector = 100U;
    frame.payload.read_sector.sector_count = 1U;
    EXPECT_EQ_U32(olnk_frame_encode(&frame, wire, sizeof(wire), &wire_len), OLNK_OK);

    /* 1. Single-bit flip in payload triggers CHECKSUM_MISMATCH */
    memcpy(bad_wire, wire, wire_len);
    bad_wire[7] ^= 0x04U;

    EXPECT_EQ_U32(olnk_init(&ctx, tr), OLNK_OK);
    olnk_set_frame_handler(&ctx, sink_cb, &sink);
    EXPECT_EQ_U32(olnk_feed_bytes(&ctx, bad_wire, wire_len, &consumed), OLNK_ERR_CHECKSUM_MISMATCH);
    EXPECT_EQ_U32(ctx.frames_rx_err, 1U);
    EXPECT_TRUE(!sink.received);

    /* 2. Oversized payload length triggers BUFFER_OVERFLOW and recovers */
    bad_wire[0] = OLNK_SYNC_BYTE_0;
    bad_wire[1] = OLNK_SYNC_BYTE_1;
    bad_wire[2] = OLNK_FRAME_CMD_READ_SECTOR;
    bad_wire[3] = 10U;
    olnk_write_u16_le(&bad_wire[4], (uint16_t)(OLNK_MAX_PAYLOAD_SIZE + 100U));
    EXPECT_EQ_U32(olnk_feed_bytes(&ctx, bad_wire, 6U, &consumed), OLNK_ERR_BUFFER_OVERFLOW);
    EXPECT_EQ_U32(ctx.frames_rx_err, 2U);

    /* 3. Subsequent clean frame is accepted after error recovery */
    EXPECT_EQ_U32(olnk_feed_bytes(&ctx, wire, wire_len, &consumed), OLNK_OK);
    EXPECT_TRUE(sink.received);
    EXPECT_EQ_U32(ctx.frames_rx_ok, 1U);
    EXPECT_EQ_U32(sink.last_frame.payload.read_sector.start_sector, 100U);
}

static void test_storage_mem_backend(void) {
    static uint8_t disk[OLNK_SECTOR_SIZE * 2U];
    static uint8_t wr[OLNK_SECTOR_SIZE];
    static uint8_t rd[OLNK_SECTOR_SIZE];
    olnk_mem_drive_t drv;
    olnk_storage_ops_t ops;

    memset(disk, 0, sizeof(disk));
    memset(wr, 0x5CU, sizeof(wr));
    memset(rd, 0, sizeof(rd));

    drv.data = disk;
    drv.total_sectors = 2U;
    drv.write_protected = false;
    drv.media_changed_flag = false;
    ops = olnk_mem_drive_ops(&drv);

    EXPECT_EQ_U32(ops.write_sectors(0U, 1U, 1U, wr, ops.user_data), OLNK_PROTO_ERR_NONE);
    EXPECT_EQ_U32(ops.read_sectors(0U, 1U, 1U, rd, ops.user_data), OLNK_PROTO_ERR_NONE);
    EXPECT_TRUE(memcmp(wr, rd, OLNK_SECTOR_SIZE) == 0);

    /* Media change error (return code 0x01) */
    drv.media_changed_flag = true;
    EXPECT_TRUE(ops.media_changed(0U, ops.user_data));
    EXPECT_EQ_U32(ops.read_sectors(0U, 0U, 1U, rd, ops.user_data), OLNK_PROTO_ERR_MEDIA_CHANGED);

    /* Write-protect error */
    drv.write_protected = true;
    EXPECT_EQ_U32(ops.write_sectors(0U, 0U, 1U, wr, ops.user_data), OLNK_PROTO_ERR_WRITE_PROTECTED);

    /* Out-of-range sector */
    EXPECT_EQ_U32(ops.read_sectors(0U, 2U, 1U, rd, ops.user_data), OLNK_PROTO_ERR_SECTOR_RANGE);
}

int main(void) {
    test_crc16_standard_vector();
    test_encode_decode_roundtrip_all_types();
    test_streaming_sync_and_noise_recovery();
    test_fault_injection_bitflip_and_overflow();
    test_storage_mem_backend();

    if (g_failures == 0) {
        printf("All libopenlnk C99 tests PASSED.\n");
        return 0;
    }
    fprintf(stderr, "%d test assertion(s) FAILED.\n", g_failures);
    return 1;
}
