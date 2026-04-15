/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#include "spi_nand_flash.h"
#include "dhara/nand.h"
#include "spi_nand_flash_test_helpers.h"
#include "nand_linux_mmap_emul.h"
#include "nand_private/nand_impl_wrap.h"
#include "esp_blockdev.h"
#include "esp_nand_blockdev.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("verify mark_bad_block works with bdl interface", "[spi_nand_flash][bdl]")
{
    nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, true};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t nand_bdl;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &nand_bdl) == 0);

    uint32_t block_size = nand_bdl->geometry.erase_size;
    uint32_t block_num = nand_bdl->geometry.disk_size / block_size;

    uint32_t test_block = 15;
    REQUIRE((test_block < block_num) == true);
    REQUIRE(nand_bdl->ops->erase(nand_bdl, test_block * block_size, block_size) == 0);
    // Verify if test_block is not bad block
    esp_blockdev_cmd_arg_is_bad_block_t bad_block_status = {test_block, false};
    REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &bad_block_status) == 0);
    REQUIRE(bad_block_status.status == false);
    // mark test_block as a bad block
    uint32_t block = test_block;
    REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &block) == 0);
    // Verify if test_block is marked as bad block
    REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &bad_block_status) == 0);
    REQUIRE(bad_block_status.status == true);

    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("verify nand_prog, nand_read, nand_copy, nand_is_free works with bdl interface", "[spi_nand_flash][bdl]")
{
    nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t nand_bdl;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &nand_bdl) == 0);

    uint32_t block_size = nand_bdl->geometry.erase_size;
    uint32_t sector_size = nand_bdl->geometry.write_size;
    uint32_t sector_num = nand_bdl->geometry.disk_size / sector_size;

    uint8_t *pattern_buf = (uint8_t *)malloc(sector_size);
    REQUIRE(pattern_buf != NULL);
    uint8_t *temp_buf = (uint8_t *)malloc(sector_size);
    REQUIRE(temp_buf != NULL);

    spi_nand_flash_fill_buffer(pattern_buf, sector_size / sizeof(uint32_t));

    uint32_t test_block = 20;
    uint32_t test_page = test_block * (block_size / sector_size); //(block_num * pages_per_block)

    REQUIRE((test_page < sector_num) == true);
    // Verify if test_page is free
    esp_blockdev_cmd_arg_is_free_page_t page_free_status = {test_page, true};
    REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_IS_FREE_PAGE, &page_free_status) == 0);
    REQUIRE(page_free_status.status == true);
    // Write/program test_page
    REQUIRE(nand_bdl->ops->write(nand_bdl, pattern_buf, test_page * sector_size, sector_size) == 0);
    // Verify if test_page is used/programmed
    REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_IS_FREE_PAGE, &page_free_status) == 0);
    REQUIRE(page_free_status.status == false);

    REQUIRE(nand_bdl->ops->read(nand_bdl, temp_buf, sector_size, test_page * sector_size, sector_size) == 0);
    REQUIRE(spi_nand_flash_check_buffer(temp_buf, sector_size / sizeof(uint32_t)) == 0);

    free(pattern_buf);
    free(temp_buf);
    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("WL BDL on host: create, geometry, write/read, release", "[spi_nand_flash][bdl]")
{
    nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t flash_bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
    REQUIRE(flash_bdl != nullptr);

    esp_blockdev_handle_t wl_bdl = nullptr;
    REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
    REQUIRE(wl_bdl != nullptr);

    REQUIRE(wl_bdl->geometry.disk_size > 0);
    REQUIRE(wl_bdl->geometry.read_size > 0);
    REQUIRE(wl_bdl->geometry.write_size > 0);
    REQUIRE(wl_bdl->geometry.erase_size > 0);

    uint32_t page_size = wl_bdl->geometry.write_size;
    REQUIRE(page_size > 0);
    uint32_t num_pages = (uint32_t)(wl_bdl->geometry.disk_size / page_size);
    REQUIRE(num_pages > 0);

    uint8_t *pattern_buf = (uint8_t *)malloc(page_size);
    uint8_t *read_buf = (uint8_t *)malloc(page_size);
    REQUIRE(pattern_buf != nullptr);
    REQUIRE(read_buf != nullptr);
    spi_nand_flash_fill_buffer(pattern_buf, page_size / sizeof(uint32_t));

    REQUIRE(wl_bdl->ops->write(wl_bdl, pattern_buf, 0, page_size) == ESP_OK);
    memset(read_buf, 0, page_size);
    REQUIRE(wl_bdl->ops->read(wl_bdl, read_buf, page_size, 0, page_size) == ESP_OK);
    REQUIRE(spi_nand_flash_check_buffer(read_buf, page_size / sizeof(uint32_t)) == 0);

    free(pattern_buf);
    free(read_buf);
    wl_bdl->ops->release(wl_bdl);
}

TEST_CASE("Flash BDL geometry and ops on host", "[spi_nand_flash][bdl]")
{
    nand_file_mmap_emul_config_t conf = {"", 20 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);
    REQUIRE(bdl != nullptr);

    uint32_t block_size = bdl->geometry.erase_size;
    uint32_t num_blocks = (uint32_t)(bdl->geometry.disk_size / block_size);
    REQUIRE(bdl->geometry.disk_size == (uint64_t)num_blocks * block_size);
    REQUIRE(bdl->geometry.read_size == bdl->geometry.write_size);
    REQUIRE(bdl->geometry.read_size > 0);
    REQUIRE(bdl->geometry.erase_size > 0);

    REQUIRE(bdl->ops != nullptr);
    REQUIRE(bdl->ops->read != nullptr);
    REQUIRE(bdl->ops->write != nullptr);
    REQUIRE(bdl->ops->erase != nullptr);
    REQUIRE(bdl->ops->ioctl != nullptr);
    REQUIRE(bdl->ops->release != nullptr);

    bdl->ops->release(bdl);
}

TEST_CASE("Flash BDL COPY_PAGE ioctl on host", "[spi_nand_flash][bdl]")
{
    nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t nand_bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &nand_bdl) == ESP_OK);
    REQUIRE(nand_bdl != nullptr);

    uint32_t block_size = nand_bdl->geometry.erase_size;
    uint32_t page_size = nand_bdl->geometry.write_size;
    uint32_t pages_per_block = block_size / page_size;
    uint32_t src_page = 5 * pages_per_block;
    uint32_t dst_page = 6 * pages_per_block;

    REQUIRE(nand_bdl->ops->erase(nand_bdl, src_page * (uint64_t)page_size, block_size) == ESP_OK);

    uint8_t *pattern_buf = (uint8_t *)malloc(page_size);
    uint8_t *read_buf = (uint8_t *)malloc(page_size);
    REQUIRE(pattern_buf != nullptr);
    REQUIRE(read_buf != nullptr);
    spi_nand_flash_fill_buffer(pattern_buf, page_size / sizeof(uint32_t));

    REQUIRE(nand_bdl->ops->write(nand_bdl, pattern_buf, src_page * (uint64_t)page_size, page_size) == ESP_OK);

    esp_blockdev_cmd_arg_copy_page_t copy_cmd = { .src_page = src_page, .dst_page = dst_page };
    REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_COPY_PAGE, &copy_cmd) == ESP_OK);

    memset(read_buf, 0, page_size);
    REQUIRE(nand_bdl->ops->read(nand_bdl, read_buf, page_size, dst_page * (uint64_t)page_size, page_size) == ESP_OK);
    REQUIRE(spi_nand_flash_check_buffer(read_buf, page_size / sizeof(uint32_t)) == 0);

    free(pattern_buf);
    free(read_buf);
    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("Flash BDL GET_NAND_FLASH_INFO and GET_BAD_BLOCKS_COUNT on host", "[spi_nand_flash][bdl]")
{
    nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t nand_bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &nand_bdl) == ESP_OK);
    REQUIRE(nand_bdl != nullptr);

    esp_blockdev_cmd_arg_nand_flash_info_t flash_info;
    memset(&flash_info, 0, sizeof(flash_info));
    REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_GET_NAND_FLASH_INFO, &flash_info) == ESP_OK);
    REQUIRE(strnlen((const char *)flash_info.device_info.chip_name, sizeof(flash_info.device_info.chip_name)) > 0);
    uint32_t total_blocks = nand_bdl->geometry.disk_size / nand_bdl->geometry.erase_size;
    REQUIRE(flash_info.geometry.num_blocks == total_blocks);

    uint32_t bad_block_count = 0xFFFF;
    REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_GET_BAD_BLOCKS_COUNT, &bad_block_count) == ESP_OK);
    REQUIRE(bad_block_count <= total_blocks);

    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("Error path: nand_flash_get_blockdev NULL/invalid args", "[spi_nand_flash][bdl]")
{
    nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, false};
    spi_nand_flash_config_t config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t out = nullptr;

    REQUIRE(nand_flash_get_blockdev(nullptr, &out) == ESP_ERR_INVALID_ARG);
    REQUIRE(out == nullptr);
    REQUIRE(nand_flash_get_blockdev(&config, nullptr) == ESP_ERR_INVALID_ARG);
}

TEST_CASE("Release and no use-after-free: create, release, create again, minimal r/w", "[spi_nand_flash][bdl]")
{
    nand_file_mmap_emul_config_t conf = {"", 20 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};

    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);
    REQUIRE(bdl != nullptr);
    bdl->ops->release(bdl);

    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);
    REQUIRE(bdl != nullptr);

    uint32_t page_size = bdl->geometry.write_size;
    uint8_t *buf = (uint8_t *)malloc(page_size);
    REQUIRE(buf != nullptr);
    spi_nand_flash_fill_buffer(buf, page_size / sizeof(uint32_t));
    REQUIRE(bdl->ops->write(bdl, buf, 0, page_size) == ESP_OK);
    memset(buf, 0, page_size);
    REQUIRE(bdl->ops->read(bdl, buf, page_size, 0, page_size) == ESP_OK);
    REQUIRE(spi_nand_flash_check_buffer(buf, page_size / sizeof(uint32_t)) == 0);

    free(buf);
    bdl->ops->release(bdl);
}

TEST_CASE("OOB LPN round-trip: prog writes LPN, read_lpn reads it back", "[dhara_oob]")
{
    /* BDL builds: use blockdev (nand_flash_get_blockdev); ctx is the underlying spi_nand_flash_device_t. */
    nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);
    auto *handle = static_cast<spi_nand_flash_device_t *>(bdl->ctx);
    REQUIRE(handle != nullptr);

    uint32_t page_size = bdl->geometry.write_size;
    REQUIRE(page_size > 0);

    std::vector<uint8_t> data(page_size, 0xAB);
    REQUIRE(nand_wrap_prog(handle, 0, data.data(), 42) == ESP_OK);

    uint32_t oob_lpn_out = 0xDEADBEEF;
    REQUIRE(nand_wrap_read_lpn(handle, 0, &oob_lpn_out) == ESP_OK);
    REQUIRE(oob_lpn_out == 42U);

    REQUIRE(nand_wrap_read_lpn(handle, 1, &oob_lpn_out) == ESP_OK);
    REQUIRE(oob_lpn_out == DHARA_OOB_LPN_NONE);

    REQUIRE(nand_wrap_prog(handle, 2, data.data(), DHARA_OOB_LPN_NONE) == ESP_OK);
    REQUIRE(nand_wrap_read_lpn(handle, 2, &oob_lpn_out) == ESP_OK);
    REQUIRE(oob_lpn_out == DHARA_OOB_LPN_NONE);

    bdl->ops->release(bdl);
}

TEST_CASE("Dhara orphan replay: writes after checkpoint are recovered on remount", "[dhara_oob][replay]")
{
    static const char k_nand_dump[] = "/tmp/dhara_replay_test.nand";
    (void)unlink(k_nand_dump);

    nand_file_mmap_emul_config_t emul_cfg = {};
    strncpy(emul_cfg.flash_file_name, k_nand_dump, sizeof(emul_cfg.flash_file_name) - 1);
    emul_cfg.flash_file_name[sizeof(emul_cfg.flash_file_name) - 1] = '\0';
    emul_cfg.flash_file_size = 50 * 1024 * 1024;
    emul_cfg.keep_dump = true;
    spi_nand_flash_config_t nand_flash_config = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};

    /* First mount: checkpointed writes, then orphan writes, power-loss (no final sync). */
    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

        uint32_t page_size = wl_bdl->geometry.write_size;
        REQUIRE(page_size > 0);

        std::vector<uint8_t> pattern(page_size, 0);
        for (uint32_t s = 0; s < 4; s++) {
            memset(pattern.data(), (int)(uint8_t)(0xA0 + s), page_size);
            REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s * page_size, page_size) == ESP_OK);
        }

        REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);

        for (uint32_t s = 4; s < 6; s++) {
            memset(pattern.data(), (int)(uint8_t)(0xA0 + s), page_size);
            REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s * page_size, page_size) == ESP_OK);
        }

        wl_bdl->ops->release(wl_bdl);
    }

    /* Second mount: orphan sectors must replay from OOB LPN. */
    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> buf(page_size, 0);

        for (uint32_t s = 0; s < 6; s++) {
            memset(buf.data(), 0, buf.size());
            REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
            REQUIRE(buf[0] == (uint8_t)(0xA0 + s));
        }

        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Dhara orphan replay: single logical page before any checkpoint survives remount", "[dhara_oob][replay]")
{
    /* Power-loss with zero prior sync: journal may have no flushed CP yet; OOB LPN replay
     * must still recover the one programmed user sector on remount. */
    static const char k_nand_dump[] = "/tmp/dhara_replay_no_checkpoint_yet.nand";
    (void)unlink(k_nand_dump);

    nand_file_mmap_emul_config_t emul_cfg = {};
    strncpy(emul_cfg.flash_file_name, k_nand_dump, sizeof(emul_cfg.flash_file_name) - 1);
    emul_cfg.flash_file_name[sizeof(emul_cfg.flash_file_name) - 1] = '\0';
    emul_cfg.flash_file_size = 50 * 1024 * 1024;
    emul_cfg.keep_dump = true;
    spi_nand_flash_config_t nand_flash_config = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};

    const uint8_t marker = 0x5A;

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

        uint32_t page_size = wl_bdl->geometry.write_size;
        REQUIRE(page_size > 0);
        std::vector<uint8_t> pattern(page_size, marker);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), 0, page_size) == ESP_OK);
        /* Deliberately no wl_bdl->ops->sync — simulate power loss before first checkpoint. */
        wl_bdl->ops->release(wl_bdl);
    }

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> buf(page_size, 0);
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, 0, page_size) == ESP_OK);
        REQUIRE(buf[0] == marker);

        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Dhara orphan replay: orphans span NAND block boundary", "[dhara_oob][replay][boundary]")
{
    /* Task 4.2 Step 3: many post-sync writes so journal programs cross at least one
     * physical erase-block boundary; remount must replay all orphans via next_upage. */
    static const char k_nand_dump[] = "/tmp/dhara_replay_block_boundary.nand";
    (void)unlink(k_nand_dump);

    nand_file_mmap_emul_config_t emul_cfg = {};
    strncpy(emul_cfg.flash_file_name, k_nand_dump, sizeof(emul_cfg.flash_file_name) - 1);
    emul_cfg.flash_file_name[sizeof(emul_cfg.flash_file_name) - 1] = '\0';
    emul_cfg.flash_file_size = 50 * 1024 * 1024;
    emul_cfg.keep_dump = true;
    spi_nand_flash_config_t nand_flash_config = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};

    uint32_t ppb = 0;
    uint32_t total_sectors = 0;
    uint32_t sync_at = 0;

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);

        esp_blockdev_cmd_arg_nand_flash_info_t flash_info = {};
        memset(&flash_info, 0, sizeof(flash_info));
        REQUIRE(flash_bdl->ops->ioctl(flash_bdl, ESP_BLOCKDEV_CMD_GET_NAND_FLASH_INFO, &flash_info) == ESP_OK);
        ppb = 1u << flash_info.geometry.log2_ppb;
        REQUIRE(ppb >= 4u);

        /* One checkpoint mid-stream, then enough unsynced logical writes to cross >=1 PPB boundary. */
        sync_at = ppb;
        total_sectors = ppb * 3u + 24u;

        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;
        REQUIRE(page_size > 0);

        std::vector<uint8_t> pattern(page_size, 0);
        for (uint32_t s = 0; s < total_sectors; s++) {
            memset(pattern.data(), (int)(uint8_t)(0x40 + (s & 0x3Fu)), page_size);
            REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s * page_size, page_size) == ESP_OK);
            if (s + 1u == sync_at) {
                REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);
            }
        }

        wl_bdl->ops->release(wl_bdl);
    }

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> buf(page_size, 0);

        for (uint32_t s = 0; s < total_sectors; s++) {
            memset(buf.data(), 0, buf.size());
            REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
            REQUIRE(buf[0] == (uint8_t)(0x40 + (s & 0x3Fu)));
        }

        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Replay: no orphans (clean shutdown)", "[dhara_oob][replay]")
{
    static const char k_nand_dump[] = "/tmp/dhara_replay_clean_shutdown.nand";
    (void)unlink(k_nand_dump);

    nand_file_mmap_emul_config_t emul_cfg = {};
    strncpy(emul_cfg.flash_file_name, k_nand_dump, sizeof(emul_cfg.flash_file_name) - 1);
    emul_cfg.flash_file_name[sizeof(emul_cfg.flash_file_name) - 1] = '\0';
    emul_cfg.flash_file_size = 50 * 1024 * 1024;
    emul_cfg.keep_dump = true;
    spi_nand_flash_config_t nand_flash_config = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};

    const uint32_t n_sectors = 8;

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> pattern(page_size, 0);
        for (uint32_t s = 0; s < n_sectors; s++) {
            memset(pattern.data(), (int)(uint8_t)(0xC0 + (s & 0x1Fu)), page_size);
            REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s * page_size, page_size) == ESP_OK);
        }
        REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);
        wl_bdl->ops->release(wl_bdl);
    }

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> buf(page_size, 0);
        for (uint32_t s = 0; s < n_sectors; s++) {
            memset(buf.data(), 0, buf.size());
            REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
            REQUIRE(buf[0] == (uint8_t)(0xC0 + (s & 0x1Fu)));
        }
        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Replay: single orphan page (one write after last checkpoint)", "[dhara_oob][replay]")
{
    static const char k_nand_dump[] = "/tmp/dhara_replay_single_orphan.nand";
    (void)unlink(k_nand_dump);

    nand_file_mmap_emul_config_t emul_cfg = {};
    strncpy(emul_cfg.flash_file_name, k_nand_dump, sizeof(emul_cfg.flash_file_name) - 1);
    emul_cfg.flash_file_name[sizeof(emul_cfg.flash_file_name) - 1] = '\0';
    emul_cfg.flash_file_size = 50 * 1024 * 1024;
    emul_cfg.keep_dump = true;
    spi_nand_flash_config_t nand_flash_config = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};

    const uint32_t n_pre = 5;
    const uint32_t n_total = 6;

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> pattern(page_size, 0);
        for (uint32_t s = 0; s < n_pre; s++) {
            memset(pattern.data(), (int)(uint8_t)(0xD0 + s), page_size);
            REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s * page_size, page_size) == ESP_OK);
        }
        REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);
        memset(pattern.data(), (int)(uint8_t)(0xD0 + n_pre), page_size);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)n_pre * page_size, page_size) == ESP_OK);
        wl_bdl->ops->release(wl_bdl);
    }

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> buf(page_size, 0);
        for (uint32_t s = 0; s < n_total; s++) {
            memset(buf.data(), 0, buf.size());
            REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
            REQUIRE(buf[0] == (uint8_t)(0xD0 + s));
        }
        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Replay: two syncs leave no pending orphans", "[dhara_oob][replay]")
{
    static const char k_nand_dump[] = "/tmp/dhara_replay_double_sync.nand";
    (void)unlink(k_nand_dump);

    nand_file_mmap_emul_config_t emul_cfg = {};
    strncpy(emul_cfg.flash_file_name, k_nand_dump, sizeof(emul_cfg.flash_file_name) - 1);
    emul_cfg.flash_file_name[sizeof(emul_cfg.flash_file_name) - 1] = '\0';
    emul_cfg.flash_file_size = 50 * 1024 * 1024;
    emul_cfg.keep_dump = true;
    spi_nand_flash_config_t nand_flash_config = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};

    const uint32_t n_sectors = 6;

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> pattern(page_size, 0);
        for (uint32_t s = 0; s < 3; s++) {
            memset(pattern.data(), (int)(uint8_t)(0xE0 + s), page_size);
            REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s * page_size, page_size) == ESP_OK);
        }
        REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);
        for (uint32_t s = 3; s < n_sectors; s++) {
            memset(pattern.data(), (int)(uint8_t)(0xE0 + s), page_size);
            REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s * page_size, page_size) == ESP_OK);
        }
        REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);
        wl_bdl->ops->release(wl_bdl);
    }

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> buf(page_size, 0);
        for (uint32_t s = 0; s < n_sectors; s++) {
            memset(buf.data(), 0, buf.size());
            REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
            REQUIRE(buf[0] == (uint8_t)(0xE0 + s));
        }
        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

