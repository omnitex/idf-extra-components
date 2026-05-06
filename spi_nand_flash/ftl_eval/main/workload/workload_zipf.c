/*
 * SPDX-FileCopyrightText: 2026 Martin Havlik <omnitex.git@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "workload.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"

typedef struct {
    uint32_t total_ops;
    uint32_t current_op;
    uint32_t num_sectors;
    unsigned int seed;
    size_t data_len;
    uint8_t *buffer;
    uint32_t *cdf;
} zipf_workload_ctx_t;

static uint32_t get_optional_u32(const cJSON *obj, const char *name, uint32_t fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);

    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return fallback;
    }

    return (uint32_t)item->valuedouble;
}

static double get_optional_double(const cJSON *obj, const char *name, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);

    if (!cJSON_IsNumber(item)) {
        return fallback;
    }

    return item->valuedouble;
}

static void fill_data_pattern(uint8_t *buffer, size_t len, uint32_t sector, uint32_t op_index)
{
    for (size_t i = 0; i < len; i++) {
        buffer[i] = (uint8_t)((sector ^ op_index ^ (uint32_t)i) & 0xFFu);
    }
}

static esp_err_t build_zipf_cdf(uint32_t num_sectors, double skew, uint32_t *cdf)
{
    double sum = 0.0;

    for (uint32_t i = 0; i < num_sectors; i++) {
        sum += 1.0 / pow((double)(i + 1), skew);
    }

    if (sum == 0.0) {
        return ESP_ERR_INVALID_ARG;
    }

    double running = 0.0;
    for (uint32_t i = 0; i < num_sectors; i++) {
        running += 1.0 / pow((double)(i + 1), skew);
        cdf[i] = (uint32_t)((running / sum) * (double)RAND_MAX);
    }
    cdf[num_sectors - 1] = (uint32_t)RAND_MAX;

    return ESP_OK;
}

static uint32_t sample_zipf(const uint32_t *cdf, uint32_t num_sectors, unsigned int *seed)
{
    unsigned int r = rand_r(seed);

    uint32_t lo = 0;
    uint32_t hi = num_sectors - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cdf[mid] < r) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static zipf_workload_ctx_t **zipf_ctx_slot(void *ctx)
{
    return (zipf_workload_ctx_t **)ctx;
}

static zipf_workload_ctx_t *zipf_ctx_from_slot(void *ctx)
{
    zipf_workload_ctx_t **slot = zipf_ctx_slot(ctx);
    return (slot == NULL) ? NULL : *slot;
}

static esp_err_t zipf_init(void *ctx, const cJSON *config)
{
    zipf_workload_ctx_t **slot = zipf_ctx_slot(ctx);
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(slot != NULL, ESP_ERR_INVALID_ARG, "workload_zipf", "ctx must not be NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, "workload_zipf", "config must not be NULL");
    ESP_RETURN_ON_FALSE(*slot == NULL, ESP_ERR_INVALID_STATE, "workload_zipf", "ctx already initialized");

    zipf_workload_ctx_t *state = calloc(1, sizeof(zipf_workload_ctx_t));
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_NO_MEM, "workload_zipf", "failed to allocate ctx");

    state->total_ops = get_optional_u32(config, "total_ops", 0);
    if (state->total_ops == 0) {
        state->total_ops = get_optional_u32(config, "total_writes", 0);
    }
    state->data_len = get_optional_u32(config, "data_len", 0);
    if (state->data_len == 0) {
        state->data_len = get_optional_u32(config, "write_size_bytes", 0);
    }
    state->num_sectors = get_optional_u32(config, "num_sectors", state->total_ops);
    state->seed        = get_optional_u32(config, "seed", 42);

    double skew = get_optional_double(config, "zipf_skew", 1.0);

    ESP_GOTO_ON_FALSE(state->total_ops > 0, ESP_ERR_INVALID_ARG, fail, "workload_zipf",
                      "total_ops/total_writes must be > 0");
    ESP_GOTO_ON_FALSE(state->data_len > 0, ESP_ERR_INVALID_ARG, fail, "workload_zipf",
                      "data_len/write_size_bytes must be > 0");
    ESP_GOTO_ON_FALSE(state->num_sectors > 0, ESP_ERR_INVALID_ARG, fail, "workload_zipf",
                      "num_sectors must be > 0");
    ESP_GOTO_ON_FALSE(skew >= 0.0, ESP_ERR_INVALID_ARG, fail, "workload_zipf",
                      "zipf_skew must be >= 0.0");

    state->buffer = malloc(state->data_len);
    ESP_GOTO_ON_FALSE(state->buffer != NULL, ESP_ERR_NO_MEM, fail, "workload_zipf",
                      "failed to allocate data buffer");

    state->cdf = malloc(state->num_sectors * sizeof(uint32_t));
    ESP_GOTO_ON_FALSE(state->cdf != NULL, ESP_ERR_NO_MEM, fail, "workload_zipf",
                      "failed to allocate CDF table");

    ESP_GOTO_ON_ERROR(build_zipf_cdf(state->num_sectors, skew, state->cdf), fail,
                      "workload_zipf", "failed to build Zipf CDF");

    *slot = state;
    return ESP_OK;

fail:
    free(state->cdf);
    free(state->buffer);
    free(state);
    return ret;
}

static esp_err_t zipf_next_op(void *ctx, workload_op_t *op)
{
    zipf_workload_ctx_t *state = zipf_ctx_from_slot(ctx);

    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_STATE, "workload_zipf", "ctx is not initialized");
    ESP_RETURN_ON_FALSE(op != NULL, ESP_ERR_INVALID_ARG, "workload_zipf", "op must not be NULL");
    ESP_RETURN_ON_FALSE(state->current_op < state->total_ops, ESP_ERR_NOT_FINISHED, "workload_zipf",
                        "workload already completed");

    uint32_t sector = sample_zipf(state->cdf, state->num_sectors, &state->seed);

    fill_data_pattern(state->buffer, state->data_len, sector, state->current_op);

    op->sector   = sector;
    op->is_write = true;
    op->data     = state->buffer;
    op->data_len = state->data_len;

    state->current_op++;
    return ESP_OK;
}

static bool zipf_is_done(void *ctx)
{
    zipf_workload_ctx_t *state = zipf_ctx_from_slot(ctx);
    return (state == NULL) ? true : (state->current_op >= state->total_ops);
}

static esp_err_t zipf_deinit(void *ctx)
{
    zipf_workload_ctx_t **slot = zipf_ctx_slot(ctx);

    ESP_RETURN_ON_FALSE(slot != NULL, ESP_ERR_INVALID_ARG, "workload_zipf", "ctx must not be NULL");
    if (*slot == NULL) {
        return ESP_OK;
    }

    free((*slot)->cdf);
    free((*slot)->buffer);
    free(*slot);
    *slot = NULL;
    return ESP_OK;
}

workload_ops_t *workload_zipf_get_ops(void)
{
    static workload_ops_t s_ops = {
        .init = zipf_init,
        .next_op = zipf_next_op,
        .is_done = zipf_is_done,
        .deinit = zipf_deinit,
    };

    return &s_ops;
}
