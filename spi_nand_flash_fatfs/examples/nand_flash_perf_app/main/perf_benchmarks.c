/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"

#include "spi_nand_flash.h"
#include "spi_nand_flash_test_helpers.h"
#include "perf_benchmarks.h"

static const char *TAG = "perf_app";

/* Bucket upper edges in microseconds; last bucket is open-ended (INT64_MAX) */
static const int64_t s_lat_bucket_us[PERF_LATENCY_BUCKETS] = {500, 1000, 1500, 2000, 2500, 3000, 5000, 10000, INT64_MAX};

static const char *s_lat_bucket_labels[PERF_LATENCY_BUCKETS] = {
    "<500us", "500-1ms", "1-1.5ms", "1.5-2ms", "2-2.5ms", "2.5-3ms", "3-5ms", "5-10ms", ">=10ms"
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Feed one page latency sample into Welford online mean/variance accumulators */
static void welford_update(perf_direction_result_t *out, int64_t lat_us)
{
    out->_wf_n++;
    double delta = (double)lat_us - out->_wf_mean;
    out->_wf_mean += delta / (double)out->_wf_n;
    double delta2 = (double)lat_us - out->_wf_mean;
    out->_wf_m2 += delta * delta2;
}

/* Accumulate one pass worth of latencies: update Welford, min/max, histogram */
static void accumulate_pass(perf_direction_result_t *out,
                             const int64_t *lats, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        int64_t lat = lats[i];

        welford_update(out, lat);

        if (lat < out->lat_min_us) {
            out->lat_min_us = lat;
        }
        if (lat > out->lat_max_us) {
            out->lat_max_us = lat;
        }

        for (int b = 0; b < PERF_LATENCY_BUCKETS; b++) {
            if (lat < s_lat_bucket_us[b]) {
                out->lat_hist[b]++;
                break;
            }
        }
    }
}

/* Finalise latency stats from Welford accumulators + histogram p95 interpolation */
static void finalise_lat_stats(perf_direction_result_t *out)
{
    if (out->_wf_n == 0) {
        return;
    }

    out->lat_mean_us   = (int64_t)out->_wf_mean;
    out->lat_stddev_us = (int64_t)sqrt(out->_wf_m2 / (double)out->_wf_n);

    /* p95 via histogram: find bucket containing the 95th-percentile sample */
    uint64_t target = (out->_wf_n * 95) / 100;
    uint64_t cumulative = 0;
    out->lat_p95_us = out->lat_max_us;
    for (int b = 0; b < PERF_LATENCY_BUCKETS; b++) {
        cumulative += out->lat_hist[b];
        if (cumulative > target) {
            out->lat_p95_us = s_lat_bucket_us[b] == INT64_MAX
                              ? out->lat_max_us
                              : s_lat_bucket_us[b];
            break;
        }
    }
}

/* Compute throughput aggregate stats from pass_kbps[] array */
static void compute_tp_stats(perf_direction_result_t *out)
{
    if (out->num_passes == 0) {
        return;
    }
    float sum = 0.0f, mn = out->pass_kbps[0], mx = out->pass_kbps[0];
    for (uint32_t i = 0; i < out->num_passes; i++) {
        sum += out->pass_kbps[i];
        if (out->pass_kbps[i] < mn) {
            mn = out->pass_kbps[i];
        }
        if (out->pass_kbps[i] > mx) {
            mx = out->pass_kbps[i];
        }
    }
    out->mean_kbps = sum / out->num_passes;
    out->min_kbps  = mn;
    out->max_kbps  = mx;

    double var = 0.0;
    for (uint32_t i = 0; i < out->num_passes; i++) {
        double d = (double)out->pass_kbps[i] - (double)out->mean_kbps;
        var += d * d;
    }
    out->stddev_kbps = (float)sqrt(var / out->num_passes);
}

/* Initialise a perf_direction_result_t before accumulating passes into it */
static void direction_result_init(perf_direction_result_t *out,
                                   uint32_t page_count, uint32_t page_size,
                                   uint32_t num_passes)
{
    memset(out, 0, sizeof(*out));
    out->page_count  = page_count;
    out->page_size   = page_size;
    out->num_passes  = num_passes;
    out->lat_min_us  = INT64_MAX;
    out->lat_max_us  = INT64_MIN;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t perf_warmup(const bench_cfg_t *cfg)
{
    uint8_t *buf = heap_caps_malloc(cfg->page_size, MALLOC_CAP_DMA);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t p = 0; p < cfg->num_pages; p++) {
        spi_nand_flash_fill_buffer_seeded(buf, cfg->page_size / 4, p);
        ESP_ERROR_CHECK(spi_nand_flash_write_page(cfg->flash, buf, p));
    }
    ESP_ERROR_CHECK(spi_nand_flash_sync(cfg->flash));

    for (uint32_t p = 0; p < cfg->num_pages; p++) {
        ESP_ERROR_CHECK(spi_nand_flash_read_page(cfg->flash, buf, p));
    }

    free(buf);
    return ESP_OK;
}

esp_err_t run_sequential_bench(const bench_cfg_t *cfg, bench_result_t *result)
{
    int64_t *lats = heap_caps_malloc(cfg->num_pages * sizeof(int64_t), MALLOC_CAP_DEFAULT);
    uint8_t *buf  = heap_caps_malloc(cfg->page_size, MALLOC_CAP_DMA);

    if (!lats || !buf) {
        free(lats);
        free(buf);
        return ESP_ERR_NO_MEM;
    }

    direction_result_init(&result->write, cfg->num_pages, cfg->page_size, cfg->num_passes);
    direction_result_init(&result->read,  cfg->num_pages, cfg->page_size, cfg->num_passes);

    /* WRITE PHASE */
    for (uint32_t pass = 0; pass < cfg->num_passes; pass++) {
        int64_t pass_start = esp_timer_get_time();
        for (uint32_t p = 0; p < cfg->num_pages; p++) {
            spi_nand_flash_fill_buffer_seeded(buf, cfg->page_size / 4,
                                              pass * cfg->num_pages + p);
            int64_t t0 = esp_timer_get_time();
            ESP_ERROR_CHECK(spi_nand_flash_write_page(cfg->flash, buf, p));
            lats[p] = esp_timer_get_time() - t0;
        }
        ESP_ERROR_CHECK(spi_nand_flash_sync(cfg->flash));
        int64_t pass_elapsed = esp_timer_get_time() - pass_start;
        result->write.pass_kbps[pass] =
            (float)(cfg->page_size * cfg->num_pages) / (float)pass_elapsed * 1000.0f;
        accumulate_pass(&result->write, lats, cfg->num_pages);
        ESP_LOGI(TAG, "  [%s] write pass %" PRIu32 "/%" PRIu32 ": %.0f kB/s",
                 result->name, pass + 1, cfg->num_passes, result->write.pass_kbps[pass]);
    }

    /* READ PHASE */
    for (uint32_t pass = 0; pass < cfg->num_passes; pass++) {
        int64_t pass_start = esp_timer_get_time();
        for (uint32_t p = 0; p < cfg->num_pages; p++) {
            int64_t t0 = esp_timer_get_time();
            ESP_ERROR_CHECK(spi_nand_flash_read_page(cfg->flash, buf, p));
            lats[p] = esp_timer_get_time() - t0;

            if (cfg->verify_data) {
                int mismatch = spi_nand_flash_check_buffer_seeded(buf, cfg->page_size / 4,
                               (cfg->num_passes - 1) * cfg->num_pages + p);
                if (mismatch) {
                    ESP_LOGW(TAG, "seq read verify mismatch: page %" PRIu32 " word %d", p, mismatch);
                }
            }
        }
        int64_t pass_elapsed = esp_timer_get_time() - pass_start;
        result->read.pass_kbps[pass] =
            (float)(cfg->page_size * cfg->num_pages) / (float)pass_elapsed * 1000.0f;
        accumulate_pass(&result->read, lats, cfg->num_pages);
        ESP_LOGI(TAG, "  [%s] read  pass %" PRIu32 "/%" PRIu32 ": %.0f kB/s",
                 result->name, pass + 1, cfg->num_passes, result->read.pass_kbps[pass]);
    }

    compute_tp_stats(&result->write);
    finalise_lat_stats(&result->write);
    compute_tp_stats(&result->read);
    finalise_lat_stats(&result->read);

    free(lats);
    free(buf);
    return ESP_OK;
}

esp_err_t run_random_bench(const bench_cfg_t *cfg, bench_result_t *result)
{
    int64_t  *lats       = heap_caps_malloc(cfg->num_pages * sizeof(int64_t), MALLOC_CAP_DEFAULT);
    uint8_t  *buf        = heap_caps_malloc(cfg->page_size, MALLOC_CAP_DMA);
    uint32_t *page_order = heap_caps_malloc(cfg->num_pages * sizeof(uint32_t), MALLOC_CAP_DEFAULT);

    if (!lats || !buf || !page_order) {
        free(lats);
        free(buf);
        free(page_order);
        return ESP_ERR_NO_MEM;
    }

    /* Fill page order 0..N-1 */
    for (uint32_t i = 0; i < cfg->num_pages; i++) {
        page_order[i] = i;
    }

    /* Fisher-Yates shuffle using esp_random() */
    for (uint32_t i = cfg->num_pages - 1; i > 0; i--) {
        uint32_t j = esp_random() % (i + 1);
        uint32_t tmp = page_order[i];
        page_order[i] = page_order[j];
        page_order[j] = tmp;
    }

    direction_result_init(&result->write, cfg->num_pages, cfg->page_size, cfg->num_passes);
    direction_result_init(&result->read,  cfg->num_pages, cfg->page_size, cfg->num_passes);

    /* WRITE PHASE */
    for (uint32_t pass = 0; pass < cfg->num_passes; pass++) {
        int64_t pass_start = esp_timer_get_time();
        for (uint32_t p = 0; p < cfg->num_pages; p++) {
            uint32_t page_idx = page_order[p];
            spi_nand_flash_fill_buffer_seeded(buf, cfg->page_size / 4,
                                              pass * cfg->num_pages + page_idx);
            int64_t t0 = esp_timer_get_time();
            ESP_ERROR_CHECK(spi_nand_flash_write_page(cfg->flash, buf, page_idx));
            lats[p] = esp_timer_get_time() - t0;
        }
        ESP_ERROR_CHECK(spi_nand_flash_sync(cfg->flash));
        int64_t pass_elapsed = esp_timer_get_time() - pass_start;
        result->write.pass_kbps[pass] =
            (float)(cfg->page_size * cfg->num_pages) / (float)pass_elapsed * 1000.0f;
        accumulate_pass(&result->write, lats, cfg->num_pages);
        ESP_LOGI(TAG, "  [%s] write pass %" PRIu32 "/%" PRIu32 ": %.0f kB/s",
                 result->name, pass + 1, cfg->num_passes, result->write.pass_kbps[pass]);
    }

    /* READ PHASE */
    for (uint32_t pass = 0; pass < cfg->num_passes; pass++) {
        int64_t pass_start = esp_timer_get_time();
        for (uint32_t p = 0; p < cfg->num_pages; p++) {
            uint32_t page_idx = page_order[p];
            int64_t t0 = esp_timer_get_time();
            ESP_ERROR_CHECK(spi_nand_flash_read_page(cfg->flash, buf, page_idx));
            lats[p] = esp_timer_get_time() - t0;

            if (cfg->verify_data) {
                int mismatch = spi_nand_flash_check_buffer_seeded(buf, cfg->page_size / 4,
                               (cfg->num_passes - 1) * cfg->num_pages + page_idx);
                if (mismatch) {
                    ESP_LOGW(TAG, "rnd read verify mismatch: page %" PRIu32 " word %d", page_idx, mismatch);
                }
            }
        }
        int64_t pass_elapsed = esp_timer_get_time() - pass_start;
        result->read.pass_kbps[pass] =
            (float)(cfg->page_size * cfg->num_pages) / (float)pass_elapsed * 1000.0f;
        accumulate_pass(&result->read, lats, cfg->num_pages);
        ESP_LOGI(TAG, "  [%s] read  pass %" PRIu32 "/%" PRIu32 ": %.0f kB/s",
                 result->name, pass + 1, cfg->num_passes, result->read.pass_kbps[pass]);
    }

    compute_tp_stats(&result->write);
    finalise_lat_stats(&result->write);
    compute_tp_stats(&result->read);
    finalise_lat_stats(&result->read);

    free(lats);
    free(buf);
    free(page_order);
    return ESP_OK;
}

void perf_print_result(const bench_result_t *result)
{
    const perf_direction_result_t *w = &result->write;
    const perf_direction_result_t *r = &result->read;

    ESP_LOGI(TAG, "[PERF] %s WRITE (%" PRIu32 " pages x %" PRIu32 " passes):",
             result->name, w->page_count, w->num_passes);
    for (uint32_t i = 0; i < w->num_passes; i++) {
        ESP_LOGI(TAG, "  Pass %" PRIu32 ": %.0f kB/s", i + 1, w->pass_kbps[i]);
    }
    ESP_LOGI(TAG, "  Mean: %.0f kB/s  Min: %.0f  Max: %.0f  StdDev: %.1f kB/s",
             w->mean_kbps, w->min_kbps, w->max_kbps, w->stddev_kbps);
    ESP_LOGI(TAG, "  Latency (us): min=%" PRId64 "  mean=%" PRId64 "  stddev=%" PRId64 "  p95=%" PRId64 "  max=%" PRId64,
             w->lat_min_us, w->lat_mean_us, w->lat_stddev_us, w->lat_p95_us, w->lat_max_us);
    for (int b = 0; b < PERF_LATENCY_BUCKETS; b++) {
        ESP_LOGI(TAG, "    %s: %" PRIu32, s_lat_bucket_labels[b], w->lat_hist[b]);
    }

    ESP_LOGI(TAG, "[PERF] %s READ (%" PRIu32 " pages x %" PRIu32 " passes):",
             result->name, r->page_count, r->num_passes);
    for (uint32_t i = 0; i < r->num_passes; i++) {
        ESP_LOGI(TAG, "  Pass %" PRIu32 ": %.0f kB/s", i + 1, r->pass_kbps[i]);
    }
    ESP_LOGI(TAG, "  Mean: %.0f kB/s  Min: %.0f  Max: %.0f  StdDev: %.1f kB/s",
             r->mean_kbps, r->min_kbps, r->max_kbps, r->stddev_kbps);
    ESP_LOGI(TAG, "  Latency (us): min=%" PRId64 "  mean=%" PRId64 "  stddev=%" PRId64 "  p95=%" PRId64 "  max=%" PRId64,
             r->lat_min_us, r->lat_mean_us, r->lat_stddev_us, r->lat_p95_us, r->lat_max_us);
    for (int b = 0; b < PERF_LATENCY_BUCKETS; b++) {
        ESP_LOGI(TAG, "    %s: %" PRIu32, s_lat_bucket_labels[b], r->lat_hist[b]);
    }
}

void perf_print_summary_table(const bench_result_t *results, uint32_t count)
{
    ESP_LOGI(TAG, "=== Summary Table ===");
    ESP_LOGI(TAG, "%-12s | %10s | %10s | %10s | %10s",
             "Benchmark", "Wr kB/s", "Rd kB/s", "Wr p95 us", "Rd p95 us");
    ESP_LOGI(TAG, "-------------|------------|------------|------------|------------");
    for (uint32_t i = 0; i < count; i++) {
        ESP_LOGI(TAG, "%-12s | %10.0f | %10.0f | %10" PRId64 " | %10" PRId64,
                 results[i].name,
                 results[i].write.mean_kbps,
                 results[i].read.mean_kbps,
                 results[i].write.lat_p95_us,
                 results[i].read.lat_p95_us);
    }
}
