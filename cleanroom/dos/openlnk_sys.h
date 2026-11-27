/* cleanroom/dos/openlnk_sys.h
 *
 * Clean-room 16-bit Real-Mode MS-DOS Block Device Driver (OPENLNK.SYS)
 * public request structures and command dispatcher interface.
 *
 * Request Header layout follows the publicly documented MS-DOS 5.0/6.22
 * Programmer's Reference (Block Device Driver commands 0x00..0x13).
 */
#ifndef OPENLNK_DOS_SYS_H
#define OPENLNK_DOS_SYS_H

#include <stdbool.h>
#include <stdint.h>

#include "dos_hal_uart.h"
#include "openlnk/client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OLNK_SYS_MAX_UNITS 8U

/* Standard MS-DOS Block Device Driver Command Codes */
#define OLNK_DOS_CMD_INIT          0x00U
#define OLNK_DOS_CMD_MEDIA_CHECK   0x01U
#define OLNK_DOS_CMD_BUILD_BPB     0x02U
#define OLNK_DOS_CMD_READ          0x04U
#define OLNK_DOS_CMD_WRITE         0x08U
#define OLNK_DOS_CMD_WRITE_VERIFY  0x09U
#define OLNK_DOS_CMD_OPEN          0x0DU
#define OLNK_DOS_CMD_CLOSE         0x0EU
#define OLNK_DOS_CMD_GENERIC_IOCTL 0x13U

typedef struct {
    uint32_t next_driver;
    uint16_t attributes;
    uint16_t strategy_ptr;
    uint16_t interrupt_ptr;
    uint8_t  unit_count;
    uint8_t  reserved[7];
} dos_block_driver_header_t;

typedef struct {
    uint8_t cmd;
    uint8_t unit;
    uint8_t media_descriptor;
    uint16_t status;
    uint32_t start_lba;
    uint16_t sector_count;
    uint8_t *xfer_buf;
    uint32_t xfer_cap;
    uint8_t media_status;
    olnk_dos_bpb_t bpb;
    bool has_bpb;
    uint8_t ioctl_category;
    uint8_t ioctl_function;
    olnk_generic_ioctl_mid_t mid;
} olnk_dos_sys_req_t;

/**
 * Initialize the freestanding OPENLNK.SYS driver state (COM port + client).
 * Pass custom_io = NULL on real MS-DOS hardware.
 */
olnk_status_t olnk_dos_sys_init(
    uint8_t com_number,
    uint32_t max_baud,
    uint8_t max_units,
    const olnk_dos_io_ops_t *custom_io
);

/**
 * Dispatch an MS-DOS Block Device Driver command (0x00..0x13) through
 * libopenlnk's zero-alloc client engine (auto-connects lazily on first I/O).
 */
uint16_t olnk_dos_sys_dispatch(olnk_dos_sys_req_t *req);

/**
 * Read-only diagnostic inspection helpers for OPENLNK.SYS state.
 */
const olnk_client_t *olnk_dos_sys_client(void);
const olnk_dos_uart_t *olnk_dos_sys_uart(void);
uint8_t olnk_dos_sys_unit_count(void);

/**
 * Assembly entry stubs referenced by dos_block_driver_header_t.
 */
void olnk_dos_strategy(void);
void olnk_dos_interrupt(void);

extern const dos_block_driver_header_t olnk_dos_header;

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_DOS_SYS_H */
