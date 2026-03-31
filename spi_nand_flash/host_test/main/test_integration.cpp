/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Integration tests: failure model wiring into core emulator operations.
 * These tests verify that the failure model's should_fail_* callbacks are
 * actually called during erase/write/read, causing operations to fail.
 */
#include <string.h>
#include <stdlib.h>
#include "nand_emul_advanced.h"
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include "nand_private/nand_impl_wrap.h"
#include <catch2/catch_test_macros.hpp>

/* -----------------------------------------------------------------------
 * Helper: make a device with the threshold failure model.
 * max_erases: block fails after this many erases (0 = never).
 * max_programs: page fails after this many programs (0 = never / large).
 * ---------------------------------------------------------------------- */
static spi_nand_flash_device_t *make_threshold_dev(uint32_t max_erases,
                                                    uint32_t max_programs)
{
    static threshold_failure_config_t th;
    th.max_block_erases  = max_erases;
    th.max_page_programs = max_programs ? max_programs : 100000u;
    th.fail_over_limit   = true;

    static sparse_hash_backend_config_t be;
    be.initial_capacity      = 16;
    be.load_factor           = 0.75f;
    be.enable_histogram_query = false;

    static nand_emul_advanced_config_t cfg;
    cfg = {};
    cfg.base_config              = {"", 32 * 1024 * 1024, true};
    cfg.metadata_backend         = &nand_sparse_hash_backend;
    cfg.metadata_backend_config  = &be;
    cfg.failure_model            = &nand_threshold_failure_model;
    cfg.failure_model_config     = &th;
    cfg.track_block_level        = true;
    cfg.track_page_level         = true;

    spi_nand_flash_device_t *dev;
    REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);
    return dev;
}

/* -----------------------------------------------------------------------
 * Erase failure tests
 * ---------------------------------------------------------------------- */

TEST_CASE("erase succeeds up to threshold, fails on threshold+1",
          "[advanced][integration][threshold-erase]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(3, 0);

    for (int i = 0; i < 3; i++) {
        REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    }
    // 4th erase must fail (threshold = 3)
    esp_err_t ret = nand_wrap_erase_block(dev, 0);
    REQUIRE(ret != ESP_OK);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("block is marked bad in metadata after erase failure",
          "[advanced][integration][threshold-erase]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(1, 0);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK); // at limit
    REQUIRE(nand_wrap_erase_block(dev, 0) != ESP_OK); // over limit → bad

    block_metadata_t meta = {};
    REQUIRE(nand_emul_get_block_wear(dev, 0, &meta) == ESP_OK);
    REQUIRE(meta.is_bad_block == true);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("other blocks unaffected when one block exceeds erase threshold",
          "[advanced][integration][threshold-erase]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(1, 0);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 0) != ESP_OK); // block 0: over limit
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK); // block 1: unaffected

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

/* -----------------------------------------------------------------------
 * Threshold model — exact count test
 * ---------------------------------------------------------------------- */

TEST_CASE("erase succeeds for exactly max_block_erases operations",
          "[advanced][integration][threshold-model]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(10, 0);

    for (int i = 0; i < 10; i++) {
        REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    }
    REQUIRE(nand_wrap_erase_block(dev, 0) != ESP_OK);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

/* -----------------------------------------------------------------------
 * Write failure tests
 * ---------------------------------------------------------------------- */

TEST_CASE("write succeeds up to threshold, fails on threshold+1",
          "[advanced][integration][threshold-write]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(100, 2);

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    // First two programs succeed
    REQUIRE(nand_wrap_prog(dev, 0, buf) == ESP_OK);
    REQUIRE(nand_wrap_prog(dev, 0, buf) == ESP_OK);
    // Third must fail (max_page_programs = 2)
    REQUIRE(nand_wrap_prog(dev, 0, buf) != ESP_OK);

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

/* -----------------------------------------------------------------------
 * Read failure tests
 * ---------------------------------------------------------------------- */

TEST_CASE("read is never failed by threshold model",
          "[advanced][integration][threshold-read]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(1, 1);

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    // Threshold model never fails reads
    for (int i = 0; i < 100; i++) {
        REQUIRE(nand_wrap_read(dev, 0, 0, sector_size, buf) == ESP_OK);
    }

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
