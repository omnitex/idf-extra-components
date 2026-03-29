/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include "nand_private/nand_impl_wrap.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("existing init/read/write/erase API is unchanged", "[advanced][backward-compat]")
{
    // Use the OLD API only — no advanced headers
    nand_file_mmap_emul_config_t conf = {"", 32 * 1024 * 1024, true};
    spi_nand_flash_config_t cfg = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    spi_nand_flash_device_t *dev;

    REQUIRE(spi_nand_flash_init_device(&cfg, &dev) == ESP_OK);

    uint32_t sector_num, sector_size, block_size;
    REQUIRE(spi_nand_flash_get_capacity(dev, &sector_num) == ESP_OK);
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);
    REQUIRE(spi_nand_flash_get_block_size(dev, &block_size) == ESP_OK);

    REQUIRE(sector_num > 0);
    REQUIRE(sector_size > 0);
    REQUIRE(block_size > 0);

    // Erase, write, read back a page — basic smoke test
    uint32_t test_block = 0;
    uint32_t pages_per_block = block_size / sector_size;
    uint32_t test_page = test_block * pages_per_block;

    REQUIRE(nand_wrap_erase_block(dev, test_block) == ESP_OK);

    uint8_t *write_buf = (uint8_t *)malloc(sector_size);
    uint8_t *read_buf  = (uint8_t *)malloc(sector_size);
    REQUIRE(write_buf != NULL);
    REQUIRE(read_buf  != NULL);

    memset(write_buf, 0xA5, sector_size);
    REQUIRE(nand_wrap_prog(dev, test_page, write_buf) == ESP_OK);
    REQUIRE(nand_wrap_read(dev, test_page, 0, sector_size, read_buf) == ESP_OK);
    REQUIRE(memcmp(write_buf, read_buf, sector_size) == 0);

    free(write_buf);
    free(read_buf);
    REQUIRE(spi_nand_flash_deinit_device(dev) == ESP_OK);
}
