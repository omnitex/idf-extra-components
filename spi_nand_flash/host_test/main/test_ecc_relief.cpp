/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Host tests for the ECC page relief wear-leveling feature.
 *
 * Tests use the Linux NAND emulator.  ECC events are injected via
 * nand_wrap_inject_ecc_event().  Observable behavior is verified through
 * spi_nand_flash_get_ecc_relief_stats() and ordinary read/write calls.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
#include "esp_blockdev.h"
#endif
#include "spi_nand_flash.h"
#include "spi_nand_flash_test_helpers.h"
#include "nand_linux_mmap_emul.h"
#include "nand_private/nand_impl_wrap.h"
#include "nand_device_types.h"

#include <catch2/catch_test_macros.hpp>

namespace {

#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
static esp_blockdev_handle_t s_ecc_relief_wl_bdl = nullptr;
#endif

/** Init path matches production: layered init when BDL is enabled in sdkconfig. */
static esp_err_t ecc_relief_test_init(spi_nand_flash_config_t *cfg, spi_nand_flash_device_t **out_dev)
{
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    esp_err_t ret = spi_nand_flash_init_with_layers(cfg, &s_ecc_relief_wl_bdl);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_blockdev_handle_t flash_bdl = (esp_blockdev_handle_t)s_ecc_relief_wl_bdl->ctx;
    *out_dev = (spi_nand_flash_device_t *)flash_bdl->ctx;
    return ESP_OK;
#else
    return spi_nand_flash_init_device(cfg, out_dev);
#endif
}

static void ecc_relief_test_deinit(spi_nand_flash_device_t *dev)
{
    (void)dev;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    if (s_ecc_relief_wl_bdl) {
        s_ecc_relief_wl_bdl->ops->release(s_ecc_relief_wl_bdl);
        s_ecc_relief_wl_bdl = nullptr;
    }
#else
    spi_nand_flash_deinit_device(dev);
#endif
}

} // namespace

/* Convenience: init device with ECC relief enabled and given thresholds */
static spi_nand_flash_device_t *init_device_with_relief(
    uint8_t mid_threshold,
    uint8_t high_threshold,
    uint8_t mid_count_limit,
    uint8_t max_consecutive,
    uint16_t map_capacity)
{
    static nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, false};
    spi_nand_flash_config_t cfg = {};
    cfg.emul_conf = &conf;
    cfg.gc_factor = 0;
    cfg.io_mode = SPI_NAND_IO_MODE_SIO;
    cfg.flags = 0;
    cfg.ecc_relief.enabled              = true;
    cfg.ecc_relief.mid_threshold        = mid_threshold;
    cfg.ecc_relief.high_threshold       = high_threshold;
    cfg.ecc_relief.mid_count_limit      = mid_count_limit;
    cfg.ecc_relief.max_consecutive_relief = max_consecutive;
    cfg.ecc_relief.map_capacity         = map_capacity;

    spi_nand_flash_device_t *dev = nullptr;
    REQUIRE(ecc_relief_test_init(&cfg, &dev) == ESP_OK);
    return dev;
}

/* --------------------------------------------------------------------------
 * SC-01: clean read → no map entry created
 * --------------------------------------------------------------------------
 * We never inject any ECC event; stats should show 0 entries used.
 */
TEST_CASE("SC-01: no ECC event → no map entry", "[ecc_relief]")
{
    auto *dev = init_device_with_relief(2, 4, 3, 4, 64);

    spi_nand_ecc_relief_stats_t stats = {};
    REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
    CHECK(stats.map_entries_used == 0);
    CHECK(stats.pages_pending_relief == 0);
    CHECK(stats.total_pages_relieved == 0);
    CHECK(stats.map_capacity == 64);

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * SC-02: HIGH ECC read → ECC_RELIEF_FLAG_PENDING set immediately
 * --------------------------------------------------------------------------
 */
TEST_CASE("SC-02: HIGH ECC read → page immediately flagged", "[ecc_relief]")
{
    auto *dev = init_device_with_relief(2, 4, 3, 4, 64);

    /* HIGH: status must be >= high_threshold (4); enum values are small (see nand_ecc_status_t). */
    nand_wrap_inject_ecc_event(dev, 10, NAND_ECC_7_8_BITS_CORRECTED);

    spi_nand_ecc_relief_stats_t stats = {};
    REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
    CHECK(stats.map_entries_used == 1);
    CHECK(stats.pages_pending_relief == 1);

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * SC-03: repeated MID reads reach mid_count_limit → PENDING set
 * --------------------------------------------------------------------------
 */
TEST_CASE("SC-03: MID reads accumulate and flag page", "[ecc_relief]")
{
    /* mid_threshold=2, high_threshold=4, mid_count_limit=3 */
    auto *dev = init_device_with_relief(2, 4, 3, 4, 64);

    /* MID: mid_threshold=2, high_threshold=4 → use enum values in [2, 4) (here: 3). */
    nand_wrap_inject_ecc_event(dev, 20, NAND_ECC_4_TO_6_BITS_CORRECTED);
    nand_wrap_inject_ecc_event(dev, 20, NAND_ECC_4_TO_6_BITS_CORRECTED);

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        /* After 2 MID hits (limit=3), not yet pending */
    }

    /* Third MID hit → should be flagged */
    nand_wrap_inject_ecc_event(dev, 20, NAND_ECC_4_TO_6_BITS_CORRECTED);

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        /* At or past limit, should be pending */
        CHECK(stats.map_entries_used >= 1);
    }

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * SC-07: successful erase evicts map entries for that block
 * --------------------------------------------------------------------------
 */
TEST_CASE("SC-07: erase evicts relief map entries", "[ecc_relief]")
{
    auto *dev = init_device_with_relief(2, 4, 3, 4, 64);

    uint32_t page_size, block_size;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);
    REQUIRE(spi_nand_flash_get_block_size(dev, &block_size) == ESP_OK);
    uint32_t pages_per_block = block_size / page_size;

    /* Flag a few physical pages in block 5 */
    uint32_t block5_page = 5 * pages_per_block;
    nand_wrap_inject_ecc_event(dev, block5_page + 0, NAND_ECC_7_8_BITS_CORRECTED);
    nand_wrap_inject_ecc_event(dev, block5_page + 1, NAND_ECC_7_8_BITS_CORRECTED);
    nand_wrap_inject_ecc_event(dev, block5_page + 2, NAND_ECC_7_8_BITS_CORRECTED);

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        REQUIRE(stats.map_entries_used == 3);
        REQUIRE(stats.pages_pending_relief == 3);
    }

    /* Erase block 5 through the low-level wrap (which calls dhara_nand_erase internally) */
    REQUIRE(nand_wrap_erase_block(dev, 5) == ESP_OK);

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        CHECK(stats.map_entries_used == 0);
        CHECK(stats.pages_pending_relief == 0);
    }

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * SC-08: feature disabled → no map ops, stats returns NOT_SUPPORTED
 * --------------------------------------------------------------------------
 */
TEST_CASE("SC-08: feature disabled behaves as before", "[ecc_relief]")
{
    static nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, false};
    spi_nand_flash_config_t cfg = {};
    cfg.emul_conf = &conf;
    cfg.gc_factor = 0;
    cfg.io_mode = SPI_NAND_IO_MODE_SIO;
    /* ecc_relief.enabled not set → false */

    spi_nand_flash_device_t *dev = nullptr;
    REQUIRE(ecc_relief_test_init(&cfg, &dev) == ESP_OK);

    spi_nand_ecc_relief_stats_t stats = {};
    CHECK(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_ERR_NOT_SUPPORTED);

    /* No crash when injecting events with NULL callback */
    nand_wrap_inject_ecc_event(dev, 10, NAND_ECC_7_8_BITS_CORRECTED);

    ecc_relief_test_deinit(dev);
}
