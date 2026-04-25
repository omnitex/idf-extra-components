/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "runner.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "ftl.h"
#include "nand_fault_sim.h"
#include "nand_linux_mmap_emul.h"
#include "reporter/reporter.h"
#include "spi_nand_flash.h"
#include "workload.h"

static const char *TAG = "runner";

static esp_err_t scenario_preset_from_name(const char *preset, nand_sim_scenario_t *scenario)
{
    ESP_RETURN_ON_FALSE(preset != NULL, ESP_ERR_INVALID_ARG, TAG, "preset must not be NULL");
    ESP_RETURN_ON_FALSE(scenario != NULL, ESP_ERR_INVALID_ARG, TAG, "scenario must not be NULL");

    if (strcmp(preset, "FRESH") == 0) {
        *scenario = NAND_SIM_SCENARIO_FRESH;
    } else if (strcmp(preset, "LIGHTLY_USED") == 0) {
        *scenario = NAND_SIM_SCENARIO_LIGHTLY_USED;
    } else if (strcmp(preset, "AGED") == 0) {
        *scenario = NAND_SIM_SCENARIO_AGED;
    } else if (strcmp(preset, "FAILING") == 0) {
        *scenario = NAND_SIM_SCENARIO_FAILING;
    } else if (strcmp(preset, "POWER_LOSS") == 0) {
        *scenario = NAND_SIM_SCENARIO_POWER_LOSS;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

static esp_err_t build_fault_config(const scenario_config_t *scenario,
                                    nand_fault_sim_config_t *fault_cfg,
                                    uint32_t **allocated_bad_blocks)
{
    nand_sim_scenario_t preset;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(scenario != NULL, ESP_ERR_INVALID_ARG, TAG, "scenario must not be NULL");
    ESP_RETURN_ON_FALSE(fault_cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "fault_cfg must not be NULL");
    ESP_RETURN_ON_FALSE(allocated_bad_blocks != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "allocated_bad_blocks must not be NULL");

    memset(fault_cfg, 0, sizeof(*fault_cfg));
    *allocated_bad_blocks = NULL;

    if (scenario->preset != NULL) {
        ret = scenario_preset_from_name(scenario->preset, &preset);
        ESP_RETURN_ON_ERROR(ret, TAG, "unsupported preset: %s", scenario->preset);
        *fault_cfg = nand_fault_sim_config_preset(preset);
        return ESP_OK;
    }

    fault_cfg->factory_bad_block_count = scenario->factory_bad_block_count;
    fault_cfg->max_erase_cycles = scenario->max_erase_cycles;
    fault_cfg->max_prog_cycles = scenario->max_prog_cycles;
    fault_cfg->grave_page_threshold = scenario->grave_page_threshold;
    fault_cfg->read_fail_prob = (float)scenario->read_fail_prob;
    fault_cfg->prog_fail_prob = (float)scenario->prog_fail_prob;
    fault_cfg->erase_fail_prob = (float)scenario->erase_fail_prob;
    fault_cfg->copy_fail_prob = (float)scenario->copy_fail_prob;
    fault_cfg->op_fail_seed = scenario->op_fail_seed;
    fault_cfg->crash_after_ops_min = scenario->crash_after_ops_min;
    fault_cfg->crash_after_ops_max = scenario->crash_after_ops_max;
    fault_cfg->crash_probability = (float)scenario->crash_probability;
    fault_cfg->crash_seed = scenario->crash_seed;
    fault_cfg->ecc_mid_threshold = scenario->ecc_mid_threshold;
    fault_cfg->ecc_high_threshold = scenario->ecc_high_threshold;
    fault_cfg->ecc_fail_threshold = scenario->ecc_fail_threshold;

    if (scenario->factory_bad_block_count > 0) {
        *allocated_bad_blocks = calloc(scenario->factory_bad_block_count, sizeof(uint32_t));
        ESP_RETURN_ON_FALSE(*allocated_bad_blocks != NULL, ESP_ERR_NO_MEM, TAG,
                            "failed to allocate factory bad block list");

        for (uint32_t i = 0; i < scenario->factory_bad_block_count; i++) {
            (*allocated_bad_blocks)[i] = i;
        }

        fault_cfg->factory_bad_blocks = *allocated_bad_blocks;
    }

    return ESP_OK;
}

static esp_err_t validate_geometry(const sweep_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg must not be NULL");
    ESP_RETURN_ON_FALSE(cfg->nand.pages_per_block == 64, ESP_ERR_NOT_SUPPORTED, TAG,
                        "only 64 pages per block supported by current linux NAND backend");
    ESP_RETURN_ON_FALSE(cfg->nand.page_size == 2048, ESP_ERR_NOT_SUPPORTED, TAG,
                        "only 2048-byte pages supported by current linux NAND backend");
    ESP_RETURN_ON_FALSE(cfg->nand.num_blocks > 0, ESP_ERR_INVALID_ARG, TAG,
                        "nand.num_blocks must be > 0");
    return ESP_OK;
}

static size_t emulated_flash_size_bytes(const sweep_config_t *cfg)
{
    const size_t emulated_page_size = (size_t)cfg->nand.page_size + 64u;
    return (size_t)cfg->nand.num_blocks * (size_t)cfg->nand.pages_per_block * emulated_page_size;
}

static cJSON *build_ftl_config_json(const ftl_config_t *ftl_cfg)
{
    cJSON *obj = cJSON_CreateObject();

    if (obj == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(obj, "name", (ftl_cfg->name != NULL) ? ftl_cfg->name : "");
    cJSON_AddStringToObject(obj, "ftl", (ftl_cfg->ftl != NULL) ? ftl_cfg->ftl : "");
    cJSON_AddNumberToObject(obj, "gc_overhead_percent", ftl_cfg->gc_overhead_percent);
    cJSON_AddNumberToObject(obj, "gc_ratio", (double)ftl_cfg->gc_ratio);
    return obj;
}

static esp_err_t build_workload_config_json(const sweep_config_t *cfg,
                                            uint32_t logical_sector_count,
                                            cJSON **out)
{
    cJSON *obj;
    uint32_t sectors_per_op;
    uint32_t safe_sector_count;

    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg must not be NULL");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out must not be NULL");
    ESP_RETURN_ON_FALSE(cfg->workload.write_size_bytes > 0, ESP_ERR_INVALID_ARG, TAG,
                        "write_size_bytes must be > 0");
    ESP_RETURN_ON_FALSE(cfg->workload.write_size_bytes % cfg->nand.page_size == 0,
                        ESP_ERR_INVALID_ARG, TAG,
                        "write_size_bytes must be a multiple of page_size");

    sectors_per_op = cfg->workload.write_size_bytes / cfg->nand.page_size;
    ESP_RETURN_ON_FALSE(sectors_per_op > 0, ESP_ERR_INVALID_ARG, TAG,
                        "workload must span at least one sector");
    ESP_RETURN_ON_FALSE(logical_sector_count >= sectors_per_op, ESP_ERR_INVALID_SIZE, TAG,
                        "logical sector count too small for workload operation size");

    safe_sector_count = logical_sector_count - sectors_per_op + 1u;
    obj = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(obj != NULL, ESP_ERR_NO_MEM, TAG, "failed to create workload config");

    cJSON_AddStringToObject(obj, "type", (cfg->workload.type != NULL) ? cfg->workload.type : "");
    cJSON_AddNumberToObject(obj, "total_writes", (double)cfg->workload.total_writes);
    cJSON_AddNumberToObject(obj, "total_ops", (double)cfg->workload.total_writes);
    cJSON_AddNumberToObject(obj, "write_size_bytes", (double)cfg->workload.write_size_bytes);
    cJSON_AddNumberToObject(obj, "data_len", (double)cfg->workload.write_size_bytes);
    cJSON_AddNumberToObject(obj, "seed", (double)cfg->workload.seed);
    cJSON_AddNumberToObject(obj, "num_sectors", (double)safe_sector_count);

    *out = obj;
    return ESP_OK;
}

static esp_err_t execute_workload_op(const workload_op_t *op,
                                     uint32_t page_size,
                                     uint32_t logical_sector_count,
                                     const ftl_ops_t *ftl_ops,
                                     void *ftl_ctx,
                                     metrics_t *metrics)
{
    uint32_t sectors_per_op;

    ESP_RETURN_ON_FALSE(op != NULL, ESP_ERR_INVALID_ARG, TAG, "op must not be NULL");
    ESP_RETURN_ON_FALSE(ftl_ops != NULL, ESP_ERR_INVALID_ARG, TAG, "ftl_ops must not be NULL");
    ESP_RETURN_ON_FALSE(metrics != NULL, ESP_ERR_INVALID_ARG, TAG, "metrics must not be NULL");
    ESP_RETURN_ON_FALSE(page_size > 0, ESP_ERR_INVALID_ARG, TAG, "page_size must not be 0");
    ESP_RETURN_ON_FALSE(op->data_len % page_size == 0, ESP_ERR_INVALID_ARG, TAG,
                        "operation size must be a multiple of page_size");

    sectors_per_op = (uint32_t)(op->data_len / page_size);
    ESP_RETURN_ON_FALSE(op->sector + sectors_per_op <= logical_sector_count, ESP_ERR_INVALID_ARG, TAG,
                        "operation exceeds logical sector range");

    for (uint32_t i = 0; i < sectors_per_op; i++) {
        const uint8_t *write_ptr = op->data + ((size_t)i * page_size);
        uint8_t *read_ptr = op->data + ((size_t)i * page_size);
        esp_err_t io_ret;

        if (op->is_write) {
            io_ret = ftl_ops->write(ftl_ctx, op->sector + i, write_ptr);
            metrics_record_write(metrics, io_ret == ESP_OK);
        } else {
            io_ret = ftl_ops->read(ftl_ctx, op->sector + i, read_ptr);
            metrics_record_read(metrics, io_ret == ESP_OK);
        }

        if (io_ret != ESP_OK) {
            metrics_record_ftl_error(metrics);
        }
    }

    return ESP_OK;
}

esp_err_t run_single(const sweep_config_t *cfg,
                     const scenario_config_t *scenario,
                     const ftl_config_t *ftl_cfg,
                     run_result_t *result)
{
    esp_err_t ret = ESP_OK;
    esp_err_t cleanup_ret;
    spi_nand_flash_device_t *nand = NULL;
    ftl_ops_t *ftl_ops = NULL;
    workload_ops_t *workload_ops = NULL;
    cJSON *ftl_config_json = NULL;
    cJSON *workload_config_json = NULL;
    nand_fault_sim_config_t fault_cfg;
    uint32_t *allocated_bad_blocks = NULL;
    nand_file_mmap_emul_config_t emul_cfg = { 0 };
    spi_nand_flash_config_t nand_cfg;
    void *ftl_ctx = NULL;
    void *workload_ctx = NULL;
    bool fault_sim_inited = false;
    bool nand_inited = false;
    bool ftl_inited = false;
    bool workload_inited = false;
    uint32_t logical_sector_count = 0;

    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg must not be NULL");
    ESP_RETURN_ON_FALSE(scenario != NULL, ESP_ERR_INVALID_ARG, TAG, "scenario must not be NULL");
    ESP_RETURN_ON_FALSE(ftl_cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "ftl_cfg must not be NULL");
    ESP_RETURN_ON_FALSE(result != NULL, ESP_ERR_INVALID_ARG, TAG, "result must not be NULL");

    memset(result, 0, sizeof(*result));
    result->scenario_name = scenario->name;
    result->ftl_config_name = ftl_cfg->name;
    result->status = "failed";
    metrics_reset(&result->metrics);

    ESP_GOTO_ON_ERROR(validate_geometry(cfg), fail, TAG, "unsupported NAND geometry");
    ESP_GOTO_ON_ERROR(build_fault_config(scenario, &fault_cfg, &allocated_bad_blocks), fail, TAG,
                      "failed to build fault config");

    ESP_GOTO_ON_ERROR(nand_fault_sim_init(cfg->nand.num_blocks, cfg->nand.pages_per_block, &fault_cfg), fail,
                      TAG, "nand_fault_sim_init failed");
    fault_sim_inited = true;

    emul_cfg.flash_file_size = emulated_flash_size_bytes(cfg);
    emul_cfg.keep_dump = false;
    emul_cfg.flash_file_name[0] = '\0';

    nand_cfg = (spi_nand_flash_config_t) {
        .emul_conf = &emul_cfg,
        .gc_factor = ftl_cfg->gc_ratio,
        .io_mode = SPI_NAND_IO_MODE_SIO,
        .flags = 0,
    };

    ESP_GOTO_ON_ERROR(spi_nand_flash_init_device(&nand_cfg, &nand), fail, TAG,
                      "spi_nand_flash_init_device failed");
    nand_inited = true;

    ESP_GOTO_ON_ERROR(metrics_collect_bad_blocks(&result->metrics, nand, true), fail, TAG,
                      "failed to collect initial bad blocks");

    ftl_ops = ftl_create(ftl_cfg->ftl);
    ESP_GOTO_ON_FALSE(ftl_ops != NULL, ESP_ERR_NOT_FOUND, fail, TAG, "unknown FTL: %s", ftl_cfg->ftl);

    ftl_config_json = build_ftl_config_json(ftl_cfg);
    ESP_GOTO_ON_FALSE(ftl_config_json != NULL, ESP_ERR_NO_MEM, fail, TAG,
                      "failed to create FTL config JSON");

    ESP_GOTO_ON_ERROR(ftl_ops->init(&ftl_ctx, nand, ftl_config_json), fail, TAG, "ftl init failed");
    ftl_inited = true;

    ESP_GOTO_ON_ERROR(spi_nand_flash_get_capacity(nand, &logical_sector_count), fail, TAG,
                      "failed to get logical sector count");

    ESP_GOTO_ON_ERROR(build_workload_config_json(cfg, logical_sector_count, &workload_config_json), fail, TAG,
                      "failed to create workload config JSON");

    workload_ops = workload_create(cfg->workload.type);
    ESP_GOTO_ON_FALSE(workload_ops != NULL, ESP_ERR_NOT_FOUND, fail, TAG,
                      "unknown workload type: %s", cfg->workload.type);
    ESP_GOTO_ON_ERROR(workload_ops->init(&workload_ctx, workload_config_json), fail, TAG,
                      "workload init failed");
    workload_inited = true;

    while (!workload_ops->is_done(&workload_ctx)) {
        workload_op_t op = { 0 };

        ESP_GOTO_ON_ERROR(workload_ops->next_op(&workload_ctx, &op), fail, TAG,
                          "failed to get workload op");
        ESP_GOTO_ON_ERROR(execute_workload_op(&op, cfg->nand.page_size, logical_sector_count,
                                              ftl_ops, &ftl_ctx, &result->metrics),
                          fail, TAG, "failed to execute workload op");
    }

    ESP_GOTO_ON_ERROR(ftl_ops->sync(&ftl_ctx), fail, TAG, "ftl sync failed");

    result->status = "completed";

fail:
    if (workload_inited) {
        cleanup_ret = workload_ops->deinit(&workload_ctx);
        if (ret == ESP_OK && cleanup_ret != ESP_OK) {
            ret = cleanup_ret;
        }
    }

    if (ftl_inited) {
        cleanup_ret = ftl_ops->deinit(&ftl_ctx);
        if (ret == ESP_OK && cleanup_ret != ESP_OK) {
            ret = cleanup_ret;
        }
    }

    if (nand_inited) {
        cleanup_ret = metrics_collect_bad_blocks(&result->metrics, nand, false);
        if (ret == ESP_OK && cleanup_ret != ESP_OK) {
            ret = cleanup_ret;
        }

        cleanup_ret = metrics_collect_prog_stats(&result->metrics,
                                                 cfg->nand.num_blocks * cfg->nand.pages_per_block);
        if (ret == ESP_OK && cleanup_ret != ESP_OK) {
            ret = cleanup_ret;
        }

        cleanup_ret = metrics_collect_erase_stats(&result->metrics, cfg->nand.num_blocks);
        if (ret == ESP_OK && cleanup_ret != ESP_OK) {
            ret = cleanup_ret;
        }

        cleanup_ret = spi_nand_flash_deinit_device(nand);
        if (ret == ESP_OK && cleanup_ret != ESP_OK) {
            ret = cleanup_ret;
        }
    }

    if (fault_sim_inited) {
        nand_fault_sim_deinit();
    }

    cJSON_Delete(ftl_config_json);
    cJSON_Delete(workload_config_json);
    free(allocated_bad_blocks);

    if (ret != ESP_OK) {
        result->status = "failed";
    }

    return ret;
}

esp_err_t run_suite(const sweep_config_t *cfg, const reporter_t *reporter, void *reporter_ctx)
{
    esp_err_t first_error = ESP_OK;

    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg must not be NULL");
    ESP_RETURN_ON_FALSE(reporter != NULL, ESP_ERR_INVALID_ARG, TAG, "reporter must not be NULL");
    ESP_RETURN_ON_FALSE(reporter->write_result != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "reporter->write_result must not be NULL");

    for (uint32_t scenario_index = 0; scenario_index < cfg->scenario_count; scenario_index++) {
        for (uint32_t ftl_index = 0; ftl_index < cfg->ftl_config_count; ftl_index++) {
            run_result_t result;
            esp_err_t run_ret = run_single(cfg, &cfg->scenarios[scenario_index],
                                           &cfg->ftl_configs[ftl_index], &result);
            esp_err_t report_ret = reporter->write_result(reporter_ctx, &result);

            if (first_error == ESP_OK && run_ret != ESP_OK) {
                first_error = run_ret;
            }
            if (first_error == ESP_OK && report_ret != ESP_OK) {
                first_error = report_ret;
            }
        }
    }

    return first_error;
}
