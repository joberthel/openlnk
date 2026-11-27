/* cleanroom/dos/opensvr_main.c
 *
 * Clean-room 16-bit Real-Mode MS-DOS OPENSVR.EXE server target.
 * Uses direct 8250/16550A COM port I/O (dos_hal_uart.c), MS-DOS INT 25h/26h
 * logical sector I/O (dos_hal_disk.c), and zero-stdio INT 21h console I/O
 * (dos_hal_io.c) backed by libopenlnk's zero-alloc olnk_server_t engine.
 *
 * Compiled via Open Watcom v2 (-ms -0 -za99 -wx -we -os -zm -s -bt=dos) for 8086/8088.
 */
#include "dos_hal_disk.h"
#include "dos_hal_io.h"
#include "dos_hal_uart.h"
#include "openlnk/server.h"
#include "openlnk/version.h"

#include <string.h>

/* Keep all large server/HAL contexts static (never on 16-bit Real-Mode stack) */
static olnk_server_t g_dos_server;
static olnk_dos_uart_t g_dos_uart;
static olnk_dos_disk_set_t g_dos_disks;

static void on_dos_server_event(const char *name, uint32_t a1, uint32_t a2, void *ud) {
    (void)ud;
    if (strcmp(name, "SYNC_OK") == 0) {
        olnk_dos_puts("[OPENSVR] Connected at 9600 bps\n");
    } else if (strcmp(name, "BAUD_SWITCH") == 0) {
        olnk_dos_puts("[OPENSVR] Negotiated ");
        olnk_dos_put_u32(a1);
        olnk_dos_puts(" bps (idx ");
        olnk_dos_put_u32(a2);
        olnk_dos_puts(")\n");
    } else if (strcmp(name, "SESSION_SYNC") == 0) {
        olnk_dos_puts("[OPENSVR] Session ");
        olnk_dos_puts((a2 != 0UL) ? "resumed" : "started");
        olnk_dos_puts(" (client_id=0x");
        olnk_dos_put_hex32(a1);
        olnk_dos_puts(")\n");
    } else if (strcmp(name, "DISCONNECT") == 0) {
        olnk_dos_puts("[OPENSVR] Link reset -> waiting at 9600 bps\n");
    }
}

static void print_dos_usage(void) {
    olnk_dos_puts(
        "OPENSVR " OLNK_VERSION_STRING " (libopenlnk 16-bit Real-Mode MS-DOS 6.22 server)\n"
        "Usage: OPENSVR [drives...] [/COM1../COM4] [/BAUD:rate] [/RO] [/BURST]\n"
        "Example: OPENSVR C: /COM1 /BAUD:115200\n"
    );
}

int OLNK_DOS_MAIN_FN(int argc, char **argv) {
    uint8_t com_num = 1U;
    uint32_t max_baud = 115200UL;
    uint8_t max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    bool read_only = false;
    bool echo_mode = true;
    bool explicit_drives = false;
    olnk_transport_t tr;
    int i;

    olnk_dos_disk_init(&g_dos_disks, false, NULL);

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if ((strcmp(arg, "/?") == 0) || olnk_ascii_ieq_prefix(arg, "/HELP") ||
            (strcmp(arg, "--help") == 0) || (strcmp(arg, "-h") == 0)) {
            print_dos_usage();
            return 0;
        }
        if (olnk_ascii_ieq_prefix(arg, "/COM") && (arg[4] >= '1') && (arg[4] <= '4') && (arg[5] == '\0')) {
            com_num = (uint8_t)(arg[4] - '0');
        } else if (olnk_ascii_ieq_prefix(arg, "/BAUD:")) {
            max_baud = olnk_ascii_parse_u32(&arg[6]);
            if (!olnk_bps_to_baud_index(max_baud, &max_baud_idx)) {
                olnk_dos_puts("Unsupported baud rate: ");
                olnk_dos_put_u32(max_baud);
                olnk_dos_putc('\n');
                return 2;
            }
        } else if (olnk_ascii_ieq_prefix(arg, "/RO") && (arg[3] == '\0')) {
            read_only = true;
        } else if (olnk_ascii_ieq_prefix(arg, "/BURST") && (arg[6] == '\0')) {
            echo_mode = false;
        } else if (olnk_ascii_ieq_prefix(arg, "/V") && (arg[2] == '\0')) {
            echo_mode = true;
        } else if (olnk_ascii_isalpha(arg[0]) && (arg[1] == ':') && (arg[2] == '\0')) {
            uint8_t unit = (uint8_t)(olnk_ascii_toupper(arg[0]) - 'A');
            g_dos_disks.read_only = read_only;
            (void)olnk_dos_disk_add_unit(&g_dos_disks, unit);
            explicit_drives = true;
        }
    }

    g_dos_disks.read_only = read_only;
    if (!explicit_drives) {
        (void)olnk_dos_disk_add_unit(&g_dos_disks, 2U); /* Default: C: */
    }
    {
        uint8_t u;
        for (u = 0U; u < OLNK_MAX_DRIVES; ++u) {
            if (g_dos_disks.enabled[u]) {
                g_dos_disks.info[u].write_protected = read_only;
            }
        }
    }

    if (olnk_dos_uart_init(&g_dos_uart, com_num, 0U, 9600UL, NULL, &tr) != OLNK_OK) {
        olnk_dos_puts("Failed to initialize COM");
        olnk_dos_put_u32((uint32_t)com_num);
        olnk_dos_putc('\n');
        return 1;
    }

    if (olnk_server_init(&g_dos_server, tr, olnk_dos_disk_ops(&g_dos_disks)) != OLNK_OK) {
        return 1;
    }

    g_dos_server.link.max_baud_idx = max_baud_idx;
    g_dos_server.link.local_requires_echo = echo_mode;
    g_dos_server.idle_disconnect_ms = 60000UL;
    olnk_server_set_event_callback(&g_dos_server, on_dos_server_event, NULL);

    olnk_dos_puts("OPENSVR " OLNK_VERSION_STRING " ready on COM");
    olnk_dos_put_u32((uint32_t)com_num);
    olnk_dos_puts(" (0x");
    olnk_dos_put_hex16(g_dos_uart.io_base);
    olnk_dos_puts(", ");
    olnk_dos_puts(g_dos_uart.has_fifo ? "16550A FIFO" : "8250/16450");
    olnk_dos_puts(", max ");
    olnk_dos_put_u32(olnk_baud_index_to_bps(max_baud_idx));
    olnk_dos_puts(" bps");
    if (read_only) {
        olnk_dos_puts(", READ-ONLY");
    }
    olnk_dos_puts(")\n");

#if defined(__WATCOMC__) && defined(__DOS__)
    olnk_dos_puts("Press ESC or Q to exit.\n");
    {
        uint32_t last_tick = olnk_dos_get_ticks(&g_dos_uart);
        for (;;) {
            uint8_t rx_buf[64];
            int n;
            uint32_t now_tick;

            if (olnk_dos_kbhit()) {
                uint8_t ch = olnk_dos_getch();
                if ((ch == 27U) || (ch == (uint8_t)'q') || (ch == (uint8_t)'Q') || (ch == 3U)) {
                    break;
                }
            }

            n = g_dos_server.link.transport.read(
                rx_buf, sizeof(rx_buf), 0U, g_dos_server.link.transport.user_data);
            if (n > 0) {
                olnk_server_feed_bytes(&g_dos_server, rx_buf, (size_t)n);
                last_tick = olnk_dos_get_ticks(&g_dos_uart);
            } else {
                now_tick = olnk_dos_get_ticks(&g_dos_uart);
                if (now_tick != last_tick) {
                    last_tick = now_tick;
                    olnk_server_tick(&g_dos_server, OLNK_BIOS_TICK_MS);
                }
            }
        }
    }
    olnk_dos_puts("OPENSVR stopped (rpcs=");
    olnk_dos_put_u32(g_dos_server.rpc_count);
    olnk_dos_puts(", rd=");
    olnk_dos_put_u32(g_dos_server.sectors_read_total);
    olnk_dos_puts(", wr=");
    olnk_dos_put_u32(g_dos_server.sectors_written_total);
    olnk_dos_puts(", err=");
    olnk_dos_put_u32(g_dos_server.link.frames_rx_err);
    olnk_dos_puts(")\n");
#endif
    return 0;
}
