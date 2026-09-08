/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/**
 * LittleFS-on-UBI example.
 *
 * Mounts joltwallet/littlefs directly on the esp_nand_ubi volume block device
 * (nand_ubi_get_blockdev()) -- no Dhara wear-leveling BDL anywhere in this
 * chain, and no filesystem adapter/shim to write: joltwallet/littlefs already
 * speaks esp_blockdev_t natively (esp_vfs_littlefs_conf_t.blockdev).
 *
 * Stack:
 *   SPI NAND hardware
 *     -> nand_flash_get_blockdev()        raw flash BDL         [spi_nand_flash]
 *     -> nand_ubi_get_blockdev()          UBI volume BDL        [esp_nand_ubi]
 *     -> esp_vfs_littlefs_register()      POSIX file API        [joltwallet/littlefs]
 *
 * See this example's README.md for the block_cycles=-1 decision (no UBI
 * wear-leveling exists yet, so there's nothing for LittleFS's own metadata
 * block-cycling to double up against) and the known LFS_ERR_CORRUPT mapping
 * gap in joltwallet's littlefs_bdl.c adapter (also documented in
 * esp_nand_ubi/README.md).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/spi_master.h"
#include "soc/spi_pins.h"

#include "spi_nand_flash.h"
#include "esp_nand_blockdev.h"
#include "esp_nand_ubi.h"
#include "esp_littlefs.h"

static const char *TAG = "example";

/* Pin mapping mirrors examples/nand_ubi_example: ESP32 uses VSPI (SPI3_HOST)
 * since HSPI/SPI2 is often reserved for PSRAM; every other target uses
 * SPI2_HOST. */
#ifdef CONFIG_IDF_TARGET_ESP32
#define HOST_ID  SPI3_HOST
#define PIN_MOSI SPI3_IOMUX_PIN_NUM_MOSI
#define PIN_MISO SPI3_IOMUX_PIN_NUM_MISO
#define PIN_CLK  SPI3_IOMUX_PIN_NUM_CLK
#define PIN_CS   SPI3_IOMUX_PIN_NUM_CS
#define PIN_WP   SPI3_IOMUX_PIN_NUM_WP
#define PIN_HD   SPI3_IOMUX_PIN_NUM_HD
#else
#define HOST_ID  SPI2_HOST
#define PIN_MOSI SPI2_IOMUX_PIN_NUM_MOSI
#define PIN_MISO SPI2_IOMUX_PIN_NUM_MISO
#define PIN_CLK  SPI2_IOMUX_PIN_NUM_CLK
#define PIN_CS   SPI2_IOMUX_PIN_NUM_CS
#define PIN_WP   SPI2_IOMUX_PIN_NUM_WP
#define PIN_HD   SPI2_IOMUX_PIN_NUM_HD
#endif
#define SPI_DMA_CHAN         SPI_DMA_CH_AUTO
#define EXAMPLE_FLASH_FREQ_HZ (40 * 1000 * 1000)

static const char *base_path = "/nand";

static esp_err_t init_spi_and_nand(spi_device_handle_t *out_spi, esp_blockdev_handle_t *out_nand_bdl)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
        .max_transfer_sz = 4096 * 2,
    };
    ESP_LOGI(TAG, "DMA CHANNEL: %d", SPI_DMA_CHAN);
    esp_err_t ret = spi_bus_initialize(HOST_ID, &bus_config, SPI_DMA_CHAN);
    if (ret != ESP_OK) {
        return ret;
    }

    /* SPI_DEVICE_HALFDUPLEX -> half duplex; 0 would mean full-duplex. */
    const uint32_t spi_flags = SPI_DEVICE_HALFDUPLEX;
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EXAMPLE_FLASH_FREQ_HZ,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 10,
        .flags = spi_flags,
    };
    spi_device_handle_t spi;
    ret = spi_bus_add_device(HOST_ID, &devcfg, &spi);
    if (ret != ESP_OK) {
        spi_bus_free(HOST_ID);
        return ret;
    }

    spi_nand_flash_config_t nand_flash_config = {
        .device_handle = spi,
        .io_mode = SPI_NAND_IO_MODE_SIO,
        .flags = spi_flags,
    };

    /* Raw flash BDL, NOT spi_nand_flash_wl_get_blockdev(). esp_nand_ubi is an
     * alternative FTL to the Dhara wear-leveling layer; stacking LittleFS on
     * Dhara AND UBI at once would be a double FTL with incompatible geometry
     * contracts -- pick one. This example demonstrates UBI. */
    esp_blockdev_handle_t nand_bdl = NULL;
    ret = nand_flash_get_blockdev(&nand_flash_config, &nand_bdl);
    if (ret != ESP_OK) {
        spi_bus_remove_device(spi);
        spi_bus_free(HOST_ID);
        return ret;
    }

    *out_spi = spi;
    *out_nand_bdl = nand_bdl;
    return ESP_OK;
}

static void deinit_spi_bus(spi_device_handle_t spi)
{
    ESP_ERROR_CHECK(spi_bus_remove_device(spi));
    ESP_ERROR_CHECK(spi_bus_free(HOST_ID));
}

void app_main(void)
{
    spi_device_handle_t spi;
    esp_blockdev_handle_t nand_bdl = NULL;
    ESP_ERROR_CHECK(init_spi_and_nand(&spi, &nand_bdl));

    ESP_LOGI(TAG, "Raw NAND: page_size=%" PRIu32 " peb_size=%" PRIu32 " disk_size=%" PRIu64,
             (uint32_t)nand_bdl->geometry.read_size, (uint32_t)nand_bdl->geometry.erase_size,
             nand_bdl->geometry.disk_size);

    ESP_LOGI(TAG, "Attaching NAND UBI volume 0 (raw BDL, not Dhara WL)");
    nand_ubi_config_t ubi_cfg = NAND_UBI_CONFIG_DEFAULT();
    esp_blockdev_handle_t vol_bdl = NULL;
    ESP_ERROR_CHECK(nand_ubi_get_blockdev(nand_bdl, &ubi_cfg, &vol_bdl));

    ESP_LOGI(TAG, "UBI volume ready: leb_size=%" PRIu64 " disk_size=%" PRIu64,
             vol_bdl->geometry.erase_size, vol_bdl->geometry.disk_size);

    ESP_LOGI(TAG, "Mounting LittleFS on the UBI volume BDL");
    esp_vfs_littlefs_conf_t conf = {
        .base_path = base_path,
        .partition_label = NULL,
        .partition = NULL,
        .blockdev = vol_bdl,
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };

    esp_err_t mount_ret = esp_vfs_littlefs_register(&conf);
    if (mount_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS on UBI volume (%s)", esp_err_to_name(mount_ret));
        /* vol_bdl->ops->release() also detaches the UBI device
         * (nand_ubi_get_blockdev() sets owns_device = true). */
        vol_bdl->ops->release(vol_bdl);
        nand_bdl->ops->release(nand_bdl);
        deinit_spi_bus(spi);
        return;
    }

    size_t total = 0, used = 0;
    ESP_ERROR_CHECK(esp_littlefs_blockdev_info(vol_bdl, &total, &used));
    ESP_LOGI(TAG, "LittleFS: %u kB total, %u kB used", (unsigned)(total / 1024), (unsigned)(used / 1024));

    ESP_LOGI(TAG, "Opening file");
    FILE *f = fopen("/nand/hello.txt", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        esp_vfs_littlefs_unregister_blockdev(vol_bdl);
        nand_bdl->ops->release(nand_bdl);
        deinit_spi_bus(spi);
        return;
    }
    fprintf(f, "Written using ESP-IDF %s over esp_nand_ubi\n", esp_get_idf_version());
    fclose(f);
    ESP_LOGI(TAG, "File written");

    /* Force a fresh read by closing and reopening rather than relying on
     * any in-RAM buffering: this is the round-trip the PoC is meant to
     * prove -- the bytes actually made it through UBI's LEB->PEB translation
     * and back. */
    ESP_LOGI(TAG, "Reading file back");
    f = fopen("/nand/hello.txt", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        esp_vfs_littlefs_unregister_blockdev(vol_bdl);
        nand_bdl->ops->release(nand_bdl);
        deinit_spi_bus(spi);
        return;
    }
    char line[128];
    if (fgets(line, sizeof(line), f) == NULL) {
        ESP_LOGE(TAG, "Failed to read file contents");
        fclose(f);
        esp_vfs_littlefs_unregister_blockdev(vol_bdl);
        nand_bdl->ops->release(nand_bdl);
        deinit_spi_bus(spi);
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    fclose(f);
    char *newline = strchr(line, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    ESP_LOGI(TAG, "Read from file: '%s'", line);

    ESP_ERROR_CHECK(esp_littlefs_blockdev_info(vol_bdl, &total, &used));
    ESP_LOGI(TAG, "LittleFS: %u kB total, %u kB used", (unsigned)(total / 1024), (unsigned)(used / 1024));

    /* Unregister unmounts LittleFS and releases vol_bdl via ops->release,
     * which also detaches the UBI device. nand_bdl is released separately:
     * esp_nand_ubi never owns the raw flash BDL it was attached to. */
    ESP_ERROR_CHECK(esp_vfs_littlefs_unregister_blockdev(vol_bdl));
    ESP_ERROR_CHECK(nand_bdl->ops->release(nand_bdl));
    deinit_spi_bus(spi);

    ESP_LOGI(TAG, "LittleFS-on-UBI example finished successfully");
}
