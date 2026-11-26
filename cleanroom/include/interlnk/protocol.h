/* cleanroom/include/interlnk/protocol.h */
#ifndef INTERLNK_PROTOCOL_H
#define INTERLNK_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ILNK_SYNC_BYTE_0      0x55U
#define ILNK_SYNC_BYTE_1      0xAAU
#define ILNK_HEADER_SIZE      6U
#define ILNK_CHECKSUM_SIZE    2U
#define ILNK_FRAME_OVERHEAD   (ILNK_HEADER_SIZE + ILNK_CHECKSUM_SIZE)
#define ILNK_RX_BUFFER_SIZE   1024U
#define ILNK_MAX_PAYLOAD_SIZE (ILNK_RX_BUFFER_SIZE - ILNK_FRAME_OVERHEAD)
#define ILNK_SECTOR_SIZE      512U

typedef enum {
    ILNK_OK = 0,
    ILNK_ERR_NEED_DATA,
    ILNK_ERR_INVALID_FRAME,
    ILNK_ERR_CHECKSUM_MISMATCH,
    ILNK_ERR_BUFFER_OVERFLOW,
    ILNK_ERR_HAL_TIMEOUT,
    ILNK_ERR_INVALID_ARG
} ilnk_status_t;

typedef enum {
    ILNK_FRAME_CMD_INIT         = 0x01U,
    ILNK_FRAME_CMD_READ_SECTOR  = 0x02U,
    ILNK_FRAME_CMD_WRITE_SECTOR = 0x03U,
    ILNK_FRAME_RESP_INIT_ACK    = 0x81U,
    ILNK_FRAME_RESP_READ_DATA   = 0x82U,
    ILNK_FRAME_RESP_ERROR       = 0xFFU
} ilnk_frame_type_t;

typedef enum {
    ILNK_PROTO_ERR_NONE            = 0x00U,
    ILNK_PROTO_ERR_MEDIA_CHANGED   = 0x01U,
    ILNK_PROTO_ERR_WRITE_PROTECTED = 0x02U,
    ILNK_PROTO_ERR_INVALID_DRIVE   = 0x03U,
    ILNK_PROTO_ERR_SECTOR_RANGE    = 0x04U,
    ILNK_PROTO_ERR_IO_FAILURE      = 0x05U,
    ILNK_PROTO_ERR_UNSUPPORTED_CMD = 0x06U
} ilnk_proto_error_t;

typedef struct {
    uint16_t client_version;
    uint8_t drive_count;
} ilnk_payload_init_t;

typedef struct {
    uint8_t drive_index;
    uint32_t start_sector;
    uint16_t sector_count;
} ilnk_payload_read_t;

typedef struct {
    uint8_t drive_index;
    uint32_t start_sector;
    uint16_t sector_count;
    const uint8_t *data;
    size_t data_len;
} ilnk_payload_write_t;

typedef struct {
    uint16_t server_version;
    uint8_t drive_count;
    uint8_t status;
} ilnk_payload_init_ack_t;

typedef struct {
    uint8_t drive_index;
    uint32_t start_sector;
    const uint8_t *data;
    size_t data_len;
} ilnk_payload_read_resp_t;

typedef struct {
    uint8_t error_code;
    uint16_t detail;
} ilnk_payload_error_t;

typedef struct {
    uint8_t frame_type;
    uint8_t sequence_number;
    uint16_t payload_length;
    uint16_t checksum;
    union {
        ilnk_payload_init_t init;
        ilnk_payload_read_t read_sector;
        ilnk_payload_write_t write_sector;
        ilnk_payload_init_ack_t init_ack;
        ilnk_payload_read_resp_t read_data;
        ilnk_payload_error_t error;
    } payload;
} ilnk_frame_t;

/* Little-endian helpers (16-bit safe, alignment-independent) */
uint16_t ilnk_read_u16_le(const uint8_t *buf);
uint32_t ilnk_read_u32_le(const uint8_t *buf);
void ilnk_write_u16_le(uint8_t *buf, uint16_t value);
void ilnk_write_u32_le(uint8_t *buf, uint32_t value);

/**
 * Encode a structured frame into a caller-supplied raw byte buffer.
 * Zero dynamic allocation.
 */
ilnk_status_t ilnk_frame_encode(
    const ilnk_frame_t *frame,
    uint8_t *out_buf,
    size_t out_cap,
    size_t *out_len
);

/**
 * Validate sync bytes, payload bounds, and CRC16, then decode a raw frame.
 * Data pointers inside out_frame borrow from raw (caller retains ownership).
 */
ilnk_status_t ilnk_frame_decode(
    const uint8_t *raw,
    size_t raw_len,
    ilnk_frame_t *out_frame
);

#ifdef __cplusplus
}
#endif

#endif /* INTERLNK_PROTOCOL_H */
