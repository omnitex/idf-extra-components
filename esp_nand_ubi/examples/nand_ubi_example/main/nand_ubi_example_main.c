/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/**
 * NAND UBI example.
 *
 * Demonstrates the esp_nand_ubi block-device layer directly on physical SPI NAND
 * flash: attach (scans/rebuilds the LEB->PEB table), erase, write, read-back
 * verification, and the logical-erase-returns-ESP_ERR_NOT_FOUND contract.
 *
 * This does NOT mount a filesystem. There is currently no FatFS/LittleFS adapter
 * for the esp_blockdev_t interface that esp_nand_ubi produces -- see this
 * example's README.md for details. Attaching and then writing IS the "format"
 * step: on a blank chip, nand_ubi_get_blockdev() already exposes the full
 * capacity, and the first write to any LEB lazily allocates a physical block and
 * writes fresh EC/VID headers on the fly (see esp_nand_ubi/src/nand_ubi.c).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "soc/spi_pins.h"

#include "spi_nand_flash.h"
#include "esp_nand_blockdev.h"
#include "esp_nand_ubi.h"

static const char *TAG = "example";

/* Pin mapping mirrors spi_nand_flash's own test_app and the spi_nand_flash_fatfs
 * example: ESP32 uses VSPI (SPI3_HOST) since HSPI/SPI2 is often reserved for
 * PSRAM; every other target uses SPI2_HOST. */
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

/* Number of logical erase blocks exercised by this demo. Kept small and fixed
 * so the example runs quickly and safely on the smallest supported chip. */
#define EXAMPLE_NUM_TEST_LEBS 4U

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
     * alternative FTL to the Dhara wear-leveling layer; stacking UBI on top of
     * Dhara would produce a double FTL with incompatible geometry contracts. */
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

/* Writes a per-LEB recognizable byte pattern and reads it back for verification.
 * One page per LEB is used (not the whole ~124 KB LEB) to keep the demo's RAM
 * footprint small and to stay within safe single-program-per-page semantics for
 * SPI NAND flash -- repeated partial reprogramming of the same page without an
 * erase in between is not guaranteed reliable on real hardware.
 *
 * @param leb_size  Erase-block stride in bytes (vol_bdl->geometry.erase_size) --
 *                  used only to compute this LEB's byte address.
 * @param page_size Read/write chunk size in bytes (vol_bdl->geometry.write_size) --
 *                  the amount of data actually written and verified. */
static void write_and_verify_leb(esp_blockdev_handle_t vol_bdl, uint32_t lnum, uint32_t leb_size, uint32_t page_size)
{
    uint8_t *pattern = malloc(page_size);
    uint8_t *readback = malloc(page_size);
    if (pattern == NULL || readback == NULL) {
        ESP_LOGE(TAG, "LEB %" PRIu32 ": out of memory for %" PRIu32 "-byte buffers", lnum, page_size);
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    memset(pattern, (uint8_t)(0xA0 + lnum), page_size);
    memset(readback, 0, page_size);

    uint64_t addr = (uint64_t)lnum * leb_size;

    ESP_ERROR_CHECK(vol_bdl->ops->write(vol_bdl, pattern, addr, page_size));
    ESP_ERROR_CHECK(vol_bdl->ops->read(vol_bdl, readback, page_size, addr, page_size));

    bool match = (memcmp(pattern, readback, page_size) == 0);
    free(pattern);
    free(readback);

    if (!match) {
        ESP_LOGE(TAG, "LEB %" PRIu32 ": write/read-back MISMATCH", lnum);
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    ESP_LOGI(TAG, "LEB %" PRIu32 ": write/read-back verified OK", lnum);
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

    uint32_t leb_size = (uint32_t)vol_bdl->geometry.erase_size;
    uint32_t page_size = (uint32_t)vol_bdl->geometry.write_size;
    uint32_t leb_count = (uint32_t)(vol_bdl->geometry.disk_size / leb_size);
    ESP_LOGI(TAG, "UBI volume ready: leb_count=%" PRIu32 " leb_size=%" PRIu32 " bytes", leb_count, leb_size);

    if (leb_count < EXAMPLE_NUM_TEST_LEBS) {
        ESP_LOGE(TAG, "Chip too small for this demo: need >= %u usable LEBs, got %" PRIu32,
                 EXAMPLE_NUM_TEST_LEBS, leb_count);
        ESP_ERROR_CHECK(ESP_ERR_INVALID_SIZE);
    }

    /* Erase before write is idempotent: a no-op for LEBs that are already
     * unmapped (factory-blank or from a previous run's final erase step
     * below), a real physical erase for LEBs left mapped by a previous run.
     * This makes the demo safely re-runnable without ever double-programming
     * an already-written physical page. */
    for (uint32_t lnum = 0; lnum < EXAMPLE_NUM_TEST_LEBS; lnum++) {
        ESP_ERROR_CHECK(vol_bdl->ops->erase(vol_bdl, (uint64_t)lnum * leb_size, leb_size));
    }

    for (uint32_t lnum = 0; lnum < EXAMPLE_NUM_TEST_LEBS; lnum++) {
        write_and_verify_leb(vol_bdl, lnum, leb_size, page_size);
    }

    ESP_LOGI(TAG, "Erasing LEB 0 again to demonstrate logical-erase semantics");
    ESP_ERROR_CHECK(vol_bdl->ops->erase(vol_bdl, 0, leb_size));

    uint8_t *post_erase_buf = malloc(page_size);
    if (post_erase_buf == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    esp_err_t post_erase_ret = vol_bdl->ops->read(vol_bdl, post_erase_buf, page_size, 0, page_size);
    free(post_erase_buf);
    if (post_erase_ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "LEB 0 read after erase correctly returned ESP_ERR_NOT_FOUND (unmapped)");
    } else {
        ESP_LOGE(TAG, "LEB 0 read after erase returned unexpected 0x%x", post_erase_ret);
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    /* Releasing vol_bdl also detaches the UBI device (nand_ubi_get_blockdev()
     * sets owns_device = true). nand_bdl is released separately: esp_nand_ubi
     * never owns the raw flash BDL it was attached to. */
    ESP_ERROR_CHECK(vol_bdl->ops->release(vol_bdl));
    ESP_ERROR_CHECK(nand_bdl->ops->release(nand_bdl));
    spi_bus_remove_device(spi);
    spi_bus_free(HOST_ID);

    ESP_LOGI(TAG, "NAND UBI example finished successfully");
}
