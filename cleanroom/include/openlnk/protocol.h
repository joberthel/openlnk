/* cleanroom/include/openlnk/protocol.h
 *
 * Clean-room wire-frame and RPC codec definitions for the verified
 * MS-DOS 6.22 INTERLNK / INTERSVR serial protocol (specs/interlnk_wire.ksy).
 *
 * Strictly ISO C99, zero dynamic allocation, zero floating-point, 16-bit safe.
 */
#ifndef OPENLNK_PROTOCOL_H
#define OPENLNK_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Protocol Sizing & Wire Constants
 * ========================================================================= */

#ifndef OLNK_MAX_PAYLOAD_SIZE
#define OLNK_MAX_PAYLOAD_SIZE 8192U
#endif

#define OLNK_FRAME_SHORT_OVERHEAD 4U /* header(1) + len_short(1) + crc16_be(2) */
#define OLNK_FRAME_MAX_OVERHEAD   5U /* header(1) + len_long(2)  + crc16_be(2) */
#define OLNK_FRAME_BUFFER_SIZE    (OLNK_MAX_PAYLOAD_SIZE + OLNK_FRAME_MAX_OVERHEAD)
#define OLNK_SECTOR_SIZE          512U
#define OLNK_MAX_DRIVES           26U
#define OLNK_MAX_LPTS             3U

/* Phase A — 3-step Initial Sync Handshake (9600 bps) */
#define OLNK_SYNC_C1 0xAAU
#define OLNK_SYNC_S1 0x00U
#define OLNK_SYNC_C2 0x55U
#define OLNK_SYNC_S2 0xFFU
#define OLNK_SYNC_C3 0x5AU
#define OLNK_SYNC_S3 0x11U

/* Phase B — Baud Negotiation Line Quality Test */
#define OLNK_BAUD_TEST_LEN     15U
#define OLNK_BAUD_INDEX_COUNT  8U

/* Phase C — Link-Layer Header Bitfields */
#define OLNK_HDR_SHORT_LEN      0x80U
#define OLNK_HDR_MORE_FRAGMENTS 0x40U
#define OLNK_HDR_RESERVED_MASK  0x38U
#define OLNK_HDR_ECHO_REQUEST   0x04U
#define OLNK_HDR_SEQ_MASK       0x03U

/* Timing constants derived from PC BIOS 18.2 Hz timer (~54.925 ms/tick) */
#define OLNK_BIOS_TICK_MS           55U
#define OLNK_TIMEOUT_SYNC_MS        275U  /* 5 ticks */
#define OLNK_TIMEOUT_RX_BYTE_MS     275U  /* 5 ticks */
#define OLNK_TIMEOUT_TX_ACK_MS      550U  /* 10 ticks */
#define OLNK_TIMEOUT_BEACON_TICK_MS 55U   /* 1 tick per beacon retry */
#define OLNK_BEACON_MAX_RETRIES     5U    /* 5 ticks total */
#define OLNK_KEEPALIVE_MAX_MISSED   10U   /* >10 consecutive misses => drop */

/* Protocol Version & Mapping Constants */
#define OLNK_CLIENT_VERSION_DOS622 0x0100U
#define OLNK_SERVER_VERSION_DOS622 0x010AU
#define OLNK_MAP_PROTO_VER         0x0001U
#define OLNK_SERVER_PROTO_MAJOR    0x01U
#define OLNK_SERVER_PROTO_MINOR    0x0AU
#define OLNK_DOS_VER_MAJOR         6U
#define OLNK_DOS_VER_MINOR         22U
#define OLNK_MAP_AUTO              0xFEU
#define OLNK_MAP_NONE              0xFFU
#define OLNK_MAP_READONLY_BIT      0x80U
#define OLNK_DRIVE_ATTR_DEFAULT    0x08C2U /* Open/Close/RM | Generic IOCTL | 32-bit LBA */

/* MS-DOS Device Driver Status Word Flags */
#define OLNK_DOS_STATUS_DONE  0x0100U
#define OLNK_DOS_STATUS_BUSY  0x0200U
#define OLNK_DOS_STATUS_ERROR 0x8000U
#define OLNK_DOS_MAKE_ERROR(code) \
    ((uint16_t)(OLNK_DOS_STATUS_ERROR | OLNK_DOS_STATUS_DONE | ((uint16_t)(code) & 0x00FFU)))

/* Generic IOCTL Constants (MS-DOS Block Driver Cmd 0x13) */
#define OLNK_IOCTL_CAT_DISK         0x08U
#define OLNK_IOCTL_FN_SET_MEDIA_ID  0x46U
#define OLNK_IOCTL_FN_GET_MEDIA_ID  0x66U

/* Fixed RPC Payload Lengths */
#define OLNK_LEN_SYNC_TRIGGER       2U
#define OLNK_LEN_SESSION_SYNC_REQ   34U
#define OLNK_LEN_SESSION_SYNC_RESP  33U
#define OLNK_LEN_SESSION_SYNC_EXT   161U
#define OLNK_LEN_MAP_CONFIG_REQ     36U
#define OLNK_LEN_MAP_CONFIG_RESP    87U
#define OLNK_LEN_MEDIA_CHECK_REQ    3U
#define OLNK_LEN_MEDIA_CHECK_RESP   19U
#define OLNK_LEN_BUILD_BPB_REQ      3U
#define OLNK_LEN_BUILD_BPB_RESP_ERR 2U
#define OLNK_LEN_BUILD_BPB_RESP_OK  33U
#define OLNK_LEN_DOS_BPB            25U
#define OLNK_LEN_SECTOR_IO_REQ      11U
#define OLNK_LEN_SECTOR_IO_RESP     4U
#define OLNK_LEN_SECTOR_IO_RESP_CHG 20U
#define OLNK_LEN_DRIVE_INCOMPAT     5U
#define OLNK_LEN_LPT_WRITE_REQ      5U
#define OLNK_LEN_LPT_WRITE_RESP     4U
#define OLNK_LEN_UNIT_CTRL_REQ      2U
#define OLNK_LEN_STATUS_ONLY_RESP   2U
#define OLNK_LEN_BIOS_PRN_WRITE_REQ 3U
#define OLNK_LEN_BIOS_PRN_CTRL_REQ  2U
#define OLNK_LEN_BIOS_PRN_RESP      1U
#define OLNK_LEN_GET_DRIVE_INFO_REQ 2U
#define OLNK_LEN_GET_DRIVE_INFO_RSP 23U
#define OLNK_LEN_GENERIC_IOCTL_REQ  4U
#define OLNK_LEN_GENERIC_IOCTL_MID  25U

/* =========================================================================
 * Enumerations
 * ========================================================================= */

typedef enum {
    OLNK_OK = 0,
    OLNK_ERR_NEED_DATA,
    OLNK_ERR_INVALID_FRAME,
    OLNK_ERR_CHECKSUM_MISMATCH,
    OLNK_ERR_BUFFER_OVERFLOW,
    OLNK_ERR_HAL_TIMEOUT,
    OLNK_ERR_HAL_IO,
    OLNK_ERR_INVALID_ARG,
    OLNK_ERR_SEQ_MISMATCH,
    OLNK_ERR_DISCONNECTED,
    OLNK_ERR_PROTOCOL
} olnk_status_t;

typedef enum {
    OLNK_BAUD_1200   = 0U,
    OLNK_BAUD_2400   = 1U,
    OLNK_BAUD_4800   = 2U,
    OLNK_BAUD_9600   = 3U,
    OLNK_BAUD_19200  = 4U,
    OLNK_BAUD_38400  = 5U,
    OLNK_BAUD_57600  = 6U,
    OLNK_BAUD_115200 = 7U
} olnk_baud_index_t;

typedef enum {
    OLNK_OP_SESSION_SYNC         = 0x00U,
    OLNK_OP_MAP_CONFIG           = 0x01U,
    OLNK_OP_MEDIA_CHECK          = 0x02U,
    OLNK_OP_BUILD_BPB            = 0x03U,
    OLNK_OP_READ_SECTORS         = 0x04U,
    OLNK_OP_WRITE_SECTORS        = 0x05U,
    OLNK_OP_WRITE_VERIFY         = 0x06U,
    OLNK_OP_DRIVE_INCOMPAT       = 0x07U,
    OLNK_OP_LPT_WRITE            = 0x0AU,
    OLNK_OP_LPT_WRITE_VERIFY     = 0x0BU,
    OLNK_OP_LPT_OUTPUT_STATUS    = 0x0EU,
    OLNK_OP_LPT_OUTPUT_FLUSH     = 0x10U,
    OLNK_OP_LPT_DEVICE_OPEN      = 0x11U,
    OLNK_OP_LPT_DEVICE_CLOSE     = 0x12U,
    OLNK_OP_LPT_AUX_WRITE        = 0x14U,
    OLNK_OP_LPT_OUTPUT_BUSY      = 0x15U,
    OLNK_OP_BLK_DEVICE_OPEN      = 0x16U,
    OLNK_OP_BLK_DEVICE_CLOSE     = 0x17U,
    OLNK_OP_BLK_DEVICE_CLOSE_ALT = 0x18U,
    OLNK_OP_BIOS_PRN_WRITE       = 0x19U,
    OLNK_OP_BIOS_PRN_INIT        = 0x1AU,
    OLNK_OP_BIOS_PRN_STATUS      = 0x1BU,
    OLNK_OP_GET_DRIVE_INFO       = 0x1CU,
    OLNK_OP_GENERIC_IOCTL        = 0x1DU
} olnk_opcode_t;

typedef enum {
    OLNK_MEDIA_UNSURE    = 0x00U,
    OLNK_MEDIA_UNCHANGED = 0x01U,
    OLNK_MEDIA_CHANGED   = 0xFFU
} olnk_media_status_t;

typedef enum {
    OLNK_DOS_ERR_WRITE_PROTECT    = 0x00U,
    OLNK_DOS_ERR_UNKNOWN_UNIT     = 0x01U,
    OLNK_DOS_ERR_DRIVE_NOT_READY  = 0x02U,
    OLNK_DOS_ERR_UNKNOWN_COMMAND  = 0x03U,
    OLNK_DOS_ERR_CRC_ERROR        = 0x04U,
    OLNK_DOS_ERR_BAD_REQ_LENGTH   = 0x05U,
    OLNK_DOS_ERR_SEEK_ERROR       = 0x06U,
    OLNK_DOS_ERR_UNKNOWN_MEDIA    = 0x07U,
    OLNK_DOS_ERR_SECTOR_NOT_FOUND = 0x08U,
    OLNK_DOS_ERR_PRINTER_NO_PAPER = 0x09U,
    OLNK_DOS_ERR_WRITE_FAULT      = 0x0AU,
    OLNK_DOS_ERR_READ_FAULT       = 0x0BU,
    OLNK_DOS_ERR_GENERAL_FAILURE  = 0x0CU,
    OLNK_DOS_ERR_INVALID_DISK_CHG = 0x0FU
} olnk_dos_error_t;

/* =========================================================================
 * Phase B 15-Pair Line Quality Test Vectors
 * ========================================================================= */

extern const uint8_t olnk_baud_test_tx[OLNK_BAUD_TEST_LEN];
extern const uint8_t olnk_baud_test_rx[OLNK_BAUD_TEST_LEN];

/* =========================================================================
 * Canonical Link-Layer Frame Structure
 * ========================================================================= */

typedef struct {
    uint8_t header;
    bool is_short_len;
    bool more_fragments;
    bool echo_request;
    uint8_t sequence_number;
    uint16_t payload_length;
    const uint8_t *payload; /* Borrows from source buffer */
    uint16_t crc16;
} olnk_wire_frame_t;

/* =========================================================================
 * Application-Layer RPC Structures
 * ========================================================================= */

typedef struct {
    uint8_t opcode;
    uint8_t subcode;
} olnk_sync_trigger_t;

typedef struct {
    uint16_t client_version;
    uint8_t proto_major;
    uint8_t ext_info_flag;
    uint8_t capability;
    uint16_t max_block_size;
    uint32_t client_id;
    uint32_t server_id;
} olnk_session_sync_req_t;

typedef struct {
    uint16_t server_version;
    uint8_t ready_flag;
    uint8_t capability;
    uint16_t max_block_size;
    uint32_t prev_client_id;
    uint32_t server_id;
} olnk_session_sync_resp_t;

typedef struct {
    uint8_t opcode;
    uint16_t client_proto_ver;
    uint8_t client_dos_minor;
    uint8_t client_dos_major;
    uint8_t max_client_drives;
    uint8_t first_client_drive;
    uint8_t lpt_map_req[OLNK_MAX_LPTS];
    uint8_t drive_map_req[OLNK_MAX_DRIVES];
} olnk_map_config_req_t;

typedef struct {
    uint8_t server_proto_major;
    uint8_t server_proto_minor;
    uint8_t server_dos_minor;
    uint8_t server_dos_major;
    uint8_t server_drive_count;
    uint8_t lpt_map_resp[OLNK_MAX_LPTS];
    uint8_t drive_map_resp[OLNK_MAX_DRIVES];
    uint16_t server_drive_attrs[OLNK_MAX_DRIVES];
    uint8_t server_flags;
} olnk_map_config_resp_t;

typedef struct {
    uint8_t opcode;
    uint8_t unit;
    uint8_t media_descriptor;
} olnk_media_check_req_t;

typedef struct {
    uint16_t status;
    uint8_t media_status;
    char volume_label[12]; /* Null-terminated ASCII, up to 11 chars + NUL */
    uint32_t volume_serial;
} olnk_media_check_resp_t;

typedef struct {
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t media_descriptor;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
} olnk_dos_bpb_t;

typedef struct {
    uint8_t opcode;
    uint8_t unit;
    uint8_t media_descriptor;
} olnk_build_bpb_req_t;

typedef struct {
    uint16_t status;
    bool has_bpb;
    olnk_dos_bpb_t bpb;
    uint8_t bpb_ext[6];
} olnk_build_bpb_resp_t;

typedef struct {
    uint8_t opcode;
    uint8_t unit;
    uint8_t media_descriptor;
    uint16_t sector_count;
    uint16_t start_sector_16;
    uint32_t start_sector_32;
} olnk_sector_io_req_t;

typedef struct {
    uint16_t status;
    uint16_t sectors_transferred;
    bool has_volume_id;
    char volume_label[12];
    uint32_t volume_serial;
} olnk_sector_io_resp_t;

typedef struct {
    uint8_t opcode;
    uint8_t flag;
    uint8_t client_unit;
    uint8_t reason;
    uint8_t server_unit;
} olnk_drive_incompat_t;

typedef struct {
    uint8_t opcode;
    uint8_t lpt_index;
    uint16_t byte_count;
    uint8_t inline_byte;
} olnk_lpt_write_req_t;

typedef struct {
    uint16_t status;
    uint16_t bytes_written;
} olnk_lpt_write_resp_t;

typedef struct {
    uint8_t opcode;
    uint8_t unit;
} olnk_unit_ctrl_req_t;

typedef struct {
    uint16_t status;
} olnk_status_only_resp_t;

typedef struct {
    uint8_t opcode;
    uint8_t unit;
} olnk_get_drive_info_req_t;

typedef struct {
    char size_string[10];
    char volume_label[13];
} olnk_get_drive_info_resp_t;

typedef struct {
    uint8_t opcode;
    uint8_t unit;
    uint8_t category;
    uint8_t function;
} olnk_generic_ioctl_req_t;

typedef struct {
    uint16_t info_level;
    uint32_t serial_number;
    uint8_t volume_label[11];    /* Space-padded 11-byte ASCII */
    uint8_t filesystem_type[8];  /* Space-padded 8-byte ASCII */
} olnk_generic_ioctl_mid_t;

/* =========================================================================
 * Endianness Helpers (16-bit safe, alignment-independent)
 * ========================================================================= */

uint16_t olnk_read_u16_le(const uint8_t *buf);
uint16_t olnk_read_u16_be(const uint8_t *buf);
uint32_t olnk_read_u32_le(const uint8_t *buf);
void olnk_write_u16_le(uint8_t *buf, uint16_t value);
void olnk_write_u16_be(uint8_t *buf, uint16_t value);
void olnk_write_u32_le(uint8_t *buf, uint32_t value);

/* =========================================================================
 * Link-Layer Header, Control Token & Baud Table Helpers
 * ========================================================================= */

uint8_t olnk_hdr_make(bool is_short_len, bool more_fragments, bool echo_request, uint8_t seq);
uint8_t olnk_hdr_compute_ack(uint8_t hdr, bool local_requires_echo);
bool olnk_hdr_verify_ack(uint8_t hdr, uint8_t ack_hdr, bool *out_echo_mode);
uint8_t olnk_keepalive_byte(uint8_t expected_seq);
uint8_t olnk_turnaround_byte(uint8_t next_seq);
uint8_t olnk_baud_req_byte(uint8_t seq);
uint8_t olnk_baud_beacon_byte(uint8_t baud_idx, uint8_t seq);

uint32_t olnk_baud_index_to_bps(uint8_t baud_idx);
uint16_t olnk_baud_index_to_divisor(uint8_t baud_idx);
bool olnk_bps_to_baud_index(uint32_t bps, uint8_t *out_idx);

/* =========================================================================
 * Canonical Wire-Frame Encode / Decode (CRC-16/GENIBUS, Big-Endian)
 * ========================================================================= */

olnk_status_t olnk_wire_frame_encode(
    uint8_t seq,
    bool more_fragments,
    bool echo_request,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out_buf,
    size_t out_cap,
    size_t *out_len
);

olnk_status_t olnk_wire_frame_decode(
    const uint8_t *raw,
    size_t raw_len,
    olnk_wire_frame_t *out_frame,
    size_t *out_consumed
);

/* =========================================================================
 * RPC Message Encoders & Decoders (Zero-Alloc, Little-Endian)
 * ========================================================================= */

olnk_status_t olnk_encode_sync_trigger(const olnk_sync_trigger_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_sync_trigger(const uint8_t *buf, size_t len, olnk_sync_trigger_t *out);

olnk_status_t olnk_encode_session_sync_req(const olnk_session_sync_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_session_sync_req(const uint8_t *buf, size_t len, olnk_session_sync_req_t *out);

olnk_status_t olnk_encode_session_sync_resp(const olnk_session_sync_resp_t *msg, bool ext_info, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_session_sync_resp(const uint8_t *buf, size_t len, olnk_session_sync_resp_t *out);

olnk_status_t olnk_encode_map_config_req(const olnk_map_config_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_map_config_req(const uint8_t *buf, size_t len, olnk_map_config_req_t *out);

olnk_status_t olnk_encode_map_config_resp(const olnk_map_config_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_map_config_resp(const uint8_t *buf, size_t len, olnk_map_config_resp_t *out);

olnk_status_t olnk_encode_media_check_req(const olnk_media_check_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_media_check_req(const uint8_t *buf, size_t len, olnk_media_check_req_t *out);

olnk_status_t olnk_encode_media_check_resp(const olnk_media_check_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_media_check_resp(const uint8_t *buf, size_t len, olnk_media_check_resp_t *out);

olnk_status_t olnk_encode_dos_bpb(const olnk_dos_bpb_t *bpb, uint8_t *buf, size_t cap);
olnk_status_t olnk_decode_dos_bpb(const uint8_t *buf, size_t len, olnk_dos_bpb_t *out);

olnk_status_t olnk_encode_build_bpb_req(const olnk_build_bpb_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_build_bpb_req(const uint8_t *buf, size_t len, olnk_build_bpb_req_t *out);

olnk_status_t olnk_encode_build_bpb_resp(const olnk_build_bpb_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_build_bpb_resp(const uint8_t *buf, size_t len, olnk_build_bpb_resp_t *out);

uint32_t olnk_sector_io_effective_lba(const olnk_sector_io_req_t *req);
olnk_status_t olnk_encode_sector_io_req(const olnk_sector_io_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_sector_io_req(const uint8_t *buf, size_t len, olnk_sector_io_req_t *out);

olnk_status_t olnk_encode_sector_io_resp(const olnk_sector_io_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_sector_io_resp(const uint8_t *buf, size_t len, olnk_sector_io_resp_t *out);

olnk_status_t olnk_encode_drive_incompat(const olnk_drive_incompat_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_drive_incompat(const uint8_t *buf, size_t len, olnk_drive_incompat_t *out);

olnk_status_t olnk_encode_lpt_write_req(const olnk_lpt_write_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_lpt_write_req(const uint8_t *buf, size_t len, olnk_lpt_write_req_t *out);

olnk_status_t olnk_encode_lpt_write_resp(const olnk_lpt_write_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_lpt_write_resp(const uint8_t *buf, size_t len, olnk_lpt_write_resp_t *out);

olnk_status_t olnk_encode_unit_ctrl_req(const olnk_unit_ctrl_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_unit_ctrl_req(const uint8_t *buf, size_t len, olnk_unit_ctrl_req_t *out);

olnk_status_t olnk_encode_status_only_resp(uint16_t status, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_status_only_resp(const uint8_t *buf, size_t len, uint16_t *out_status);

olnk_status_t olnk_encode_get_drive_info_req(const olnk_get_drive_info_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_get_drive_info_req(const uint8_t *buf, size_t len, olnk_get_drive_info_req_t *out);

olnk_status_t olnk_encode_get_drive_info_resp(const olnk_get_drive_info_resp_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_get_drive_info_resp(const uint8_t *buf, size_t len, olnk_get_drive_info_resp_t *out);

olnk_status_t olnk_encode_generic_ioctl_req(const olnk_generic_ioctl_req_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_generic_ioctl_req(const uint8_t *buf, size_t len, olnk_generic_ioctl_req_t *out);

olnk_status_t olnk_encode_generic_ioctl_mid(const olnk_generic_ioctl_mid_t *msg, uint8_t *buf, size_t cap, size_t *out_len);
olnk_status_t olnk_decode_generic_ioctl_mid(const uint8_t *buf, size_t len, olnk_generic_ioctl_mid_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_PROTOCOL_H */
