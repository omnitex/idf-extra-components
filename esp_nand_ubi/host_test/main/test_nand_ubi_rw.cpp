/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <algorithm>
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

/* A blank chip followed by explicit creation of volume 0. Task 14 supersedes the
 * Phase 1 behavior that allowed opening an implicit volume before it had a vtbl. */
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
    uint32_t peb_count = (uint32_t)(f.nand_bdl->geometry.disk_size / f.nand_bdl->geometry.erase_size);
    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    uint32_t leb_count = peb_count - cfg.reserved_pebs - UBI_LAYOUT_VOLUME_EBS;
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(f.dev, nullptr, UBI_VID_DYNAMIC, leb_count, &vol_id) == ESP_OK);
    REQUIRE(vol_id == 0);
    REQUIRE(nand_ubi_open_volume(f.dev, vol_id, &f.vol_bdl) == ESP_OK);
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
    nand_ubi_device_t *format_dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, nullptr, &format_dev) == ESP_OK);
    uint32_t peb_count = (uint32_t)(nand_bdl->geometry.disk_size / nand_bdl->geometry.erase_size);
    nand_ubi_config_t defaults = NAND_UBI_CONFIG_DEFAULT();
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(format_dev, nullptr, UBI_VID_DYNAMIC,
                                   peb_count - defaults.reserved_pebs - UBI_LAYOUT_VOLUME_EBS,
                                   &vol_id) == ESP_OK);
    REQUIRE(nand_ubi_detach(format_dev) == ESP_OK);

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

TEST_CASE("read: unmapped lnum reads back as ESP_OK 0xFF-filled data", "[nand_ubi][rw]")
{
    blank_fixture f = make_blank_fixture();

    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, buf, sizeof(buf), 0, sizeof(buf)) == ESP_OK);
    for (size_t i = 0; i < sizeof(buf); i++) {
        REQUIRE(buf[i] == 0xFF);
    }

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

/* Every erase bumps and persists the PEB's erase count, and allocation takes the free
 * PEB with the lowest count, so rewriting one LEB walks across the chip instead of
 * wearing one PEB. */
TEST_CASE("erase: repeated rewrites of one LEB spread over PEBs, each erase counted once",
          "[nand_ubi][rw][ec]")
{
    blank_fixture f = make_blank_fixture();

    std::vector<uint8_t> src(f.dev->page_size, 0x55);
    constexpr int kCycles = 5;
    std::vector<int32_t> used;

    auto ec_of = [&](int32_t pnum) {
        std::vector<uint8_t> ec_page(f.dev->page_size, 0);
        REQUIRE(f.nand_bdl->ops->read(f.nand_bdl, ec_page.data(), ec_page.size(),
                                      (uint64_t)pnum * f.dev->peb_size, ec_page.size()) == ESP_OK);
        const auto *ec_hdr = reinterpret_cast<const nand_ubi_ec_hdr_t *>(ec_page.data());
        REQUIRE(nand_ubi_ec_hdr_valid(ec_hdr));
        return nand_ubi_be64(ec_hdr->ec);
    };

    for (int cycle = 0; cycle < kCycles; cycle++) {
        uint64_t mean = f.dev->mean_ec;
        REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);
        int32_t pnum = nand_ubi_eba_get_pnum(&f.dev->eba, 0);
        REQUIRE(pnum != UBI_LEB_UNMAPPED);
        REQUIRE(std::find(used.begin(), used.end(), pnum) == used.end());
        used.push_back(pnum);
        /* First use of a virgin PEB erases it and counts it as average: mean + 1. */
        REQUIRE(ec_of(pnum) == mean + 1);

        REQUIRE(f.vol_bdl->ops->erase(f.vol_bdl, 0, f.dev->leb_size) == ESP_OK);
        REQUIRE(ec_of(pnum) == mean + 2);
    }

    release_fixture(f);
}

TEST_CASE("write: returns ESP_ERR_NO_MEM once the physical free pool is exhausted", "[nand_ubi][rw]")
{
    /* Volume sized for a healthy chip, then blocks go bad after creation: the volume is
     * now over-committed, and writes past the remaining good PEBs must fail cleanly.
     * peb_count is read back from nand_bdl's geometry because the requested file size
     * does not map 1:1 to block count once the emulation's own overhead is included. */
    esp_blockdev_handle_t nand_bdl = make_test_nand(8u * 131072u);
    uint32_t peb_count = (uint32_t)(nand_bdl->geometry.disk_size / nand_bdl->geometry.erase_size);
    REQUIRE(peb_count >= 5);

    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    cfg.reserved_pebs = 0;
    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &dev) == ESP_OK);
    REQUIRE(dev->leb_capacity == peb_count - UBI_LAYOUT_VOLUME_EBS);
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(dev, nullptr, UBI_VID_DYNAMIC,
                                   peb_count - UBI_LAYOUT_VOLUME_EBS, &vol_id) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);

    /* Leave the 2 layout PEBs plus 2 user PEBs good. */
    for (uint32_t pnum = 4; pnum < peb_count; pnum++) {
        REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &pnum) == ESP_OK);
    }
    REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &dev) == ESP_OK);
    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, vol_id, &vol_bdl) == ESP_OK);

    std::vector<uint8_t> buf(dev->page_size, 0);
    REQUIRE(vol_bdl->ops->write(vol_bdl, buf.data(), 0 * dev->leb_size, buf.size()) == ESP_OK);
    REQUIRE(vol_bdl->ops->write(vol_bdl, buf.data(), 1 * dev->leb_size, buf.size()) == ESP_OK);
    REQUIRE(vol_bdl->ops->write(vol_bdl, buf.data(), 2 * dev->leb_size, buf.size()) == ESP_ERR_NO_MEM);

    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("capacity: bad PEBs are subtracted from what volumes may reserve", "[nand_ubi][rw][capacity]")
{
    esp_blockdev_handle_t nand_bdl = make_test_nand();
    uint32_t peb_count = (uint32_t)(nand_bdl->geometry.disk_size / nand_bdl->geometry.erase_size);
    constexpr uint32_t kBad = 6; /* more than the default reserved_pebs (4) */
    for (uint32_t pnum = 10; pnum < 10 + kBad; pnum++) {
        REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &pnum) == ESP_OK);
    }
    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    uint32_t usable = peb_count - kBad - UBI_LAYOUT_VOLUME_EBS;

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &dev) == ESP_OK);
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(dev, nullptr, UBI_VID_DYNAMIC,
                                   usable - cfg.reserved_pebs + 1, &vol_id) == ESP_ERR_NO_MEM);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);

    /* get_blockdev() auto-sizes to what is actually there, and every LEB is writable. */
    esp_blockdev_handle_t vol_bdl = nullptr;
    REQUIRE(nand_ubi_get_blockdev(nand_bdl, &cfg, &vol_bdl) == ESP_OK);
    uint32_t leb_size = (uint32_t)vol_bdl->geometry.erase_size;
    uint32_t lebs = (uint32_t)(vol_bdl->geometry.disk_size / leb_size);
    REQUIRE(lebs == usable - cfg.reserved_pebs);
    std::vector<uint8_t> buf(nand_bdl->geometry.write_size, 0xA1);
    for (uint32_t lnum = 0; lnum < lebs; lnum++) {
        REQUIRE(vol_bdl->ops->write(vol_bdl, buf.data(), (uint64_t)lnum * leb_size, buf.size()) == ESP_OK);
    }

    REQUIRE(vol_bdl->ops->release(vol_bdl) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("round-trip: data written through the UBI layer survives detach + reattach", "[nand_ubi][rw]")
{
    esp_blockdev_handle_t nand_bdl = make_test_nand();

    nand_ubi_device_t *dev1 = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, nullptr, &dev1) == ESP_OK);
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(dev1, nullptr, UBI_VID_DYNAMIC, 1, &vol_id) == ESP_OK);
    esp_blockdev_handle_t vol_bdl1 = nullptr;
    REQUIRE(nand_ubi_open_volume(dev1, vol_id, &vol_bdl1) == ESP_OK);

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

/* Companion to the round-trip test above, covering a case that test doesn't: a PEB that
 * has been erased (and therefore carries a non-zero, freshly-persisted EC per
 * nand_ubi_vol_erase()) but is not currently mapped to any LEB when attach() runs. Its
 * valid-EC-header/blank-VID-header shape must be recognized as an ordinary free PEB (see
 * the attach() comment in src/nand_ubi.c next to page_is_blank()'s VID-header check), not
 * misclassified as an interrupted allocation and shunted into UBI_PEB_ERASE_PENDING --
 * which would otherwise permanently shrink the free pool by one PEB on every reattach
 * that happens to follow an erase. */
TEST_CASE("erase: a freed PEB's persisted EC survives detach + reattach and the PEB stays usable",
          "[nand_ubi][rw][ec]")
{
    esp_blockdev_handle_t nand_bdl = make_test_nand();

    nand_ubi_device_t *dev1 = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, nullptr, &dev1) == ESP_OK);
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(dev1, nullptr, UBI_VID_DYNAMIC, 1, &vol_id) == ESP_OK);
    esp_blockdev_handle_t vol_bdl1 = nullptr;
    REQUIRE(nand_ubi_open_volume(dev1, vol_id, &vol_bdl1) == ESP_OK);

    std::vector<uint8_t> src(nand_bdl->geometry.write_size, 0x77);
    REQUIRE(vol_bdl1->ops->write(vol_bdl1, src.data(), 0, src.size()) == ESP_OK);
    int32_t pnum = nand_ubi_eba_get_pnum(&dev1->eba, 0);
    REQUIRE(pnum != UBI_LEB_UNMAPPED);

    /* Erase (not another write): the PEB goes back to the free pool, unmapped from any
     * LEB, with a fresh EC header: ec=1 from the erase at first allocation, +1 now. */
    REQUIRE(vol_bdl1->ops->erase(vol_bdl1, 0, dev1->leb_size) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&dev1->eba, 0) == UBI_LEB_UNMAPPED);

    REQUIRE(vol_bdl1->ops->release(vol_bdl1) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev1) == ESP_OK);

    nand_ubi_device_t *dev2 = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, nullptr, &dev2) == ESP_OK);
    REQUIRE(dev2->leb_count == 1); /* volume 0's capacity as created, unaffected by the erase */
    REQUIRE(nand_ubi_eba_get_pnum(&dev2->eba, 0) == UBI_LEB_UNMAPPED); /* but LEB 0 itself is unmapped */

    /* The raw on-flash EC survived the round-trip untouched by attach() itself. */
    std::vector<uint8_t> ec_page(dev2->page_size, 0);
    REQUIRE(nand_bdl->ops->read(nand_bdl, ec_page.data(), ec_page.size(),
                                (uint64_t)pnum * dev2->peb_size, ec_page.size()) == ESP_OK);
    const nand_ubi_ec_hdr_t *ec_hdr = reinterpret_cast<const nand_ubi_ec_hdr_t *>(ec_page.data());
    REQUIRE(nand_ubi_ec_hdr_valid(ec_hdr));
    REQUIRE(nand_ubi_be64(ec_hdr->ec) == 2);

    /* attach() classified the PEB as FREE (not ERASE_PENDING). A new write goes to a
     * less-erased PEB, and the freed one keeps its history. Volume 0 already exists in
     * the on-flash vtbl from dev1's session, so reopen it. */
    REQUIRE(nand_ubi_eba_peb_is_free(&dev2->eba, (uint32_t)pnum));
    esp_blockdev_handle_t vol_bdl2 = nullptr;
    REQUIRE(nand_ubi_open_volume(dev2, vol_id, &vol_bdl2) == ESP_OK);
    uint64_t mean_before = dev2->mean_ec;
    REQUIRE(vol_bdl2->ops->write(vol_bdl2, src.data(), 0, src.size()) == ESP_OK);
    int32_t new_pnum = nand_ubi_eba_get_pnum(&dev2->eba, 0);
    REQUIRE(new_pnum != pnum);
    /* A PEB without an EC header is stamped mean_ec + 1 when first erased. */
    REQUIRE(peb_ec_on_flash(nand_bdl, (uint32_t)new_pnum, dev2->page_size, dev2->peb_size) == mean_before + 1);

    REQUIRE(nand_bdl->ops->read(nand_bdl, ec_page.data(), ec_page.size(),
                                (uint64_t)pnum * dev2->peb_size, ec_page.size()) == ESP_OK);
    REQUIRE(nand_ubi_ec_hdr_valid(ec_hdr));
    REQUIRE(nand_ubi_be64(ec_hdr->ec) == 2);

    REQUIRE(vol_bdl2->ops->release(vol_bdl2) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev2) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}

namespace {

struct faulty_fixture {
    faulty_nand *nand;
    nand_ubi_device_t *dev;
    esp_blockdev_handle_t vol_bdl;
};

faulty_fixture make_faulty_fixture()
{
    faulty_fixture f {};
    f.nand = make_faulty_nand();
    REQUIRE(nand_ubi_attach(&f.nand->bdl, nullptr, &f.dev) == ESP_OK);
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(f.dev, nullptr, UBI_VID_DYNAMIC, 8, &vol_id) == ESP_OK);
    REQUIRE(nand_ubi_open_volume(f.dev, vol_id, &f.vol_bdl) == ESP_OK);
    return f;
}

void reattach(faulty_fixture &f)
{
    REQUIRE(f.vol_bdl->ops->release(f.vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(f.dev) == ESP_OK);
    REQUIRE(nand_ubi_attach(&f.nand->bdl, nullptr, &f.dev) == ESP_OK);
    REQUIRE(nand_ubi_open_volume(f.dev, 0, &f.vol_bdl) == ESP_OK);
}

void release_faulty_fixture(faulty_fixture &f)
{
    REQUIRE(f.vol_bdl->ops->release(f.vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(f.dev) == ESP_OK);
    REQUIRE(f.nand->bdl.ops->release(&f.nand->bdl) == ESP_OK);
}

bool chip_says_bad(faulty_nand *nand, uint32_t pnum)
{
    esp_blockdev_cmd_arg_status_t arg = {pnum, false};
    REQUIRE(nand->inner->ops->ioctl(nand->inner, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &arg) == ESP_OK);
    return arg.status;
}

} // namespace

TEST_CASE("erase: failed physical erase retires the PEB and the old data does not come back",
          "[nand_ubi][rw][fault]")
{
    faulty_fixture f = make_faulty_fixture();
    std::vector<uint8_t> src(f.dev->page_size, 0x3C);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);
    int32_t pnum = nand_ubi_eba_get_pnum(&f.dev->eba, 0);

    f.nand->fail_peb((uint32_t)pnum);
    f.nand->fail_erases = 1;
    REQUIRE(f.vol_bdl->ops->erase(f.vol_bdl, 0, f.dev->leb_size) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == UBI_LEB_UNMAPPED);
    REQUIRE(peb_state_of(f.dev->eba, (uint32_t)pnum) == UBI_PEB_BAD);
    REQUIRE(chip_says_bad(f.nand, (uint32_t)pnum));

    /* The PEB still physically holds the old VID header + data; reattach must not remap it. */
    reattach(f);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == UBI_LEB_UNMAPPED);
    std::vector<uint8_t> dst(src.size(), 0);
    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, dst.data(), dst.size(), 0, dst.size()) == ESP_OK);
    REQUIRE(dst == std::vector<uint8_t>(src.size(), 0xFF));

    release_faulty_fixture(f);
}

TEST_CASE("erase: failed post-erase EC header write retires the PEB, LEB stays unmapped",
          "[nand_ubi][rw][fault]")
{
    faulty_fixture f = make_faulty_fixture();
    std::vector<uint8_t> src(f.dev->page_size, 0x3C);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);
    int32_t pnum = nand_ubi_eba_get_pnum(&f.dev->eba, 0);

    f.nand->fail_peb((uint32_t)pnum);
    f.nand->fail_writes = 1;
    f.nand->program_before_fail = true;
    REQUIRE(f.vol_bdl->ops->erase(f.vol_bdl, 0, f.dev->leb_size) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == UBI_LEB_UNMAPPED);
    REQUIRE(peb_state_of(f.dev->eba, (uint32_t)pnum) == UBI_PEB_BAD);

    /* The volume keeps working on another PEB. */
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) != pnum);

    release_faulty_fixture(f);
}

TEST_CASE("write: failed VID header program retires the PEB and allocation moves to the next one",
          "[nand_ubi][rw][fault]")
{
    faulty_fixture f = make_faulty_fixture();
    int32_t doomed = next_alloc_peb(f.dev->eba);
    REQUIRE(doomed >= 0);

    f.nand->fail_peb((uint32_t)doomed);
    f.nand->fail_writes = 1;
    f.nand->program_before_fail = true;
    std::vector<uint8_t> src(f.dev->page_size, 0x5D);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);

    int32_t pnum = nand_ubi_eba_get_pnum(&f.dev->eba, 0);
    REQUIRE(pnum != UBI_LEB_UNMAPPED);
    REQUIRE(pnum != doomed);
    REQUIRE(peb_state_of(f.dev->eba, (uint32_t)doomed) == UBI_PEB_BAD);
    REQUIRE(chip_says_bad(f.nand, (uint32_t)doomed));

    /* The half-programmed PEB never shows up as a mapping after reattach. */
    reattach(f);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == pnum);
    std::vector<uint8_t> dst(src.size(), 0);
    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, dst.data(), dst.size(), 0, dst.size()) == ESP_OK);
    REQUIRE(dst == src);

    release_faulty_fixture(f);
}

TEST_CASE("write: persistent header write failures give up after a bounded number of PEBs",
          "[nand_ubi][rw][fault]")
{
    faulty_fixture f = make_faulty_fixture();
    f.nand->fail_lo = 0;
    f.nand->fail_hi = UINT64_MAX;
    f.nand->fail_writes = 1000;

    std::vector<uint8_t> src(f.dev->page_size, 0x5D);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) != ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == UBI_LEB_UNMAPPED);

    uint32_t bad = 0;
    for (uint32_t pnum = 0; pnum < f.dev->peb_count; pnum++) {
        bad += peb_state_of(f.dev->eba, pnum) == UBI_PEB_BAD;
    }
    REQUIRE(bad == 3);

    f.nand->fail_writes = 0;
    release_faulty_fixture(f);
}

TEST_CASE("layout: a volume-table copy whose PEB fails to program moves to another PEB",
          "[nand_ubi][rw][fault][layout]")
{
    faulty_fixture f = make_faulty_fixture();
    int32_t old_pnum = f.dev->vtbl_pnum[0];
    REQUIRE(old_pnum >= 0);

    f.nand->fail_peb((uint32_t)old_pnum);
    f.nand->fail_writes = 1;
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(f.dev, "second", UBI_VID_DYNAMIC, 1, &vol_id) == ESP_OK);
    REQUIRE(f.dev->vtbl_pnum[0] != old_pnum);
    REQUIRE(peb_state_of(f.dev->eba, (uint32_t)old_pnum) == UBI_PEB_BAD);

    reattach(f);
    REQUIRE(f.dev->vol_count == 2);
    REQUIRE(f.dev->vtbl_pnum[0] >= 0);
    REQUIRE(f.dev->vtbl_pnum[0] != old_pnum);

    release_faulty_fixture(f);
}

namespace {

void reboot(faulty_fixture &f)
{
    f.nand->dead = false;
    f.nand->fail_writes = 0;
    f.nand->fail_erases = 0;
    f.nand->pass_writes = 0;
    f.nand->partial_bytes = 0;
    f.nand->die_on_fail = false;
    f.nand->die_after_erase = false;
    reattach(f);
}

uint64_t layout_sqnum(faulty_fixture &f, uint32_t copy)
{
    std::vector<uint8_t> page(f.dev->page_size, 0);
    uint64_t addr = (uint64_t)f.dev->vtbl_pnum[copy] * f.dev->peb_size + f.dev->vid_hdr_offset;
    REQUIRE(f.nand->inner->ops->read(f.nand->inner, page.data(), page.size(), addr, page.size()) == ESP_OK);
    return nand_ubi_be64(reinterpret_cast<const nand_ubi_vid_hdr_t *>(page.data())->sqnum);
}

} // namespace

TEST_CASE("power cut: a volume-table copy cut off mid-table is rejected and repaired",
          "[nand_ubi][rw][layout][powercut]")
{
    /* Sparse table (vol_ids 0 and 12, as a ubinize image may have). The newer copy 0 was
     * cut after its first table page: every record it holds is intact or blank, so only
     * the VID data CRC shows that vol 12's record is missing. */
    esp_blockdev_handle_t nand_bdl = make_test_nand();
    uint32_t page_size = (uint32_t)nand_bdl->geometry.read_size;
    uint32_t peb_size = (uint32_t)nand_bdl->geometry.erase_size;
    uint32_t vid_off = page_size, data_off = 2 * page_size;
    std::vector<uint32_t> lebs(13, 0);
    lebs[0] = 1;
    lebs[12] = 1;
    REQUIRE(12 * sizeof(nand_ubi_vtbl_record_t) >= page_size);
    format_volume_table(nand_bdl, page_size, peb_size, 0, vid_off, data_off, lebs);

    uint32_t table_bytes = 0;
    std::vector<uint8_t> table = build_vtbl(page_size, peb_size - data_off, lebs, &table_bytes);
    REQUIRE(nand_bdl->ops->erase(nand_bdl, 0, peb_size) == ESP_OK);
    std::vector<uint8_t> page(page_size, 0xFF);
    nand_ubi_ec_hdr_t ec {};
    fill_ec_hdr(&ec, 0, vid_off, data_off);
    memcpy(page.data(), &ec, sizeof(ec));
    REQUIRE(nand_bdl->ops->write(nand_bdl, page.data(), 0, page_size) == ESP_OK);
    std::fill(page.begin(), page.end(), 0xFFu);
    nand_ubi_vid_hdr_t vid {};
    fill_vid_hdr(&vid, UBI_LAYOUT_VOL_ID, 0, 10, 0, table_bytes, nand_ubi_crc32(table.data(), table_bytes));
    memcpy(page.data(), &vid, sizeof(vid));
    REQUIRE(nand_bdl->ops->write(nand_bdl, page.data(), vid_off, page_size) == ESP_OK);
    REQUIRE(nand_bdl->ops->write(nand_bdl, table.data(), data_off, page_size) == ESP_OK);

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, nullptr, &dev) == ESP_OK);
    REQUIRE(dev->vol_count == 2);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);

    /* The read-write attach above rewrote the truncated copy. */
    nand_ubi_config_t ro = NAND_UBI_CONFIG_DEFAULT();
    ro.read_only = true;
    REQUIRE(nand_ubi_attach(nand_bdl, &ro, &dev) == ESP_OK);
    REQUIRE(dev->vtbl_copy_good[0]);
    REQUIRE(dev->vtbl_copy_good[1]);
    REQUIRE(dev->vol_count == 2);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    nand_bdl->ops->release(nand_bdl);
}

TEST_CASE("power cut: the only good volume-table copy is not erased first",
          "[nand_ubi][rw][fault][layout][powercut]")
{
    faulty_fixture f = make_faulty_fixture();
    std::vector<uint8_t> src(f.dev->page_size, 0x6B);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);

    /* Copy 1 is damaged and was not repaired (as after an attach whose repair failed). */
    uint32_t copy1_pnum = (uint32_t)f.dev->vtbl_pnum[1];
    REQUIRE(f.nand->inner->ops->erase(f.nand->inner, (uint64_t)copy1_pnum * f.dev->peb_size,
                                      f.dev->peb_size) == ESP_OK);
    f.dev->vtbl_copy_good[1] = false;

    /* Power dies right after copy 0's PEB is erased. */
    f.nand->fail_peb((uint32_t)f.dev->vtbl_pnum[0]);
    f.nand->die_after_erase = true;
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(f.dev, "second", UBI_VID_DYNAMIC, 1, &vol_id) != ESP_OK);

    /* Copy 1 was rewritten before copy 0 was touched, so a valid table survives. */
    reboot(f);
    REQUIRE(f.dev->vol_count == 2);
    std::vector<uint8_t> dst(src.size(), 0);
    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, dst.data(), dst.size(), 0, dst.size()) == ESP_OK);
    REQUIRE(dst == src);

    release_faulty_fixture(f);
}

TEST_CASE("layout: attach counts volume-table sqnums, so rewrites always win", "[nand_ubi][rw][layout]")
{
    faulty_fixture f = make_faulty_fixture();
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(f.dev, nullptr, UBI_VID_DYNAMIC, 1, &vol_id) == ESP_OK);
    reattach(f);
    uint64_t before = std::max(layout_sqnum(f, 0), layout_sqnum(f, 1));
    REQUIRE(f.dev->global_sqnum >= before);

    REQUIRE(nand_ubi_create_volume(f.dev, nullptr, UBI_VID_DYNAMIC, 1, &vol_id) == ESP_OK);
    REQUIRE(layout_sqnum(f, 0) > before);
    REQUIRE(layout_sqnum(f, 1) > before);

    release_faulty_fixture(f);
}

TEST_CASE("torn erase: a blank PEB is erased before its first use", "[nand_ubi][rw][fault][torn]")
{
    /* A cut during an erase can leave a PEB that reads as all 0xFF but loses bits when
     * programmed. Attach cannot tell it from a factory-fresh PEB, so allocation must
     * erase any PEB without a valid EC header before writing to it. */
    faulty_fixture f = make_faulty_fixture();
    int32_t next = next_alloc_peb(f.dev->eba);
    REQUIRE(next >= 0);
    f.nand->torn_pebs.insert((uint32_t)next);

    std::vector<uint8_t> src(f.dev->page_size, 0xFF);
    for (size_t i = 0; i < src.size(); i++) {
        src[i] = (uint8_t)(i * 7 + 1);
    }
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == next);
    REQUIRE(f.nand->torn_pebs.count((uint32_t)next) == 0);

    reattach(f);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == next);
    std::vector<uint8_t> dst(src.size(), 0);
    REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, dst.data(), dst.size(), 0, dst.size()) == ESP_OK);
    REQUIRE(dst == src);

    release_faulty_fixture(f);
}

TEST_CASE("torn erase: a cut mid-erase leaves a PEB that reattach and reuse handle cleanly",
          "[nand_ubi][rw][fault][torn][powercut]")
{
    faulty_fixture f = make_faulty_fixture();
    std::vector<uint8_t> src(f.dev->page_size, 0x3A);
    REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);
    int32_t pnum = nand_ubi_eba_get_pnum(&f.dev->eba, 0);

    /* The LEB erase is cut part-way: the PEB reads blank, has no EC header, and is torn. */
    f.nand->fail_peb((uint32_t)pnum);
    f.nand->torn_erases = 1;
    f.nand->die_on_fail = true;
    REQUIRE(f.vol_bdl->ops->erase(f.vol_bdl, 0, f.dev->leb_size) != ESP_OK);
    f.nand->dead = false;
    f.nand->die_on_fail = false;
    REQUIRE(f.nand->torn_pebs.count((uint32_t)pnum) == 1);

    /* After reboot the torn PEB is a free, blank PEB. Writing to the volume must not
     * hand out a PEB that silently corrupts data, wherever the allocator lands. */
    reattach(f);
    REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, 0) == UBI_LEB_UNMAPPED);
    for (uint32_t lnum = 0; lnum < 8; lnum++) {
        REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), (uint64_t)lnum * f.dev->leb_size, src.size()) == ESP_OK);
    }
    reattach(f);
    for (uint32_t lnum = 0; lnum < 8; lnum++) {
        std::vector<uint8_t> dst(src.size(), 0);
        REQUIRE(f.vol_bdl->ops->read(f.vol_bdl, dst.data(), dst.size(), (uint64_t)lnum * f.dev->leb_size,
                                     dst.size()) == ESP_OK);
        REQUIRE(dst == src);
    }

    release_faulty_fixture(f);
}

TEST_CASE("alloc: after reattach the least-erased free PEB is used first", "[nand_ubi][rw][ec]")
{
    /* PEBs 2..2+k-1 carry EC headers with decreasing counts; every other PEB is marked bad.
     * With no more than k PEBs free, the sampler sees all of them, so allocation must take
     * them strictly in order of erase count. */
    constexpr uint32_t k = CONFIG_ESP_NAND_UBI_ALLOC_SAMPLES;
    faulty_fixture f = make_faulty_fixture();
    REQUIRE(f.vol_bdl->ops->release(f.vol_bdl) == ESP_OK);
    REQUIRE(nand_ubi_detach(f.dev) == ESP_OK);
    uint32_t page = (uint32_t)f.nand->inner->geometry.read_size;
    uint32_t peb = (uint32_t)f.nand->inner->geometry.erase_size;
    uint32_t peb_count = (uint32_t)(f.nand->inner->geometry.disk_size / peb);
    std::vector<uint8_t> hdr(page, 0xFF);
    for (uint32_t pnum = 2; pnum < 2 + k; pnum++) {
        REQUIRE(f.nand->inner->ops->erase(f.nand->inner, (uint64_t)pnum * peb, peb) == ESP_OK);
        nand_ubi_ec_hdr_t ec {};
        fill_ec_hdr(&ec, 0, page, 2 * page);
        ec.ec = to_be64(100 - pnum * 10); /* higher pnum -> fewer erases */
        ec.hdr_crc = to_be32(nand_ubi_crc32(&ec, UBI_EC_HDR_SIZE_CRC));
        memcpy(hdr.data(), &ec, sizeof(ec));
        REQUIRE(f.nand->inner->ops->write(f.nand->inner, hdr.data(), (uint64_t)pnum * peb, page) == ESP_OK);
    }
    for (uint32_t pnum = 2 + k; pnum < peb_count; pnum++) {
        REQUIRE(f.nand->inner->ops->ioctl(f.nand->inner, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &pnum) == ESP_OK);
    }
    REQUIRE(nand_ubi_attach(&f.nand->bdl, nullptr, &f.dev) == ESP_OK);
    REQUIRE(nand_ubi_open_volume(f.dev, 0, &f.vol_bdl) == ESP_OK);

    std::vector<uint8_t> src(page, 0x21);
    uint32_t lebs = std::min<uint32_t>(k, (uint32_t)(f.vol_bdl->geometry.disk_size / f.dev->leb_size));
    for (uint32_t lnum = 0; lnum < lebs; lnum++) {
        REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), (uint64_t)lnum * f.dev->leb_size, src.size()) == ESP_OK);
        REQUIRE(nand_ubi_eba_get_pnum(&f.dev->eba, lnum) == (int32_t)(2 + k - 1 - lnum));
    }

    release_faulty_fixture(f);
}
