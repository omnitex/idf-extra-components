/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
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
