/* esp32/src/esp32_hal_sd.c
 *
 * Zero-allocation ESP32 Raw SD-Card Block Storage HAL implementing
 * olnk_storage_ops_t for MS-DOS FAT12/FAT16 volumes.
 *
 * Supports:
 *   - Caller-supplied sdmmc_card_t *
 *   - Convenience SDMMC 1-bit / 4-bit init (guarded by SOC_SDMMC_HOST_SUPPORTED)
 *   - Convenience SDSPI init (ESP32 / S2 / S3 / C3 / C6)
 *   - Injectable block I/O callbacks (olnk_esp32_sd_block_ops_t) for host CTest
 *   - Superfloppy FAT12/FAT16 VBR vs. MBR partition auto-detection or explicit
 *     MBR partition index (0..3)
 *   - Write-verify read-back comparison using a static 512-byte scratch buffer
 *   - Write-protect (0x8100), out-of-range LBA (0x8108), card absent (0x8102),
 *     card swap/re-insert MEDIA_CHANGED (0xFF once), Generic IOCTL 08h/46h Set MID
 *
 * Strictly ISO C99, zero dynamic allocation, zero floating-point.
 */
#include "openlnk_esp32.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "soc/soc_caps.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_host.h"
#endif
#endif

/* =========================================================================
 * Low-Level Hardware / Mock Block & GPIO Dispatch
 * ========================================================================= */

static bool sd_hw_is_present(const olnk_esp32_sd_drive_t *drv) {
    if ((drv == NULL) || (!drv->initialized)) {
        return false;
    }
    if (drv->host_type == OLNK_ESP32_SD_HOST_CUSTOM) {
        if (drv->custom_ops.is_present != NULL) {
            return drv->custom_ops.is_present(drv->custom_ops.ctx);
        }
        return true;
    }
#ifdef ESP_PLATFORM
    if (drv->opts.cd_gpio >= 0) {
        int level = gpio_get_level((gpio_num_t)drv->opts.cd_gpio);
        bool inserted = drv->opts.cd_active_low ? (level == 0) : (level != 0);
        if (!inserted) {
            return false;
        }
    }
    if (drv->card == NULL) {
        return false;
    }
    return sdmmc_get_status(drv->card) == ESP_OK;
#else
    return false;
#endif
}

static bool sd_hw_is_write_protected(const olnk_esp32_sd_drive_t *drv) {
    if (drv == NULL) {
        return true;
    }
    if (drv->opts.write_protected) {
        return true;
    }
#ifdef ESP_PLATFORM
    if ((drv->host_type != OLNK_ESP32_SD_HOST_CUSTOM) && (drv->opts.wp_gpio >= 0)) {
        int level = gpio_get_level((gpio_num_t)drv->opts.wp_gpio);
        return drv->opts.wp_active_high ? (level != 0) : (level == 0);
    }
#endif
    return false;
}

static bool sd_hw_read_blocks(
    olnk_esp32_sd_drive_t *drv,
    uint8_t *dst,
    uint32_t start_block,
    uint32_t block_count
) {
    if ((drv == NULL) || (!drv->initialized) || (dst == NULL) || (block_count == 0UL)) {
        return false;
    }
    if (drv->host_type == OLNK_ESP32_SD_HOST_CUSTOM) {
        if (drv->custom_ops.read_blocks == NULL) {
            return false;
        }
        return drv->custom_ops.read_blocks(dst, start_block, block_count, drv->custom_ops.ctx);
    }
#ifdef ESP_PLATFORM
    if (drv->card == NULL) {
        return false;
    }
    return sdmmc_read_sectors(
               drv->card,
               dst,
               (size_t)start_block,
               (size_t)block_count) == ESP_OK;
#else
    (void)start_block;
    return false;
#endif
}

static bool sd_hw_write_blocks(
    olnk_esp32_sd_drive_t *drv,
    const uint8_t *src,
    uint32_t start_block,
    uint32_t block_count
) {
    if ((drv == NULL) || (!drv->initialized) || (src == NULL) || (block_count == 0UL)) {
        return false;
    }
    if (drv->host_type == OLNK_ESP32_SD_HOST_CUSTOM) {
        if (drv->custom_ops.write_blocks == NULL) {
            return false;
        }
        return drv->custom_ops.write_blocks(src, start_block, block_count, drv->custom_ops.ctx);
    }
#ifdef ESP_PLATFORM
    if (drv->card == NULL) {
        return false;
    }
    return sdmmc_write_sectors(
               drv->card,
               src,
               (size_t)start_block,
               (size_t)block_count) == ESP_OK;
#else
    (void)start_block;
    return false;
#endif
}

#ifdef ESP_PLATFORM
static void sd_configure_input_gpio(int pin) {
    gpio_config_t io_conf;
    if (pin < 0) {
        return;
    }
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.pin_bit_mask = (1ULL << (uint32_t)pin);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    (void)gpio_config(&io_conf);
}
#endif

/* =========================================================================
 * MBR / Superfloppy FAT12/FAT16 VBR Probing
 * ========================================================================= */

static bool is_fat12_or_fat16_part_type(uint8_t ptype) {
    return (ptype == 0x01U) || (ptype == 0x04U) || (ptype == 0x06U) || (ptype == 0x0EU);
}

static bool find_explicit_mbr_partition(
    const uint8_t sec0[OLNK_SECTOR_SIZE],
    int8_t part_idx,
    uint32_t *out_start_lba,
    uint32_t *out_num_sectors
) {
    olnk_drive_info_t probe;
    const uint8_t *ent;
    uint8_t ptype;
    uint32_t start_lba;
    uint32_t num_sec;

    if ((part_idx < 0) || (part_idx >= 4)) {
        return false;
    }
    if ((sec0[510] != 0x55U) || (sec0[511] != 0xAAU)) {
        return false;
    }
    /* Superfloppy VBR at sector 0 has no MBR partition table */
    if (olnk_fat_parse_vbr(sec0, &probe)) {
        return false;
    }

    ent = &sec0[0x1BEU + ((size_t)(uint8_t)part_idx * 16U)];
    ptype = ent[4];
    start_lba = olnk_read_u32_le(&ent[8]);
    num_sec = olnk_read_u32_le(&ent[12]);

    if ((!is_fat12_or_fat16_part_type(ptype)) || (start_lba == 0UL) || (num_sec == 0UL)) {
        return false;
    }

    *out_start_lba = start_lba;
    *out_num_sectors = num_sec;
    return true;
}

static uint16_t sd_drive_refresh_vbr(olnk_esp32_sd_drive_t *drv) {
    uint32_t part_lba = 0UL;
    uint32_t part_sec = 0UL;
    uint32_t end_part;
    olnk_drive_info_t parsed;

    if ((drv == NULL) || (!drv->initialized)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }

    if (!sd_hw_is_present(drv)) {
        drv->was_absent = true;
        drv->info.present = true;
        drv->info.ready = false;
        drv->info.drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        drv->info.write_protected = sd_hw_is_write_protected(drv);
        drv->info.removable = true;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }

    if (drv->raw_sectors == 0UL) {
        drv->info.present = true;
        drv->info.ready = false;
        drv->info.drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        drv->info.write_protected = sd_hw_is_write_protected(drv);
        drv->info.removable = true;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }

    /* Read physical sector 0 (MBR or superfloppy VBR) into static scratch */
    if (!sd_hw_read_blocks(drv, drv->scratch, 0UL, 1UL)) {
        drv->was_absent = true;
        drv->info.present = true;
        drv->info.ready = false;
        drv->info.drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        drv->info.write_protected = sd_hw_is_write_protected(drv);
        drv->info.removable = true;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }

    drv->partition_lba = 0UL;
    drv->volume_sectors = drv->raw_sectors;

    if (drv->opts.partition_index >= 0) {
        if (!find_explicit_mbr_partition(drv->scratch, drv->opts.partition_index, &part_lba, &part_sec)) {
            drv->info.present = true;
            drv->info.ready = false;
            drv->info.drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
            drv->info.write_protected = sd_hw_is_write_protected(drv);
            drv->info.removable = true;
            return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_MEDIA);
        }
        end_part = part_lba + part_sec;
        if ((end_part < part_lba) || (end_part > drv->raw_sectors)) {
            drv->info.present = true;
            drv->info.ready = false;
            return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_MEDIA);
        }
        if (!sd_hw_read_blocks(drv, drv->scratch, part_lba, 1UL)) {
            return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
        }
        drv->partition_lba = part_lba;
        drv->volume_sectors = part_sec;
    } else if (olnk_mbr_find_fat_partition(drv->scratch, &part_lba, &part_sec)) {
        end_part = part_lba + part_sec;
        if ((end_part >= part_lba) && (end_part <= drv->raw_sectors)) {
            if (!sd_hw_read_blocks(drv, drv->scratch, part_lba, 1UL)) {
                return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
            }
            drv->partition_lba = part_lba;
            drv->volume_sectors = part_sec;
        }
    }

    if (!olnk_fat_parse_vbr(drv->scratch, &parsed)) {
        drv->info.present = true;
        drv->info.ready = false;
        drv->info.drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        drv->info.write_protected = sd_hw_is_write_protected(drv);
        drv->info.removable = true;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_MEDIA);
    }

    if (parsed.bpb.bytes_per_sector != OLNK_SECTOR_SIZE) {
        drv->info.present = true;
        drv->info.ready = false;
        drv->info.drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        drv->info.write_protected = sd_hw_is_write_protected(drv);
        drv->info.removable = true;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_MEDIA);
    }

    if (parsed.total_sectors > drv->volume_sectors) {
        parsed.total_sectors = drv->volume_sectors;
    } else if (parsed.total_sectors > 0UL) {
        drv->volume_sectors = parsed.total_sectors;
    }

    parsed.write_protected = sd_hw_is_write_protected(drv);
    parsed.removable = true;
    drv->info = parsed;
    return OLNK_DOS_STATUS_DONE;
}

static void sd_record_vbr_identity(olnk_esp32_sd_drive_t *drv) {
    drv->last_partition_lba = drv->partition_lba;
    drv->last_volume_serial = drv->info.volume_serial;
    memcpy(drv->last_volume_label, drv->info.volume_label, sizeof(drv->last_volume_label));
    drv->has_last_vbr = true;
}

/* =========================================================================
 * Public Initialization & Deinitialization API
 * ========================================================================= */

void olnk_esp32_sd_opts_default(olnk_esp32_sd_opts_t *opts) {
    if (opts == NULL) {
        return;
    }
    memset(opts, 0, sizeof(*opts));
    opts->partition_index = (int8_t)OLNK_ESP32_PART_AUTO;
    opts->write_protected = false;
    opts->cd_gpio = OLNK_ESP32_PIN_UNUSED;
    opts->cd_active_low = true;
    opts->wp_gpio = OLNK_ESP32_PIN_UNUSED;
    opts->wp_active_high = true;
}

void olnk_esp32_sdmmc_cfg_default(olnk_esp32_sdmmc_cfg_t *cfg) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->slot = 1; /* SDMMC_HOST_SLOT_1 */
    cfg->bus_width = 1U;
    cfg->max_freq_khz = 0;
    cfg->clk_pin = OLNK_ESP32_PIN_UNUSED;
    cfg->cmd_pin = OLNK_ESP32_PIN_UNUSED;
    cfg->d0_pin = OLNK_ESP32_PIN_UNUSED;
    cfg->d1_pin = OLNK_ESP32_PIN_UNUSED;
    cfg->d2_pin = OLNK_ESP32_PIN_UNUSED;
    cfg->d3_pin = OLNK_ESP32_PIN_UNUSED;
    cfg->internal_pullups = true;
}

void olnk_esp32_sdspi_cfg_default(
    olnk_esp32_sdspi_cfg_t *cfg,
    int mosi_pin,
    int miso_pin,
    int clk_pin,
    int cs_pin
) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->spi_host = 0; /* Defaults to SPI2_HOST */
    cfg->mosi_pin = mosi_pin;
    cfg->miso_pin = miso_pin;
    cfg->clk_pin = clk_pin;
    cfg->cs_pin = cs_pin;
    cfg->max_freq_khz = 0;
    cfg->skip_bus_init = false;
}

olnk_status_t olnk_esp32_sd_init_custom(
    olnk_esp32_sd_drive_t *drv,
    const olnk_esp32_sd_block_ops_t *block_ops,
    const olnk_esp32_sd_opts_t *opts
) {
    uint16_t st;

    if ((drv == NULL) || (block_ops == NULL) || (block_ops->read_blocks == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    memset(drv, 0, sizeof(*drv));
    olnk_esp32_sd_opts_default(&drv->opts);
    if (opts != NULL) {
        drv->opts = *opts;
    }
    drv->host_type = OLNK_ESP32_SD_HOST_CUSTOM;
    drv->custom_ops = *block_ops;
    drv->raw_sectors = block_ops->total_blocks;
    drv->initialized = true;

    st = sd_drive_refresh_vbr(drv);
    if ((st & OLNK_DOS_STATUS_ERROR) == 0U) {
        sd_record_vbr_identity(drv);
    }
    return OLNK_OK;
}

#ifdef ESP_PLATFORM
olnk_status_t olnk_esp32_sd_attach_card(
    olnk_esp32_sd_drive_t *drv,
    sdmmc_card_t *card,
    const olnk_esp32_sd_opts_t *opts
) {
    uint16_t st;

    if ((drv == NULL) || (card == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }
    if ((card->csd.sector_size != OLNK_SECTOR_SIZE) || (card->csd.capacity <= 0)) {
        return OLNK_ERR_INVALID_ARG;
    }

    memset(drv, 0, sizeof(*drv));
    olnk_esp32_sd_opts_default(&drv->opts);
    if (opts != NULL) {
        drv->opts = *opts;
    }

    sd_configure_input_gpio(drv->opts.cd_gpio);
    sd_configure_input_gpio(drv->opts.wp_gpio);

    drv->host_type = OLNK_ESP32_SD_HOST_EXT;
    drv->card = card;
    drv->raw_sectors = (uint32_t)card->csd.capacity;
    drv->initialized = true;

    st = sd_drive_refresh_vbr(drv);
    if ((st & OLNK_DOS_STATUS_ERROR) == 0U) {
        sd_record_vbr_identity(drv);
    }
    return OLNK_OK;
}

olnk_status_t olnk_esp32_sd_init_sdmmc(
    olnk_esp32_sd_drive_t *drv,
    const olnk_esp32_sdmmc_cfg_t *sdmmc_cfg,
    const olnk_esp32_sd_opts_t *opts
) {
#if SOC_SDMMC_HOST_SUPPORTED
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    uint16_t st;

    if (drv == NULL) {
        return OLNK_ERR_INVALID_ARG;
    }

    memset(drv, 0, sizeof(*drv));
    olnk_esp32_sd_opts_default(&drv->opts);
    if (opts != NULL) {
        drv->opts = *opts;
    }

    sd_configure_input_gpio(drv->opts.cd_gpio);
    sd_configure_input_gpio(drv->opts.wp_gpio);

    if (sdmmc_cfg != NULL) {
        host.slot = sdmmc_cfg->slot;
        if (sdmmc_cfg->max_freq_khz > 0) {
            host.max_freq_khz = sdmmc_cfg->max_freq_khz;
        }
        slot_cfg.width = (sdmmc_cfg->bus_width == 4U) ? 4U : 1U;
        if (sdmmc_cfg->internal_pullups) {
            slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        }
#if SOC_SDMMC_USE_GPIO_MATRIX
        if (sdmmc_cfg->clk_pin >= 0) {
            slot_cfg.clk = (gpio_num_t)sdmmc_cfg->clk_pin;
        }
        if (sdmmc_cfg->cmd_pin >= 0) {
            slot_cfg.cmd = (gpio_num_t)sdmmc_cfg->cmd_pin;
        }
        if (sdmmc_cfg->d0_pin >= 0) {
            slot_cfg.d0 = (gpio_num_t)sdmmc_cfg->d0_pin;
        }
        if (slot_cfg.width == 4U) {
            if (sdmmc_cfg->d1_pin >= 0) {
                slot_cfg.d1 = (gpio_num_t)sdmmc_cfg->d1_pin;
            }
            if (sdmmc_cfg->d2_pin >= 0) {
                slot_cfg.d2 = (gpio_num_t)sdmmc_cfg->d2_pin;
            }
            if (sdmmc_cfg->d3_pin >= 0) {
                slot_cfg.d3 = (gpio_num_t)sdmmc_cfg->d3_pin;
            }
        }
#endif
    } else {
        slot_cfg.width = 1U;
        slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    }

    if (sdmmc_host_init() != ESP_OK) {
        return OLNK_ERR_HAL_IO;
    }
    if (sdmmc_host_init_slot(host.slot, &slot_cfg) != ESP_OK) {
        (void)sdmmc_host_deinit();
        return OLNK_ERR_HAL_IO;
    }
    if (sdmmc_card_init(&host, &drv->owned_card) != ESP_OK) {
        (void)sdmmc_host_deinit();
        return OLNK_ERR_HAL_IO;
    }

    drv->host_type = OLNK_ESP32_SD_HOST_SDMMC;
    drv->card = &drv->owned_card;
    drv->raw_sectors = (drv->owned_card.csd.capacity > 0)
                           ? (uint32_t)drv->owned_card.csd.capacity
                           : 0UL;
    drv->initialized = true;

    st = sd_drive_refresh_vbr(drv);
    if ((st & OLNK_DOS_STATUS_ERROR) == 0U) {
        sd_record_vbr_identity(drv);
    }
    return OLNK_OK;
#else
    (void)drv;
    (void)sdmmc_cfg;
    (void)opts;
    return OLNK_ERR_PROTOCOL;
#endif
}

olnk_status_t olnk_esp32_sd_init_sdspi(
    olnk_esp32_sd_drive_t *drv,
    const olnk_esp32_sdspi_cfg_t *sdspi_cfg,
    const olnk_esp32_sd_opts_t *opts
) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    spi_host_device_t spi_host;
    sdspi_dev_handle_t dev_handle = -1;
    bool owns_bus = false;
    uint16_t st;

    if ((drv == NULL) || (sdspi_cfg == NULL)) {
        return OLNK_ERR_INVALID_ARG;
    }

    memset(drv, 0, sizeof(*drv));
    olnk_esp32_sd_opts_default(&drv->opts);
    if (opts != NULL) {
        drv->opts = *opts;
    }

    sd_configure_input_gpio(drv->opts.cd_gpio);
    sd_configure_input_gpio(drv->opts.wp_gpio);

    spi_host = (sdspi_cfg->spi_host > 0)
                   ? (spi_host_device_t)sdspi_cfg->spi_host
                   : SPI2_HOST;

    if (!sdspi_cfg->skip_bus_init) {
        spi_bus_config_t bus_cfg;
        memset(&bus_cfg, 0, sizeof(bus_cfg));
        bus_cfg.mosi_io_num = sdspi_cfg->mosi_pin;
        bus_cfg.miso_io_num = sdspi_cfg->miso_pin;
        bus_cfg.sclk_io_num = sdspi_cfg->clk_pin;
        bus_cfg.quadwp_io_num = -1;
        bus_cfg.quadhd_io_num = -1;
        bus_cfg.max_transfer_sz = 4096;

        if (spi_bus_initialize(spi_host, &bus_cfg, SDSPI_DEFAULT_DMA) != ESP_OK) {
            return OLNK_ERR_HAL_IO;
        }
        owns_bus = true;
    }

    if (sdspi_host_init() != ESP_OK) {
        if (owns_bus) {
            (void)spi_bus_free(spi_host);
        }
        return OLNK_ERR_HAL_IO;
    }

    host.slot = (int)spi_host;
    if (sdspi_cfg->max_freq_khz > 0) {
        host.max_freq_khz = sdspi_cfg->max_freq_khz;
    }

    slot_cfg.gpio_cs = (gpio_num_t)sdspi_cfg->cs_pin;
    slot_cfg.host_id = spi_host;

    if (sdspi_host_init_device(&slot_cfg, &dev_handle) != ESP_OK) {
        (void)sdspi_host_deinit();
        if (owns_bus) {
            (void)spi_bus_free(spi_host);
        }
        return OLNK_ERR_HAL_IO;
    }

    host.slot = (int)dev_handle;
    if (sdmmc_card_init(&host, &drv->owned_card) != ESP_OK) {
        (void)sdspi_host_remove_device(dev_handle);
        (void)sdspi_host_deinit();
        if (owns_bus) {
            (void)spi_bus_free(spi_host);
        }
        return OLNK_ERR_HAL_IO;
    }

    drv->host_type = OLNK_ESP32_SD_HOST_SDSPI;
    drv->card = &drv->owned_card;
    drv->sdspi_dev_handle = (int)dev_handle;
    drv->spi_host_id = (int)spi_host;
    drv->owns_spi_bus = owns_bus;
    drv->raw_sectors = (drv->owned_card.csd.capacity > 0)
                           ? (uint32_t)drv->owned_card.csd.capacity
                           : 0UL;
    drv->initialized = true;

    st = sd_drive_refresh_vbr(drv);
    if ((st & OLNK_DOS_STATUS_ERROR) == 0U) {
        sd_record_vbr_identity(drv);
    }
    return OLNK_OK;
}
#endif /* ESP_PLATFORM */

void olnk_esp32_sd_deinit(olnk_esp32_sd_drive_t *drv) {
    if ((drv == NULL) || (!drv->initialized)) {
        return;
    }
#ifdef ESP_PLATFORM
#if SOC_SDMMC_HOST_SUPPORTED
    if (drv->host_type == OLNK_ESP32_SD_HOST_SDMMC) {
        (void)sdmmc_host_deinit();
    }
#endif
    if (drv->host_type == OLNK_ESP32_SD_HOST_SDSPI) {
        if (drv->sdspi_dev_handle >= 0) {
            (void)sdspi_host_remove_device((sdspi_dev_handle_t)drv->sdspi_dev_handle);
        }
        (void)sdspi_host_deinit();
        if (drv->owns_spi_bus) {
            (void)spi_bus_free((spi_host_device_t)drv->spi_host_id);
        }
    }
#endif
    drv->card = NULL;
    drv->initialized = false;
    drv->host_type = OLNK_ESP32_SD_HOST_NONE;
}

void olnk_esp32_sd_notify_media_changed(olnk_esp32_sd_drive_t *drv) {
    if (drv == NULL) {
        return;
    }
    drv->media_changed_pending = true;
}

/* =========================================================================
 * olnk_storage_ops_t Core Drive Handlers
 * ========================================================================= */

static uint16_t sd_drv_get_info(olnk_esp32_sd_drive_t *drv, olnk_drive_info_t *out_info) {
    if (out_info == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }
    memset(out_info, 0, sizeof(*out_info));
    if ((drv == NULL) || (!drv->initialized)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }

    if (!sd_hw_is_present(drv)) {
        drv->was_absent = true;
        drv->info.present = true;
        drv->info.ready = false;
        drv->info.drive_attrs = OLNK_DRIVE_ATTR_DEFAULT;
        drv->info.write_protected = sd_hw_is_write_protected(drv);
        drv->info.removable = true;
    } else {
        if (!drv->info.ready) {
            (void)sd_drive_refresh_vbr(drv);
        }
        drv->info.write_protected = sd_hw_is_write_protected(drv);
    }

    *out_info = drv->info;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t sd_drv_media_check(
    olnk_esp32_sd_drive_t *drv,
    uint8_t media_descriptor,
    uint8_t *out_media_status,
    olnk_drive_info_t *out_info
) {
    uint16_t st;
    bool changed;
    (void)media_descriptor;

    if ((out_media_status == NULL) || (out_info == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }
    memset(out_info, 0, sizeof(*out_info));
    if ((drv == NULL) || (!drv->initialized)) {
        *out_media_status = (uint8_t)OLNK_MEDIA_UNSURE;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }

    if (!sd_hw_is_present(drv)) {
        drv->was_absent = true;
        drv->info.present = true;
        drv->info.ready = false;
        drv->info.write_protected = sd_hw_is_write_protected(drv);
        *out_info = drv->info;
        *out_media_status = (uint8_t)OLNK_MEDIA_UNSURE;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }

    st = sd_drive_refresh_vbr(drv);
    *out_info = drv->info;
    if ((st & OLNK_DOS_STATUS_ERROR) != 0U) {
        *out_media_status = (uint8_t)OLNK_MEDIA_UNSURE;
        return st;
    }

    changed = drv->was_absent || drv->media_changed_pending;
    if (!changed && drv->has_last_vbr) {
        if ((drv->partition_lba != drv->last_partition_lba) ||
            (drv->info.volume_serial != drv->last_volume_serial) ||
            (memcmp(drv->info.volume_label, drv->last_volume_label, 11U) != 0)) {
            changed = true;
        }
    }

    drv->was_absent = false;
    drv->media_changed_pending = false;
    sd_record_vbr_identity(drv);

    *out_media_status = changed
                            ? (uint8_t)OLNK_MEDIA_CHANGED
                            : (uint8_t)OLNK_MEDIA_UNCHANGED;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t sd_drv_build_bpb(
    olnk_esp32_sd_drive_t *drv,
    uint8_t media_descriptor,
    olnk_dos_bpb_t *out_bpb
) {
    uint16_t st;
    (void)media_descriptor;

    if (out_bpb == NULL) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_GENERAL_FAILURE);
    }
    if ((drv == NULL) || (!drv->initialized)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }

    st = sd_drive_refresh_vbr(drv);
    if ((st & OLNK_DOS_STATUS_ERROR) != 0U) {
        return st;
    }

    sd_record_vbr_identity(drv);
    *out_bpb = drv->info.bpb;
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t sd_drv_read_sectors(
    olnk_esp32_sd_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    uint8_t *out_buf,
    uint16_t *out_transferred
) {
    uint32_t end_lba;
    uint32_t phys_lba;
    uint32_t phys_end;

    if (out_transferred != NULL) {
        *out_transferred = 0U;
    }
    if ((drv == NULL) || (!drv->initialized)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (!sd_hw_is_present(drv)) {
        drv->was_absent = true;
        drv->info.ready = false;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if (!drv->info.ready) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if ((out_buf == NULL) || (sector_count == 0U)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
    }

    end_lba = start_lba + (uint32_t)sector_count;
    if ((end_lba < start_lba) || (end_lba > drv->volume_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }

    phys_lba = drv->partition_lba + start_lba;
    phys_end = phys_lba + (uint32_t)sector_count;
    if ((phys_lba < drv->partition_lba) || (phys_end < phys_lba) || (phys_end > drv->raw_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }

    if (!sd_hw_read_blocks(drv, out_buf, phys_lba, (uint32_t)sector_count)) {
        if (!sd_hw_is_present(drv)) {
            drv->was_absent = true;
            drv->info.ready = false;
            return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
        }
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
    }

    if (out_transferred != NULL) {
        *out_transferred = sector_count;
    }
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t sd_drv_write_sectors(
    olnk_esp32_sd_drive_t *drv,
    uint32_t start_lba,
    uint16_t sector_count,
    const uint8_t *in_buf,
    uint16_t *out_transferred,
    bool verify
) {
    uint32_t end_lba;
    uint32_t phys_lba;
    uint32_t phys_end;

    if (out_transferred != NULL) {
        *out_transferred = 0U;
    }
    if ((drv == NULL) || (!drv->initialized)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (!sd_hw_is_present(drv)) {
        drv->was_absent = true;
        drv->info.ready = false;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if (!drv->info.ready) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if (sd_hw_is_write_protected(drv)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
    }
    if ((in_buf == NULL) || (sector_count == 0U)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
    }

    end_lba = start_lba + (uint32_t)sector_count;
    if ((end_lba < start_lba) || (end_lba > drv->volume_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }

    phys_lba = drv->partition_lba + start_lba;
    phys_end = phys_lba + (uint32_t)sector_count;
    if ((phys_lba < drv->partition_lba) || (phys_end < phys_lba) || (phys_end > drv->raw_sectors)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_SECTOR_NOT_FOUND);
    }

    if (!sd_hw_write_blocks(drv, in_buf, phys_lba, (uint32_t)sector_count)) {
        if (!sd_hw_is_present(drv)) {
            drv->was_absent = true;
            drv->info.ready = false;
            return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
        }
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
    }

    if (verify) {
        uint16_t i;
        for (i = 0U; i < sector_count; ++i) {
            const uint8_t *expected = &in_buf[(size_t)i * OLNK_SECTOR_SIZE];
            if (!sd_hw_read_blocks(drv, drv->scratch, phys_lba + (uint32_t)i, 1UL)) {
                return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
            }
            if (memcmp(drv->scratch, expected, OLNK_SECTOR_SIZE) != 0) {
                return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
            }
        }
    }

    if (start_lba == 0UL) {
        if ((sd_drive_refresh_vbr(drv) & OLNK_DOS_STATUS_ERROR) == 0U) {
            sd_record_vbr_identity(drv);
        }
    }

    if (out_transferred != NULL) {
        *out_transferred = sector_count;
    }
    return OLNK_DOS_STATUS_DONE;
}

static uint16_t sd_drv_set_media_id(
    olnk_esp32_sd_drive_t *drv,
    const olnk_generic_ioctl_mid_t *mid
) {
    if ((drv == NULL) || (!drv->initialized) || (mid == NULL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_UNKNOWN_UNIT);
    }
    if (!sd_hw_is_present(drv)) {
        drv->was_absent = true;
        drv->info.ready = false;
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if (!drv->info.ready) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_DRIVE_NOT_READY);
    }
    if (sd_hw_is_write_protected(drv)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_PROTECT);
    }

    if (!sd_hw_read_blocks(drv, drv->scratch, drv->partition_lba, 1UL)) {
        return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_READ_FAULT);
    }

    if (drv->scratch[38] == 0x29U) {
        olnk_write_u32_le(&drv->scratch[39], mid->serial_number);
        memcpy(&drv->scratch[43], mid->volume_label, 11U);
        memcpy(&drv->scratch[54], mid->filesystem_type, 8U);
        if (!sd_hw_write_blocks(drv, drv->scratch, drv->partition_lba, 1UL)) {
            return OLNK_DOS_MAKE_ERROR(OLNK_DOS_ERR_WRITE_FAULT);
        }
    }

    drv->info.volume_serial = mid->serial_number;
    memcpy(drv->info.volume_label, mid->volume_label, 11U);
    drv->info.volume_label[11] = '\0';
    memcpy(drv->info.filesystem_type, mid->filesystem_type, 8U);
    drv->info.filesystem_type[8] = '\0';
    sd_record_vbr_identity(drv);
    return OLNK_DOS_STATUS_DONE;
}

/* =========================================================================
 * Single-Drive & Multi-Drive Set Vtable Wrappers
 * ========================================================================= */

static olnk_esp32_sd_drive_t *resolve_sd_single(uint8_t unit, void *ud) {
    if (unit != 0U) {
        return NULL;
    }
    return (olnk_esp32_sd_drive_t *)ud;
}

static olnk_esp32_sd_drive_t *resolve_sd_set(uint8_t unit, void *ud) {
    olnk_esp32_drive_set_t *set = (olnk_esp32_drive_set_t *)ud;
    if ((set == NULL) || (unit >= OLNK_MAX_DRIVES)) {
        return NULL;
    }
    return set->drives[unit];
}

static uint8_t sd_single_drive_count(void *ud) {
    return (ud != NULL) ? 1U : 0U;
}

static uint16_t sd_single_get_info(uint8_t unit, olnk_drive_info_t *out, void *ud) {
    return sd_drv_get_info(resolve_sd_single(unit, ud), out);
}

static uint16_t sd_single_media_check(
    uint8_t unit,
    uint8_t md,
    uint8_t *ms,
    olnk_drive_info_t *info,
    void *ud
) {
    return sd_drv_media_check(resolve_sd_single(unit, ud), md, ms, info);
}

static uint16_t sd_single_build_bpb(uint8_t unit, uint8_t md, olnk_dos_bpb_t *bpb, void *ud) {
    return sd_drv_build_bpb(resolve_sd_single(unit, ud), md, bpb);
}

static uint16_t sd_single_read(
    uint8_t unit,
    uint32_t lba,
    uint16_t cnt,
    uint8_t *buf,
    uint16_t *xfer,
    void *ud
) {
    return sd_drv_read_sectors(resolve_sd_single(unit, ud), lba, cnt, buf, xfer);
}

static uint16_t sd_single_write(
    uint8_t unit,
    uint32_t lba,
    uint16_t cnt,
    const uint8_t *buf,
    uint16_t *xfer,
    bool ver,
    void *ud
) {
    return sd_drv_write_sectors(resolve_sd_single(unit, ud), lba, cnt, buf, xfer, ver);
}

static uint16_t sd_single_set_mid(
    uint8_t unit,
    const olnk_generic_ioctl_mid_t *mid,
    void *ud
) {
    return sd_drv_set_media_id(resolve_sd_single(unit, ud), mid);
}

olnk_storage_ops_t olnk_esp32_sd_drive_ops(olnk_esp32_sd_drive_t *drv) {
    olnk_storage_ops_t ops;
    ops.drive_count = sd_single_drive_count;
    ops.get_drive_info = sd_single_get_info;
    ops.media_check = sd_single_media_check;
    ops.build_bpb = sd_single_build_bpb;
    ops.read_sectors = sd_single_read;
    ops.write_sectors = sd_single_write;
    ops.set_media_id = sd_single_set_mid;
    ops.user_data = drv;
    return ops;
}

void olnk_esp32_drive_set_init(olnk_esp32_drive_set_t *set) {
    if (set == NULL) {
        return;
    }
    memset(set, 0, sizeof(*set));
}

bool olnk_esp32_drive_set_attach(
    olnk_esp32_drive_set_t *set,
    uint8_t unit,
    olnk_esp32_sd_drive_t *drv
) {
    if ((set == NULL) || (drv == NULL) || (unit >= OLNK_MAX_DRIVES)) {
        return false;
    }
    set->drives[unit] = drv;
    if ((uint8_t)(unit + 1U) > set->drive_count) {
        set->drive_count = (uint8_t)(unit + 1U);
    }
    return true;
}

static uint8_t sd_set_drive_count(void *ud) {
    const olnk_esp32_drive_set_t *set = (const olnk_esp32_drive_set_t *)ud;
    return (set != NULL) ? set->drive_count : 0U;
}

static uint16_t sd_set_get_info(uint8_t unit, olnk_drive_info_t *out, void *ud) {
    return sd_drv_get_info(resolve_sd_set(unit, ud), out);
}

static uint16_t sd_set_media_check(
    uint8_t unit,
    uint8_t md,
    uint8_t *ms,
    olnk_drive_info_t *info,
    void *ud
) {
    return sd_drv_media_check(resolve_sd_set(unit, ud), md, ms, info);
}

static uint16_t sd_set_build_bpb(uint8_t unit, uint8_t md, olnk_dos_bpb_t *bpb, void *ud) {
    return sd_drv_build_bpb(resolve_sd_set(unit, ud), md, bpb);
}

static uint16_t sd_set_read(
    uint8_t unit,
    uint32_t lba,
    uint16_t cnt,
    uint8_t *buf,
    uint16_t *xfer,
    void *ud
) {
    return sd_drv_read_sectors(resolve_sd_set(unit, ud), lba, cnt, buf, xfer);
}

static uint16_t sd_set_write(
    uint8_t unit,
    uint32_t lba,
    uint16_t cnt,
    const uint8_t *buf,
    uint16_t *xfer,
    bool ver,
    void *ud
) {
    return sd_drv_write_sectors(resolve_sd_set(unit, ud), lba, cnt, buf, xfer, ver);
}

static uint16_t sd_set_set_mid(
    uint8_t unit,
    const olnk_generic_ioctl_mid_t *mid,
    void *ud
) {
    return sd_drv_set_media_id(resolve_sd_set(unit, ud), mid);
}

olnk_storage_ops_t olnk_esp32_drive_set_ops(olnk_esp32_drive_set_t *set) {
    olnk_storage_ops_t ops;
    ops.drive_count = sd_set_drive_count;
    ops.get_drive_info = sd_set_get_info;
    ops.media_check = sd_set_media_check;
    ops.build_bpb = sd_set_build_bpb;
    ops.read_sectors = sd_set_read;
    ops.write_sectors = sd_set_write;
    ops.set_media_id = sd_set_set_mid;
    ops.user_data = set;
    return ops;
}
