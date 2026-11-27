/* cleanroom/desktop/main_server.c
 *
 * Clean-room MS-DOS 6.22 INTERSVR-compatible server CLI (opensvr) built on
 * libopenlnk (zero-alloc C99 core).
 *
 * Supports:
 *   --version              Print version info
 *   --self-test            Full in-memory loopback verification (Sync, Baud, RPCs, 16 KiB I/O)
 *   --port <tty/pty>       POSIX termios raw 8N1 serial transport (macOS/Linux)
 *   --pipe <prefix>        86Box Named-Pipe (.in/.out) FIFO transport (macOS/Linux)
 *   --image <path>         Attach RAW FAT12/FAT16 or Fixed-Size VHD image (auto unit)
 *   --drive <L>:<path>     Attach image to explicit server drive letter A..Z
 *   --baud <rate>          Maximum negotiated baud rate (1200..115200, default 115200)
 *   --read-only            Expose all drives read-only (returns 0x8100 on writes)
 *   --burst                Allow Burst (no-echo) RX mode (default: Echo-Paced RX)
 *   --pace-us <us>         Optional TX pacing delay per byte in --pipe mode
 *   --verbose / -v         Log link and RPC events to stderr
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

#include "openlnk/openlnk.h"
#include "openlnk/server.h"
#include "openlnk/storage.h"
#include "vfat_dir.h"

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
 * Cross-Platform Raw / Fixed-VHD File Storage Backend
 * ========================================================================= */

typedef struct {
    FILE *fp;
    vfat_dir_drive_t *vfat;
    char path[512];
    uint32_t raw_sectors;
    uint32_t partition_lba;
    uint32_t volume_sectors;
    olnk_drive_info_t info;
    bool present;
    bool write_protected;
} file_drive_slot_t;

typedef struct {
    file_drive_slot_t slots[OLNK_MAX_DRIVES];
    uint8_t drive_count;
} file_drive_set_t;

static bool file_seek_sector(FILE *fp, uint32_t phys_lba) {
    uint64_t byte_off = (uint64_t)phys_lba * (uint64_t)OLNK_SECTOR_SIZE;
#if !defined(_WIN32)
    return fseeko(fp, (off_t)byte_off, SEEK_SET) == 0;
#else
    if (byte_off > 0x7FFFFFFFULL) {
        return false;
    }
    return fseek(fp, (long)byte_off, SEEK_SET) == 0;
#endif
}

static uint64_t file_get_size(FILE *fp) {
#if !defined(_WIN32)
    off_t sz;
    if (fseeko(fp, 0, SEEK_END) != 0) {
        return 0ULL;
    }
    sz = ftello(fp);
    return (sz > 0) ? (uint64_t)sz : 0ULL;
#else
    long sz;
    if (fseek(fp, 0L, SEEK_END) != 0) {
        return 0ULL;
    }
    sz = ftell(fp);
    return (sz > 0L) ? (uint64_t)sz : 0ULL;
#endif
}

static bool file_drive_refresh_vbr(file_drive_slot_t *slot) {
    uint8_t sec0[OLNK_SECTOR_SIZE];
    uint8_t vbr[OLNK_SECTOR_SIZE];
    uint32_t part_lba = 0UL;
    uint32_t part_sec = 0UL;

    if ((slot == NULL) || (slot->fp == NULL) || (slot->raw_sectors == 0UL)) {
        return false;
    }

    if (!file_seek_sector(slot->fp, 0UL) ||
        (fread(sec0, 1U, OLNK_SECTOR_SIZE, slot->fp) != OLNK_SECTOR_SIZE)) {
        return false;
    }

    slot->partition_lba = 0UL;
    slot->volume_sectors = slot->raw_sectors;
    memcpy(vbr, sec0, OLNK_SECTOR_SIZE);

    if (olnk_mbr_find_fat_partition(sec0, &part_lba, &part_sec)) {
        if ((part_lba < slot->raw_sectors) &&
            ((part_lba + part_sec) <= slot->raw_sectors)) {
            if (file_seek_sector(slot->fp, part_lba) &&
                (fread(vbr, 1U, OLNK_SECTOR_SIZE, slot->fp) == OLNK_SECTOR_SIZE)) {
                slot->partition_lba = part_lba;
                slot->volume_sectors = part_sec;
            }
        }
    }

    if (!olnk_fat_parse_vbr(vbr, &slot->info)) {
        memset(&slot->info, 0, sizeof(slot->info));
        slot->info.bpb.bytes_per_sector = OLNK_SECTOR_SIZE;
        slot->info.bpb.sectors_per_cluster = 1U;
        slot->info.bpb.reserved_sectors = 1U;
        slot->info.bpb.num_fats = 2U;
        slot->info.bpb.root_entries = 224U;
        if (slot->volume_sectors <= 65535UL) {
            slot->info.bpb.total_sectors_16 = (uint16_t)slot->volume_sectors;
        } else {
            slot->info.bpb.total_sectors_32 = slot->volume_sectors;
        }
        slot->info.bpb.media_descriptor = 0xF8U;
        slot->info.bpb.sectors_per_fat = 9U;
        slot->info.bpb.sectors_per_track = 17U;
        slot->info.bpb.num_heads = 4U;
        slot->info.total_sectors = slot->volume_sectors;
        slot->info.drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        slot->info.present = true;
        slot->info.ready = true;
        memcpy(slot->info.volume_label, "OPENLNK    ", 12U);
        memcpy(slot->info.filesystem_type, "FAT16   ", 9U);
    }

    if (slot->info.total_sectors > slot->volume_sectors) {
        slot->info.total_sectors = slot->volume_sectors;
    }
    slot->info.write_protected = slot->write_protected;
    return true;
}

static bool file_drive_attach(
    file_drive_set_t *set,
    uint8_t unit,
    const char *path,
    bool read_only
) {
    file_drive_slot_t *slot;
    uint64_t fsize;
    uint8_t tail[OLNK_SECTOR_SIZE];
    bool is_vhd = false;
    char size_str[10];
    char trim_lbl[12];

    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES) || (path == NULL)) {
        return false;
    }
    slot = &set->slots[unit];
    memset(slot, 0, sizeof(*slot));

    slot->write_protected = read_only;
    if (!read_only) {
        slot->fp = fopen(path, "r+b");
        if (slot->fp == NULL) {
            slot->fp = fopen(path, "rb");
            slot->write_protected = true;
        }
    } else {
        slot->fp = fopen(path, "rb");
    }

    if (slot->fp == NULL) {
        fprintf(stderr, "[opensvr] ERROR: Failed to open image '%s'\n", path);
        return false;
    }

    fsize = file_get_size(slot->fp);
    if (fsize < (uint64_t)OLNK_SECTOR_SIZE) {
        fprintf(stderr, "[opensvr] ERROR: Image '%s' is smaller than 512 bytes\n", path);
        fclose(slot->fp);
        slot->fp = NULL;
        return false;
    }

    /* Strip Fixed-Size VHD 512-byte "conectix" footer if present */
    if ((fsize >= 1024ULL) && ((fsize % OLNK_SECTOR_SIZE) == 0ULL)) {
        uint32_t last_sec = (uint32_t)((fsize / OLNK_SECTOR_SIZE) - 1ULL);
        if (file_seek_sector(slot->fp, last_sec) &&
            (fread(tail, 1U, OLNK_SECTOR_SIZE, slot->fp) == OLNK_SECTOR_SIZE) &&
            olnk_vhd_has_fixed_footer(tail)) {
            fsize -= OLNK_SECTOR_SIZE;
            is_vhd = true;
        }
    }

    slot->raw_sectors = (uint32_t)(fsize / OLNK_SECTOR_SIZE);
    if (!file_drive_refresh_vbr(slot)) {
        fprintf(stderr, "[opensvr] ERROR: Could not read boot sector from '%s'\n", path);
        fclose(slot->fp);
        slot->fp = NULL;
        return false;
    }

    slot->present = true;
    strncpy(slot->path, path, sizeof(slot->path) - 1U);
    if ((uint8_t)(unit + 1U) > set->drive_count) {
        set->drive_count = (uint8_t)(unit + 1U);
    }

    olnk_fat_format_size_string(slot->info.total_sectors, slot->info.bpb.bytes_per_sector, size_str);
    olnk_label_trim_copy(trim_lbl, sizeof(trim_lbl), slot->info.volume_label, 11U);
    fprintf(
        stderr,
        "[opensvr] Unit %u (%c:): %s%s | part_lba=%lu sectors=%lu %s | fs=%.8s label='%s' ser=%08lX%s\n",
        (unsigned int)unit,
        (char)('A' + (char)unit),
        path,
        is_vhd ? " [VHD]" : " [RAW]",
        (unsigned long)slot->partition_lba,
        (unsigned long)slot->volume_sectors,
        size_str,
        slot->info.filesystem_type,
        trim_lbl,
        (unsigned long)slot->info.volume_serial,
        slot->write_protected ? " [READ-ONLY]" : ""
    );
    return true;
}

static bool file_drive_attach_dir(
    file_drive_set_t *set,
    uint8_t unit,
    const char *dir_path,
    bool read_only,
    const char *volume_label
) {
    file_drive_slot_t *slot;
    vfat_dir_drive_t *vfat;
    olnk_drive_info_t info;
    char size_str[10];
    char trim_lbl[12];

    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES) || (dir_path == NULL)) {
        return false;
    }

    vfat = vfat_dir_open(dir_path, read_only, volume_label);
    if (vfat == NULL) {
        return false;
    }

    slot = &set->slots[unit];
    memset(slot, 0, sizeof(*slot));
    slot->vfat = vfat;
    slot->present = true;
    slot->write_protected = read_only;
    strncpy(slot->path, dir_path, sizeof(slot->path) - 1U);
    (void)vfat_dir_get_info(vfat, &info);
    slot->info = info;
    slot->volume_sectors = info.total_sectors;

    if ((uint8_t)(unit + 1U) > set->drive_count) {
        set->drive_count = (uint8_t)(unit + 1U);
    }

    olnk_fat_format_size_string(info.total_sectors, info.bpb.bytes_per_sector, size_str);
    olnk_label_trim_copy(trim_lbl, sizeof(trim_lbl), info.volume_label, 11U);
    fprintf(
        stderr,
        "[opensvr] Unit %u (%c:): %s [VFAT-DIR] | sectors=%lu %s | fs=FAT16 label='%s' ser=%08lX%s\n",
        (unsigned int)unit,
        (char)('A' + (char)unit),
        dir_path,
        (unsigned long)info.total_sectors,
        size_str,
        trim_lbl,
        (unsigned long)info.volume_serial,
        read_only ? " [READ-ONLY]" : " [READ-WRITE]"
    );
    return true;
}

static bool file_drive_path_is_dir(const char *path) {
#if !defined(_WIN32)
    struct stat st;
    return (path != NULL) && (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
#else
    (void)path;
    return false;
#endif
}

static void file_drive_set_close(file_drive_set_t *set) {
    uint8_t i;
    for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
        if (set->slots[i].vfat != NULL) {
            vfat_dir_close(set->slots[i].vfat);
            set->slots[i].vfat = NULL;
        }
        if (set->slots[i].fp != NULL) {
            fflush(set->slots[i].fp);
            fclose(set->slots[i].fp);
            set->slots[i].fp = NULL;
        }
    }
}

#if !defined(_WIN32)
static uint8_t fset_drive_count(void *ud) {
    const file_drive_set_t *set = (const file_drive_set_t *)ud;
    return (set != NULL) ? set->drive_count : 0U;
}

static uint16_t fset_get_info(uint8_t unit, olnk_drive_info_t *out, void *ud) {
    file_drive_set_t *set = (file_drive_set_t *)ud;
    if ((set == NULL) || (out == NULL) || (unit >= OLNK_MAX_DRIVES) || (!set->slots[unit].present)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (set->slots[unit].vfat != NULL) {
        return vfat_dir_get_info(set->slots[unit].vfat, out);
    }
    *out = set->slots[unit].info;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t fset_media_check(
    uint8_t unit,
    uint8_t media_desc,
    uint8_t *out_ms,
    olnk_drive_info_t *out_info,
    void *ud
) {
    file_drive_set_t *set = (file_drive_set_t *)ud;
    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES) || (!set->slots[unit].present)) {
        *out_ms = (uint8_t)OLNK_MEDIA_UNSURE;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (set->slots[unit].vfat != NULL) {
        return vfat_dir_media_check(set->slots[unit].vfat, media_desc, out_ms, out_info);
    }
    *out_info = set->slots[unit].info;
    *out_ms = (uint8_t)OLNK_MEDIA_UNCHANGED;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t fset_build_bpb(
    uint8_t unit,
    uint8_t media_desc,
    olnk_dos_bpb_t *out_bpb,
    void *ud
) {
    file_drive_set_t *set = (file_drive_set_t *)ud;
    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES) || (!set->slots[unit].present)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (set->slots[unit].vfat != NULL) {
        return vfat_dir_build_bpb(set->slots[unit].vfat, media_desc, out_bpb);
    }
    (void)file_drive_refresh_vbr(&set->slots[unit]);
    *out_bpb = set->slots[unit].info.bpb;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t fset_read_sectors(
    uint8_t unit,
    uint32_t start_lba,
    uint16_t sector_count,
    uint8_t *out_buf,
    uint16_t *out_xfer,
    void *ud
) {
    file_drive_set_t *set = (file_drive_set_t *)ud;
    file_drive_slot_t *slot;
    uint32_t end_lba;
    size_t byte_len;

    if (out_xfer != NULL) {
        *out_xfer = 0U;
    }
    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES) || (!set->slots[unit].present)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    slot = &set->slots[unit];
    if (slot->vfat != NULL) {
        return vfat_dir_read_sectors(slot->vfat, start_lba, sector_count, out_buf, out_xfer);
    }
    end_lba = start_lba + (uint32_t)sector_count;
    if ((end_lba < start_lba) || (end_lba > slot->volume_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }
    byte_len = (size_t)sector_count * OLNK_SECTOR_SIZE;
    if (!file_seek_sector(slot->fp, slot->partition_lba + start_lba) ||
        (fread(out_buf, 1U, byte_len, slot->fp) != byte_len)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
    }
    if (out_xfer != NULL) {
        *out_xfer = sector_count;
    }
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t fset_write_sectors(
    uint8_t unit,
    uint32_t start_lba,
    uint16_t sector_count,
    const uint8_t *in_buf,
    uint16_t *out_xfer,
    bool verify,
    void *ud
) {
    file_drive_set_t *set = (file_drive_set_t *)ud;
    file_drive_slot_t *slot;
    uint32_t end_lba;
    size_t byte_len;

    if (out_xfer != NULL) {
        *out_xfer = 0U;
    }
    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES) || (!set->slots[unit].present)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    slot = &set->slots[unit];
    if (slot->vfat != NULL) {
        return vfat_dir_write_sectors(slot->vfat, start_lba, sector_count, in_buf, out_xfer, verify);
    }
    if (slot->write_protected) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
    }
    end_lba = start_lba + (uint32_t)sector_count;
    if ((end_lba < start_lba) || (end_lba > slot->volume_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }
    byte_len = (size_t)sector_count * OLNK_SECTOR_SIZE;
    if (!file_seek_sector(slot->fp, slot->partition_lba + start_lba) ||
        (fwrite(in_buf, 1U, byte_len, slot->fp) != byte_len) ||
        (fflush(slot->fp) != 0)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
    }
    if (verify) {
        static uint8_t verify_buf[OLNK_MAX_PAYLOAD_SIZE];
        if ((byte_len <= sizeof(verify_buf)) &&
            (!file_seek_sector(slot->fp, slot->partition_lba + start_lba) ||
             (fread(verify_buf, 1U, byte_len, slot->fp) != byte_len) ||
             (memcmp(verify_buf, in_buf, byte_len) != 0))) {
            return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
        }
    }
    if (start_lba == 0UL) {
        (void)file_drive_refresh_vbr(slot);
    }
    if (out_xfer != NULL) {
        *out_xfer = sector_count;
    }
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t fset_set_mid(uint8_t unit, const olnk_generic_ioctl_mid_t *mid, void *ud) {
    file_drive_set_t *set = (file_drive_set_t *)ud;
    file_drive_slot_t *slot;
    uint8_t vbr[OLNK_SECTOR_SIZE];

    if ((set == NULL) || (mid == NULL) || (unit >= OLNK_MAX_DRIVES) || (!set->slots[unit].present)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    slot = &set->slots[unit];
    if (slot->vfat != NULL) {
        return vfat_dir_set_media_id(slot->vfat, mid);
    }
    if (slot->write_protected) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
    }
    if (!file_seek_sector(slot->fp, slot->partition_lba) ||
        (fread(vbr, 1U, OLNK_SECTOR_SIZE, slot->fp) != OLNK_SECTOR_SIZE)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
    }
    if (vbr[38] == 0x29U) {
        olnk_write_u32_le(&vbr[39], mid->serial_number);
        memcpy(&vbr[43], mid->volume_label, 11U);
        memcpy(&vbr[54], mid->filesystem_type, 8U);
        if (!file_seek_sector(slot->fp, slot->partition_lba) ||
            (fwrite(vbr, 1U, OLNK_SECTOR_SIZE, slot->fp) != OLNK_SECTOR_SIZE) ||
            (fflush(slot->fp) != 0)) {
            return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
        }
    }
    (void)file_drive_refresh_vbr(slot);
    return OLNK_DOS_STATUS_DONE;
}

static olnk_storage_ops_t file_drive_set_ops(file_drive_set_t *set) {
    olnk_storage_ops_t ops;
    ops.drive_count = fset_drive_count;
    ops.get_drive_info = fset_get_info;
    ops.media_check = fset_media_check;
    ops.build_bpb = fset_build_bpb;
    ops.read_sectors = fset_read_sectors;
    ops.write_sectors = fset_write_sectors;
    ops.set_media_id = fset_set_mid;
    ops.user_data = set;
    return ops;
}
#endif /* !_WIN32 */

/* =========================================================================
 * Self-Test: Full In-Memory Loopback (Sync, Baud, Echo, Multi-Frag, Errors)
 * ========================================================================= */

#define SELF_TEST_RING_CAP 16384U

typedef struct {
    uint8_t buf[SELF_TEST_RING_CAP];
    size_t head;
    size_t tail;
    size_t count;
} st_ring_t;

typedef struct {
    st_ring_t s2c;
    olnk_server_t *srv;
    uint32_t cli_baud;
    uint32_t srv_baud;
} st_loopback_t;

static void ring_push(st_ring_t *r, const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0U; i < len; ++i) {
        if (r->count < SELF_TEST_RING_CAP) {
            r->buf[r->tail] = data[i];
            r->tail = (r->tail + 1U) % SELF_TEST_RING_CAP;
            r->count += 1U;
        }
    }
}

static size_t ring_pop(st_ring_t *r, uint8_t *out, size_t len) {
    size_t n = 0U;
    while ((n < len) && (r->count > 0U)) {
        out[n++] = r->buf[r->head];
        r->head = (r->head + 1U) % SELF_TEST_RING_CAP;
        r->count -= 1U;
    }
    return n;
}

static int st_srv_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    st_loopback_t *lb = (st_loopback_t *)ud;
    (void)timeout_ms;
    ring_push(&lb->s2c, buf, len);
    return (int)len;
}

static bool st_srv_set_baud(uint32_t baud, void *ud) {
    st_loopback_t *lb = (st_loopback_t *)ud;
    lb->srv_baud = baud;
    return true;
}

static int st_cli_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    st_loopback_t *lb = (st_loopback_t *)ud;
    (void)timeout_ms;
    /* Feed bytes directly into reactive non-blocking server state machine */
    olnk_server_feed_bytes(lb->srv, buf, len);
    return (int)len;
}

static int st_cli_read(uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    st_loopback_t *lb = (st_loopback_t *)ud;
    (void)timeout_ms;
    return (int)ring_pop(&lb->s2c, buf, len);
}

static bool st_cli_set_baud(uint32_t baud, void *ud) {
    st_loopback_t *lb = (st_loopback_t *)ud;
    lb->cli_baud = baud;
    return true;
}

static int run_self_test(void) {
    static uint8_t disk_a[OLNK_SECTOR_SIZE * 64U];
    static uint8_t disk_b[OLNK_SECTOR_SIZE * 64U];
    static uint8_t tx_16k[OLNK_SECTOR_SIZE * 32U];
    static uint8_t rx_16k[OLNK_SECTOR_SIZE * 32U];
    static olnk_server_t srv;
    static olnk_ctx_t cli;
    static st_loopback_t lb;

    olnk_mem_drive_t drv;
    olnk_transport_t srv_tr;
    olnk_transport_t cli_tr;
    uint8_t msg_buf[256];
    size_t msg_len = 0U;
    uint16_t rx_len = 0U;
    uint32_t total_rx = 0UL;
    bool more = false;
    size_t i;

    memset(&lb, 0, sizeof(lb));
    memset(disk_a, 0, sizeof(disk_a));
    memset(disk_b, 0, sizeof(disk_b));
    olnk_fat_build_synthetic_vbr(disk_a, 64UL, 0xF0U, "SELF_TEST_A", 0x11223344UL);
    olnk_fat_build_synthetic_vbr(disk_b, 64UL, 0xF0U, "SELF_TEST_B", 0x55667788UL);
    olnk_mem_drive_init(&drv, disk_a, 64UL);

    lb.srv = &srv;
    lb.cli_baud = 9600UL;
    lb.srv_baud = 9600UL;

    memset(&srv_tr, 0, sizeof(srv_tr));
    srv_tr.write = st_srv_write;
    srv_tr.set_baud = st_srv_set_baud;
    srv_tr.user_data = &lb;

    memset(&cli_tr, 0, sizeof(cli_tr));
    cli_tr.read = st_cli_read;
    cli_tr.write = st_cli_write;
    cli_tr.set_baud = st_cli_set_baud;
    cli_tr.user_data = &lb;

    if ((olnk_server_init(&srv, srv_tr, olnk_mem_drive_ops(&drv)) != OLNK_OK) ||
        (olnk_init(&cli, cli_tr) != OLNK_OK)) {
        return 1;
    }

    /* 1. Phase A Sync + Phase B Baud Step-Up (9600 -> 115200) */
    if ((olnk_link_connect_client(&cli) != OLNK_OK) ||
        (lb.cli_baud != 115200UL) || (lb.srv_baud != 115200UL)) {
        fprintf(stderr, "self-test FAIL: sync/baud negotiation\n");
        return 2;
    }

    /* 2. 0x00 SESSION_SYNC */
    {
        olnk_sync_trigger_t trig = { OLNK_OP_SESSION_SYNC, 0U };
        olnk_session_sync_req_t sreq;
        olnk_session_sync_resp_t sresp;
        memset(&sreq, 0, sizeof(sreq));
        sreq.client_version = OLNK_CLIENT_VERSION_DOS622;
        sreq.proto_major = 1U;
        sreq.capability = 1U;
        sreq.max_block_size = 8192U;
        sreq.client_id = 0xCAFEBABEUL;

        (void)olnk_encode_sync_trigger(&trig, msg_buf, sizeof(msg_buf), &msg_len);
        if (olnk_link_send_frame(&cli, msg_buf, (uint16_t)msg_len, false) != OLNK_OK) {
            return 3;
        }
        (void)olnk_encode_session_sync_req(&sreq, msg_buf, sizeof(msg_buf), &msg_len);
        if (olnk_link_send_frame(&cli, msg_buf, (uint16_t)msg_len, false) != OLNK_OK) {
            return 4;
        }
        if ((olnk_link_send_turnaround(&cli) != OLNK_OK) ||
            (olnk_link_recv_frame(&cli, msg_buf, sizeof(msg_buf), &rx_len, &more, 500U) != OLNK_OK) ||
            (olnk_link_wait_turnaround(&cli, 500U) != OLNK_OK) ||
            (olnk_decode_session_sync_resp(msg_buf, rx_len, &sresp) != OLNK_OK) ||
            (sresp.ready_flag != 1U) || (sresp.max_block_size != 8192U)) {
            fprintf(stderr, "self-test FAIL: SESSION_SYNC\n");
            return 5;
        }
    }

    /* 3. 0x03 BUILD_BPB */
    {
        olnk_build_bpb_req_t breq = { OLNK_OP_BUILD_BPB, 0U, 0xF0U };
        olnk_build_bpb_resp_t bresp;
        (void)olnk_encode_build_bpb_req(&breq, msg_buf, sizeof(msg_buf), &msg_len);
        if ((olnk_link_send_frame(&cli, msg_buf, (uint16_t)msg_len, false) != OLNK_OK) ||
            (olnk_link_send_turnaround(&cli) != OLNK_OK) ||
            (olnk_link_recv_frame(&cli, msg_buf, sizeof(msg_buf), &rx_len, &more, 500U) != OLNK_OK) ||
            (olnk_link_wait_turnaround(&cli, 500U) != OLNK_OK) ||
            (olnk_decode_build_bpb_resp(msg_buf, rx_len, &bresp) != OLNK_OK) ||
            (!bresp.has_bpb) || (bresp.bpb.bytes_per_sector != 512U) || (bresp.bpb.total_sectors_16 != 64U)) {
            fprintf(stderr, "self-test FAIL: BUILD_BPB\n");
            return 6;
        }
    }

    /* 4. Multi-fragment 16 KiB Write (0x05) + 16 KiB Read (0x04) (> 8192 B!) */
    for (i = 0U; i < sizeof(tx_16k); ++i) {
        tx_16k[i] = (uint8_t)((i * 37U + 11U) & 0xFFU);
    }
    {
        olnk_sector_io_req_t wreq;
        olnk_sector_io_resp_t wresp;
        wreq.opcode = OLNK_OP_WRITE_SECTORS;
        wreq.unit = 0U;
        wreq.media_descriptor = 0xF0U;
        wreq.sector_count = 32U;
        wreq.start_sector_16 = 0xFFFFU;
        wreq.start_sector_32 = 16UL;

        (void)olnk_encode_sector_io_req(&wreq, msg_buf, sizeof(msg_buf), &msg_len);
        if ((olnk_link_send_frame(&cli, msg_buf, (uint16_t)msg_len, false) != OLNK_OK) ||
            (olnk_link_send_message(&cli, tx_16k, (uint32_t)sizeof(tx_16k)) != OLNK_OK) ||
            (olnk_link_send_turnaround(&cli) != OLNK_OK) ||
            (olnk_link_recv_frame(&cli, msg_buf, sizeof(msg_buf), &rx_len, &more, 500U) != OLNK_OK) ||
            (olnk_link_wait_turnaround(&cli, 500U) != OLNK_OK) ||
            (olnk_decode_sector_io_resp(msg_buf, rx_len, &wresp) != OLNK_OK) ||
            (wresp.status != OLNK_DOS_STATUS_DONE) || (wresp.sectors_transferred != 32U)) {
            fprintf(stderr, "self-test FAIL: 16 KiB WRITE_SECTORS\n");
            return 7;
        }
    }
    {
        olnk_sector_io_req_t rreq;
        olnk_sector_io_resp_t rresp;
        rreq.opcode = OLNK_OP_READ_SECTORS;
        rreq.unit = 0U;
        rreq.media_descriptor = 0xF0U;
        rreq.sector_count = 32U;
        rreq.start_sector_16 = 0xFFFFU;
        rreq.start_sector_32 = 16UL;

        (void)olnk_encode_sector_io_req(&rreq, msg_buf, sizeof(msg_buf), &msg_len);
        if ((olnk_link_send_frame(&cli, msg_buf, (uint16_t)msg_len, false) != OLNK_OK) ||
            (olnk_link_send_turnaround(&cli) != OLNK_OK) ||
            (olnk_link_recv_frame(&cli, msg_buf, sizeof(msg_buf), &rx_len, &more, 500U) != OLNK_OK) ||
            (olnk_decode_sector_io_resp(msg_buf, rx_len, &rresp) != OLNK_OK) ||
            (rresp.status != OLNK_DOS_STATUS_DONE) || (rresp.sectors_transferred != 32U) ||
            (olnk_link_recv_message(&cli, rx_16k, (uint32_t)sizeof(rx_16k), &total_rx, 500U) != OLNK_OK) ||
            (olnk_link_wait_turnaround(&cli, 500U) != OLNK_OK) ||
            (total_rx != (uint32_t)sizeof(tx_16k)) ||
            (memcmp(tx_16k, rx_16k, sizeof(tx_16k)) != 0)) {
            fprintf(stderr, "self-test FAIL: 16 KiB READ_SECTORS\n");
            return 8;
        }
    }

    /* 5. Media Swap (0xFF) + Write-Protect (0x8100) + Not-Ready (0x8102) */
    olnk_mem_drive_swap_media(&drv, disk_b, 64UL);
    {
        olnk_media_check_req_t mreq = { OLNK_OP_MEDIA_CHECK, 0U, 0xF0U };
        olnk_media_check_resp_t mresp;
        (void)olnk_encode_media_check_req(&mreq, msg_buf, sizeof(msg_buf), &msg_len);
        if ((olnk_link_send_frame(&cli, msg_buf, (uint16_t)msg_len, false) != OLNK_OK) ||
            (olnk_link_send_turnaround(&cli) != OLNK_OK) ||
            (olnk_link_recv_frame(&cli, msg_buf, sizeof(msg_buf), &rx_len, &more, 500U) != OLNK_OK) ||
            (olnk_link_wait_turnaround(&cli, 500U) != OLNK_OK) ||
            (olnk_decode_media_check_resp(msg_buf, rx_len, &mresp) != OLNK_OK) ||
            (mresp.media_status != OLNK_MEDIA_CHANGED) ||
            (mresp.volume_serial != 0x55667788UL)) {
            fprintf(stderr, "self-test FAIL: MEDIA_CHECK swap\n");
            return 9;
        }
    }

    drv.write_protected = true;
    {
        olnk_sector_io_req_t wreq = { OLNK_OP_WRITE_SECTORS, 0U, 0xF0U, 1U, 1U, 1UL };
        olnk_sector_io_resp_t wresp;
        (void)olnk_encode_sector_io_req(&wreq, msg_buf, sizeof(msg_buf), &msg_len);
        if ((olnk_link_send_frame(&cli, msg_buf, (uint16_t)msg_len, false) != OLNK_OK) ||
            (olnk_link_send_frame(&cli, tx_16k, OLNK_SECTOR_SIZE, false) != OLNK_OK) ||
            (olnk_link_send_turnaround(&cli) != OLNK_OK) ||
            (olnk_link_recv_frame(&cli, msg_buf, sizeof(msg_buf), &rx_len, &more, 500U) != OLNK_OK) ||
            (olnk_link_wait_turnaround(&cli, 500U) != OLNK_OK) ||
            (olnk_decode_sector_io_resp(msg_buf, rx_len, &wresp) != OLNK_OK) ||
            (wresp.status != OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT))) {
            fprintf(stderr, "self-test FAIL: WRITE_PROTECT 0x8100\n");
            return 10;
        }
    }

    drv.not_ready = true;
    {
        olnk_media_check_req_t mreq = { OLNK_OP_MEDIA_CHECK, 0U, 0xF0U };
        olnk_media_check_resp_t mresp;
        (void)olnk_encode_media_check_req(&mreq, msg_buf, sizeof(msg_buf), &msg_len);
        if ((olnk_link_send_frame(&cli, msg_buf, (uint16_t)msg_len, false) != OLNK_OK) ||
            (olnk_link_send_turnaround(&cli) != OLNK_OK) ||
            (olnk_link_recv_frame(&cli, msg_buf, sizeof(msg_buf), &rx_len, &more, 500U) != OLNK_OK) ||
            (olnk_link_wait_turnaround(&cli, 500U) != OLNK_OK) ||
            (olnk_decode_media_check_resp(msg_buf, rx_len, &mresp) != OLNK_OK) ||
            (mresp.status != OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY))) {
            fprintf(stderr, "self-test FAIL: DRIVE_NOT_READY 0x8102\n");
            return 11;
        }
    }

    printf(
        "opensvr self-test PASSED (baud=%lu, rpcs=%lu, rd_sec=%lu, wr_sec=%lu, frames_ok=%lu)\n",
        (unsigned long)lb.srv_baud,
        (unsigned long)srv.rpc_count,
        (unsigned long)srv.sectors_read_total,
        (unsigned long)srv.sectors_written_total,
        (unsigned long)srv.link.frames_rx_ok
    );
    return 0;
}

/* =========================================================================
 * POSIX Serial (termios) & 86Box Named Pipe (.in/.out FIFO) Transports
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
} posix_hal_t;

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

    /* Pure POSIX 8N1 raw mode (no cfmakeraw dependency) */
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

static bool posix_hal_set_baud(uint32_t baudrate, void *ud) {
    posix_hal_t *hal = (posix_hal_t *)ud;
    if (!hal->is_serial) {
        return true; /* Named pipes ignore physical UART baud */
    }
    (void)tcdrain(hal->tx_fd);
    return posix_serial_configure(hal->rx_fd, baudrate);
}

static int posix_hal_read(uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    posix_hal_t *hal = (posix_hal_t *)ud;
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

static int posix_hal_write(const uint8_t *buf, size_t len, uint32_t timeout_ms, void *ud) {
    posix_hal_t *hal = (posix_hal_t *)ud;
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
        fprintf(stderr, "[opensvr] ERROR: Path exists and is not a FIFO: %s\n", path);
        return false;
    }
    if (mkfifo(path, 0600) != 0) {
        fprintf(stderr, "[opensvr] ERROR: mkfifo(%s) failed: %s\n", path, strerror(errno));
        return false;
    }
    return true;
}

static bool posix_open_pipe(const char *prefix, posix_hal_t *hal) {
    char in_path[512];
    char out_path[512];
    uint8_t drain[256];

    if (strlen(prefix) + 8U >= sizeof(in_path)) {
        fprintf(stderr, "[opensvr] ERROR: Pipe prefix too long\n");
        return false;
    }

    /* 86Box Named Pipe (COM) convention: 86Box reads .in and writes .out */
    snprintf(in_path, sizeof(in_path), "%s.in", prefix);
    snprintf(out_path, sizeof(out_path), "%s.out", prefix);

    if (!ensure_fifo(in_path) || !ensure_fifo(out_path)) {
        return false;
    }

    hal->rx_fd = open(out_path, O_RDWR | O_NONBLOCK);
    if (hal->rx_fd < 0) {
        fprintf(stderr, "[opensvr] ERROR: open(%s) failed: %s\n", out_path, strerror(errno));
        return false;
    }
    hal->tx_fd = open(in_path, O_RDWR | O_NONBLOCK);
    if (hal->tx_fd < 0) {
        fprintf(stderr, "[opensvr] ERROR: open(%s) failed: %s\n", in_path, strerror(errno));
        close(hal->rx_fd);
        hal->rx_fd = -1;
        return false;
    }
    hal->is_serial = false;

    /* Drain any stale bytes left from a previous session */
    while (read(hal->rx_fd, drain, sizeof(drain)) > 0) {
        /* discard */
    }

    fprintf(stderr, "[opensvr] Listening on 86Box Named Pipe: %s (.in/.out)\n", prefix);
    return true;
}

static bool posix_open_serial(const char *port_path, posix_hal_t *hal) {
    int fd = open(port_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[opensvr] ERROR: open(%s) failed: %s\n", port_path, strerror(errno));
        return false;
    }
    if (!posix_serial_configure(fd, 9600UL)) {
        fprintf(stderr, "[opensvr] WARN: termios configuration skipped on %s (%s)\n",
                port_path, strerror(errno));
    }
    (void)tcflush(fd, TCIOFLUSH);

    hal->rx_fd = fd;
    hal->tx_fd = fd;
    hal->is_serial = true;
    fprintf(stderr, "[opensvr] Listening on serial port: %s (initial 9600 8N1)\n", port_path);
    return true;
}

static void posix_close_hal(posix_hal_t *hal) {
    if (hal->rx_fd >= 0) {
        close(hal->rx_fd);
    }
    if ((hal->tx_fd >= 0) && (hal->tx_fd != hal->rx_fd)) {
        close(hal->tx_fd);
    }
    hal->rx_fd = -1;
    hal->tx_fd = -1;
}

static uint64_t monotonic_ms(void) {
    struct timeval tv;
    (void)gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
}

static void on_verbose_event(const char *name, uint32_t a1, uint32_t a2, void *ud) {
    (void)ud;
    fprintf(stderr, "[opensvr] event=%s arg1=%lu (0x%lX) arg2=%lu\n",
            name, (unsigned long)a1, (unsigned long)a1, (unsigned long)a2);
}

static int run_posix_server(
    const char *port_path,
    const char *pipe_prefix,
    uint8_t max_baud_idx,
    bool echo_mode,
    uint32_t pace_us,
    bool verbose,
    file_drive_set_t *drives
) {
    static olnk_server_t srv;
    posix_hal_t hal;
    olnk_transport_t tr;
    uint64_t last_ms;

    hal.rx_fd = -1;
    hal.tx_fd = -1;
    hal.is_serial = false;
    hal.pace_us = pace_us;

    if (pipe_prefix != NULL) {
        if (!posix_open_pipe(pipe_prefix, &hal)) {
            return 1;
        }
    } else if (port_path != NULL) {
        if (!posix_open_serial(port_path, &hal)) {
            return 1;
        }
    } else {
        fprintf(stderr, "[opensvr] ERROR: Specify --port <tty/pty> or --pipe <prefix>.\n");
        return 2;
    }

    tr.read = posix_hal_read;
    tr.write = posix_hal_write;
    tr.set_baud = posix_hal_set_baud;
    tr.user_data = &hal;

    if (olnk_server_init(&srv, tr, file_drive_set_ops(drives)) != OLNK_OK) {
        posix_close_hal(&hal);
        return 1;
    }
    srv.link.max_baud_idx = max_baud_idx;
    srv.link.local_requires_echo = echo_mode;
    /* Auto-recover back to 9600 bps after 3 seconds of silence on high-baud links */
    srv.idle_disconnect_ms = 3000UL;
    if (verbose) {
        olnk_server_set_event_callback(&srv, on_verbose_event, NULL);
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    fprintf(
        stderr,
        "[opensvr] Ready: max_baud=%lu bps, pacing=%s, drives=%u (Ctrl-C to stop)\n",
        (unsigned long)olnk_baud_index_to_bps(max_baud_idx),
        echo_mode ? "echo" : "burst",
        (unsigned int)drives->drive_count
    );

    last_ms = monotonic_ms();
    while (!g_stop_requested) {
        uint8_t buf[64];
        int n = posix_hal_read(buf, sizeof(buf), 20U, &hal);
        uint64_t now_ms = monotonic_ms();
        uint32_t dt_ms = (now_ms >= last_ms) ? (uint32_t)(now_ms - last_ms) : 0UL;
        last_ms = now_ms;

        if (n < 0) {
            fprintf(stderr, "[opensvr] Fatal transport read error, exiting.\n");
            break;
        }
        if (n > 0) {
            olnk_server_feed_bytes(&srv, buf, (size_t)n);
        } else if (dt_ms > 0UL) {
            olnk_server_tick(&srv, dt_ms);
        }
    }

    fprintf(
        stderr,
        "[opensvr] Stopped. rpcs=%lu rd_sec=%lu wr_sec=%lu ka=%lu baud_sw=%lu rx_ok=%lu rx_err=%lu\n",
        (unsigned long)srv.rpc_count,
        (unsigned long)srv.sectors_read_total,
        (unsigned long)srv.sectors_written_total,
        (unsigned long)srv.link.keepalives_ok,
        (unsigned long)srv.link.baud_switches,
        (unsigned long)srv.link.frames_rx_ok,
        (unsigned long)srv.link.frames_rx_err
    );
    posix_close_hal(&hal);
    return 0;
}

#endif /* !_WIN32 */

/* =========================================================================
 * CLI Argument Parsing & Entrypoint
 * ========================================================================= */

static void print_usage(void) {
    printf("opensvr (libopenlnk 0.2.0 — MS-DOS 6.22 INTERSVR clean-room server)\n");
    printf("Usage:\n");
    printf("  opensvr --version\n");
    printf("  opensvr --self-test\n");
    printf("  opensvr --port <tty/pty> --image <disk.vhd|disk.img> [options]\n");
    printf("  opensvr --pipe <prefix>  --dir <folder> [options]\n");
    printf("Options:\n");
    printf("  --image <path>       Attach RAW/VHD FAT12/FAT16 disk image (auto unit)\n");
    printf("  --dir <folder>       Share host folder as Virtual FAT16 drive (read-write + live refresh)\n");
    printf("  --drive <L>:<path>   Map image or folder to explicit server letter (e.g. C:~/dos_share)\n");
    printf("  --label <name>       Optional DOS volume label for --dir shares (up to 11 chars)\n");
    printf("  --baud <rate>        Max negotiated baud (1200..115200, default 115200)\n");
    printf("  --read-only          Write-protect all attached drives (returns DOS error 0x8100)\n");
    printf("  --burst              Allow Burst (no-echo) frame reception (default: Echo-Paced)\n");
    printf("  --pace-us <us>       Per-byte TX delay in microseconds for --pipe mode (default: 0)\n");
    printf("  --verbose, -v        Log sync, baud negotiation, and RPC events to stderr\n");
}

int main(int argc, char **argv) {
    static file_drive_set_t drives;
    const char *port_path = NULL;
    const char *pipe_prefix = NULL;
    const char *volume_label = NULL;
    const char *image_paths[OLNK_MAX_DRIVES];
    uint8_t image_units[OLNK_MAX_DRIVES];
    bool image_is_dir[OLNK_MAX_DRIVES];
    size_t image_count = 0U;
    uint8_t next_auto_unit = 0U;
    uint32_t max_baud = 115200UL;
    uint8_t max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    uint32_t pace_us = 0UL;
    bool read_only = false;
    bool echo_mode = true;
    bool verbose = false;
    int i;
    size_t k;

    memset(&drives, 0, sizeof(drives));

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("opensvr 0.2.0 (libopenlnk C99 zero-alloc MS-DOS 6.22 protocol)\n");
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
        } else if ((strcmp(argv[i], "--image") == 0) && ((i + 1) < argc)) {
            if (image_count < OLNK_MAX_DRIVES) {
                image_units[image_count] = next_auto_unit++;
                image_paths[image_count] = argv[++i];
                image_is_dir[image_count] = false;
                image_count += 1U;
            }
        } else if ((strcmp(argv[i], "--dir") == 0) && ((i + 1) < argc)) {
            if (image_count < OLNK_MAX_DRIVES) {
                image_units[image_count] = next_auto_unit++;
                image_paths[image_count] = argv[++i];
                image_is_dir[image_count] = true;
                image_count += 1U;
            }
        } else if ((strcmp(argv[i], "--label") == 0) && ((i + 1) < argc)) {
            volume_label = argv[++i];
        } else if ((strcmp(argv[i], "--drive") == 0) && ((i + 1) < argc)) {
            const char *spec = argv[++i];
            if ((spec[0] != '\0') && (spec[1] == ':') && (spec[2] != '\0')) {
                char c = spec[0];
                uint8_t u;
                if ((c >= 'a') && (c <= 'z')) {
                    c = (char)(c - 'a' + 'A');
                }
                if ((c < 'A') || (c > 'Z')) {
                    fprintf(stderr, "[opensvr] ERROR: Invalid drive letter in '%s'\n", spec);
                    return 2;
                }
                u = (uint8_t)(c - 'A');
                if (image_count < OLNK_MAX_DRIVES) {
                    image_units[image_count] = u;
                    image_paths[image_count] = &spec[2];
                    image_is_dir[image_count] = file_drive_path_is_dir(&spec[2]);
                    image_count += 1U;
                }
            } else {
                fprintf(stderr, "[opensvr] ERROR: Expected --drive <L>:<path>, got '%s'\n", spec);
                return 2;
            }
        } else if ((strcmp(argv[i], "--baud") == 0) && ((i + 1) < argc)) {
            max_baud = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (!olnk_bps_to_baud_index(max_baud, &max_baud_idx)) {
                fprintf(stderr, "[opensvr] ERROR: Unsupported baud rate %lu\n", (unsigned long)max_baud);
                return 2;
            }
        } else if (strcmp(argv[i], "--read-only") == 0) {
            read_only = true;
        } else if (strcmp(argv[i], "--burst") == 0) {
            echo_mode = false;
        } else if ((strcmp(argv[i], "--pace-us") == 0) && ((i + 1) < argc)) {
            pace_us = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if ((strcmp(argv[i], "--verbose") == 0) || (strcmp(argv[i], "-v") == 0)) {
            verbose = true;
        } else {
            fprintf(stderr, "[opensvr] ERROR: Unknown or incomplete argument '%s'\n", argv[i]);
            print_usage();
            return 2;
        }
    }

    if ((port_path == NULL) && (pipe_prefix == NULL)) {
        print_usage();
        return 0;
    }

    if (image_count == 0U) {
        fprintf(stderr, "[opensvr] ERROR: Attach at least one drive via --dir, --image, or --drive.\n");
        return 2;
    }

    for (k = 0U; k < image_count; ++k) {
        bool ok = image_is_dir[k]
                      ? file_drive_attach_dir(&drives, image_units[k], image_paths[k], read_only, volume_label)
                      : file_drive_attach(&drives, image_units[k], image_paths[k], read_only);
        if (!ok) {
            file_drive_set_close(&drives);
            return 1;
        }
    }

#if defined(_WIN32)
    (void)max_baud_idx;
    (void)echo_mode;
    (void)pace_us;
    (void)verbose;
    fprintf(stderr, "[opensvr] POSIX --port/--pipe server mode requires Linux or macOS.\n");
    file_drive_set_close(&drives);
    return 1;
#else
    {
        int rc = run_posix_server(
            port_path,
            pipe_prefix,
            max_baud_idx,
            echo_mode,
            pace_us,
            verbose,
            &drives
        );
        file_drive_set_close(&drives);
        return rc;
    }
#endif
}
