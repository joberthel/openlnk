/* cleanroom/desktop/vfat_dir.c
 *
 * Clean-room Desktop Virtual FAT16 (Read-Write & Read-Only) Directory Backend.
 * Strictly ISO C99 + POSIX.1-2008 (with clean _WIN32 stub for MSVC /W4 /WX).
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

#include "vfat_dir.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

struct vfat_dir_drive {
    int unused;
};

vfat_dir_drive_t *vfat_dir_open(const char *root_path, bool read_only, const char *volume_label) {
    (void)root_path;
    (void)read_only;
    (void)volume_label;
    fprintf(stderr, "[opensvr] Virtual FAT16 --dir mode requires POSIX (Linux/macOS).\n");
    return NULL;
}

void vfat_dir_close(vfat_dir_drive_t *drv) {
    (void)drv;
}

uint16_t vfat_dir_get_info(vfat_dir_drive_t *drv, olnk_drive_info_t *out_info) {
    (void)drv;
    if (out_info != NULL) {
        memset(out_info, 0, sizeof(*out_info));
    }
    return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
}

uint16_t vfat_dir_media_check(
    vfat_dir_drive_t *drv,
    uint8_t media_descriptor,
    uint8_t *out_media_status,
    olnk_drive_info_t *out_info
) {
    (void)drv;
    (void)media_descriptor;
    if (out_media_status != NULL) {
        *out_media_status = (uint8_t)OLNK_MEDIA_UNSURE;
    }
    if (out_info != NULL) {
        memset(out_info, 0, sizeof(*out_info));
    }
    return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
}

uint16_t vfat_dir_build_bpb(vfat_dir_drive_t *drv, uint8_t media_descriptor, olnk_dos_bpb_t *out_bpb) {
    (void)drv;
    (void)media_descriptor;
    (void)out_bpb;
    return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
}

uint16_t vfat_dir_read_sectors(
    vfat_dir_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    uint8_t *out_buf,
    uint16_t *out_transferred
) {
    (void)drv;
    (void)start_lba;
    (void)sector_count;
    (void)out_buf;
    if (out_transferred != NULL) {
        *out_transferred = 0U;
    }
    return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
}

uint16_t vfat_dir_write_sectors(
    vfat_dir_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    const uint8_t *in_buf,
    uint16_t *out_transferred,
    bool verify
) {
    (void)drv;
    (void)start_lba;
    (void)sector_count;
    (void)in_buf;
    (void)verify;
    if (out_transferred != NULL) {
        *out_transferred = 0U;
    }
    return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
}

uint16_t vfat_dir_set_media_id(vfat_dir_drive_t *drv, const olnk_generic_ioctl_mid_t *mid) {
    (void)drv;
    (void)mid;
    return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
}

#else /* POSIX (macOS / Linux) */

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define VFAT_BPS            512U
#define VFAT_ROOT_ENTRIES   512U
#define VFAT_ROOT_SECTORS   ((VFAT_ROOT_ENTRIES * 32U) / VFAT_BPS) /* 32 sectors */
#define VFAT_RESERVED_SECS  1U
#define VFAT_NUM_FATS       2U
#define VFAT_MAX_PATH       1024U
#define VFAT_MAX_DEPTH      16U
#define VFAT_RESCAN_MIN_MS  150ULL

typedef enum {
    VFAT_BACKING_FREE = 0,
    VFAT_BACKING_FILE,
    VFAT_BACKING_DIR
} vfat_backing_kind_t;

typedef struct {
    uint8_t kind;
    uint32_t file_index;     /* Index into drv->backing_files if VFAT_BACKING_FILE */
    uint32_t file_offset;    /* Byte offset within host file */
    uint8_t *dir_bytes;      /* Synthesized subdir cluster buffer if VFAT_BACKING_DIR */
} vfat_cluster_backing_t;

typedef struct {
    char host_path[VFAT_MAX_PATH];
    uint32_t size;
} vfat_backing_file_t;

typedef struct {
    uint16_t cluster;
    uint8_t *data;           /* cluster_bytes */
    uint64_t dirty_gen;      /* Monotonic write sequence number */
} vfat_dirty_cluster_t;

typedef struct {
    char dos_path[VFAT_MAX_PATH];   /* Uppercase DOS-relative path, e.g. "SUB/FILE.TXT" */
    char host_path[VFAT_MAX_PATH];  /* Real host path under root_path */
    bool is_dir;
    uint16_t first_cluster;
    uint32_t file_size;
    uint64_t synced_gen;            /* Max dirty_gen at last host write */
    bool seen_in_walk;
} vfat_tracked_node_t;

struct vfat_dir_drive {
    char root_path[VFAT_MAX_PATH];
    char volume_label[12];
    bool read_only;
    uint32_t volume_serial;

    /* FAT16 Geometry */
    uint8_t spc;
    uint32_t cluster_bytes;
    uint16_t num_clusters;      /* Valid data clusters: 2 .. num_clusters + 1 */
    uint16_t sectors_per_fat;
    uint32_t fat1_lba;
    uint32_t fat2_lba;
    uint32_t root_lba;
    uint32_t data_lba;
    uint32_t total_sectors;
    olnk_drive_info_t info;

    /* Virtual FAT16 Metadata Buffers */
    uint8_t vbr[VFAT_BPS];
    uint8_t *fat1;
    uint8_t *fat2;
    uint8_t root_dir[VFAT_ROOT_SECTORS * VFAT_BPS];

    /* Clean Cluster Backing Map (indexed by cluster 0 .. num_clusters + 1) */
    vfat_cluster_backing_t *backing;
    uint16_t next_alloc_cluster;

    vfat_backing_file_t *backing_files;
    size_t backing_file_count;
    size_t backing_file_cap;

    /* Copy-on-Write Dirty Cluster Overlay */
    vfat_dirty_cluster_t *dirty;
    size_t dirty_count;
    size_t dirty_cap;
    uint64_t write_gen;

    /* Tracked Host <-> DOS Mapping for Read-Write Reconciliation */
    vfat_tracked_node_t *tracked;
    size_t tracked_count;
    size_t tracked_cap;

    /* Live Host Change Detection */
    uint64_t host_fingerprint;
    uint64_t last_scan_ms;
    bool media_changed_pending;
    bool has_in_flight_writes;
};

/* =========================================================================
 * Utility Helpers
 * ========================================================================= */

static uint64_t vfat_now_ms(void) {
    struct timeval tv;
    (void)gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
}

static void vfat_copy_str(char *dst, size_t dst_cap, const char *src) {
    size_t len;
    if ((dst == NULL) || (dst_cap == 0U)) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_cap) {
        len = dst_cap - 1U;
    }
    if (len > 0U) {
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

static uint64_t fnv1a_update(uint64_t h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    for (i = 0U; i < len; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static bool vfat_is_ignored_host_name(const char *name) {
    size_t len;
    if ((name == NULL) || (name[0] == '\0') || (name[0] == '.')) {
        return true;
    }
    len = strlen(name);
    if ((len > 12U) && (strcmp(&name[len - 12U], ".openlnk.tmp") == 0)) {
        return true;
    }
    return false;
}

static bool vfat_is_valid_dos_char(char c) {
    unsigned char u = (unsigned char)c;
    if (isalnum(u)) {
        return true;
    }
    switch (c) {
        case '_': case '-': case '$': case '!': case '#':
        case '%': case '&': case '(': case ')': case '@':
        case '^': case '{': case '}': case '~':
            return true;
        default:
            return false;
    }
}

static void vfat_time_to_dos(time_t t, uint16_t *out_time, uint16_t *out_date) {
    struct tm *lt = localtime(&t);
    int yr;
    if (lt == NULL) {
        *out_time = 0U;
        *out_date = (uint16_t)((1994U - 1980U) << 9U) | (5U << 5U) | 31U;
        return;
    }
    yr = lt->tm_year + 1900;
    if (yr < 1980) {
        yr = 1980;
    } else if (yr > 2107) {
        yr = 2107;
    }
    *out_date = (uint16_t)(((unsigned int)(yr - 1980) << 9U) |
                           ((unsigned int)(lt->tm_mon + 1) << 5U) |
                           (unsigned int)lt->tm_mday);
    *out_time = (uint16_t)(((unsigned int)lt->tm_hour << 11U) |
                           ((unsigned int)lt->tm_min << 5U) |
                           ((unsigned int)lt->tm_sec >> 1U));
}

static void vfat_format_83_str(const uint8_t name83[11], char out[16]) {
    size_t pos = 0U;
    size_t i;
    size_t base_end = 8U;
    size_t ext_end = 11U;

    while ((base_end > 0U) && (name83[base_end - 1U] == ' ')) {
        base_end -= 1U;
    }
    while ((ext_end > 8U) && (name83[ext_end - 1U] == ' ')) {
        ext_end -= 1U;
    }
    for (i = 0U; i < base_end; ++i) {
        out[pos++] = (char)name83[i];
    }
    if (ext_end > 8U) {
        out[pos++] = '.';
        for (i = 8U; i < ext_end; ++i) {
            out[pos++] = (char)name83[i];
        }
    }
    out[pos] = '\0';
}

static void vfat_make_unique_83(
    const char *host_name,
    uint8_t used_names[][11],
    size_t used_count,
    uint8_t out83[11]
) {
    const char *dot = strrchr(host_name, '.');
    char base[64];
    char ext[16];
    size_t blen = 0U;
    size_t elen = 0U;
    bool lossy = false;
    const char *p;
    size_t i;
    unsigned int suffix;

    if ((dot == host_name) || (dot == NULL)) {
        dot = NULL;
    }

    for (p = host_name; (*p != '\0') && (p != dot); ++p) {
        if (*p == '.') {
            lossy = true;
            continue;
        }
        if (vfat_is_valid_dos_char(*p)) {
            if (blen < sizeof(base) - 1U) {
                base[blen++] = (char)toupper((unsigned char)*p);
            }
        } else {
            lossy = true;
        }
    }
    base[blen] = '\0';
    if (blen == 0U) {
        strcpy(base, "FILE");
        blen = 4U;
        lossy = true;
    }
    if (blen > 8U) {
        lossy = true;
    }

    if (dot != NULL) {
        for (p = dot + 1; *p != '\0'; ++p) {
            if (vfat_is_valid_dos_char(*p)) {
                if (elen < sizeof(ext) - 1U) {
                    ext[elen++] = (char)toupper((unsigned char)*p);
                }
            } else {
                lossy = true;
            }
        }
        if (elen > 3U) {
            lossy = true;
        }
    }
    ext[elen] = '\0';

    /* Try clean 8.3 first if not lossy */
    if (!lossy) {
        bool collision = false;
        memset(out83, ' ', 11U);
        memcpy(&out83[0], base, blen);
        if (elen > 0U) {
            memcpy(&out83[8], ext, elen);
        }
        for (i = 0U; i < used_count; ++i) {
            if (memcmp(used_names[i], out83, 11U) == 0) {
                collision = true;
                break;
            }
        }
        if (!collision) {
            return;
        }
    }

    /* Fallback: BASENA~1.EXT .. ~999 */
    for (suffix = 1U; suffix <= 999U; ++suffix) {
        char tag[8];
        size_t tag_len;
        size_t prefix_len;
        bool collision = false;

        snprintf(tag, sizeof(tag), "~%u", suffix);
        tag_len = strlen(tag);
        prefix_len = (blen > (8U - tag_len)) ? (8U - tag_len) : blen;

        memset(out83, ' ', 11U);
        memcpy(&out83[0], base, prefix_len);
        memcpy(&out83[prefix_len], tag, tag_len);
        if (elen > 0U) {
            memcpy(&out83[8], ext, (elen > 3U) ? 3U : elen);
        }

        for (i = 0U; i < used_count; ++i) {
            if (memcmp(used_names[i], out83, 11U) == 0) {
                collision = true;
                break;
            }
        }
        if (!collision) {
            return;
        }
    }
}

static bool vfat_safe_dos_component(const char *comp) {
    if ((comp == NULL) || (comp[0] == '\0')) {
        return false;
    }
    if ((strcmp(comp, ".") == 0) || (strcmp(comp, "..") == 0)) {
        return false;
    }
    if ((strchr(comp, '/') != NULL) || (strchr(comp, '\\') != NULL)) {
        return false;
    }
    return true;
}

/* =========================================================================
 * Host Directory Scan & Fingerprint (Deterministic Sort)
 * ========================================================================= */

typedef struct {
    char name[256];
    char full_path[VFAT_MAX_PATH];
    bool is_dir;
    uint32_t size;
    time_t mtime;
} host_entry_t;

static int cmp_host_entry(const void *a, const void *b) {
    const host_entry_t *ea = (const host_entry_t *)a;
    const host_entry_t *eb = (const host_entry_t *)b;
    return strcmp(ea->name, eb->name);
}

static size_t list_host_dir(const char *dir_path, host_entry_t **out_list) {
    DIR *dp;
    struct dirent *de;
    host_entry_t *list = NULL;
    size_t count = 0U;
    size_t cap = 0U;

    *out_list = NULL;
    dp = opendir(dir_path);
    if (dp == NULL) {
        return 0U;
    }

    while ((de = readdir(dp)) != NULL) {
        char child[VFAT_MAX_PATH];
        struct stat st;
        int n;

        if (vfat_is_ignored_host_name(de->d_name)) {
            continue;
        }
        n = snprintf(child, sizeof(child), "%s/%s", dir_path, de->d_name);
        if ((n <= 0) || ((size_t)n >= sizeof(child))) {
            continue;
        }
        if (lstat(child, &st) != 0) {
            continue;
        }
        if ((!S_ISREG(st.st_mode)) && (!S_ISDIR(st.st_mode))) {
            continue; /* Skip symlinks, FIFOs, sockets, devices */
        }

        if (count >= cap) {
            size_t new_cap = (cap == 0U) ? 32U : (cap * 2U);
            host_entry_t *nl = (host_entry_t *)realloc(list, new_cap * sizeof(host_entry_t));
            if (nl == NULL) {
                break;
            }
            list = nl;
            cap = new_cap;
        }

        memset(&list[count], 0, sizeof(host_entry_t));
        vfat_copy_str(list[count].name, sizeof(list[count].name), de->d_name);
        vfat_copy_str(list[count].full_path, sizeof(list[count].full_path), child);
        list[count].is_dir = S_ISDIR(st.st_mode);
        list[count].size = (st.st_size > 0) ? (uint32_t)st.st_size : 0UL;
        list[count].mtime = st.st_mtime;
        count += 1U;
    }

    closedir(dp);
    if (count > 1U) {
        qsort(list, count, sizeof(host_entry_t), cmp_host_entry);
    }
    *out_list = list;
    return count;
}

static void compute_tree_stats_and_fp(
    const char *dir_path,
    unsigned int depth,
    uint64_t *fp,
    uint64_t *total_bytes
) {
    host_entry_t *entries = NULL;
    size_t count;
    size_t i;

    if (depth >= VFAT_MAX_DEPTH) {
        return;
    }

    count = list_host_dir(dir_path, &entries);
    for (i = 0U; i < count; ++i) {
        uint8_t kind = entries[i].is_dir ? 1U : 0U;
        uint64_t sz = (uint64_t)entries[i].size;
        uint64_t mt = (uint64_t)entries[i].mtime;
        *fp = fnv1a_update(*fp, entries[i].name, strlen(entries[i].name));
        *fp = fnv1a_update(*fp, &kind, 1U);
        if (!entries[i].is_dir) {
            *fp = fnv1a_update(*fp, &sz, sizeof(sz));
            *fp = fnv1a_update(*fp, &mt, sizeof(mt));
            *total_bytes += sz + 4096ULL;
        } else {
            *total_bytes += 4096ULL;
            compute_tree_stats_and_fp(entries[i].full_path, depth + 1U, fp, total_bytes);
        }
    }
    free(entries);
}

static uint64_t compute_host_fingerprint(const char *root_path, uint64_t *out_bytes) {
    uint64_t fp = 14695981039346656037ULL;
    uint64_t bytes = 0ULL;
    compute_tree_stats_and_fp(root_path, 0U, &fp, &bytes);
    if (out_bytes != NULL) {
        *out_bytes = bytes;
    }
    return fp;
}

/* =========================================================================
 * Virtual FAT16 Builder & Tracked Node Management
 * ========================================================================= */

static void vfat_clear_image_state(vfat_dir_drive_t *drv) {
    size_t i;
    if (drv->backing != NULL) {
        for (i = 0U; i < (size_t)drv->num_clusters + 2U; ++i) {
            free(drv->backing[i].dir_bytes);
        }
        free(drv->backing);
        drv->backing = NULL;
    }
    free(drv->fat1);
    drv->fat1 = NULL;
    free(drv->fat2);
    drv->fat2 = NULL;

    free(drv->backing_files);
    drv->backing_files = NULL;
    drv->backing_file_count = 0U;
    drv->backing_file_cap = 0U;

    if (drv->dirty != NULL) {
        for (i = 0U; i < drv->dirty_count; ++i) {
            free(drv->dirty[i].data);
        }
        free(drv->dirty);
        drv->dirty = NULL;
        drv->dirty_count = 0U;
        drv->dirty_cap = 0U;
    }

    free(drv->tracked);
    drv->tracked = NULL;
    drv->tracked_count = 0U;
    drv->tracked_cap = 0U;
}

static uint16_t vfat_fat16_get(const vfat_dir_drive_t *drv, uint16_t cluster) {
    if ((drv->fat1 == NULL) || (cluster >= (uint16_t)(drv->num_clusters + 2U))) {
        return 0xFFFFU;
    }
    return olnk_read_u16_le(&drv->fat1[(size_t)cluster * 2U]);
}

static void vfat_fat16_set(vfat_dir_drive_t *drv, uint16_t cluster, uint16_t val) {
    if (cluster >= (uint16_t)(drv->num_clusters + 2U)) {
        return;
    }
    olnk_write_u16_le(&drv->fat1[(size_t)cluster * 2U], val);
    olnk_write_u16_le(&drv->fat2[(size_t)cluster * 2U], val);
}

static uint16_t vfat_alloc_cluster(vfat_dir_drive_t *drv) {
    if (drv->next_alloc_cluster >= (uint16_t)(drv->num_clusters + 2U)) {
        return 0U;
    }
    return drv->next_alloc_cluster++;
}

static uint32_t vfat_add_backing_file(vfat_dir_drive_t *drv, const char *path, uint32_t size) {
    if (drv->backing_file_count >= drv->backing_file_cap) {
        size_t new_cap = (drv->backing_file_cap == 0U) ? 64U : (drv->backing_file_cap * 2U);
        vfat_backing_file_t *nf = (vfat_backing_file_t *)realloc(
            drv->backing_files, new_cap * sizeof(vfat_backing_file_t)
        );
        if (nf == NULL) {
            return 0xFFFFFFFFUL;
        }
        drv->backing_files = nf;
        drv->backing_file_cap = new_cap;
    }
    vfat_copy_str(
        drv->backing_files[drv->backing_file_count].host_path,
        sizeof(drv->backing_files[drv->backing_file_count].host_path),
        path
    );
    drv->backing_files[drv->backing_file_count].size = size;
    return (uint32_t)(drv->backing_file_count++);
}

static vfat_tracked_node_t *vfat_add_tracked(
    vfat_dir_drive_t *drv,
    const char *dos_path,
    const char *host_path,
    bool is_dir,
    uint16_t first_cluster,
    uint32_t file_size,
    uint64_t synced_gen
) {
    vfat_tracked_node_t *node;
    if (drv->tracked_count >= drv->tracked_cap) {
        size_t new_cap = (drv->tracked_cap == 0U) ? 64U : (drv->tracked_cap * 2U);
        vfat_tracked_node_t *nt = (vfat_tracked_node_t *)realloc(
            drv->tracked, new_cap * sizeof(vfat_tracked_node_t)
        );
        if (nt == NULL) {
            return NULL;
        }
        drv->tracked = nt;
        drv->tracked_cap = new_cap;
    }
    node = &drv->tracked[drv->tracked_count++];
    memset(node, 0, sizeof(*node));
    vfat_copy_str(node->dos_path, sizeof(node->dos_path), dos_path);
    vfat_copy_str(node->host_path, sizeof(node->host_path), host_path);
    node->is_dir = is_dir;
    node->first_cluster = first_cluster;
    node->file_size = file_size;
    node->synced_gen = synced_gen;
    node->seen_in_walk = true;
    return node;
}

static vfat_tracked_node_t *vfat_find_tracked(vfat_dir_drive_t *drv, const char *dos_path) {
    size_t i;
    for (i = 0U; i < drv->tracked_count; ++i) {
        if (strcmp(drv->tracked[i].dos_path, dos_path) == 0) {
            return &drv->tracked[i];
        }
    }
    return NULL;
}

static uint16_t vfat_populate_dir(
    vfat_dir_drive_t *drv,
    const char *host_dir,
    const char *dos_prefix,
    uint16_t parent_cluster,
    bool is_root,
    unsigned int depth
) {
    host_entry_t *entries = NULL;
    size_t count;
    uint8_t (*used_83)[11] = NULL;
    size_t used_count = 0U;
    size_t max_entries;
    uint8_t *dir_buf = NULL;
    uint16_t dir_cluster = 0U;
    size_t dir_slot = 0U;
    size_t i;

    if (depth >= VFAT_MAX_DEPTH) {
        return 0U;
    }

    count = list_host_dir(host_dir, &entries);
    if (is_root) {
        dir_buf = drv->root_dir;
        max_entries = VFAT_ROOT_ENTRIES;
        /* Slot 0: Volume Label entry */
        memset(&dir_buf[0], 0, 32U);
        olnk_label_pad_copy(&dir_buf[0], 11U, drv->volume_label);
        dir_buf[11] = 0x08U; /* Volume ID attribute */
        dir_slot = 1U;
    } else {
        dir_cluster = vfat_alloc_cluster(drv);
        if (dir_cluster == 0U) {
            free(entries);
            return 0U;
        }
        vfat_fat16_set(drv, dir_cluster, 0xFFFFU);
        dir_buf = (uint8_t *)calloc(1U, drv->cluster_bytes);
        if (dir_buf == NULL) {
            free(entries);
            return 0U;
        }
        drv->backing[dir_cluster].kind = (uint8_t)VFAT_BACKING_DIR;
        drv->backing[dir_cluster].dir_bytes = dir_buf;
        max_entries = drv->cluster_bytes / 32U;

        /* '.' and '..' entries */
        memset(&dir_buf[0], ' ', 11U);
        dir_buf[0] = '.';
        dir_buf[11] = 0x10U;
        olnk_write_u16_le(&dir_buf[26], dir_cluster);

        memset(&dir_buf[32], ' ', 11U);
        dir_buf[32] = '.';
        dir_buf[33] = '.';
        dir_buf[32 + 11] = 0x10U;
        olnk_write_u16_le(&dir_buf[32 + 26], parent_cluster);
        dir_slot = 2U;
    }

    if (count > 0U) {
        used_83 = (uint8_t (*)[11])calloc(count, 11U);
    }

    for (i = 0U; (i < count) && (dir_slot < max_entries); ++i) {
        uint8_t name83[11];
        char dos_comp[16];
        char dos_rel[VFAT_MAX_PATH];
        uint8_t *slot = &dir_buf[dir_slot * 32U];
        uint16_t dtime = 0U;
        uint16_t ddate = 0U;
        uint16_t start_cluster = 0U;

        vfat_make_unique_83(entries[i].name, used_83, used_count, name83);
        if (used_83 != NULL) {
            memcpy(used_83[used_count++], name83, 11U);
        }
        vfat_format_83_str(name83, dos_comp);
        if (dos_prefix[0] == '\0') {
            snprintf(dos_rel, sizeof(dos_rel), "%s", dos_comp);
        } else {
            snprintf(dos_rel, sizeof(dos_rel), "%s/%s", dos_prefix, dos_comp);
        }
        vfat_time_to_dos(entries[i].mtime, &dtime, &ddate);

        if (entries[i].is_dir) {
            start_cluster = vfat_populate_dir(
                drv, entries[i].full_path, dos_rel, dir_cluster, false, depth + 1U
            );
            if (start_cluster == 0U) {
                continue;
            }
            memset(slot, 0, 32U);
            memcpy(slot, name83, 11U);
            slot[11] = 0x10U;
            olnk_write_u16_le(&slot[22], dtime);
            olnk_write_u16_le(&slot[24], ddate);
            olnk_write_u16_le(&slot[26], start_cluster);
            olnk_write_u32_le(&slot[28], 0UL);
            (void)vfat_add_tracked(drv, dos_rel, entries[i].full_path, true, start_cluster, 0UL, 0ULL);
            dir_slot += 1U;
        } else {
            uint32_t fsize = entries[i].size;
            uint32_t need_clusters = (fsize > 0UL)
                                         ? ((fsize + drv->cluster_bytes - 1UL) / drv->cluster_bytes)
                                         : 0UL;
            uint32_t fidx = 0xFFFFFFFFUL;
            uint16_t prev_c = 0U;
            uint32_t ci;
            bool ok = true;

            if (need_clusters > 0UL) {
                fidx = vfat_add_backing_file(drv, entries[i].full_path, fsize);
                if (fidx == 0xFFFFFFFFUL) {
                    continue;
                }
                for (ci = 0UL; ci < need_clusters; ++ci) {
                    uint16_t c = vfat_alloc_cluster(drv);
                    if (c == 0U) {
                        ok = false;
                        break;
                    }
                    if (ci == 0UL) {
                        start_cluster = c;
                    } else {
                        vfat_fat16_set(drv, prev_c, c);
                    }
                    vfat_fat16_set(drv, c, 0xFFFFU);
                    drv->backing[c].kind = (uint8_t)VFAT_BACKING_FILE;
                    drv->backing[c].file_index = fidx;
                    drv->backing[c].file_offset = ci * drv->cluster_bytes;
                    prev_c = c;
                }
            }
            if (!ok) {
                continue;
            }

            memset(slot, 0, 32U);
            memcpy(slot, name83, 11U);
            slot[11] = 0x20U; /* Archive */
            olnk_write_u16_le(&slot[22], dtime);
            olnk_write_u16_le(&slot[24], ddate);
            olnk_write_u16_le(&slot[26], start_cluster);
            olnk_write_u32_le(&slot[28], fsize);
            (void)vfat_add_tracked(drv, dos_rel, entries[i].full_path, false, start_cluster, fsize, 0ULL);
            dir_slot += 1U;
        }
    }

    free(used_83);
    free(entries);
    return dir_cluster;
}

static bool vfat_rebuild_from_host(vfat_dir_drive_t *drv) {
    uint64_t host_bytes = 0ULL;
    uint64_t fp;
    uint8_t spc = 8U;         /* 4 KiB clusters -> ~31.25 MiB default */
    uint16_t num_clusters = 8000U;
    uint32_t fat_bytes;

    vfat_clear_image_state(drv);

    fp = compute_host_fingerprint(drv->root_path, &host_bytes);
    /* Scale cluster size cleanly if host folder is large (up to ~500 MiB) */
    if (host_bytes > 24ULL * 1024ULL * 1024ULL) {
        spc = 16U; /* 8 KiB clusters -> ~62.5 MiB */
    }
    if (host_bytes > 52ULL * 1024ULL * 1024ULL) {
        spc = 32U; /* 16 KiB clusters -> ~125 MiB */
    }
    if (host_bytes > 108ULL * 1024ULL * 1024ULL) {
        spc = 64U; /* 32 KiB clusters -> ~250 MiB */
    }
    if (host_bytes > 220ULL * 1024ULL * 1024ULL) {
        num_clusters = 16000U; /* ~500 MiB */
    }

    drv->spc = spc;
    drv->cluster_bytes = (uint32_t)spc * VFAT_BPS;
    drv->num_clusters = num_clusters;
    drv->sectors_per_fat = (uint16_t)((((uint32_t)num_clusters + 2UL) * 2UL + (VFAT_BPS - 1UL)) / VFAT_BPS);
    drv->fat1_lba = VFAT_RESERVED_SECS;
    drv->fat2_lba = drv->fat1_lba + (uint32_t)drv->sectors_per_fat;
    drv->root_lba = drv->fat2_lba + (uint32_t)drv->sectors_per_fat;
    drv->data_lba = drv->root_lba + VFAT_ROOT_SECTORS;
    drv->total_sectors = drv->data_lba + ((uint32_t)num_clusters * (uint32_t)spc);

    fat_bytes = (uint32_t)drv->sectors_per_fat * VFAT_BPS;
    drv->fat1 = (uint8_t *)calloc(1U, fat_bytes);
    drv->fat2 = (uint8_t *)calloc(1U, fat_bytes);
    drv->backing = (vfat_cluster_backing_t *)calloc((size_t)num_clusters + 2U, sizeof(vfat_cluster_backing_t));
    if ((drv->fat1 == NULL) || (drv->fat2 == NULL) || (drv->backing == NULL)) {
        vfat_clear_image_state(drv);
        return false;
    }

    memset(drv->root_dir, 0, sizeof(drv->root_dir));
    drv->next_alloc_cluster = 2U;
    drv->write_gen = 0ULL;

    /* Standard FAT16 media descriptor in entries 0 & 1 */
    vfat_fat16_set(drv, 0U, 0xFFF8U);
    vfat_fat16_set(drv, 1U, 0xFFFFU);

    /* Populate directory tree & file cluster chains */
    (void)vfat_populate_dir(drv, drv->root_path, "", 0U, true, 0U);

    /* Build authentic MS-DOS 6.22 FAT16 VBR at sector 0 */
    {
        olnk_dos_bpb_t bpb;
        memset(drv->vbr, 0, sizeof(drv->vbr));
        memset(&bpb, 0, sizeof(bpb));
        drv->vbr[0] = 0xEBU;
        drv->vbr[1] = 0x3CU;
        drv->vbr[2] = 0x90U;
        memcpy(&drv->vbr[3], "OPENLNK ", 8U);

        bpb.bytes_per_sector = VFAT_BPS;
        bpb.sectors_per_cluster = drv->spc;
        bpb.reserved_sectors = VFAT_RESERVED_SECS;
        bpb.num_fats = VFAT_NUM_FATS;
        bpb.root_entries = VFAT_ROOT_ENTRIES;
        if (drv->total_sectors <= 65535UL) {
            bpb.total_sectors_16 = (uint16_t)drv->total_sectors;
            bpb.total_sectors_32 = 0UL;
        } else {
            bpb.total_sectors_16 = 0U;
            bpb.total_sectors_32 = drv->total_sectors;
        }
        bpb.media_descriptor = 0xF8U;
        bpb.sectors_per_fat = drv->sectors_per_fat;
        bpb.sectors_per_track = 63U;
        bpb.num_heads = 16U;
        bpb.hidden_sectors = 0UL;

        (void)olnk_encode_dos_bpb(&bpb, &drv->vbr[11], OLNK_LEN_DOS_BPB);
        drv->vbr[36] = 0x80U;
        drv->vbr[37] = 0x00U;
        drv->vbr[38] = 0x29U;
        olnk_write_u32_le(&drv->vbr[39], drv->volume_serial);
        olnk_label_pad_copy(&drv->vbr[43], 11U, drv->volume_label);
        olnk_label_pad_copy(&drv->vbr[54], 8U, "FAT16");
        drv->vbr[510] = 0x55U;
        drv->vbr[511] = 0xAAU;

        (void)olnk_fat_parse_vbr(drv->vbr, &drv->info);
        drv->info.write_protected = drv->read_only;
        drv->info.removable = true; /* Allows MS-DOS MEDIA_CHECK live refresh! */
    }

    drv->host_fingerprint = fp;
    drv->last_scan_ms = vfat_now_ms();
    return true;
}

/* =========================================================================
 * Cluster I/O with Copy-on-Write Dirty Cache
 * ========================================================================= */

static vfat_dirty_cluster_t *vfat_find_dirty(vfat_dir_drive_t *drv, uint16_t cluster) {
    size_t i;
    for (i = 0U; i < drv->dirty_count; ++i) {
        if (drv->dirty[i].cluster == cluster) {
            return &drv->dirty[i];
        }
    }
    return NULL;
}

static void vfat_read_clean_cluster(vfat_dir_drive_t *drv, uint16_t cluster, uint8_t *out_buf) {
    vfat_cluster_backing_t *bk;
    memset(out_buf, 0, drv->cluster_bytes);
    if ((cluster < 2U) || (cluster >= (uint16_t)(drv->num_clusters + 2U))) {
        return;
    }
    bk = &drv->backing[cluster];
    if (bk->kind == (uint8_t)VFAT_BACKING_DIR) {
        if (bk->dir_bytes != NULL) {
            memcpy(out_buf, bk->dir_bytes, drv->cluster_bytes);
        }
    } else if (bk->kind == (uint8_t)VFAT_BACKING_FILE) {
        if (bk->file_index < drv->backing_file_count) {
            FILE *fp = fopen(drv->backing_files[bk->file_index].host_path, "rb");
            if (fp != NULL) {
                if (fseeko(fp, (off_t)bk->file_offset, SEEK_SET) == 0) {
                    if ((fread(out_buf, 1U, drv->cluster_bytes, fp) == 0U) && ferror(fp)) {
                        memset(out_buf, 0, drv->cluster_bytes);
                    }
                }
                fclose(fp);
            }
        }
    }
}

static void vfat_read_cluster(vfat_dir_drive_t *drv, uint16_t cluster, uint8_t *out_buf) {
    vfat_dirty_cluster_t *dc = vfat_find_dirty(drv, cluster);
    if (dc != NULL) {
        memcpy(out_buf, dc->data, drv->cluster_bytes);
        return;
    }
    vfat_read_clean_cluster(drv, cluster, out_buf);
}

static vfat_dirty_cluster_t *vfat_get_or_create_dirty(vfat_dir_drive_t *drv, uint16_t cluster) {
    vfat_dirty_cluster_t *dc = vfat_find_dirty(drv, cluster);
    if (dc != NULL) {
        return dc;
    }
    if (drv->dirty_count >= drv->dirty_cap) {
        size_t new_cap = (drv->dirty_cap == 0U) ? 64U : (drv->dirty_cap * 2U);
        vfat_dirty_cluster_t *nd = (vfat_dirty_cluster_t *)realloc(
            drv->dirty, new_cap * sizeof(vfat_dirty_cluster_t)
        );
        if (nd == NULL) {
            return NULL;
        }
        drv->dirty = nd;
        drv->dirty_cap = new_cap;
    }
    dc = &drv->dirty[drv->dirty_count];
    dc->data = (uint8_t *)malloc(drv->cluster_bytes);
    if (dc->data == NULL) {
        return NULL;
    }
    dc->cluster = cluster;
    vfat_read_clean_cluster(drv, cluster, dc->data);
    dc->dirty_gen = 0ULL;
    drv->dirty_count += 1U;
    return dc;
}

/* =========================================================================
 * Read-Write Reconciliation: Virtual FAT16 Tree -> Host Directory
 * ========================================================================= */

static bool vfat_chain_valid_and_max_gen(
    vfat_dir_drive_t *drv,
    uint16_t start_cluster,
    uint32_t file_size,
    uint64_t *out_max_gen
) {
    uint32_t need = (file_size > 0UL)
                        ? ((file_size + drv->cluster_bytes - 1UL) / drv->cluster_bytes)
                        : 0UL;
    uint16_t cur = start_cluster;
    uint32_t steps = 0UL;
    uint64_t max_gen = 0ULL;

    *out_max_gen = 0ULL;
    if (need == 0UL) {
        return true;
    }
    if ((start_cluster < 2U) || (start_cluster >= (uint16_t)(drv->num_clusters + 2U))) {
        return false;
    }

    while (steps < need) {
        vfat_dirty_cluster_t *dc;
        uint16_t next;

        if ((cur < 2U) || (cur >= (uint16_t)(drv->num_clusters + 2U))) {
            return false;
        }
        dc = vfat_find_dirty(drv, cur);
        if ((dc != NULL) && (dc->dirty_gen > max_gen)) {
            max_gen = dc->dirty_gen;
        }
        next = vfat_fat16_get(drv, cur);
        if (next == 0x0000U) {
            /* FAT not flushed yet for this cluster */
            return false;
        }
        steps += 1UL;
        if (steps == need) {
            break;
        }
        if (next >= 0xFFF8U) {
            /* Premature EOF: FAT chain shorter than file_size (in-flight write) */
            return false;
        }
        cur = next;
    }

    *out_max_gen = max_gen;
    return true;
}

static bool vfat_write_chain_to_host_file(
    vfat_dir_drive_t *drv,
    uint16_t start_cluster,
    uint32_t file_size,
    const char *host_path
) {
    char tmp_path[VFAT_MAX_PATH];
    uint8_t *cbuf;
    FILE *fp;
    uint16_t cur = start_cluster;
    uint32_t rem = file_size;
    int n;

    n = snprintf(tmp_path, sizeof(tmp_path), "%s.openlnk.tmp", host_path);
    if ((n <= 0) || ((size_t)n >= sizeof(tmp_path))) {
        return false;
    }

    fp = fopen(tmp_path, "wb");
    if (fp == NULL) {
        return false;
    }

    cbuf = (uint8_t *)malloc(drv->cluster_bytes);
    if (cbuf == NULL) {
        fclose(fp);
        (void)unlink(tmp_path);
        return false;
    }

    while (rem > 0UL) {
        uint32_t chunk = (rem > drv->cluster_bytes) ? drv->cluster_bytes : rem;
        if ((cur < 2U) || (cur >= (uint16_t)(drv->num_clusters + 2U))) {
            free(cbuf);
            fclose(fp);
            (void)unlink(tmp_path);
            return false;
        }
        vfat_read_cluster(drv, cur, cbuf);
        if (fwrite(cbuf, 1U, (size_t)chunk, fp) != (size_t)chunk) {
            free(cbuf);
            fclose(fp);
            (void)unlink(tmp_path);
            return false;
        }
        rem -= chunk;
        if (rem > 0UL) {
            cur = vfat_fat16_get(drv, cur);
        }
    }

    free(cbuf);
    if (fclose(fp) != 0) {
        (void)unlink(tmp_path);
        return false;
    }
    if (rename(tmp_path, host_path) != 0) {
        (void)unlink(tmp_path);
        return false;
    }
    return true;
}

static void vfat_sync_walk_dir(
    vfat_dir_drive_t *drv,
    uint16_t dir_cluster, /* 0 = root */
    const char *dos_prefix,
    const char *host_dir,
    unsigned int depth,
    bool *any_in_flight
) {
    uint8_t *cbuf = NULL;
    uint16_t cur_c = dir_cluster;
    uint32_t guard = 0UL;

    if (depth >= VFAT_MAX_DEPTH) {
        return;
    }

    if (dir_cluster != 0U) {
        cbuf = (uint8_t *)malloc(drv->cluster_bytes);
        if (cbuf == NULL) {
            return;
        }
    }

    for (;;) {
        const uint8_t *buf;
        size_t buf_len;
        size_t off;

        if (dir_cluster == 0U) {
            buf = drv->root_dir;
            buf_len = sizeof(drv->root_dir);
        } else {
            if ((cur_c < 2U) || (cur_c >= (uint16_t)(drv->num_clusters + 2U)) || (guard > 64UL)) {
                break;
            }
            vfat_read_cluster(drv, cur_c, cbuf);
            buf = cbuf;
            buf_len = drv->cluster_bytes;
            guard += 1UL;
        }

        for (off = 0U; (off + 32U) <= buf_len; off += 32U) {
            const uint8_t *ent = &buf[off];
            uint8_t attr = ent[11];
            char dos_comp[16];
            char dos_rel[VFAT_MAX_PATH];
            char host_child[VFAT_MAX_PATH];
            uint16_t fclus;
            uint32_t fsize;
            vfat_tracked_node_t *tn;

            if (ent[0] == 0x00U) {
                goto done_dir;
            }
            if ((ent[0] == 0xE5U) || (attr == 0x0FU) || ((attr & 0x08U) != 0U)) {
                continue;
            }

            vfat_format_83_str(ent, dos_comp);
            if (!vfat_safe_dos_component(dos_comp)) {
                continue;
            }

            if (dos_prefix[0] == '\0') {
                snprintf(dos_rel, sizeof(dos_rel), "%s", dos_comp);
            } else {
                snprintf(dos_rel, sizeof(dos_rel), "%s/%s", dos_prefix, dos_comp);
            }

            fclus = olnk_read_u16_le(&ent[26]);
            fsize = olnk_read_u32_le(&ent[28]);
            tn = vfat_find_tracked(drv, dos_rel);

            if (tn != NULL) {
                vfat_copy_str(host_child, sizeof(host_child), tn->host_path);
            } else {
                int n = snprintf(host_child, sizeof(host_child), "%s/%s", host_dir, dos_comp);
                if ((n <= 0) || ((size_t)n >= sizeof(host_child))) {
                    continue;
                }
            }

            if ((attr & 0x10U) != 0U) {
                /* Subdirectory */
                if ((fclus < 2U) || (fclus >= (uint16_t)(drv->num_clusters + 2U)) ||
                    (vfat_fat16_get(drv, fclus) == 0x0000U)) {
                    *any_in_flight = true;
                    continue;
                }
                if (tn == NULL) {
                    (void)mkdir(host_child, 0755);
                    tn = vfat_add_tracked(drv, dos_rel, host_child, true, fclus, 0UL, drv->write_gen);
                } else {
                    tn->seen_in_walk = true;
                    tn->first_cluster = fclus;
                }
                vfat_sync_walk_dir(drv, fclus, dos_rel, host_child, depth + 1U, any_in_flight);
            } else {
                /* Regular file */
                uint64_t max_gen = 0ULL;
                if (!vfat_chain_valid_and_max_gen(drv, fclus, fsize, &max_gen)) {
                    *any_in_flight = true;
                    if (tn != NULL) {
                        tn->seen_in_walk = true;
                    }
                    continue;
                }

                if (tn == NULL) {
                    if (vfat_write_chain_to_host_file(drv, fclus, fsize, host_child)) {
                        (void)vfat_add_tracked(drv, dos_rel, host_child, false, fclus, fsize, max_gen);
                    }
                } else {
                    bool changed = (tn->file_size != fsize) ||
                                   (tn->first_cluster != fclus) ||
                                   (max_gen > tn->synced_gen);
                    tn->seen_in_walk = true;
                    if (changed) {
                        if (vfat_write_chain_to_host_file(drv, fclus, fsize, tn->host_path)) {
                            tn->file_size = fsize;
                            tn->first_cluster = fclus;
                            tn->synced_gen = max_gen;
                        }
                    }
                }
            }
        }

        if (dir_cluster == 0U) {
            break;
        }
        cur_c = vfat_fat16_get(drv, cur_c);
        if (cur_c >= 0xFFF8U) {
            break;
        }
    }

done_dir:
    free(cbuf);
}

static void vfat_sync_to_host(vfat_dir_drive_t *drv) {
    bool any_in_flight = false;
    size_t i;

    if (drv->read_only) {
        return;
    }

    for (i = 0U; i < drv->tracked_count; ++i) {
        drv->tracked[i].seen_in_walk = false;
    }

    vfat_sync_walk_dir(drv, 0U, "", drv->root_path, 0U, &any_in_flight);

    /* Remove files deleted by DOS (two-pass: files first, then dirs deepest-first).
     * Preserve any still-allocated backing clusters (e.g. DOS REN) in dirty cache before unlink. */
    for (i = 0U; i < drv->tracked_count; ++i) {
        if ((!drv->tracked[i].seen_in_walk) && (!drv->tracked[i].is_dir)) {
            uint16_t c;
            for (c = 2U; c < (uint16_t)(drv->num_clusters + 2U); ++c) {
                if ((drv->backing[c].kind == (uint8_t)VFAT_BACKING_FILE) &&
                    (drv->backing[c].file_index < drv->backing_file_count) &&
                    (strcmp(drv->backing_files[drv->backing[c].file_index].host_path,
                            drv->tracked[i].host_path) == 0) &&
                    (vfat_fat16_get(drv, c) != 0x0000U)) {
                    (void)vfat_get_or_create_dirty(drv, c);
                }
            }
            (void)unlink(drv->tracked[i].host_path);
        }
    }
    for (i = drv->tracked_count; i > 0U; --i) {
        size_t idx = i - 1U;
        if ((!drv->tracked[idx].seen_in_walk) && drv->tracked[idx].is_dir) {
            (void)rmdir(drv->tracked[idx].host_path);
        }
    }

    /* Compact tracked array */
    {
        size_t dst = 0U;
        for (i = 0U; i < drv->tracked_count; ++i) {
            if (drv->tracked[i].seen_in_walk) {
                if (dst != i) {
                    drv->tracked[dst] = drv->tracked[i];
                }
                dst += 1U;
            }
        }
        drv->tracked_count = dst;
    }

    drv->has_in_flight_writes = any_in_flight;

    /* Update expected host fingerprint so our own DOS writes never trigger MEDIA_CHANGED */
    if (!any_in_flight) {
        drv->host_fingerprint = compute_host_fingerprint(drv->root_path, NULL);
        drv->last_scan_ms = vfat_now_ms();
    }
}

/* =========================================================================
 * Public Virtual FAT16 Storage Operations
 * ========================================================================= */

vfat_dir_drive_t *vfat_dir_open(
    const char *root_path,
    bool read_only,
    const char *volume_label
) {
    struct stat st;
    vfat_dir_drive_t *drv;
    const char *base_name;

    if ((root_path == NULL) || (stat(root_path, &st) != 0) || (!S_ISDIR(st.st_mode))) {
        fprintf(stderr, "[opensvr] ERROR: Not a directory: '%s'\n",
                (root_path != NULL) ? root_path : "");
        return NULL;
    }

    drv = (vfat_dir_drive_t *)calloc(1U, sizeof(vfat_dir_drive_t));
    if (drv == NULL) {
        return NULL;
    }

    vfat_copy_str(drv->root_path, sizeof(drv->root_path), root_path);
    /* Strip trailing slashes (except "/") */
    {
        size_t rlen = strlen(drv->root_path);
        while ((rlen > 1U) && (drv->root_path[rlen - 1U] == '/')) {
            drv->root_path[--rlen] = '\0';
        }
    }
    drv->read_only = read_only;
    drv->volume_serial = 0x56464154UL; /* "VFAT" */

    if ((volume_label != NULL) && (volume_label[0] != '\0')) {
        base_name = volume_label;
    } else {
        const char *slash = strrchr(drv->root_path, '/');
        base_name = ((slash != NULL) && (slash[1] != '\0')) ? (slash + 1) : "OPENLNK_DIR";
    }
    {
        size_t pos = 0U;
        const char *p = base_name;
        memset(drv->volume_label, 0, sizeof(drv->volume_label));
        while ((*p != '\0') && (pos < 11U)) {
            if (vfat_is_valid_dos_char(*p)) {
                drv->volume_label[pos++] = (char)toupper((unsigned char)*p);
            }
            ++p;
        }
        if (pos == 0U) {
            strcpy(drv->volume_label, "OPENLNK_DIR");
        }
    }

    if (!vfat_rebuild_from_host(drv)) {
        free(drv);
        return NULL;
    }
    return drv;
}

void vfat_dir_close(vfat_dir_drive_t *drv) {
    if (drv == NULL) {
        return;
    }
    if (!drv->read_only) {
        vfat_sync_to_host(drv);
    }
    vfat_clear_image_state(drv);
    free(drv);
}

uint16_t vfat_dir_get_info(vfat_dir_drive_t *drv, olnk_drive_info_t *out_info) {
    if ((drv == NULL) || (out_info == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    *out_info = drv->info;
    return OLNK_DOS_STATUS_DONE;
}

uint16_t vfat_dir_media_check(
    vfat_dir_drive_t *drv,
    uint8_t media_descriptor,
    uint8_t *out_media_status,
    olnk_drive_info_t *out_info
) {
    uint64_t now;
    (void)media_descriptor;

    if ((drv == NULL) || (out_media_status == NULL) || (out_info == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }

    now = vfat_now_ms();
    if ((!drv->has_in_flight_writes) && ((now - drv->last_scan_ms) >= VFAT_RESCAN_MIN_MS)) {
        uint64_t fp = compute_host_fingerprint(drv->root_path, NULL);
        drv->last_scan_ms = now;
        if (fp != drv->host_fingerprint) {
            drv->volume_serial += 1UL;
            if (vfat_rebuild_from_host(drv)) {
                drv->media_changed_pending = true;
            }
        }
    }

    *out_info = drv->info;
    if (drv->media_changed_pending) {
        drv->media_changed_pending = false;
        *out_media_status = (uint8_t)OLNK_MEDIA_CHANGED;
    } else {
        *out_media_status = (uint8_t)OLNK_MEDIA_UNCHANGED;
    }
    return OLNK_DOS_STATUS_DONE;
}

uint16_t vfat_dir_build_bpb(
    vfat_dir_drive_t *drv,
    uint8_t media_descriptor,
    olnk_dos_bpb_t *out_bpb
) {
    (void)media_descriptor;
    if ((drv == NULL) || (out_bpb == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    *out_bpb = drv->info.bpb;
    return OLNK_DOS_STATUS_DONE;
}

static void vfat_read_one_sector(vfat_dir_drive_t *drv, uint32_t lba, uint8_t out_sec[VFAT_BPS]) {
    if (lba == 0UL) {
        memcpy(out_sec, drv->vbr, VFAT_BPS);
        return;
    }
    if ((lba >= drv->fat1_lba) && (lba < drv->fat2_lba)) {
        size_t off = (size_t)(lba - drv->fat1_lba) * VFAT_BPS;
        memcpy(out_sec, &drv->fat1[off], VFAT_BPS);
        return;
    }
    if ((lba >= drv->fat2_lba) && (lba < drv->root_lba)) {
        size_t off = (size_t)(lba - drv->fat2_lba) * VFAT_BPS;
        memcpy(out_sec, &drv->fat2[off], VFAT_BPS);
        return;
    }
    if ((lba >= drv->root_lba) && (lba < drv->data_lba)) {
        size_t off = (size_t)(lba - drv->root_lba) * VFAT_BPS;
        memcpy(out_sec, &drv->root_dir[off], VFAT_BPS);
        return;
    }
    if (lba < drv->total_sectors) {
        uint32_t rel = lba - drv->data_lba;
        uint16_t cluster = (uint16_t)(2UL + (rel / (uint32_t)drv->spc));
        uint32_t sec_in_cluster = rel % (uint32_t)drv->spc;
        vfat_dirty_cluster_t *dc = vfat_find_dirty(drv, cluster);

        if (dc != NULL) {
            memcpy(out_sec, &dc->data[(size_t)sec_in_cluster * VFAT_BPS], VFAT_BPS);
            return;
        }
        {
            uint8_t *cbuf = (uint8_t *)malloc(drv->cluster_bytes);
            if (cbuf != NULL) {
                vfat_read_clean_cluster(drv, cluster, cbuf);
                memcpy(out_sec, &cbuf[(size_t)sec_in_cluster * VFAT_BPS], VFAT_BPS);
                free(cbuf);
                return;
            }
        }
    }
    memset(out_sec, 0, VFAT_BPS);
}

uint16_t vfat_dir_read_sectors(
    vfat_dir_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    uint8_t *out_buf,
    uint16_t *out_transferred
) {
    uint32_t end_lba;
    uint16_t i;

    if (out_transferred != NULL) {
        *out_transferred = 0U;
    }
    if ((drv == NULL) || (out_buf == NULL) || (sector_count == 0U)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
    }
    end_lba = start_lba + (uint32_t)sector_count;
    if ((end_lba < start_lba) || (end_lba > drv->total_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }

    for (i = 0U; i < sector_count; ++i) {
        vfat_read_one_sector(drv, start_lba + (uint32_t)i, &out_buf[(size_t)i * VFAT_BPS]);
    }

    if (out_transferred != NULL) {
        *out_transferred = sector_count;
    }
    return OLNK_DOS_STATUS_DONE;
}

uint16_t vfat_dir_write_sectors(
    vfat_dir_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    const uint8_t *in_buf,
    uint16_t *out_transferred,
    bool verify
) {
    uint32_t end_lba;
    uint16_t i;
    (void)verify;

    if (out_transferred != NULL) {
        *out_transferred = 0U;
    }
    if (drv == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (drv->read_only) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
    }
    if ((in_buf == NULL) || (sector_count == 0U)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
    }
    end_lba = start_lba + (uint32_t)sector_count;
    if ((end_lba < start_lba) || (end_lba > drv->total_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }

    drv->write_gen += 1ULL;

    for (i = 0U; i < sector_count; ++i) {
        uint32_t lba = start_lba + (uint32_t)i;
        const uint8_t *src = &in_buf[(size_t)i * VFAT_BPS];

        if (lba == 0UL) {
            /* Keep virtual VBR intact */
            continue;
        }
        if ((lba >= drv->fat1_lba) && (lba < drv->fat2_lba)) {
            size_t off = (size_t)(lba - drv->fat1_lba) * VFAT_BPS;
            memcpy(&drv->fat1[off], src, VFAT_BPS);
            continue;
        }
        if ((lba >= drv->fat2_lba) && (lba < drv->root_lba)) {
            size_t off = (size_t)(lba - drv->fat2_lba) * VFAT_BPS;
            memcpy(&drv->fat2[off], src, VFAT_BPS);
            continue;
        }
        if ((lba >= drv->root_lba) && (lba < drv->data_lba)) {
            size_t off = (size_t)(lba - drv->root_lba) * VFAT_BPS;
            memcpy(&drv->root_dir[off], src, VFAT_BPS);
            continue;
        }
        if (lba < drv->total_sectors) {
            uint32_t rel = lba - drv->data_lba;
            uint16_t cluster = (uint16_t)(2UL + (rel / (uint32_t)drv->spc));
            uint32_t sec_in_cluster = rel % (uint32_t)drv->spc;
            vfat_dirty_cluster_t *dc = vfat_get_or_create_dirty(drv, cluster);
            if (dc == NULL) {
                return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
            }
            memcpy(&dc->data[(size_t)sec_in_cluster * VFAT_BPS], src, VFAT_BPS);
            dc->dirty_gen = drv->write_gen;
        }
    }

    vfat_sync_to_host(drv);

    if (out_transferred != NULL) {
        *out_transferred = sector_count;
    }
    return OLNK_DOS_STATUS_DONE;
}

uint16_t vfat_dir_set_media_id(vfat_dir_drive_t *drv, const olnk_generic_ioctl_mid_t *mid) {
    if ((drv == NULL) || (mid == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (drv->read_only) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
    }
    drv->volume_serial = mid->serial_number;
    olnk_write_u32_le(&drv->vbr[39], mid->serial_number);
    memcpy(&drv->vbr[43], mid->volume_label, 11U);
    olnk_label_trim_copy(drv->volume_label, sizeof(drv->volume_label), (const char *)mid->volume_label, 11U);
    (void)olnk_fat_parse_vbr(drv->vbr, &drv->info);
    drv->info.write_protected = drv->read_only;
    drv->info.removable = true;
    return OLNK_DOS_STATUS_DONE;
}

#endif /* !_WIN32 */
