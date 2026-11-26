/* cleanroom/desktop/vector_check.c
 *
 * Clean-room CLI utility that feeds a hex-encoded wire frame through
 * libinterlnk's streaming state machine and prints the decoded structure
 * as JSON for cross-verification against Kaitai Struct test vectors.
 */
#include "interlnk/interlnk.h"

#include <stdio.h>
#include <string.h>

static int hex_nibble(char c) {
    if ((c >= '0') && (c <= '9')) {
        return (int)(c - '0');
    }
    if ((c >= 'a') && (c <= 'f')) {
        return 10 + (int)(c - 'a');
    }
    if ((c >= 'A') && (c <= 'F')) {
        return 10 + (int)(c - 'A');
    }
    return -1;
}

static int parse_hex(const char *hex, uint8_t *out, size_t out_cap, size_t *out_len) {
    size_t i = 0U;
    size_t n = 0U;

    while (hex[i] != '\0') {
        int hi;
        int lo;
        if ((hex[i] == ' ') || (hex[i] == '\n') || (hex[i] == '\r') || (hex[i] == '\t')) {
            ++i;
            continue;
        }
        hi = hex_nibble(hex[i]);
        if ((hi < 0) || (hex[i + 1U] == '\0')) {
            return -1;
        }
        lo = hex_nibble(hex[i + 1U]);
        if ((lo < 0) || (n >= out_cap)) {
            return -1;
        }
        out[n] = (uint8_t)(((unsigned int)hi << 4U) | (unsigned int)lo);
        n += 1U;
        i += 2U;
    }

    *out_len = n;
    return 0;
}

static const char *status_name(ilnk_status_t st) {
    switch (st) {
        case ILNK_OK: return "OK";
        case ILNK_ERR_NEED_DATA: return "NEED_DATA";
        case ILNK_ERR_INVALID_FRAME: return "INVALID_FRAME";
        case ILNK_ERR_CHECKSUM_MISMATCH: return "CHECKSUM_MISMATCH";
        case ILNK_ERR_BUFFER_OVERFLOW: return "BUFFER_OVERFLOW";
        case ILNK_ERR_HAL_TIMEOUT: return "HAL_TIMEOUT";
        case ILNK_ERR_INVALID_ARG: return "INVALID_ARG";
        default: return "UNKNOWN";
    }
}

static void print_hex_bytes(const uint8_t *buf, size_t len) {
    size_t i;
    putchar('"');
    for (i = 0U; i < len; ++i) {
        printf("%02x", (unsigned int)buf[i]);
    }
    putchar('"');
}

typedef struct {
    bool got_frame;
    ilnk_frame_t frame;
    uint8_t data_copy[ILNK_MAX_PAYLOAD_SIZE];
    size_t data_copy_len;
} capture_t;

static void capture_frame(const ilnk_frame_t *frame, void *user_data) {
    capture_t *cap = (capture_t *)user_data;
    cap->got_frame = true;
    cap->frame = *frame;
    cap->data_copy_len = 0U;

    if ((frame->frame_type == ILNK_FRAME_CMD_WRITE_SECTOR) &&
        (frame->payload.write_sector.data != NULL) &&
        (frame->payload.write_sector.data_len <= sizeof(cap->data_copy))) {
        memcpy(cap->data_copy, frame->payload.write_sector.data, frame->payload.write_sector.data_len);
        cap->data_copy_len = frame->payload.write_sector.data_len;
        cap->frame.payload.write_sector.data = cap->data_copy;
    } else if ((frame->frame_type == ILNK_FRAME_RESP_READ_DATA) &&
               (frame->payload.read_data.data != NULL) &&
               (frame->payload.read_data.data_len <= sizeof(cap->data_copy))) {
        memcpy(cap->data_copy, frame->payload.read_data.data, frame->payload.read_data.data_len);
        cap->data_copy_len = frame->payload.read_data.data_len;
        cap->frame.payload.read_data.data = cap->data_copy;
    }
}

int main(int argc, char **argv) {
    static char input_hex[4096];
    static uint8_t raw[2048];
    size_t raw_len = 0U;
    size_t consumed = 0U;
    ilnk_transport_t tr;
    ilnk_ctx_t ctx;
    capture_t cap;
    ilnk_status_t st;

    memset(&tr, 0, sizeof(tr));
    memset(&cap, 0, sizeof(cap));

    if (argc >= 2) {
        if (parse_hex(argv[1], raw, sizeof(raw), &raw_len) != 0) {
            printf("{\"status\":\"INVALID_HEX\"}\n");
            return 2;
        }
    } else {
        size_t n = fread(input_hex, 1U, sizeof(input_hex) - 1U, stdin);
        input_hex[n] = '\0';
        if (parse_hex(input_hex, raw, sizeof(raw), &raw_len) != 0) {
            printf("{\"status\":\"INVALID_HEX\"}\n");
            return 2;
        }
    }

    (void)ilnk_init(&ctx, tr);
    ilnk_set_frame_handler(&ctx, capture_frame, &cap);
    st = ilnk_feed_bytes(&ctx, raw, raw_len, &consumed);

    if ((st != ILNK_OK) || (!cap.got_frame)) {
        printf("{\"status\":\"%s\",\"bytes_processed\":%lu}\n",
               status_name(st),
               (unsigned long)consumed);
        return 0;
    }

    printf("{\"status\":\"OK\",\"sync_bytes\":[85,170],\"frame_type\":%u,"
           "\"sequence_number\":%u,\"payload_length\":%u,\"checksum\":%u,\"payload\":{",
           (unsigned int)cap.frame.frame_type,
           (unsigned int)cap.frame.sequence_number,
           (unsigned int)cap.frame.payload_length,
           (unsigned int)cap.frame.checksum);

    switch (cap.frame.frame_type) {
        case ILNK_FRAME_CMD_INIT:
            printf("\"client_version\":%u,\"drive_count\":%u",
                   (unsigned int)cap.frame.payload.init.client_version,
                   (unsigned int)cap.frame.payload.init.drive_count);
            break;
        case ILNK_FRAME_CMD_READ_SECTOR:
            printf("\"drive_index\":%u,\"start_sector\":%lu,\"sector_count\":%u",
                   (unsigned int)cap.frame.payload.read_sector.drive_index,
                   (unsigned long)cap.frame.payload.read_sector.start_sector,
                   (unsigned int)cap.frame.payload.read_sector.sector_count);
            break;
        case ILNK_FRAME_CMD_WRITE_SECTOR:
            printf("\"drive_index\":%u,\"start_sector\":%lu,\"sector_count\":%u,\"data_hex\":",
                   (unsigned int)cap.frame.payload.write_sector.drive_index,
                   (unsigned long)cap.frame.payload.write_sector.start_sector,
                   (unsigned int)cap.frame.payload.write_sector.sector_count);
            print_hex_bytes(cap.data_copy, cap.data_copy_len);
            break;
        case ILNK_FRAME_RESP_INIT_ACK:
            printf("\"server_version\":%u,\"drive_count\":%u,\"status\":%u",
                   (unsigned int)cap.frame.payload.init_ack.server_version,
                   (unsigned int)cap.frame.payload.init_ack.drive_count,
                   (unsigned int)cap.frame.payload.init_ack.status);
            break;
        case ILNK_FRAME_RESP_READ_DATA:
            printf("\"drive_index\":%u,\"start_sector\":%lu,\"data_hex\":",
                   (unsigned int)cap.frame.payload.read_data.drive_index,
                   (unsigned long)cap.frame.payload.read_data.start_sector);
            print_hex_bytes(cap.data_copy, cap.data_copy_len);
            break;
        case ILNK_FRAME_RESP_ERROR:
            printf("\"error_code\":%u,\"detail\":%u",
                   (unsigned int)cap.frame.payload.error.error_code,
                   (unsigned int)cap.frame.payload.error.detail);
            break;
        default:
            break;
    }

    printf("}}\n");
    return 0;
}
