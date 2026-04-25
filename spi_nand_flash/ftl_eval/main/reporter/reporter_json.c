/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "reporter/reporter.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"

static const char *TAG = "reporter_json";

static char *dup_string_or_null(const char *src)
{
    size_t len;
    char *copy;

    if (src == NULL) {
        return NULL;
    }

    len = strlen(src) + 1;
    copy = malloc(len);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, src, len);
    return copy;
}

static void reporter_json_reset_doc(reporter_json_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->root != NULL) {
        cJSON_Delete((cJSON *)ctx->root);
        ctx->root = NULL;
        ctx->results = NULL;
    }
}

void reporter_json_init_ctx(reporter_json_ctx_t *ctx, const sweep_config_t *cfg)
{
    if (ctx == NULL) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    if (cfg == NULL) {
        return;
    }

    ctx->sweep_name = cfg->name;
    ctx->nand = cfg->nand;
}

static esp_err_t reporter_json_open(void *ctx, const char *output_path)
{
    reporter_json_ctx_t *json_ctx = (reporter_json_ctx_t *)ctx;
    cJSON *root;
    cJSON *nand;
    cJSON *results;

    ESP_RETURN_ON_FALSE(json_ctx != NULL, ESP_ERR_INVALID_ARG, TAG, "ctx must not be NULL");
    ESP_RETURN_ON_FALSE(output_path != NULL, ESP_ERR_INVALID_ARG, TAG, "output_path must not be NULL");

    reporter_json_reset_doc(json_ctx);
    free(json_ctx->output_path);
    json_ctx->output_path = dup_string_or_null(output_path);
    ESP_RETURN_ON_FALSE(json_ctx->output_path != NULL, ESP_ERR_NO_MEM, TAG, "failed to copy output path");

    root = cJSON_CreateObject();
    results = cJSON_CreateArray();
    nand = cJSON_CreateObject();
    if (root == NULL || results == NULL || nand == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(results);
        cJSON_Delete(nand);
        free(json_ctx->output_path);
        json_ctx->output_path = NULL;
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "sweep", (json_ctx->sweep_name != NULL) ? json_ctx->sweep_name : "");
    cJSON_AddItemToObject(root, "nand", nand);
    cJSON_AddNumberToObject(nand, "num_blocks", (double)json_ctx->nand.num_blocks);
    cJSON_AddNumberToObject(nand, "pages_per_block", (double)json_ctx->nand.pages_per_block);
    cJSON_AddNumberToObject(nand, "page_size", (double)json_ctx->nand.page_size);
    cJSON_AddItemToObject(root, "results", results);

    json_ctx->root = root;
    json_ctx->results = results;
    return ESP_OK;
}

static cJSON *reporter_json_build_metrics(const metrics_t *metrics)
{
    cJSON *obj = cJSON_CreateObject();

    if (obj == NULL || metrics == NULL) {
        cJSON_Delete(obj);
        return NULL;
    }

    cJSON_AddNumberToObject(obj, "reads_attempted", (double)metrics->reads_attempted);
    cJSON_AddNumberToObject(obj, "reads_succeeded", (double)metrics->reads_succeeded);
    cJSON_AddNumberToObject(obj, "writes_attempted", (double)metrics->writes_attempted);
    cJSON_AddNumberToObject(obj, "writes_succeeded", (double)metrics->writes_succeeded);
    cJSON_AddNumberToObject(obj, "write_amplification_factor", metrics->write_amplification_factor);
    cJSON_AddNumberToObject(obj, "total_erases", (double)metrics->total_erases);
    cJSON_AddNumberToObject(obj, "total_prog_ops", (double)metrics->total_prog_ops);
    cJSON_AddNumberToObject(obj, "bad_blocks_final", (double)metrics->bad_blocks_final);
    cJSON_AddNumberToObject(obj, "bad_blocks_initial", (double)metrics->bad_blocks_initial);
    cJSON_AddNumberToObject(obj, "mean_erase_count", metrics->mean_erase_count);
    cJSON_AddNumberToObject(obj, "max_erase_count", (double)metrics->max_erase_count);
    cJSON_AddNumberToObject(obj, "min_erase_count", (double)metrics->min_erase_count);
    cJSON_AddNumberToObject(obj, "erase_count_stddev", metrics->erase_count_stddev);
    cJSON_AddNumberToObject(obj, "erase_count_p50", (double)metrics->erase_count_p50);
    cJSON_AddNumberToObject(obj, "erase_count_p90", (double)metrics->erase_count_p90);
    cJSON_AddNumberToObject(obj, "erase_count_p99", (double)metrics->erase_count_p99);
    cJSON_AddNumberToObject(obj, "erase_count_gini", metrics->erase_count_gini);
    cJSON_AddNumberToObject(obj, "blocks_worn_out", (double)metrics->blocks_worn_out);
    cJSON_AddNumberToObject(obj, "first_worn_out_at_write", (double)metrics->first_worn_out_at_write);
    cJSON_AddNumberToObject(obj, "ftl_errors", (double)metrics->ftl_errors);
    return obj;
}

static esp_err_t reporter_json_write_result(void *ctx, const run_result_t *result)
{
    reporter_json_ctx_t *json_ctx = (reporter_json_ctx_t *)ctx;
    cJSON *item;
    cJSON *metrics;

    ESP_RETURN_ON_FALSE(json_ctx != NULL, ESP_ERR_INVALID_ARG, TAG, "ctx must not be NULL");
    ESP_RETURN_ON_FALSE(result != NULL, ESP_ERR_INVALID_ARG, TAG, "result must not be NULL");
    ESP_RETURN_ON_FALSE(json_ctx->results != NULL, ESP_ERR_INVALID_STATE, TAG, "reporter not opened");

    item = cJSON_CreateObject();
    metrics = reporter_json_build_metrics(&result->metrics);
    if (item == NULL || metrics == NULL) {
        cJSON_Delete(item);
        cJSON_Delete(metrics);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(item, "scenario", (result->scenario_name != NULL) ? result->scenario_name : "");
    cJSON_AddStringToObject(item, "ftl_config", (result->ftl_config_name != NULL) ? result->ftl_config_name : "");
    cJSON_AddStringToObject(item, "status", (result->status != NULL) ? result->status : "failed");
    cJSON_AddItemToObject(item, "metrics", metrics);
    cJSON_AddItemToArray((cJSON *)json_ctx->results, item);
    return ESP_OK;
}

static bool reporter_json_set_timestamp(cJSON *root)
{
    time_t now;
    struct tm tm_utc;
    char timestamp[32];

    now = time(NULL);
    if (gmtime_r(&now, &tm_utc) == NULL) {
        return false;
    }

    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        return false;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(root, "timestamp");
    cJSON_AddStringToObject(root, "timestamp", timestamp);
    return true;
}

static esp_err_t reporter_json_close(void *ctx)
{
    reporter_json_ctx_t *json_ctx = (reporter_json_ctx_t *)ctx;
    char *json_str;
    FILE *out;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(json_ctx != NULL, ESP_ERR_INVALID_ARG, TAG, "ctx must not be NULL");
    ESP_RETURN_ON_FALSE(json_ctx->root != NULL, ESP_ERR_INVALID_STATE, TAG, "reporter not opened");
    ESP_RETURN_ON_FALSE(json_ctx->output_path != NULL, ESP_ERR_INVALID_STATE, TAG, "output path missing");

    if (!reporter_json_set_timestamp((cJSON *)json_ctx->root)) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    json_str = cJSON_Print((cJSON *)json_ctx->root);
    if (json_str == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    out = fopen(json_ctx->output_path, "w");
    if (out == NULL) {
        free(json_str);
        ret = ESP_FAIL;
        goto cleanup;
    }

    if (fputs(json_str, out) == EOF || fputc('\n', out) == EOF || fclose(out) != 0) {
        free(json_str);
        ret = ESP_FAIL;
        goto cleanup;
    }

    free(json_str);

cleanup:
    reporter_json_reset_doc(json_ctx);
    free(json_ctx->output_path);
    json_ctx->output_path = NULL;
    return ret;
}

reporter_t *reporter_json_get(void)
{
    static reporter_t s_reporter = {
        .open = reporter_json_open,
        .write_result = reporter_json_write_result,
        .close = reporter_json_close,
    };

    return &s_reporter;
}
