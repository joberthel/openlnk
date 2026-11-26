/* cleanroom/src/crc16.c */
#include "openlnk/crc16.h"

uint16_t olnk_crc16_update(uint16_t crc, uint8_t byte) {
    uint8_t bit;
    crc ^= (uint16_t)((uint16_t)byte << 8U);
    for (bit = 0U; bit < 8U; ++bit) {
        if ((crc & 0x8000U) != 0U) {
            crc = (uint16_t)((uint16_t)(crc << 1U) ^ OLNK_CRC16_POLY);
        } else {
            crc = (uint16_t)(crc << 1U);
        }
    }
    return crc;
}

uint16_t olnk_crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = OLNK_CRC16_INIT;
    size_t i;

    if (data == NULL) {
        return crc;
    }

    for (i = 0U; i < len; ++i) {
        crc = olnk_crc16_update(crc, data[i]);
    }
    return crc;
}
