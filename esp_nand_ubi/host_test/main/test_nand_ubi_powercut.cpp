/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* Power-cut tests: the ptest_ubi workload on the fault-injecting emulated NAND. See
 * docs/plans/2026-10-08-esp-nand-ubi-powercut-stress-design.md. */

#include <algorithm>
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

constexpr uint32_t kDataLebs = 16;
constexpr uint32_t kPagesPerStep = 4;
constexpr uint32_t kChipBytes = 12u * 1024u * 1024u;
constexpr int kMaxStepsUntilCut = 200;

struct rig {
    faulty_nand *nand = nullptr;
    nand_ubi_device_t *dev = nullptr;
    esp_blockdev_handle_t vol = nullptr;
    ptest_ubi_t *work = nullptr;
};

/* After a read-write attach: nothing left to erase, and every used PEB is either a
 * volume-table copy or mapped to exactly one LEB. A PEB lost by a cut shows up here. */
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
    REQUIRE(used == mapped + UBI_LAYOUT_VOLUME_EBS);
}

ptest_ubi_result_t verify(rig &r)
{
    ptest_ubi_result_t res;
    REQUIRE(ptest_ubi_verify(r.work, &res) == ESP_OK);
    INFO("verify: " << res.first_error);
    REQUIRE(res.ok);
    return res;
}

rig make_rig(uint32_t seed)
{
    rig r;
    r.nand = make_faulty_nand(kChipBytes);
    REQUIRE(nand_ubi_attach(&r.nand->bdl, nullptr, &r.dev) == ESP_OK);
    nand_ubi_eba_seed(&r.dev->eba, seed); /* attach seeds from esp_random(); keep runs reproducible */
    uint32_t vol_id = UINT32_MAX;
    REQUIRE(nand_ubi_create_volume(r.dev, "ptest", UBI_VID_DYNAMIC, kDataLebs + PTEST_UBI_JOURNAL_LEBS,
                                   &vol_id) == ESP_OK);
    REQUIRE(nand_ubi_open_volume(r.dev, vol_id, &r.vol) == ESP_OK);
    ptest_ubi_cfg_t cfg = { kDataLebs, kPagesPerStep, seed };
    REQUIRE(ptest_ubi_create(r.vol, &cfg, &r.work) == ESP_OK);
    verify(r);
    return r;
}

/* Power comes back: re-attach (read-write, so attach drains and repairs), check UBI's
 * own state, then let the workload check the volume. */
ptest_ubi_result_t reboot(rig &r)
{
    /* Drop the RAM state while the chip is still dead: a detach on a live chip would
     * write a checkpoint, which a power cut never does. */
    r.nand->dead = true;
    REQUIRE(r.vol->ops->release(r.vol) == ESP_OK);
    REQUIRE(nand_ubi_detach(r.dev) == ESP_OK);
    r.nand->dead = false;
    r.nand->cut_after = -1;
    r.nand->cut_match = nullptr;
    REQUIRE(nand_ubi_attach(&r.nand->bdl, nullptr, &r.dev) == ESP_OK);
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
    REQUIRE(r.vol->ops->release(r.vol) == ESP_OK);
    REQUIRE(nand_ubi_detach(r.dev) == ESP_OK);
    REQUIRE(r.nand->bdl.ops->release(&r.nand->bdl) == ESP_OK);
}

void run_steps(rig &r, int n)
{
    for (int i = 0; i < n; i++) {
        REQUIRE(ptest_ubi_step(r.work) == ESP_OK);
    }
}

/* Runs workload steps until the armed cut fires, then reboots and verifies. Returns
 * false if the cut never fired within kMaxStepsUntilCut steps. */
bool cut_and_recover(rig &r, uint32_t *in_flight = nullptr)
{
    for (int i = 0; i < kMaxStepsUntilCut; i++) {
        if (ptest_ubi_step(r.work) != ESP_OK) {
            REQUIRE(r.nand->dead);
            ptest_ubi_result_t res = reboot(r);
            if (in_flight != nullptr) {
                *in_flight += res.in_flight;
            }
            run_steps(r, 2);
            verify(r);
            return true;
        }
    }
    r.nand->cut_after = -1;
    r.nand->cut_match = nullptr;
    return false;
}

const char *kind_name(cut_kind k)
{
    return k == cut_kind::clean ? "clean" : k == cut_kind::partial ? "partial" : "torn";
}

} // namespace

TEST_CASE("powercut ubi: workload verifies across steps and reattach", "[nand_ubi][powercut]")
{
    rig r = make_rig(1);
    for (int round = 0; round < 4; round++) {
        run_steps(r, 50);
        verify(r);
    }
    ptest_ubi_result_t res = reboot(r);
    REQUIRE(res.lebs_matching == kDataLebs);
    REQUIRE(res.in_flight == 0);
    release_rig(r);
}

TEST_CASE("powercut ubi: verify reports lost and corrupt data", "[nand_ubi][powercut]")
{
    rig r = make_rig(2);
    run_steps(r, 40);
    verify(r);

    /* Erase a committed LEB behind the workload's back: its data is lost. */
    int32_t victim = -1;
    for (uint32_t l = 0; l < kDataLebs && victim < 0; l++) {
        if (nand_ubi_eba_get_pnum(&r.dev->eba, l) != UBI_LEB_UNMAPPED) {
            victim = (int32_t)l;
        }
    }
    REQUIRE(victim >= 0);
    REQUIRE(r.vol->ops->erase(r.vol, (uint64_t)victim * r.dev->leb_size, r.dev->leb_size) == ESP_OK);
    ptest_ubi_result_t res;
    REQUIRE(ptest_ubi_verify(r.work, &res) == ESP_OK);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.lost == 1);

    /* Stray data after the last page of a committed LEB: corruption. */
    int32_t other = -1;
    for (uint32_t l = 0; l < kDataLebs && other < 0; l++) {
        if ((int32_t)l != victim && nand_ubi_eba_get_pnum(&r.dev->eba, l) != UBI_LEB_UNMAPPED) {
            other = (int32_t)l;
        }
    }
    REQUIRE(other >= 0);
    std::vector<uint8_t> junk(r.dev->page_size, 0x5A);
    REQUIRE(r.vol->ops->write(r.vol, junk.data(),
                              (uint64_t)other * r.dev->leb_size + (uint64_t)kPagesPerStep * r.dev->page_size,
                              junk.size()) == ESP_OK);
    REQUIRE(ptest_ubi_verify(r.work, &res) == ESP_OK);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.corrupt >= 1);

    release_rig(r);
}

TEST_CASE("powercut ubi: log sweep of cut points", "[nand_ubi][powercut]")
{
    for (cut_kind kind : { cut_kind::clean, cut_kind::partial, cut_kind::torn }) {
        rig r = make_rig(3);
        run_steps(r, 10);
        int cuts = 0;
        uint32_t in_flight = 0;
        for (long n = 0; n <= 512; n = n == 0 ? 1 : n * 2) {
            INFO("kind=" << kind_name(kind) << " cut_after=" << n);
            r.nand->cut_type = kind;
            r.nand->cut_after = n;
            cuts += cut_and_recover(r, &in_flight) ? 1 : 0;
        }
        REQUIRE(cuts >= 8);
        REQUIRE(in_flight >= 3); /* the sweep must actually interrupt LEB rewrites */
        release_rig(r);
    }
}

TEST_CASE("powercut ubi: targeted cuts at UBI header writes and erases", "[nand_ubi][powercut]")
{
    struct target {
        const char *name;
        std::function<bool(const nand_op &)> match;
        std::vector<cut_kind> kinds;
    };
    const uint32_t page = 2048;
    const uint32_t peb = 131072;
    std::vector<target> targets = {
        { "EC header write", [&](const nand_op & op) {
              return !op.is_erase && op.addr % peb == 0;
          }, { cut_kind::clean, cut_kind::partial }
        },
        { "VID header write", [&](const nand_op & op) {
              return !op.is_erase && op.addr % peb == page;
          }, { cut_kind::clean, cut_kind::partial }
        },
        { "PEB erase", [](const nand_op & op) {
              return op.is_erase;
          }, { cut_kind::clean, cut_kind::torn }
        },
    };

    rig r = make_rig(4);
    REQUIRE(r.dev->page_size == page);
    REQUIRE(r.dev->peb_size == peb);
    run_steps(r, 10);
    for (const target &t : targets) {
        for (cut_kind kind : t.kinds) {
            for (long skip = 0; skip < 6; skip++) {
                INFO("target=" << t.name << " kind=" << kind_name(kind) << " skip=" << skip);
                r.nand->cut_type = kind;
                r.nand->cut_match = t.match;
                r.nand->cut_after = skip;
                REQUIRE(cut_and_recover(r));
            }
        }
    }
    release_rig(r);
}

TEST_CASE("powercut ubi: a second cut before the redo of an interrupted rewrite erases it",
          "[nand_ubi][powercut]")
{
    rig r = make_rig(5);
    run_steps(r, 10);

    /* First cut: half-way through a LEB's data pages, leaving a partial generation. */
    r.nand->cut_type = cut_kind::partial;
    r.nand->cut_match = [&](const nand_op & op) {
        return !op.is_erase && op.addr % r.dev->peb_size >= 2 * r.dev->page_size;
    };
    r.nand->cut_after = 2;
    bool fired = false;
    for (int i = 0; i < kMaxStepsUntilCut && !fired; i++) {
        fired = ptest_ubi_step(r.work) != ESP_OK;
    }
    REQUIRE(fired);
    ptest_ubi_result_t res = reboot(r);
    REQUIRE(res.in_flight == 1);

    /* Second cut: on the first erase of the redo, after its INTENT reached the journal. */
    r.nand->cut_type = cut_kind::clean;
    r.nand->cut_match = [](const nand_op & op) {
        return op.is_erase;
    };
    r.nand->cut_after = 0;
    REQUIRE(ptest_ubi_step(r.work) != ESP_OK);
    res = reboot(r);
    REQUIRE(res.in_flight == 1);

    run_steps(r, 3);
    verify(r);
    release_rig(r);
}

TEST_CASE("wear: a churning workload spreads erases evenly over the chip", "[nand_ubi][powercut][ec]")
{
    rig r = make_rig(6);
    run_steps(r, 600);
    reboot(r);

    uint64_t min_ec = UINT64_MAX, max_ec = 0;
    for (uint32_t pnum = 0; pnum < r.dev->peb_count; pnum++) {
        /* The two volume-table PEBs hold static data, written once when the volumes
         * were created; moving static data is background wear leveling, not done here. */
        if (nand_ubi_eba_peb_get_state(&r.dev->eba, pnum) == UBI_PEB_BAD ||
                (int32_t)pnum == r.dev->vtbl_pnum[0] || (int32_t)pnum == r.dev->vtbl_pnum[1]) {
            continue;
        }
        uint64_t ec = peb_ec_on_flash(&r.nand->bdl, pnum, r.dev->page_size, r.dev->peb_size);
        min_ec = std::min(min_ec, ec);
        max_ec = std::max(max_ec, ec);
    }
    INFO("ec range " << min_ec << ".." << max_ec << " over " << r.dev->peb_count << " PEBs");
    printf("wear: ec range %llu..%llu over %u PEBs\n", (unsigned long long)min_ec, (unsigned long long)max_ec,
           (unsigned)r.dev->peb_count);
    REQUIRE(min_ec >= 5);          /* every PEB, not just a few, has been cycled */
    REQUIRE(max_ec - min_ec <= 3); /* sampled allocation, not the exact minimum */
    release_rig(r);
}
