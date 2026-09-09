/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

/**
 * @file nand_ubi_ram_stats.h
 *
 * Header-only RAM instrumentation shared by esp_nand_ubi hardware examples.
 *
 * esp_nand_ubi's own plan doc (docs/plans/2026-07-09-esp-nand-ubi-mvp.md,
 * "RAM Optimization Strategy") makes specific quantitative claims about its
 * footprint -- e.g. ~4.5 KB resident for a 1024-PEB chip, transient spikes
 * during nand_ubi_attach()'s scan (page_buf, sqnum_seen[]) that are freed
 * before attach() returns. nand_ubi_log_ram() lets examples verify those
 * claims on real hardware instead of leaving them as paper estimates.
 *
 * Two capability buckets are reported:
 *   - MALLOC_CAP_INTERNAL: internal SRAM, always present, the scarce
 *     resource esp_nand_ubi's own RAM strategy is designed around.
 *   - MALLOC_CAP_SPIRAM: only meaningful when CONFIG_SPIRAM is enabled;
 *     ubi_alloc() (src/nand_ubi.c) prefers this when available.
 *
 * Both "free now" and "minimum free since boot" are reported per bucket, in
 * KiB rather than raw byte counts (bytes with no separators are hard to
 * eyeball at a glance -- "324.9 KiB" reads faster than "332696"). The
 * minimum is the more revealing number for transient allocations: a
 * snapshot taken only after nand_ubi_attach() returns would miss the
 * temporary sqnum_seen[] spike entirely, since it is freed before attach()
 * returns. Call nand_ubi_log_ram() immediately after each step you want to
 * measure, so the "min" column captures whatever happened during that step.
 *
 * Baseline and percentage-used: the *first* call to nand_ubi_log_ram() in a
 * given run captures its internal-SRAM free size as the baseline -- call it
 * before touching any blockdev/SPI/UBI/filesystem API (both examples already
 * do this as their very first line: nand_ubi_log_ram(TAG, "before init")).
 * Every later call then also prints how much internal SRAM has been used
 * relative to that baseline (baseline minus the *minimum* free-ever, i.e.
 * the high-water mark) and what percentage of the baseline that represents,
 * so "is step X eating an unreasonable chunk of what we started with" is a
 * single number instead of mental subtraction across log lines.
 *
 * Baseline state is function-local static storage inside this header, so it
 * only behaves as a single running baseline if all nand_ubi_log_ram() calls
 * in a given process come from the same translation unit (true for both
 * bundled examples: each has exactly one main.c including this header).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

/**
 * @brief Format a byte count as "NNN.N KiB" without pulling in printf's
 *        float support (often stripped under CONFIG_NEWLIB_NANO_FORMAT).
 */
static inline void nand_ubi_fmt_kib(size_t bytes, char *buf, size_t bufsize)
{
    unsigned whole = (unsigned)(bytes / 1024);
    unsigned tenths = (unsigned)(((bytes % 1024) * 10) / 1024);
    snprintf(buf, bufsize, "%u.%u KiB", whole, tenths);
}

/**
 * @brief Log current and minimum-ever free heap, split by capability, under a caller-supplied tag.
 *
 * The very first call in a process becomes the internal-SRAM baseline; every call (including the
 * first) also reports internal SRAM used-since-baseline and that used amount as a percentage of
 * the baseline. See the file-level comment above for why this only works as one running baseline
 * per translation unit.
 *
 * @param tag   ESP_LOG tag to log under (pass the calling example's own TAG so output interleaves
 *              naturally with the rest of its log lines).
 * @param label Short label identifying which step this checkpoint follows (e.g. "after attach").
 */
static inline void nand_ubi_log_ram(const char *tag, const char *label)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    static bool s_have_baseline = false;
    static size_t s_baseline_internal = 0;
    if (!s_have_baseline) {
        s_have_baseline = true;
        s_baseline_internal = internal_free;
    }
    size_t used_since_baseline = (s_baseline_internal > internal_min) ? (s_baseline_internal - internal_min) : 0;
    unsigned pct_tenths = s_baseline_internal
                          ? (unsigned)(((uint64_t)used_since_baseline * 1000) / s_baseline_internal)
                          : 0;

    char free_buf[16];
    char min_buf[16];
    char used_buf[16];
    nand_ubi_fmt_kib(internal_free, free_buf, sizeof(free_buf));
    nand_ubi_fmt_kib(internal_min, min_buf, sizeof(min_buf));
    nand_ubi_fmt_kib(used_since_baseline, used_buf, sizeof(used_buf));

#if CONFIG_SPIRAM
    char spiram_free_buf[16];
    char spiram_min_buf[16];
    nand_ubi_fmt_kib(heap_caps_get_free_size(MALLOC_CAP_SPIRAM), spiram_free_buf, sizeof(spiram_free_buf));
    nand_ubi_fmt_kib(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM), spiram_min_buf, sizeof(spiram_min_buf));
    ESP_LOGI(tag, "[RAM %-28s] internal: %8s free (min %8s)  used vs baseline: %8s (%2u.%u%%)  spiram: %8s free (min %8s)",
             label, free_buf, min_buf, used_buf, pct_tenths / 10, pct_tenths % 10, spiram_free_buf, spiram_min_buf);
#else
    ESP_LOGI(tag, "[RAM %-28s] internal: %8s free (min %8s)  used vs baseline: %8s (%2u.%u%%)  (no PSRAM)",
             label, free_buf, min_buf, used_buf, pct_tenths / 10, pct_tenths % 10);
#endif
}
