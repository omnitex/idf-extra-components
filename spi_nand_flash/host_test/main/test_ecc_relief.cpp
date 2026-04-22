/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Host tests for the ECC page relief wear-leveling feature.
 *
 * Tests use the Linux NAND emulator.  ECC events are injected via
 * nand_wrap_inject_ecc_event().  Observable behavior is verified through
 * spi_nand_flash_get_ecc_relief_stats() and ordinary read/write calls.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
#include "esp_blockdev.h"
#endif
#include "spi_nand_flash.h"
#include "spi_nand_flash_test_helpers.h"
#include "nand_linux_mmap_emul.h"
#include "nand_private/nand_impl_wrap.h"
#include "nand_device_types.h"

#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
static esp_blockdev_handle_t s_ecc_relief_wl_bdl = nullptr;
#endif

/** Init path matches production: layered init when BDL is enabled in sdkconfig. */
static esp_err_t ecc_relief_test_init(spi_nand_flash_config_t *cfg, spi_nand_flash_device_t **out_dev)
{
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    esp_err_t ret = spi_nand_flash_init_with_layers(cfg, &s_ecc_relief_wl_bdl);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_blockdev_handle_t flash_bdl = (esp_blockdev_handle_t)s_ecc_relief_wl_bdl->ctx;
    *out_dev = (spi_nand_flash_device_t *)flash_bdl->ctx;
    return ESP_OK;
#else
    return spi_nand_flash_init_device(cfg, out_dev);
#endif
}

static void ecc_relief_test_deinit(spi_nand_flash_device_t *dev)
{
    (void)dev;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    if (s_ecc_relief_wl_bdl) {
        s_ecc_relief_wl_bdl->ops->release(s_ecc_relief_wl_bdl);
        s_ecc_relief_wl_bdl = nullptr;
    }
#else
    spi_nand_flash_deinit_device(dev);
#endif
}

} // namespace

/* Host tests run one device at a time; emul config must outlive init. */
static nand_file_mmap_emul_config_t g_relief_emul_conf;

/**
 * Init with ECC relief plus mmap size and Dhara gc_factor (passed through to
 * dhara_map_init as gc_ratio; 0 is bumped to 1 inside Dhara).
 */
static spi_nand_flash_device_t *init_device_with_relief_ex(
    uint8_t mid_threshold,
    uint8_t high_threshold,
    uint8_t mid_count_limit,
    uint8_t max_consecutive,
    uint16_t map_capacity,
    uint8_t gc_factor,
    size_t flash_file_size)
{
    g_relief_emul_conf = {"", flash_file_size, false};
    spi_nand_flash_config_t cfg = {};
    cfg.emul_conf = &g_relief_emul_conf;
    cfg.gc_factor = gc_factor;
    cfg.io_mode = SPI_NAND_IO_MODE_SIO;
    cfg.flags = 0;
    cfg.ecc_relief.enabled              = true;
    cfg.ecc_relief.mid_threshold        = mid_threshold;
    cfg.ecc_relief.high_threshold       = high_threshold;
    cfg.ecc_relief.mid_count_limit      = mid_count_limit;
    cfg.ecc_relief.max_consecutive_relief = max_consecutive;
    cfg.ecc_relief.map_capacity         = map_capacity;

    spi_nand_flash_device_t *dev = nullptr;
    REQUIRE(ecc_relief_test_init(&cfg, &dev) == ESP_OK);
    return dev;
}

/* Default: 50 MiB backing, gc_factor 0 (Dhara still uses minimum gc_ratio 1). */
static spi_nand_flash_device_t *init_device_with_relief(
    uint8_t mid_threshold,
    uint8_t high_threshold,
    uint8_t mid_count_limit,
    uint8_t max_consecutive,
    uint16_t map_capacity)
{
    return init_device_with_relief_ex(mid_threshold, high_threshold, mid_count_limit,
                                      max_consecutive, map_capacity, 0,
                                      50 * 1024 * 1024);
}

/* --------------------------------------------------------------------------
 * SC-01: clean read → no map entry created
 * --------------------------------------------------------------------------
 * We never inject any ECC event; stats should show 0 entries used.
 */
TEST_CASE("SC-01: no ECC event → no map entry", "[ecc_relief]")
{
    auto *dev = init_device_with_relief(2, 4, 3, 4, 64);

    spi_nand_ecc_relief_stats_t stats = {};
    REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
    CHECK(stats.map_entries_used == 0);
    CHECK(stats.pages_pending_relief == 0);
    CHECK(stats.total_pages_relieved == 0);
    CHECK(stats.map_capacity == 64);
    CHECK(stats.consecutive_cap_hits == 0);

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * SC-09: diagnostic stats reflect relieved page count
 * --------------------------------------------------------------------------
 * After relief fires (SC-04 scenario), total_pages_relieved in stats must
 * match the number of pages actually relieved during enqueue.
 * --------------------------------------------------------------------------
 */
TEST_CASE("SC-09: stats.total_pages_relieved reflects actual relief count", "[ecc_relief]")
{
    /* max_consecutive_relief=1 so exactly 1 relief per write before forced prog */
    auto *dev = init_device_with_relief(2, 4, 3, 1, 256);

    uint32_t page_size = 0;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);

    /* Warmup write to trigger the initial block erase. */
    std::vector<uint8_t> warmup(page_size, 0x00);
    REQUIRE(spi_nand_flash_write_page(dev, warmup.data(), 0) == ESP_OK);

    /* Inject on a wide range after the erase. */
    const uint32_t inject_count = 128;
    for (uint32_t p = 0; p < inject_count; p++) {
        nand_wrap_inject_ecc_event(dev, p, NAND_ECC_7_8_BITS_CORRECTED);
    }

    {
        spi_nand_ecc_relief_stats_t s = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &s) == ESP_OK);
        CHECK(s.map_entries_used == inject_count);
        CHECK(s.pages_pending_relief == inject_count);
        CHECK(s.total_pages_relieved == 0);
        CHECK(s.consecutive_cap_hits == 0);
    }

    /* Perform 3 writes; each enqueue may relieve at most 1 page (cap=1). */
    std::vector<uint8_t> buf(page_size, 0xCC);
    for (uint32_t i = 1; i <= 3; i++) {
        REQUIRE(spi_nand_flash_write_page(dev, buf.data(), i) == ESP_OK);
    }

    spi_nand_ecc_relief_stats_t stats = {};
    REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
    /* Each logical write ran one journal enqueue → at most one relief each. */
    CHECK(stats.total_pages_relieved >= 1);
    CHECK(stats.total_pages_relieved <= 3);
    /* With max_consecutive_relief=1, every relieved page is followed by a cap hit before prog. */
    CHECK(stats.consecutive_cap_hits == stats.total_pages_relieved);

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * T10.10: BDL path — ECC event reaches on_page_read_ecc and updates map
 * --------------------------------------------------------------------------
 * This test verifies that when the stack is initialized via the BDL path
 * (spi_nand_flash_init_with_layers), the ECC observation callback is still
 * wired up correctly and relief map entries are created on injection.
 *
 * Under CONFIG_NAND_FLASH_ENABLE_BDL the init uses the layered BDL path;
 * under plain WL it uses spi_nand_flash_init_device — both are exercised by
 * the ecc_relief_test_init() helper, making the test meaningful in both
 * build configurations.
 * --------------------------------------------------------------------------
 */
TEST_CASE("T10.10: BDL path ECC event updates relief map", "[ecc_relief]")
{
    auto *dev = init_device_with_relief(2, 4, 3, 4, 64);

    /* Inject a HIGH ECC event (mimics nand_read() ECC event on any read path) */
    nand_wrap_inject_ecc_event(dev, 42, NAND_ECC_7_8_BITS_CORRECTED);

    spi_nand_ecc_relief_stats_t stats = {};
    REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
    /* The callback must have been registered regardless of BDL layer. */
    CHECK(stats.map_entries_used == 1);
    CHECK(stats.pages_pending_relief == 1);
    CHECK(stats.total_pages_relieved == 0);
    CHECK(stats.map_capacity == 64);
    CHECK(stats.consecutive_cap_hits == 0);

    ecc_relief_test_deinit(dev);
}


/* --------------------------------------------------------------------------
 * SC-03: repeated MID reads reach mid_count_limit → PENDING set
 * --------------------------------------------------------------------------
 */
TEST_CASE("SC-03: MID reads accumulate and flag page", "[ecc_relief]")
{
    /* mid_threshold=2, high_threshold=4, mid_count_limit=3 */
    auto *dev = init_device_with_relief(2, 4, 3, 4, 64);

    /* MID: mid_threshold=2, high_threshold=4 → use enum values in [2, 4) (here: 3). */
    nand_wrap_inject_ecc_event(dev, 20, NAND_ECC_4_TO_6_BITS_CORRECTED);
    nand_wrap_inject_ecc_event(dev, 20, NAND_ECC_4_TO_6_BITS_CORRECTED);

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        /* After 2 MID hits (limit=3): one tracked page, not yet at PENDING threshold */
        CHECK(stats.map_entries_used == 1);
        CHECK(stats.pages_pending_relief == 0);
        CHECK(stats.total_pages_relieved == 0);
        CHECK(stats.map_capacity == 64);
        CHECK(stats.consecutive_cap_hits == 0);
    }

    /* Third MID hit → should be flagged */
    nand_wrap_inject_ecc_event(dev, 20, NAND_ECC_4_TO_6_BITS_CORRECTED);

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        /* At limit: same map slot, now PENDING */
        CHECK(stats.map_entries_used == 1);
        CHECK(stats.pages_pending_relief == 1);
        CHECK(stats.map_capacity == 64);
        CHECK(stats.consecutive_cap_hits == 0);
        CHECK(stats.total_pages_relieved == 0);
    }

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * SC-04: flagged page at j->head → filler written, flag cleared
 * --------------------------------------------------------------------------
 * On a fresh emulated flash Dhara erases block 0 on the very first journal
 * enqueue (prepare_head), which would evict any pre-injected entries.
 * Strategy: do one warmup write first (triggers the initial erase and leaves
 * j->head somewhere inside block 0), then inject ECC events on a wide range
 * so j->head is guaranteed to be covered.  The next write must encounter at
 * least one PENDING page and relieve it.
 *
 * Observable: total_pages_relieved >= 1 after the second write.
 *             pages_pending_relief has decreased (relief clears the flag).
 * --------------------------------------------------------------------------
 */
TEST_CASE("SC-04: flagged head page → relief filler written, flag cleared", "[ecc_relief]")
{
    auto *dev = init_device_with_relief(2, 4, 3, 4, 256);

    uint32_t page_size = 0;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);
    REQUIRE(page_size > 0);

    /* Warmup write: triggers initial block erase; j->head advances into block 0. */
    std::vector<uint8_t> warmup(page_size, 0x00);
    REQUIRE(spi_nand_flash_write_page(dev, warmup.data(), 0) == ESP_OK);

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        CHECK(stats.map_entries_used == 0);
        CHECK(stats.pages_pending_relief == 0);
        CHECK(stats.total_pages_relieved == 0);
        CHECK(stats.consecutive_cap_hits == 0);
    }

    /* Inject HIGH ECC on a wide range covering j->head's current position. */
    const uint32_t inject_count = 128;
    for (uint32_t p = 0; p < inject_count; p++) {
        nand_wrap_inject_ecc_event(dev, p, NAND_ECC_7_8_BITS_CORRECTED);
    }

    uint32_t pending_before = 0;
    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        pending_before = stats.pages_pending_relief;
        REQUIRE(pending_before > 0);
        CHECK(stats.map_entries_used == inject_count);
        CHECK(stats.pages_pending_relief == inject_count);
        CHECK(stats.total_pages_relieved == 0);
        CHECK(stats.map_capacity == 256);
    }

    /* Write another logical page; Dhara enqueues → relief check fires. */
    std::vector<uint8_t> buf(page_size, 0xA5);
    REQUIRE(spi_nand_flash_write_page(dev, buf.data(), 1) == ESP_OK);

    spi_nand_ecc_relief_stats_t stats = {};
    REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
    /* At least one page was relieved (filler written, not real prog). */
    CHECK(stats.total_pages_relieved >= 1);
    /* Relieved pages had their PENDING flag cleared. */
    CHECK(stats.pages_pending_relief < pending_before);
    CHECK(stats.map_entries_used == inject_count);

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * SC-05: data written to page after the relieved one is correct
 * --------------------------------------------------------------------------
 * Same warmup strategy as SC-04.  After relief fires, data written to the
 * next available (non-flagged) page must be readable back without corruption.
 * --------------------------------------------------------------------------
 */
TEST_CASE("SC-05: data written after relief page is readable", "[ecc_relief]")
{
    auto *dev = init_device_with_relief(2, 4, 3, 4, 256);

    uint32_t page_size = 0;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);

    /* Warmup write to trigger the initial block erase. */
    std::vector<uint8_t> warmup(page_size, 0x00);
    REQUIRE(spi_nand_flash_write_page(dev, warmup.data(), 0) == ESP_OK);

    const uint32_t inject_count = 128;
    /* Inject HIGH ECC on a wide range after the erase. */
    for (uint32_t p = 0; p < inject_count; p++) {
        nand_wrap_inject_ecc_event(dev, p, NAND_ECC_7_8_BITS_CORRECTED);
    }

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        CHECK(stats.map_entries_used == inject_count);
        CHECK(stats.pages_pending_relief == inject_count);
        CHECK(stats.total_pages_relieved == 0);
    }

    /* Write a known pattern to logical page 1. */
    std::vector<uint8_t> write_buf(page_size);
    for (uint32_t i = 0; i < page_size; i++) {
        write_buf[i] = static_cast<uint8_t>(i & 0xFF);
    }
    REQUIRE(spi_nand_flash_write_page(dev, write_buf.data(), 1) == ESP_OK);

    /* Read back and verify data integrity. */
    std::vector<uint8_t> read_buf(page_size, 0);
    REQUIRE(spi_nand_flash_read_page(dev, read_buf.data(), 1) == ESP_OK);
    CHECK(read_buf == write_buf);

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        CHECK(stats.total_pages_relieved >= 1);
        CHECK(stats.pages_pending_relief < inject_count);
        CHECK(stats.map_entries_used == inject_count);
    }

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * SC-06: consecutive-skip cap enforced — write forced at cap limit
 * --------------------------------------------------------------------------
 * Set max_consecutive_relief=2.  Flag enough pages that the journal
 * is guaranteed to hit the cap on a single write.
 * After the writes: consecutive_cap_hits >= 1.
 * --------------------------------------------------------------------------
 */
TEST_CASE("SC-06: consecutive-skip cap enforced", "[ecc_relief]")
{
    /* max_consecutive_relief = 2 */
    auto *dev = init_device_with_relief(2, 4, 3, 2, 256);

    uint32_t page_size = 0;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);

    /* Warmup write to trigger the initial block erase. */
    std::vector<uint8_t> warmup(page_size, 0x00);
    REQUIRE(spi_nand_flash_write_page(dev, warmup.data(), 0) == ESP_OK);

    /* Flag a wide range so the cap is hit. */
    const uint32_t inject_count = 128;
    for (uint32_t p = 0; p < inject_count; p++) {
        nand_wrap_inject_ecc_event(dev, p, NAND_ECC_7_8_BITS_CORRECTED);
    }

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        CHECK(stats.map_entries_used == inject_count);
        CHECK(stats.pages_pending_relief == inject_count);
        CHECK(stats.total_pages_relieved == 0);
        CHECK(stats.consecutive_cap_hits == 0);
    }

    /* Perform several writes to ensure the cap is exercised. */
    std::vector<uint8_t> buf(page_size, 0xBB);
    for (uint32_t i = 1; i <= 4; i++) {
        REQUIRE(spi_nand_flash_write_page(dev, buf.data(), i) == ESP_OK);
    }

    spi_nand_ecc_relief_stats_t stats = {};
    REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
    CHECK(stats.consecutive_cap_hits >= 1);
    CHECK(stats.total_pages_relieved >= 1);
    CHECK(stats.pages_pending_relief < inject_count);
    CHECK(stats.map_entries_used == inject_count);

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * SC-07: successful erase evicts map entries for that block
 * --------------------------------------------------------------------------
 */
TEST_CASE("SC-07: erase evicts relief map entries", "[ecc_relief]")
{
    auto *dev = init_device_with_relief(2, 4, 3, 4, 64);

    uint32_t page_size, block_size;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);
    REQUIRE(spi_nand_flash_get_block_size(dev, &block_size) == ESP_OK);
    uint32_t pages_per_block = block_size / page_size;

    /* Flag a few physical pages in block 5 */
    uint32_t block5_page = 5 * pages_per_block;
    nand_wrap_inject_ecc_event(dev, block5_page + 0, NAND_ECC_7_8_BITS_CORRECTED);
    nand_wrap_inject_ecc_event(dev, block5_page + 1, NAND_ECC_7_8_BITS_CORRECTED);
    nand_wrap_inject_ecc_event(dev, block5_page + 2, NAND_ECC_7_8_BITS_CORRECTED);

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        REQUIRE(stats.map_entries_used == 3);
        REQUIRE(stats.pages_pending_relief == 3);
        CHECK(stats.total_pages_relieved == 0);
        CHECK(stats.consecutive_cap_hits == 0);
    }

    /* Erase block 5 through the low-level wrap (which calls dhara_nand_erase internally) */
    REQUIRE(nand_wrap_erase_block(dev, 5) == ESP_OK);

    {
        spi_nand_ecc_relief_stats_t stats = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_OK);
        CHECK(stats.map_entries_used == 0);
        CHECK(stats.pages_pending_relief == 0);
        CHECK(stats.map_capacity == 64);
        CHECK(stats.total_pages_relieved == 0);
        CHECK(stats.consecutive_cap_hits == 0);
    }

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * SC-08: feature disabled → no map ops, stats returns NOT_SUPPORTED
 * --------------------------------------------------------------------------
 */
TEST_CASE("SC-08: feature disabled behaves as before", "[ecc_relief]")
{
    static nand_file_mmap_emul_config_t conf = {"", 50 * 1024 * 1024, false};
    spi_nand_flash_config_t cfg = {};
    cfg.emul_conf = &conf;
    cfg.gc_factor = 0;
    cfg.io_mode = SPI_NAND_IO_MODE_SIO;
    /* ecc_relief.enabled not set → false */

    spi_nand_flash_device_t *dev = nullptr;
    REQUIRE(ecc_relief_test_init(&cfg, &dev) == ESP_OK);

    spi_nand_ecc_relief_stats_t stats = {};
    CHECK(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_ERR_NOT_SUPPORTED);

    /* No crash when injecting events with NULL callback */
    nand_wrap_inject_ecc_event(dev, 10, NAND_ECC_7_8_BITS_CORRECTED);

    CHECK(spi_nand_flash_get_ecc_relief_stats(dev, &stats) == ESP_ERR_NOT_SUPPORTED);

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * CC-A: relief map exactly full (all PENDING) — 33rd HIGH cannot add a slot
 * --------------------------------------------------------------------------
 */
TEST_CASE("CC-A: map full of HIGH then extra HIGH does not grow map", "[ecc_relief]")
{
    constexpr uint16_t map_cap = 32;
    auto *dev = init_device_with_relief_ex(2, 4, 3, 4, map_cap, 0, 50 * 1024 * 1024);

    uint32_t page_size = 0;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);

    std::vector<uint8_t> warmup(page_size, 0x00);
    REQUIRE(spi_nand_flash_write_page(dev, warmup.data(), 0) == ESP_OK);

    for (uint32_t p = 0; p < map_cap; p++) {
        nand_wrap_inject_ecc_event(dev, p, NAND_ECC_7_8_BITS_CORRECTED);
    }

    {
        spi_nand_ecc_relief_stats_t s = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &s) == ESP_OK);
        CHECK(s.map_entries_used == map_cap);
        CHECK(s.pages_pending_relief == map_cap);
        CHECK(s.map_capacity == map_cap);
    }

    nand_wrap_inject_ecc_event(dev, 100, NAND_ECC_7_8_BITS_CORRECTED);

    {
        spi_nand_ecc_relief_stats_t s = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &s) == ESP_OK);
        CHECK(s.map_entries_used == map_cap);
        CHECK(s.pages_pending_relief == map_cap);
    }

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * CC-A1: full map evicts a non-PENDING (MID-only) entry for a new HIGH
 * --------------------------------------------------------------------------
 */
TEST_CASE("CC-A1: map full evicts non-PENDING slot for new HIGH", "[ecc_relief]")
{
    constexpr uint16_t map_cap = 32;
    auto *dev = init_device_with_relief_ex(2, 4, 3, 4, map_cap, 0, 50 * 1024 * 1024);

    uint32_t page_size = 0;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);

    std::vector<uint8_t> warmup(page_size, 0x00);
    REQUIRE(spi_nand_flash_write_page(dev, warmup.data(), 0) == ESP_OK);

    for (uint32_t p = 0; p < map_cap - 1; p++) {
        nand_wrap_inject_ecc_event(dev, p, NAND_ECC_7_8_BITS_CORRECTED);
    }
    const uint32_t mid_track_page = 5000;
    nand_wrap_inject_ecc_event(dev, mid_track_page, NAND_ECC_4_TO_6_BITS_CORRECTED);
    nand_wrap_inject_ecc_event(dev, mid_track_page, NAND_ECC_4_TO_6_BITS_CORRECTED);

    {
        spi_nand_ecc_relief_stats_t s = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &s) == ESP_OK);
        CHECK(s.map_entries_used == map_cap);
        CHECK(s.pages_pending_relief == map_cap - 1);
    }

    nand_wrap_inject_ecc_event(dev, 9000, NAND_ECC_7_8_BITS_CORRECTED);

    {
        spi_nand_ecc_relief_stats_t s = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &s) == ESP_OK);
        CHECK(s.map_entries_used == map_cap);
        CHECK(s.pages_pending_relief == map_cap);
    }

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * CC-B: map full of HIGH — MID on new page is silently skipped (no growth)
 * --------------------------------------------------------------------------
 */
TEST_CASE("CC-B: map full of HIGH then MID on new page does not grow map", "[ecc_relief]")
{
    constexpr uint16_t map_cap = 16;
    auto *dev = init_device_with_relief_ex(2, 4, 3, 4, map_cap, 0, 50 * 1024 * 1024);

    uint32_t page_size = 0;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);

    std::vector<uint8_t> warmup(page_size, 0x00);
    REQUIRE(spi_nand_flash_write_page(dev, warmup.data(), 0) == ESP_OK);

    for (uint32_t p = 0; p < map_cap; p++) {
        nand_wrap_inject_ecc_event(dev, p, NAND_ECC_7_8_BITS_CORRECTED);
    }

    {
        spi_nand_ecc_relief_stats_t s = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &s) == ESP_OK);
        CHECK(s.map_entries_used == map_cap);
        CHECK(s.pages_pending_relief == map_cap);
    }

    nand_wrap_inject_ecc_event(dev, 9999, NAND_ECC_4_TO_6_BITS_CORRECTED);
    nand_wrap_inject_ecc_event(dev, 9999, NAND_ECC_4_TO_6_BITS_CORRECTED);

    {
        spi_nand_ecc_relief_stats_t s = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &s) == ESP_OK);
        CHECK(s.map_entries_used == map_cap);
        CHECK(s.pages_pending_relief == map_cap);
    }

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * CC-C: hammer same logical page after wide HIGH inject
 * --------------------------------------------------------------------------
 */
TEST_CASE("CC-C: hammer same logical page with relief inject", "[ecc_relief]")
{
    auto *dev = init_device_with_relief(2, 4, 3, 4, 256);

    uint32_t page_size = 0;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);

    std::vector<uint8_t> warmup(page_size, 0x00);
    REQUIRE(spi_nand_flash_write_page(dev, warmup.data(), 0) == ESP_OK);

    const uint32_t inject_count = 128;
    for (uint32_t p = 0; p < inject_count; p++) {
        nand_wrap_inject_ecc_event(dev, p, NAND_ECC_7_8_BITS_CORRECTED);
    }

    const uint32_t hammer_iters = 120;
    const uint32_t logical_page = 1;
    uint32_t relieved_before = 0;
    {
        spi_nand_ecc_relief_stats_t s = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &s) == ESP_OK);
        relieved_before = s.total_pages_relieved;
    }

    std::vector<uint8_t> buf(page_size);
    for (uint32_t iter = 0; iter < hammer_iters; iter++) {
        memset(buf.data(), static_cast<int>(0x10 + (iter & 0x0F)), page_size);
        REQUIRE(spi_nand_flash_write_page(dev, buf.data(), logical_page) == ESP_OK);
    }

    std::vector<uint8_t> read_buf(page_size, 0);
    REQUIRE(spi_nand_flash_read_page(dev, read_buf.data(), logical_page) == ESP_OK);
    CHECK(read_buf == buf);

    {
        spi_nand_ecc_relief_stats_t s = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &s) == ESP_OK);
        CHECK(s.total_pages_relieved >= relieved_before);
        CHECK(s.map_entries_used <= s.map_capacity);
        /* Journal activity can erase blocks and evict relief entries; count may drop below inject_count. */
        CHECK(s.map_entries_used <= inject_count);
    }

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * CC-D: hot set — round-robin small logical page ring under relief inject
 * --------------------------------------------------------------------------
 */
TEST_CASE("CC-D: hot set round-robin logical pages with relief inject", "[ecc_relief]")
{
    auto *dev = init_device_with_relief(2, 4, 3, 4, 256);

    uint32_t page_size = 0;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);

    std::vector<uint8_t> warmup(page_size, 0x00);
    REQUIRE(spi_nand_flash_write_page(dev, warmup.data(), 0) == ESP_OK);

    const uint32_t inject_count = 128;
    for (uint32_t p = 0; p < inject_count; p++) {
        nand_wrap_inject_ecc_event(dev, p, NAND_ECC_7_8_BITS_CORRECTED);
    }

    constexpr uint32_t k_ring = 16;
    constexpr uint32_t m_iters = 256;
    std::vector<uint8_t> buf(page_size);
    for (uint32_t iter = 0; iter < m_iters; iter++) {
        const uint32_t lp = iter % k_ring;
        memset(buf.data(), static_cast<int>(0x40 + (iter & 0x3F)), page_size);
        memcpy(buf.data(), &iter, sizeof(iter));
        REQUIRE(spi_nand_flash_write_page(dev, buf.data(), lp) == ESP_OK);
    }

    for (uint32_t lp = 0; lp < k_ring; lp++) {
        uint32_t last_iter = 0;
        for (uint32_t iter = 0; iter < m_iters; iter++) {
            if (iter % k_ring == lp) {
                last_iter = iter;
            }
        }
        memset(buf.data(), static_cast<int>(0x40 + (last_iter & 0x3F)), page_size);
        memcpy(buf.data(), &last_iter, sizeof(last_iter));
        std::vector<uint8_t> read_buf(page_size, 0);
        REQUIRE(spi_nand_flash_read_page(dev, read_buf.data(), lp) == ESP_OK);
        CHECK(read_buf == buf);
    }

    {
        spi_nand_ecc_relief_stats_t s = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &s) == ESP_OK);
        CHECK(s.map_entries_used <= s.map_capacity);
        CHECK(s.map_entries_used <= inject_count);
        CHECK(s.total_pages_relieved >= 1);
    }

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * CC-E: smaller backing store + gc_factor — writes, sync, GC, readback
 * --------------------------------------------------------------------------
 */
TEST_CASE("CC-E: GC under pressure with small flash and relief map", "[ecc_relief]")
{
    constexpr size_t flash_sz = 4 * 1024 * 1024;
    constexpr uint16_t map_cap = 128;
    auto *dev = init_device_with_relief_ex(2, 4, 3, 4, map_cap, 4, flash_sz);

    uint32_t page_size = 0;
    uint32_t page_count = 0;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);
    REQUIRE(spi_nand_flash_get_page_count(dev, &page_count) == ESP_OK);
    REQUIRE(page_count > 8);

    std::vector<uint8_t> warmup(page_size, 0x00);
    REQUIRE(spi_nand_flash_write_page(dev, warmup.data(), 0) == ESP_OK);

    const uint32_t inject_count = 64;
    for (uint32_t p = 0; p < inject_count; p++) {
        nand_wrap_inject_ecc_event(dev, p, NAND_ECC_7_8_BITS_CORRECTED);
    }

    const uint32_t write_span = (page_count - 1 < 300) ? (page_count - 1) : 300;
    std::vector<uint8_t> wbuf(page_size);
    for (uint32_t lp = 0; lp <= write_span; lp++) {
        memset(wbuf.data(), static_cast<int>(0xA0 + (lp & 0x1F)), page_size);
        memcpy(wbuf.data(), &lp, sizeof(lp));
        REQUIRE(spi_nand_flash_write_page(dev, wbuf.data(), lp) == ESP_OK);
    }

    REQUIRE(spi_nand_flash_sync(dev) == ESP_OK);

    for (int g = 0; g < 48; g++) {
        REQUIRE(spi_nand_flash_gc(dev) == ESP_OK);
        (void)g;
    }

    for (uint32_t check_lp : {0u, write_span / 2, write_span}) {
        memset(wbuf.data(), static_cast<int>(0xA0 + (check_lp & 0x1F)), page_size);
        memcpy(wbuf.data(), &check_lp, sizeof(check_lp));
        std::vector<uint8_t> rbuf(page_size, 0);
        REQUIRE(spi_nand_flash_read_page(dev, rbuf.data(), check_lp) == ESP_OK);
        CHECK(rbuf == wbuf);
    }

    {
        spi_nand_ecc_relief_stats_t s = {};
        REQUIRE(spi_nand_flash_get_ecc_relief_stats(dev, &s) == ESP_OK);
        CHECK(s.map_entries_used <= map_cap);
        CHECK(s.map_capacity == map_cap);
    }

    ecc_relief_test_deinit(dev);
}

/* --------------------------------------------------------------------------
 * CC-F: explicit GC after relief write — logical data unchanged
 * --------------------------------------------------------------------------
 */
TEST_CASE("CC-F: GC after relief scenario leaves logical readback stable", "[ecc_relief]")
{
    auto *dev = init_device_with_relief(2, 4, 3, 4, 256);

    uint32_t page_size = 0;
    REQUIRE(spi_nand_flash_get_page_size(dev, &page_size) == ESP_OK);

    std::vector<uint8_t> warmup(page_size, 0x00);
    REQUIRE(spi_nand_flash_write_page(dev, warmup.data(), 0) == ESP_OK);

    const uint32_t inject_count = 128;
    for (uint32_t p = 0; p < inject_count; p++) {
        nand_wrap_inject_ecc_event(dev, p, NAND_ECC_7_8_BITS_CORRECTED);
    }

    std::vector<uint8_t> write_buf(page_size);
    for (uint32_t i = 0; i < page_size; i++) {
        write_buf[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    }
    REQUIRE(spi_nand_flash_write_page(dev, write_buf.data(), 1) == ESP_OK);

    for (int g = 0; g < 32; g++) {
        (void)spi_nand_flash_gc(dev);
    }
    REQUIRE(spi_nand_flash_sync(dev) == ESP_OK);

    std::vector<uint8_t> read_buf(page_size, 0);
    REQUIRE(spi_nand_flash_read_page(dev, read_buf.data(), 1) == ESP_OK);
    CHECK(read_buf == write_buf);

    ecc_relief_test_deinit(dev);
}
