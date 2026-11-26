/* cleanroom/include/openlnk/crc16.h */
#ifndef OPENLNK_CRC16_H
#define OPENLNK_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLNK_CRC16_POLY 0x1021U
#define OLNK_CRC16_INIT 0xFFFFU

/**
 * Update a running CRC-16/CCITT-FALSE value with a single byte.
 */
uint16_t olnk_crc16_update(uint16_t crc, uint8_t byte);

/**
 * Compute CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over a buffer.
 * Zero dynamic allocation, zero floating-point arithmetic, 16-bit safe.
 */
uint16_t olnk_crc16_ccitt(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_CRC16_H */
