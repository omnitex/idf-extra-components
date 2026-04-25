/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "nand_fault_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t num_blocks;
    uint32_t pages_per_block;
    uint32_t page_size;
} nand_geometry_t;

typedef struct {
    char *name;
    char *preset;

    uint32_t factory_bad_block_count;
    uint32_t max_erase_cycles;
    uint32_t max_prog_cycles;
    uint32_t grave_page_threshold;

    double read_fail_prob;
    double prog_fail_prob;
    double erase_fail_prob;
    double copy_fail_prob;
    uint32_t op_fail_seed;

    uint32_t crash_after_ops_min;
    uint32_t crash_after_ops_max;
    double crash_probability;
    uint32_t crash_seed;

    uint32_t ecc_mid_threshold;
    uint32_t ecc_high_threshold;
    uint32_t ecc_fail_threshold;
} scenario_config_t;

typedef struct {
    char *name;
    char *ftl;
    uint32_t gc_factor;
} ftl_config_t;

typedef struct {
    char *type;
    uint32_t total_writes;
    uint32_t write_size_bytes;
    uint32_t seed;
} workload_config_t;

typedef struct {
    char *name;
    char *output;
    nand_geometry_t nand;
    scenario_config_t *scenarios;
    uint32_t scenario_count;
    ftl_config_t *ftl_configs;
    uint32_t ftl_config_count;
    workload_config_t workload;
} sweep_config_t;

sweep_config_t *app_config_parse(const char *json_str);
void app_config_free(sweep_config_t *cfg);

#ifdef __cplusplus
}
#endif
