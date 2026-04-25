/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static char *dup_json_string(const cJSON *item)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return NULL;
    }

    size_t len = strlen(item->valuestring) + 1;
    char *copy = malloc(len);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, item->valuestring, len);
    return copy;
}

static bool parse_required_u32(cJSON *obj, const char *name, uint32_t *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return false;
    }

    *out = (uint32_t)item->valuedouble;
    return true;
}

static void parse_optional_u32(cJSON *obj, const char *name, uint32_t *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0) {
        *out = (uint32_t)item->valuedouble;
    }
}

static void parse_optional_double(cJSON *obj, const char *name, double *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
    }
}

static bool parse_required_string(cJSON *obj, const char *name, char **out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    *out = dup_json_string(item);
    return *out != NULL;
}

static void free_scenario_config(scenario_config_t *scenario)
{
    if (scenario == NULL) {
        return;
    }

    free(scenario->name);
    free(scenario->preset);
}

static void free_ftl_config(ftl_config_t *ftl_cfg)
{
    if (ftl_cfg == NULL) {
        return;
    }

    free(ftl_cfg->name);
    free(ftl_cfg->ftl);
}

static void free_workload_config(workload_config_t *workload)
{
    if (workload == NULL) {
        return;
    }

    free(workload->type);
}

void app_config_free(sweep_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    free(cfg->name);
    free(cfg->output);

    for (uint32_t i = 0; i < cfg->scenario_count; i++) {
        free_scenario_config(&cfg->scenarios[i]);
    }
    free(cfg->scenarios);

    for (uint32_t i = 0; i < cfg->ftl_config_count; i++) {
        free_ftl_config(&cfg->ftl_configs[i]);
    }
    free(cfg->ftl_configs);

    free_workload_config(&cfg->workload);
    free(cfg);
}

static bool parse_nand_geometry(cJSON *root, nand_geometry_t *nand)
{
    cJSON *nand_obj = cJSON_GetObjectItemCaseSensitive(root, "nand");
    if (!cJSON_IsObject(nand_obj)) {
        return false;
    }

    return parse_required_u32(nand_obj, "num_blocks", &nand->num_blocks) &&
           parse_required_u32(nand_obj, "pages_per_block", &nand->pages_per_block) &&
           parse_required_u32(nand_obj, "page_size", &nand->page_size);
}

static bool parse_scenario(cJSON *obj, scenario_config_t *scenario)
{
    if (!parse_required_string(obj, "name", &scenario->name)) {
        return false;
    }

    cJSON *preset = cJSON_GetObjectItemCaseSensitive(obj, "preset");
    if (cJSON_IsString(preset)) {
        scenario->preset = dup_json_string(preset);
        if (scenario->preset == NULL) {
            return false;
        }
    }

    parse_optional_u32(obj, "factory_bad_block_count", &scenario->factory_bad_block_count);
    parse_optional_u32(obj, "max_erase_cycles", &scenario->max_erase_cycles);
    parse_optional_u32(obj, "max_prog_cycles", &scenario->max_prog_cycles);
    parse_optional_u32(obj, "grave_page_threshold", &scenario->grave_page_threshold);
    parse_optional_double(obj, "read_fail_prob", &scenario->read_fail_prob);
    parse_optional_double(obj, "prog_fail_prob", &scenario->prog_fail_prob);
    parse_optional_double(obj, "erase_fail_prob", &scenario->erase_fail_prob);
    parse_optional_double(obj, "copy_fail_prob", &scenario->copy_fail_prob);
    parse_optional_u32(obj, "op_fail_seed", &scenario->op_fail_seed);
    parse_optional_u32(obj, "crash_after_ops_min", &scenario->crash_after_ops_min);
    parse_optional_u32(obj, "crash_after_ops_max", &scenario->crash_after_ops_max);
    parse_optional_double(obj, "crash_probability", &scenario->crash_probability);
    parse_optional_u32(obj, "crash_seed", &scenario->crash_seed);
    parse_optional_u32(obj, "ecc_mid_threshold", &scenario->ecc_mid_threshold);
    parse_optional_u32(obj, "ecc_high_threshold", &scenario->ecc_high_threshold);
    parse_optional_u32(obj, "ecc_fail_threshold", &scenario->ecc_fail_threshold);

    return true;
}

static bool parse_scenarios(cJSON *root, sweep_config_t *cfg)
{
    cJSON *scenarios = cJSON_GetObjectItemCaseSensitive(root, "scenarios");
    if (!cJSON_IsArray(scenarios)) {
        return false;
    }

    int count = cJSON_GetArraySize(scenarios);
    if (count <= 0) {
        return false;
    }

    cfg->scenarios = calloc((size_t)count, sizeof(scenario_config_t));
    if (cfg->scenarios == NULL) {
        return false;
    }

    cfg->scenario_count = (uint32_t)count;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(scenarios, i);
        if (!cJSON_IsObject(item) || !parse_scenario(item, &cfg->scenarios[i])) {
            return false;
        }
    }

    return true;
}

static bool parse_ftl_gc(cJSON *item, ftl_config_t *ftl_cfg)
{
    cJSON *pct_item = cJSON_GetObjectItemCaseSensitive(item, "gc_overhead_percent");
    cJSON *legacy_item = cJSON_GetObjectItemCaseSensitive(item, "gc_factor");

    if (cJSON_IsNumber(pct_item) && pct_item->valuedouble > 0.0 && pct_item->valuedouble < 100.0) {
        ftl_cfg->gc_overhead_percent = pct_item->valuedouble;
    } else if (cJSON_IsNumber(legacy_item) && legacy_item->valuedouble >= 1.0) {
        double gc_ratio = legacy_item->valuedouble;
        ftl_cfg->gc_overhead_percent = 100.0 / (gc_ratio + 1.0);
    } else {
        return false;
    }

    double ratio = round(100.0 / ftl_cfg->gc_overhead_percent - 1.0);
    if (ratio < 1.0) {
        ratio = 1.0;
    } else if (ratio > 255.0) {
        ratio = 255.0;
    }
    ftl_cfg->gc_ratio = (uint8_t)ratio;
    return true;
}

static bool parse_ftl_configs(cJSON *root, sweep_config_t *cfg)
{
    cJSON *ftl_configs = cJSON_GetObjectItemCaseSensitive(root, "ftl_configs");
    if (!cJSON_IsArray(ftl_configs)) {
        return false;
    }

    int count = cJSON_GetArraySize(ftl_configs);
    if (count <= 0) {
        return false;
    }

    cfg->ftl_configs = calloc((size_t)count, sizeof(ftl_config_t));
    if (cfg->ftl_configs == NULL) {
        return false;
    }

    cfg->ftl_config_count = (uint32_t)count;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(ftl_configs, i);
        if (!cJSON_IsObject(item) ||
            !parse_required_string(item, "name", &cfg->ftl_configs[i].name) ||
            !parse_required_string(item, "ftl", &cfg->ftl_configs[i].ftl) ||
            !parse_ftl_gc(item, &cfg->ftl_configs[i])) {
            return false;
        }
    }

    return true;
}

static bool parse_workload(cJSON *root, workload_config_t *workload)
{
    cJSON *workload_obj = cJSON_GetObjectItemCaseSensitive(root, "workload");
    bool ok;

    if (!cJSON_IsObject(workload_obj)) {
        return false;
    }

    ok = parse_required_string(workload_obj, "type", &workload->type) &&
         parse_required_u32(workload_obj, "total_writes", &workload->total_writes) &&
         parse_required_u32(workload_obj, "write_size_bytes", &workload->write_size_bytes);
    parse_optional_u32(workload_obj, "seed", &workload->seed);
    return ok;
}

sweep_config_t *app_config_parse(const char *json_str)
{
    if (json_str == NULL) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        return NULL;
    }

    sweep_config_t *cfg = calloc(1, sizeof(sweep_config_t));
    if (cfg == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    bool ok = parse_required_string(root, "name", &cfg->name) &&
              parse_required_string(root, "output", &cfg->output) &&
              parse_nand_geometry(root, &cfg->nand) &&
              parse_scenarios(root, cfg) &&
              parse_ftl_configs(root, cfg) &&
              parse_workload(root, &cfg->workload);

    cJSON_Delete(root);

    if (!ok) {
        app_config_free(cfg);
        return NULL;
    }

    return cfg;
}
