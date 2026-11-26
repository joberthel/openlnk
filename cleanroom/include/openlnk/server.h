/* cleanroom/include/openlnk/server.h
 *
 * Platform-independent, zero-allocation, non-blocking MS-DOS 6.22 INTERSVR
 * clean-room server engine built on libopenlnk.
 *
 * Runs identically on POSIX/Windows desktop (opensvr), 16-bit Real-Mode DOS
 * (OPENSVR.EXE), and ESP32 (FreeRTOS UART + SDMMC).
 *
 * Strictly ISO C99, zero dynamic memory allocation, zero floating-point.
 */
#ifndef OPENLNK_SERVER_H
#define OPENLNK_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openlnk/openlnk.h"
#include "openlnk/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OLNK_SRV_SYNC_WAIT_AA = 0U,
    OLNK_SRV_SYNC_WAIT_55,
    OLNK_SRV_SYNC_WAIT_5A,
    OLNK_SRV_IDLE_RX,
    OLNK_SRV_BAUD_WAIT_IDX,
    OLNK_SRV_BAUD_WAIT_BEACON,
    OLNK_SRV_BAUD_TEST_PAIR,
    OLNK_SRV_RX_LEN_LO,
    OLNK_SRV_RX_LEN_HI,
    OLNK_SRV_RX_PAYLOAD,
    OLNK_SRV_RX_CRC_HI,
    OLNK_SRV_RX_CRC_LO,
    OLNK_SRV_RX_ERROR_DRAIN,
    OLNK_SRV_WAIT_TURNAROUND,
    OLNK_SRV_TX_WAIT_HDR_ACK,
    OLNK_SRV_TX_ECHO_BYTE,
    OLNK_SRV_TX_WAIT_FINAL_ACK
} olnk_server_state_t;

typedef enum {
    OLNK_REQ_PHASE_CMD = 0U,
    OLNK_REQ_PHASE_SYNC_BODY,
    OLNK_REQ_PHASE_WRITE_DATA,
    OLNK_REQ_PHASE_LPT_DATA,
    OLNK_REQ_PHASE_IOCTL_MID
} olnk_req_phase_t;

typedef enum {
    OLNK_RESP_NONE = 0U,
    OLNK_RESP_SINGLE,
    OLNK_RESP_READ_HDR_THEN_DATA,
    OLNK_RESP_IOCTL_HDR_THEN_MID
} olnk_resp_phase_t;

typedef void (*olnk_server_event_cb_t)(
    const char *event_name,
    uint32_t arg1,
    uint32_t arg2,
    void *user_data
);

typedef struct {
    olnk_ctx_t link;
    olnk_storage_ops_t storage;

    /* Server configuration */
    uint8_t server_flags;
    uint32_t idle_disconnect_ms; /* 0 = disabled; >0 drops >9600 bps idle links to 9600 */

    /* Event hook for verbose diagnostics (optional) */
    olnk_server_event_cb_t on_event;
    void *on_event_user_data;

    /* Non-blocking link/RPC state */
    uint8_t srv_state;
    uint8_t req_phase;
    uint8_t resp_phase;
    uint32_t state_elapsed_ms;
    uint32_t idle_elapsed_ms;
    uint8_t idle_garbage_count;

    /* Phase B baud negotiation state */
    uint8_t baud_prev_idx;
    uint8_t baud_target_idx;
    uint8_t baud_beacon;
    uint8_t baud_beacon_sent;
    uint8_t baud_test_idx;

    /* Phase C active RX frame state */
    uint8_t rx_hdr;
    bool rx_is_short;
    bool rx_more_frags;
    bool rx_echo_mode;
    uint8_t rx_len_lo;
    uint16_t rx_payload_len;
    uint16_t rx_pos;
    uint16_t rx_crc;

    /* Phase C active TX frame state */
    uint8_t tx_ctrl_buf[256];
    uint8_t tx_data_buf[OLNK_MAX_PAYLOAD_SIZE];
    const uint8_t *tx_payload_ptr;
    uint16_t tx_payload_len;
    bool tx_more_frags;
    uint8_t tx_hdr;
    uint8_t tx_len_bytes[2];
    uint8_t tx_len_sz;
    uint8_t tx_crc_be[2];
    bool tx_echo_mode;
    bool tx_echo_request;
    uint8_t tx_retries;
    uint16_t tx_stream_pos;
    uint16_t tx_stream_total;
    uint8_t tx_last_sent_byte;

    /* Streaming Read (0x04) state */
    uint8_t read_unit;
    uint32_t read_next_lba;
    uint16_t read_sectors_remaining;
    uint16_t read_bps;
    uint16_t read_prefetched_len;
    bool read_prefetched_valid;

    /* Streaming Write (0x05/0x06) state */
    uint8_t write_unit;
    bool write_verify;
    uint32_t write_next_lba;
    uint16_t write_bps;
    uint32_t write_bytes_expected;
    uint32_t write_bytes_received;
    uint16_t write_sectors_written;
    uint16_t write_error_status;
    uint8_t write_spill[OLNK_SECTOR_SIZE];
    uint16_t write_spill_len;

    /* Pending LPT write / Generic IOCTL state */
    uint16_t lpt_bytes_expected;
    uint16_t lpt_bytes_received;
    uint8_t ioctl_unit;

    /* Telemetry counters */
    uint32_t rpc_count;
    uint32_t sectors_read_total;
    uint32_t sectors_written_total;
} olnk_server_t;

/**
 * Initialize caller-allocated server engine (keep static on 16-bit DOS!).
 */
olnk_status_t olnk_server_init(
    olnk_server_t *srv,
    olnk_transport_t transport,
    olnk_storage_ops_t storage
);

/**
 * Optional diagnostic event callback (sync, baud, rpc, disconnect).
 */
void olnk_server_set_event_callback(
    olnk_server_t *srv,
    olnk_server_event_cb_t cb,
    void *user_data
);

/**
 * Reset server back to Phase A Sync wait at 9600 bps (preserves session IDs
 * so MS-DOS INTERLNK can transparently resume the session on reconnect).
 */
void olnk_server_drop_link(olnk_server_t *srv);

/**
 * Feed one or more raw bytes received from the serial/pipe transport into
 * the server state machine. Replies are emitted immediately via transport.write.
 */
void olnk_server_feed_bytes(
    olnk_server_t *srv,
    const uint8_t *data,
    size_t len
);

/**
 * Advance internal protocol timers by elapsed_ms (handles baud beacon retries,
 * error silent-drain timeouts, TX ACK retries, and optional idle disconnect).
 */
void olnk_server_tick(olnk_server_t *srv, uint32_t elapsed_ms);

/**
 * Convenience single-step poll for main loops (reads available bytes with
 * timeout_ms, feeds them into the state machine, and advances timers on idle).
 * Returns OLNK_OK on activity/timeout, or OLNK_ERR_HAL_IO on fatal transport error.
 */
olnk_status_t olnk_server_poll(olnk_server_t *srv, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_SERVER_H */
