/*
 * SPDX-FileCopyrightText: 2026 Martin Havlik <omnitex.git@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "spi_nand_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Number of latency histogram buckets (see s_lat_bucket_us in .c) */
#define PERF_LATENCY_BUCKETS 9

/** Maximum number of passes (size of per-pass throughput array) */
#define PERF_MAX_PASSES 10

/** JSON report schema version (bump when fields change). */
#define PERF_JSON_SCHEMA "esp_nand_perf_v1"

/** Marker lines for host-side log scraping (stable across releases unless schema bumps). */
#define PERF_JSON_BEGIN "<<<NAND_PERF_JSON_BEGIN>>>"
#define PERF_JSON_END   "<<<NAND_PERF_JSON_END>>>"

/**
 * @brief Result of a single benchmark direction (read or write).
 */
typedef struct {
    uint32_t page_count;         ///< Number of pages exercised per pass
    uint32_t page_size;          ///< Page size in bytes

    /* Per-pass throughput in kB/s */
    float    pass_kbps[PERF_MAX_PASSES];
    uint32_t num_passes;

    /* Throughput aggregate across passes */
    float    mean_kbps;
    float    min_kbps;
    float    max_kbps;
    float    stddev_kbps;

    /* Per-page latency (us) — accumulated across all passes via Welford + histogram */
    int64_t  lat_min_us;
    int64_t  lat_max_us;
    int64_t  lat_mean_us;        ///< Welford mean across all passes × pages
    int64_t  lat_stddev_us;      ///< Welford stddev across all passes × pages
    int64_t  lat_p95_us;         ///< Interpolated from histogram

    /* Histogram accumulated across all passes */
    uint32_t lat_hist[PERF_LATENCY_BUCKETS];

    /* Welford online accumulators (internal, used during benchmark) */
    double   _wf_mean;
    double   _wf_m2;
    uint64_t _wf_n;
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
 * @brief Snapshot of the SPI bus/device and NAND config used for one run.
 *
 * Filled by the caller (perf_app_main.c) from the same literals used to
 * configure the hardware, then embedded in bench_cfg_t so the JSON emitter
 * can describe the exact setup that produced the numbers.
 * Only primitive C types — no IDF structs leak into perf_benchmarks.c.
 */
typedef struct {
    uint32_t clock_khz;         ///< SPI clock frequency in kHz
    uint8_t  io_mode;           ///< spi_nand_flash_io_mode_t value (SIO=0, DOUT=1, DIO=2, QOUT=3, QIO=4)
    bool     half_duplex;       ///< true when SPI_DEVICE_HALFDUPLEX flag is set
    bool     quad_pins_wired;   ///< true when quadwp/quadhd pins are assigned (not -1)
    int      dma_chan;          ///< SPI DMA channel (SPI_DMA_CH_AUTO = -1 in ESP-IDF)
    uint32_t max_transfer_sz;   ///< spi_bus_config_t.max_transfer_sz in bytes
    uint8_t  gc_factor;         ///< Dhara GC factor (0 = library default)
    uint8_t  gc_percentage;     ///< GC percentage (0 = library default)
} perf_spi_hw_config_t;

/**
 * @brief Configuration for all benchmarks — passed once to each run_* function.
 */
typedef struct {
    spi_nand_flash_device_t *flash;
    uint32_t                 num_pages;    ///< How many logical pages to exercise
    uint32_t                 num_passes;   ///< Repetitions for averaging (1–4)
    uint32_t                 page_size;    ///< Filled in by init from flash query
    bool                     verify_data;  ///< Read-back verify writes (slower but safe)
    perf_spi_hw_config_t     hw;           ///< SPI + NAND config snapshot for JSON export

    /* Zipf workload parameters (used by run_zipf_bench only) */
    float                    zipf_skew;    ///< Zipf exponent s ≥ 0.0 (0 = uniform, 1.0 = classic Zipf, >1 = more skewed)
    uint32_t                 zipf_seed;    ///< RNG seed for Zipf sampling (0 → use 42)
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
 * @brief Zipf-distributed write then Zipf-distributed read benchmark.
 *
 * Samples cfg->num_pages page indices from a Zipf distribution (exponent
 * cfg->zipf_skew) for the write phase, then samples a fresh sequence for
 * the read phase.  Hot pages are visited many times; cold pages rarely or
 * never — stressing Dhara's remapping under skewed write pressure.
 *
 * cfg->zipf_skew = 0.0  → uniform (equivalent to random bench)
 * cfg->zipf_skew = 1.0  → classic Zipf
 * cfg->zipf_skew > 1.0  → increasingly concentrated on the first few pages
 */
esp_err_t run_zipf_bench(const bench_cfg_t *cfg, bench_result_t *result);

/**
 * @brief Print a bench_result_t in a human-readable format to the ESP log.
 */
void perf_print_result(const bench_result_t *result);

/**
 * @brief Print a compact comparison table for an array of results.
 */
void perf_print_summary_table(const bench_result_t *results, uint32_t count);

/**
 * @brief Emit one compact JSON report on stdout between PERF_JSON_BEGIN / PERF_JSON_END.
 *
 * Uses printf (not ESP_LOG) so the payload has no log prefix. Call after benchmarks
 * and human-readable summary. Requires ``cfg->flash`` for logical page count in ``flash``
 * and ``cfg->hw`` populated for the SPI config block.
 */
void perf_emit_benchmark_report_json(const bench_cfg_t *cfg,
                                     const bench_result_t *results,
                                     uint32_t result_count);

#ifdef __cplusplus
}
#endif
