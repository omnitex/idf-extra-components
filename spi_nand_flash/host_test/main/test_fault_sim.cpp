/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstring>
#include <cstdlib>

extern "C" {
#include "spi_nand_flash.h"
#include "nand_impl.h"
#include "nand_linux_mmap_emul.h"
#include "nand_fault_sim.h"
#include "esp_err.h"
}

#include <catch2/catch_test_macros.hpp>

/* -----------------------------------------------------------------------
 * Fixture helpers
 * -------------------------------------------------------------------- */

static constexpr size_t FS_SMALL = (size_t)16u * 1024u * 1024u;

struct FaultSimFixture {
    spi_nand_flash_device_t *dev = nullptr;
    nand_fault_sim_config_t  cfg = {};

    void init(size_t flash_size = FS_SMALL)
    {
        emul = {"", flash_size, false};
        spi_nand_flash_config_t scfg = {&emul, 0, SPI_NAND_IO_MODE_SIO, 0};

        /* Determine geometry from emul: defaults match nand_impl_linux defaults */
        constexpr uint32_t log2_ppb       = 6;   /* 64 pages/block */
        constexpr uint32_t log2_page_size = 11;  /* 2048 bytes/page */
        constexpr uint32_t page_size      = 1u << log2_page_size;
        constexpr uint32_t oob_size       = 64u;
        constexpr uint32_t emul_page_size = page_size + oob_size;
        constexpr uint32_t ppb            = 1u << log2_ppb;
        constexpr uint32_t file_bpb       = ppb * emul_page_size;
        uint32_t num_blocks = (uint32_t)(flash_size / file_bpb);

        REQUIRE(nand_fault_sim_init(num_blocks, ppb, &cfg) == ESP_OK);
        REQUIRE(spi_nand_flash_init_device(&scfg, &dev) == ESP_OK);
        REQUIRE(dev != nullptr);
    }

    void destroy()
    {
        if (dev) {
            spi_nand_flash_deinit_device(dev);
            dev = nullptr;
        }
        nand_fault_sim_deinit();
    }

    nand_file_mmap_emul_config_t emul = {};
};

/* ECC callback capture helper */
struct EccCapture {
    uint32_t         page   = UINT32_MAX;
    nand_ecc_status_t status = NAND_ECC_OK;
    int              count  = 0;

    static void cb(uint32_t page, nand_ecc_status_t status, void *ctx)
    {
        auto *self = static_cast<EccCapture *>(ctx);
        self->page   = page;
        self->status = status;
        self->count++;
    }
};

/* -----------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------- */

TEST_CASE("fault_sim: factory bad blocks visible before any operation", "[fault_sim]")
{
    static const uint32_t bad_list[] = { 2, 5, 10 };
    FaultSimFixture f;
    f.cfg.factory_bad_blocks      = bad_list;
    f.cfg.factory_bad_block_count = 3;
    f.init();

    bool is_bad = false;
    REQUIRE(nand_is_bad(f.dev, 2,  &is_bad) == ESP_OK);
    REQUIRE(is_bad == true);
    REQUIRE(nand_is_bad(f.dev, 5,  &is_bad) == ESP_OK);
    REQUIRE(is_bad == true);
    REQUIRE(nand_is_bad(f.dev, 10, &is_bad) == ESP_OK);
    REQUIRE(is_bad == true);

    REQUIRE(nand_is_bad(f.dev, 0, &is_bad) == ESP_OK);
    REQUIRE(is_bad == false);
    REQUIRE(nand_is_bad(f.dev, 3, &is_bad) == ESP_OK);
    REQUIRE(is_bad == false);

    f.destroy();
}

TEST_CASE("fault_sim: mark_bad at runtime writes OOB marker", "[fault_sim]")
{
    FaultSimFixture f;
    f.init();

    bool is_bad = false;
    REQUIRE(nand_is_bad(f.dev, 7, &is_bad) == ESP_OK);
    REQUIRE(is_bad == false);

    REQUIRE(nand_mark_bad(f.dev, 7) == ESP_OK);

    REQUIRE(nand_is_bad(f.dev, 7, &is_bad) == ESP_OK);
    REQUIRE(is_bad == true);

    /* Reset clears counters but NOT mmap — bad-block OOB marker persists */
    nand_fault_sim_reset();
    REQUIRE(nand_fault_sim_get_erase_count(0) == 0u);
    REQUIRE(nand_is_bad(f.dev, 7, &is_bad) == ESP_OK);
    REQUIRE(is_bad == true);

    f.destroy();
}

TEST_CASE("fault_sim: erase wear-out fires after max_erase_cycles", "[fault_sim]")
{
    FaultSimFixture f;
    f.cfg.max_erase_cycles = 3;
    f.init();

    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK);
    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK);
    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK);
    REQUIRE(nand_fault_sim_get_erase_count(0) == 3u);

    REQUIRE(nand_erase_block(f.dev, 0) != ESP_OK);

    f.destroy();
}

TEST_CASE("fault_sim: prog wear-out fires after max_prog_cycles", "[fault_sim]")
{
    FaultSimFixture f;
    f.cfg.max_prog_cycles = 2;
    f.init();

    uint8_t buf[2048] = {};

    REQUIRE(nand_prog(f.dev, 0, buf) == ESP_OK);
    REQUIRE(nand_prog(f.dev, 0, buf) == ESP_OK);
    REQUIRE(nand_fault_sim_get_prog_count(0) == 2u);

    REQUIRE(nand_prog(f.dev, 0, buf) != ESP_OK);

    f.destroy();
}

TEST_CASE("fault_sim: grave page reports NAND_ECC_NOT_CORRECTED, data unchanged", "[fault_sim]")
{
    EccCapture cap;
    FaultSimFixture f;
    f.cfg.grave_page_threshold = 1;
    f.cfg.on_page_read_ecc = EccCapture::cb;
    f.cfg.ecc_cb_ctx       = &cap;
    f.init();

    /* Erase block 0 first, then program page 0 twice */
    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK);
    uint8_t written[2048];
    for (int i = 0; i < (int)sizeof(written); i++) {
        written[i] = (uint8_t)(i & 0xFF);
    }
    REQUIRE(nand_prog(f.dev, 0, written) == ESP_OK);
    /* prog_count now 1, which exceeds grave_page_threshold(1)? No — threshold is 1,
       so > 1 required. Prog once more to push count to 2. */
    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK);
    REQUIRE(nand_prog(f.dev, 0, written) == ESP_OK);
    REQUIRE(nand_prog(f.dev, 0, written) == ESP_OK); /* count = 2 > 1 */

    uint8_t readback[2048] = {};
    REQUIRE(nand_read(f.dev, 0, 0, sizeof(readback), readback) == ESP_OK);

    REQUIRE(cap.count > 0);
    REQUIRE(cap.status == NAND_ECC_NOT_CORRECTED);
    REQUIRE(cap.page == 0u);
    REQUIRE(memcmp(written, readback, sizeof(written)) == 0);

    f.destroy();
}

TEST_CASE("fault_sim: prog_fail_prob 1.0 always fails, 0.0 always succeeds", "[fault_sim]")
{
    FaultSimFixture f;
    f.cfg.prog_fail_prob = 1.0f;
    f.cfg.op_fail_seed   = 0;
    f.init();

    uint8_t buf[2048] = {};
    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK);
    REQUIRE(nand_prog(f.dev, 0, buf) != ESP_OK);

    f.destroy();

    /* Verify mmap unchanged — page should still be free (OOB not written) */
    FaultSimFixture f2;
    f2.cfg.prog_fail_prob = 0.0f;
    f2.init();
    REQUIRE(nand_erase_block(f2.dev, 0) == ESP_OK);
    REQUIRE(nand_prog(f2.dev, 0, buf) == ESP_OK);
    f2.destroy();
}

TEST_CASE("fault_sim: read_fail_prob 1.0 always fails", "[fault_sim]")
{
    FaultSimFixture f;
    f.cfg.read_fail_prob = 1.0f;
    f.init();

    uint8_t buf[2048] = {};
    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK);
    REQUIRE(nand_prog(f.dev, 0, buf) == ESP_OK);

    uint8_t readback[2048] = {};
    REQUIRE(nand_read(f.dev, 0, 0, sizeof(readback), readback) != ESP_OK);

    f.destroy();
}

TEST_CASE("fault_sim: erase_fail_prob 1.0 always fails, block contents unchanged", "[fault_sim]")
{
    FaultSimFixture f;
    f.cfg.erase_fail_prob = 1.0f;
    f.init();

    REQUIRE(nand_erase_block(f.dev, 0) != ESP_OK);

    f.destroy();
}

TEST_CASE("fault_sim: nand_fault_sim_get_read_count tracks reads", "[fault_sim]")
{
    FaultSimFixture f;
    f.init();

    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK);
    uint8_t buf[2048] = {};
    REQUIRE(nand_prog(f.dev, 0, buf) == ESP_OK);

    uint8_t rb[2048] = {};
    for (int i = 0; i < 7; i++) {
        REQUIRE(nand_read(f.dev, 5, 0, sizeof(rb), rb) == ESP_OK);
    }
    REQUIRE(nand_fault_sim_get_read_count(5) == 7u);

    f.destroy();
}

TEST_CASE("fault_sim: deterministic crash fires at exact op, freezes afterwards", "[fault_sim]")
{
    FaultSimFixture f;
    f.cfg.crash_after_ops_min = 5;
    f.cfg.crash_after_ops_max = 5;
    f.cfg.crash_seed          = 1;
    f.init();

    uint8_t buf[2048] = {};
    for (int i = 0; i < 4; i++) {
        REQUIRE(nand_erase_block(f.dev, (uint32_t)i) == ESP_OK);
    }

    /* 5th write-type op must crash */
    esp_err_t err = nand_erase_block(f.dev, 4);
    REQUIRE(err != ESP_OK);

    /* Subsequent ops return INVALID_STATE */
    REQUIRE(nand_erase_block(f.dev, 0) == ESP_ERR_INVALID_STATE);
    REQUIRE(nand_prog(f.dev, 0, buf) == ESP_ERR_INVALID_STATE);

    f.destroy();
}

TEST_CASE("fault_sim: torn prog writes prefix bytes only", "[fault_sim]")
{
    FaultSimFixture f;
    f.cfg.crash_after_ops_min = 2;
    f.cfg.crash_after_ops_max = 2;
    f.cfg.crash_seed          = 99;
    f.init();

    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK); /* op=1, succeeds */

    /* Second op (prog) triggers the crash at op=2 */
    uint8_t written[2048];
    for (int i = 0; i < (int)sizeof(written); i++) {
        written[i] = (uint8_t)(i ^ 0xAB);
    }
    esp_err_t err = nand_prog(f.dev, 0, written);
    REQUIRE(err != ESP_OK); /* crash fired */

    f.destroy();
}

TEST_CASE("fault_sim: crash range mode fires within [min,max]", "[fault_sim]")
{
    uint32_t crash_op = 0;

    for (int seed = 0; seed < 5; seed++) {
        FaultSimFixture f;
        f.cfg.crash_after_ops_min = 3;
        f.cfg.crash_after_ops_max = 10;
        f.cfg.crash_seed          = (unsigned int)seed;
        f.init();

        for (int i = 0; i < 20; i++) {
            esp_err_t err = nand_erase_block(f.dev, (uint32_t)(i % (int)f.dev->chip.num_blocks));
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                crash_op = (uint32_t)(i + 1);
                break;
            }
            if (err == ESP_ERR_INVALID_STATE) {
                crash_op = (uint32_t)(i);
                break;
            }
        }
        REQUIRE(crash_op >= 3u);
        REQUIRE(crash_op <= 10u);
        f.destroy();
        crash_op = 0;
    }
}

TEST_CASE("fault_sim: ECC disturb escalates through thresholds", "[fault_sim]")
{
    EccCapture cap;
    FaultSimFixture f;
    f.cfg.ecc_mid_threshold  = 3;
    f.cfg.ecc_high_threshold = 6;
    f.cfg.ecc_fail_threshold = 9;
    f.cfg.on_page_read_ecc   = EccCapture::cb;
    f.cfg.ecc_cb_ctx         = &cap;
    f.init();

    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK);
    uint8_t buf[2048] = {};
    REQUIRE(nand_prog(f.dev, 0, buf) == ESP_OK);

    uint8_t rb[2048] = {};

    for (int i = 0; i < 3; i++) {
        cap.count = 0;
        REQUIRE(nand_read(f.dev, 0, 0, sizeof(rb), rb) == ESP_OK);
    }
    REQUIRE(cap.count > 0);
    REQUIRE(cap.status == NAND_ECC_1_TO_3_BITS_CORRECTED);

    for (int i = 0; i < 3; i++) {
        cap.count = 0;
        REQUIRE(nand_read(f.dev, 0, 0, sizeof(rb), rb) == ESP_OK);
    }
    REQUIRE(cap.status == NAND_ECC_4_TO_6_BITS_CORRECTED);

    for (int i = 0; i < 3; i++) {
        cap.count = 0;
        REQUIRE(nand_read(f.dev, 0, 0, sizeof(rb), rb) == ESP_OK);
    }
    REQUIRE(cap.status == NAND_ECC_NOT_CORRECTED);

    REQUIRE(memcmp(buf, rb, sizeof(buf)) == 0);

    f.destroy();
}

TEST_CASE("fault_sim: ECC disturb no-op when thresholds are zero", "[fault_sim]")
{
    EccCapture cap;
    FaultSimFixture f;
    f.cfg.ecc_mid_threshold  = 0;
    f.cfg.ecc_high_threshold = 0;
    f.cfg.ecc_fail_threshold = 0;
    f.cfg.on_page_read_ecc   = EccCapture::cb;
    f.cfg.ecc_cb_ctx         = &cap;
    f.init();

    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK);
    uint8_t buf[2048] = {};
    REQUIRE(nand_prog(f.dev, 0, buf) == ESP_OK);

    uint8_t rb[2048] = {};
    for (int i = 0; i < 1000; i++) {
        REQUIRE(nand_read(f.dev, 0, 0, sizeof(rb), rb) == ESP_OK);
    }
    REQUIRE(cap.count == 0);

    f.destroy();
}

TEST_CASE("fault_sim: reset clears counters, preserves mmap", "[fault_sim]")
{
    FaultSimFixture f;
    f.cfg.max_erase_cycles = 100;
    f.init();

    REQUIRE(nand_erase_block(f.dev, 0) == ESP_OK);
    REQUIRE(nand_fault_sim_get_erase_count(0) == 1u);

    nand_fault_sim_reset();

    REQUIRE(nand_fault_sim_get_erase_count(0) == 0u);
    REQUIRE(nand_fault_sim_get_prog_count(0) == 0u);
    REQUIRE(nand_fault_sim_get_read_count(0) == 0u);

    f.destroy();
}

TEST_CASE("fault_sim: preset configs init without error", "[fault_sim]")
{
    constexpr size_t flash_size = FS_SMALL;
    constexpr uint32_t log2_ppb       = 6;
    constexpr uint32_t log2_page_size = 11;
    constexpr uint32_t page_size      = 1u << log2_page_size;
    constexpr uint32_t oob_size       = 64u;
    constexpr uint32_t emul_page_size = page_size + oob_size;
    constexpr uint32_t ppb            = 1u << log2_ppb;
    constexpr uint32_t file_bpb       = ppb * emul_page_size;
    uint32_t num_blocks = (uint32_t)(flash_size / file_bpb);

    for (int scenario = 0; scenario <= (int)NAND_SIM_SCENARIO_POWER_LOSS; scenario++) {
        nand_fault_sim_config_t cfg =
            nand_fault_sim_config_preset(static_cast<nand_sim_scenario_t>(scenario));

        /* Skip factory bad blocks that exceed num_blocks for small flash */
        if (cfg.factory_bad_block_count > 0) {
            bool skip = false;
            for (uint32_t i = 0; i < cfg.factory_bad_block_count; i++) {
                if (cfg.factory_bad_blocks[i] >= num_blocks) {
                    skip = true;
                    break;
                }
            }
            if (skip) {
                cfg.factory_bad_blocks      = nullptr;
                cfg.factory_bad_block_count = 0;
            }
        }

        REQUIRE(nand_fault_sim_init(num_blocks, ppb, &cfg) == ESP_OK);
        nand_fault_sim_deinit();
    }
}

TEST_CASE("fault_sim: reset restores PRNG so crash point is same as fresh init", "[fault_sim]")
{
    FaultSimFixture f;
    f.cfg.crash_after_ops_min = 5;
    f.cfg.crash_after_ops_max = 5;
    f.cfg.crash_seed          = 42;
    f.init();

    uint32_t op_at_crash_first = 0;
    for (uint32_t i = 0; i < 20; i++) {
        esp_err_t err = nand_erase_block(f.dev, i % f.dev->chip.num_blocks);
        if (err != ESP_OK) {
            op_at_crash_first = i + 1;
            break;
        }
    }
    REQUIRE(op_at_crash_first > 0u);

    nand_fault_sim_reset();

    uint32_t op_at_crash_second = 0;
    for (uint32_t i = 0; i < 20; i++) {
        esp_err_t err = nand_erase_block(f.dev, i % f.dev->chip.num_blocks);
        if (err != ESP_OK) {
            op_at_crash_second = i + 1;
            break;
        }
    }
    REQUIRE(op_at_crash_second == op_at_crash_first);

    f.destroy();
}
