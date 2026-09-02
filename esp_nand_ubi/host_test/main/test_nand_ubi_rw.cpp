/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "esp_nand_ubi.h"
#include "nand_ubi_test_helpers.h"

extern "C" {
#include "nand_ubi_priv.h"
}

using namespace nand_ubi_test;

namespace {

/* A blank (unformatted) chip, attached with defaults: leb_count == 0, but leb_capacity
 * covers peb_count - reserved_pebs, so lnum 0.. is writable per the Task 6 capacity fix. */
struct blank_fixture {
    esp_blockdev_handle_t nand_bdl;
    nand_ubi_device_t *dev;
    esp_blockdev_handle_t vol_bdl;
};

blank_fixture make_blank_fixture(uint32_t file_bytes = 50u * 1024u * 1024u)
{
    blank_fixture f {};
    f.nand_bdl = make_test_nand(file_bytes);
    REQUIRE(nand_ubi_attach(f.nand_bdl, nullptr, &f.dev) == ESP_OK);
    REQUIRE(f.dev->leb_count == 0);
    REQUIRE(nand_ubi_open_volume(f.dev, 0, &f.vol_bdl) == ESP_OK);
    return f;
}

void release_fixture(blank_fixture &f)
{
    REQUIRE(f.vol_bdl->ops->release(f.vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(f.dev) == ESP_OK);
    f.nand_bdl->ops->release(f.nand_bdl);
}

} // namespace

TEST_CASE("write+read: first write to a blank chip allocates a PEB and round-trips data", "[nand_ubi][rw]")
{
    blank_fixture f = make_blank_fixture();

    std::vector<uint8_t> src(f.dev->page_size, 0x42);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) != UBI_LEB_UNMAPPED);

    std::vector<uint8_t> dst(f.dev->page_size, 0);
    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, dst.data(), dst.size(), 0, dst.size()) == ESP_OK);
    REQUIRE(dst == src);

    release_fixture(f);
}

TEST_CASE("write: second write to an already-mapped lnum reuses the same pnum", "[nand_ubi][rw]")
{
    blank_fixture f = make_blank_fixture();
    uint32_t page_size = f.dev->page_size;

    std::vector<uint8_t> src1(page_size, 0x11);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src1.data(), 0, src1.size()) == ESP_OK);
    int32_t pnum_after_first = nand_ubi_eba_get_pnum(&f.dev->eba, 0);

    std::vector<uint8_t> src2(page_size, 0x22);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src2.data(), page_size, src2.size()) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == pnum_after_first);

    std::vector<uint8_t> dst(page_size, 0);
    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, dst.data(), dst.size(), page_size, dst.size()) == ESP_OK);
    REQUIRE(dst == src2);

    release_fixture(f);
}

TEST_CASE("write: read_only volume rejects writes and erases", "[nand_ubi][rw]")
{
    esp_blockdev_handle_t nand_bdl = make_test_nand();
    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    cfg.read_only = true;
    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_get_blockdev(nand_bdl, &cfg, &vol_bdl) == ESP_OK);

    uint8_t buf[16] = {0};
    REQUIRE(vol_bdl->ops->write(vol_bdl, buf, 0, sizeof(buf)) == ESP_ERR_NOT_SUPPORTED);
    REQUIRE(vol_bdl->ops->erase(vol_bdl, 0, vol_bdl->geometry.erase_size) == ESP_ERR_NOT_SUPPORTED);

    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("write: rejects data crossing an LEB boundary", "[nand_ubi][rw]")
{
    blank_fixture f = make_blank_fixture();

    uint8_t buf[16] = {0};
    uint64_t leb_size = f.dev->leb_size;
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, buf, leb_size - 8, sizeof(buf)) == ESP_ERR_INVALID_ARG);

    release_fixture(f);
}

TEST_CASE("write: rejects writes beyond the volume's capacity", "[nand_ubi][rw]")
{
    blank_fixture f = make_blank_fixture();

    uint8_t buf[16] = {0};
    uint64_t past_capacity = f.vol_bdl->geometry.disk_size;
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, buf, past_capacity, sizeof(buf)) == ESP_ERR_INVALID_SIZE);

    release_fixture(f);
}

TEST_CASE("read: unmapped lnum returns ESP_ERR_NOT_FOUND", "[nand_ubi][rw]")
{
    blank_fixture f = make_blank_fixture();

    uint8_t buf[16] = {0};
    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, buf, sizeof(buf), 0, sizeof(buf)) == ESP_ERR_NOT_FOUND);

    release_fixture(f);
}

TEST_CASE("read: rejects null dst_buf, undersized dst_buf_size, and LEB-boundary crossing", "[nand_ubi][rw]")
{
    blank_fixture f = make_blank_fixture();
    uint8_t buf[16] = {0};

    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, nullptr, sizeof(buf), 0, sizeof(buf)) == ESP_ERR_INVALID_ARG);
    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, buf, 4, 0, sizeof(buf)) == ESP_ERR_INVALID_ARG);

    uint64_t leb_size = f.dev->leb_size;
    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, buf, sizeof(buf), leb_size - 8, sizeof(buf)) == ESP_ERR_INVALID_ARG);

    release_fixture(f);
}

TEST_CASE("erase: unmaps the lnum and the freed PEB is reused by a subsequent write", "[nand_ubi][rw]")
{
    blank_fixture f = make_blank_fixture();
    uint32_t page_size = f.dev->page_size;

    std::vector<uint8_t> src(page_size, 0x77);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);
    int32_t pnum = nand_ubi_eba_get_pnum(&f.dev->eba, 0);
    REQUIRE(pnum != UBI_LEB_UNMAPPED);

    REQUIRE(f.vol_bdl->ops->erase(f.vol_bdl, 0, f.dev->leb_size) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == UBI_LEB_UNMAPPED);
    REQUIRE(nand_ubi_eba_peb_is_free(&f.dev->eba, (uint32_t)pnum));

    /* A fresh write to the same lnum must succeed again (the freed PEB, or another
     * free one, is picked up cleanly) with independent data. */
    std::vector<uint8_t> src2(page_size, 0x88);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src2.data(), 0, src2.size()) == ESP_OK);
    std::vector<uint8_t> dst(page_size, 0);
    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, dst.data(), dst.size(), 0, dst.size()) == ESP_OK);
    REQUIRE(dst == src2);

    release_fixture(f);
}

TEST_CASE("erase: idempotent no-op on an already-unmapped lnum", "[nand_ubi][rw]")
{
    blank_fixture f = make_blank_fixture();

    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == UBI_LEB_UNMAPPED);
    REQUIRE(f.vol_bdl->ops->erase(f.vol_bdl, 0, f.dev->leb_size) == ESP_OK);

    release_fixture(f);
}

TEST_CASE("erase: rejects misaligned start_addr and erase_len", "[nand_ubi][rw]")
{
    blank_fixture f = make_blank_fixture();

    uint64_t leb_size = f.dev->leb_size;
    REQUIRE(f.vol_bdl->ops->erase(f.vol_bdl, 1, leb_size) == ESP_ERR_INVALID_SIZE);
    REQUIRE(f.vol_bdl->ops->erase(f.vol_bdl, 0, leb_size - 1) == ESP_ERR_INVALID_SIZE);
    REQUIRE(f.vol_bdl->ops->erase(f.vol_bdl, 0, 0) == ESP_ERR_INVALID_SIZE);

    release_fixture(f);
}

TEST_CASE("erase: a single call spanning multiple LEBs frees all of them", "[nand_ubi][rw]")
{
    blank_fixture f = make_blank_fixture();

    std::vector<uint8_t> src(f.dev->page_size, 0x33);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), f.dev->leb_size, src.size()) == ESP_OK);

    REQUIRE(f.vol_bdl->ops->erase(f.vol_bdl, 0, (uint64_t)2 * f.dev->leb_size) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == UBI_LEB_UNMAPPED);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 1) == UBI_LEB_UNMAPPED);

    release_fixture(f);
}

TEST_CASE("write: returns ESP_ERR_NO_MEM once the physical free pool is exhausted", "[nand_ubi][rw]")
{
    /* A handful of PEBs; mark all but 2 bad so only 2 physical PEBs are ever free,
     * while reserved_pebs=0 keeps nominal capacity at every lnum so the capacity
     * bounds check (which fires first in write()) does not mask the free-pool
     * exhaustion path this test targets. peb_count is read back from nand_bdl's own
     * geometry rather than assumed from the requested file size, which does not
     * translate 1:1 into block count once the emulation's own overhead is included. */
    esp_blockdev_handle_t nand_bdl = make_test_nand(8u * 131072u);
    uint32_t peb_count = (uint32_t)(nand_bdl->geometry.disk_size / nand_bdl->geometry.erase_size);
    REQUIRE(peb_count >= 3);
    for (uint32_t pnum = 2; pnum < peb_count; pnum++) {
        REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &pnum) == ESP_OK);
    }

    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    cfg.reserved_pebs = 0;
    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &dev) == ESP_OK);
    REQUIRE(dev->leb_capacity == peb_count);

    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, 0, &vol_bdl) == ESP_OK);

    std::vector<uint8_t> buf(dev->page_size, 0);
    REQUIRE(vol_bdl->ops->write(vol_bdl, buf.data(), 0 * dev->leb_size, buf.size()) == ESP_OK);
    REQUIRE(vol_bdl->ops->write(vol_bdl, buf.data(), 1 * dev->leb_size, buf.size()) == ESP_OK);
    REQUIRE(vol_bdl->ops->write(vol_bdl, buf.data(), 2 * dev->leb_size, buf.size()) == ESP_ERR_NO_MEM);

    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("round-trip: data written through the UBI layer survives detach + reattach", "[nand_ubi][rw]")
{
    esp_blockdev_handle_t nand_bdl = make_test_nand();

    nand_ubi_device_t *dev1 = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, nullptr, &dev1) == ESP_OK);
    esp_blockdev_handle_t vol_bdl1 = nullptr;
    REQUIRE(nand_ubi_open_volume(dev1, 0, &vol_bdl1) == ESP_OK);

    std::vector<uint8_t> src(nand_bdl->geometry.write_size, 0x5A);
    REQUIRE(vol_bdl1->ops->write(vol_bdl1, src.data(), 0, src.size()) == ESP_OK);

    REQUIRE(vol_bdl1->ops->release(vol_bdl1) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev1) == ESP_OK);

    /* Fresh attach on the same underlying nand_bdl: proves the EC/VID headers written by
     * nand_ubi_vol_alloc_peb() are byte-correct against attach()'s own validation, not
     * just self-consistent within a single already-attached session. */
    nand_ubi_device_t *dev2 = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, nullptr, &dev2) == ESP_OK);
    REQUIRE(dev2->leb_count == 1);
    REQUIRE(nand_ubi_eba_get_pnum(&dev2->eba, 0) != UBI_LEB_UNMAPPED);

    esp_blockdev_handle_t vol_bdl2 = nullptr;
    REQUIRE(nand_ubi_open_volume(dev2, 0, &vol_bdl2) == ESP_OK);
    std::vector<uint8_t> dst(src.size(), 0);
    REQUIRE(vol_bdl2->ops->read(vol_bdl2, dst.data(), dst.size(), 0, dst.size()) == ESP_OK);
    REQUIRE(dst == src);

    REQUIRE(vol_bdl2->ops->release(vol_bdl2) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev2) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}
