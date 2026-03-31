/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Phase 2, T6–T15: Sparse hash backend tests.
 *
 * Tests written first (RED) before implementation.
 * Tags: [advanced][sparse-backend]
 */

#include "nand_emul_advanced.h"
#include "nand_linux_mmap_emul.h"
#include "nand_private/nand_impl_wrap.h"
#include "spi_nand_flash.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cstdlib>
#include <cstring>
#include <cstdio>

/* --------------------------------------------------------------------------
 * Shared fixture helper
 * -------------------------------------------------------------------------- */

static spi_nand_flash_device_t *make_advanced_dev_with_sparse_backend(void)
{
    sparse_hash_backend_config_t be_cfg = {};
    be_cfg.initial_capacity        = 16;
    be_cfg.load_factor             = 0.75f;
    be_cfg.enable_histogram_query  = false;

    nand_emul_advanced_config_t cfg = {};
    cfg.base_config                = {"", 32 * 1024 * 1024, true};
    cfg.metadata_backend           = &nand_sparse_hash_backend;
    cfg.metadata_backend_config    = &be_cfg;
    cfg.track_block_level          = true;
    cfg.track_page_level           = true;

    spi_nand_flash_device_t *dev = NULL;
    REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);
    REQUIRE(dev != NULL);
    return dev;
}

/* --------------------------------------------------------------------------
 * P2.T7a: sparse backend init / deinit
 * -------------------------------------------------------------------------- */

TEST_CASE("sparse backend init/deinit, no leaks", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

/* --------------------------------------------------------------------------
 * P2.T9a: block erase tracking
 * -------------------------------------------------------------------------- */

TEST_CASE("block erase increments erase_count in block metadata",
          "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    REQUIRE(nand_wrap_erase_block(dev, 3) == ESP_OK);

    block_metadata_t meta = {};
    REQUIRE(nand_emul_get_block_wear(dev, 3, &meta) == ESP_OK);
    REQUIRE(meta.erase_count == 1);
    REQUIRE(meta.block_num   == 3);

    REQUIRE(nand_wrap_erase_block(dev, 3) == ESP_OK);
    REQUIRE(nand_emul_get_block_wear(dev, 3, &meta) == ESP_OK);
    REQUIRE(meta.erase_count == 2);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("never-erased block returns zero metadata", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();
    block_metadata_t meta = {};
    REQUIRE(nand_emul_get_block_wear(dev, 99, &meta) == ESP_OK);
    REQUIRE(meta.erase_count == 0);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

/* --------------------------------------------------------------------------
 * P2.T11a: page program tracking
 * -------------------------------------------------------------------------- */

TEST_CASE("page program increments program_count", "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    uint8_t *buf = (uint8_t *)malloc(sector_size);
    REQUIRE(buf != NULL);
    memset(buf, 0xCC, sector_size);

    REQUIRE(nand_wrap_prog(dev, 0, buf) == ESP_OK); // page 0

    page_metadata_t pm = {};
    REQUIRE(nand_emul_get_page_wear(dev, 0, &pm) == ESP_OK);
    REQUIRE(pm.program_count       == 1);
    REQUIRE(pm.program_count_total == 0); // no erase yet, nothing folded

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("program_count resets to 0 after block erase, total preserved",
          "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    REQUIRE(nand_wrap_prog(dev, 0, buf) == ESP_OK);       // program_count → 1
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);     // fold: total += 1, count → 0

    page_metadata_t pm = {};
    REQUIRE(nand_emul_get_page_wear(dev, 0, &pm) == ESP_OK);
    REQUIRE(pm.program_count       == 0);
    REQUIRE(pm.program_count_total == 1);

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

/* --------------------------------------------------------------------------
 * P2.T11b-a: page read tracking and erase fold
 * -------------------------------------------------------------------------- */

TEST_CASE("read_count increments once per page per read call",
          "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    /* Read page 0 three times */
    REQUIRE(nand_wrap_read(dev, 0, 0, sector_size, buf) == ESP_OK);
    REQUIRE(nand_wrap_read(dev, 0, 0, sector_size, buf) == ESP_OK);
    REQUIRE(nand_wrap_read(dev, 0, 0, sector_size, buf) == ESP_OK);

    page_metadata_t pm = {};
    REQUIRE(nand_emul_get_page_wear(dev, 0, &pm) == ESP_OK);
    REQUIRE(pm.read_count       == 3);
    REQUIRE(pm.read_count_total == 0);

    /* Erase: fold read_count into read_count_total */
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_emul_get_page_wear(dev, 0, &pm) == ESP_OK);
    REQUIRE(pm.read_count       == 0);
    REQUIRE(pm.read_count_total == 3);

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

/* --------------------------------------------------------------------------
 * P2.T15a: aggregate statistics
 * -------------------------------------------------------------------------- */

TEST_CASE("get_wear_stats returns correct min/max/avg after known operations",
          "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    /* Erase block 0 once, block 1 three times */
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);

    nand_wear_stats_t stats = {};
    REQUIRE(nand_emul_get_wear_stats(dev, &stats) == ESP_OK);

    REQUIRE(stats.min_block_erases == 1);
    REQUIRE(stats.max_block_erases == 3);
    /* avg = (1+3)/2 = 2; variation = (max-min)/avg = (3-1)/2 = 1.0 */
    REQUIRE(stats.wear_leveling_variation == Catch::Approx(1.0));

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("wear_leveling_variation is 0.0 when no blocks erased",
          "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();
    nand_wear_stats_t stats = {};
    REQUIRE(nand_emul_get_wear_stats(dev, &stats) == ESP_OK);
    REQUIRE(stats.wear_leveling_variation == 0.0);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

/* --------------------------------------------------------------------------
 * P2.T13a: block iteration
 * -------------------------------------------------------------------------- */

TEST_CASE("iterate_worn_blocks visits all erased blocks",
          "[advanced][sparse-backend]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    for (uint32_t b = 0; b < 5; b++) {
        REQUIRE(nand_wrap_erase_block(dev, b) == ESP_OK);
    }

    int count = 0;
    REQUIRE(nand_emul_iterate_worn_blocks(dev,
        [](uint32_t, block_metadata_t *, void *ud) -> bool {
            (*(int *)ud)++;
            return true;
        }, &count) == ESP_OK);

    REQUIRE(count == 5);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

/* --------------------------------------------------------------------------
 * P4.T11-T12: JSON export
 * -------------------------------------------------------------------------- */

TEST_CASE("export_json creates a non-empty JSON file starting with '{'",
          "[advanced][sparse-backend][json]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    // Perform some operations so there's data to export
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);

    const char *path = "/tmp/test_nand_export.json";
    REQUIRE(nand_emul_export_json(dev, path) == ESP_OK);

    FILE *fp = fopen(path, "r");
    REQUIRE(fp != NULL);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    int first = fgetc(fp);
    fclose(fp);

    REQUIRE(sz > 0);
    REQUIRE(first == '{');

    remove(path);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("export_json returns INVALID_STATE without advanced init",
          "[advanced][sparse-backend][json]")
{
    nand_file_mmap_emul_config_t conf = {"", 32 * 1024 * 1024, true};
    spi_nand_flash_config_t nand_cfg  = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    spi_nand_flash_device_t *dev;
    REQUIRE(spi_nand_flash_init_device(&nand_cfg, &dev) == ESP_OK);
    REQUIRE(nand_emul_export_json(dev, "/tmp/x.json") == ESP_ERR_INVALID_STATE);
    REQUIRE(spi_nand_flash_deinit_device(dev) == ESP_OK);
}
