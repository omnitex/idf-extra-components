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

/* Regression test for the *current, intentional* Phase-1 gap: erase-counter tracking.
 * Real Linux UBI bumps and persists a PEB's EC every time it is physically erased, and
 * uses low-EC PEBs preferentially so wear spreads evenly (that's "wear leveling"). This
 * component's on-flash EC header field exists and round-trips correctly (readable via
 * examples/nand_ubi_metadata_dump), but fill_ec_header() (src/nand_ubi.c) hardcodes
 * ec=0 on every write and nand_ubi_eba_find_free_peb() does an EC-blind linear scan --
 * there is no code path anywhere that reads an old EC and increments it.
 *
 * This test erases and rewrites the same LEB several times and asserts the underlying
 * PEB's on-flash EC stays 0 across every cycle. It exists so that the day EC tracking
 * lands (Phase 4 background WL, see docs/plans/2026-07-09-esp-nand-ubi-mvp.md), this
 * test fails loudly and gets flipped into a real assertion (ec == cycle count) instead
 * of silently going stale as a passive gap nobody notices regressed. */
TEST_CASE("erase: EC is NOT incremented across repeated erase/rewrite cycles (Phase 1 has no WL yet)",
          "[nand_ubi][rw][ec]")
{
    blank_fixture f = make_blank_fixture();

    std::vector<uint8_t> src(f.dev->page_size, 0x55);
    constexpr int kCycles = 5;
    int32_t first_pnum = -1;

    for (int cycle = 0; cycle < kCycles; cycle++) {
        REQUIRE(f.vol_bdl->ops->write(f.vol_bdl, src.data(), 0, src.size()) == ESP_OK);

        int32_t pnum = nand_ubi_eba_get_pnum(&f.dev->eba, 0);
        REQUIRE(pnum != UBI_LEB_UNMAPPED);
        if (cycle == 0) {
            first_pnum = pnum;
        } else {
            /* find_free_peb()'s linear scan keeps handing back the same lowest-numbered
             * free PEB once nothing else churns the pool, so this is expected to hold in
             * this single-LEB test -- not a hard guarantee of the API. */
            REQUIRE(pnum == first_pnum);
        }

        std::vector<uint8_t> ec_page(f.dev->page_size, 0);
        REQUIRE(f.nand_bdl->ops->read(f.nand_bdl, ec_page.data(), ec_page.size(),
                                       (uint64_t)pnum * f.dev->peb_size, ec_page.size()) == ESP_OK);
        const nand_ubi_ec_hdr_t *ec_hdr = reinterpret_cast<const nand_ubi_ec_hdr_t *>(ec_page.data());
        REQUIRE(nand_ubi_ec_hdr_valid(ec_hdr));
        REQUIRE(nand_ubi_be64(ec_hdr->ec) == 0); /* <-- the gap this test documents */

        REQUIRE(f.vol_bdl->ops->erase(f.vol_bdl, 0, f.dev->leb_size) == ESP_OK);
    }

    release_fixture(f);
}


TEST_CASE("write: returns ESP_ERR_NO_MEM once the physical free pool is exhausted", "[nand_ubi][rw]")
{
    /* A handful of PEBs; leave the 2 layout PEBs plus 2 user PEBs good so only
     * 2 physical PEBs are available to user volumes,
     * while reserved_pebs=0 keeps nominal capacity at every lnum so the capacity
     * bounds check (which fires first in write()) does not mask the free-pool
     * exhaustion path this test targets. peb_count is read back from nand_bdl's own
     * geometry rather than assumed from the requested file size, which does not
     * translate 1:1 into block count once the emulation's own overhead is included. */
    esp_blockdev_handle_t nand_bdl = make_test_nand(8u * 131072u);
    uint32_t peb_count = (uint32_t)(nand_bdl->geometry.disk_size / nand_bdl->geometry.erase_size);
    REQUIRE(peb_count >= 3);
    for (uint32_t pnum = 4; pnum < peb_count; pnum++) {
        REQUIRE(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &pnum) == ESP_OK);
    }

    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    cfg.reserved_pebs = 0;
    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &dev) == ESP_OK);
    REQUIRE(dev->leb_capacity == peb_count - UBI_LAYOUT_VOLUME_EBS);

    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(dev, nullptr, UBI_VID_DYNAMIC,
                                   peb_count - UBI_LAYOUT_VOLUME_EBS, &vol_id) == ESP_OK);
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
