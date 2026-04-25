/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "workload.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"

typedef struct {
    uint32_t total_ops;
    uint32_t current_op;
    uint32_t next_sector;
    uint32_t num_sectors;
    size_t data_len;
    uint8_t *buffer;
} sequential_workload_ctx_t;

static uint32_t get_optional_u32(const cJSON *obj, const char *name, uint32_t fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);

    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return fallback;
    }

    return (uint32_t)item->valuedouble;
}

static void fill_data_pattern(uint8_t *buffer, size_t len, uint32_t sector, uint32_t op_index)
{
    for (size_t i = 0; i < len; i++) {
        buffer[i] = (uint8_t)((sector + op_index + i) & 0xFFu);
    }
}

static sequential_workload_ctx_t **sequential_ctx_slot(void *ctx)
{
    return (sequential_workload_ctx_t **)ctx;
}

static sequential_workload_ctx_t *sequential_ctx_from_slot(void *ctx)
{
    sequential_workload_ctx_t **slot = sequential_ctx_slot(ctx);
    return (slot == NULL) ? NULL : *slot;
}

static esp_err_t sequential_init(void *ctx, const cJSON *config)
{
    sequential_workload_ctx_t **slot = sequential_ctx_slot(ctx);
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(slot != NULL, ESP_ERR_INVALID_ARG, "workload_seq", "ctx must not be NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, "workload_seq", "config must not be NULL");
    ESP_RETURN_ON_FALSE(*slot == NULL, ESP_ERR_INVALID_STATE, "workload_seq", "ctx already initialized");

    sequential_workload_ctx_t *state = calloc(1, sizeof(sequential_workload_ctx_t));
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_NO_MEM, "workload_seq", "failed to allocate ctx");

    state->total_ops = get_optional_u32(config, "total_ops", 0);
    if (state->total_ops == 0) {
        state->total_ops = get_optional_u32(config, "total_writes", 0);
    }
    state->data_len = get_optional_u32(config, "data_len", 0);
    if (state->data_len == 0) {
        state->data_len = get_optional_u32(config, "write_size_bytes", 0);
    }
    state->num_sectors = get_optional_u32(config, "num_sectors", state->total_ops);

    ESP_GOTO_ON_FALSE(state->total_ops > 0, ESP_ERR_INVALID_ARG, fail, "workload_seq",
                      "total_ops/total_writes must be > 0");
    ESP_GOTO_ON_FALSE(state->data_len > 0, ESP_ERR_INVALID_ARG, fail, "workload_seq",
                      "data_len/write_size_bytes must be > 0");
    ESP_GOTO_ON_FALSE(state->num_sectors > 0, ESP_ERR_INVALID_ARG, fail, "workload_seq",
                      "num_sectors must be > 0");

    state->buffer = malloc(state->data_len);
    ESP_GOTO_ON_FALSE(state->buffer != NULL, ESP_ERR_NO_MEM, fail, "workload_seq",
                      "failed to allocate data buffer");

    *slot = state;
    return ESP_OK;

fail:
    free(state->buffer);
    free(state);
    return ret;
}

static esp_err_t sequential_next_op(void *ctx, workload_op_t *op)
{
    sequential_workload_ctx_t *state = sequential_ctx_from_slot(ctx);

    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_STATE, "workload_seq", "ctx is not initialized");
    ESP_RETURN_ON_FALSE(op != NULL, ESP_ERR_INVALID_ARG, "workload_seq", "op must not be NULL");
    ESP_RETURN_ON_FALSE(state->current_op < state->total_ops, ESP_ERR_NOT_FINISHED, "workload_seq",
                        "workload already completed");

    fill_data_pattern(state->buffer, state->data_len, state->next_sector, state->current_op);

    op->sector = state->next_sector;
    op->is_write = true;
    op->data = state->buffer;
    op->data_len = state->data_len;

    state->current_op++;
    state->next_sector = (state->next_sector + 1) % state->num_sectors;
    return ESP_OK;
}

static bool sequential_is_done(void *ctx)
{
    sequential_workload_ctx_t *state = sequential_ctx_from_slot(ctx);
    return (state == NULL) ? true : (state->current_op >= state->total_ops);
}

static esp_err_t sequential_deinit(void *ctx)
{
    sequential_workload_ctx_t **slot = sequential_ctx_slot(ctx);

    ESP_RETURN_ON_FALSE(slot != NULL, ESP_ERR_INVALID_ARG, "workload_seq", "ctx must not be NULL");
    if (*slot == NULL) {
        return ESP_OK;
    }

    free((*slot)->buffer);
    free(*slot);
    *slot = NULL;
    return ESP_OK;
}

workload_ops_t *workload_sequential_get_ops(void)
{
    static workload_ops_t s_ops = {
        .init = sequential_init,
        .next_op = sequential_next_op,
        .is_done = sequential_is_done,
        .deinit = sequential_deinit,
    };

    return &s_ops;
}
