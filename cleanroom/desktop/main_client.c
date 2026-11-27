/* cleanroom/desktop/main_client.c
 *
 * Clean-room MS-DOS 6.22 INTERLNK-compatible client CLI (openlnk) built on
 * libopenlnk (zero-alloc C99 core).
 *
 * Interoperates directly with original MS-DOS 6.22 INTERSVR.EXE (e.g. in 86Box)
 * as well as clean-room opensvr over POSIX serial ports (--port), 86Box Named
 * Pipes (--pipe), or host-to-host peer pipes (--peer-pipe).
 */

#if !defined(_WIN32)
#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#endif

#include "openlnk/client.h"
#include "openlnk/server.h"
#include "openlnk/storage.h"
#include "openlnk/version.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#endif

/* =========================================================================
 * Self-Test: Full Client <-> Server In-Memory Loopback via olnk_client_*
 * ========================================================================= */

#define CLI_ST_RING_CAP 20480U

typedef struct {
    uint8_t buf[CLI_ST_RING_CAP];
    size_t head;
    size_t tail;
    size_t count;
} cli_ring_t;

typedef struct {
    cli_ring_t s2c;
    olnk_server_t *srv;
    uint32_t cli_baud;
    uint32_t srv_baud;
} cli_loopback_t;

static void cli_ring_push(cli_ring_t *r, const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0U; i < len; ++i) {
        if (r->count < CLI_ST_RING_CAP) {
            r->buf[r->tail] = data[i];
            r->tail = (r->tail + 1U) % CLI_ST_RING_CAP;
            r->count += 1U;
        }
    }
}

static size_t cli_ring_pop(cli_ring_t *r, uint8_t *out, size_t len) {
    size_t n = 0U;
    while ((n < len) && (r->count > 0U)) {
        out[n++] = r->buf[r->head];
        r->head = (r->head + 1U) % CLI_ST_RING_CAP;
        r->count -= 1U;
    }
    return n;
}

static int cli_st_srv_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    cli_loopback_t *lb = (cli_loopback_t *)ud;
    (void)timeout_ms;
    cli_ring_push(&lb->s2c, buf, len);
    return (int)len;
}

static bool cli_st_srv_set_baud(uint32_t baud, void *ud) {
    cli_loopback_t *lb = (cli_loopback_t *)ud;
    lb->srv_baud = baud;
    return true;
}

static int cli_st_cli_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    cli_loopback_t *lb = (cli_loopback_t *)ud;
    (void)timeout_ms;
    if (lb->cli_baud == lb->srv_baud) {
        olnk_server_feed_bytes(lb->srv, buf, len);
    }
    return (int)len;
}

static int cli_st_cli_read(uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    cli_loopback_t *lb = (cli_loopback_t *)ud;
    size_t n = cli_ring_pop(&lb->s2c, buf, len);
    if ((n == 0U) && (timeout_ms > 0U)) {
        olnk_server_tick(lb->srv, timeout_ms);
        n = cli_ring_pop(&lb->s2c, buf, len);
    }
    return (int)n;
}

static bool cli_st_cli_set_baud(uint32_t baud, void *ud) {
    cli_loopback_t *lb = (cli_loopback_t *)ud;
    lb->cli_baud = baud;
    return true;
}

static int run_self_test(void) {
    static uint8_t disk_img[OLNK_SECTOR_SIZE * 64U];
    static uint8_t wr_20k[OLNK_SECTOR_SIZE * 40U];
    static uint8_t rd_20k[OLNK_SECTOR_SIZE * 40U];
    static olnk_server_t srv;
    static olnk_client_t cli;
    static cli_loopback_t lb;

    olnk_mem_drive_t drv;
    olnk_transport_t srv_tr;
    olnk_transport_t cli_tr;
    olnk_session_sync_resp_t sresp;
    olnk_map_config_resp_t mresp;
    olnk_get_drive_info_resp_t iresp;
    olnk_media_check_resp_t mcresp;
    olnk_build_bpb_resp_t bresp;
    olnk_sector_io_resp_t ior;
    olnk_generic_ioctl_mid_t mid;
    uint16_t dos_st = 0U;
    size_t i;

    memset(&lb, 0, sizeof(lb));
    memset(disk_img, 0, sizeof(disk_img));
    olnk_fat_build_synthetic_vbr(disk_img, 64UL, 0xF8U, "OPENLNK_CLI", 0x20261127UL);
    olnk_mem_drive_init(&drv, disk_img, 64UL);

    lb.srv = &srv;
    lb.cli_baud = 9600UL;
    lb.srv_baud = 9600UL;

    memset(&srv_tr, 0, sizeof(srv_tr));
    srv_tr.write = cli_st_srv_write;
    srv_tr.set_baud = cli_st_srv_set_baud;
    srv_tr.user_data = &lb;

    memset(&cli_tr, 0, sizeof(cli_tr));
    cli_tr.read = cli_st_cli_read;
    cli_tr.write = cli_st_cli_write;
    cli_tr.set_baud = cli_st_cli_set_baud;
    cli_tr.user_data = &lb;

    if ((olnk_server_init(&srv, srv_tr, olnk_mem_drive_ops(&drv)) != OLNK_OK) ||
        (olnk_client_init(&cli, cli_tr) != OLNK_OK)) {
        return 1;
    }

    if ((olnk_client_connect(&cli, 2U) != OLNK_OK) ||
        (lb.cli_baud != 115200UL) || (lb.srv_baud != 115200UL)) {
        fprintf(stderr, "openlnk self-test FAIL: connect/baud\n");
        return 2;
    }
    if ((olnk_client_session_sync(&cli, false, &sresp) != OLNK_OK) ||
        (sresp.ready_flag != 1U) || (sresp.max_block_size != 8192U)) {
        fprintf(stderr, "openlnk self-test FAIL: session_sync\n");
        return 3;
    }
    if ((olnk_client_map_config(&cli, 8U, 3U, &mresp) != OLNK_OK) ||
        (mresp.server_drive_count != 1U) || (mresp.drive_map_resp[0] != 0U)) {
        fprintf(stderr, "openlnk self-test FAIL: map_config\n");
        return 4;
    }
    if ((olnk_client_get_drive_info(&cli, 0U, &iresp) != OLNK_OK) ||
        (strcmp(iresp.volume_label, "OPENLNK_CLI") != 0)) {
        fprintf(stderr, "openlnk self-test FAIL: get_drive_info\n");
        return 5;
    }
    if ((olnk_client_media_check(&cli, 0U, 0xF8U, &mcresp) != OLNK_OK) ||
        ((mcresp.status & OLNK_DOS_STATUS_ERROR) != 0U) ||
        (mcresp.media_status != OLNK_MEDIA_UNCHANGED)) {
        fprintf(stderr, "openlnk self-test FAIL: media_check\n");
        return 6;
    }
    if ((olnk_client_build_bpb(&cli, 0U, 0xF8U, &bresp) != OLNK_OK) ||
        (!bresp.has_bpb) || (bresp.bpb.bytes_per_sector != 512U) ||
        (bresp.bpb.total_sectors_16 != 64U)) {
        fprintf(stderr, "openlnk self-test FAIL: build_bpb\n");
        return 7;
    }
    if ((olnk_client_get_media_id(&cli, 0U, &dos_st, &mid) != OLNK_OK) ||
        ((dos_st & OLNK_DOS_STATUS_ERROR) != 0U) ||
        (mid.serial_number != 0x20261127UL)) {
        fprintf(stderr, "openlnk self-test FAIL: generic_ioctl get_mid\n");
        return 8;
    }

    for (i = 0U; i < sizeof(wr_20k); ++i) {
        wr_20k[i] = (uint8_t)((i * 53U + 17U) & 0xFFU);
    }
    if ((olnk_client_write_sectors(&cli, 0U, 0xF8U, 10UL, 40U, 512U, wr_20k, true, &ior) != OLNK_OK) ||
        ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U) ||
        (ior.sectors_transferred != 40U)) {
        fprintf(stderr, "openlnk self-test FAIL: 20 KiB write_verify\n");
        return 9;
    }
    if ((olnk_client_read_sectors(&cli, 0U, 0xF8U, 10UL, 40U, 512U, rd_20k, (uint32_t)sizeof(rd_20k), &ior) != OLNK_OK) ||
        ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U) ||
        (ior.sectors_transferred != 40U) ||
        (memcmp(wr_20k, rd_20k, sizeof(wr_20k)) != 0)) {
        fprintf(stderr, "openlnk self-test FAIL: 20 KiB read_sectors\n");
        return 10;
    }
    if (olnk_client_keepalive(&cli) != OLNK_OK) {
        fprintf(stderr, "openlnk self-test FAIL: keepalive\n");
        return 11;
    }

    printf(
        "openlnk self-test PASSED (baud=%lu, rpcs=%lu, rd_sec=%lu, wr_sec=%lu, ka=%lu)\n",
        (unsigned long)lb.cli_baud,
        (unsigned long)cli.rpc_count,
        (unsigned long)cli.sectors_read_total,
        (unsigned long)cli.sectors_written_total,
        (unsigned long)cli.link.keepalives_ok
    );
    return 0;
}

/* =========================================================================
 * POSIX Transport & Remote FAT12/FAT16 Client Commands
 * ========================================================================= */

#if !defined(_WIN32)

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_stop_requested = 1;
}

typedef struct {
    int rx_fd;
    int tx_fd;
    bool is_serial;
    uint32_t pace_us;
} cli_posix_hal_t;

static speed_t bps_to_termios_speed(uint32_t bps) {
    switch (bps) {
        case 1200UL:   return B1200;
        case 2400UL:   return B2400;
        case 4800UL:   return B4800;
        case 9600UL:   return B9600;
        case 19200UL:  return B19200;
        case 38400UL:  return B38400;
        case 57600UL:  return B57600;
        case 115200UL: return B115200;
        default:       return B9600;
    }
}

static bool posix_serial_configure(int fd, uint32_t bps) {
    struct termios tio;
    speed_t spd = bps_to_termios_speed(bps);

    if (tcgetattr(fd, &tio) != 0) {
        return false;
    }
    tio.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    tio.c_oflag &= (tcflag_t)~(OPOST);
    tio.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag &= (tcflag_t)~(CSIZE | PARENB | CSTOPB);
    tio.c_cflag |= (tcflag_t)(CS8 | CLOCAL | CREAD);
    tio.c_cc[VMIN] = 0U;
    tio.c_cc[VTIME] = 0U;

    if ((cfsetispeed(&tio, spd) != 0) || (cfsetospeed(&tio, spd) != 0)) {
        return false;
    }
    return tcsetattr(fd, TCSADRAIN, &tio) == 0;
}

static bool cli_hal_set_baud(uint32_t baudrate, void *ud) {
    cli_posix_hal_t *hal = (cli_posix_hal_t *)ud;
    if (!hal->is_serial) {
        return true;
    }
    (void)tcdrain(hal->tx_fd);
    return posix_serial_configure(hal->rx_fd, baudrate);
}

static int cli_hal_read(uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    cli_posix_hal_t *hal = (cli_posix_hal_t *)ud;
    fd_set rfds;
    struct timeval tv;
    int rc;

    FD_ZERO(&rfds);
    FD_SET(hal->rx_fd, &rfds);
    tv.tv_sec = (time_t)(timeout_ms / 1000UL);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000UL) * 1000UL);

    rc = select(hal->rx_fd + 1, &rfds, NULL, NULL, &tv);
    if (rc < 0) {
        return (errno == EINTR) ? 0 : -1;
    }
    if (rc == 0) {
        return 0;
    }

    {
        ssize_t n = read(hal->rx_fd, buf, len);
        if (n < 0) {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) {
                return 0;
            }
            return -1;
        }
        return (int)n;
    }
}

static int cli_hal_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    cli_posix_hal_t *hal = (cli_posix_hal_t *)ud;
    size_t sent = 0U;

    while (sent < len) {
        size_t chunk = (hal->pace_us > 0UL) ? 1U : (len - sent);
        ssize_t n = write(hal->tx_fd, &buf[sent], chunk);
        if (n > 0) {
            sent += (size_t)n;
            if (hal->pace_us > 0UL) {
                struct timeval ptv;
                ptv.tv_sec = (time_t)(hal->pace_us / 1000000UL);
                ptv.tv_usec = (suseconds_t)(hal->pace_us % 1000000UL);
                (void)select(0, NULL, NULL, NULL, &ptv);
            }
            continue;
        }
        if ((n < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
            fd_set wfds;
            struct timeval tv;
            int rc;
            FD_ZERO(&wfds);
            FD_SET(hal->tx_fd, &wfds);
            tv.tv_sec = (time_t)(timeout_ms / 1000UL);
            tv.tv_usec = (suseconds_t)((timeout_ms % 1000UL) * 1000UL);
            rc = select(hal->tx_fd + 1, NULL, &wfds, NULL, &tv);
            if (rc <= 0) {
                return (sent > 0U) ? (int)sent : 0;
            }
            continue;
        }
        if ((n < 0) && (errno == EINTR)) {
            continue;
        }
        return -1;
    }
    return (int)sent;
}

static bool ensure_fifo(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISFIFO(st.st_mode)) {
            return true;
        }
        fprintf(stderr, "[openlnk] ERROR: Path exists and is not a FIFO: %s\n", path);
        return false;
    }
    if (mkfifo(path, 0600) != 0) {
        fprintf(stderr, "[openlnk] ERROR: mkfifo(%s) failed: %s\n", path, strerror(errno));
        return false;
    }
    return true;
}

static bool cli_open_pipe(const char *prefix, bool swap_dirs, cli_posix_hal_t *hal) {
    char in_path[512];
    char out_path[512];
    const char *rx_path;
    const char *tx_path;
    uint8_t drain[256];

    if (strlen(prefix) + 8U >= sizeof(in_path)) {
        fprintf(stderr, "[openlnk] ERROR: Pipe prefix too long\n");
        return false;
    }
    snprintf(in_path, sizeof(in_path), "%s.in", prefix);
    snprintf(out_path, sizeof(out_path), "%s.out", prefix);

    if (!ensure_fifo(in_path) || !ensure_fifo(out_path)) {
        return false;
    }

    /* 86Box writes to .out and reads from .in.
     * In --peer-pipe mode (connecting to host opensvr --pipe), roles swap! */
    rx_path = swap_dirs ? in_path : out_path;
    tx_path = swap_dirs ? out_path : in_path;

    hal->rx_fd = open(rx_path, O_RDWR | O_NONBLOCK);
    if (hal->rx_fd < 0) {
        fprintf(stderr, "[openlnk] ERROR: open(%s) failed: %s\n", rx_path, strerror(errno));
        return false;
    }
    hal->tx_fd = open(tx_path, O_RDWR | O_NONBLOCK);
    if (hal->tx_fd < 0) {
        fprintf(stderr, "[openlnk] ERROR: open(%s) failed: %s\n", tx_path, strerror(errno));
        close(hal->rx_fd);
        hal->rx_fd = -1;
        return false;
    }
    hal->is_serial = false;

    while (read(hal->rx_fd, drain, sizeof(drain)) > 0) {
        /* drain stale bytes */
    }
    return true;
}

static bool cli_open_serial(const char *port_path, cli_posix_hal_t *hal) {
    int fd = open(port_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[openlnk] ERROR: open(%s) failed: %s\n", port_path, strerror(errno));
        return false;
    }
    if (!posix_serial_configure(fd, 9600UL)) {
        fprintf(stderr, "[openlnk] WARN: termios config skipped on %s (%s)\n",
                port_path, strerror(errno));
    }
    (void)tcflush(fd, TCIOFLUSH);
    hal->rx_fd = fd;
    hal->tx_fd = fd;
    hal->is_serial = true;
    return true;
}

static void cli_close_hal(cli_posix_hal_t *hal) {
    if (hal->rx_fd >= 0) {
        close(hal->rx_fd);
    }
    if ((hal->tx_fd >= 0) && (hal->tx_fd != hal->rx_fd)) {
        close(hal->tx_fd);
    }
    hal->rx_fd = -1;
    hal->tx_fd = -1;
}

/* =========================================================================
 * Remote FAT12 / FAT16 Helpers (using olnk_client_read_sectors)
 * ========================================================================= */

typedef struct {
    uint8_t unit;
    olnk_dos_bpb_t bpb;
    uint16_t bps;
    uint32_t total_sectors;
    uint32_t fat_start_lba;
    uint32_t root_start_lba;
    uint32_t root_sectors;
    uint32_t data_start_lba;
    bool is_fat12;

    /* 2-sector FAT window cache (covers FAT12 entries spanning sector boundary) */
    uint8_t fat_cache[OLNK_SECTOR_SIZE * 2U];
    uint32_t fat_cache_lba;
    uint16_t fat_cache_secs;
} remote_fat_vol_t;

static bool parse_unit_spec(const char *spec, uint8_t *out_unit) {
    if ((spec == NULL) || (spec[0] == '\0')) {
        return false;
    }
    if (isalpha((unsigned char)spec[0]) &&
        ((spec[1] == '\0') || ((spec[1] == ':') && (spec[2] == '\0')))) {
        char c = (char)toupper((unsigned char)spec[0]);
        *out_unit = (uint8_t)(c - 'A');
        return true;
    }
    {
        char *end = NULL;
        unsigned long v = strtoul(spec, &end, 10);
        if ((end != spec) && (*end == '\0') && (v < OLNK_MAX_DRIVES)) {
            *out_unit = (uint8_t)v;
            return true;
        }
    }
    return false;
}

static const char *dos_err_name(uint16_t status) {
    if ((status & OLNK_DOS_STATUS_ERROR) == 0U) {
        return "OK";
    }
    switch (status & 0x00FFU) {
        case OLNK_DOS_ERR_WRITE_PROTECT:    return "WRITE_PROTECT";
        case OLNK_DOS_ERR_UNKNOWN_UNIT:     return "UNKNOWN_UNIT";
        case OLNK_DOS_ERR_DRIVE_NOT_READY:  return "DRIVE_NOT_READY";
        case OLNK_DOS_ERR_UNKNOWN_COMMAND:  return "UNKNOWN_COMMAND";
        case OLNK_DOS_ERR_CRC_ERROR:        return "CRC_ERROR";
        case OLNK_DOS_ERR_BAD_REQ_LENGTH:   return "BAD_REQ_LENGTH";
        case OLNK_DOS_ERR_SEEK_ERROR:       return "SEEK_ERROR";
        case OLNK_DOS_ERR_UNKNOWN_MEDIA:    return "UNKNOWN_MEDIA";
        case OLNK_DOS_ERR_SECTOR_NOT_FOUND: return "SECTOR_NOT_FOUND";
        case OLNK_DOS_ERR_WRITE_FAULT:      return "WRITE_FAULT";
        case OLNK_DOS_ERR_READ_FAULT:       return "READ_FAULT";
        case OLNK_DOS_ERR_INVALID_DISK_CHG: return "INVALID_DISK_CHANGE";
        default:                            return "GENERAL_FAILURE";
    }
}

static bool select_default_unit(olnk_client_t *cli, uint8_t *out_unit) {
    size_t i;
    uint8_t first_mapped = 0xFFU;

    if (!cli->has_map) {
        *out_unit = 0U;
        return true;
    }

    for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
        uint8_t entry = cli->map.drive_map_resp[i];
        if ((entry == OLNK_MAP_AUTO) || (entry == OLNK_MAP_NONE)) {
            continue;
        }
        {
            uint8_t u = (uint8_t)(entry & 0x7FU);
            olnk_build_bpb_resp_t bpb;
            if (first_mapped == 0xFFU) {
                first_mapped = u;
            }
            if ((olnk_client_build_bpb(cli, u, 0xF8U, &bpb) == OLNK_OK) &&
                ((bpb.status & OLNK_DOS_STATUS_ERROR) == 0U) && bpb.has_bpb) {
                *out_unit = u;
                return true;
            }
        }
    }

    if (first_mapped != 0xFFU) {
        *out_unit = first_mapped;
        return true;
    }
    return false;
}

static bool remote_fat_mount(olnk_client_t *cli, uint8_t unit, remote_fat_vol_t *vol) {
    olnk_build_bpb_resp_t bresp;
    olnk_media_check_resp_t mresp;
    uint32_t data_sectors;
    uint32_t clusters;

    memset(vol, 0, sizeof(*vol));
    vol->unit = unit;

    /* Run MEDIA_CHECK first (mirrors MS-DOS block driver sequence before BUILD_BPB) */
    (void)olnk_client_media_check(cli, unit, 0xF8U, &mresp);

    if (olnk_client_build_bpb(cli, unit, 0xF8U, &bresp) != OLNK_OK) {
        fprintf(stderr, "[openlnk] ERROR: Transport failure during BUILD_BPB on unit %u (%c:)\n",
                (unsigned int)unit, (char)('A' + (char)unit));
        return false;
    }
    if (((bresp.status & OLNK_DOS_STATUS_ERROR) != 0U) || (!bresp.has_bpb)) {
        fprintf(stderr, "[openlnk] ERROR: BUILD_BPB failed on unit %u (%c:): status=0x%04X (%s)\n",
                (unsigned int)unit, (char)('A' + (char)unit),
                (unsigned int)bresp.status, dos_err_name(bresp.status));
        return false;
    }

    vol->bpb = bresp.bpb;
    vol->bps = (bresp.bpb.bytes_per_sector != 0U) ? bresp.bpb.bytes_per_sector : OLNK_SECTOR_SIZE;
    vol->total_sectors = (bresp.bpb.total_sectors_16 != 0U)
                             ? (uint32_t)bresp.bpb.total_sectors_16
                             : bresp.bpb.total_sectors_32;
    vol->fat_start_lba = (uint32_t)bresp.bpb.reserved_sectors;
    vol->root_start_lba = vol->fat_start_lba +
                          (uint32_t)bresp.bpb.num_fats * (uint32_t)bresp.bpb.sectors_per_fat;
    vol->root_sectors = ((uint32_t)bresp.bpb.root_entries * 32UL + (uint32_t)vol->bps - 1UL) /
                        (uint32_t)vol->bps;
    vol->data_start_lba = vol->root_start_lba + vol->root_sectors;

    data_sectors = (vol->total_sectors > vol->data_start_lba)
                       ? (vol->total_sectors - vol->data_start_lba)
                       : 0UL;
    clusters = (bresp.bpb.sectors_per_cluster != 0U)
                   ? (data_sectors / (uint32_t)bresp.bpb.sectors_per_cluster)
                   : 0UL;
    vol->is_fat12 = (clusters < 4085UL);
    vol->fat_cache_secs = 0U;
    return true;
}

static bool remote_fat_next_cluster(
    olnk_client_t *cli,
    remote_fat_vol_t *vol,
    uint16_t cluster,
    uint16_t *out_next
) {
    uint32_t fat_byte_off = vol->is_fat12
                                ? (((uint32_t)cluster * 3UL) / 2UL)
                                : ((uint32_t)cluster * 2UL);
    uint32_t fat_sec = vol->fat_start_lba + (fat_byte_off / (uint32_t)vol->bps);
    uint32_t off_in_sec = fat_byte_off % (uint32_t)vol->bps;
    uint16_t need_secs = ((off_in_sec + 1U) >= (uint32_t)vol->bps) ? 2U : 1U;
    uint16_t raw_val;

    if ((vol->fat_cache_secs == 0U) ||
        (fat_sec < vol->fat_cache_lba) ||
        ((fat_sec + (uint32_t)need_secs) > (vol->fat_cache_lba + (uint32_t)vol->fat_cache_secs))) {
        olnk_sector_io_resp_t ior;
        uint32_t max_fat_end = vol->fat_start_lba + (uint32_t)vol->bpb.sectors_per_fat;
        uint16_t to_read = ((fat_sec + 2UL) <= max_fat_end) ? 2U : 1U;
        if ((olnk_client_read_sectors(
                 cli, vol->unit, vol->bpb.media_descriptor, fat_sec, to_read, vol->bps,
                 vol->fat_cache, (uint32_t)sizeof(vol->fat_cache), &ior) != OLNK_OK) ||
            ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U)) {
            return false;
        }
        vol->fat_cache_lba = fat_sec;
        vol->fat_cache_secs = to_read;
    }

    {
        size_t idx = (size_t)((fat_sec - vol->fat_cache_lba) * (uint32_t)vol->bps + off_in_sec);
        raw_val = olnk_read_u16_le(&vol->fat_cache[idx]);
    }

    if (vol->is_fat12) {
        *out_next = ((cluster & 1U) != 0U) ? (uint16_t)(raw_val >> 4U) : (uint16_t)(raw_val & 0x0FFFU);
    } else {
        *out_next = raw_val;
    }
    return true;
}

static bool remote_fat_is_eof(const remote_fat_vol_t *vol, uint16_t cluster) {
    if (cluster < 2U) {
        return true;
    }
    return vol->is_fat12 ? (cluster >= 0x0FF8U) : (cluster >= 0xFFF8U);
}

static void format_83_name(const uint8_t ent[32], char out[16]) {
    size_t pos = 0U;
    size_t i;
    size_t name_end = 8U;
    size_t ext_end = 11U;

    while ((name_end > 0U) && (ent[name_end - 1U] == ' ')) {
        name_end -= 1U;
    }
    while ((ext_end > 8U) && (ent[ext_end - 1U] == ' ')) {
        ext_end -= 1U;
    }

    for (i = 0U; i < name_end; ++i) {
        out[pos++] = (char)ent[i];
    }
    if (ext_end > 8U) {
        out[pos++] = '.';
        for (i = 8U; i < ext_end; ++i) {
            out[pos++] = (char)ent[i];
        }
    }
    out[pos] = '\0';
}

static void make_83_pattern(const char *comp, uint8_t pat[11]) {
    size_t i = 0U;
    size_t dst = 0U;
    memset(pat, ' ', 11U);
    while ((comp[i] != '\0') && (comp[i] != '.') && (dst < 8U)) {
        pat[dst++] = (uint8_t)toupper((unsigned char)comp[i++]);
    }
    while ((comp[i] != '\0') && (comp[i] != '.')) {
        ++i;
    }
    if (comp[i] == '.') {
        ++i;
        dst = 8U;
        while ((comp[i] != '\0') && (dst < 11U)) {
            pat[dst++] = (uint8_t)toupper((unsigned char)comp[i++]);
        }
    }
}

typedef struct {
    bool found;
    bool is_dir;
    uint8_t attr;
    uint16_t start_cluster;
    uint32_t file_size;
    char name[16];
} fat_dirent_t;

static bool scan_dir_entries(
    olnk_client_t *cli,
    remote_fat_vol_t *vol,
    uint16_t dir_cluster, /* 0 = root directory */
    const uint8_t *match_83, /* NULL = list all entries */
    fat_dirent_t *out_match
) {
    static uint8_t sec_buf[OLNK_SECTOR_SIZE];
    uint16_t cur_cluster = dir_cluster;
    uint32_t sec_idx = 0UL;
    uint32_t total_entries = 0UL;
    uint32_t total_bytes = 0UL;

    if (out_match != NULL) {
        memset(out_match, 0, sizeof(*out_match));
    }

    for (;;) {
        uint32_t lba;
        olnk_sector_io_resp_t ior;
        size_t off;

        if (dir_cluster == 0U) {
            if (sec_idx >= vol->root_sectors) {
                break;
            }
            lba = vol->root_start_lba + sec_idx;
            sec_idx += 1UL;
        } else {
            if (remote_fat_is_eof(vol, cur_cluster)) {
                break;
            }
            lba = vol->data_start_lba +
                  ((uint32_t)(cur_cluster - 2U) * (uint32_t)vol->bpb.sectors_per_cluster) +
                  sec_idx;
            sec_idx += 1UL;
            if (sec_idx >= (uint32_t)vol->bpb.sectors_per_cluster) {
                sec_idx = 0UL;
                if (!remote_fat_next_cluster(cli, vol, cur_cluster, &cur_cluster)) {
                    return false;
                }
            }
        }

        if ((olnk_client_read_sectors(
                 cli, vol->unit, vol->bpb.media_descriptor, lba, 1U, vol->bps,
                 sec_buf, (uint32_t)sizeof(sec_buf), &ior) != OLNK_OK) ||
            ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U)) {
            return false;
        }

        for (off = 0U; (off + 32U) <= (size_t)vol->bps; off += 32U) {
            const uint8_t *ent = &sec_buf[off];
            uint8_t attr = ent[11];
            if (ent[0] == 0x00U) {
                goto done_scan;
            }
            if ((ent[0] == 0xE5U) || (attr == 0x0FU) || ((attr & 0x08U) != 0U)) {
                continue;
            }

            if (match_83 != NULL) {
                if (memcmp(ent, match_83, 11U) == 0) {
                    if (out_match != NULL) {
                        out_match->found = true;
                        out_match->attr = attr;
                        out_match->is_dir = ((attr & 0x10U) != 0U);
                        out_match->start_cluster = olnk_read_u16_le(&ent[26]);
                        out_match->file_size = olnk_read_u32_le(&ent[28]);
                        format_83_name(ent, out_match->name);
                    }
                    return true;
                }
            } else {
                char name[16];
                uint16_t dtime = olnk_read_u16_le(&ent[22]);
                uint16_t ddate = olnk_read_u16_le(&ent[24]);
                uint32_t fsize = olnk_read_u32_le(&ent[28]);
                unsigned int yr = 1980U + (unsigned int)((ddate >> 9U) & 0x7FU);
                unsigned int mo = (unsigned int)((ddate >> 5U) & 0x0FU);
                unsigned int dy = (unsigned int)(ddate & 0x1FU);
                unsigned int hr = (unsigned int)((dtime >> 11U) & 0x1FU);
                unsigned int mn = (unsigned int)((dtime >> 5U) & 0x3FU);

                format_83_name(ent, name);
                if ((attr & 0x10U) != 0U) {
                    printf("  %-13s <DIR>        %04u-%02u-%02u %02u:%02u\n",
                           name, yr, mo, dy, hr, mn);
                } else {
                    printf("  %-13s %10lu   %04u-%02u-%02u %02u:%02u\n",
                           name, (unsigned long)fsize, yr, mo, dy, hr, mn);
                    total_bytes += fsize;
                }
                total_entries += 1UL;
            }
        }
    }

done_scan:
    if (match_83 == NULL) {
        printf("  %lu entry(ies), %lu bytes\n", (unsigned long)total_entries, (unsigned long)total_bytes);
    }
    return true;
}

static bool resolve_fat_path(
    olnk_client_t *cli,
    remote_fat_vol_t *vol,
    const char *path,
    fat_dirent_t *out_entry
) {
    uint16_t cur_dir = 0U;
    const char *p = path;
    fat_dirent_t ent;

    memset(&ent, 0, sizeof(ent));
    if (p == NULL) {
        return false;
    }
    while ((*p == '/') || (*p == '\\')) {
        ++p;
    }
    if (*p == '\0') {
        out_entry->found = true;
        out_entry->is_dir = true;
        out_entry->start_cluster = 0U;
        strcpy(out_entry->name, "/");
        return true;
    }

    while (*p != '\0') {
        char comp[32];
        size_t clen = 0U;
        uint8_t pat[11];

        while ((*p != '\0') && (*p != '/') && (*p != '\\')) {
            if (clen + 1U < sizeof(comp)) {
                comp[clen++] = *p;
            }
            ++p;
        }
        comp[clen] = '\0';
        while ((*p == '/') || (*p == '\\')) {
            ++p;
        }
        if (clen == 0U) {
            continue;
        }

        make_83_pattern(comp, pat);
        if (!scan_dir_entries(cli, vol, cur_dir, pat, &ent) || (!ent.found)) {
            return false;
        }
        if (*p != '\0') {
            if (!ent.is_dir) {
                return false;
            }
            cur_dir = ent.start_cluster;
        }
    }

    *out_entry = ent;
    return true;
}

/* =========================================================================
 * High-Level Client Commands (info, ls, get, cat, dump, write-sector, shell)
 * ========================================================================= */

static int cmd_info(olnk_client_t *cli) {
    size_t i;
    printf("Connected @ %lu bps | server_id=0x%08lX | max_block=%u | DOS %u.%02u (server proto %u.%u)\n",
           (unsigned long)olnk_baud_index_to_bps(cli->link.current_baud_idx),
           (unsigned long)cli->link.server_id,
           (unsigned int)cli->link.max_block_size,
           (unsigned int)cli->map.server_dos_major,
           (unsigned int)cli->map.server_dos_minor,
           (unsigned int)cli->map.server_proto_major,
           (unsigned int)cli->map.server_proto_minor);

    printf("Server drives reported: %u\n", (unsigned int)cli->map.server_drive_count);
    for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
        uint8_t entry = cli->map.drive_map_resp[i];
        uint8_t u;
        olnk_get_drive_info_resp_t ginfo;
        olnk_media_check_resp_t mchk;
        olnk_build_bpb_resp_t bpb;
        olnk_generic_ioctl_mid_t mid;
        uint16_t mid_st = OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_COMMAND);

        if ((entry == OLNK_MAP_AUTO) || (entry == OLNK_MAP_NONE)) {
            continue;
        }
        u = (uint8_t)(entry & 0x7FU);
        memset(&ginfo, 0, sizeof(ginfo));
        memset(&mchk, 0, sizeof(mchk));
        memset(&bpb, 0, sizeof(bpb));
        memset(&mid, 0, sizeof(mid));

        (void)olnk_client_get_drive_info(cli, u, &ginfo);
        (void)olnk_client_media_check(cli, u, 0xF8U, &mchk);
        (void)olnk_client_build_bpb(cli, u, 0xF8U, &bpb);
        (void)olnk_client_get_media_id(cli, u, &mid_st, &mid);

        printf("  Slot %u -> Server %c: (unit=%u, attr=0x%04X%s) size='%s' label='%s'\n",
               (unsigned int)i,
               (char)('A' + (char)u),
               (unsigned int)u,
               (unsigned int)cli->map.server_drive_attrs[u],
               ((entry & OLNK_MAP_READONLY_BIT) != 0U) ? ", RO" : "",
               ginfo.size_string,
               ginfo.volume_label);

        if (((bpb.status & OLNK_DOS_STATUS_ERROR) == 0U) && bpb.has_bpb) {
            uint32_t tot = (bpb.bpb.total_sectors_16 != 0U)
                               ? (uint32_t)bpb.bpb.total_sectors_16
                               : bpb.bpb.total_sectors_32;
            printf("    BPB: bps=%u spc=%u res=%u fats=%u root=%u spf=%u total=%lu media=0x%02X\n",
                   (unsigned int)bpb.bpb.bytes_per_sector,
                   (unsigned int)bpb.bpb.sectors_per_cluster,
                   (unsigned int)bpb.bpb.reserved_sectors,
                   (unsigned int)bpb.bpb.num_fats,
                   (unsigned int)bpb.bpb.root_entries,
                   (unsigned int)bpb.bpb.sectors_per_fat,
                   (unsigned long)tot,
                   (unsigned int)bpb.bpb.media_descriptor);
        } else {
            printf("    BPB: status=0x%04X (%s)\n", (unsigned int)bpb.status, dos_err_name(bpb.status));
        }

        if ((mid_st & OLNK_DOS_STATUS_ERROR) == 0U) {
            printf("    MID: serial=%08lX label='%.11s' fs='%.8s'\n",
                   (unsigned long)mid.serial_number,
                   (const char *)mid.volume_label,
                   (const char *)mid.filesystem_type);
        }
    }
    return 0;
}

static int cmd_ls(olnk_client_t *cli, uint8_t unit, const char *subdir) {
    remote_fat_vol_t vol;
    fat_dirent_t dir;

    if (!remote_fat_mount(cli, unit, &vol)) {
        return 1;
    }

    if ((subdir != NULL) && (subdir[0] != '\0')) {
        if (!resolve_fat_path(cli, &vol, subdir, &dir) || (!dir.is_dir)) {
            fprintf(stderr, "[openlnk] ERROR: Directory not found: %s\n", subdir);
            return 1;
        }
        printf("Directory of %c:\\%s (%s, %lu sectors):\n",
               (char)('A' + (char)unit), subdir,
               vol.is_fat12 ? "FAT12" : "FAT16",
               (unsigned long)vol.total_sectors);
        return scan_dir_entries(cli, &vol, dir.start_cluster, NULL, NULL) ? 0 : 1;
    }

    printf("Directory of %c:\\ (%s, %lu sectors):\n",
           (char)('A' + (char)unit),
           vol.is_fat12 ? "FAT12" : "FAT16",
           (unsigned long)vol.total_sectors);
    return scan_dir_entries(cli, &vol, 0U, NULL, NULL) ? 0 : 1;
}

static int cmd_get(olnk_client_t *cli, uint8_t unit, const char *remote_path, const char *out_path) {
    static uint8_t cluster_buf[OLNK_MAX_PAYLOAD_SIZE];
    remote_fat_vol_t vol;
    fat_dirent_t ent;
    FILE *out_fp = stdout;
    bool close_out = false;
    uint16_t cur_cluster;
    uint32_t remaining;

    if (!remote_fat_mount(cli, unit, &vol)) {
        return 1;
    }
    if (!resolve_fat_path(cli, &vol, remote_path, &ent) || ent.is_dir) {
        fprintf(stderr, "[openlnk] ERROR: File not found: %s\n", remote_path);
        return 1;
    }

    if ((out_path != NULL) && (strcmp(out_path, "-") != 0)) {
        out_fp = fopen(out_path, "wb");
        if (out_fp == NULL) {
            fprintf(stderr, "[openlnk] ERROR: Cannot open '%s' for writing\n", out_path);
            return 1;
        }
        close_out = true;
    }

    cur_cluster = ent.start_cluster;
    remaining = ent.file_size;

    while ((remaining > 0UL) && (!remote_fat_is_eof(&vol, cur_cluster))) {
        uint32_t lba = vol.data_start_lba +
                       ((uint32_t)(cur_cluster - 2U) * (uint32_t)vol.bpb.sectors_per_cluster);
        uint16_t secs_left = (uint16_t)vol.bpb.sectors_per_cluster;
        uint16_t max_secs = (uint16_t)(sizeof(cluster_buf) / (size_t)vol.bps);
        if (max_secs == 0U) {
            max_secs = 1U;
        }

        while ((secs_left > 0U) && (remaining > 0UL)) {
            uint16_t chunk = (secs_left > max_secs) ? max_secs : secs_left;
            olnk_sector_io_resp_t ior;
            uint32_t avail_bytes;
            uint32_t to_write;

            if ((olnk_client_read_sectors(
                     cli, vol.unit, vol.bpb.media_descriptor, lba, chunk, vol.bps,
                     cluster_buf, (uint32_t)sizeof(cluster_buf), &ior) != OLNK_OK) ||
                ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U)) {
                fprintf(stderr, "[openlnk] ERROR: Sector read failed at LBA %lu (0x%04X)\n",
                        (unsigned long)lba, (unsigned int)ior.status);
                if (close_out) {
                    fclose(out_fp);
                }
                return 1;
            }

            avail_bytes = (uint32_t)ior.sectors_transferred * (uint32_t)vol.bps;
            to_write = (remaining < avail_bytes) ? remaining : avail_bytes;
            if (fwrite(cluster_buf, 1U, (size_t)to_write, out_fp) != (size_t)to_write) {
                fprintf(stderr, "[openlnk] ERROR: Local write failed\n");
                if (close_out) {
                    fclose(out_fp);
                }
                return 1;
            }
            remaining -= to_write;
            lba += (uint32_t)chunk;
            secs_left = (uint16_t)(secs_left - chunk);
        }

        if (remaining > 0UL) {
            if (!remote_fat_next_cluster(cli, &vol, cur_cluster, &cur_cluster)) {
                fprintf(stderr, "[openlnk] ERROR: Failed reading FAT chain\n");
                if (close_out) {
                    fclose(out_fp);
                }
                return 1;
            }
        }
    }

    if (close_out) {
        fclose(out_fp);
        fprintf(stderr, "[openlnk] Downloaded %c:\\%s -> %s (%lu bytes)\n",
                (char)('A' + (char)unit), remote_path, out_path, (unsigned long)ent.file_size);
    } else {
        fflush(out_fp);
    }
    return 0;
}

static int cmd_dump(
    olnk_client_t *cli,
    uint8_t unit,
    const char *out_path,
    uint32_t start_lba,
    uint32_t count_sectors
) {
    static uint8_t io_buf[OLNK_MAX_PAYLOAD_SIZE];
    remote_fat_vol_t vol;
    FILE *fp;
    uint32_t done = 0UL;
    uint32_t total;
    uint16_t max_chunk;

    if (!remote_fat_mount(cli, unit, &vol)) {
        return 1;
    }
    if (start_lba >= vol.total_sectors) {
        fprintf(stderr, "[openlnk] ERROR: start LBA %lu out of range (total %lu)\n",
                (unsigned long)start_lba, (unsigned long)vol.total_sectors);
        return 1;
    }
    total = (count_sectors == 0UL) ? (vol.total_sectors - start_lba) : count_sectors;
    if ((start_lba + total) > vol.total_sectors) {
        total = vol.total_sectors - start_lba;
    }

    fp = fopen(out_path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "[openlnk] ERROR: Cannot open '%s' for writing\n", out_path);
        return 1;
    }

    max_chunk = (uint16_t)(cli->link.max_block_size / vol.bps);
    if (max_chunk == 0U) {
        max_chunk = 1U;
    }

    while ((done < total) && (!g_stop_requested)) {
        uint32_t rem = total - done;
        uint16_t chunk = (rem > (uint32_t)max_chunk) ? max_chunk : (uint16_t)rem;
        olnk_sector_io_resp_t ior;
        size_t nbytes;

        if ((olnk_client_read_sectors(
                 cli, unit, vol.bpb.media_descriptor, start_lba + done, chunk, vol.bps,
                 io_buf, (uint32_t)sizeof(io_buf), &ior) != OLNK_OK) ||
            ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U)) {
            fprintf(stderr, "[openlnk] ERROR: Read failed at LBA %lu (status=0x%04X %s)\n",
                    (unsigned long)(start_lba + done),
                    (unsigned int)ior.status,
                    dos_err_name(ior.status));
            fclose(fp);
            return 1;
        }
        nbytes = (size_t)ior.sectors_transferred * (size_t)vol.bps;
        if (fwrite(io_buf, 1U, nbytes, fp) != nbytes) {
            fclose(fp);
            return 1;
        }
        done += (uint32_t)ior.sectors_transferred;
    }

    fclose(fp);
    fprintf(stderr, "[openlnk] Dumped %lu sectors (%lu bytes) from %c: LBA %lu -> %s\n",
            (unsigned long)done, (unsigned long)(done * (uint32_t)vol.bps),
            (char)('A' + (char)unit), (unsigned long)start_lba, out_path);
    return 0;
}

static int cmd_write_sector(
    olnk_client_t *cli,
    uint8_t unit,
    uint32_t start_lba,
    const char *in_path,
    bool verify
) {
    static uint8_t io_buf[OLNK_MAX_PAYLOAD_SIZE];
    remote_fat_vol_t vol;
    FILE *fp;
    size_t nread;
    uint16_t sec_cnt;
    olnk_sector_io_resp_t ior;

    if (!remote_fat_mount(cli, unit, &vol)) {
        return 1;
    }
    fp = fopen(in_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "[openlnk] ERROR: Cannot open '%s'\n", in_path);
        return 1;
    }
    memset(io_buf, 0, sizeof(io_buf));
    nread = fread(io_buf, 1U, sizeof(io_buf), fp);
    fclose(fp);

    if (nread == 0U) {
        fprintf(stderr, "[openlnk] ERROR: Input file '%s' is empty\n", in_path);
        return 1;
    }
    sec_cnt = (uint16_t)((nread + (size_t)vol.bps - 1U) / (size_t)vol.bps);

    if (olnk_client_write_sectors(
            cli, unit, vol.bpb.media_descriptor, start_lba, sec_cnt, vol.bps,
            io_buf, verify, &ior) != OLNK_OK) {
        fprintf(stderr, "[openlnk] ERROR: Transport error during WRITE_SECTORS\n");
        return 1;
    }
    printf("WRITE_%s unit=%u (%c:) lba=%lu sectors=%u -> status=0x%04X (%s), transferred=%u\n",
           verify ? "VERIFY" : "SECTORS",
           (unsigned int)unit, (char)('A' + (char)unit),
           (unsigned long)start_lba, (unsigned int)sec_cnt,
           (unsigned int)ior.status, dos_err_name(ior.status),
           (unsigned int)ior.sectors_transferred);
    return ((ior.status & OLNK_DOS_STATUS_ERROR) == 0U) ? 0 : 1;
}

static int cmd_shell(olnk_client_t *cli, uint8_t default_unit) {
    char line[256];
    uint8_t cur_unit = default_unit;

    printf("OpenLNK interactive shell (unit %c:, %lu bps). Type 'help' or 'quit'.\n",
           (char)('A' + (char)cur_unit),
           (unsigned long)olnk_baud_index_to_bps(cli->link.current_baud_idx));

    while (!g_stop_requested) {
        fd_set rfds;
        struct timeval tv;
        int rc;

        printf("openlnk %c:> ", (char)('A' + (char)cur_unit));
        fflush(stdout);

        /* Poll stdin while keeping the MS-DOS server link alive with keepalives */
        for (;;) {
            if (g_stop_requested) {
                return 0;
            }
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            tv.tv_sec = 0;
            tv.tv_usec = 100000; /* 100 ms keepalive cadence while idle */
            rc = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
            if (rc > 0) {
                break;
            }
            if ((rc < 0) && (errno != EINTR)) {
                return 0;
            }
            if (olnk_client_keepalive(cli) == OLNK_ERR_DISCONNECTED) {
                fprintf(stderr, "\n[openlnk] Link lost (keepalive timeout).\n");
                return 1;
            }
        }

        if (fgets(line, (int)sizeof(line), stdin) == NULL) {
            break;
        }
        {
            char *cmd = strtok(line, " \t\r\n");
            char *arg1 = strtok(NULL, " \t\r\n");
            char *arg2 = strtok(NULL, " \t\r\n");
            if (cmd == NULL) {
                continue;
            }
            if ((strcmp(cmd, "quit") == 0) || (strcmp(cmd, "exit") == 0)) {
                break;
            } else if (strcmp(cmd, "help") == 0) {
                printf("Commands: info | unit <L|N> | ls [dir] | cat <file> | get <file> <out> | dump <out> [count] | ping | quit\n");
            } else if (strcmp(cmd, "info") == 0) {
                (void)cmd_info(cli);
            } else if (strcmp(cmd, "unit") == 0) {
                uint8_t u = cur_unit;
                if ((arg1 != NULL) && parse_unit_spec(arg1, &u)) {
                    cur_unit = u;
                } else {
                    printf("Usage: unit <A..Z | 0..25>\n");
                }
            } else if ((strcmp(cmd, "ls") == 0) || (strcmp(cmd, "dir") == 0)) {
                (void)cmd_ls(cli, cur_unit, arg1);
            } else if (strcmp(cmd, "cat") == 0) {
                if (arg1 != NULL) {
                    (void)cmd_get(cli, cur_unit, arg1, "-");
                    putchar('\n');
                } else {
                    printf("Usage: cat <remote_path>\n");
                }
            } else if (strcmp(cmd, "get") == 0) {
                if ((arg1 != NULL) && (arg2 != NULL)) {
                    (void)cmd_get(cli, cur_unit, arg1, arg2);
                } else {
                    printf("Usage: get <remote_path> <local_file>\n");
                }
            } else if (strcmp(cmd, "dump") == 0) {
                if (arg1 != NULL) {
                    uint32_t cnt = (arg2 != NULL) ? (uint32_t)strtoul(arg2, NULL, 10) : 0UL;
                    (void)cmd_dump(cli, cur_unit, arg1, 0UL, cnt);
                } else {
                    printf("Usage: dump <local_img> [sector_count]\n");
                }
            } else if (strcmp(cmd, "ping") == 0) {
                olnk_status_t st = olnk_client_keepalive(cli);
                printf("Keepalive: %s (total=%lu)\n",
                       (st == OLNK_OK) ? "OK" : "TIMEOUT",
                       (unsigned long)cli->link.keepalives_ok);
            } else {
                printf("Unknown command '%s'. Type 'help'.\n", cmd);
            }
        }
    }
    return 0;
}

#endif /* !_WIN32 */

/* =========================================================================
 * CLI Entrypoint
 * ========================================================================= */

static void print_usage(void) {
    printf("openlnk (libopenlnk 0.2.0 — MS-DOS 6.22 INTERLNK clean-room client)\n");
    printf("Usage:\n");
    printf("  openlnk --version\n");
    printf("  openlnk --self-test\n");
    printf("  openlnk --pipe <prefix> [options] <command> [args...]\n");
    printf("  openlnk --port <tty/pty> [options] <command> [args...]\n");
    printf("  openlnk --peer-pipe <prefix> [options] <command> [args...]\n");
    printf("Options:\n");
    printf("  --baud <rate>        Max negotiated baud (1200..115200, default 115200)\n");
    printf("  --unit <L|N>         Target server drive letter (e.g. C) or unit index (0..25)\n");
    printf("  --retries <N>        Phase A sync retries (default 25)\n");
    printf("  --force-echo         Force byte-by-byte Echo pacing on TX (for XT/8250 without /V)\n");
    printf("  --burst              Allow Burst (no-echo) RX from server (default: Echo-Paced RX)\n");
    printf("  --pace-us <us>       Per-byte TX delay in microseconds for pipe modes\n");
    printf("Commands:\n");
    printf("  info                                     Connect & display remote drives, BPB, Media ID\n");
    printf("  ls [subdir]                              List remote FAT12/FAT16 directory\n");
    printf("  cat <remote_path>                        Print remote file to stdout\n");
    printf("  get <remote_path> -o <local_file>        Download file from remote FAT12/FAT16 drive\n");
    printf("  dump -o <out.img> [--start L] [--count N] Dump raw partition sectors to image file\n");
    printf("  write-sector --lba <L> --in <file> [--verify] Write sector(s) to remote drive\n");
    printf("  shell                                    Interactive session with automatic keepalives\n");
}

int main(int argc, char **argv) {
    const char *port_path = NULL;
    const char *pipe_prefix = NULL;
    bool peer_pipe = false;
    uint32_t max_baud = 115200UL;
    uint8_t max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    uint16_t sync_retries = 25U;
    uint32_t pace_us = 0UL;
    bool force_echo = false;
    bool rx_echo = true;
    bool unit_explicit = false;
    uint8_t target_unit = 0U;
    const char *command = NULL;
    const char *cmd_arg = NULL;
    const char *out_path = NULL;
    const char *in_path = NULL;
    uint32_t start_lba = 0UL;
    uint32_t sector_count = 0UL;
    bool verify_write = false;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("openlnk %s (libopenlnk C99 zero-alloc MS-DOS 6.22 client)\n",
                   OLNK_VERSION_STRING);
            return 0;
        }
        if (strcmp(argv[i], "--self-test") == 0) {
            return run_self_test();
        }
        if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
            print_usage();
            return 0;
        }
        if ((strcmp(argv[i], "--port") == 0) && ((i + 1) < argc)) {
            port_path = argv[++i];
        } else if ((strcmp(argv[i], "--pipe") == 0) && ((i + 1) < argc)) {
            pipe_prefix = argv[++i];
            peer_pipe = false;
        } else if ((strcmp(argv[i], "--peer-pipe") == 0) && ((i + 1) < argc)) {
            pipe_prefix = argv[++i];
            peer_pipe = true;
        } else if ((strcmp(argv[i], "--baud") == 0) && ((i + 1) < argc)) {
            max_baud = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (!olnk_bps_to_baud_index(max_baud, &max_baud_idx)) {
                fprintf(stderr, "[openlnk] ERROR: Unsupported baud rate %lu\n", (unsigned long)max_baud);
                return 2;
            }
        } else if ((strcmp(argv[i], "--unit") == 0) && ((i + 1) < argc)) {
            const char *uspec = argv[++i];
#if !defined(_WIN32)
            if (!parse_unit_spec(uspec, &target_unit)) {
                fprintf(stderr, "[openlnk] ERROR: Invalid unit '%s'\n", uspec);
                return 2;
            }
#else
            (void)uspec;
#endif
            unit_explicit = true;
        } else if ((strcmp(argv[i], "--retries") == 0) && ((i + 1) < argc)) {
            sync_retries = (uint16_t)strtoul(argv[++i], NULL, 10);
        } else if ((strcmp(argv[i], "--pace-us") == 0) && ((i + 1) < argc)) {
            pace_us = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--force-echo") == 0) {
            force_echo = true;
        } else if (strcmp(argv[i], "--burst") == 0) {
            rx_echo = false;
        } else if (((strcmp(argv[i], "-o") == 0) || (strcmp(argv[i], "--out") == 0)) && ((i + 1) < argc)) {
            out_path = argv[++i];
        } else if ((strcmp(argv[i], "--in") == 0) && ((i + 1) < argc)) {
            in_path = argv[++i];
        } else if (((strcmp(argv[i], "--lba") == 0) || (strcmp(argv[i], "--start") == 0)) && ((i + 1) < argc)) {
            start_lba = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if ((strcmp(argv[i], "--count") == 0) && ((i + 1) < argc)) {
            sector_count = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--verify") == 0) {
            verify_write = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "[openlnk] ERROR: Unknown option '%s'\n", argv[i]);
            print_usage();
            return 2;
        } else if (command == NULL) {
            command = argv[i];
        } else if (cmd_arg == NULL) {
            cmd_arg = argv[i];
        } else {
            fprintf(stderr, "[openlnk] ERROR: Unexpected extra argument '%s'\n", argv[i]);
            return 2;
        }
    }

    if ((port_path == NULL) && (pipe_prefix == NULL)) {
        print_usage();
        return 0;
    }
    if (command == NULL) {
        command = "info";
    }

#if defined(_WIN32)
    (void)peer_pipe;
    (void)max_baud_idx;
    (void)sync_retries;
    (void)pace_us;
    (void)force_echo;
    (void)rx_echo;
    (void)unit_explicit;
    (void)target_unit;
    (void)cmd_arg;
    (void)out_path;
    (void)in_path;
    (void)start_lba;
    (void)sector_count;
    (void)verify_write;
    fprintf(stderr, "[openlnk] POSIX --port/--pipe client mode requires Linux or macOS.\n");
    return 1;
#else
    {
        static olnk_client_t cli;
        cli_posix_hal_t hal;
        olnk_transport_t tr;
        olnk_session_sync_resp_t sresp;
        olnk_map_config_resp_t mresp;
        int rc = 0;

        hal.rx_fd = -1;
        hal.tx_fd = -1;
        hal.is_serial = false;
        hal.pace_us = pace_us;

        if (pipe_prefix != NULL) {
            if (!cli_open_pipe(pipe_prefix, peer_pipe, &hal)) {
                return 1;
            }
        } else {
            if (!cli_open_serial(port_path, &hal)) {
                return 1;
            }
        }

        tr.read = cli_hal_read;
        tr.write = cli_hal_write;
        tr.set_baud = cli_hal_set_baud;
        tr.user_data = &hal;

        (void)olnk_client_init(&cli, tr);
        cli.link.max_baud_idx = max_baud_idx;
        cli.link.local_requires_echo = rx_echo;

        signal(SIGINT, handle_signal);
        signal(SIGTERM, handle_signal);

        fprintf(stderr, "[openlnk] Connecting (max_baud=%lu, retries=%u)...\n",
                (unsigned long)olnk_baud_index_to_bps(max_baud_idx),
                (unsigned int)sync_retries);

        if (olnk_client_connect(&cli, sync_retries) != OLNK_OK) {
            fprintf(stderr, "[openlnk] ERROR: Phase A sync failed (is INTERSVR / opensvr running?).\n");
            cli_close_hal(&hal);
            return 1;
        }
        if (force_echo) {
            cli.link.peer_echo_forced = true;
        }

        if (olnk_client_session_sync(&cli, false, &sresp) != OLNK_OK) {
            fprintf(stderr, "[openlnk] ERROR: Opcode 0x00 SESSION_SYNC failed.\n");
            cli_close_hal(&hal);
            return 1;
        }
        if (olnk_client_map_config(&cli, 8U, 3U, &mresp) != OLNK_OK) {
            fprintf(stderr, "[openlnk] ERROR: Opcode 0x01 MAP_CONFIG failed.\n");
            cli_close_hal(&hal);
            return 1;
        }

        if (!unit_explicit) {
            if (!select_default_unit(&cli, &target_unit)) {
                target_unit = 0U;
            }
        }

        if (strcmp(command, "info") == 0) {
            rc = cmd_info(&cli);
        } else if ((strcmp(command, "ls") == 0) || (strcmp(command, "dir") == 0)) {
            rc = cmd_ls(&cli, target_unit, cmd_arg);
        } else if (strcmp(command, "cat") == 0) {
            if (cmd_arg == NULL) {
                fprintf(stderr, "[openlnk] ERROR: cat requires <remote_path>\n");
                rc = 2;
            } else {
                rc = cmd_get(&cli, target_unit, cmd_arg, "-");
            }
        } else if (strcmp(command, "get") == 0) {
            if (cmd_arg == NULL) {
                fprintf(stderr, "[openlnk] ERROR: get requires <remote_path> [-o <local_file>]\n");
                rc = 2;
            } else {
                rc = cmd_get(&cli, target_unit, cmd_arg, (out_path != NULL) ? out_path : "-");
            }
        } else if (strcmp(command, "dump") == 0) {
            if (out_path == NULL) {
                fprintf(stderr, "[openlnk] ERROR: dump requires -o <out.img>\n");
                rc = 2;
            } else {
                rc = cmd_dump(&cli, target_unit, out_path, start_lba, sector_count);
            }
        } else if (strcmp(command, "write-sector") == 0) {
            if (in_path == NULL) {
                fprintf(stderr, "[openlnk] ERROR: write-sector requires --in <file>\n");
                rc = 2;
            } else {
                rc = cmd_write_sector(&cli, target_unit, start_lba, in_path, verify_write);
            }
        } else if (strcmp(command, "shell") == 0) {
            rc = cmd_shell(&cli, target_unit);
        } else {
            fprintf(stderr, "[openlnk] ERROR: Unknown command '%s'\n", command);
            print_usage();
            rc = 2;
        }

        cli_close_hal(&hal);
        return rc;
    }
#endif
}
