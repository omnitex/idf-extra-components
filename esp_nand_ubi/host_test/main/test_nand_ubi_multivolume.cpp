/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "esp_nand_ubi.h"
#include "nand_ubi_test_helpers.h"

extern "C" {
#include "nand_ubi_priv.h"
}

using namespace nand_ubi_test;

namespace {

constexpr uint32_t kTestPebCount = 16;
constexpr uint32_t kTestNandBytes = kTestPebCount * 131072u;

const nand_ubi_volume_info_t *find_volume(const nand_ubi_device_t *dev, uint32_t vol_id)
{
    for (uint32_t i = 0; i < dev->vol_count; i++) {
        if (dev->volumes[i].vol_id == vol_id) {
            return &dev->volumes[i];
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("multi-volume: distinct data and EBA slices survive detach and attach", "[nand_ubi][multivolume]")
{
    esp_blockdev_handle_t nand_bdl = make_test_nand(kTestNandBytes);
    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    cfg.reserved_pebs = 2;

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &dev) == ESP_OK);
    REQUIRE(dev->vol_count == 0);

    uint32_t vol_a_id = UINT32_MAX;
    uint32_t vol_b_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(dev, "alpha", UBI_VID_DYNAMIC, 3, &vol_a_id) == ESP_OK);
    REQUIRE(nand_ubi_create_volume(dev, "beta", UBI_VID_DYNAMIC, 2, &vol_b_id) == ESP_OK);
    REQUIRE(vol_a_id == 0);
    REQUIRE(vol_b_id == 1);

    const nand_ubi_volume_info_t *vol_a_info = find_volume(dev, vol_a_id);
    const nand_ubi_volume_info_t *vol_b_info = find_volume(dev, vol_b_id);
    REQUIRE(vol_a_info != nullptr);
    REQUIRE(vol_b_info != nullptr);
    REQUIRE(vol_a_info->leb_count == 3);
    REQUIRE(vol_b_info->leb_count == 2);
    REQUIRE(vol_a_info->eba_offset == 0);
    REQUIRE(vol_b_info->eba_offset == 3);

    esp_blockdev_handle_t vol_a = nullptr;
    esp_blockdev_handle_t vol_b = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, vol_a_id, &vol_a) == ESP_OK);
    REQUIRE(nand_ubi_open_volume(dev, vol_b_id, &vol_b) == ESP_OK);
    REQUIRE(vol_a->geometry.disk_size == 3u * dev->leb_size);
    REQUIRE(vol_b->geometry.disk_size == 2u * dev->leb_size);

    std::vector<uint8_t> alpha(dev->page_size, 0xA5);
    std::vector<uint8_t> beta(dev->page_size, 0x5A);
    REQUIRE(vol_a->ops->write(vol_a, alpha.data(), 0, alpha.size()) == ESP_OK);
    REQUIRE(vol_b->ops->write(vol_b, beta.data(), 0, beta.size()) == ESP_OK);

    int32_t alpha_pnum = nand_ubi_eba_get_pnum(&dev->eba, vol_a_info->eba_offset);
    int32_t beta_pnum = nand_ubi_eba_get_pnum(&dev->eba, vol_b_info->eba_offset);
    REQUIRE(alpha_pnum != UBI_LEB_UNMAPPED);
    REQUIRE(beta_pnum != UBI_LEB_UNMAPPED);
    REQUIRE(alpha_pnum != beta_pnum);

    std::vector<uint8_t> readback(dev->page_size, 0);
    REQUIRE(vol_a->ops->read(vol_a, readback.data(), readback.size(), 0, readback.size()) == ESP_OK);
    REQUIRE(readback == alpha);
    std::fill(readback.begin(), readback.end(), 0);
    REQUIRE(vol_b->ops->read(vol_b, readback.data(), readback.size(), 0, readback.size()) == ESP_OK);
    REQUIRE(readback == beta);
    REQUIRE(readback != alpha);

    REQUIRE(vol_a->ops->release(vol_a) == ESP_OK);
    REQUIRE(vol_b->ops->release(vol_b) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);

    nand_ubi_device_t *reattached = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &reattached) == ESP_OK);
    REQUIRE(reattached->vol_count == 2);
    const nand_ubi_volume_info_t *reattached_a = find_volume(reattached, vol_a_id);
    const nand_ubi_volume_info_t *reattached_b = find_volume(reattached, vol_b_id);
    REQUIRE(reattached_a != nullptr);
    REQUIRE(reattached_b != nullptr);
    REQUIRE(reattached_a->leb_count == 3);
    REQUIRE(reattached_b->leb_count == 2);

    REQUIRE(nand_ubi_open_volume(reattached, vol_a_id, &vol_a) == ESP_OK);
    REQUIRE(nand_ubi_open_volume(reattached, vol_b_id, &vol_b) == ESP_OK);
    std::fill(readback.begin(), readback.end(), 0);
    REQUIRE(vol_a->ops->read(vol_a, readback.data(), readback.size(), 0, readback.size()) == ESP_OK);
    REQUIRE(readback == alpha);
    std::fill(readback.begin(), readback.end(), 0);
    REQUIRE(vol_b->ops->read(vol_b, readback.data(), readback.size(), 0, readback.size()) == ESP_OK);
    REQUIRE(readback == beta);

    REQUIRE(vol_a->ops->release(vol_a) == ESP_OK);
    REQUIRE(vol_b->ops->release(vol_b) == ESP_OK);
    REQUIRE(nand_ubi_detach(reattached) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("multi-volume: creation validates capacity and volume lookup", "[nand_ubi][multivolume]")
{
    esp_blockdev_handle_t nand_bdl = make_test_nand(kTestNandBytes);
    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    cfg.reserved_pebs = 2;
    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &dev) == ESP_OK);

    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(nullptr, "bad", UBI_VID_DYNAMIC, 1, &vol_id) == ESP_ERR_INVALID_ARG);
    REQUIRE(nand_ubi_create_volume(dev, "bad", UBI_VID_DYNAMIC, 1, nullptr) == ESP_ERR_INVALID_ARG);
    REQUIRE(nand_ubi_create_volume(dev, "empty", UBI_VID_DYNAMIC, 0, &vol_id) == ESP_ERR_INVALID_SIZE);
    std::string long_name(UBI_VOL_NAME_MAX + 1u, 'x');
    REQUIRE(nand_ubi_create_volume(dev, long_name.c_str(), UBI_VID_DYNAMIC, 1, &vol_id) == ESP_ERR_INVALID_ARG);
    REQUIRE(nand_ubi_create_volume(dev, "bad-type", 0, 1, &vol_id) == ESP_ERR_INVALID_ARG);

    uint32_t available = dev->peb_count - cfg.reserved_pebs - UBI_LAYOUT_VOLUME_EBS;
    REQUIRE(nand_ubi_create_volume(dev, "full", UBI_VID_DYNAMIC, available, &vol_id) == ESP_OK);
    uint32_t unused_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(dev, "overflow", UBI_VID_DYNAMIC, 1, &unused_id) == ESP_ERR_NO_MEM);

    esp_blockdev_handle_t missing = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, vol_id + 1, &missing) == ESP_ERR_NOT_FOUND);
    REQUIRE(missing == nullptr);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("multi-volume: attach uses the valid vtbl mirror", "[nand_ubi][multivolume]")
{
    esp_blockdev_handle_t nand_bdl = make_test_nand(kTestNandBytes);
    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    cfg.reserved_pebs = 2;
    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &dev) == ESP_OK);
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(dev, "mirror", UBI_VID_DYNAMIC, 2, &vol_id) == ESP_OK);
    uint32_t data_offset = dev->data_offset;
    uint32_t page_size = dev->page_size;
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);

    std::vector<uint8_t> corrupt(page_size, 0);
    REQUIRE(nand_bdl->ops->write(nand_bdl, corrupt.data(), data_offset, page_size) == ESP_OK);

    REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &dev) == ESP_OK);
    REQUIRE(dev->vol_count == 1);
    REQUIRE(dev->volumes[0].vol_id == vol_id);
    REQUIRE(dev->volumes[0].leb_count == 2);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("multi-volume: releasing every volume permits detach without leaks", "[nand_ubi][multivolume][leak]")
{
    for (int iteration = 0; iteration < 8; iteration++) {
        esp_blockdev_handle_t nand_bdl = make_test_nand(kTestNandBytes);
        nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
        cfg.reserved_pebs = 2;
        nand_ubi_device_t *dev = nullptr;
        REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &dev) == ESP_OK);

        uint32_t first_id = UINT32_MAX;
        uint32_t second_id = UINT32_MAX;
        REQUIRE(nand_ubi_create_volume(dev, "first", UBI_VID_DYNAMIC, 2, &first_id) == ESP_OK);
        REQUIRE(nand_ubi_create_volume(dev, "second", UBI_VID_DYNAMIC, 2, &second_id) == ESP_OK);

        esp_blockdev_handle_t first = nullptr;
        esp_blockdev_handle_t second = nullptr;
        REQUIRE(nand_ubi_open_volume(dev, first_id, &first) == ESP_OK);
        REQUIRE(nand_ubi_open_volume(dev, second_id, &second) == ESP_OK);
        REQUIRE(first->ops->release(first) == ESP_OK);
        REQUIRE(second->ops->release(second) == ESP_OK);
        REQUIRE(nand_ubi_detach(dev) == ESP_OK);
        nand_bdl->ops->release(nand_bdl);
    }
}
