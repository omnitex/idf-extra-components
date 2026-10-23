/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <catch2/catch_test_macros.hpp>

#include "esp_nand_ubi.h"
#include "nand_ubi_test_helpers.h"

extern "C" {
#include "nand_ubi_priv.h"
}

using namespace nand_ubi_test;

namespace {

constexpr uint32_t kImageSeq = 0xABCD1234u;

struct test_geometry {
    esp_blockdev_handle_t nand_bdl;
    uint32_t page_size;
    uint32_t peb_size;
    uint32_t vid_hdr_offset;
    uint32_t data_offset;
};

test_geometry make_geometry()
{
    test_geometry g {};
    g.nand_bdl = make_test_nand();
    g.page_size = (uint32_t)g.nand_bdl->geometry.read_size;
    g.peb_size = (uint32_t)g.nand_bdl->geometry.erase_size;
    g.vid_hdr_offset = g.page_size;
    g.data_offset = 2u * g.page_size;
    return g;
}

/* Formats 3 PEBs (lnum 0..2) and attaches; leb_count == 3, matching test_nand_ubi_attach.cpp's
 * primary fixture. Shared here so volume-open tests don't need to re-derive it. */
nand_ubi_device_t *attach_with_3_lebs(const test_geometry &g)
{
    format_volume_table(g.nand_bdl, g.page_size, g.peb_size, kImageSeq,
                        g.vid_hdr_offset, g.data_offset, {3});
    for (uint32_t pnum = 0; pnum < 3; pnum++) {
        format_peb(g.nand_bdl, pnum + 2, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset,
                   /*lnum=*/pnum, /*sqnum=*/pnum + 1);
    }
    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);
    return dev;
}

} // namespace

TEST_CASE("open_volume: vol_id 0 geometry matches its vtbl LEB count", "[nand_ubi][volume]")
{
    test_geometry g = make_geometry();
    nand_ubi_device_t *dev = attach_with_3_lebs(g);

    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, 0, &vol_bdl) == ESP_OK);
    REQUIRE(vol_bdl != nullptr);

    /* Task 14 replaces Phase 1's implicit whole-device geometry with the explicit
     * LEB count persisted in the volume table. */
    REQUIRE(vol_bdl->geometry.disk_size == 3u * dev->leb_size);
    REQUIRE(vol_bdl->geometry.read_size == g.nand_bdl->geometry.read_size);
    REQUIRE(vol_bdl->geometry.write_size == g.nand_bdl->geometry.write_size);
    REQUIRE(vol_bdl->geometry.erase_size == dev->leb_size);

    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("open_volume: device_flags propagate physical properties, read_only from config", "[nand_ubi][volume]")
{
    test_geometry g = make_geometry();
    format_volume_table(g.nand_bdl, g.page_size, g.peb_size, kImageSeq,
                        g.vid_hdr_offset, g.data_offset, {1});
    format_peb(g.nand_bdl, 2, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset, 0, 1);

    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    cfg.read_only = true;
    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, &cfg, &dev) == ESP_OK);

    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, 0, &vol_bdl) == ESP_OK);

    REQUIRE(vol_bdl->device_flags.read_only == true);
    REQUIRE(vol_bdl->device_flags.erase_before_write == g.nand_bdl->device_flags.erase_before_write);
    REQUIRE(vol_bdl->device_flags.and_type_write == g.nand_bdl->device_flags.and_type_write);

    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("open_volume: absent vol_id returns ESP_ERR_NOT_FOUND", "[nand_ubi][volume]")
{
    test_geometry g = make_geometry();
    nand_ubi_device_t *dev = attach_with_3_lebs(g);

    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, 1, &vol_bdl) == ESP_ERR_NOT_FOUND);
    REQUIRE(vol_bdl == nullptr);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("open_volume: rejects null arguments", "[nand_ubi][volume]")
{
    test_geometry g = make_geometry();
    nand_ubi_device_t *dev = attach_with_3_lebs(g);
    esp_blockdev_handle_t vol_bdl = nullptr;

    REQUIRE(nand_ubi_open_volume(nullptr, 0, &vol_bdl) == ESP_ERR_INVALID_ARG);
    REQUIRE(nand_ubi_open_volume(dev, 0, nullptr) == ESP_ERR_INVALID_ARG);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("open_volume: release() without owns_device leaves the device attached", "[nand_ubi][volume]")
{
    test_geometry g = make_geometry();
    nand_ubi_device_t *dev = attach_with_3_lebs(g);

    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, 0, &vol_bdl) == ESP_OK);
    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);

    /* dev must still be valid: a second open_volume() on it must succeed. */
    esp_blockdev_handle_t vol_bdl2 = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, 0, &vol_bdl2) == ESP_OK);
    REQUIRE(vol_bdl2->ops->release(vol_bdl2) == ESP_OK);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("detach: refused while a volume is open, succeeds after release", "[nand_ubi][volume]")
{
    test_geometry g = make_geometry();
    nand_ubi_device_t *dev = attach_with_3_lebs(g);

    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, 0, &vol_bdl) == ESP_OK);

    REQUIRE(nand_ubi_detach(dev) == ESP_ERR_INVALID_STATE);

    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("volume ioctl speaks LEB/volume-page numbers, never raw physical ones", "[nand_ubi][volume][ioctl]")
{
    test_geometry g = make_geometry();

    /* Physical PEB 7 is bad; LEB 7 of the volume is a perfectly good logical block. */
    uint32_t bad_pnum = 7;
    REQUIRE(g.nand_bdl->ops->ioctl(g.nand_bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &bad_pnum) == ESP_OK);

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(dev, "ioctl", UBI_VID_DYNAMIC, 8, &vol_id) == ESP_OK);
    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, vol_id, &vol_bdl) == ESP_OK);

    esp_blockdev_cmd_arg_status_t arg = { .num = 7, .status = true };
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &arg) == ESP_OK);
    REQUIRE(arg.status == false);
    arg.num = 8;
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &arg) == ESP_ERR_INVALID_ARG);

    /* MARK_BAD_BLOCK must not reach the chip with a LEB number. */
    uint32_t lnum = 3;
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &lnum) == ESP_ERR_NOT_SUPPORTED);
    esp_blockdev_cmd_arg_status_t phys = { .num = 3, .status = true };
    REQUIRE(g.nand_bdl->ops->ioctl(g.nand_bdl, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &phys) == ESP_OK);
    REQUIRE(phys.status == false);

    /* IS_FREE_PAGE is translated through the EBA. */
    std::vector<uint8_t> buf(g.page_size, 0x11);
    REQUIRE(vol_bdl->ops->write(vol_bdl, buf.data(), 0, buf.size()) == ESP_OK);
    uint32_t pages_per_leb = dev->leb_size / g.page_size;
    esp_blockdev_cmd_arg_status_t page = { .num = 0, .status = true };
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_IS_FREE_PAGE, &page) == ESP_OK);
    REQUIRE(page.status == false);
    page = { .num = 1, .status = false };
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_IS_FREE_PAGE, &page) == ESP_OK);
    REQUIRE(page.status == true);
    page = { .num = pages_per_leb, .status = false }; /* LEB 1, unmapped */
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_IS_FREE_PAGE, &page) == ESP_OK);
    REQUIRE(page.status == true);
    page = { .num = 8 * pages_per_leb, .status = false };
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_IS_FREE_PAGE, &page) == ESP_ERR_INVALID_ARG);

    /* ERASE_CONTENTS maps onto the volume's own erase. */
    esp_blockdev_cmd_arg_erase_t erase = { .start_addr = 0, .erase_len = dev->leb_size };
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_ERASE_CONTENTS, &erase) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 0) == UBI_LEB_UNMAPPED);

    /* MARK_DELETED (TRIM) accepts sector-granular ranges and only unmaps whole LEBs. */
    REQUIRE(vol_bdl->ops->write(vol_bdl, buf.data(), 0, buf.size()) == ESP_OK);
    REQUIRE(vol_bdl->ops->write(vol_bdl, buf.data(), dev->leb_size, buf.size()) == ESP_OK);
    esp_blockdev_cmd_arg_erase_t trim = { .start_addr = 512, .erase_len = 2 * dev->leb_size - 512 };
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_MARK_DELETED, &trim) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 0) != UBI_LEB_UNMAPPED);
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 1) == UBI_LEB_UNMAPPED);
    trim = { .start_addr = 512, .erase_len = 512 };
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_MARK_DELETED, &trim) == ESP_OK);

    /* Chip-wide diagnostics still pass through. */
    uint32_t bad_count = 0;
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_GET_BAD_BLOCKS_COUNT, &bad_count) == ESP_OK);
    REQUIRE(bad_count == 1);

    esp_blockdev_cmd_arg_copy_page_t copy = { .src_page = 0, .dst_page = 1 };
    REQUIRE(vol_bdl->ops->ioctl(vol_bdl, ESP_BLOCKDEV_CMD_COPY_PAGE, &copy) == ESP_ERR_NOT_SUPPORTED);

    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("volume sync passes through to the raw NAND BDL", "[nand_ubi][volume]")
{
    test_geometry g = make_geometry();
    nand_ubi_device_t *dev = attach_with_3_lebs(g);

    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, 0, &vol_bdl) == ESP_OK);
    REQUIRE(vol_bdl->ops->sync(vol_bdl) == ESP_OK);

    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("get_blockdev: attach + open_volume(0) in one call", "[nand_ubi][volume]")
{
    test_geometry g = make_geometry();

    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_get_blockdev(g.nand_bdl, &cfg, &vol_bdl) == ESP_OK);
    REQUIRE(vol_bdl != nullptr);

    uint32_t peb_count = (uint32_t)(g.nand_bdl->geometry.disk_size / g.peb_size);
    uint32_t expected_capacity = peb_count - cfg.reserved_pebs - UBI_LAYOUT_VOLUME_EBS;
    uint32_t leb_size = g.peb_size - g.data_offset;
    REQUIRE(vol_bdl->geometry.disk_size == (uint64_t)expected_capacity * leb_size);

    /* release() must also detach the device: no separate nand_ubi_detach() call. */
    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("get_blockdev: rejects null arguments", "[nand_ubi][volume]")
{
    test_geometry g = make_geometry();
    esp_blockdev_handle_t vol_bdl = nullptr;

    REQUIRE(nand_ubi_get_blockdev(nullptr, nullptr, &vol_bdl) == ESP_ERR_INVALID_ARG);
    REQUIRE(nand_ubi_get_blockdev(g.nand_bdl, nullptr, nullptr) == ESP_ERR_INVALID_ARG);

    g.nand_bdl->ops->release(g.nand_bdl);
}
