/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nand_emul_advanced.h"
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include "nand_private/nand_impl_wrap.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <cstdlib>

TEST_CASE("advanced init returns ESP_OK with minimal config", "[advanced][advanced-init]")
{
    nand_emul_advanced_config_t cfg = {};
    cfg.base_config = {"", 32 * 1024 * 1024, true};
    cfg.metadata_backend = NULL;  // no backend yet
    cfg.failure_model    = NULL;
    cfg.track_block_level = true;
    cfg.track_page_level  = true;

    spi_nand_flash_device_t *dev;
    REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);
    REQUIRE(dev != NULL);

    // Basic operations must still work after advanced init
    uint32_t sector_num;
    REQUIRE(spi_nand_flash_get_capacity(dev, &sector_num) == ESP_OK);
    REQUIRE(sector_num > 0);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("advanced init with NULL config returns error", "[advanced][advanced-init]")
{
    spi_nand_flash_device_t *dev = NULL;
    REQUIRE(nand_emul_advanced_init(&dev, NULL) != ESP_OK);
    REQUIRE(dev == NULL);
}

TEST_CASE("advanced field is NULL after normal nand_emul_init", "[advanced][advanced-init]")
{
    // The existing init path must leave the advanced pointer NULL
    // (backward compat: existing code must not be affected)
    nand_file_mmap_emul_config_t conf = {"", 32 * 1024 * 1024, true};
    spi_nand_flash_config_t nand_cfg = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    spi_nand_flash_device_t *dev;
    REQUIRE(spi_nand_flash_init_device(&nand_cfg, &dev) == ESP_OK);

    // Expose via a getter rather than poking internals
    REQUIRE(nand_emul_has_advanced_tracking(dev) == false);

    REQUIRE(spi_nand_flash_deinit_device(dev) == ESP_OK);
}
