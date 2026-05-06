/*
 * SPDX-FileCopyrightText: 2026 Martin Havlik <omnitex.git@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_blockdev.h"
#include "esp_err.h"
#include "nand_device_types.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t reads_attempted;
    uint32_t reads_succeeded;
    uint32_t writes_attempted;
    uint32_t writes_succeeded;
    double write_amplification_factor;
    uint32_t total_erases;
    uint32_t total_prog_ops;
    uint32_t bad_blocks_initial;
    uint32_t bad_blocks_final;
    double mean_erase_count;
    uint32_t max_erase_count;
    uint32_t min_erase_count;
    double erase_count_stddev;
    uint32_t erase_count_p50;
    uint32_t erase_count_p90;
    uint32_t erase_count_p99;
    double erase_count_gini;
    uint32_t blocks_worn_out;
    uint32_t first_worn_out_at_write;
    uint32_t ftl_errors;

    /* ECC read-disturb event counters (fed by on_page_read_ecc callback) */
    uint32_t ecc_mid_events;    /*!< NAND_ECC_1_TO_3_BITS_CORRECTED occurrences */
    uint32_t ecc_high_events;   /*!< NAND_ECC_4_TO_6_BITS_CORRECTED occurrences */
    uint32_t ecc_fail_events;   /*!< NAND_ECC_NOT_CORRECTED occurrences */

    /* Page-relief event counters (fed by on_page_relief callback) */
    uint32_t page_relief_skips;           /*!< Pre-prog ECC checks that returned elevated status */
    uint32_t page_relief_first_at_write;  /*!< writes_attempted value when first relief event fired */
} metrics_t;

void metrics_reset(metrics_t *m);
void metrics_snapshot(metrics_t *dst, const metrics_t *src);
void metrics_record_read(metrics_t *m, bool success);
void metrics_record_write(metrics_t *m, bool success);
void metrics_record_ftl_error(metrics_t *m);
void metrics_record_worn_out(metrics_t *m, uint32_t write_index);
void metrics_record_ecc_event(metrics_t *m, nand_ecc_status_t status);
void metrics_record_page_relief(metrics_t *m);
esp_err_t metrics_collect_bad_blocks(metrics_t *m, esp_blockdev_handle_t bdl, bool initial_sample);
esp_err_t metrics_collect_prog_stats(metrics_t *m, uint32_t num_pages);
esp_err_t metrics_collect_erase_stats(metrics_t *m, uint32_t num_blocks);

#ifdef __cplusplus
}
#endif
