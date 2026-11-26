/* cleanroom/include/openlnk/protocol.h */
#ifndef OPENLNK_PROTOCOL_H
#define OPENLNK_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLNK_SYNC_BYTE_0      0x55U
#define OLNK_SYNC_BYTE_1      0xAAU
#define OLNK_HEADER_SIZE      6U
#define OLNK_CHECKSUM_SIZE    2U
#define OLNK_FRAME_OVERHEAD   (OLNK_HEADER_SIZE + OLNK_CHECKSUM_SIZE)
#define OLNK_RX_BUFFER_SIZE   1024U
#define OLNK_MAX_PAYLOAD_SIZE (OLNK_RX_BUFFER_SIZE - OLNK_FRAME_OVERHEAD)
#define OLNK_SECTOR_SIZE      512U

typedef enum {
    OLNK_OK = 0,
    OLNK_ERR_NEED_DATA,
    OLNK_ERR_INVALID_FRAME,
    OLNK_ERR_CHECKSUM_MISMATCH,
    OLNK_ERR_BUFFER_OVERFLOW,
    OLNK_ERR_HAL_TIMEOUT,
    OLNK_ERR_INVALID_ARG
} olnk_status_t;

typedef enum {
    OLNK_FRAME_CMD_INIT         = 0x01U,
    OLNK_FRAME_CMD_READ_SECTOR  = 0x02U,
    OLNK_FRAME_CMD_WRITE_SECTOR = 0x03U,
    OLNK_FRAME_RESP_INIT_ACK    = 0x81U,
    OLNK_FRAME_RESP_READ_DATA   = 0x82U,
    OLNK_FRAME_RESP_ERROR       = 0xFFU
} olnk_frame_type_t;

typedef enum {
    OLNK_PROTO_ERR_NONE            = 0x00U,
    OLNK_PROTO_ERR_MEDIA_CHANGED   = 0x01U,
    OLNK_PROTO_ERR_WRITE_PROTECTED = 0x02U,
    OLNK_PROTO_ERR_INVALID_DRIVE   = 0x03U,
    OLNK_PROTO_ERR_SECTOR_RANGE    = 0x04U,
    OLNK_PROTO_ERR_IO_FAILURE      = 0x05U,
    OLNK_PROTO_ERR_UNSUPPORTED_CMD = 0x06U
} olnk_proto_error_t;

typedef struct {
    uint16_t client_version;
    uint8_t drive_count;
} olnk_payload_init_t;

typedef struct {
    uint8_t drive_index;
    uint32_t start_sector;
    uint16_t sector_count;
} olnk_payload_read_t;

typedef struct {
    uint8_t drive_index;
    uint32_t start_sector;
    uint16_t sector_count;
    const uint8_t *data;
    size_t data_len;
} olnk_payload_write_t;

typedef struct {
    uint16_t server_version;
    uint8_t drive_count;
    uint8_t status;
} olnk_payload_init_ack_t;

typedef struct {
    uint8_t drive_index;
    uint32_t start_sector;
    const uint8_t *data;
    size_t data_len;
} olnk_payload_read_resp_t;

typedef struct {
    uint8_t error_code;
    uint16_t detail;
} olnk_payload_error_t;

typedef struct {
    uint8_t frame_type;
    uint8_t sequence_number;
    uint16_t payload_length;
    uint16_t checksum;
    union {
        olnk_payload_init_t init;
        olnk_payload_read_t read_sector;
        olnk_payload_write_t write_sector;
        olnk_payload_init_ack_t init_ack;
        olnk_payload_read_resp_t read_data;
        olnk_payload_error_t error;
    } payload;
} olnk_frame_t;

/* Little-endian helpers (16-bit safe, alignment-independent) */
uint16_t olnk_read_u16_le(const uint8_t *buf);
uint32_t olnk_read_u32_le(const uint8_t *buf);
void olnk_write_u16_le(uint8_t *buf, uint16_t value);
void olnk_write_u32_le(uint8_t *buf, uint32_t value);

/**
 * Encode a structured frame into a caller-supplied raw byte buffer.
 * Zero dynamic allocation.
 */
olnk_status_t olnk_frame_encode(
    const olnk_frame_t *frame,
    uint8_t *out_buf,
    size_t out_cap,
    size_t *out_len
);

/**
 * Validate sync bytes, payload bounds, and CRC16, then decode a raw frame.
 * Data pointers inside out_frame borrow from raw (caller retains ownership).
 */
olnk_status_t olnk_frame_decode(
    const uint8_t *raw,
    size_t raw_len,
    olnk_frame_t *out_frame
);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_PROTOCOL_H */
