/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "app_config.h"
#include "esp_err.h"
#include "runner/runner.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct reporter_t {
    esp_err_t (*open)(void *ctx, const char *output_path);
    esp_err_t (*write_result)(void *ctx, const run_result_t *result);
    esp_err_t (*close)(void *ctx);
} reporter_t;

typedef struct {
    const char *sweep_name;
    nand_geometry_t nand;
    char *output_path;
    void *root;
    void *results;
} reporter_json_ctx_t;

void reporter_json_init_ctx(reporter_json_ctx_t *ctx, const sweep_config_t *cfg);
reporter_t *reporter_json_get(void);

#ifdef __cplusplus
}
#endif
