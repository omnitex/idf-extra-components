/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "metrics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nand_fault_sim.h"
#include "spi_nand_flash.h"

#include "../../../priv_include/nand_impl.h"

static int compare_u32_ascending(const void *lhs, const void *rhs)
{
    uint32_t left = *(const uint32_t *)lhs;
    uint32_t right = *(const uint32_t *)rhs;

    if (left < right) {
        return -1;
    }
    if (left > right) {
        return 1;
    }
    return 0;
}

static void metrics_update_waf(metrics_t *m)
{
    uint32_t divisor = (m->writes_succeeded > 0) ? m->writes_succeeded : m->writes_attempted;

    if (divisor == 0) {
        m->write_amplification_factor = 0.0;
        return;
    }

    m->write_amplification_factor = (double)m->total_prog_ops / (double)divisor;
}

static uint32_t percentile_by_index(const uint32_t *sorted, uint32_t count, uint32_t percentile)
{
    uint32_t index;

    if (count == 0) {
        return 0;
    }

    index = ((count - 1u) * percentile) / 100u;
    return sorted[index];
}

void metrics_reset(metrics_t *m)
{
    if (m == NULL) {
        return;
    }

    memset(m, 0, sizeof(*m));
}

void metrics_snapshot(metrics_t *dst, const metrics_t *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }

    memcpy(dst, src, sizeof(*dst));
}

void metrics_record_read(metrics_t *m, bool success)
{
    if (m == NULL) {
        return;
    }

    m->reads_attempted++;
    if (success) {
        m->reads_succeeded++;
    }
}

void metrics_record_write(metrics_t *m, bool success)
{
    if (m == NULL) {
        return;
    }

    m->writes_attempted++;
    if (success) {
        m->writes_succeeded++;
    }
    metrics_update_waf(m);
}

void metrics_record_ftl_error(metrics_t *m)
{
    if (m == NULL) {
        return;
    }

    m->ftl_errors++;
}

void metrics_record_worn_out(metrics_t *m, uint32_t write_index)
{
    if (m == NULL) {
        return;
    }

    m->blocks_worn_out++;
    if (m->first_worn_out_at_write == 0) {
        m->first_worn_out_at_write = write_index;
    }
}

esp_err_t metrics_collect_bad_blocks(metrics_t *m, spi_nand_flash_device_t *nand, bool initial_sample)
{
    uint32_t bad_blocks = 0;
    uint32_t num_blocks = 0;
    uint32_t block = 0;
    esp_err_t err = ESP_OK;

    if (m == NULL || nand == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = spi_nand_flash_get_block_num(nand, &num_blocks);
    if (err != ESP_OK) {
        return err;
    }

    for (block = 0; block < num_blocks; block++) {
        bool is_bad = false;

        err = nand_is_bad(nand, block, &is_bad);
        if (err != ESP_OK) {
            return err;
        }

        if (is_bad) {
            bad_blocks++;
        }
    }

    if (initial_sample) {
        m->bad_blocks_initial = bad_blocks;
    } else {
        m->bad_blocks_final = bad_blocks;
    }

    return ESP_OK;
}

esp_err_t metrics_collect_prog_stats(metrics_t *m, uint32_t num_pages)
{
    uint64_t total_prog_ops = 0;

    if (m == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t page = 0; page < num_pages; page++) {
        total_prog_ops += nand_fault_sim_get_prog_count(page);
    }

    m->total_prog_ops = (uint32_t)total_prog_ops;
    metrics_update_waf(m);
    return ESP_OK;
}

esp_err_t metrics_collect_erase_stats(metrics_t *m, uint32_t num_blocks)
{
    uint32_t *erase_counts;
    uint64_t sum = 0;
    double variance_acc = 0.0;
    double gini_numerator = 0.0;

    if (m == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (num_blocks == 0) {
        m->total_erases = 0;
        m->mean_erase_count = 0.0;
        m->max_erase_count = 0;
        m->min_erase_count = 0;
        m->erase_count_stddev = 0.0;
        m->erase_count_p50 = 0;
        m->erase_count_p90 = 0;
        m->erase_count_p99 = 0;
        m->erase_count_gini = 0.0;
        return ESP_OK;
    }

    erase_counts = calloc(num_blocks, sizeof(uint32_t));
    if (erase_counts == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t block = 0; block < num_blocks; block++) {
        erase_counts[block] = nand_fault_sim_get_erase_count(block);
        sum += erase_counts[block];
    }

    qsort(erase_counts, num_blocks, sizeof(uint32_t), compare_u32_ascending);

    m->total_erases = (uint32_t)sum;
    m->min_erase_count = erase_counts[0];
    m->max_erase_count = erase_counts[num_blocks - 1];
    m->mean_erase_count = (double)sum / (double)num_blocks;
    m->erase_count_p50 = percentile_by_index(erase_counts, num_blocks, 50);
    m->erase_count_p90 = percentile_by_index(erase_counts, num_blocks, 90);
    m->erase_count_p99 = percentile_by_index(erase_counts, num_blocks, 99);

    for (uint32_t i = 0; i < num_blocks; i++) {
        double delta = (double)erase_counts[i] - m->mean_erase_count;
        variance_acc += delta * delta;
        gini_numerator += (double)(i + 1u) * (double)erase_counts[i];
    }

    m->erase_count_stddev = sqrt(variance_acc / (double)num_blocks);
    if (sum == 0) {
        m->erase_count_gini = 0.0;
    } else {
        m->erase_count_gini = ((2.0 * gini_numerator) / ((double)num_blocks * (double)sum)) -
                              ((double)num_blocks + 1.0) / (double)num_blocks;
    }

    free(erase_counts);
    return ESP_OK;
}
