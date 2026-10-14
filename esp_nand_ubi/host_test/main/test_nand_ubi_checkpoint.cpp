/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* Attach checkpoint tests. See docs/fastmap-design.md. */

#include <cstdio>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "esp_nand_ubi.h"
#include "nand_ubi_test_helpers.h"
#include "ptest_ubi.h"

extern "C" {
#include "nand_ubi_priv.h"
}

using namespace nand_ubi_test;

namespace {

constexpr uint32_t kChipBytes = 12u * 1024u * 1024u; /* 96 PEBs: region 10, pool 10 */
constexpr uint32_t kDataLebs = 16;
constexpr uint32_t kPagesPerStep = 4;

nand_ubi_config_t ckpt_cfg()
{
    nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
    cfg.checkpoint = true;
    return cfg;
}

struct rig {
    faulty_nand *nand = nullptr;
    nand_ubi_device_t *dev = nullptr;
    esp_blockdev_handle_t vol = nullptr;
    ptest_ubi_t *work = nullptr;
};

ptest_ubi_result_t verify(rig &r)
{
    ptest_ubi_result_t res;
    REQUIRE(ptest_ubi_verify(r.work, &res) == ESP_OK);
    INFO("verify: " << res.first_error);
    REQUIRE(res.ok);
    return res;
}

/* Every used PEB is a volume-table copy, the checkpoint, or mapped to exactly one LEB,
 * and nothing is left to erase after a read-write attach. */
void check_peb_accounting(const nand_ubi_device_t *dev)
{
    uint32_t used = 0;
    for (uint32_t pnum = 0; pnum < dev->peb_count; pnum++) {
        uint8_t state = nand_ubi_eba_peb_get_state(&dev->eba, pnum);
        REQUIRE(state != UBI_PEB_ERASE_PENDING);
        used += state == UBI_PEB_USED;
    }
    uint32_t mapped = 0;
    for (uint32_t i = 0; i < dev->leb_count; i++) {
        mapped += nand_ubi_eba_get_pnum(&dev->eba, i) != UBI_LEB_UNMAPPED;
    }
    REQUIRE(used == mapped + UBI_LAYOUT_VOLUME_EBS + (dev->ckpt_pnum >= 0 ? 1u : 0u));
}

rig make_rig(uint32_t seed)
{
    rig r;
    r.nand = make_faulty_nand(kChipBytes);
    nand_ubi_config_t cfg = ckpt_cfg();
    REQUIRE(nand_ubi_attach(&r.nand->bdl, &cfg, &r.dev) == ESP_OK);
    REQUIRE(r.dev->ckpt);
    nand_ubi_eba_seed(&r.dev->eba, seed);
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(r.dev, "ptest", UBI_VID_DYNAMIC, kDataLebs + PTEST_UBI_JOURNAL_LEBS,
                                   &vol_id) == ESP_OK);
    REQUIRE(nand_ubi_open_volume(r.dev, vol_id, &r.vol) == ESP_OK);
    ptest_ubi_cfg_t pcfg = { kDataLebs, kPagesPerStep, seed };
    REQUIRE(ptest_ubi_create(r.vol, &pcfg, &r.work) == ESP_OK);
    verify(r);
    return r;
}

void release_handles(rig &r)
{
    REQUIRE(r.vol->ops->release(r.vol) == ESP_OK);
    REQUIRE(nand_ubi_detach(r.dev) == ESP_OK);
}

/* Power cut: the device dies, the RAM state is dropped without touching the flash. */
void crash(rig &r)
{
    r.nand->dead = true;
    REQUIRE(r.vol->ops->release(r.vol) == ESP_OK);
    REQUIRE(nand_ubi_detach(r.dev) == ESP_OK);
    r.nand->dead = false;
    r.nand->cut_after = -1;
    r.nand->cut_match = nullptr;
}

/* Attaches and verifies the workload. With a full-scan reference, the volume is also
 * verified read-only without the checkpoint, as Linux would attach it. */
ptest_ubi_result_t attach_and_verify(rig &r, uint64_t *reads = nullptr)
{
    nand_ubi_config_t ref_cfg = NAND_UBI_CONFIG_DEFAULT();
    ref_cfg.read_only = true;
    nand_ubi_device_t *ref = nullptr;
    REQUIRE(nand_ubi_attach(&r.nand->bdl, &ref_cfg, &ref) == ESP_OK);
    esp_blockdev_handle_t ref_vol = nullptr;
    REQUIRE(nand_ubi_open_volume(ref, 0, &ref_vol) == ESP_OK);
    ptest_ubi_set_volume(r.work, ref_vol);
    ptest_ubi_result_t ref_res;
    REQUIRE(ptest_ubi_verify(r.work, &ref_res) == ESP_OK);
    INFO("full-scan verify: " << ref_res.first_error);
    REQUIRE(ref_res.ok);
    REQUIRE(ref_vol->ops->release(ref_vol) == ESP_OK);
    REQUIRE(nand_ubi_detach(ref) == ESP_OK);

    uint64_t before = r.nand->reads;
    nand_ubi_config_t cfg = ckpt_cfg();
    REQUIRE(nand_ubi_attach(&r.nand->bdl, &cfg, &r.dev) == ESP_OK);
    if (reads != nullptr) {
        *reads = r.nand->reads - before;
    }
    nand_ubi_eba_seed(&r.dev->eba, (uint32_t)r.dev->global_sqnum);
    REQUIRE(r.dev->vtbl_copy_good[0]);
    REQUIRE(r.dev->vtbl_copy_good[1]);
    check_peb_accounting(r.dev);
    REQUIRE(nand_ubi_open_volume(r.dev, 0, &r.vol) == ESP_OK);
    ptest_ubi_set_volume(r.work, r.vol);
    return verify(r);
}

void release_rig(rig &r)
{
    ptest_ubi_destroy(r.work);
    release_handles(r);
    REQUIRE(r.nand->bdl.ops->release(&r.nand->bdl) == ESP_OK);
}

void run_steps(rig &r, int n)
{
    for (int i = 0; i < n; i++) {
        REQUIRE(ptest_ubi_step(r.work) == ESP_OK);
    }
}

} // namespace

TEST_CASE("checkpoint: attach after a power cut reads the checkpoint, region and pool only",
          "[nand_ubi][ckpt]")
{
    rig r = make_rig(1);
    run_steps(r, 25); /* more allocations than the pool holds: several checkpoints */
    REQUIRE(r.dev->ckpt);
    REQUIRE(r.dev->ckpt_pnum >= 0);
    REQUIRE((uint32_t)r.dev->ckpt_pnum < r.dev->ckpt_region);
    uint32_t region = r.dev->ckpt_region;
    uint32_t pool = r.dev->ckpt_pool_max;
    crash(r);

    uint64_t reads = 0;
    attach_and_verify(r, &reads);
    REQUIRE(r.dev->ckpt_pnum >= 0); /* attached from the checkpoint */
    printf("checkpoint attach: %llu reads (region %u, pool %u, %u PEBs)\n", (unsigned long long)reads,
           (unsigned)region, (unsigned)pool, (unsigned)r.dev->peb_count);
    /* Two volume-table copies (EC, VID, table pages each) plus about 3 reads per region and
     * pool PEB and the checkpoint payload. */
    uint32_t vtbl_pages = (r.dev->vtbl_slots * (uint32_t)sizeof(nand_ubi_vtbl_record_t) + r.dev->page_size - 1) /
                          r.dev->page_size;
    REQUIRE(reads <= 2u * (2u + vtbl_pages) + 3u * (region + pool) + 8u);
    REQUIRE(reads < 2u * r.dev->peb_count);
    release_rig(r);
}

TEST_CASE("checkpoint: no data PEB is allocated inside the region", "[nand_ubi][ckpt]")
{
    rig r = make_rig(2);
    run_steps(r, 60);
    for (uint32_t i = 0; i < r.dev->leb_count; i++) {
        int32_t pnum = nand_ubi_eba_get_pnum(&r.dev->eba, i);
        REQUIRE((pnum == UBI_LEB_UNMAPPED || (uint32_t)pnum >= r.dev->ckpt_region));
    }
    release_rig(r);
}

TEST_CASE("checkpoint: a clean detach erases the PEBs of erased LEBs", "[nand_ubi][ckpt]")
{
    rig r = make_rig(3);
    run_steps(r, 3);
    std::vector<uint8_t> page(r.dev->page_size, 0);
    REQUIRE(r.vol->ops->erase(r.vol, 0, r.dev->leb_size) == ESP_OK);
    REQUIRE(r.vol->ops->read(r.vol, page.data(), page.size(), 0, page.size()) == ESP_OK);
    REQUIRE(std::all_of(page.begin(), page.end(), [](uint8_t b) {
        return b == 0xFF;
    }));
    release_handles(r);

    nand_ubi_config_t cfg = ckpt_cfg();
    REQUIRE(nand_ubi_attach(&r.nand->bdl, &cfg, &r.dev) == ESP_OK);
    REQUIRE(nand_ubi_eba_get_pnum(&r.dev->eba, 0) == UBI_LEB_UNMAPPED);
    check_peb_accounting(r.dev);
    REQUIRE(nand_ubi_open_volume(r.dev, 0, &r.vol) == ESP_OK);
    ptest_ubi_destroy(r.work);
    release_handles(r);
    REQUIRE(r.nand->bdl.ops->release(&r.nand->bdl) == ESP_OK);
}

TEST_CASE("checkpoint: reattach after a clean detach writes and erases nothing", "[nand_ubi][ckpt]")
{
    /* Each step erases a LEB, so the detach checkpoint lists PEBs as erase pending and
     * erases them after writing it. Reattach must see they are done. */
    rig r = make_rig(7);
    run_steps(r, 5);
    release_handles(r);
    for (int boot = 0; boot < 3; boot++) {
        uint64_t ops = r.nand->ops;
        nand_ubi_config_t cfg = ckpt_cfg();
        REQUIRE(nand_ubi_attach(&r.nand->bdl, &cfg, &r.dev) == ESP_OK);
        REQUIRE(r.dev->ckpt_pnum >= 0);
        REQUIRE(r.nand->ops == ops);
        check_peb_accounting(r.dev);
        REQUIRE(nand_ubi_open_volume(r.dev, 0, &r.vol) == ESP_OK);
        ptest_ubi_set_volume(r.work, r.vol);
        verify(r);
        release_handles(r);
        REQUIRE(r.nand->ops == ops); /* nothing changed, so detach writes no checkpoint */
    }
    ptest_ubi_destroy(r.work);
    REQUIRE(r.nand->bdl.ops->release(&r.nand->bdl) == ESP_OK);
}

TEST_CASE("checkpoint: a new checkpoint keeps the unused pool PEBs and samples only the used ones",
          "[nand_ubi][ckpt]")
{
    rig r = make_rig(8);
    REQUIRE(nand_ubi_checkpoint(r.dev) == ESP_OK);
    std::vector<uint16_t> before(r.dev->pool, r.dev->pool + r.dev->pool_count);
    REQUIRE(before.size() == r.dev->ckpt_pool_max);

    /* Allocate exactly 3 pool PEBs: write three unmapped LEBs. */
    std::vector<uint8_t> page(r.dev->page_size, 0x5A);
    for (uint32_t lnum = 0; lnum < 3; lnum++) {
        uint64_t addr = (uint64_t)(kDataLebs + PTEST_UBI_JOURNAL_LEBS - 1 - lnum) * r.dev->leb_size;
        REQUIRE(r.vol->ops->erase(r.vol, addr, r.dev->leb_size) == ESP_OK);
        REQUIRE(r.vol->ops->write(r.vol, page.data(), addr, page.size()) == ESP_OK);
    }
    std::vector<uint16_t> unused;
    for (uint16_t p : before) {
        if (nand_ubi_eba_peb_is_free(&r.dev->eba, p)) {
            unused.push_back(p);
        }
    }
    REQUIRE(unused.size() == before.size() - 3);

    uint64_t reads = r.nand->reads;
    REQUIRE(nand_ubi_checkpoint(r.dev) == ESP_OK);
    reads = r.nand->reads - reads;
    std::vector<uint16_t> after(r.dev->pool, r.dev->pool + r.dev->pool_count);
    REQUIRE(after.size() == r.dev->ckpt_pool_max);
    REQUIRE(std::vector<uint16_t>(after.begin(), after.begin() + (long)unused.size()) == unused);
    /* 3 entries refilled with k samples each, plus the EC headers of the free region PEBs. */
    REQUIRE(reads <= 3u * CONFIG_ESP_NAND_UBI_ALLOC_SAMPLES + r.dev->ckpt_region);
    release_rig(r);
}

TEST_CASE("checkpoint: the mean erase count follows erases and matches a full scan", "[nand_ubi][ckpt]")
{
    rig r = make_rig(9);
    run_steps(r, 80);
    REQUIRE(r.dev->mean_ec == 0);              /* attached to a blank chip */
    REQUIRE(r.dev->ec_sum > r.dev->ec_count);  /* but more than one erase per PEB since */
    release_handles(r); /* clean detach: checkpoint and deferred erases done */

    nand_ubi_config_t ref_cfg = NAND_UBI_CONFIG_DEFAULT();
    ref_cfg.read_only = true;
    nand_ubi_device_t *ref = nullptr;
    REQUIRE(nand_ubi_attach(&r.nand->bdl, &ref_cfg, &ref) == ESP_OK);

    nand_ubi_config_t cfg = ckpt_cfg();
    REQUIRE(nand_ubi_attach(&r.nand->bdl, &cfg, &r.dev) == ESP_OK);
    REQUIRE(r.dev->ckpt_pnum >= 0);
    REQUIRE(r.dev->ec_count == ref->ec_count);
    REQUIRE(r.dev->ec_sum == ref->ec_sum);
    REQUIRE(r.dev->mean_ec == ref->mean_ec);
    REQUIRE(r.dev->mean_ec >= 1);
    REQUIRE(nand_ubi_detach(ref) == ESP_OK);
    REQUIRE(nand_ubi_open_volume(r.dev, 0, &r.vol) == ESP_OK);
    release_rig(r);
}

TEST_CASE("checkpoint: a corrupt checkpoint falls back to a full scan", "[nand_ubi][ckpt]")
{
    rig r = make_rig(4);
    run_steps(r, 12);
    int32_t ckpt = r.dev->ckpt_pnum;
    uint64_t payload = (uint64_t)ckpt * r.dev->peb_size + r.dev->data_offset;
    crash(r);

    /* Programming zeros over the first payload page clears the magic. */
    std::vector<uint8_t> zeros(r.nand->inner->geometry.read_size, 0);
    REQUIRE(r.nand->inner->ops->write(r.nand->inner, zeros.data(), payload, zeros.size()) == ESP_OK);

    uint64_t reads = 0;
    attach_and_verify(r, &reads);
    REQUIRE(reads >= r.dev->peb_count); /* full scan */
    REQUIRE(r.dev->ckpt_pnum >= 0);     /* and a new checkpoint written after it */
    REQUIRE(r.dev->ckpt_pnum != ckpt);
    run_steps(r, 12);
    crash(r);
    attach_and_verify(r);
    release_rig(r);
}

TEST_CASE("checkpoint: a full scan without checkpoint support erases the checkpoint PEBs",
          "[nand_ubi][ckpt]")
{
    rig r = make_rig(5);
    run_steps(r, 12);
    int32_t ckpt = r.dev->ckpt_pnum;
    crash(r);

    /* The behaviour of an implementation that does not know the checkpoint volume. */
    nand_ubi_device_t *plain = nullptr;
    REQUIRE(nand_ubi_attach(&r.nand->bdl, nullptr, &plain) == ESP_OK);
    REQUIRE_FALSE(plain->ckpt);
    REQUIRE(nand_ubi_eba_peb_get_state(&plain->eba, (uint32_t)ckpt) == UBI_PEB_FREE);
    REQUIRE(nand_ubi_open_volume(plain, 0, &r.vol) == ESP_OK);
    ptest_ubi_set_volume(r.work, r.vol);
    verify(r);
    run_steps(r, 6);
    r.dev = plain;
    crash(r);

    attach_and_verify(r);
    release_rig(r);
}

TEST_CASE("checkpoint: power cut at every write and erase of a long workload", "[nand_ubi][ckpt][powercut]")
{
    for (cut_kind kind : { cut_kind::clean, cut_kind::partial, cut_kind::torn }) {
        rig r = make_rig(6);
        uint32_t cuts = 0;
        for (long n = 0; n < 400; n += 3) {
            r.nand->cut_type = kind;
            r.nand->cut_after = n;
            bool fired = false;
            for (int i = 0; i < 60 && !fired; i++) {
                fired = ptest_ubi_step(r.work) != ESP_OK;
            }
            if (!fired) {
                r.nand->cut_after = -1;
                continue;
            }
            REQUIRE(r.nand->dead);
            cuts++;
            crash(r);
            INFO("cut kind " << (int)kind << " after " << n << " ops");
            attach_and_verify(r);
            run_steps(r, 2);
        }
        printf("checkpoint power cuts (%s): %u\n",
               kind == cut_kind::clean ? "clean" : kind == cut_kind::partial ? "partial" : "torn", (unsigned)cuts);
        REQUIRE(cuts > 100);
        release_rig(r);
    }
}
