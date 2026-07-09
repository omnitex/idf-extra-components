/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Task 7 item 6 ("Release: all memory freed"): repeatedly exercises every
 * attach/detach and open_volume/release path so AddressSanitizer's
 * LeakSanitizer - enabled for this binary in host_test/main/CMakeLists.txt -
 * can catch anything nand_ubi_detach()/vol_bdl->ops->release() fail to free.
 * Looping amplifies a single leaked allocation into a total LeakSanitizer is
 * guaranteed to report, rather than relying on the one-shot coverage the
 * functional test files (test_nand_ubi_attach.cpp / _volume.cpp / _rw.cpp)
 * already incidentally provide.
 *
 * Note: LeakSanitizer only performs its exit-time leak check on Linux. Apple
 * Clang's ASAN runtime on macOS does not implement LeakSanitizer (ASAN's own
 * invalid-access checks still apply), so running this binary on macOS cannot
 * produce a leak verdict - use a Linux run (matches CI's run-target-linux job)
 * for the leak-detection guarantee this file exists to provide.
 */

#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "esp_nand_ubi.h"
#include "nand_ubi_test_helpers.h"

extern "C" {
#include "nand_ubi_priv.h"
}

using namespace nand_ubi_test;

namespace {

constexpr uint32_t kImageSeq = 0xABCD1234u;
constexpr int kIterations = 32;
/* 8 PEBs (matches the free-pool-exhaustion fixture in test_nand_ubi_rw.cpp):
 * enough to exercise multiple lnums per cycle while keeping each of the
 * kIterations emulated-NAND mmap files small and fast to create/tear down. */
constexpr uint32_t kFileBytes = 8u * 131072u;

} // namespace

TEST_CASE("leak: repeated attach/detach on a blank chip frees everything", "[nand_ubi][leak]")
{
    for (int i = 0; i < kIterations; i++) {
        esp_blockdev_handle_t nand_bdl = make_test_nand(kFileBytes);
        nand_ubi_device_t *dev = nullptr;
        REQUIRE(nand_ubi_attach(nand_bdl, nullptr, &dev) == ESP_OK);
        REQUIRE(nand_ubi_detach(dev) == ESP_OK);
        nand_bdl->ops->release(nand_bdl);
    }
}

TEST_CASE("leak: repeated attach/detach on a formatted chip (with a bad block) frees everything",
          "[nand_ubi][leak]")
{
    for (int i = 0; i < kIterations; i++) {
        esp_blockdev_handle_t nand_bdl = make_test_nand(kFileBytes);
        uint32_t page_size = (uint32_t)nand_bdl->geometry.read_size;
        uint32_t peb_size = (uint32_t)nand_bdl->geometry.erase_size;
        uint32_t vid_hdr_offset = page_size;
        uint32_t data_offset = 2u * page_size;

        uint32_t bad_pnum = 2;
        REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &bad_pnum) == ESP_OK);
        format_peb(nand_bdl, 0, page_size, peb_size, kImageSeq, vid_hdr_offset, data_offset, 0, 1);
        format_peb(nand_bdl, 1, page_size, peb_size, kImageSeq, vid_hdr_offset, data_offset, 1, 2);

        nand_ubi_device_t *dev = nullptr;
        REQUIRE(nand_ubi_attach(nand_bdl, nullptr, &dev) == ESP_OK);
        REQUIRE(dev->leb_count == 2);
        REQUIRE(nand_ubi_detach(dev) == ESP_OK);
        nand_bdl->ops->release(nand_bdl);
    }
}

TEST_CASE("leak: repeated open_volume/release on the same device frees everything", "[nand_ubi][leak]")
{
    esp_blockdev_handle_t nand_bdl = make_test_nand(kFileBytes);
    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, nullptr, &dev) == ESP_OK);

    for (int i = 0; i < kIterations; i++) {
        esp_blockdev_handle_t vol_bdl = nullptr;
        REQUIRE(nand_ubi_open_volume(dev, 0, &vol_bdl) == ESP_OK);
        REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    }

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("leak: repeated get_blockdev + release (owns_device) frees everything", "[nand_ubi][leak]")
{
    for (int i = 0; i < kIterations; i++) {
        esp_blockdev_handle_t nand_bdl = make_test_nand(kFileBytes);
        nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
        esp_blockdev_handle_t vol_bdl = nullptr;
        REQUIRE(nand_ubi_get_blockdev(nand_bdl, &cfg, &vol_bdl) == ESP_OK);
        REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
        nand_bdl->ops->release(nand_bdl);
    }
}

TEST_CASE("leak: write/erase cycles across every lnum free every allocated PEB header buffer",
          "[nand_ubi][leak]")
{
    esp_blockdev_handle_t nand_bdl = make_test_nand(kFileBytes);
    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, nullptr, &dev) == ESP_OK);
    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, 0, &vol_bdl) == ESP_OK);

    uint32_t leb_count = (uint32_t)(vol_bdl->geometry.disk_size / dev->leb_size);
    std::vector<uint8_t> buf(dev->page_size, 0x5A);
    for (uint32_t lnum = 0; lnum < leb_count; lnum++) {
        REQUIRE(vol_bdl->ops->write(vol_bdl, buf.data(), (uint64_t)lnum * dev->leb_size, buf.size()) == ESP_OK);
        REQUIRE(vol_bdl->ops->erase(vol_bdl, (uint64_t)lnum * dev->leb_size, dev->leb_size) == ESP_OK);
    }

    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}
