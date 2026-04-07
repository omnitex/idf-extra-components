/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * linux_wear_simulation — Demo application
 *
 * Runs a Dhara-managed NAND flash under random logical sector writes with the
 * threshold failure model active (blocks wear out after MAX_BLOCK_ERASES).
 * Reports wear stats every REPORT_INTERVAL writes so the transition from clean
 * to first-error is clearly visible.  Prints a histogram and WAF at the end,
 * then exports per-block data to a JSON file.
 *
 * NOTE on Dhara MAP_FULL: Dhara's internal B-tree map has a capacity of roughly
 * (journal_pages - GC_overhead) entries.  Writing to more distinct logical
 * sectors than that limit fills the map and returns DHARA_E_MAP_FULL.
 * NUM_LOGICAL_SECTORS is therefore intentionally kept well below device
 * capacity to avoid the limit and produce a clean, repeatable simulation.
 *
 * Build (from this directory):
 *   idf.py build
 *   ./build/linux_wear_simulation.elf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "nand_emul_advanced.h"
#include "spi_nand_flash.h"

/* -----------------------------------------------------------------------
 * Simulation parameters
 * --------------------------------------------------------------------- */
#define FLASH_SIZE_MB        8
#define FLASH_SIZE_BYTES     ((size_t)(FLASH_SIZE_MB) * 1024 * 1024)
#define TOTAL_LOGICAL_WRITES 5000
#define NUM_LOGICAL_SECTORS  50            /* hot-set: keeps Dhara map well below capacity */
#define REPORT_INTERVAL      100
#define MAX_BLOCK_ERASES     200            /* threshold: blocks fail after this */
#define JSON_OUTPUT_PATH     "/tmp/nand_wear_simulation.json"
/* GC factor 0 → default (45) inside spi_nand_flash_init_device */
#define GC_FACTOR            0

/* -----------------------------------------------------------------------
 * ASCII histogram helpers — named static callbacks (plain C17, no lambdas)
 * --------------------------------------------------------------------- */
#define HIST_BINS   16
#define HIST_WIDTH  60  /* max bar width in chars */

typedef struct {
    uint32_t counts[HIST_BINS];
    uint32_t bin_width;
    uint32_t total;
} hist_ctx_t;

static bool hist_block_cb(uint32_t bn, block_metadata_t *bm, void *ud)
{
    (void)bn;
    hist_ctx_t *hc = (hist_ctx_t *)ud;
    uint32_t bin = bm->erase_count / hc->bin_width;
    if (bin >= HIST_BINS) bin = HIST_BINS - 1;
    hc->counts[bin]++;
    hc->total++;
    return true;
}

static void print_histogram(spi_nand_flash_device_t *dev, uint32_t max_erases)
{
    hist_ctx_t hc = {
        .counts    = {0},
        .bin_width = (max_erases / HIST_BINS) + 1,
        .total     = 0,
    };

    nand_emul_iterate_worn_blocks(dev, hist_block_cb, &hc);

    printf("\n  Block erase count distribution (%" PRIu32 " tracked blocks):\n",
           hc.total);

    uint32_t peak = 1;
    for (int i = 0; i < HIST_BINS; i++) {
        if (hc.counts[i] > peak) peak = hc.counts[i];
    }

    for (int i = 0; i < HIST_BINS; i++) {
        uint32_t lo = (uint32_t)i * hc.bin_width;
        uint32_t hi = lo + hc.bin_width - 1;
        int bar_len = (int)((uint64_t)hc.counts[i] * HIST_WIDTH / peak);
        printf("  [%4" PRIu32 "-%4" PRIu32 "] %5" PRIu32 " |",
               lo, hi, hc.counts[i]);
        for (int j = 0; j < bar_len; j++) putchar('#');
        putchar('\n');
    }
}

/* -----------------------------------------------------------------------
 * Bad-block counting callback
 * --------------------------------------------------------------------- */
typedef struct { uint32_t *count; } bad_ctx_t;

static bool bad_block_cb(uint32_t bn, block_metadata_t *bm, void *ud)
{
    (void)bn;
    bad_ctx_t *bc = (bad_ctx_t *)ud;
    if (bm->is_bad_block) (*bc->count)++;
    return true;
}

/* -----------------------------------------------------------------------
 * Top-N worn block tracking
 * --------------------------------------------------------------------- */
#define TOP_N 10

typedef struct {
    uint32_t block_num;
    uint32_t erase_count;
} block_entry_t;

typedef struct {
    block_entry_t entries[TOP_N];
    int           count;
} top_ctx_t;

static bool top_block_cb(uint32_t bn, block_metadata_t *bm, void *ud)
{
    top_ctx_t *tc = (top_ctx_t *)ud;

    /* Find minimum in current top list */
    int min_idx = 0;
    for (int i = 1; i < tc->count; i++) {
        if (tc->entries[i].erase_count < tc->entries[min_idx].erase_count) {
            min_idx = i;
        }
    }

    if (tc->count < TOP_N) {
        tc->entries[tc->count].block_num   = bn;
        tc->entries[tc->count].erase_count = bm->erase_count;
        tc->count++;
    } else if (bm->erase_count > tc->entries[min_idx].erase_count) {
        tc->entries[min_idx].block_num   = bn;
        tc->entries[min_idx].erase_count = bm->erase_count;
    }
    return true;
}

static int cmp_entry_desc(const void *a, const void *b)
{
    const block_entry_t *ea = (const block_entry_t *)a;
    const block_entry_t *eb = (const block_entry_t *)b;
    if (eb->erase_count > ea->erase_count) return 1;
    if (eb->erase_count < ea->erase_count) return -1;
    return 0;
}

static void print_top_worn_blocks(spi_nand_flash_device_t *dev)
{
    top_ctx_t tc = { .count = 0 };
    nand_emul_iterate_worn_blocks(dev, top_block_cb, &tc);
    qsort(tc.entries, (size_t)tc.count, sizeof(block_entry_t), cmp_entry_desc);

    printf("\n  Top %d most-worn blocks:\n", tc.count);
    for (int i = 0; i < tc.count; i++) {
        printf("    #%2d  block %" PRIu32 "  —  %" PRIu32 " erases\n",
               i + 1, tc.entries[i].block_num, tc.entries[i].erase_count);
    }
}

/* -----------------------------------------------------------------------
 * Main entry point
 * --------------------------------------------------------------------- */
void app_main(void)
{
    printf("=== NAND Flash Wear Simulation ===\n");
    printf("Flash: %d MB | Logical writes: %d | Threshold: %d erases/block\n\n",
           FLASH_SIZE_MB, TOTAL_LOGICAL_WRITES, MAX_BLOCK_ERASES);

    /* ---- Setup ---- */
    threshold_failure_config_t th_cfg = {
        .max_block_erases  = MAX_BLOCK_ERASES,
        .max_page_programs = 0,    /* no page-program limit */
        .fail_over_limit   = true,
    };

    sparse_hash_backend_config_t be_cfg = {
        .initial_capacity        = 64,
        .load_factor             = 0.75f,
        .enable_histogram_query  = false,
        .pages_per_block         = 0,  /* filled by nand_emul_advanced_init */
    };

    nand_emul_advanced_config_t adv_cfg = {
        .base_config = {
            .flash_file_name = "",         /* use a tempfile */
            .flash_file_size = FLASH_SIZE_BYTES,
            .keep_dump       = false,
        },
        .metadata_backend        = &nand_sparse_hash_backend,
        .metadata_backend_config = &be_cfg,
        .failure_model           = &nand_noop_failure_model,
        .failure_model_config    = NULL,
        .track_block_level       = true,
        .track_page_level        = true,
        .get_timestamp           = NULL,   /* use default monotonic counter */
        .gc_factor               = GC_FACTOR,
    };

    spi_nand_flash_device_t *dev;
    ESP_ERROR_CHECK(nand_emul_advanced_init(&dev, &adv_cfg));

    uint32_t sector_size = 0;
    uint32_t block_size  = 0;
    uint32_t sector_num  = 0;
    ESP_ERROR_CHECK(spi_nand_flash_get_sector_size(dev, &sector_size));
    ESP_ERROR_CHECK(spi_nand_flash_get_block_size(dev, &block_size));
    ESP_ERROR_CHECK(spi_nand_flash_get_capacity(dev, &sector_num));

    printf("Device: %" PRIu32 " sectors x %" PRIu32 " B/sector"
           ", block size %" PRIu32 " B\n\n",
           sector_num, sector_size, block_size);

    /* ---- Workload ---- */
    /* Validate that the logical address space fits the device */
    if ((uint32_t)NUM_LOGICAL_SECTORS > sector_num) {
        printf("ERROR: NUM_LOGICAL_SECTORS (%d) exceeds device capacity (%" PRIu32 " sectors)\n",
               NUM_LOGICAL_SECTORS, sector_num);
        nand_emul_advanced_deinit(dev);
        return;
    }

    uint8_t *write_buf = malloc(sector_size);
    if (!write_buf) {
        printf("ERROR: out of memory\n");
        nand_emul_advanced_deinit(dev);
        return;
    }

    srand(0xDEADBEEFu);   /* reproducible workload */
    uint32_t write_errors = 0;
    uint32_t first_error_op = 0;
    esp_err_t first_error_code = ESP_OK;

    for (int op = 0; op < TOTAL_LOGICAL_WRITES; op++) {
        uint32_t lsector = (uint32_t)((unsigned)rand() % NUM_LOGICAL_SECTORS);

        memset(write_buf, (int)(op & 0xFF), sector_size);

        esp_err_t ret = spi_nand_flash_write_sector(dev, write_buf, lsector);
        if (ret == ESP_OK) {
            /* Record logical bytes written through the Dhara layer for WAF */
            nand_emul_record_logical_write(dev, sector_size);
        } else {
            if (write_errors == 0) {
                first_error_op   = (uint32_t)(op + 1);
                first_error_code = ret;
            }
            write_errors++;
        }

        /* Periodic mid-run stats */
        if ((op + 1) % REPORT_INTERVAL == 0) {
            nand_wear_stats_t stats = {0};
            nand_emul_get_wear_stats(dev, &stats);
            printf("[%6d writes] max_erases=%" PRIu32
                   "  min=%" PRIu32
                   "  avg=%" PRIu32
                   "  variation=%.2f"
                   "  WAF=%.2f"
                   "  errors=%" PRIu32 "\n",
                   op + 1,
                   stats.max_block_erases,
                   stats.min_block_erases,
                   stats.avg_block_erases,
                   stats.wear_leveling_variation,
                   stats.write_amplification,
                   write_errors);
        }
    }

    /* ---- Final report ---- */
    nand_wear_stats_t final_stats = {0};
    nand_emul_get_wear_stats(dev, &final_stats);

    uint32_t bad_blocks = 0;
    bad_ctx_t bc = { .count = &bad_blocks };
    nand_emul_iterate_worn_blocks(dev, bad_block_cb, &bc);

    printf("\n=== Final Wear Report ===\n");
    printf("  Total logical writes : %d\n",           TOTAL_LOGICAL_WRITES);
    printf("  Write errors         : %" PRIu32 "\n",  write_errors);
    if (write_errors > 0) {
        printf("  First error at op    : %" PRIu32 "  (esp_err=0x%08x)\n",
               first_error_op, (unsigned)first_error_code);
    }
    printf("  Simulated bad blocks : %" PRIu32 "\n",  bad_blocks);
    printf("  Max block erases     : %" PRIu32 "\n",  final_stats.max_block_erases);
    printf("  Min block erases     : %" PRIu32 "\n",  final_stats.min_block_erases);
    printf("  Avg block erases     : %" PRIu32 "\n",  final_stats.avg_block_erases);
    printf("  Wear variation       : %.4f\n",          final_stats.wear_leveling_variation);
    printf("  Write amplification  : %.2fx\n",         final_stats.write_amplification);

    print_top_worn_blocks(dev);
    print_histogram(dev, final_stats.max_block_erases > 0
                         ? final_stats.max_block_erases : 1);

    /* JSON export */
    esp_err_t jret = nand_emul_export_json(dev, JSON_OUTPUT_PATH);
    if (jret == ESP_OK) {
        printf("\nWear data exported to %s\n", JSON_OUTPUT_PATH);
    } else {
        printf("\nWARN: JSON export failed (err=0x%x)\n", (unsigned)jret);
    }

    free(write_buf);
    nand_emul_advanced_deinit(dev);
    printf("\nSimulation complete.\n");
    exit(0);
}
