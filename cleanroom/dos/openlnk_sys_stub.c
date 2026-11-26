/* cleanroom/dos/openlnk_sys_stub.c
 *
 * Clean-room 16-bit Real-Mode DOS Block Device Driver (OPENLNK.SYS) skeleton.
 * Header layout follows the publicly documented MS-DOS Programmer's Reference.
 * Compiled via Open Watcom v2 (-ms small model, zero C runtime overhead).
 */
#include "openlnk/openlnk.h"

#include <stdint.h>

typedef struct {
    uint32_t next_driver;
    uint16_t attributes;
    uint16_t strategy_ptr;
    uint16_t interrupt_ptr;
    uint8_t  unit_count;
    uint8_t  reserved[7];
} dos_block_driver_header_t;

static olnk_ctx_t g_driver_ctx;

void olnk_dos_strategy(void) {
    /* Placeholder: capture ES:BX Request Header pointer in Phase 2 DOS HAL */
}

void olnk_dos_interrupt(void) {
    /* Placeholder: dispatch DOS block device commands via g_driver_ctx */
    (void)olnk_poll(&g_driver_ctx);
}

const dos_block_driver_header_t olnk_dos_header = {
    0xFFFFFFFFUL,            /* Last driver in chain */
    OLNK_DRIVE_ATTR_DEFAULT, /* Block device, Open/Close/RM + Generic IOCTL + 32-bit LBA */
    0x0000U,                 /* Strategy offset (linked in assembly stub) */
    0x0000U,                 /* Interrupt offset (linked in assembly stub) */
    1U,                      /* 1 redirected remote block unit */
    { 'O', 'P', 'E', 'N', 'L', 'N', 'K' }
};
