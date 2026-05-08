/*
 * SPDX-FileCopyrightText: 2026 Martin Havlik <omnitex.git@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "runner/runner.h"
#include "reporter/reporter.h"

static void verify_api_contract(const sweep_config_t *cfg,
                                const scenario_config_t *scenario,
                                const ftl_config_t *ftl_cfg,
                                const run_result_t *result,
                                const reporter_t *reporter,
                                reporter_json_ctx_t *ctx)
{
    esp_err_t (*suite_fn)(const sweep_config_t *, const reporter_t *, void *) = run_suite;
    esp_err_t (*single_fn)(const sweep_config_t *, const scenario_config_t *, const ftl_config_t *,
                           run_result_t *) = run_single;
    void (*json_init_fn)(reporter_json_ctx_t *, const sweep_config_t *) = reporter_json_init_ctx;
    reporter_t *(*reporter_factory)(void) = reporter_json_get;

    (void)cfg;
    (void)scenario;
    (void)ftl_cfg;
    (void)ctx;
    (void)suite_fn;
    (void)single_fn;
    (void)json_init_fn;
    (void)reporter_factory;
    (void)reporter;
    (void)result->scenario_name;
    (void)result->ftl_config_name;
    (void)result->status;
    (void)result->metrics;
}
