/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* Random power-cut soak for esp_nand_ubi on the emulated NAND. Each cycle arms a cut
 * after a random number of flash operations, runs the ptest workload until the cut
 * fires, then re-attaches and checks UBI and the workload. A failing run prints the
 * arguments that replay it. See docs/plans/2026-10-08-esp-nand-ubi-powercut-stress-design.md. */

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esp_nand_ubi.h"
#include "esp_nand_blockdev.h"
#include "faulty_nand.hpp"
#include "nand_linux_mmap_emul.h"
#include "ptest_ubi.h"
#include "spi_nand_flash.h"

extern "C" {
#include "nand_ubi_priv.h"
void start_cpu0(void);
}

using namespace nand_fault;

namespace {

int s_argc;
char **s_argv;

struct options {
    uint32_t seed = 1;
    uint32_t cycles = 1000;
    uint32_t max_ops = 300;
    uint32_t chip_mb = 16;
    uint32_t data_lebs = 32;
    uint32_t pages = 4;
    bool clean = true;
    bool partial = true;
    bool torn = true;
    bool verbose = false;
};

void usage()
{
    printf("usage: host_ptest [--seed N] [--cycles N] [--max-ops N] [--faults clean,partial,torn]\n"
           "                  [--chip-mb N] [--data-lebs N] [--pages N] [-v]\n"
           "  --max-ops    a cut fires after 0 .. max-ops-1 flash operations (default 300)\n"
           "  --pages      pages written per LEB rewrite (default 4)\n");
}

bool parse(options &o)
{
    for (int i = 1; i < s_argc; i++) {
        std::string a = s_argv[i];
        auto num = [&](uint32_t &dst) {
            if (i + 1 >= s_argc) {
                return false;
            }
            dst = (uint32_t)strtoul(s_argv[++i], nullptr, 0);
            return true;
        };
        bool ok = true;
        if (a == "--seed") {
            ok = num(o.seed);
        } else if (a == "--cycles") {
            ok = num(o.cycles);
        } else if (a == "--max-ops") {
            ok = num(o.max_ops);
        } else if (a == "--chip-mb") {
            ok = num(o.chip_mb);
        } else if (a == "--data-lebs") {
            ok = num(o.data_lebs);
        } else if (a == "--pages") {
            ok = num(o.pages);
        } else if (a == "--faults" && i + 1 < s_argc) {
            std::string f = s_argv[++i];
            o.clean = f.find("clean") != std::string::npos;
            o.partial = f.find("partial") != std::string::npos;
            o.torn = f.find("torn") != std::string::npos;
            ok = o.clean || o.partial || o.torn;
        } else if (a == "-v") {
            o.verbose = true;
        } else {
            ok = false;
        }
        if (!ok) {
            return false;
        }
    }
    return o.max_ops > 0 && o.cycles > 0;
}

const char *kind_name(cut_kind k)
{
    return k == cut_kind::clean ? "clean" : k == cut_kind::partial ? "partial" : "torn";
}

uint32_t xorshift32(uint32_t &x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

struct rig {
    faulty_nand *nand = nullptr;
    nand_ubi_device_t *dev = nullptr;
    esp_blockdev_handle_t vol = nullptr;
    ptest_ubi_t *work = nullptr;
};

[[noreturn]] void fail_run(const options &o, uint32_t cycle, const char *what)
{
    printf("FAIL cycle=%" PRIu32 ": %s\n", cycle, what);
    printf("replay: host_ptest --seed %" PRIu32 " --cycles %" PRIu32 " --max-ops %" PRIu32
           " --chip-mb %" PRIu32 " --data-lebs %" PRIu32 " --pages %" PRIu32 " --faults %s%s%s\n",
           o.seed, cycle, o.max_ops, o.chip_mb, o.data_lebs, o.pages,
           o.clean ? "clean," : "", o.partial ? "partial," : "", o.torn ? "torn" : "");
    fflush(stdout);
    exit(1);
}

/* After a read-write attach nothing is left to erase, and every used PEB is a
 * volume-table copy, the attach checkpoint, or mapped to exactly one LEB. */
bool peb_accounting_ok(const nand_ubi_device_t *dev, char *msg, size_t len)
{
    uint32_t used = 0;
    for (uint32_t pnum = 0; pnum < dev->peb_count; pnum++) {
        uint8_t state = nand_ubi_eba_peb_get_state(&dev->eba, pnum);
        if (state == UBI_PEB_ERASE_PENDING) {
            snprintf(msg, len, "PEB %" PRIu32 " still erase-pending after attach", pnum);
            return false;
        }
        used += state == UBI_PEB_USED;
    }
    uint32_t mapped = 0;
    for (uint32_t i = 0; i < dev->leb_count; i++) {
        mapped += nand_ubi_eba_get_pnum(&dev->eba, i) != UBI_LEB_UNMAPPED;
    }
    uint32_t meta = UBI_LAYOUT_VOLUME_EBS + (dev->ckpt_pnum >= 0 ? 1u : 0u);
    if (used != mapped + meta) {
        snprintf(msg, len, "%" PRIu32 " used PEBs but %" PRIu32 " mapped LEBs + %u metadata PEBs",
                 used, mapped, (unsigned)meta);
        return false;
    }
    return true;
}

void setup(rig &r, const options &o)
{
    nand_file_mmap_emul_config_t conf = {"", o.chip_mb * 1024u * 1024u, false};
    spi_nand_flash_config_t cfg = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    esp_blockdev_handle_t inner = nullptr;
    if (nand_flash_get_blockdev(&cfg, &inner) != ESP_OK) {
        fail_run(o, 0, "cannot create the emulated NAND");
    }
    r.nand = wrap_faulty_nand(inner);
    uint32_t vol_id = 0;
    ptest_ubi_cfg_t pcfg = {o.data_lebs, o.pages, o.seed};
    ptest_ubi_result_t res;
    if (nand_ubi_attach(&r.nand->bdl, nullptr, &r.dev) != ESP_OK ||
            nand_ubi_create_volume(r.dev, "ptest", UBI_VID_DYNAMIC, o.data_lebs + PTEST_UBI_JOURNAL_LEBS,
                                   &vol_id) != ESP_OK ||
            nand_ubi_open_volume(r.dev, vol_id, &r.vol) != ESP_OK ||
            ptest_ubi_create(r.vol, &pcfg, &r.work) != ESP_OK ||
            ptest_ubi_verify(r.work, &res) != ESP_OK || !res.ok) {
        fail_run(o, 0, "setup failed (chip too small for --data-lebs?)");
    }
}

/* Power comes back: re-attach, check UBI's own state, then verify the workload. */
void reboot(rig &r, const options &o, uint32_t cycle, ptest_ubi_result_t &res)
{
    char msg[160];
    /* Drop the RAM state while the chip is still dead: a detach on a live chip would
     * write a checkpoint, which a power cut never does. */
    r.nand->dead = true;
    r.vol->ops->release(r.vol);
    nand_ubi_detach(r.dev);
    r.dev = nullptr;
    r.nand->dead = false;
    r.nand->cut_after = -1;
    esp_err_t err = nand_ubi_attach(&r.nand->bdl, nullptr, &r.dev);
    if (err != ESP_OK) {
        snprintf(msg, sizeof(msg), "attach failed: 0x%x", err);
        fail_run(o, cycle, msg);
    }
    if (!r.dev->vtbl_copy_good[0] || !r.dev->vtbl_copy_good[1]) {
        fail_run(o, cycle, "a volume-table copy is not good after attach");
    }
    if (!peb_accounting_ok(r.dev, msg, sizeof(msg))) {
        fail_run(o, cycle, msg);
    }
    if (nand_ubi_open_volume(r.dev, 0, &r.vol) != ESP_OK) {
        fail_run(o, cycle, "cannot open the ptest volume");
    }
    ptest_ubi_set_volume(r.work, r.vol);
    if (ptest_ubi_verify(r.work, &res) != ESP_OK) {
        fail_run(o, cycle, "verify: I/O error");
    }
    if (!res.ok) {
        snprintf(msg, sizeof(msg), "verify: %s", res.first_error);
        fail_run(o, cycle, msg);
    }
}

int run(const options &o)
{
    cut_kind kinds[3];
    uint32_t nkinds = 0;
    if (o.clean) {
        kinds[nkinds++] = cut_kind::clean;
    }
    if (o.partial) {
        kinds[nkinds++] = cut_kind::partial;
    }
    if (o.torn) {
        kinds[nkinds++] = cut_kind::torn;
    }

    rig r;
    setup(r, o);
    uint32_t rng = o.seed * 2654435761u + 0x9E3779B9u;
    uint32_t cuts = 0, no_cut = 0, in_flight = 0, journal_torn = 0;
    uint32_t free_min = UINT32_MAX, free_max = 0;

    for (uint32_t cycle = 1; cycle <= o.cycles; cycle++) {
        cut_kind kind = kinds[xorshift32(rng) % nkinds];
        long after = (long)(xorshift32(rng) % o.max_ops);
        r.nand->cut_type = kind;
        r.nand->cut_after = after;

        bool fired = false;
        for (int step = 0; step < 10000 && !fired; step++) {
            fired = ptest_ubi_step(r.work) != ESP_OK;
        }
        if (!fired) {
            r.nand->cut_after = -1;
            no_cut++;
            continue;
        }
        if (!r.nand->dead) {
            fail_run(o, cycle, "a step failed without a cut (I/O error)");
        }
        cuts++;

        ptest_ubi_result_t res;
        reboot(r, o, cycle, res);
        in_flight += res.in_flight;
        journal_torn += res.journal_torn;
        uint32_t free_pebs = 0;
        for (uint32_t pnum = 0; pnum < r.dev->peb_count; pnum++) {
            free_pebs += nand_ubi_eba_peb_is_free(&r.dev->eba, pnum);
        }
        free_min = free_pebs < free_min ? free_pebs : free_min;
        free_max = free_pebs > free_max ? free_pebs : free_max;

        if (o.verbose) {
            printf("cycle %" PRIu32 ": %s cut after %ld ops, in-flight %" PRIu32 ", free PEBs %" PRIu32 "\n",
                   cycle, kind_name(kind), after, res.in_flight, free_pebs);
        } else if (cycle % 100 == 0) {
            printf("cycle %" PRIu32 "/%" PRIu32 ": %" PRIu32 " cuts, %" PRIu32 " in-flight\n",
                   cycle, o.cycles, cuts, in_flight);
            fflush(stdout);
        }
    }

    printf("PASS seed=%" PRIu32 " cycles=%" PRIu32 ": %" PRIu32 " cuts (%" PRIu32 " cycles without a cut), "
           "%" PRIu32 " in-flight rewrites, %" PRIu32 " torn journal records, free PEBs %" PRIu32 "..%" PRIu32 "\n",
           o.seed, o.cycles, cuts, no_cut, in_flight, journal_torn, free_min, free_max);

    ptest_ubi_destroy(r.work);
    r.vol->ops->release(r.vol);
    nand_ubi_detach(r.dev);
    r.nand->bdl.ops->release(&r.nand->bdl);
    return 0;
}

} // namespace

/* Overrides ESP-IDF's weak Linux-target main() to keep argv, then starts IDF as usual. */
int main(int argc, char **argv)
{
    s_argc = argc;
    s_argv = argv;
    start_cpu0();
    return 0;
}

extern "C" void app_main(void)
{
    options o;
    if (!parse(o)) {
        usage();
        exit(2);
    }
    printf("host_ptest: seed=%" PRIu32 " cycles=%" PRIu32 " max-ops=%" PRIu32 " chip=%" PRIu32 " MiB "
           "data-lebs=%" PRIu32 " pages=%" PRIu32 "\n",
           o.seed, o.cycles, o.max_ops, o.chip_mb, o.data_lebs, o.pages);
    exit(run(o));
}
