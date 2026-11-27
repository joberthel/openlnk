/* tools/bench/dos_profiler/com2log.c
 *
 * Flush MS-DOS disk buffers (INT 21h AH=0Dh) and stream a file or literal line
 * over COM2 (0x2F8) polled UART @ 115200 8N1 without DSR/CTS flow-control hangs.
 *
 * Usage:
 *   COM2LOG -f <file>
 *   COM2LOG -m <message...>
 */
#include <conio.h>
#include <dos.h>
#include <stdio.h>
#include <string.h>

#define COM2_BASE 0x2F8U

static void com2_init(void) {
    outp(COM2_BASE + 1U, 0x00U); /* Disable interrupts */
    outp(COM2_BASE + 3U, 0x80U); /* DLAB=1 */
    outp(COM2_BASE + 0U, 0x01U); /* Divisor=1 (115200 bps) */
    outp(COM2_BASE + 1U, 0x00U);
    outp(COM2_BASE + 3U, 0x03U); /* 8N1, DLAB=0 */
    outp(COM2_BASE + 2U, 0xC7U); /* Enable 16550A FIFO if present */
    outp(COM2_BASE + 4U, 0x03U); /* DTR + RTS */
}

static void com2_putc(unsigned char ch) {
    unsigned long spin = 0UL;
    while (((inp(COM2_BASE + 5U) & 0x20U) == 0U) && (spin < 200000UL)) {
        ++spin;
    }
    outp(COM2_BASE, ch);
}

static void com2_puts(const char *s) {
    while (*s != '\0') {
        com2_putc((unsigned char)*s++);
    }
}

int main(int argc, char **argv) {
    int i;
    union REGS r;
    r.h.ah = 0x0D; /* Flush DOS disk buffers before notifying host */
    int86(0x21, &r, &r);
    com2_init();

    if ((argc >= 3) && (strcmp(argv[1], "-f") == 0)) {
        FILE *fp = fopen(argv[2], "rb");
        if (fp == NULL) {
            com2_puts("[COM2LOG] ERROR opening file\r\n");
            return 1;
        }
        {
            static unsigned char buf[512];
            size_t n;
            while ((n = fread(buf, 1U, sizeof(buf), fp)) > 0U) {
                size_t k;
                for (k = 0U; k < n; ++k) {
                    com2_putc(buf[k]);
                }
            }
        }
        fclose(fp);
        return 0;
    }

    if ((argc >= 3) && (strcmp(argv[1], "-m") == 0)) {
        for (i = 2; i < argc; ++i) {
            if (i > 2) {
                com2_putc(' ');
            }
            com2_puts(argv[i]);
        }
        com2_puts("\r\n");
        return 0;
    }

    com2_puts("Usage: COM2LOG -f <file> | -m <msg>\r\n");
    return 1;
}
