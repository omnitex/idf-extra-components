/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * On-die ECC tests with injected bit errors.
 *
 * Injection: program a page with ECC on (the chip stores parity in the spare area), turn ECC
 * off, program the same page again with some data bits cleared, turn ECC back on. The second
 * program leaves the parity unchanged (nand_prog() reloads the page into the cache first), so
 * the next read sees the cleared bits as errors.
 *
 * Each case is standalone: it forces ECC on at start, erases its own block, and turns ECC back
 * on before any assertion so a failure cannot leave the chip with ECC off for later cases.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "driver/spi_master.h"
#include "spi_nand_flash.h"
#include "nand_private/nand_impl_wrap.h"
#include "spi_nand_flash_test_helpers.h"
#include "unity.h"
#include "test_spi_nand_common.h"
#include "test_nand_board.h"

/* Away from blocks 20/21 used by the other raw-page tests. Moved from 30, where the board's
 * Zetta ZD35Q1GC had cells that would not program, to tell a weak block from the chip. */
#define ECC_INJECT_BLOCK  100

/* Expected status for chips whose ECC strength the test does not know: any corrected status. */
#define EXPECT_ANY_CORRECTED  NAND_ECC_MAX

typedef struct {
    uint8_t bits;               /* bits cleared, all at the start of ECC sector 0 */
    esp_err_t read_ret;         /* expected nand_wrap_read() result with ECC on */
    nand_ecc_status_t status;   /* expected status, or EXPECT_ANY_CORRECTED */
} ecc_inject_step_t;

/*
 * Expected results per chip, from the decoder the driver installs for it. Bits are flipped in
 * one ECC sector only. Rows beyond the chip's strength expect the read to fail.
 */

/* GigaDevice 8 bits/528B (C0h ECCS + F0h ECCSE), e.g. GD5F4GM8UExxG. */
static const ecc_inject_step_t s_gd_max8_steps[] = {
    { 1, ESP_OK,   NAND_ECC_1_TO_4_BITS_CORRECTED },
    { 4, ESP_OK,   NAND_ECC_1_TO_4_BITS_CORRECTED },
    { 5, ESP_OK,   NAND_ECC_5_BITS_CORRECTED },
    { 6, ESP_OK,   NAND_ECC_6_BITS_CORRECTED },
    { 7, ESP_OK,   NAND_ECC_7_BITS_CORRECTED },
    { 8, ESP_OK,   NAND_ECC_8_BITS_CORRECTED },
    { 9, ESP_FAIL, NAND_ECC_NOT_CORRECTED },
};

/* GigaDevice 4 bits/528B: C0h 01b, F0h ECCSE gives the exact count. e.g. GD5F2GQ5UExxG. */
static const ecc_inject_step_t s_gd_max4_steps[] = {
    { 1, ESP_OK,   NAND_ECC_1_BIT_CORRECTED },
    { 2, ESP_OK,   NAND_ECC_2_BITS_CORRECTED },
    { 3, ESP_OK,   NAND_ECC_3_BITS_CORRECTED },
    { 4, ESP_OK,   NAND_ECC_4_BITS_CORRECTED },
    { 5, ESP_FAIL, NAND_ECC_NOT_CORRECTED },
};

/* Micron 3-bit ECCS, 8 bits/sector: 001b 1-3, 011b 4-6, 101b 7-8, 010b not corrected. */
static const ecc_inject_step_t s_micron_steps[] = {
    { 1, ESP_OK,   NAND_ECC_1_TO_3_BITS_CORRECTED },
    { 3, ESP_OK,   NAND_ECC_1_TO_3_BITS_CORRECTED },
    { 4, ESP_OK,   NAND_ECC_4_TO_6_BITS_CORRECTED },
    { 6, ESP_OK,   NAND_ECC_4_TO_6_BITS_CORRECTED },
    { 7, ESP_OK,   NAND_ECC_7_8_BITS_CORRECTED },
    { 8, ESP_OK,   NAND_ECC_7_8_BITS_CORRECTED },
    { 9, ESP_FAIL, NAND_ECC_NOT_CORRECTED },
};

/* 2-bit ECCS, 8 bits/528B, no count register (Zetta ZD35Q1GC): 01b 1-7, 11b exactly 8. */
static const ecc_inject_step_t s_2bit_max8_steps[] = {
    { 1, ESP_OK,   NAND_ECC_1_TO_7_BITS_CORRECTED },
    { 7, ESP_OK,   NAND_ECC_1_TO_7_BITS_CORRECTED },
    { 8, ESP_OK,   NAND_ECC_8_BITS_CORRECTED },
    { 9, ESP_FAIL, NAND_ECC_NOT_CORRECTED },
};

/* Winbond 1-bit Hamming (W25N01GV, W25N512G, W25N01GW/JW): 2 bits in a sector are fatal. */
static const ecc_inject_step_t s_wb_max1_steps[] = {
    { 1, ESP_OK,   NAND_ECC_1_BIT_CORRECTED },
    { 2, ESP_FAIL, NAND_ECC_NOT_CORRECTED },
};

/* Winbond KV, 8 bits/528B: exact count from register 30h MBF. */
static const ecc_inject_step_t s_wb_kv_steps[] = {
    { 1, ESP_OK,   NAND_ECC_1_BIT_CORRECTED },
    { 4, ESP_OK,   NAND_ECC_4_BITS_CORRECTED },
    { 5, ESP_OK,   NAND_ECC_5_BITS_CORRECTED },
    { 8, ESP_OK,   NAND_ECC_8_BITS_CORRECTED },
    { 9, ESP_FAIL, NAND_ECC_NOT_CORRECTED },
};

/* Unknown strength: a single bit is within any on-die ECC. */
static const ecc_inject_step_t s_generic_steps[] = {
    { 1, ESP_OK, EXPECT_ANY_CORRECTED },
};

#define STEPS(t)  t, sizeof(t) / sizeof(t[0])

typedef struct {
    uint8_t manufacturer_id;
    uint16_t device_id;
    const ecc_inject_step_t *steps;
    size_t count;
} ecc_inject_chip_t;

static const ecc_inject_chip_t s_chips[] = {
    { 0xC8, 0x95,   STEPS(s_gd_max8_steps) },   /* GD5F4GM8UExxG */
    { 0xC8, 0x52,   STEPS(s_gd_max4_steps) },   /* GD5F2GQ5UExxG */
    { 0x2C, 0x24,   STEPS(s_micron_steps) },    /* MT29F2G01ABAGD */
    { 0xBA, 0x71,   STEPS(s_2bit_max8_steps) }, /* ZD35Q1GC */
    { 0xEF, 0xAA21, STEPS(s_wb_max1_steps) },   /* W25N01GV */
    { 0xEF, 0xAA22, STEPS(s_wb_kv_steps) },     /* W25N02KV */
};

static void select_steps(const nand_device_info_t *info, const ecc_inject_step_t **steps, size_t *count)
{
    for (size_t i = 0; i < sizeof(s_chips) / sizeof(s_chips[0]); i++) {
        if (s_chips[i].manufacturer_id == info->manufacturer_id && s_chips[i].device_id == info->device_id) {
            *steps = s_chips[i].steps;
            *count = s_chips[i].count;
            return;
        }
    }
    *steps = s_generic_steps;
    *count = sizeof(s_generic_steps) / sizeof(s_generic_steps[0]);
}

/* Clear the first n set bits, starting at byte 0. The test pattern has ~4 set bits per byte,
 * so n <= 9 stays within the first few bytes, i.e. one ECC sector. */
static void clear_bits(uint8_t *buf, size_t len, uint8_t n)
{
    for (size_t i = 0; i < len && n > 0; i++) {
        while (buf[i] != 0 && n > 0) {
            buf[i] &= (uint8_t)(buf[i] - 1);    /* clear lowest set bit */
            n--;
        }
    }
}

static const char *ecc_status_name(nand_ecc_status_t status)
{
#define ECC_NAME(x) case x: return #x
    switch (status) {
        ECC_NAME(NAND_ECC_OK);
        ECC_NAME(NAND_ECC_1_TO_3_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_NOT_CORRECTED);
        ECC_NAME(NAND_ECC_4_TO_6_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_7_8_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_1_TO_4_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_1_TO_7_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_1_BIT_CORRECTED);
        ECC_NAME(NAND_ECC_2_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_3_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_4_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_5_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_6_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_7_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_8_BITS_CORRECTED);
        ECC_NAME(NAND_ECC_INVALID);
        ECC_NAME(NAND_ECC_UNKNOWN);
    default:
        return "(not a status)";
    }
#undef ECC_NAME
}

/* Print up to a few bytes of a raw read that differ from what was programmed. */
static int report_raw_diffs(const char *label, const uint8_t *raw, const uint8_t *pattern,
                            const uint8_t *flipped, uint32_t page_size)
{
    int diffs = 0;
    for (uint32_t i = 0; i < page_size; i++) {
        if (flipped[i] == pattern[i] && raw[i] != flipped[i]) {
            if (diffs < 8) {
                printf("    %s: byte %" PRIu32 " wrote %02X read %02X\n", label, i, flipped[i], raw[i]);
            }
            diffs++;
        }
    }
    return diffs;
}

/*
 * Reprogram the page with ECC off and check that the injected bits landed. Never asserts;
 * always turns ECC back on. Without ECC a raw read also shows the chip's own bit errors and
 * any SPI transfer errors, so only the bytes the test changed must match. Other differences
 * are printed, and the page is read twice: differences in both reads are in the array,
 * differences in one read are on the bus.
 * @return NULL on success, else a description of what failed (valid until the next call).
 */
static const char *inject(spi_nand_flash_device_t *dev, uint32_t page, const uint8_t *pattern,
                          const uint8_t *flipped, uint8_t *readback, uint32_t page_size)
{
    static char err_buf[112];
    const char *err = NULL;
    bool ecc_on = true;
    uint8_t *raw2 = heap_caps_malloc(page_size, MALLOC_CAP_DMA);
    if (raw2 == NULL) {
        return "out of memory";
    }
    if (spi_nand_test_set_on_die_ecc(dev, false) != ESP_OK) {
        err = "failed to disable ECC";
    } else if (spi_nand_test_get_on_die_ecc(dev, &ecc_on) != ESP_OK || ecc_on) {
        err = "CFG ECC-EN still set after disabling ECC";
    } else if (nand_wrap_read(dev, page, 0, page_size, raw2) != ESP_OK) {
        err = "raw read of the clean page (ECC off) failed";
    } else {
        /* Cell errors the chip's ECC hid before the injection (diagnostic only). */
        report_raw_diffs("raw before 2nd program", raw2, pattern, pattern, page_size);
        if (nand_wrap_prog(dev, page, flipped) != ESP_OK) {
            err = "second program (ECC off) failed";
        } else {
            esp_err_t ret1 = nand_wrap_read(dev, page, 0, page_size, readback);
            esp_err_t ret2 = nand_wrap_read(dev, page, 0, page_size, raw2);
            if (ret1 != ESP_OK || ret2 != ESP_OK) {
                snprintf(err_buf, sizeof(err_buf), "raw read (ECC off) returned %s / %s",
                         esp_err_to_name(ret1), esp_err_to_name(ret2));
                err = err_buf;
            } else {
                for (uint32_t i = 0; i < page_size && err == NULL; i++) {
                    if (flipped[i] != pattern[i] && (readback[i] != flipped[i] || raw2[i] != flipped[i])) {
                        snprintf(err_buf, sizeof(err_buf), "injected byte %" PRIu32 " wrote %02X, raw reads %02X %02X",
                                 i, flipped[i], readback[i], raw2[i]);
                        err = err_buf;
                    }
                }
                int d1 = report_raw_diffs("raw read 1", readback, pattern, flipped, page_size);
                int d2 = report_raw_diffs("raw read 2", raw2, pattern, flipped, page_size);
                if (d1 || d2) {
                    printf("    raw diffs outside the injection: %d / %d bytes, reads %s\n", d1, d2,
                           memcmp(readback, raw2, page_size) == 0 ? "agree (array)" : "disagree (bus)");
                }
            }
        }
    }
    if (spi_nand_test_set_on_die_ecc(dev, true) != ESP_OK) {
        err = "failed to re-enable ECC";
    }
    free(raw2);
    return err;
}

/* One chip under test, so its resources can be freed after a failed assertion. */
typedef struct {
    spi_nand_flash_device_t *dev;
    nand_device_info_t info;
    uint32_t page;
    uint32_t page_size;
    uint8_t *pattern;
    uint8_t *flipped;
    uint8_t *readback;
} slot_ctx_t;

/* Init the chip in @p slot and allocate buffers. @return false if the slot has no usable chip. */
static bool open_slot(const nand_slot_t *slot, slot_ctx_t *ctx)
{
    spi_device_handle_t spi;
    nand_board_select(slot, SPI_DEVICE_HALFDUPLEX, slot->clock_hz, &spi);
    spi_nand_flash_config_t cfg = {
        .device_handle = spi,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .io_mode = SPI_NAND_IO_MODE_SIO,
    };
    esp_err_t init_ret = spi_nand_flash_init_device(&cfg, &ctx->dev);
    if (init_ret != ESP_OK) {
        /* Unpopulated slot or a chip the driver does not support. */
        ctx->dev = NULL;
        printf("%s: skipped, init %s\n", slot->name, esp_err_to_name(init_ret));
        return false;
    }
    spi_nand_test_get_device_info(ctx->dev, &ctx->info);

    uint32_t block_size;
    TEST_ESP_OK(spi_nand_flash_get_page_size(ctx->dev, &ctx->page_size));
    TEST_ESP_OK(spi_nand_flash_get_block_size(ctx->dev, &block_size));
    ctx->page = ECC_INJECT_BLOCK * (block_size / ctx->page_size);

    ctx->pattern = heap_caps_malloc(ctx->page_size, MALLOC_CAP_DMA);
    ctx->flipped = heap_caps_malloc(ctx->page_size, MALLOC_CAP_DMA);
    ctx->readback = heap_caps_malloc(ctx->page_size, MALLOC_CAP_DMA);
    TEST_ASSERT_NOT_NULL(ctx->pattern);
    TEST_ASSERT_NOT_NULL(ctx->flipped);
    TEST_ASSERT_NOT_NULL(ctx->readback);
    spi_nand_flash_fill_buffer(ctx->pattern, ctx->page_size / sizeof(uint32_t));

    /* Do not rely on the chip state left by earlier cases. */
    TEST_ESP_OK(spi_nand_test_set_on_die_ecc(ctx->dev, true));
    return true;
}

/*
 * Program a clean page, inject the bits already cleared in ctx->flipped, and read it back with
 * ECC on. Asserts on the setup; the ECC verdict is returned for the caller to check.
 */
static void inject_and_read(slot_ctx_t *ctx, const char *msg, esp_err_t *ret, nand_ecc_status_t *status)
{
    spi_nand_flash_device_t *dev = ctx->dev;

    /* Clean page with parity, ECC on. */
    TEST_ESP_OK(nand_wrap_erase_block(dev, ECC_INJECT_BLOCK));
    TEST_ESP_OK(nand_wrap_prog(dev, ctx->page, ctx->pattern));
    TEST_ESP_OK(nand_wrap_read(dev, ctx->page, 0, ctx->page_size, ctx->readback));
    TEST_ASSERT_EQUAL_MESSAGE(NAND_ECC_OK, spi_nand_test_get_last_ecc_status(dev), msg);

    const char *inject_err = inject(dev, ctx->page, ctx->pattern, ctx->flipped, ctx->readback, ctx->page_size);
    if (inject_err) {
        printf("    %s\n", inject_err);
    }
    TEST_ASSERT_NULL_MESSAGE(inject_err, msg);

    *ret = nand_wrap_read(dev, ctx->page, 0, ctx->page_size, ctx->readback);
    *status = spi_nand_test_get_last_ecc_status(dev);
}

/* Free what open_slot() allocated, whether the body returned or a failed assertion jumped out. */
static void cleanup_slot(slot_ctx_t *ctx)
{
    if (ctx->dev) {
        /* inject() restores ECC before returning, so only the scratch block is left to erase. */
        spi_nand_test_set_on_die_ecc(ctx->dev, true);
        nand_wrap_erase_block(ctx->dev, ECC_INJECT_BLOCK);
        spi_nand_flash_deinit_device(ctx->dev);
    }
    free(ctx->pattern);
    free(ctx->flipped);
    free(ctx->readback);
    spi_nand_flash_test_teardown();
}

/* Run every step of the chip's table. */
static void run_steps(const nand_slot_t *slot, slot_ctx_t *ctx)
{
    const ecc_inject_step_t *steps;
    size_t step_count;
    select_steps(&ctx->info, &steps, &step_count);
    printf("%s: chip %s (%02X %04X), %s steps\n", slot->name, ctx->info.chip_name, ctx->info.manufacturer_id,
           ctx->info.device_id, steps == s_generic_steps ? "generic" : "exact");

    char msg[64];
    for (size_t i = 0; i < step_count; i++) {
        const ecc_inject_step_t *step = &steps[i];
        snprintf(msg, sizeof(msg), "%s, %u bits", slot->name, step->bits);

        memcpy(ctx->flipped, ctx->pattern, ctx->page_size);
        clear_bits(ctx->flipped, ctx->page_size, step->bits);
        esp_err_t ret;
        nand_ecc_status_t status;
        inject_and_read(ctx, msg, &ret, &status);
        printf("  %u bits: read %s, %s\n", step->bits, esp_err_to_name(ret), ecc_status_name(status));

        TEST_ASSERT_EQUAL_MESSAGE(step->read_ret, ret, msg);
        if (step->status == EXPECT_ANY_CORRECTED) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(NAND_ECC_OK, status, msg);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(NAND_ECC_NOT_CORRECTED, status, msg);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(NAND_ECC_INVALID, status, msg);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(NAND_ECC_UNKNOWN, status, msg);
        } else if (status != step->status) {
            char fail[128];
            snprintf(fail, sizeof(fail), "%s: expected %s, got %s", msg, ecc_status_name(step->status),
                     ecc_status_name(status));
            TEST_FAIL_MESSAGE(fail);
        }
        if (step->read_ret == ESP_OK) {
            TEST_ASSERT_EQUAL_MESSAGE(0, spi_nand_flash_check_buffer(ctx->readback, ctx->page_size / sizeof(uint32_t)), msg);
        }
    }
    printf("%s: PASS\n", slot->name);
}

/*
 * A failed assertion in one chip must not hide the others: each slot runs under its own
 * TEST_PROTECT, so a failure jumps back here. Unity aborts every later assertion once
 * CurrentTestFailed is set, so the flag is cleared per slot and the case is failed once at the
 * end, under a fresh TEST_PROTECT frame. The slot context is static: a local modified after
 * setjmp() is indeterminate after the longjmp().
 */
static slot_ctx_t s_ctx;

static void for_each_slot(void (*body)(const nand_slot_t *slot, slot_ctx_t *ctx))
{
    volatile int failed = 0;
    for (volatile size_t i = 0; i < nand_board_slot_count; i++) {
        s_ctx = (slot_ctx_t) {
            0
        };
        if (TEST_PROTECT()) {
            if (open_slot(&nand_board_slots[i], &s_ctx)) {
                body(&nand_board_slots[i], &s_ctx);
            }
        }
        if (Unity.CurrentTestFailed) {
            printf("%s: FAIL\n", nand_board_slots[i].name);
            Unity.CurrentTestFailed = 0;
            failed++;
        }
        cleanup_slot(&s_ctx);
    }
    nand_board_release_cs();
    if (TEST_PROTECT()) {
        TEST_ASSERT_EQUAL_MESSAGE(0, failed, "chips failed, see FAIL lines above");
    }
}

TEST_CASE("on-die ECC reports injected bit errors (bypassing dhara)", "[spi_nand_flash][ecc_inject]")
{
    for_each_slot(run_steps);
}

/*
 * Which sector does the status describe? ECC corrects each 512 B data sector separately; the
 * page status is assumed to be the worst sector (the driver's refresh threshold relies on it).
 * Bits are spread over sectors so "worst sector", "sum over the page" and "sector 0 only"
 * predict different counts. Needs a chip that reports exact counts.
 */
#define ECC_SECTOR_SIZE  512
#define ECC_SECTORS      4

typedef struct {
    uint8_t bits[ECC_SECTORS];  /* bits cleared at the start of each sector */
} ecc_sector_row_t;

static const ecc_sector_row_t s_sector_rows[] = {
    { { 2, 0, 0, 0 } },     /* baseline: all three models say 2 */
    { { 2, 2, 0, 0 } },     /* worst 2, sum 4 */
    { { 2, 2, 2, 0 } },     /* worst 2, sum 6 */
    { { 2, 2, 2, 2 } },     /* worst 2, sum 8 */
    { { 1, 3, 0, 0 } },     /* worst 3, sum 4, sector 0 only 1 */
    { { 0, 0, 0, 3 } },     /* worst 3, sum 3, sector 0 only 0 */
};

/* Chips whose status is an exact count of at least 3 bits, enough for every row above. */
static bool reports_exact_count(const nand_device_info_t *info)
{
    return (info->manufacturer_id == 0xC8 && info->device_id == 0x52)        /* GD5F2GQ5UExxG, 1-4 */
           || (info->manufacturer_id == 0xEF && info->device_id == 0xAA22);  /* W25N02KV, 1-8 */
}

/* Exact count of an exact-count status; -1 for OK-like or range statuses. */
static int status_count(nand_ecc_status_t status)
{
    switch (status) {
    case NAND_ECC_OK: return 0;
    case NAND_ECC_1_BIT_CORRECTED: return 1;
    case NAND_ECC_2_BITS_CORRECTED: return 2;
    case NAND_ECC_3_BITS_CORRECTED: return 3;
    case NAND_ECC_4_BITS_CORRECTED: return 4;
    case NAND_ECC_5_BITS_CORRECTED: return 5;
    case NAND_ECC_6_BITS_CORRECTED: return 6;
    case NAND_ECC_7_BITS_CORRECTED: return 7;
    case NAND_ECC_8_BITS_CORRECTED: return 8;
    default: return -1;
    }
}

static void run_sectors(const nand_slot_t *slot, slot_ctx_t *ctx)
{
    if (!reports_exact_count(&ctx->info)) {
        printf("%s: skipped, no exact per-page count for %s\n", slot->name, ctx->info.chip_name);
        return;
    }
    TEST_ASSERT_TRUE(ctx->page_size >= ECC_SECTORS * ECC_SECTOR_SIZE);
    printf("%s: chip %s, bits per sector -> reported (worst / sum / sector 0)\n", slot->name, ctx->info.chip_name);

    char msg[64];
    for (size_t r = 0; r < sizeof(s_sector_rows) / sizeof(s_sector_rows[0]); r++) {
        const uint8_t *bits = s_sector_rows[r].bits;
        int worst = 0, sum = 0;
        memcpy(ctx->flipped, ctx->pattern, ctx->page_size);
        for (int sec = 0; sec < ECC_SECTORS; sec++) {
            clear_bits(ctx->flipped + sec * ECC_SECTOR_SIZE, ECC_SECTOR_SIZE, bits[sec]);
            worst = bits[sec] > worst ? bits[sec] : worst;
            sum += bits[sec];
        }
        snprintf(msg, sizeof(msg), "%s, sectors %u/%u/%u/%u", slot->name, bits[0], bits[1], bits[2], bits[3]);

        esp_err_t ret;
        nand_ecc_status_t status;
        inject_and_read(ctx, msg, &ret, &status);
        int got = ret == ESP_OK ? status_count(status) : -1;
        printf("  %u/%u/%u/%u: read %s, %s  (worst %d, sum %d, sector 0 %u) -> %s\n",
               bits[0], bits[1], bits[2], bits[3], esp_err_to_name(ret), ecc_status_name(status), worst, sum, bits[0],
               got == worst ? "worst" : got == sum ? "SUM" : got == bits[0] ? "SECTOR 0" : "none");

        /* The driver assumes the worst sector. Under "sum", rows above the chip's strength fail. */
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, ret, msg);
        TEST_ASSERT_EQUAL_MESSAGE(worst, got, msg);
        TEST_ASSERT_EQUAL_MESSAGE(0, spi_nand_flash_check_buffer(ctx->readback, ctx->page_size / sizeof(uint32_t)), msg);
    }
    printf("%s: PASS\n", slot->name);
}

TEST_CASE("on-die ECC status reports the worst sector (bypassing dhara)", "[spi_nand_flash][ecc_sector]")
{
    for_each_slot(run_sectors);
}
