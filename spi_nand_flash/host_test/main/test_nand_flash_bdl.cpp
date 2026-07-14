/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cstdlib>
#include <unistd.h>
#include <vector>

#include "spi_nand_flash.h"
#include "dhara/nand.h"
#include "dhara/journal.h"
#include "spi_nand_flash_test_helpers.h"
#include "nand_linux_mmap_emul.h"
#include "nand_private/nand_impl_wrap.h"
#include "esp_nand_blockdev.h"

#include <catch2/catch_test_macros.hpp>

static uint32_t test_checkpoint_group_size(uint32_t page_size, uint32_t block_size)
{
    int log2_page_size = __builtin_ctz(page_size);
    int log2_ppb = __builtin_ctz(block_size / page_size);
    int max_meta = (1 << log2_page_size) - DHARA_HEADER_SIZE - DHARA_COOKIE_SIZE;
    int total_meta = DHARA_META_SIZE;
    int ppc = 1;
    while (ppc < log2_ppb) {
        total_meta = (total_meta << 1) + DHARA_META_SIZE;
        if (total_meta > max_meta) {
            break;
        }
        ppc++;
    }
    return 1u << ppc;
}

/* Linux mmap file: each page is k_page data + k_oob, so the file uses k_ppb * (k_page + k_oob)
 * bytes per erase block. chip.block_size is k_ppb * k_page (same as real chips); num_blocks
 * is file_size / (k_ppb * (k_page + k_oob)). BDL disk_size must be num_blocks * chip.block_size.
 * Regression: storing the file stride in chip.block_size broke BDL erase decode. */
TEST_CASE("BDL geometry matches Linux mmap file and chip.block_size", "[spi_nand_flash][bdl]")
{
    constexpr uint32_t k_file_bytes = 16u * 1024u * 1024u;
    constexpr uint32_t k_page = 2048u;
    constexpr uint32_t k_oob = 64u;
    constexpr uint32_t k_ppb = 64u;
    const uint32_t file_bytes_per_physical_block = k_ppb * (k_page + k_oob);
    const uint32_t bdl_bytes_per_erase_block = k_ppb * k_page;
    const uint32_t num_physical_blocks = k_file_bytes / file_bytes_per_physical_block;
    const uint64_t expected_disk_size = (uint64_t)num_physical_blocks * bdl_bytes_per_erase_block;

    nand_file_mmap_emul_config_t conf = {"", k_file_bytes, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);
    REQUIRE(bdl->geometry.write_size == k_page);
    REQUIRE(bdl->geometry.erase_size == bdl_bytes_per_erase_block);
    REQUIRE(bdl->geometry.disk_size == expected_disk_size);
    bdl->ops->release(bdl);
}

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

TEST_CASE("Flash BDL last physical page write/read", "[spi_nand_flash][bdl][raw]")
{
    nand_file_mmap_emul_config_t conf = {"", 16 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);

    const uint32_t page_size = bdl->geometry.write_size;
    const uint32_t block_size = bdl->geometry.erase_size;
    const uint64_t disk_size = bdl->geometry.disk_size;
    const uint32_t num_pages = (uint32_t)(disk_size / page_size);
    const uint32_t pages_per_block = block_size / page_size;
    const uint32_t last_page = num_pages - 1u;
    const uint32_t last_block = last_page / pages_per_block;
    const uint64_t block_addr = (uint64_t)last_block * block_size;

    REQUIRE(bdl->ops->erase(bdl, block_addr, block_size) == ESP_OK);

    uint8_t *w = (uint8_t *)malloc(page_size);
    uint8_t *r = (uint8_t *)malloc(page_size);
    REQUIRE(w != nullptr);
    REQUIRE(r != nullptr);

    spi_nand_flash_fill_buffer(w, page_size / sizeof(uint32_t));
    const uint64_t last_off = (uint64_t)last_page * page_size;
    REQUIRE(bdl->ops->write(bdl, w, last_off, page_size) == ESP_OK);
    memset(r, 0, page_size);
    REQUIRE(bdl->ops->read(bdl, r, page_size, last_off, page_size) == ESP_OK);
    REQUIRE(spi_nand_flash_check_buffer(r, page_size / sizeof(uint32_t)) == 0);

    free(w);
    free(r);
    bdl->ops->release(bdl);
}

TEST_CASE("Flash BDL 16 MiB full erase+program sweep then read-back all pages", "[spi_nand_flash][bdl][raw][sequential]")
{
    nand_file_mmap_emul_config_t conf = {"", 16 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);

    const uint32_t page_size = bdl->geometry.write_size;
    const uint32_t block_size = bdl->geometry.erase_size;
    const uint64_t disk_size = bdl->geometry.disk_size;
    const uint32_t num_blocks = (uint32_t)(disk_size / block_size);
    const uint32_t pages_per_block = block_size / page_size;

    for (uint32_t b = 0; b < num_blocks; b++) {
        const uint64_t ba = (uint64_t)b * block_size;
        REQUIRE(bdl->ops->erase(bdl, ba, block_size) == ESP_OK);
        for (uint32_t i = 0; i < pages_per_block; i++) {
            const uint32_t page = b * pages_per_block + i;
            uint8_t *w = (uint8_t *)malloc(page_size);
            REQUIRE(w != nullptr);
            spi_nand_flash_fill_buffer(w, page_size / sizeof(uint32_t));
            REQUIRE(bdl->ops->write(bdl, w, (uint64_t)page * page_size, page_size) == ESP_OK);
            free(w);
        }
    }

    const uint32_t num_pages = (uint32_t)(disk_size / page_size);
    uint8_t *r = (uint8_t *)malloc(page_size);
    REQUIRE(r != nullptr);
    for (uint32_t page = 0; page < num_pages; page++) {
        memset(r, 0, page_size);
        REQUIRE(bdl->ops->read(bdl, r, page_size, (uint64_t)page * page_size, page_size) == ESP_OK);
        REQUIRE(spi_nand_flash_check_buffer(r, page_size / sizeof(uint32_t)) == 0);
    }
    free(r);
    bdl->ops->release(bdl);
}

TEST_CASE("Flash BDL overwrite same physical page many times", "[spi_nand_flash][bdl][raw][stress]")
{
    nand_file_mmap_emul_config_t conf = {"", 20 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);

    const uint32_t page_size = bdl->geometry.write_size;
    const uint32_t block_size = bdl->geometry.erase_size;
    REQUIRE(bdl->ops->erase(bdl, 0, block_size) == ESP_OK);

    uint8_t *w = (uint8_t *)malloc(page_size);
    uint8_t *r = (uint8_t *)malloc(page_size);
    REQUIRE(w != nullptr);
    REQUIRE(r != nullptr);

    constexpr int kRounds = 150;
    for (int i = 0; i < kRounds; i++) {
        spi_nand_flash_fill_buffer(w, page_size / sizeof(uint32_t));
        REQUIRE(bdl->ops->write(bdl, w, 0, page_size) == ESP_OK);
    }
    spi_nand_flash_fill_buffer(w, page_size / sizeof(uint32_t));
    REQUIRE(bdl->ops->read(bdl, r, page_size, 0, page_size) == ESP_OK);
    REQUIRE(spi_nand_flash_check_buffer(r, page_size / sizeof(uint32_t)) == 0);

    free(w);
    free(r);
    bdl->ops->release(bdl);
}

TEST_CASE("Flash BDL multi-page write in one call", "[spi_nand_flash][bdl][raw]")
{
    nand_file_mmap_emul_config_t conf = {"", 20 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);

    const uint32_t page_size = bdl->geometry.write_size;
    const uint32_t block_size = bdl->geometry.erase_size;
    const uint32_t n = 4;
    REQUIRE(block_size >= n * page_size);

    REQUIRE(bdl->ops->erase(bdl, 0, block_size) == ESP_OK);

    const size_t total = (size_t)n * page_size;
    uint8_t *w = (uint8_t *)malloc(total);
    uint8_t *r = (uint8_t *)malloc(page_size);
    REQUIRE(w != nullptr);
    REQUIRE(r != nullptr);

    for (uint32_t p = 0; p < n; p++) {
        spi_nand_flash_fill_buffer(w + (size_t)p * page_size, page_size / sizeof(uint32_t));
    }
    REQUIRE(bdl->ops->write(bdl, w, 0, total) == ESP_OK);

    for (uint32_t p = 0; p < n; p++) {
        memset(r, 0, page_size);
        REQUIRE(bdl->ops->read(bdl, r, page_size, (uint64_t)p * page_size, page_size) == ESP_OK);
        REQUIRE(spi_nand_flash_check_buffer(r, page_size / sizeof(uint32_t)) == 0);
    }

    free(w);
    free(r);
    bdl->ops->release(bdl);
}

TEST_CASE("Flash BDL sub-page read and misaligned write rejected", "[spi_nand_flash][bdl][raw]")
{
    nand_file_mmap_emul_config_t conf = {"", 20 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);

    const uint32_t page_size = bdl->geometry.write_size;
    const uint32_t block_size = bdl->geometry.erase_size;
    REQUIRE(bdl->ops->erase(bdl, 0, block_size) == ESP_OK);

    uint8_t *full = (uint8_t *)malloc(page_size);
    uint8_t *slice = (uint8_t *)malloc(page_size);
    REQUIRE(full != nullptr);
    REQUIRE(slice != nullptr);
    spi_nand_flash_fill_buffer(full, page_size / sizeof(uint32_t));
    REQUIRE(bdl->ops->write(bdl, full, 0, page_size) == ESP_OK);

    const size_t off = 64;
    const size_t len = page_size - 128;
    REQUIRE(off + len <= page_size);
    memset(slice, 0, len);
    REQUIRE(bdl->ops->read(bdl, slice, len, off, len) == ESP_OK);
    REQUIRE(memcmp(slice, full + off, len) == 0);

    REQUIRE(bdl->ops->write(bdl, full, 1, page_size) == ESP_ERR_INVALID_SIZE);

    free(full);
    free(slice);
    bdl->ops->release(bdl);
}

TEST_CASE("Flash BDL COPY_PAGE same page is idempotent", "[spi_nand_flash][bdl][raw][copy]")
{
    nand_file_mmap_emul_config_t conf = {"", 20 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);

    const uint32_t page_size = bdl->geometry.write_size;
    const uint32_t block_size = bdl->geometry.erase_size;
    const uint32_t page = 3 * (block_size / page_size);

    REQUIRE(bdl->ops->erase(bdl, (page / (block_size / page_size)) * (uint64_t)block_size, block_size) == ESP_OK);

    uint8_t *w = (uint8_t *)malloc(page_size);
    uint8_t *r = (uint8_t *)malloc(page_size);
    REQUIRE(w != nullptr);
    REQUIRE(r != nullptr);
    spi_nand_flash_fill_buffer(w, page_size / sizeof(uint32_t));
    const uint64_t addr = (uint64_t)page * page_size;
    REQUIRE(bdl->ops->write(bdl, w, addr, page_size) == ESP_OK);

    esp_blockdev_cmd_arg_copy_page_t copy_cmd = {.src_page = page, .dst_page = page};
    REQUIRE(bdl->ops->ioctl(bdl, ESP_BLOCKDEV_CMD_COPY_PAGE, &copy_cmd) == ESP_OK);

    memset(r, 0, page_size);
    REQUIRE(bdl->ops->read(bdl, r, page_size, addr, page_size) == ESP_OK);
    REQUIRE(spi_nand_flash_check_buffer(r, page_size / sizeof(uint32_t)) == 0);

    free(w);
    free(r);
    bdl->ops->release(bdl);
}

TEST_CASE("Flash BDL COPY_PAGE does not alter source page", "[spi_nand_flash][bdl][raw][copy]")
{
    nand_file_mmap_emul_config_t conf = {"", 20 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);

    const uint32_t page_size = bdl->geometry.write_size;
    const uint32_t block_size = bdl->geometry.erase_size;
    const uint32_t ppb = block_size / page_size;
    const uint32_t src_page = 2u * ppb;
    const uint32_t dst_page = 2u * ppb + 1u;

    REQUIRE(bdl->ops->erase(bdl, 2u * (uint64_t)block_size, block_size) == ESP_OK);

    uint8_t *w = (uint8_t *)malloc(page_size);
    uint8_t *r = (uint8_t *)malloc(page_size);
    REQUIRE(w != nullptr);
    REQUIRE(r != nullptr);
    spi_nand_flash_fill_buffer(w, page_size / sizeof(uint32_t));
    REQUIRE(bdl->ops->write(bdl, w, (uint64_t)src_page * page_size, page_size) == ESP_OK);

    esp_blockdev_cmd_arg_copy_page_t copy_cmd = {.src_page = src_page, .dst_page = dst_page};
    REQUIRE(bdl->ops->ioctl(bdl, ESP_BLOCKDEV_CMD_COPY_PAGE, &copy_cmd) == ESP_OK);

    memset(r, 0, page_size);
    REQUIRE(bdl->ops->read(bdl, r, page_size, (uint64_t)src_page * page_size, page_size) == ESP_OK);
    REQUIRE(memcmp(w, r, page_size) == 0);

    free(w);
    free(r);
    bdl->ops->release(bdl);
}

TEST_CASE("Flash BDL invalid write/read geometry returns error", "[spi_nand_flash][bdl][raw]")
{
    nand_file_mmap_emul_config_t conf = {"", 20 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);

    const uint32_t page_size = bdl->geometry.write_size;
    const uint64_t disk = bdl->geometry.disk_size;
    uint8_t *buf = (uint8_t *)malloc(page_size * 2);
    REQUIRE(buf != nullptr);
    spi_nand_flash_fill_buffer(buf, page_size / sizeof(uint32_t));

    REQUIRE(bdl->ops->write(bdl, buf, page_size / 2u, page_size) == ESP_ERR_INVALID_SIZE);
    if (page_size >= 512u) {
        REQUIRE(bdl->ops->write(bdl, buf, 0, page_size / 2u) == ESP_ERR_INVALID_SIZE);
    }

    memset(buf, 0, page_size);
    REQUIRE(bdl->ops->read(bdl, buf, page_size, disk, page_size) == ESP_ERR_INVALID_SIZE);
    REQUIRE(bdl->ops->write(bdl, buf, disk, page_size) == ESP_ERR_INVALID_SIZE);

    free(buf);
    bdl->ops->release(bdl);
}

TEST_CASE("Flash BDL GET_PAGE_ECC_STATUS and GET_ECC_STATS (small image)", "[spi_nand_flash][bdl][raw]")
{
    nand_file_mmap_emul_config_t conf = {"", 4 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);

    const uint32_t page_size = bdl->geometry.write_size;
    const uint32_t block_size = bdl->geometry.erase_size;
    REQUIRE(bdl->ops->erase(bdl, 0, block_size) == ESP_OK);

    uint8_t *buf = (uint8_t *)malloc(page_size);
    REQUIRE(buf != nullptr);
    spi_nand_flash_fill_buffer(buf, page_size / sizeof(uint32_t));
    REQUIRE(bdl->ops->write(bdl, buf, 0, page_size) == ESP_OK);

    esp_blockdev_cmd_arg_ecc_status_t ecc_page = {};
    ecc_page.page_num = 0;
    REQUIRE(bdl->ops->ioctl(bdl, ESP_BLOCKDEV_CMD_GET_PAGE_ECC_STATUS, &ecc_page) == ESP_OK);

    esp_blockdev_cmd_arg_ecc_stats_t ecc_stats = {};
    REQUIRE(bdl->ops->ioctl(bdl, ESP_BLOCKDEV_CMD_GET_ECC_STATS, &ecc_stats) == ESP_OK);

    free(buf);
    bdl->ops->release(bdl);
}

TEST_CASE("WL BDL sync after write preserves data", "[spi_nand_flash][bdl][wl]")
{
    nand_file_mmap_emul_config_t conf = {"", 16 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t flash_bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
    esp_blockdev_handle_t wl_bdl = nullptr;
    REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

    const uint32_t page_size = wl_bdl->geometry.write_size;
    uint8_t *w = (uint8_t *)malloc(page_size);
    uint8_t *r = (uint8_t *)malloc(page_size);
    REQUIRE(w != nullptr);
    REQUIRE(r != nullptr);

    spi_nand_flash_fill_buffer(w, page_size / sizeof(uint32_t));
    REQUIRE(wl_bdl->ops->write(wl_bdl, w, page_size, page_size) == ESP_OK);
    REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);
    memset(r, 0, page_size);
    REQUIRE(wl_bdl->ops->read(wl_bdl, r, page_size, page_size, page_size) == ESP_OK);
    REQUIRE(spi_nand_flash_check_buffer(r, page_size / sizeof(uint32_t)) == 0);

    free(w);
    free(r);
    wl_bdl->ops->release(wl_bdl);
}

TEST_CASE("WL BDL MARK_DELETED then rewrite same logical page", "[spi_nand_flash][bdl][wl]")
{
    nand_file_mmap_emul_config_t conf = {"", 16 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t flash_bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
    esp_blockdev_handle_t wl_bdl = nullptr;
    REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

    const uint32_t page_size = wl_bdl->geometry.write_size;
    const uint32_t page_index = 5;
    const uint64_t off = (uint64_t)page_index * page_size;

    uint8_t *w = (uint8_t *)malloc(page_size);
    uint8_t *r = (uint8_t *)malloc(page_size);
    REQUIRE(w != nullptr);
    REQUIRE(r != nullptr);

    memset(w, 0x22, page_size);
    REQUIRE(wl_bdl->ops->write(wl_bdl, w, off, page_size) == ESP_OK);

    esp_blockdev_cmd_arg_erase_t trim_arg = {.start_addr = off, .erase_len = page_size};
    REQUIRE(wl_bdl->ops->ioctl(wl_bdl, ESP_BLOCKDEV_CMD_MARK_DELETED, &trim_arg) == ESP_OK);

    spi_nand_flash_fill_buffer(w, page_size / sizeof(uint32_t));
    REQUIRE(wl_bdl->ops->write(wl_bdl, w, off, page_size) == ESP_OK);
    memset(r, 0, page_size);
    REQUIRE(wl_bdl->ops->read(wl_bdl, r, page_size, off, page_size) == ESP_OK);
    REQUIRE(spi_nand_flash_check_buffer(r, page_size / sizeof(uint32_t)) == 0);

    free(w);
    free(r);
    wl_bdl->ops->release(wl_bdl);
}

TEST_CASE("WL BDL MARK_DELETED misaligned range returns ESP_ERR_INVALID_SIZE", "[spi_nand_flash][bdl][wl]")
{
    nand_file_mmap_emul_config_t conf = {"", 20 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t flash_bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
    esp_blockdev_handle_t wl_bdl = nullptr;
    REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

    const uint32_t page_size = wl_bdl->geometry.write_size;
    esp_blockdev_cmd_arg_erase_t bad = {.start_addr = 1, .erase_len = page_size};
    REQUIRE(wl_bdl->ops->ioctl(wl_bdl, ESP_BLOCKDEV_CMD_MARK_DELETED, &bad) == ESP_ERR_INVALID_SIZE);

    wl_bdl->ops->release(wl_bdl);
}

TEST_CASE("WL BDL 16 MiB sequential logical page fill and read-back", "[spi_nand_flash][bdl][wl][sequential]")
{
    nand_file_mmap_emul_config_t conf = {"", 16 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t flash_bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
    esp_blockdev_handle_t wl_bdl = nullptr;
    REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

    const uint32_t page_size = wl_bdl->geometry.write_size;
    const uint32_t num_pages = (uint32_t)(wl_bdl->geometry.disk_size / page_size);
    REQUIRE(wl_bdl->geometry.disk_size == (uint64_t)num_pages * page_size);

    uint8_t *w = (uint8_t *)malloc(page_size);
    REQUIRE(w != nullptr);

    for (uint32_t p = 0; p < num_pages; p++) {
        spi_nand_flash_fill_buffer(w, page_size / sizeof(uint32_t));
        REQUIRE(wl_bdl->ops->write(wl_bdl, w, (uint64_t)p * page_size, page_size) == ESP_OK);
    }
    REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);

    uint8_t *r = (uint8_t *)malloc(page_size);
    REQUIRE(r != nullptr);
    for (uint32_t p = 0; p < num_pages; p++) {
        memset(r, 0, page_size);
        REQUIRE(wl_bdl->ops->read(wl_bdl, r, page_size, (uint64_t)p * page_size, page_size) == ESP_OK);
        REQUIRE(spi_nand_flash_check_buffer(r, page_size / sizeof(uint32_t)) == 0);
    }

    free(w);
    free(r);
    wl_bdl->ops->release(wl_bdl);
}

TEST_CASE("WL BDL hot-set random writes then read-back", "[spi_nand_flash][bdl][wl][stress]")
{
    nand_file_mmap_emul_config_t conf = {"", 16 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t flash_bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
    esp_blockdev_handle_t wl_bdl = nullptr;
    REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

    constexpr uint32_t kHotSetSize = 30u;
    constexpr uint32_t kTotalWrites = 1200u;

    const uint32_t page_size = wl_bdl->geometry.write_size;
    const uint32_t num_pages = (uint32_t)(wl_bdl->geometry.disk_size / page_size);
    REQUIRE(num_pages >= kHotSetSize);

    uint8_t *w = (uint8_t *)malloc(page_size);
    uint8_t *r = (uint8_t *)malloc(page_size);
    REQUIRE(w != nullptr);
    REQUIRE(r != nullptr);

    bool written[kHotSetSize] = {};

    std::srand(0xDEADBEEFu);
    for (uint32_t op = 0; op < kTotalWrites; op++) {
        const uint32_t lp = (uint32_t)((unsigned)std::rand() % kHotSetSize);
        spi_nand_flash_fill_buffer(w, page_size / sizeof(uint32_t));
        REQUIRE(wl_bdl->ops->write(wl_bdl, w, (uint64_t)lp * page_size, page_size) == ESP_OK);
        written[lp] = true;
    }

    REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);

    for (uint32_t s = 0; s < kHotSetSize; s++) {
        if (!written[s]) {
            continue;
        }
        REQUIRE(wl_bdl->ops->read(wl_bdl, r, page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
        REQUIRE(spi_nand_flash_check_buffer(r, page_size / sizeof(uint32_t)) == 0);
    }

    free(w);
    free(r);
    wl_bdl->ops->release(wl_bdl);
}

TEST_CASE("WL BDL hot-set with interleaved MARK_DELETED", "[spi_nand_flash][bdl][wl][stress][trim]")
{
    nand_file_mmap_emul_config_t conf = {"", 16 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t flash_bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
    esp_blockdev_handle_t wl_bdl = nullptr;
    REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

    constexpr uint32_t kHotSetSize = 30u;
    constexpr uint32_t kTotalWrites = 1200u;

    const uint32_t page_size = wl_bdl->geometry.write_size;
    const uint32_t num_pages = (uint32_t)(wl_bdl->geometry.disk_size / page_size);
    REQUIRE(num_pages >= kHotSetSize);

    uint8_t *w = (uint8_t *)malloc(page_size);
    uint8_t *r = (uint8_t *)malloc(page_size);
    REQUIRE(w != nullptr);
    REQUIRE(r != nullptr);

    bool trimmed[kHotSetSize] = {};
    bool written[kHotSetSize] = {};

    std::srand(0xCAFEBABEu);
    for (uint32_t op = 0; op < kTotalWrites; op++) {
        const uint32_t lp = (uint32_t)((unsigned)std::rand() % kHotSetSize);

        if (op % 100u == 99u) {
            const uint32_t t = (uint32_t)((unsigned)std::rand() % kHotSetSize);
            esp_blockdev_cmd_arg_erase_t trim_arg = {
                .start_addr = (uint64_t)t * page_size,
                .erase_len = page_size,
            };
            if (wl_bdl->ops->ioctl(wl_bdl, ESP_BLOCKDEV_CMD_MARK_DELETED, &trim_arg) == ESP_OK) {
                trimmed[t] = true;
            }
        }

        spi_nand_flash_fill_buffer(w, page_size / sizeof(uint32_t));
        REQUIRE(wl_bdl->ops->write(wl_bdl, w, (uint64_t)lp * page_size, page_size) == ESP_OK);
        trimmed[lp] = false;
        written[lp] = true;
    }

    REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);

    for (uint32_t s = 0; s < kHotSetSize; s++) {
        if (trimmed[s] || !written[s]) {
            continue;
        }
        REQUIRE(wl_bdl->ops->read(wl_bdl, r, page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
        REQUIRE(spi_nand_flash_check_buffer(r, page_size / sizeof(uint32_t)) == 0);
    }

    free(w);
    free(r);
    wl_bdl->ops->release(wl_bdl);
}

TEST_CASE("Dhara OOB LPN: prog writes LPN, read_lpn reads it back", "[dhara_oob]")
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
    uint8_t lpn_buf[4] = { 42, 0, 0, 0 };
    REQUIRE(nand_wrap_prog_ext(handle, 0, data.data(),
                               CONFIG_NAND_FLASH_OOB_LPN_OFFSET, 4, lpn_buf) == ESP_OK);

    uint32_t oob_lpn_out = 0xDEADBEEF;
    REQUIRE(nand_wrap_read_lpn(handle, 0, &oob_lpn_out) == ESP_OK);
    REQUIRE(oob_lpn_out == 42U);

    REQUIRE(nand_wrap_read_lpn(handle, 1, &oob_lpn_out) == ESP_OK);
    REQUIRE(oob_lpn_out == DHARA_OOB_LPN_NONE);

    REQUIRE(nand_wrap_prog(handle, 2, data.data()) == ESP_OK);
    REQUIRE(nand_wrap_read_lpn(handle, 2, &oob_lpn_out) == ESP_OK);
    REQUIRE(oob_lpn_out == DHARA_OOB_LPN_NONE);

    bdl->ops->release(bdl);
}

TEST_CASE("Dhara OOB LPN: copy_ext writes LPN on destination, source OOB not inherited", "[dhara_oob]")
{
    nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);
    auto *handle = static_cast<spi_nand_flash_device_t *>(bdl->ctx);
    REQUIRE(handle != nullptr);

    uint32_t page_size = bdl->geometry.write_size;
    REQUIRE(page_size > 0);

    std::vector<uint8_t> data_src(page_size, 0xAB);
    uint8_t lpn_src[4] = { 42, 0, 0, 0 };
    REQUIRE(nand_wrap_prog_ext(handle, 0, data_src.data(),
                               CONFIG_NAND_FLASH_OOB_LPN_OFFSET, 4, lpn_src) == ESP_OK);

    uint8_t lpn_dst[4] = { 99, 0, 0, 0 };
    REQUIRE(nand_wrap_copy_ext(handle, 0, 1,
                               CONFIG_NAND_FLASH_OOB_LPN_OFFSET, 4, lpn_dst) == ESP_OK);

    uint32_t oob_lpn_out = 0xDEADBEEF;
    REQUIRE(nand_wrap_read_lpn(handle, 1, &oob_lpn_out) == ESP_OK);
    REQUIRE(oob_lpn_out == 99u);

    std::vector<uint8_t> data_dst(page_size, 0);
    REQUIRE(nand_wrap_read(handle, 1, 0, page_size, data_dst.data()) == ESP_OK);
    REQUIRE(memcmp(data_src.data(), data_dst.data(), page_size) == 0);

    REQUIRE(nand_wrap_copy_ext(handle, 0, 2, 0, 0, nullptr) == ESP_OK);
    REQUIRE(nand_wrap_read_lpn(handle, 2, &oob_lpn_out) == ESP_OK);
    REQUIRE(oob_lpn_out == DHARA_OOB_LPN_NONE);

    bdl->ops->release(bdl);
}

TEST_CASE("Dhara OOB LPN: prog_ext with oob_len=0 and arbitrary OOB offset does not corrupt markers", "[dhara_oob]")
{
    nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, false};
    spi_nand_flash_config_t nand_flash_config = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t bdl = nullptr;
    REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &bdl) == ESP_OK);
    auto *handle = static_cast<spi_nand_flash_device_t *>(bdl->ctx);
    REQUIRE(handle != nullptr);

    uint32_t page_size = bdl->geometry.write_size;
    REQUIRE(page_size > 0);

    std::vector<uint8_t> data(page_size, 0x77);

    REQUIRE(nand_wrap_prog_ext(handle, 0, data.data(), 0, 0, nullptr) == ESP_OK);
    uint32_t oob_lpn_out = 0xDEADBEEF;
    REQUIRE(nand_wrap_read_lpn(handle, 0, &oob_lpn_out) == ESP_OK);
    REQUIRE(oob_lpn_out == DHARA_OOB_LPN_NONE);
    bool is_free = true;
    REQUIRE(nand_wrap_is_free(handle, 0, &is_free) == ESP_OK);
    REQUIRE_FALSE(is_free);

    uint8_t arbitrary_oob[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    REQUIRE(nand_wrap_prog_ext(handle, 1, data.data(), 8, 4, arbitrary_oob) == ESP_OK);
    REQUIRE(nand_wrap_read_lpn(handle, 1, &oob_lpn_out) == ESP_OK);
    REQUIRE(oob_lpn_out == DHARA_OOB_LPN_NONE);
    REQUIRE(nand_wrap_is_free(handle, 1, &is_free) == ESP_OK);
    REQUIRE_FALSE(is_free);
    std::vector<uint8_t> readback(page_size, 0);
    REQUIRE(nand_wrap_read(handle, 1, 0, page_size, readback.data()) == ESP_OK);
    REQUIRE(memcmp(data.data(), readback.data(), page_size) == 0);

    uint8_t lpn_buf[4] = { 42, 0, 0, 0 };
    REQUIRE(nand_wrap_prog_ext(handle, 2, data.data(),
                               CONFIG_NAND_FLASH_OOB_LPN_OFFSET, 4, lpn_buf) == ESP_OK);
    REQUIRE(nand_wrap_read_lpn(handle, 2, &oob_lpn_out) == ESP_OK);
    REQUIRE(oob_lpn_out == 42u);

    bdl->ops->release(bdl);
}

TEST_CASE("Dhara orphan replay: GC copy-page orphans recovered on remount", "[dhara_oob][replay][gc]")
{
    static const char k_nand_dump[] = "/tmp/dhara_replay_gc_copy_orphan.nand";
    (void)unlink(k_nand_dump);

    const uint32_t file_size = 4325376u;
    nand_file_mmap_emul_config_t emul_cfg = {};
    strncpy(emul_cfg.flash_file_name, k_nand_dump, sizeof(emul_cfg.flash_file_name) - 1);
    emul_cfg.flash_file_name[sizeof(emul_cfg.flash_file_name) - 1] = '\0';
    emul_cfg.flash_file_size = file_size;
    emul_cfg.keep_dump = true;
    spi_nand_flash_config_t nand_flash_config = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};

    const uint8_t tag_fill = 0xA0;
    const uint8_t tag_overwrite = 0xB0;
    const uint32_t overwrite_sector = 0;

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

        uint32_t page_size = wl_bdl->geometry.write_size;
        REQUIRE(page_size > 0);
        uint32_t total_pages = wl_bdl->geometry.disk_size / page_size;

        std::vector<uint8_t> pattern(page_size, tag_fill);

        for (uint32_t s = 0; s < total_pages; s++) {
            REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(),
                                       (uint64_t)s * page_size, page_size) == ESP_OK);
        }

        REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);

        memset(pattern.data(), (int)tag_overwrite, page_size);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(),
                                   (uint64_t)overwrite_sector * page_size, page_size) == ESP_OK);

        wl_bdl->ops->release(wl_bdl);
    }

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

        uint32_t page_size = wl_bdl->geometry.write_size;
        uint32_t total_pages = wl_bdl->geometry.disk_size / page_size;
        std::vector<uint8_t> buf(page_size, 0);
        std::vector<uint8_t> expected_overwrite(page_size, tag_overwrite);
        std::vector<uint8_t> expected_fill(page_size, tag_fill);

        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, 0, page_size) == ESP_OK);
        REQUIRE(memcmp(buf.data(), expected_overwrite.data(), page_size) == 0);

        uint32_t check_mid = total_pages / 2;
        memset(buf.data(), 0, page_size);
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size,
                                  (uint64_t)check_mid * page_size, page_size) == ESP_OK);
        REQUIRE(memcmp(buf.data(), expected_fill.data(), page_size) == 0);

        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
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
        uint32_t block_size = flash_bdl->geometry.erase_size;
        REQUIRE(page_size > 0);
        const uint32_t pages_per_checkpoint = test_checkpoint_group_size(page_size, block_size);

        std::vector<uint8_t> pattern(page_size, 0);
        for (uint32_t s = 0; s < pages_per_checkpoint; s++) {
            memset(pattern.data(), (int)(uint8_t)(0xA0 + s), page_size);
            REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s * page_size, page_size) == ESP_OK);
        }

        REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);

        for (uint32_t s = pages_per_checkpoint; s < pages_per_checkpoint + 2u; s++) {
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
        uint32_t block_size = flash_bdl->geometry.erase_size;
        const uint32_t total = test_checkpoint_group_size(page_size, block_size) + 2u;
        std::vector<uint8_t> buf(page_size, 0);
        std::vector<uint8_t> expected(page_size, 0);

        for (uint32_t s = 0; s < total; s++) {
            memset(buf.data(), 0, buf.size());
            REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
            memset(expected.data(), (int)(uint8_t)(0xA0 + s), page_size);
            REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
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
        /* Deliberately no wl_bdl->ops->sync — simulate power loss before first checkpoint.
         *
         * release() simulates sudden power loss because the WL release path does NOT
         * call dhara_map_sync().  The call chain is:
         *
         *   wl_bdl->ops->release()
         *     → spi_nand_flash_wl_blockdev_release()       (nand_wl_blockdev.c)
         *       → nand_wl_detach_ops()                     (dhara_glue.c)
         *           free(handle->ops_priv_data)             // drops dhara_map in-memory state
         *           handle->ops = NULL                      // no dhara_map_sync() called
         *       → nand_handle->ops->release()               (nand_flash_blockdev.c)
         *           → nand_emul_deinit()                    // munmap + close backing file
         *           free(dev_handle->work_buffer / read_buffer / temp_buffer)
         *           free(dev_handle); free(handle);
         *       → free(wl_handle)
         *
         * Dhara's in-memory journal/metadata buffers are discarded without being
         * flushed, while the mmap emulation retains all physically-programmed pages
         * (including OOB LPN) in the backing file (keep_dump=true).
         * On remount, dhara_map_resume() scans NAND, discovers orphan pages via
         * OOB LPNs, and replays them — exactly matching real sudden-power-loss behavior. */
        wl_bdl->ops->release(wl_bdl);
    }

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> buf(page_size, 0);
        std::vector<uint8_t> expected(page_size, 0);
        memset(expected.data(), (int)marker, page_size);
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, 0, page_size) == ESP_OK);
        REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);

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
        std::vector<uint8_t> expected(page_size, 0);

        for (uint32_t s = 0; s < total_sectors; s++) {
            memset(buf.data(), 0, buf.size());
            REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
            memset(expected.data(), (int)(uint8_t)(0x40 + (s & 0x3Fu)), page_size);
            REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        }

        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Dhara orphan replay: no orphans after clean shutdown", "[dhara_oob][replay]")
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
        std::vector<uint8_t> expected(page_size, 0);
        for (uint32_t s = 0; s < n_sectors; s++) {
            memset(buf.data(), 0, buf.size());
            REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
            memset(expected.data(), (int)(uint8_t)(0xC0 + (s & 0x1Fu)), page_size);
            REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        }
        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Dhara orphan replay: single orphan after last checkpoint", "[dhara_oob][replay]")
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
        std::vector<uint8_t> expected(page_size, 0);
        for (uint32_t s = 0; s < n_total; s++) {
            memset(buf.data(), 0, buf.size());
            REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
            memset(expected.data(), (int)(uint8_t)(0xD0 + s), page_size);
            REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        }
        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Dhara orphan replay: two syncs leave no pending orphans", "[dhara_oob][replay]")
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
        std::vector<uint8_t> expected(page_size, 0);
        for (uint32_t s = 0; s < n_sectors; s++) {
            memset(buf.data(), 0, buf.size());
            REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
            memset(expected.data(), (int)(uint8_t)(0xE0 + s), page_size);
            REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        }
        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Dhara orphan replay: permuted arbitrary logical sector order before sync", "[dhara_oob][replay]")
{
    static const char k_nand_dump[] = "/tmp/dhara_replay_permuted_lpn.nand";
    (void)unlink(k_nand_dump);

    /* Sparse small logical sector indices (not 0,1,2…); WL capacity always >> 100 here. */
    const uint32_t sa = 17u;
    const uint32_t sb = 59u;
    const uint32_t sc = 83u;
    const uint8_t tag_sa = 0x4Du;
    const uint8_t tag_sb = 0x92u;
    const uint8_t tag_sc = 0xE4u;

    nand_file_mmap_emul_config_t emul_cfg = {};
    strncpy(emul_cfg.flash_file_name, k_nand_dump, sizeof(emul_cfg.flash_file_name) - 1);
    emul_cfg.flash_file_name[sizeof(emul_cfg.flash_file_name) - 1] = '\0';
    emul_cfg.flash_file_size = 50 * 1024 * 1024;
    emul_cfg.keep_dump = true;
    spi_nand_flash_config_t nand_flash_config = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;

        std::vector<uint8_t> pattern(page_size, 0);
        /* Physical journal order != ascending LPN: write sc, then sa, then sb — all before any sync. */
        memset(pattern.data(), (int)tag_sc, page_size);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)sc * page_size, page_size) == ESP_OK);
        memset(pattern.data(), (int)tag_sa, page_size);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)sa * page_size, page_size) == ESP_OK);
        memset(pattern.data(), (int)tag_sb, page_size);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)sb * page_size, page_size) == ESP_OK);

        wl_bdl->ops->release(wl_bdl);
    }

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> buf(page_size, 0);
        std::vector<uint8_t> expected(page_size, 0);
        memset(buf.data(), 0, buf.size());
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)sa * page_size, page_size) == ESP_OK);
        memset(expected.data(), (int)tag_sa, page_size);
        REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        memset(buf.data(), 0, buf.size());
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)sb * page_size, page_size) == ESP_OK);
        memset(expected.data(), (int)tag_sb, page_size);
        REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        memset(buf.data(), 0, buf.size());
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)sc * page_size, page_size) == ESP_OK);
        memset(expected.data(), (int)tag_sc, page_size);
        REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Dhara orphan replay: two physical writes same arbitrary LPN — last wins", "[dhara_oob][replay]")
{
    static const char k_nand_dump[] = "/tmp/dhara_replay_double_lpn.nand";
    (void)unlink(k_nand_dump);

    const uint32_t sid = 37u;
    const uint8_t tag_first = 0x71u;
    const uint8_t tag_second = 0xE8u;

    nand_file_mmap_emul_config_t emul_cfg = {};
    strncpy(emul_cfg.flash_file_name, k_nand_dump, sizeof(emul_cfg.flash_file_name) - 1);
    emul_cfg.flash_file_name[sizeof(emul_cfg.flash_file_name) - 1] = '\0';
    emul_cfg.flash_file_size = 50 * 1024 * 1024;
    emul_cfg.keep_dump = true;
    spi_nand_flash_config_t nand_flash_config = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;

        std::vector<uint8_t> pattern(page_size, 0);
        memset(pattern.data(), (int)tag_first, page_size);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)sid * page_size, page_size) == ESP_OK);
        memset(pattern.data(), (int)tag_second, page_size);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)sid * page_size, page_size) == ESP_OK);

        wl_bdl->ops->release(wl_bdl);
    }

    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> buf(page_size, 0);
        std::vector<uint8_t> expected(page_size, 0);
        memset(expected.data(), (int)tag_second, page_size);
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)sid * page_size, page_size) == ESP_OK);
        REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Dhara orphan replay: two power-loss generations on same dump", "[dhara_oob][replay]")
{
    static const char k_nand_dump[] = "/tmp/dhara_replay_two_generations.nand";
    (void)unlink(k_nand_dump);

    const uint32_t s1 = 11u;
    const uint32_t s2 = 47u;
    const uint32_t s3 = 91u;
    const uint8_t tag_s1 = 0x55u;
    const uint8_t tag_s2 = 0xAAu;
    const uint8_t tag_s3 = 0x5Au;

    nand_file_mmap_emul_config_t emul_cfg = {};
    strncpy(emul_cfg.flash_file_name, k_nand_dump, sizeof(emul_cfg.flash_file_name) - 1);
    emul_cfg.flash_file_name[sizeof(emul_cfg.flash_file_name) - 1] = '\0';
    emul_cfg.flash_file_size = 50 * 1024 * 1024;
    emul_cfg.keep_dump = true;
    spi_nand_flash_config_t nand_flash_config = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};

    /* Generation 1: two orphans, no sync. */
    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;

        std::vector<uint8_t> pattern(page_size, 0);
        memset(pattern.data(), (int)tag_s1, page_size);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s1 * page_size, page_size) == ESP_OK);
        memset(pattern.data(), (int)tag_s2, page_size);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s2 * page_size, page_size) == ESP_OK);
        wl_bdl->ops->release(wl_bdl);
    }

    /* Remount 1: recover gen1, add gen2 orphan, no sync again. */
    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;

        std::vector<uint8_t> buf(page_size, 0);
        std::vector<uint8_t> expected(page_size, 0);
        memset(buf.data(), 0, buf.size());
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s1 * page_size, page_size) == ESP_OK);
        memset(expected.data(), (int)tag_s1, page_size);
        REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        memset(buf.data(), 0, buf.size());
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s2 * page_size, page_size) == ESP_OK);
        memset(expected.data(), (int)tag_s2, page_size);
        REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);

        std::vector<uint8_t> pattern(page_size, 0);
        memset(pattern.data(), (int)tag_s3, page_size);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s3 * page_size, page_size) == ESP_OK);

        wl_bdl->ops->release(wl_bdl);
    }

    /* Remount 2: all three sectors must match (gen1 + gen2 orphans replayed). */
    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);
        uint32_t page_size = wl_bdl->geometry.write_size;
        std::vector<uint8_t> buf(page_size, 0);
        std::vector<uint8_t> expected(page_size, 0);
        memset(buf.data(), 0, buf.size());
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s1 * page_size, page_size) == ESP_OK);
        memset(expected.data(), (int)tag_s1, page_size);
        REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        memset(buf.data(), 0, buf.size());
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s2 * page_size, page_size) == ESP_OK);
        memset(expected.data(), (int)tag_s2, page_size);
        REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        memset(buf.data(), 0, buf.size());
        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s3 * page_size, page_size) == ESP_OK);
        memset(expected.data(), (int)tag_s3, page_size);
        REQUIRE(memcmp(buf.data(), expected.data(), page_size) == 0);
        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

TEST_CASE("Dhara orphan replay: orphan that updates a pre-checkpoint sector is correctly replayed", "[dhara_oob][replay]")
{
    /* Scenario exercised here (distinct from the new-allocation tests above):
     *   1. Write sectors 0..ppc-1 and sync  → checkpoint written; all sectors in map.
     *   2. Re-write sector 0 (update)       → one mid-group orphan; trace_path must
     *                                          find the EXISTING mapping (not NOT_FOUND)
     *                                          and j->root advances to the orphan page.
     *   3. Power-loss (release without sync).
     *   4. Remount: replay sees the orphan via OOB LPN, calls trace_path which finds
     *      sector 0 already mapped, patches page_buf, and sets j->root = orphan page.
     *   5. Read sector 0 → must return the UPDATED value (step 2), not the
     *      pre-checkpoint value (step 1). A j->root or trace_path bug would
     *      cause the old data to be returned or a read error.
     */
    static const char k_nand_dump[] = "/tmp/dhara_replay_sector_update.nand";
    (void)unlink(k_nand_dump);

    nand_file_mmap_emul_config_t emul_cfg = {};
    strncpy(emul_cfg.flash_file_name, k_nand_dump, sizeof(emul_cfg.flash_file_name) - 1);
    emul_cfg.flash_file_name[sizeof(emul_cfg.flash_file_name) - 1] = '\0';
    emul_cfg.flash_file_size = 50 * 1024 * 1024;
    emul_cfg.keep_dump = true;
    spi_nand_flash_config_t nand_flash_config = {&emul_cfg, 0, SPI_NAND_IO_MODE_SIO, 0};

    const uint8_t tag_initial = 0x11u;
    const uint8_t tag_updated = 0x22u;
    uint32_t page_size = 0;
    uint32_t pages_per_checkpoint = 0;

    /* Mount 1: fill one checkpoint group, sync, then overwrite sector 0 (mid-group orphan). */
    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

        page_size = wl_bdl->geometry.write_size;
        pages_per_checkpoint = test_checkpoint_group_size(page_size, flash_bdl->geometry.erase_size);
        REQUIRE(page_size > 0);
        REQUIRE(pages_per_checkpoint >= 2u);

        std::vector<uint8_t> pattern(page_size, tag_initial);
        for (uint32_t s = 0; s < pages_per_checkpoint; s++) {
            REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), (uint64_t)s * page_size, page_size) == ESP_OK);
        }
        REQUIRE(wl_bdl->ops->sync(wl_bdl) == ESP_OK);

        memset(pattern.data(), (int)tag_updated, page_size);
        REQUIRE(wl_bdl->ops->write(wl_bdl, pattern.data(), 0, page_size) == ESP_OK);

        wl_bdl->ops->release(wl_bdl);
    }

    /* Mount 2: orphan replay must return the updated value for sector 0. */
    {
        esp_blockdev_handle_t flash_bdl = nullptr;
        esp_blockdev_handle_t wl_bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(&nand_flash_config, &flash_bdl) == ESP_OK);
        REQUIRE(spi_nand_flash_wl_get_blockdev(flash_bdl, &wl_bdl) == ESP_OK);

        std::vector<uint8_t> buf(page_size, 0);
        std::vector<uint8_t> expected_updated(page_size, tag_updated);
        std::vector<uint8_t> expected_initial(page_size, tag_initial);

        REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, 0, page_size) == ESP_OK);
        REQUIRE(memcmp(buf.data(), expected_updated.data(), page_size) == 0);

        for (uint32_t s = 1; s < pages_per_checkpoint; s++) {
            memset(buf.data(), 0, page_size);
            REQUIRE(wl_bdl->ops->read(wl_bdl, buf.data(), page_size, (uint64_t)s * page_size, page_size) == ESP_OK);
            REQUIRE(memcmp(buf.data(), expected_initial.data(), page_size) == 0);
        }

        wl_bdl->ops->release(wl_bdl);
    }

    (void)unlink(k_nand_dump);
}

