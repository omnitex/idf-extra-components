/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "spi_nand_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Number of latency histogram buckets (see LATENCY_BUCKET_US_* defines in .c) */
#define PERF_LATENCY_BUCKETS 6

/**
 * @brief Result of a single benchmark direction (read or write).
 *
 * Populated by run_sequential_write_bench() / run_sequential_read_bench() /
 * run_random_write_bench() / run_random_read_bench().
 */
typedef struct {
    uint32_t page_count;         ///< Number of pages exercised
    uint32_t page_size;          ///< Page size in bytes

    /* Per-pass throughput in kB/s (up to PERF_MAX_PASSES entries, rest = 0) */
    float    pass_kbps[4];
    uint32_t num_passes;

    /* Aggregate stats across all passes */
    float    mean_kbps;
    float    min_kbps;
    float    max_kbps;
    float    stddev_kbps;

    /* Per-page latency stats (microseconds), across all passes */
    int64_t  lat_min_us;
    int64_t  lat_max_us;
    int64_t  lat_mean_us;
    int64_t  lat_p95_us;         ///< 95th-percentile latency

    /* Latency histogram — counts per bucket */
    uint32_t lat_hist[PERF_LATENCY_BUCKETS];
} perf_direction_result_t;

/**
 * @brief Full benchmark result (write + read phases).
 */
typedef struct {
    const char              *name;
    perf_direction_result_t  write;
    perf_direction_result_t  read;
} bench_result_t;

/**
 * @brief Configuration for all benchmarks — passed once to each run_* function.
 */
typedef struct {
    spi_nand_flash_device_t *flash;
    uint32_t                 num_pages;    ///< How many logical pages to exercise
    uint32_t                 num_passes;   ///< Repetitions for averaging (1–4)
    uint32_t                 page_size;    ///< Filled in by init from flash query
    bool                     verify_data;  ///< Read-back verify writes (slower but safe)
} bench_cfg_t;

/**
 * @brief Perform a warmup pass (not timed) to bring Dhara to steady state.
 *
 * Writes then reads all pages in bench_cfg once without recording results.
 */
esp_err_t perf_warmup(const bench_cfg_t *cfg);

/**
 * @brief Sequential write then sequential read benchmark.
 *
 * Writes cfg->num_pages pages in logical order across cfg->num_passes passes,
 * then reads them back in the same order.
 */
esp_err_t run_sequential_bench(const bench_cfg_t *cfg, bench_result_t *result);

/**
 * @brief Random-order write then random-order read benchmark.
 *
 * Shuffles page indices (Fisher-Yates) then writes/reads in that order.
 */
esp_err_t run_random_bench(const bench_cfg_t *cfg, bench_result_t *result);

/**
 * @brief Print a bench_result_t in a human-readable format to the ESP log.
 */
void perf_print_result(const bench_result_t *result);

/**
 * @brief Print a compact comparison table for an array of results.
 */
void perf_print_summary_table(const bench_result_t *results, uint32_t count);

#ifdef __cplusplus
}
#endif
