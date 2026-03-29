/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nand_emul_advanced.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("advanced header types compile and have correct sizes", "[advanced][types]")
{
    // Verify struct sizes match the snapshot format spec
    // snapshot_header_t must be exactly 64 bytes
    STATIC_REQUIRE(sizeof(snapshot_header_t) == 64);

    // Verify page_metadata_t has the expected fields (compile-time probe)
    page_metadata_t pm = {};
    (void)pm.page_num;
    (void)pm.program_count;
    (void)pm.program_count_total;
    (void)pm.read_count;
    (void)pm.read_count_total;
    (void)pm.first_program_timestamp;
    (void)pm.last_program_timestamp;

    // Verify block_metadata_t has the expected fields
    block_metadata_t bm = {};
    (void)bm.block_num;
    (void)bm.erase_count;
    (void)bm.first_erase_timestamp;
    (void)bm.last_erase_timestamp;
    (void)bm.total_page_programs;
    (void)bm.total_page_programs_total;
    (void)bm.is_bad_block;

    // Verify nand_wear_stats_t has write-amplification fields
    nand_wear_stats_t ws = {};
    (void)ws.logical_write_bytes_recorded;
    (void)ws.write_amplification;
    (void)ws.wear_leveling_variation;

    // Verify vtable types compile
    nand_metadata_backend_ops_t backend_ops = {};
    nand_failure_model_ops_t    failure_ops  = {};
    (void)backend_ops;
    (void)failure_ops;

    // Verify config struct compiles
    nand_emul_advanced_config_t adv_cfg = {};
    (void)adv_cfg.metadata_backend;
    (void)adv_cfg.failure_model;
    (void)adv_cfg.track_block_level;
    (void)adv_cfg.track_page_level;

    // Verify histogram types
    nand_wear_histogram_t hist = {};
    REQUIRE(hist.n_bins == 0);
    nand_wear_histograms_t hists = {};
    (void)hists.block_erase_count;
    (void)hists.page_lifetime_programs;

    REQUIRE(true); // reached here = compiled correctly
}
