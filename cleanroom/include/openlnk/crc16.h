/* cleanroom/include/openlnk/crc16.h
 *
 * Clean-room CRC-16/GENIBUS implementation for the MS-DOS 6.22
 * INTERLNK / INTERSVR wire protocol.
 *
 * Specification:
 *   Polynomial : 0x1021 (x^16 + x^12 + x^5 + 1)
 *   Init       : 0xFFFF
 *   RefIn      : false
 *   RefOut     : false
 *   XorOut     : 0xFFFF (bitwise complement of CRC-16/CCITT-FALSE)
 *   Wire order : Big-Endian (u2be)
 *   Check res. : 0x1D0F when feeding [length_bytes || payload || crc_be]
 *                through uncomplemented CRC-16/CCITT-FALSE.
 */
#ifndef OPENLNK_CRC16_H
#define OPENLNK_CRC16_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLNK_CRC16_POLY         0x1021U
#define OLNK_CRC16_INIT         0xFFFFU
#define OLNK_CRC16_XOROUT       0xFFFFU
#define OLNK_CRC16_RESIDUE_GOOD 0x1D0FU

/**
 * Update a running uncomplemented CRC-16/CCITT-FALSE value with one byte.
 * Zero dynamic allocation, zero floating-point arithmetic, 16-bit safe.
 */
uint16_t olnk_crc16_update(uint16_t crc, uint8_t byte);

/**
 * Compute uncomplemented CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no xorout).
 * Standard check vector "123456789" -> 0x29B1.
 * Feeding [length_bytes || payload || crc16_genibus_be] yields 0x1D0F.
 */
uint16_t olnk_crc16_ccitt(const uint8_t *data, size_t len);

/**
 * Compute CRC-16/GENIBUS (poly 0x1021, init 0xFFFF, xorout 0xFFFF).
 * Standard check vector "123456789" -> 0xD64E.
 */
uint16_t olnk_crc16_genibus(const uint8_t *data, size_t len);

/**
 * Verify big-endian wire CRC residue over [length_bytes || payload || crc_hi || crc_lo].
 * Returns true iff len >= 2 and CRC-16/CCITT-FALSE residue equals 0x1D0F.
 */
bool olnk_crc16_check_residue(const uint8_t *data_with_crc_be, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_CRC16_H */
