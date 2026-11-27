/* cleanroom/dos/openlnk_sys.h
 *
 * Clean-room 16-bit Real-Mode MS-DOS Block Device Driver (OPENLNK.SYS)
 * public request structures, CONFIG.SYS argument parser, binary MS-DOS
 * Request Header marshaller, and command dispatcher interface.
 *
 * Request Header layout follows the publicly documented MS-DOS 5.0/6.22
 * Programmer's Reference (Block Device Driver commands 0x00..0x19).
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

#define OLNK_SYS_MAX_UNITS      8U
#define OLNK_SYS_BOUNCE_SECTORS 8U
#define OLNK_SYS_BOUNCE_BYTES   (OLNK_SYS_BOUNCE_SECTORS * OLNK_SECTOR_SIZE)

/* MS-DOS 5.0/6.22 Block Device Driver Header Attribute Word Bits */
#define OLNK_DOS_ATTR_LBA32      0x0002U /* Bit 1:  32-bit sector addressing */
#define OLNK_DOS_ATTR_GEN_IOCTL  0x0040U /* Bit 6:  Generic IOCTL & Get/Set Logical */
#define OLNK_DOS_ATTR_OPEN_CLOSE 0x0800U /* Bit 11: Open/Close/Removable Media */
#define OLNK_DOS_ATTR_NON_IBM    0x2000U /* Bit 13: Non-IBM format (no FAT pre-read) */
#define OLNK_DOS_SYS_HEADER_ATTR \
    ((uint16_t)(OLNK_DOS_ATTR_NON_IBM | OLNK_DOS_ATTR_OPEN_CLOSE | \
                OLNK_DOS_ATTR_GEN_IOCTL | OLNK_DOS_ATTR_LBA32)) /* 0x2842 */

/* Standard MS-DOS Block Device Driver Command Codes */
#define OLNK_DOS_CMD_INIT            0x00U
#define OLNK_DOS_CMD_MEDIA_CHECK     0x01U
#define OLNK_DOS_CMD_BUILD_BPB       0x02U
#define OLNK_DOS_CMD_READ            0x04U
#define OLNK_DOS_CMD_WRITE           0x08U
#define OLNK_DOS_CMD_WRITE_VERIFY    0x09U
#define OLNK_DOS_CMD_OPEN            0x0DU
#define OLNK_DOS_CMD_CLOSE           0x0EU
#define OLNK_DOS_CMD_REMOVABLE_MEDIA 0x0FU
#define OLNK_DOS_CMD_GENERIC_IOCTL   0x13U
#define OLNK_DOS_CMD_GET_LOGICAL     0x17U
#define OLNK_DOS_CMD_SET_LOGICAL     0x18U
#define OLNK_DOS_CMD_IOCTL_QUERY     0x19U

typedef struct {
    uint32_t next_driver;
    uint16_t attributes;
    uint16_t strategy_ptr;
    uint16_t interrupt_ptr;
    uint8_t unit_count;
    uint8_t reserved[7];
} dos_block_driver_header_t;

typedef struct {
    uint8_t com_number; /* 1..4 (default 1) */
    uint32_t max_baud;  /* 1200..115200 (default 115200) */
    uint8_t max_units;  /* 1..8 (default 4) */
} olnk_dos_sys_cfg_t;

/* Abstracted Real-Mode far-memory & INIT-time console hooks (host testable) */
typedef struct {
    void (*far_read)(uint16_t seg, uint16_t off, void *dst, uint16_t len, void *ctx);
    void (*far_write)(uint16_t seg, uint16_t off, const void *src, uint16_t len, void *ctx);
    void (*set_header_units)(uint8_t unit_count, void *ctx);
    void (*print_init_msg)(const char *dollar_msg, void *ctx);
    /* Non-DOS host tests only: offset in drv_seg where packed driver tables are mirrored */
    uint16_t host_table_base_off;
    void *ctx;
} olnk_dos_sys_mem_ops_t;

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
    char volume_label[12];
    olnk_dos_bpb_t bpb;
    bool has_bpb;
    uint8_t ioctl_category;
    uint8_t ioctl_function;
    olnk_generic_ioctl_mid_t mid;
} olnk_dos_sys_req_t;

/**
 * Parse CONFIG.SYS DEVICE=OPENLNK.SYS command-line switches:
 *   /COM1../COM4  /BAUD:<1200..115200>  /DRIVES:<1..8>
 * Defaults to COM1, 115200 bps, 4 drives on omitted or invalid switches.
 */
void olnk_dos_sys_parse_args(const char *cmdline, olnk_dos_sys_cfg_t *out_cfg);

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
 * Dispatch an MS-DOS Block Device Driver command (0x00..0x19) through
 * libopenlnk's zero-alloc client engine (auto-connects lazily on first I/O).
 */
uint16_t olnk_dos_sys_dispatch(olnk_dos_sys_req_t *req);

/**
 * Byte-exact MS-DOS Request Header marshaller (0x00..0x19) bridging real
 * segmented MS-DOS Request Headers (rh_seg:rh_off) to olnk_dos_sys_dispatch().
 * Uses a 4 KiB bounce buffer with 64-KiB-boundary-safe far pointer advancement.
 */
uint16_t olnk_dos_sys_handle_rh(
    uint16_t rh_seg,
    uint16_t rh_off,
    uint16_t drv_seg,
    uint16_t resident_end,
    const olnk_dos_sys_mem_ops_t *mem_ops,
    const olnk_dos_io_ops_t *custom_io
);

/**
 * Read-only diagnostic inspection helpers for OPENLNK.SYS state.
 */
const olnk_client_t *olnk_dos_sys_client(void);
const olnk_dos_uart_t *olnk_dos_sys_uart(void);
uint8_t olnk_dos_sys_unit_count(void);

#if !defined(__WATCOMC__) || !defined(__DOS__)
void olnk_dos_strategy(void);
void olnk_dos_interrupt(void);
extern const dos_block_driver_header_t olnk_dos_header;
#endif

#ifdef __cplusplus
}
#endif

#endif /* OPENLNK_DOS_SYS_H */
