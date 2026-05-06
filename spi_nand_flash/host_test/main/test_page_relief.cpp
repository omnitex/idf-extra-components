/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstring>
#include <vector>

extern "C" {
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include "nand_fault_sim.h"
#include "esp_blockdev.h"
#include "esp_nand_blockdev.h"
#include "spi_nand_flash_test_helpers.h"
}

#include <catch2/catch_test_macros.hpp>

static constexpr size_t FS_SMALL = (size_t)16u * 1024u * 1024u;
static constexpr uint32_t LOG2_PPB = 6u;
static constexpr uint32_t LOG2_PAGE_SIZE = 11u;
static constexpr uint32_t PAGE_SIZE = 1u << LOG2_PAGE_SIZE;
static constexpr uint32_t OOB_SIZE = 64u;
static constexpr uint32_t EMUL_PAGE_SIZE = PAGE_SIZE + OOB_SIZE;
static constexpr uint32_t PAGES_PER_BLOCK = 1u << LOG2_PPB;
static constexpr uint32_t FILE_BYTES_PER_BLOCK = PAGES_PER_BLOCK * EMUL_PAGE_SIZE;

static uint32_t blocks_for_size(size_t flash_size)
{
    return (uint32_t)(flash_size / FILE_BYTES_PER_BLOCK);
}

struct PageReliefCapture {
    int count = 0;

    static void on_relief(uint32_t, nand_ecc_status_t, void *ctx)
    {
        auto *self = static_cast<PageReliefCapture *>(ctx);
        self->count++;
    }
};

struct PageReliefFixture {
    nand_file_mmap_emul_config_t emul_cfg = {};
    nand_fault_sim_config_t fault_cfg = {};
    esp_blockdev_handle_t flash_bdl = nullptr;
    esp_blockdev_handle_t wl_bdl = nullptr;
    PageReliefCapture capture = {};
    uint32_t sector_size = 0;

    void init(size_t flash_size, uint32_t warm_relief_blocks)
    {
        emul_cfg = {"", flash_size, false};

        fault_cfg = {};
        fault_cfg.on_page_relief = &PageReliefCapture::on_relief;
        fault_cfg.page_relief_cb_ctx = &capture;
        if (warm_relief_blocks > 0) {
            /* Block erase count >= 1 should emulate elevated ECC on those blocks. */
            fault_cfg.ecc_prog_high_erase_threshold = 1;
        }

        REQUIRE(nand_fault_sim_init(blocks_for_size(flash_size), PAGES_PER_BLOCK, &fault_cfg) == ESP_OK);

        spi_nand_flash_config_t flash_cfg = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};
        REQUIRE(nand_flash_get_blockdev(&flash_cfg, &flash_bdl) == ESP_OK);
        REQUIRE(flash_bdl != nullptr);

        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        REQUIRE(wl_bdl != nullptr);
        sector_size = wl_bdl->geometry.write_size;
        REQUIRE(sector_size > 0);

        const uint32_t total_blocks = (uint32_t)(flash_bdl->geometry.disk_size / flash_bdl->geometry.erase_size);
        REQUIRE(warm_relief_blocks < total_blocks);
        for (uint32_t b = 0; b < warm_relief_blocks; b++) {
            const uint64_t addr = (uint64_t)b * flash_bdl->geometry.erase_size;
            REQUIRE(flash_bdl->ops->erase(flash_bdl, addr, flash_bdl->geometry.erase_size) == ESP_OK);
        }
    }

    void destroy()
    {
        if (wl_bdl != nullptr) {
            wl_bdl->ops->release(wl_bdl);
            wl_bdl = nullptr;
            flash_bdl = nullptr;
        } else if (flash_bdl != nullptr) {
            flash_bdl->ops->release(flash_bdl);
            flash_bdl = nullptr;
        }
        nand_fault_sim_deinit();
    }

    uint32_t get_bad_block_count() const
    {
        uint32_t bad_blocks = 0;
        REQUIRE(flash_bdl->ops->ioctl(flash_bdl, ESP_BLOCKDEV_CMD_GET_BAD_BLOCKS_COUNT, &bad_blocks) == ESP_OK);
        return bad_blocks;
    }

    void write_sector_with_seed(uint32_t sector, uint32_t seed)
    {
        std::vector<uint8_t> wbuf(sector_size);
        spi_nand_flash_fill_buffer_seeded(wbuf.data(), sector_size / sizeof(uint32_t), seed);
        REQUIRE(wl_bdl->ops->write(wl_bdl, wbuf.data(), (uint64_t)sector * sector_size, sector_size) == ESP_OK);
    }

    void verify_sector_with_seed(uint32_t sector, uint32_t seed)
    {
        std::vector<uint8_t> rbuf(sector_size, 0);
        REQUIRE(wl_bdl->ops->read(wl_bdl, rbuf.data(), sector_size, (uint64_t)sector * sector_size, sector_size) == ESP_OK);
        REQUIRE(spi_nand_flash_check_buffer_seeded(rbuf.data(), sector_size / sizeof(uint32_t), seed) == 0);
    }
};

TEST_CASE("page_relief: enqueue skips worn pages and data survives", "[page_relief]")
{
    PageReliefFixture f;
    f.init(FS_SMALL, /*warm_relief_blocks=*/2u);
    const uint32_t bad_before = f.get_bad_block_count();

    f.write_sector_with_seed(0, 0xA501u);
    REQUIRE(f.wl_bdl->ops->sync(f.wl_bdl) == ESP_OK);
    f.verify_sector_with_seed(0, 0xA501u);

    const uint32_t bad_after = f.get_bad_block_count();
    CHECK(f.capture.count > 0);
    CHECK(bad_after == bad_before);
    f.destroy();
}

TEST_CASE("page_relief: multiple relief events across sequential writes", "[page_relief]")
{
    PageReliefFixture f;
    f.init(FS_SMALL, /*warm_relief_blocks=*/3u);
    const uint32_t bad_before = f.get_bad_block_count();

    constexpr uint32_t kSectors = 24u;
    for (uint32_t s = 0; s < kSectors; s++) {
        f.write_sector_with_seed(s, 0x2000u + s);
    }
    REQUIRE(f.wl_bdl->ops->sync(f.wl_bdl) == ESP_OK);

    for (uint32_t s = 0; s < kSectors; s++) {
        f.verify_sector_with_seed(s, 0x2000u + s);
    }

    const uint32_t bad_after = f.get_bad_block_count();
    CHECK(f.capture.count >= 2);
    CHECK(bad_after == bad_before);
    f.destroy();
}

TEST_CASE("page_relief: no elevated ECC keeps normal operation", "[page_relief]")
{
    PageReliefFixture f;
    f.init(FS_SMALL, /*warm_relief_blocks=*/0u);
    const uint32_t bad_before = f.get_bad_block_count();

    constexpr uint32_t kSectors = 20u;
    for (uint32_t s = 0; s < kSectors; s++) {
        f.write_sector_with_seed(s, 0x5A00u + s);
    }
    REQUIRE(f.wl_bdl->ops->sync(f.wl_bdl) == ESP_OK);

    for (uint32_t s = 0; s < kSectors; s++) {
        f.verify_sector_with_seed(s, 0x5A00u + s);
    }

    const uint32_t bad_after = f.get_bad_block_count();
    CHECK(f.capture.count == 0);
    CHECK(bad_after == bad_before);
    f.destroy();
}
