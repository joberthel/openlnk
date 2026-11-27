/* cleanroom/dos/openlnk_main.c
 *
 * Clean-room 16-bit Real-Mode MS-DOS OPENLNK.EXE client utility.
 * Connects over COM1..COM4 (8250/16550A HAL) to opensvr or MS-DOS 6.22
 * INTERSVR.EXE, displays mapped remote drives/BPBs, lists remote FAT12/FAT16
 * directories (/DIR), and downloads files (/GET) with zero dynamic allocation.
 *
 * Compiled via Open Watcom v2 (-ms -0 -za99 -wx -we -os -bt=dos) for 8086/8088.
 */
#include "dos_hal_uart.h"
#include "openlnk/client.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Static buffers (16-bit Real-Mode Small Model safe — never on stack!) */
static olnk_client_t g_dos_client;
static olnk_dos_uart_t g_dos_uart;
static uint8_t g_sec_buf[OLNK_SECTOR_SIZE];
static uint8_t g_fat_cache[OLNK_SECTOR_SIZE * 2U];

static uint32_t parse_u32_ascii(const char *s) {
    uint32_t val = 0UL;
    if (s == NULL) {
        return 0UL;
    }
    while ((*s >= '0') && (*s <= '9')) {
        val = (val * 10UL) + (uint32_t)(*s - '0');
        ++s;
    }
    return val;
}

static bool str_ieq_prefix(const char *s, const char *prefix) {
    while (*prefix != '\0') {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*prefix)) {
            return false;
        }
        ++s;
        ++prefix;
    }
    return true;
}

static void format_83(const uint8_t ent[32], char out[16]) {
    size_t pos = 0U;
    size_t i;
    size_t b_end = 8U;
    size_t e_end = 11U;

    while ((b_end > 0U) && (ent[b_end - 1U] == ' ')) {
        b_end -= 1U;
    }
    while ((e_end > 8U) && (ent[e_end - 1U] == ' ')) {
        e_end -= 1U;
    }
    for (i = 0U; i < b_end; ++i) {
        out[pos++] = (char)ent[i];
    }
    if (e_end > 8U) {
        out[pos++] = '.';
        for (i = 8U; i < e_end; ++i) {
            out[pos++] = (char)ent[i];
        }
    }
    out[pos] = '\0';
}

static void make_83_pat(const char *name, uint8_t pat[11]) {
    size_t i = 0U;
    size_t d = 0U;
    memset(pat, ' ', 11U);
    while ((name[i] != '\0') && (name[i] != '.') && (d < 8U)) {
        pat[d++] = (uint8_t)toupper((unsigned char)name[i++]);
    }
    while ((name[i] != '\0') && (name[i] != '.')) {
        ++i;
    }
    if (name[i] == '.') {
        ++i;
        d = 8U;
        while ((name[i] != '\0') && (d < 11U)) {
            pat[d++] = (uint8_t)toupper((unsigned char)name[i++]);
        }
    }
}

static bool dos_select_unit(uint8_t *out_unit) {
    size_t i;
    for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
        uint8_t entry = g_dos_client.map.drive_map_resp[i];
        if ((entry != OLNK_MAP_AUTO) && (entry != OLNK_MAP_NONE)) {
            uint8_t u = (uint8_t)(entry & 0x7FU);
            olnk_build_bpb_resp_t bpb;
            if ((olnk_client_build_bpb(&g_dos_client, u, 0xF8U, &bpb) == OLNK_OK) &&
                ((bpb.status & OLNK_DOS_STATUS_ERROR) == 0U) && bpb.has_bpb) {
                *out_unit = u;
                return true;
            }
        }
    }
    return false;
}

static void dos_cmd_info(void) {
    size_t i;
    printf("Connected @ %lu bps | Server DOS %u.%02u | max_block=%u\n",
           (unsigned long)olnk_baud_index_to_bps(g_dos_client.link.current_baud_idx),
           (unsigned int)g_dos_client.map.server_dos_major,
           (unsigned int)g_dos_client.map.server_dos_minor,
           (unsigned int)g_dos_client.link.max_block_size);

    for (i = 0U; i < OLNK_MAX_DRIVES; ++i) {
        uint8_t entry = g_dos_client.map.drive_map_resp[i];
        uint8_t u;
        olnk_get_drive_info_resp_t info;
        if ((entry == OLNK_MAP_AUTO) || (entry == OLNK_MAP_NONE)) {
            continue;
        }
        u = (uint8_t)(entry & 0x7FU);
        memset(&info, 0, sizeof(info));
        (void)olnk_client_get_drive_info(&g_dos_client, u, &info);
        printf("  Remote %c: -> %s '%s'\n",
               (char)('A' + (char)u),
               info.size_string,
               info.volume_label);
    }
}

static int dos_cmd_dir(uint8_t unit) {
    olnk_build_bpb_resp_t bpb;
    olnk_sector_io_resp_t ior;
    uint32_t root_lba;
    uint32_t root_secs;
    uint32_t s;

    if ((olnk_client_build_bpb(&g_dos_client, unit, 0xF8U, &bpb) != OLNK_OK) ||
        ((bpb.status & OLNK_DOS_STATUS_ERROR) != 0U) || (!bpb.has_bpb)) {
        printf("Drive %c: not ready (0x%04X)\n", (char)('A' + (char)unit), (unsigned int)bpb.status);
        return 1;
    }

    root_lba = (uint32_t)bpb.bpb.reserved_sectors +
               ((uint32_t)bpb.bpb.num_fats * (uint32_t)bpb.bpb.sectors_per_fat);
    root_secs = ((uint32_t)bpb.bpb.root_entries * 32UL + (uint32_t)OLNK_SECTOR_SIZE - 1UL) /
                (uint32_t)OLNK_SECTOR_SIZE;

    printf("Directory of remote %c:\\\n", (char)('A' + (char)unit));
    for (s = 0UL; s < root_secs; ++s) {
        size_t off;
        if ((olnk_client_read_sectors(
                 &g_dos_client, unit, bpb.bpb.media_descriptor, root_lba + s, 1U,
                 OLNK_SECTOR_SIZE, g_sec_buf, (uint32_t)sizeof(g_sec_buf), &ior) != OLNK_OK) ||
            ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U)) {
            return 1;
        }
        for (off = 0U; off < OLNK_SECTOR_SIZE; off += 32U) {
            const uint8_t *e = &g_sec_buf[off];
            char nm[16];
            if (e[0] == 0x00U) {
                return 0;
            }
            if ((e[0] == 0xE5U) || (e[11] == 0x0FU) || ((e[11] & 0x08U) != 0U)) {
                continue;
            }
            format_83(e, nm);
            if ((e[11] & 0x10U) != 0U) {
                printf("  %-13s <DIR>\n", nm);
            } else {
                printf("  %-13s %lu\n", nm, (unsigned long)olnk_read_u32_le(&e[28]));
            }
        }
    }
    return 0;
}

static bool dos_fat_next(
    uint8_t unit,
    const olnk_dos_bpb_t *bpb,
    bool is_fat12,
    uint16_t cluster,
    uint32_t *cache_lba,
    uint16_t *cache_secs,
    uint16_t *out_next
) {
    uint32_t byte_off = is_fat12 ? (((uint32_t)cluster * 3UL) / 2UL) : ((uint32_t)cluster * 2UL);
    uint32_t fsec = (uint32_t)bpb->reserved_sectors + (byte_off / (uint32_t)OLNK_SECTOR_SIZE);
    uint32_t in_sec = byte_off % (uint32_t)OLNK_SECTOR_SIZE;
    uint16_t need = ((in_sec + 1UL) >= (uint32_t)OLNK_SECTOR_SIZE) ? 2U : 1U;
    uint16_t val;

    if ((*cache_secs == 0U) || (fsec < *cache_lba) ||
        ((fsec + (uint32_t)need) > (*cache_lba + (uint32_t)*cache_secs))) {
        olnk_sector_io_resp_t ior;
        uint32_t fat_end = (uint32_t)bpb->reserved_sectors + (uint32_t)bpb->sectors_per_fat;
        uint16_t cnt = ((fsec + 2UL) <= fat_end) ? 2U : 1U;
        if ((olnk_client_read_sectors(
                 &g_dos_client, unit, bpb->media_descriptor, fsec, cnt,
                 OLNK_SECTOR_SIZE, g_fat_cache, (uint32_t)sizeof(g_fat_cache), &ior) != OLNK_OK) ||
            ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U)) {
            return false;
        }
        *cache_lba = fsec;
        *cache_secs = cnt;
    }

    val = olnk_read_u16_le(&g_fat_cache[(size_t)((fsec - *cache_lba) * (uint32_t)OLNK_SECTOR_SIZE + in_sec)]);
    *out_next = is_fat12
                    ? (((cluster & 1U) != 0U) ? (uint16_t)(val >> 4U) : (uint16_t)(val & 0x0FFFU))
                    : val;
    return true;
}

static int dos_cmd_get(uint8_t unit, const char *remote_name, const char *local_name) {
    olnk_build_bpb_resp_t bpb;
    olnk_sector_io_resp_t ior;
    uint8_t pat[11];
    uint32_t root_lba;
    uint32_t root_secs;
    uint32_t data_lba;
    uint32_t total_secs;
    bool is_fat12;
    uint32_t s;
    uint16_t cur_c = 0U;
    uint32_t remain = 0UL;
    bool found = false;
    uint32_t fat_cache_lba = 0UL;
    uint16_t fat_cache_secs = 0U;
    FILE *fp;

    if ((olnk_client_build_bpb(&g_dos_client, unit, 0xF8U, &bpb) != OLNK_OK) ||
        ((bpb.status & OLNK_DOS_STATUS_ERROR) != 0U) || (!bpb.has_bpb)) {
        printf("Drive %c: not ready\n", (char)('A' + (char)unit));
        return 1;
    }

    root_lba = (uint32_t)bpb.bpb.reserved_sectors +
               ((uint32_t)bpb.bpb.num_fats * (uint32_t)bpb.bpb.sectors_per_fat);
    root_secs = ((uint32_t)bpb.bpb.root_entries * 32UL + (uint32_t)OLNK_SECTOR_SIZE - 1UL) /
                (uint32_t)OLNK_SECTOR_SIZE;
    data_lba = root_lba + root_secs;
    total_secs = (bpb.bpb.total_sectors_16 != 0U)
                     ? (uint32_t)bpb.bpb.total_sectors_16
                     : bpb.bpb.total_sectors_32;
    if ((bpb.bpb.sectors_per_cluster == 0U) || (total_secs <= data_lba)) {
        printf("Invalid FAT BPB on drive %c:\n", (int)('A' + (int)unit));
        return 1;
    }
    is_fat12 = (((total_secs - data_lba) / (uint32_t)bpb.bpb.sectors_per_cluster) < 4085UL);

    make_83_pat(remote_name, pat);
    for (s = 0UL; (s < root_secs) && (!found); ++s) {
        size_t off;
        if ((olnk_client_read_sectors(
                 &g_dos_client, unit, bpb.bpb.media_descriptor, root_lba + s, 1U,
                 OLNK_SECTOR_SIZE, g_sec_buf, (uint32_t)sizeof(g_sec_buf), &ior) != OLNK_OK) ||
            ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U)) {
            return 1;
        }
        for (off = 0U; off < OLNK_SECTOR_SIZE; off += 32U) {
            const uint8_t *e = &g_sec_buf[off];
            if (e[0] == 0x00U) {
                break;
            }
            if ((e[0] == 0xE5U) || ((e[11] & 0x18U) != 0U)) {
                continue;
            }
            if (memcmp(e, pat, 11U) == 0) {
                cur_c = olnk_read_u16_le(&e[26]);
                remain = olnk_read_u32_le(&e[28]);
                found = true;
                break;
            }
        }
    }

    if (!found) {
        printf("File not found: %s\n", remote_name);
        return 1;
    }

    fp = fopen(local_name, "wb");
    if (fp == NULL) {
        printf("Cannot create %s\n", local_name);
        return 1;
    }

    while ((remain > 0UL) && (cur_c >= 2U) && (cur_c < (is_fat12 ? 0x0FF8U : 0xFFF8U))) {
        uint32_t cl_lba = data_lba + ((uint32_t)(cur_c - 2U) * (uint32_t)bpb.bpb.sectors_per_cluster);
        uint8_t k;
        for (k = 0U; (k < bpb.bpb.sectors_per_cluster) && (remain > 0UL); ++k) {
            size_t to_w = (remain > (uint32_t)OLNK_SECTOR_SIZE) ? (size_t)OLNK_SECTOR_SIZE : (size_t)remain;
            if ((olnk_client_read_sectors(
                     &g_dos_client, unit, bpb.bpb.media_descriptor, cl_lba + (uint32_t)k, 1U,
                     OLNK_SECTOR_SIZE, g_sec_buf, (uint32_t)sizeof(g_sec_buf), &ior) != OLNK_OK) ||
                ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U) ||
                (fwrite(g_sec_buf, 1U, to_w, fp) != to_w)) {
                fclose(fp);
                return 1;
            }
            remain -= (uint32_t)to_w;
        }
        if (remain > 0UL) {
            if (!dos_fat_next(unit, &bpb.bpb, is_fat12, cur_c, &fat_cache_lba, &fat_cache_secs, &cur_c)) {
                fclose(fp);
                return 1;
            }
        }
    }

    fclose(fp);
    printf("Copied %c:\\%s -> %s\n", (char)('A' + (char)unit), remote_name, local_name);
    return 0;
}

static void print_usage(void) {
    printf("OPENLNK 0.2.0 (libopenlnk 16-bit Real-Mode MS-DOS 6.22 client)\n");
    printf("Usage:\n");
    printf("  OPENLNK [/COM1../COM4] [/BAUD:rate]            Show remote drives\n");
    printf("  OPENLNK [/COM1] /DIR [C:]                      List remote root directory\n");
    printf("  OPENLNK [/COM1] /GET <file> [local] [C:]       Download file from remote server\n");
}

int main(int argc, char **argv) {
    uint8_t com_num = 1U;
    uint32_t max_baud = 115200UL;
    uint8_t max_baud_idx = (uint8_t)OLNK_BAUD_115200;
    bool mode_dir = false;
    bool mode_get = false;
    const char *get_remote = NULL;
    const char *get_local = NULL;
    bool unit_set = false;
    uint8_t unit = 0U;
    olnk_transport_t tr;
    olnk_session_sync_resp_t sresp;
    int i;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if ((strcmp(arg, "/?") == 0) || str_ieq_prefix(arg, "/HELP") ||
            (strcmp(arg, "--help") == 0) || (strcmp(arg, "-h") == 0)) {
            print_usage();
            return 0;
        }
        if (str_ieq_prefix(arg, "/COM") && (arg[4] >= '1') && (arg[4] <= '4') && (arg[5] == '\0')) {
            com_num = (uint8_t)(arg[4] - '0');
        } else if (str_ieq_prefix(arg, "/BAUD:")) {
            max_baud = parse_u32_ascii(&arg[6]);
            if (!olnk_bps_to_baud_index(max_baud, &max_baud_idx)) {
                printf("Unsupported baud rate: %lu\n", (unsigned long)max_baud);
                return 2;
            }
        } else if (str_ieq_prefix(arg, "/DIR") && (arg[4] == '\0')) {
            mode_dir = true;
        } else if (str_ieq_prefix(arg, "/GET") && (arg[4] == '\0')) {
            mode_get = true;
        } else if (isalpha((unsigned char)arg[0]) && (arg[1] == ':') && (arg[2] == '\0')) {
            unit = (uint8_t)(toupper((unsigned char)arg[0]) - 'A');
            unit_set = true;
        } else if (mode_get && (get_remote == NULL)) {
            get_remote = arg;
        } else if (mode_get && (get_local == NULL)) {
            get_local = arg;
        }
    }

    if (olnk_dos_uart_init(&g_dos_uart, com_num, 0U, 9600UL, NULL, &tr) != OLNK_OK) {
        printf("Failed to initialize COM%u\n", (unsigned int)com_num);
        return 1;
    }
    if (olnk_client_init(&g_dos_client, tr) != OLNK_OK) {
        return 1;
    }
    g_dos_client.link.max_baud_idx = max_baud_idx;

#if defined(__WATCOMC__) && defined(__DOS__)
    if (olnk_client_connect(&g_dos_client, 15U) != OLNK_OK) {
        printf("No server response on COM%u (0x%03X)\n",
               (unsigned int)com_num, (unsigned int)g_dos_uart.io_base);
        return 1;
    }
    if ((olnk_client_session_sync(&g_dos_client, false, &sresp) != OLNK_OK) ||
        (olnk_client_map_config(&g_dos_client, 8U, 3U, NULL) != OLNK_OK)) {
        printf("Session handshake failed\n");
        return 1;
    }
    if (!unit_set) {
        (void)dos_select_unit(&unit);
    }
    if (mode_dir) {
        return dos_cmd_dir(unit);
    }
    if (mode_get && (get_remote != NULL)) {
        return dos_cmd_get(unit, get_remote, (get_local != NULL) ? get_local : get_remote);
    }
    dos_cmd_info();
#else
    (void)sresp;
    (void)unit_set;
    (void)unit;
    (void)mode_dir;
    (void)mode_get;
    (void)get_remote;
    (void)get_local;
    (void)dos_select_unit;
    (void)dos_cmd_info;
    (void)dos_cmd_dir;
    (void)dos_cmd_get;
    printf("OPENLNK 0.2.0 (libopenlnk 16-bit Real-Mode client ready)\n");
#endif
    return 0;
}
