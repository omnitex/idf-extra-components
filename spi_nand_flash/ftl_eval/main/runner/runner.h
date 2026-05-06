/*
 * SPDX-FileCopyrightText: 2026 Martin Havlik <omnitex.git@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "app_config.h"
#include "esp_err.h"
#include "metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct reporter_t reporter_t;

typedef struct {
    const char *scenario_name;
    const char *ftl_config_name;
    const char *status;
    const char *recovery_status;
    uint32_t    data_loss_pages;
    uint32_t    sectors_written;
    metrics_t metrics;
} run_result_t;

esp_err_t run_single(const sweep_config_t *cfg,
                     const scenario_config_t *scenario,
                     const ftl_config_t *ftl_cfg,
                     run_result_t *result);

esp_err_t run_suite(const sweep_config_t *cfg, const reporter_t *reporter, void *reporter_ctx);

#ifdef __cplusplus
}
#endif
