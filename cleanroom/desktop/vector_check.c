/* cleanroom/desktop/vector_check.c
 *
 * Clean-room CLI utility that feeds a hex-encoded canonical wire frame through
 * libopenlnk's streaming state machine and prints the decoded structure as JSON
 * for 24/24 differential parity verification against Kaitai Struct test vectors.
 */
#include "openlnk/openlnk.h"

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

static const char *status_name(olnk_status_t st) {
    switch (st) {
        case OLNK_OK: return "OK";
        /* For standalone single-frame vector checks, truncated frames map to INVALID_FRAME */
        case OLNK_ERR_NEED_DATA: return "INVALID_FRAME";
        case OLNK_ERR_INVALID_FRAME: return "INVALID_FRAME";
        case OLNK_ERR_CHECKSUM_MISMATCH: return "CHECKSUM_MISMATCH";
        case OLNK_ERR_BUFFER_OVERFLOW: return "BUFFER_OVERFLOW";
        case OLNK_ERR_HAL_TIMEOUT: return "HAL_TIMEOUT";
        case OLNK_ERR_INVALID_ARG: return "INVALID_ARG";
        default: return "INVALID_FRAME";
    }
}

typedef struct {
    bool got_frame;
    olnk_wire_frame_t frame;
} capture_t;

static void capture_frame(const olnk_wire_frame_t *frame, void *user_data) {
    capture_t *cap = (capture_t *)user_data;
    cap->got_frame = true;
    cap->frame = *frame;
}

int main(int argc, char **argv) {
    static char input_hex[OLNK_FRAME_BUFFER_SIZE * 2U + 1024U];
    static uint8_t raw[OLNK_FRAME_BUFFER_SIZE + 64U];
    static olnk_ctx_t ctx;
    size_t raw_len = 0U;
    size_t consumed = 0U;
    olnk_transport_t tr;
    capture_t cap;
    olnk_status_t st;

    memset(&tr, 0, sizeof(tr));
    memset(&cap, 0, sizeof(cap));

    if (argc >= 2) {
        if (parse_hex(argv[1], raw, sizeof(raw), &raw_len) != 0) {
            printf("{\"status\":\"INVALID_HEX\"}\n");
            return 2;
        }
    } else {
        size_t total = 0U;
        while (total < (sizeof(input_hex) - 1U)) {
            size_t n = fread(&input_hex[total], 1U, (sizeof(input_hex) - 1U) - total, stdin);
            if (n == 0U) {
                break;
            }
            total += n;
        }
        input_hex[total] = '\0';
        if (parse_hex(input_hex, raw, sizeof(raw), &raw_len) != 0) {
            printf("{\"status\":\"INVALID_HEX\"}\n");
            return 2;
        }
    }

    (void)olnk_init(&ctx, tr);
    olnk_set_frame_handler(&ctx, capture_frame, &cap);
    st = olnk_feed_bytes(&ctx, raw, raw_len, &consumed);

    if ((st != OLNK_OK) || (!cap.got_frame)) {
        printf("{\"status\":\"%s\",\"detail\":\"%s\",\"bytes_processed\":%lu}\n",
               status_name(st),
               (st == OLNK_ERR_NEED_DATA) ? "TRUNCATED_FRAME" : status_name(st),
               (unsigned long)consumed);
        return 0;
    }

    if (consumed != raw_len) {
        printf("{\"status\":\"INVALID_FRAME\",\"detail\":\"TRAILING_BYTES\",\"bytes_processed\":%lu}\n",
               (unsigned long)consumed);
        return 0;
    }

    printf(
        "{\"status\":\"OK\",\"header\":%u,\"is_short_len\":%s,"
        "\"more_fragments\":%s,\"echo_request\":%s,\"sequence_number\":%u,"
        "\"len_payload\":%u,\"crc16\":%u,\"bytes_processed\":%lu}\n",
        (unsigned int)cap.frame.header,
        cap.frame.is_short_len ? "true" : "false",
        cap.frame.more_fragments ? "true" : "false",
        cap.frame.echo_request ? "true" : "false",
        (unsigned int)cap.frame.sequence_number,
        (unsigned int)cap.frame.payload_length,
        (unsigned int)cap.frame.crc16,
        (unsigned long)consumed
    );
    return 0;
}
