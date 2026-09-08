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
 * Both "free now" and "minimum free since boot" are reported per bucket.
 * The minimum is the more revealing number for transient allocations:
 * a snapshot taken only after nand_ubi_attach() returns would miss the
 * temporary sqnum_seen[] spike entirely, since it is freed before attach()
 * returns. Call nand_ubi_log_ram() immediately after each step you want to
 * measure, so the "min" column captures whatever happened during that step.
 */

#include "esp_heap_caps.h"
#include "esp_log.h"

/**
 * @brief Log current and minimum-ever free heap, split by capability, under a caller-supplied tag.
 *
 * @param tag   ESP_LOG tag to log under (pass the calling example's own TAG so output interleaves
 *              naturally with the rest of its log lines).
 * @param label Short label identifying which step this checkpoint follows (e.g. "after attach").
 */
static inline void nand_ubi_log_ram(const char *tag, const char *label)
{
#if CONFIG_SPIRAM
    ESP_LOGI(tag, "[RAM %-24s] internal free=%u (min=%u)  spiram free=%u (min=%u)",
             label,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
#else
    ESP_LOGI(tag, "[RAM %-24s] internal free=%u (min=%u)  (no PSRAM)",
             label,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
#endif
}
