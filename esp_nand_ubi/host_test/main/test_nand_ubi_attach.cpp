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

/* Read-only attach skips the post-scan erase drain, so the scan's own PEB
 * classification (ERASE_PENDING vs FREE vs USED) stays observable. */
nand_ubi_device_t *attach_read_only(esp_blockdev_handle_t nand_bdl)
{
    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    cfg.read_only = true;
    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(nand_bdl, &cfg, &dev) == ESP_OK);
    return dev;
}

uint64_t read_ec(esp_blockdev_handle_t nand_bdl, uint32_t pnum, uint32_t page_size, uint32_t peb_size)
{
    std::vector<uint8_t> page(page_size, 0);
    REQUIRE(nand_bdl->ops->read(nand_bdl, page.data(), page.size(), (uint64_t)pnum * peb_size, page.size()) == ESP_OK);
    const auto *ec = reinterpret_cast<const nand_ubi_ec_hdr_t *>(page.data());
    REQUIRE(nand_ubi_ec_hdr_valid(ec));
    return nand_ubi_be64(ec->ec);
}

} // namespace

TEST_CASE("attach: format + attach produces a correct EBA table", "[nand_ubi][attach]")
{
    test_geometry g = make_geometry();
    format_volume_table(g.nand_bdl, g.page_size, g.peb_size, kImageSeq,
                        g.vid_hdr_offset, g.data_offset, {3});

    for (uint32_t pnum = 0; pnum < 3; pnum++) {
        format_peb(g.nand_bdl, pnum + 2, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset,
                   /*lnum=*/pnum, /*sqnum=*/pnum + 1);
    }

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);
    REQUIRE(dev != nullptr);

    REQUIRE(dev->peb_size == g.peb_size);
    REQUIRE(dev->page_size == g.page_size);
    REQUIRE(dev->vid_hdr_offset == g.vid_hdr_offset);
    REQUIRE(dev->data_offset == g.data_offset);
    REQUIRE(dev->leb_size == g.peb_size - g.data_offset);
    REQUIRE(dev->image_seq == kImageSeq);
    REQUIRE(dev->leb_count == 3);
    REQUIRE(dev->global_sqnum == 3);

    for (uint32_t lnum = 0; lnum < 3; lnum++) {
        REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, lnum) == (int32_t)lnum + 2);
        REQUIRE(peb_state_of(dev->eba, lnum + 2) == UBI_PEB_USED);
    }

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("attach: blank/unformatted chip attaches with zero LEBs", "[nand_ubi][attach]")
{
    test_geometry g = make_geometry();

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);
    REQUIRE(dev != nullptr);
    REQUIRE(dev->leb_count == 0);
    REQUIRE(dev->image_seq == 0);
    REQUIRE(dev->vid_hdr_offset == g.page_size);
    REQUIRE(dev->data_offset == 2u * g.page_size);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("attach: factory bad blocks are excluded from the EBA", "[nand_ubi][attach]")
{
    test_geometry g = make_geometry();

    uint32_t bad_pnum = 5;
    REQUIRE(g.nand_bdl->ops->ioctl(g.nand_bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &bad_pnum) == ESP_OK);

    /* Format three good PEBs around the bad one; LEB numbering must stay contiguous
     * and none of them may resolve to the bad physical block. */
    format_volume_table(g.nand_bdl, g.page_size, g.peb_size, kImageSeq,
                        g.vid_hdr_offset, g.data_offset, {3});
    format_peb(g.nand_bdl, 2, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset, 0, 1);
    format_peb(g.nand_bdl, 3, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset, 1, 2);
    format_peb(g.nand_bdl, 6, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset, 2, 3);

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);

    REQUIRE(dev->leb_count == 3);
    REQUIRE(peb_state_of(dev->eba, bad_pnum) == UBI_PEB_BAD);
    for (uint32_t lnum = 0; lnum < 3; lnum++) {
        int32_t pnum = nand_ubi_eba_get_pnum(&dev->eba, lnum);
        REQUIRE(pnum != (int32_t)bad_pnum);
    }
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 0) == 2);
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 1) == 3);
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 2) == 6);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("attach: corrupt EC header is scheduled for erase, not treated as free", "[nand_ubi][attach]")
{
    test_geometry g = make_geometry();

    REQUIRE(g.nand_bdl->ops->erase(g.nand_bdl, (uint64_t)2 * g.peb_size, g.peb_size) == ESP_OK);
    std::vector<uint8_t> garbage(g.page_size, 0xA5);
    REQUIRE(g.nand_bdl->ops->write(g.nand_bdl, garbage.data(), (uint64_t)2 * g.peb_size, g.page_size) == ESP_OK);

    nand_ubi_device_t *dev = attach_read_only(g.nand_bdl);
    REQUIRE(dev->leb_count == 0);
    REQUIRE(peb_state_of(dev->eba, 2) == UBI_PEB_ERASE_PENDING);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("attach: valid EC header with blank VID header is treated as free, not scheduled for erase",
          "[nand_ubi][attach]")
{
    /* A valid EC header with a blank VID header is the normal on-flash shape of a free
     * PEB now that nand_ubi_vol_erase() persists a fresh EC header immediately after
     * erasing (matching Linux UBI's own free-PEB representation) -- format_ec_hdr_only()
     * below models exactly that PEB shape, whether it arrived there via a genuine erase
     * or an allocation interrupted between the EC-header write and the VID-header write
     * in nand_ubi_vol_alloc_peb(). Either way it must be usable: attach() must leave it
     * in the default FREE peb_state rather than UBI_PEB_ERASE_PENDING (see
     * nand_ubi_vol_alloc_peb()'s own read-before-write handling of an already-valid EC
     * header for why reusing this exact PEB is safe). */
    test_geometry g = make_geometry();

    format_ec_hdr_only(g.nand_bdl, 2, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset);

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);
    REQUIRE(dev->leb_count == 0);
    REQUIRE(peb_state_of(dev->eba, 2) == UBI_PEB_FREE);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("attach: duplicate PEBs resolved by higher sqnum, loser scheduled for erase", "[nand_ubi][attach]")
{
    test_geometry g = make_geometry();
    format_volume_table(g.nand_bdl, g.page_size, g.peb_size, kImageSeq,
                        g.vid_hdr_offset, g.data_offset, {1});

    /* Simulated interrupted power-cut mid-write: two PEBs both claim lnum=0. */
    format_peb(g.nand_bdl, 2, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset,
               /*lnum=*/0, /*sqnum=*/5);
    format_peb(g.nand_bdl, 3, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset,
               /*lnum=*/0, /*sqnum=*/9);

    nand_ubi_device_t *dev = attach_read_only(g.nand_bdl);

    REQUIRE(dev->leb_count == 1);
    REQUIRE(dev->global_sqnum == 9);
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 0) == 3);
    REQUIRE(peb_state_of(dev->eba, 3) == UBI_PEB_USED);
    REQUIRE(peb_state_of(dev->eba, 2) == UBI_PEB_ERASE_PENDING);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("attach: copy_flag with matching data_crc accepts the newer copy", "[nand_ubi][attach]")
{
    test_geometry g = make_geometry();
    format_volume_table(g.nand_bdl, g.page_size, g.peb_size, kImageSeq,
                        g.vid_hdr_offset, g.data_offset, {1});

    std::vector<uint8_t> leb_data(64, 0x42);
    uint32_t data_crc = nand_ubi_crc32(leb_data.data(), (uint32_t)leb_data.size());

    format_peb(g.nand_bdl, 2, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset,
               /*lnum=*/0, /*sqnum=*/5);
    format_peb(g.nand_bdl, 3, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset,
               /*lnum=*/0, /*sqnum=*/9, /*copy_flag=*/1, (uint32_t)leb_data.size(), data_crc);
    write_leb_data(g.nand_bdl, 3, g.peb_size, g.data_offset, g.page_size, leb_data.data(), (uint32_t)leb_data.size());

    nand_ubi_device_t *dev = attach_read_only(g.nand_bdl);

    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 0) == 3);
    REQUIRE(peb_state_of(dev->eba, 3) == UBI_PEB_USED);
    REQUIRE(peb_state_of(dev->eba, 2) == UBI_PEB_ERASE_PENDING);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("attach: copy_flag with mismatched data_crc falls back to the older copy", "[nand_ubi][attach]")
{
    test_geometry g = make_geometry();
    format_volume_table(g.nand_bdl, g.page_size, g.peb_size, kImageSeq,
                        g.vid_hdr_offset, g.data_offset, {1});

    std::vector<uint8_t> leb_data(64, 0x42);
    uint32_t bogus_crc = nand_ubi_crc32(leb_data.data(), (uint32_t)leb_data.size()) ^ 0xFFFFFFFFu;

    format_peb(g.nand_bdl, 2, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset,
               /*lnum=*/0, /*sqnum=*/5);
    format_peb(g.nand_bdl, 3, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset,
               /*lnum=*/0, /*sqnum=*/9, /*copy_flag=*/1, (uint32_t)leb_data.size(), bogus_crc);
    write_leb_data(g.nand_bdl, 3, g.peb_size, g.data_offset, g.page_size, leb_data.data(), (uint32_t)leb_data.size());

    nand_ubi_device_t *dev = attach_read_only(g.nand_bdl);

    /* pnum 1's data_crc doesn't match -> pnum 0 (the older, valid copy) must win. */
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 0) == 2);
    REQUIRE(peb_state_of(dev->eba, 2) == UBI_PEB_USED);
    REQUIRE(peb_state_of(dev->eba, 3) == UBI_PEB_ERASE_PENDING);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("attach: rejects null arguments", "[nand_ubi][attach]")
{
    test_geometry g = make_geometry();
    nand_ubi_device_t *dev = nullptr;

    REQUIRE(nand_ubi_attach(nullptr, nullptr, &dev) == ESP_ERR_INVALID_ARG);
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, nullptr) == ESP_ERR_INVALID_ARG);
    REQUIRE(nand_ubi_detach(nullptr) == ESP_ERR_INVALID_ARG);

    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("attach: ERASE_PENDING PEBs are erased back into the free pool with EC+1", "[nand_ubi][attach][reclaim]")
{
    test_geometry g = make_geometry();
    format_volume_table(g.nand_bdl, g.page_size, g.peb_size, kImageSeq,
                        g.vid_hdr_offset, g.data_offset, {1});
    /* pnum 2: stale duplicate of lnum 0 (ec=0); pnum 4: corrupt EC header. */
    format_peb(g.nand_bdl, 2, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset, 0, 5);
    format_peb(g.nand_bdl, 3, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset, 0, 9);
    REQUIRE(g.nand_bdl->ops->erase(g.nand_bdl, (uint64_t)4 * g.peb_size, g.peb_size) == ESP_OK);
    std::vector<uint8_t> garbage(g.page_size, 0xA5);
    REQUIRE(g.nand_bdl->ops->write(g.nand_bdl, garbage.data(), (uint64_t)4 * g.peb_size, g.page_size) == ESP_OK);

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 0) == 3);
    REQUIRE(peb_state_of(dev->eba, 2) == UBI_PEB_FREE);
    REQUIRE(peb_state_of(dev->eba, 4) == UBI_PEB_FREE);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);

    /* Both carry a fresh EC header: the duplicate's own ec+1, and mean_ec+1 for the
     * PEB whose EC header was unreadable (every valid EC on this chip is 0). */
    REQUIRE(read_ec(g.nand_bdl, 2, g.page_size, g.peb_size) == 1);
    REQUIRE(read_ec(g.nand_bdl, 4, g.page_size, g.peb_size) == 1);

    /* A second attach finds nothing left to reclaim: the PEBs stay FREE, EC unchanged. */
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);
    REQUIRE(peb_state_of(dev->eba, 2) == UBI_PEB_FREE);
    REQUIRE(peb_state_of(dev->eba, 4) == UBI_PEB_FREE);
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 0) == 3);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    REQUIRE(read_ec(g.nand_bdl, 2, g.page_size, g.peb_size) == 1);

    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("attach: data PEBs without a volume table are refused, not erased", "[nand_ubi][attach][reclaim]")
{
    test_geometry g = make_geometry();
    format_peb(g.nand_bdl, 2, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset, 0, 1);

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_ERR_INVALID_STATE);
    REQUIRE(dev == nullptr);

    /* Read-only attach still works for inspection, and the data PEB is untouched. */
    dev = attach_read_only(g.nand_bdl);
    REQUIRE(peb_state_of(dev->eba, 2) == UBI_PEB_ERASE_PENDING);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    std::vector<uint8_t> page(g.page_size, 0);
    REQUIRE(g.nand_bdl->ops->read(g.nand_bdl, page.data(), page.size(),
                                  (uint64_t)2 * g.peb_size + g.vid_hdr_offset, page.size()) == ESP_OK);
    REQUIRE(nand_ubi_vid_hdr_valid(reinterpret_cast<const nand_ubi_vid_hdr_t *>(page.data())));

    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("layout: factory-bad PEB 0 and 1 do not block volume creation or reattach", "[nand_ubi][attach][layout]")
{
    test_geometry g = make_geometry();
    for (uint32_t pnum = 0; pnum < 2; pnum++) {
        REQUIRE(g.nand_bdl->ops->ioctl(g.nand_bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &pnum) == ESP_OK);
    }

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(dev, "data", UBI_VID_DYNAMIC, 4, &vol_id) == ESP_OK);
    REQUIRE(dev->vtbl_pnum[0] == 2);
    REQUIRE(dev->vtbl_pnum[1] == 3);
    esp_blockdev_handle_t vol = nullptr;
    REQUIRE(nand_ubi_open_volume(dev, vol_id, &vol) == ESP_OK);
    std::vector<uint8_t> src(g.page_size, 0xE7);
    REQUIRE(vol->ops->write(vol, src.data(), 0, src.size()) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 0) > 3);
    REQUIRE(vol->ops->release(vol) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);

    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);
    REQUIRE(dev->vol_count == 1);
    REQUIRE(dev->vtbl_pnum[0] == 2);
    REQUIRE(dev->vtbl_pnum[1] == 3);
    REQUIRE(peb_state_of(dev->eba, 0) == UBI_PEB_BAD);
    REQUIRE(nand_ubi_open_volume(dev, vol_id, &vol) == ESP_OK);
    std::vector<uint8_t> dst(src.size(), 0);
    REQUIRE(vol->ops->read(vol, dst.data(), dst.size(), 0, dst.size()) == ESP_OK);
    REQUIRE(dst == src);
    REQUIRE(vol->ops->release(vol) == ESP_OK);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("layout: volume-table copies are found wherever the image put them", "[nand_ubi][attach][layout]")
{
    /* A position-independent image: the flasher skipped bad blocks, so the layout
     * copies landed on PEBs 3 and 5 and LEB 0's data on PEB 0. */
    test_geometry g = make_geometry();
    format_volume_table(g.nand_bdl, g.page_size, g.peb_size, kImageSeq,
                        g.vid_hdr_offset, g.data_offset, {2}, {3, 5});
    format_peb(g.nand_bdl, 0, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset, 0, 10);

    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);
    REQUIRE(dev->vol_count == 1);
    REQUIRE(dev->vtbl_pnum[0] == 3);
    REQUIRE(dev->vtbl_pnum[1] == 5);
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 0) == 0);
    REQUIRE(peb_state_of(dev->eba, 3) == UBI_PEB_USED);
    REQUIRE(peb_state_of(dev->eba, 5) == UBI_PEB_USED);
    REQUIRE(peb_state_of(dev->eba, 1) == UBI_PEB_FREE);
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("layout: rewriting the volume table carries each copy's EC forward", "[nand_ubi][attach][layout]")
{
    test_geometry g = make_geometry();
    nand_ubi_device_t *dev = nullptr;
    REQUIRE(nand_ubi_attach(g.nand_bdl, nullptr, &dev) == ESP_OK);
    uint32_t vol_id = 0;
    REQUIRE(nand_ubi_create_volume(dev, nullptr, UBI_VID_DYNAMIC, 1, &vol_id) == ESP_OK);
    uint64_t first[2];
    for (uint32_t copy = 0; copy < 2; copy++) {
        first[copy] = read_ec(g.nand_bdl, (uint32_t)dev->vtbl_pnum[copy], g.page_size, g.peb_size);
    }
    for (int i = 0; i < 2; i++) {
        REQUIRE(nand_ubi_create_volume(dev, nullptr, UBI_VID_DYNAMIC, 1, &vol_id) == ESP_OK);
    }
    for (uint32_t copy = 0; copy < 2; copy++) {
        REQUIRE(read_ec(g.nand_bdl, (uint32_t)dev->vtbl_pnum[copy], g.page_size, g.peb_size) == first[copy] + 2);
    }
    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}

TEST_CASE("attach: three PEBs claiming one LEB resolve to the highest sqnum", "[nand_ubi][attach]")
{
    test_geometry g = make_geometry();
    format_volume_table(g.nand_bdl, g.page_size, g.peb_size, kImageSeq,
                        g.vid_hdr_offset, g.data_offset, {1});
    format_peb(g.nand_bdl, 2, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset, 0, 5);
    format_peb(g.nand_bdl, 3, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset, 0, 9);
    format_peb(g.nand_bdl, 4, g.page_size, g.peb_size, kImageSeq, g.vid_hdr_offset, g.data_offset, 0, 7);

    nand_ubi_device_t *dev = attach_read_only(g.nand_bdl);
    REQUIRE(nand_ubi_eba_get_pnum(&dev->eba, 0) == 3);
    REQUIRE(peb_state_of(dev->eba, 3) == UBI_PEB_USED);
    REQUIRE(peb_state_of(dev->eba, 2) == UBI_PEB_ERASE_PENDING);
    REQUIRE(peb_state_of(dev->eba, 4) == UBI_PEB_ERASE_PENDING);
    REQUIRE(dev->global_sqnum == 9);

    REQUIRE(nand_ubi_detach(dev) == ESP_OK);
    g.nand_bdl->ops->release(g.nand_bdl);
}
