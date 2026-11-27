/* esp32/src/esp32_client_vfs.c
 *
 * FatFs DiskIO block-driver translation layer and ESP-IDF POSIX VFS mount
 * bridge (e.g. "/dos") for remote MS-DOS INTERSVR.EXE / OPENSVR.EXE / opensvr
 * drives accessed over olnk_esp32_client_t.
 *
 * Architecture:
 *   - Platform-independent DiskIO core (olnk_esp32_vfs_disk_*) compiles and is
 *     tested directly on host CTest without ESP-IDF or FatFs headers.
 *   - On ESP-IDF (ESP_PLATFORM), wraps the core in ff_diskio_impl_t, allocates
 *     a free FatFs pdrv via ff_diskio_get_drive() (coexists with "/sdcard"),
 *     registers VFS via esp_vfs_fat_register_cfg() on IDF >= 5.3.0 (zero
 *     deprecation warnings), and mounts immediately with f_mount(..., 1).
 *
 * Strictly ISO C99, zero dynamic allocation (malloc/free), zero floating-point.
 */
#include "openlnk_esp32.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "diskio_impl.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#endif

/* =========================================================================
 * Helper: Map DOS Status Word to DiskIO Result
 * ========================================================================= */

static olnk_esp32_disk_res_t map_dos_status_to_disk_res(uint16_t dos_status) {
    uint8_t err_code;

    if ((dos_status & OLNK_DOS_STATUS_ERROR) == 0U) {
        return OLNK_ESP32_DISK_RES_OK;
    }

    err_code = (uint8_t)(dos_status & 0x00FFU);
    switch (err_code) {
        case OLNK_DOS_ERR_WRITE_PROTECT:
            return OLNK_ESP32_DISK_RES_WRPRT;
        case OLNK_DOS_ERR_DRIVE_NOT_READY:
            return OLNK_ESP32_DISK_RES_NOTRDY;
        default:
            return OLNK_ESP32_DISK_RES_ERROR;
    }
}

/* =========================================================================
 * Platform-Independent DiskIO Core API (Host + ESP-IDF)
 * ========================================================================= */

void olnk_esp32_vfs_cfg_default(
    olnk_esp32_vfs_cfg_t *cfg,
    const char *base_path,
    uint8_t unit
) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->base_path = (base_path != NULL) ? base_path : "/dos";
    cfg->unit = unit;
    cfg->read_only = false;
    cfg->write_verify = false;
    cfg->max_files = OLNK_ESP32_VFS_DEFAULT_MAX_FILES;
}

olnk_status_t olnk_esp32_vfs_disk_attach(
    olnk_esp32_vfs_mount_t *mnt,
    olnk_esp32_client_t *client,
    const olnk_esp32_vfs_cfg_t *cfg
) {
    olnk_esp32_vfs_cfg_t eff_cfg;
    const char *path;
    size_t path_len;

    if ((mnt == NULL) || (client == NULL) || (!client->initialized)) {
        return OLNK_ERR_INVALID_ARG;
    }

    olnk_esp32_vfs_cfg_default(&eff_cfg, "/dos", OLNK_ESP32_UNIT_AUTO);
    if (cfg != NULL) {
        eff_cfg = *cfg;
    }

    path = (eff_cfg.base_path != NULL) ? eff_cfg.base_path : "/dos";
    path_len = strlen(path);
    if ((path_len < 2U) || (path[0] != '/') || (path_len >= sizeof(mnt->base_path))) {
        return OLNK_ERR_INVALID_ARG;
    }
    if ((eff_cfg.unit != OLNK_ESP32_UNIT_AUTO) && (eff_cfg.unit >= OLNK_MAX_DRIVES)) {
        return OLNK_ERR_INVALID_ARG;
    }

    memset(mnt, 0, sizeof(*mnt));
    mnt->client = client;
    mnt->unit = eff_cfg.unit;
    mnt->cfg_read_only = eff_cfg.read_only;
    mnt->read_only = eff_cfg.read_only;
    mnt->write_verify = eff_cfg.write_verify;
    mnt->max_files = (eff_cfg.max_files != 0U)
                         ? eff_cfg.max_files
                         : OLNK_ESP32_VFS_DEFAULT_MAX_FILES;
    mnt->pdrv = OLNK_ESP32_VFS_PDRV_NONE;
    memcpy(mnt->base_path, path, path_len + 1U);
    return OLNK_OK;
}

uint8_t olnk_esp32_vfs_disk_init(olnk_esp32_vfs_mount_t *mnt) {
    olnk_media_check_resp_t mcresp;
    olnk_build_bpb_resp_t bpbresp;
    uint16_t bps;
    uint32_t total;
    olnk_status_t st;

    if ((mnt == NULL) || (mnt->client == NULL) || (!mnt->client->initialized)) {
        return (uint8_t)(OLNK_ESP32_DISK_STA_NOINIT | OLNK_ESP32_DISK_STA_NODISK);
    }

    mnt->disk_ready = false;

    if (mnt->unit == OLNK_ESP32_UNIT_AUTO) {
        uint8_t auto_unit = 0U;
        if (olnk_esp32_client_select_default_unit(mnt->client, &auto_unit) != OLNK_OK) {
            return (uint8_t)(OLNK_ESP32_DISK_STA_NOINIT | OLNK_ESP32_DISK_STA_NODISK);
        }
        mnt->unit = auto_unit;
    }
    if (mnt->unit >= OLNK_MAX_DRIVES) {
        return (uint8_t)(OLNK_ESP32_DISK_STA_NOINIT | OLNK_ESP32_DISK_STA_NODISK);
    }

    /* Step 1: MEDIA_CHECK (matches MS-DOS block driver mount sequence) */
    memset(&mcresp, 0, sizeof(mcresp));
    st = olnk_esp32_client_media_check(mnt->client, mnt->unit, 0xF8U, &mcresp);
    if (st != OLNK_OK) {
        return (uint8_t)(OLNK_ESP32_DISK_STA_NOINIT | OLNK_ESP32_DISK_STA_NODISK);
    }
    if (((mcresp.status & OLNK_DOS_STATUS_ERROR) != 0U) &&
        ((uint8_t)(mcresp.status & 0x00FFU) == OLNK_DOS_ERR_DRIVE_NOT_READY)) {
        return (uint8_t)(OLNK_ESP32_DISK_STA_NOINIT | OLNK_ESP32_DISK_STA_NODISK);
    }

    /* Step 2: BUILD_BPB to cache partition-relative FAT12/FAT16 volume geometry */
    memset(&bpbresp, 0, sizeof(bpbresp));
    st = olnk_esp32_client_build_bpb(mnt->client, mnt->unit, 0xF8U, &bpbresp);
    if (st != OLNK_OK) {
        return (uint8_t)OLNK_ESP32_DISK_STA_NOINIT;
    }
    if (((bpbresp.status & OLNK_DOS_STATUS_ERROR) != 0U) || (!bpbresp.has_bpb)) {
        if ((uint8_t)(bpbresp.status & 0x00FFU) == OLNK_DOS_ERR_DRIVE_NOT_READY) {
            return (uint8_t)(OLNK_ESP32_DISK_STA_NOINIT | OLNK_ESP32_DISK_STA_NODISK);
        }
        return (uint8_t)OLNK_ESP32_DISK_STA_NOINIT;
    }

    /* Step 3: Validate geometry (FatFs requires 512-byte sectors & non-zero count) */
    bps = bpbresp.bpb.bytes_per_sector;
    total = (bpbresp.bpb.total_sectors_16 != 0U)
                ? (uint32_t)bpbresp.bpb.total_sectors_16
                : bpbresp.bpb.total_sectors_32;
    if ((bps != OLNK_SECTOR_SIZE) || (total == 0UL)) {
        return (uint8_t)OLNK_ESP32_DISK_STA_NOINIT;
    }

    mnt->bpb = bpbresp.bpb;
    mnt->bytes_per_sector = bps;
    mnt->total_sectors = total;
    mnt->sectors_per_cluster = (bpbresp.bpb.sectors_per_cluster != 0U)
                                   ? bpbresp.bpb.sectors_per_cluster
                                   : 1U;
    mnt->media_descriptor = (bpbresp.bpb.media_descriptor != 0U)
                                ? bpbresp.bpb.media_descriptor
                                : 0xF8U;
    mnt->read_only = mnt->cfg_read_only ||
                     olnk_esp32_client_unit_is_readonly(mnt->client, mnt->unit);
    mnt->disk_ready = true;

    return mnt->read_only
               ? (uint8_t)OLNK_ESP32_DISK_STA_PROTECT
               : (uint8_t)OLNK_ESP32_DISK_STA_OK;
}

uint8_t olnk_esp32_vfs_disk_status(const olnk_esp32_vfs_mount_t *mnt) {
    if ((mnt == NULL) || (!mnt->disk_ready)) {
        return (uint8_t)OLNK_ESP32_DISK_STA_NOINIT;
    }
    return mnt->read_only
               ? (uint8_t)OLNK_ESP32_DISK_STA_PROTECT
               : (uint8_t)OLNK_ESP32_DISK_STA_OK;
}

olnk_esp32_disk_res_t olnk_esp32_vfs_disk_read(
    olnk_esp32_vfs_mount_t *mnt,
    uint8_t *buff,
    uint32_t sector,
    uint32_t count
) {
    uint32_t max_blk;
    uint32_t max_chunk_secs;
    uint32_t done = 0UL;

    if ((mnt == NULL) || (buff == NULL) || (count == 0UL)) {
        return OLNK_ESP32_DISK_RES_PARERR;
    }
    if ((!mnt->disk_ready) || (mnt->client == NULL) || (mnt->bytes_per_sector != OLNK_SECTOR_SIZE)) {
        return OLNK_ESP32_DISK_RES_NOTRDY;
    }
    if ((sector >= mnt->total_sectors) || (count > (mnt->total_sectors - sector))) {
        return OLNK_ESP32_DISK_RES_PARERR;
    }

    max_blk = (uint32_t)olnk_esp32_client_max_block_size(mnt->client);
    max_chunk_secs = max_blk / (uint32_t)mnt->bytes_per_sector;
    if (max_chunk_secs == 0UL) {
        max_chunk_secs = 1UL;
    }
    if (max_chunk_secs > 0xFFFFUL) {
        max_chunk_secs = 0xFFFFUL;
    }

    while (done < count) {
        uint32_t rem = count - done;
        uint16_t chunk = (rem > max_chunk_secs) ? (uint16_t)max_chunk_secs : (uint16_t)rem;
        uint32_t byte_off = done * (uint32_t)mnt->bytes_per_sector;
        uint32_t chunk_bytes = (uint32_t)chunk * (uint32_t)mnt->bytes_per_sector;
        olnk_sector_io_resp_t ior;
        olnk_status_t st;

        memset(&ior, 0, sizeof(ior));
        st = olnk_esp32_client_read_sectors(
            mnt->client,
            mnt->unit,
            mnt->media_descriptor,
            sector + done,
            chunk,
            mnt->bytes_per_sector,
            &buff[byte_off],
            chunk_bytes,
            &ior
        );
        if (st != OLNK_OK) {
            mnt->disk_errors += 1UL;
            return OLNK_ESP32_DISK_RES_ERROR;
        }
        if ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U) {
            mnt->disk_errors += 1UL;
            return map_dos_status_to_disk_res(ior.status);
        }
        if (ior.sectors_transferred != chunk) {
            mnt->disk_errors += 1UL;
            return OLNK_ESP32_DISK_RES_ERROR;
        }
        done += (uint32_t)chunk;
    }

    mnt->disk_reads += 1UL;
    return OLNK_ESP32_DISK_RES_OK;
}

olnk_esp32_disk_res_t olnk_esp32_vfs_disk_write(
    olnk_esp32_vfs_mount_t *mnt,
    const uint8_t *buff,
    uint32_t sector,
    uint32_t count
) {
    uint32_t max_blk;
    uint32_t max_chunk_secs;
    uint32_t done = 0UL;

    if ((mnt == NULL) || (buff == NULL) || (count == 0UL)) {
        return OLNK_ESP32_DISK_RES_PARERR;
    }
    if ((!mnt->disk_ready) || (mnt->client == NULL) || (mnt->bytes_per_sector != OLNK_SECTOR_SIZE)) {
        return OLNK_ESP32_DISK_RES_NOTRDY;
    }
    if (mnt->read_only) {
        return OLNK_ESP32_DISK_RES_WRPRT;
    }
    if ((sector >= mnt->total_sectors) || (count > (mnt->total_sectors - sector))) {
        return OLNK_ESP32_DISK_RES_PARERR;
    }

    max_blk = (uint32_t)olnk_esp32_client_max_block_size(mnt->client);
    max_chunk_secs = max_blk / (uint32_t)mnt->bytes_per_sector;
    if (max_chunk_secs == 0UL) {
        max_chunk_secs = 1UL;
    }
    if (max_chunk_secs > 0xFFFFUL) {
        max_chunk_secs = 0xFFFFUL;
    }

    while (done < count) {
        uint32_t rem = count - done;
        uint16_t chunk = (rem > max_chunk_secs) ? (uint16_t)max_chunk_secs : (uint16_t)rem;
        uint32_t byte_off = done * (uint32_t)mnt->bytes_per_sector;
        olnk_sector_io_resp_t ior;
        olnk_status_t st;

        memset(&ior, 0, sizeof(ior));
        st = olnk_esp32_client_write_sectors(
            mnt->client,
            mnt->unit,
            mnt->media_descriptor,
            sector + done,
            chunk,
            mnt->bytes_per_sector,
            &buff[byte_off],
            mnt->write_verify,
            &ior
        );
        if (st != OLNK_OK) {
            mnt->disk_errors += 1UL;
            return OLNK_ESP32_DISK_RES_ERROR;
        }
        if ((ior.status & OLNK_DOS_STATUS_ERROR) != 0U) {
            mnt->disk_errors += 1UL;
            return map_dos_status_to_disk_res(ior.status);
        }
        if (ior.sectors_transferred != chunk) {
            mnt->disk_errors += 1UL;
            return OLNK_ESP32_DISK_RES_ERROR;
        }
        done += (uint32_t)chunk;
    }

    mnt->disk_writes += 1UL;
    return OLNK_ESP32_DISK_RES_OK;
}

olnk_esp32_disk_res_t olnk_esp32_vfs_disk_ioctl(
    olnk_esp32_vfs_mount_t *mnt,
    uint8_t cmd,
    void *buff
) {
    if (mnt == NULL) {
        return OLNK_ESP32_DISK_RES_PARERR;
    }
    if (!mnt->disk_ready) {
        return OLNK_ESP32_DISK_RES_NOTRDY;
    }

    switch (cmd) {
        case OLNK_ESP32_DISK_CTRL_SYNC:
            /* Remote sector writes are synchronous over OpenLNK RPC */
            return OLNK_ESP32_DISK_RES_OK;

        case OLNK_ESP32_DISK_GET_SECTOR_COUNT:
            if (buff == NULL) {
                return OLNK_ESP32_DISK_RES_PARERR;
            }
#ifdef ESP_PLATFORM
            *(LBA_t *)buff = (LBA_t)mnt->total_sectors;
#else
            *(uint32_t *)buff = mnt->total_sectors;
#endif
            return OLNK_ESP32_DISK_RES_OK;

        case OLNK_ESP32_DISK_GET_SECTOR_SIZE:
            if (buff == NULL) {
                return OLNK_ESP32_DISK_RES_PARERR;
            }
#ifdef ESP_PLATFORM
            *(WORD *)buff = (WORD)mnt->bytes_per_sector;
#else
            *(uint16_t *)buff = mnt->bytes_per_sector;
#endif
            return OLNK_ESP32_DISK_RES_OK;

        case OLNK_ESP32_DISK_GET_BLOCK_SIZE:
            if (buff == NULL) {
                return OLNK_ESP32_DISK_RES_PARERR;
            }
#ifdef ESP_PLATFORM
            *(DWORD *)buff = (DWORD)((mnt->sectors_per_cluster != 0U)
                                         ? mnt->sectors_per_cluster
                                         : 1U);
#else
            *(uint32_t *)buff = (uint32_t)((mnt->sectors_per_cluster != 0U)
                                               ? mnt->sectors_per_cluster
                                               : 1U);
#endif
            return OLNK_ESP32_DISK_RES_OK;

        default:
            return OLNK_ESP32_DISK_RES_PARERR;
    }
}

/* =========================================================================
 * ESP-IDF FatFs DiskIO & VFS Registration Glue
 * ========================================================================= */

#ifdef ESP_PLATFORM

#if (STA_NOINIT != OLNK_ESP32_DISK_STA_NOINIT) || \
    (STA_NODISK != OLNK_ESP32_DISK_STA_NODISK) || \
    (STA_PROTECT != OLNK_ESP32_DISK_STA_PROTECT)
#error "FatFs STA_* flags diverged from OLNK_ESP32_DISK_STA_* constants"
#endif

#if (CTRL_SYNC != OLNK_ESP32_DISK_CTRL_SYNC) || \
    (GET_SECTOR_COUNT != OLNK_ESP32_DISK_GET_SECTOR_COUNT) || \
    (GET_SECTOR_SIZE != OLNK_ESP32_DISK_GET_SECTOR_SIZE) || \
    (GET_BLOCK_SIZE != OLNK_ESP32_DISK_GET_BLOCK_SIZE)
#error "FatFs ioctl commands diverged from OLNK_ESP32_DISK_* constants"
#endif

/* Static pdrv slot table (zero malloc) */
static olnk_esp32_vfs_mount_t *s_vfs_mounts[FF_VOLUMES];

static DRESULT to_fatfs_dresult(olnk_esp32_disk_res_t res) {
    switch (res) {
        case OLNK_ESP32_DISK_RES_OK:     return RES_OK;
        case OLNK_ESP32_DISK_RES_WRPRT:  return RES_WRPRT;
        case OLNK_ESP32_DISK_RES_NOTRDY: return RES_NOTRDY;
        case OLNK_ESP32_DISK_RES_PARERR: return RES_PARERR;
        case OLNK_ESP32_DISK_RES_ERROR:
        default:                         return RES_ERROR;
    }
}

static DSTATUS idf_olnk_disk_init(unsigned char pdrv) {
    if ((pdrv >= (unsigned char)FF_VOLUMES) || (s_vfs_mounts[pdrv] == NULL)) {
        return (DSTATUS)(STA_NOINIT | STA_NODISK);
    }
    return (DSTATUS)olnk_esp32_vfs_disk_init(s_vfs_mounts[pdrv]);
}

static DSTATUS idf_olnk_disk_status(unsigned char pdrv) {
    if ((pdrv >= (unsigned char)FF_VOLUMES) || (s_vfs_mounts[pdrv] == NULL)) {
        return (DSTATUS)STA_NOINIT;
    }
    return (DSTATUS)olnk_esp32_vfs_disk_status(s_vfs_mounts[pdrv]);
}

static DRESULT idf_olnk_disk_read(
    unsigned char pdrv,
    unsigned char *buff,
    uint32_t sector,
    unsigned int count
) {
    if ((pdrv >= (unsigned char)FF_VOLUMES) || (s_vfs_mounts[pdrv] == NULL)) {
        return RES_NOTRDY;
    }
    return to_fatfs_dresult(
        olnk_esp32_vfs_disk_read(s_vfs_mounts[pdrv], (uint8_t *)buff, sector, (uint32_t)count)
    );
}

static DRESULT idf_olnk_disk_write(
    unsigned char pdrv,
    const unsigned char *buff,
    uint32_t sector,
    unsigned int count
) {
    if ((pdrv >= (unsigned char)FF_VOLUMES) || (s_vfs_mounts[pdrv] == NULL)) {
        return RES_NOTRDY;
    }
    return to_fatfs_dresult(
        olnk_esp32_vfs_disk_write(s_vfs_mounts[pdrv], (const uint8_t *)buff, sector, (uint32_t)count)
    );
}

static DRESULT idf_olnk_disk_ioctl(unsigned char pdrv, unsigned char cmd, void *buff) {
    if ((pdrv >= (unsigned char)FF_VOLUMES) || (s_vfs_mounts[pdrv] == NULL)) {
        return RES_NOTRDY;
    }
    return to_fatfs_dresult(
        olnk_esp32_vfs_disk_ioctl(s_vfs_mounts[pdrv], (uint8_t)cmd, buff)
    );
}

static const ff_diskio_impl_t s_olnk_diskio_impl = {
    .init   = idf_olnk_disk_init,
    .status = idf_olnk_disk_status,
    .read   = idf_olnk_disk_read,
    .write  = idf_olnk_disk_write,
    .ioctl  = idf_olnk_disk_ioctl
};

#endif /* ESP_PLATFORM */

olnk_status_t olnk_esp32_vfs_mount(
    olnk_esp32_vfs_mount_t *mnt,
    olnk_esp32_client_t *client,
    const olnk_esp32_vfs_cfg_t *cfg
) {
#ifdef ESP_PLATFORM
    BYTE pdrv = FF_DRV_NOT_USED;
    FATFS *fs = NULL;
    esp_err_t err;
    FRESULT fres;
    olnk_status_t st;

    st = olnk_esp32_vfs_disk_attach(mnt, client, cfg);
    if (st != OLNK_OK) {
        return st;
    }

    if (mnt->unit == OLNK_ESP32_UNIT_AUTO) {
        st = olnk_esp32_client_select_default_unit(client, &mnt->unit);
        if (st != OLNK_OK) {
            return st;
        }
    }

    if ((ff_diskio_get_drive(&pdrv) != ESP_OK) ||
        (pdrv >= (BYTE)FF_VOLUMES) ||
        (pdrv >= 10U)) {
        return OLNK_ERR_BUFFER_OVERFLOW;
    }

    mnt->pdrv = (uint8_t)pdrv;
    mnt->fat_drv_str[0] = (char)('0' + (int)pdrv);
    mnt->fat_drv_str[1] = ':';
    mnt->fat_drv_str[2] = '\0';
    mnt->fat_drv_str[3] = '\0';

    s_vfs_mounts[pdrv] = mnt;
    ff_diskio_register(pdrv, &s_olnk_diskio_impl);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    {
        esp_vfs_fat_conf_t vfs_conf;
        memset(&vfs_conf, 0, sizeof(vfs_conf));
        vfs_conf.base_path = mnt->base_path;
        vfs_conf.fat_drive = mnt->fat_drv_str;
        vfs_conf.max_files = (size_t)mnt->max_files;
        err = esp_vfs_fat_register_cfg(&vfs_conf, &fs);
    }
#else
    err = esp_vfs_fat_register(
        mnt->base_path,
        mnt->fat_drv_str,
        (size_t)mnt->max_files,
        &fs
    );
#endif
    if ((err != ESP_OK) || (fs == NULL)) {
        ff_diskio_unregister(pdrv);
        s_vfs_mounts[pdrv] = NULL;
        mnt->pdrv = OLNK_ESP32_VFS_PDRV_NONE;
        return OLNK_ERR_HAL_IO;
    }

    /* Immediate mount (opt=1) validates BPB and FAT12/FAT16 volume immediately */
    fres = f_mount(fs, mnt->fat_drv_str, 1);
    if (fres != FR_OK) {
        (void)esp_vfs_fat_unregister_path(mnt->base_path);
        ff_diskio_unregister(pdrv);
        s_vfs_mounts[pdrv] = NULL;
        mnt->pdrv = OLNK_ESP32_VFS_PDRV_NONE;
        mnt->fatfs = NULL;
        mnt->disk_ready = false;
        return OLNK_ERR_HAL_IO;
    }

    mnt->fatfs = fs;
    mnt->vfs_mounted = true;
    return OLNK_OK;
#else
    (void)mnt;
    (void)client;
    (void)cfg;
    return OLNK_ERR_PROTOCOL;
#endif
}

olnk_status_t olnk_esp32_vfs_unmount(olnk_esp32_vfs_mount_t *mnt) {
    if (mnt == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

#ifdef ESP_PLATFORM
    if (mnt->vfs_mounted) {
        (void)f_mount(NULL, mnt->fat_drv_str, 0);
        (void)esp_vfs_fat_unregister_path(mnt->base_path);
        if (mnt->pdrv < (uint8_t)FF_VOLUMES) {
            ff_diskio_unregister((BYTE)mnt->pdrv);
            if (s_vfs_mounts[mnt->pdrv] == mnt) {
                s_vfs_mounts[mnt->pdrv] = NULL;
            }
        }
    }
#endif

    mnt->vfs_mounted = false;
    mnt->disk_ready = false;
    mnt->pdrv = OLNK_ESP32_VFS_PDRV_NONE;
    mnt->fatfs = NULL;
    mnt->client = NULL;
    return OLNK_OK;
}
