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
    uint32_t num_sectors;
    uint32_t write_ratio_percent;
    unsigned int seed;
    size_t data_len;
    uint8_t *buffer;
    uint8_t *written_bitmap;
    uint32_t written_count;
} mixed_workload_ctx_t;

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
        buffer[i] = (uint8_t)(((sector * 31u) + op_index + i) & 0xFFu);
    }
}

static mixed_workload_ctx_t **mixed_ctx_slot(void *ctx)
{
    return (mixed_workload_ctx_t **)ctx;
}

static mixed_workload_ctx_t *mixed_ctx_from_slot(void *ctx)
{
    mixed_workload_ctx_t **slot = mixed_ctx_slot(ctx);
    return (slot == NULL) ? NULL : *slot;
}

static uint32_t pick_written_sector(mixed_workload_ctx_t *state)
{
    uint32_t start = (uint32_t)(rand_r(&state->seed) % state->num_sectors);

    for (uint32_t i = 0; i < state->num_sectors; i++) {
        uint32_t sector = (start + i) % state->num_sectors;
        if (state->written_bitmap[sector] != 0) {
            return sector;
        }
    }

    return 0;
}

static esp_err_t mixed_init(void *ctx, const cJSON *config)
{
    mixed_workload_ctx_t **slot = mixed_ctx_slot(ctx);
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(slot != NULL, ESP_ERR_INVALID_ARG, "workload_mix", "ctx must not be NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, "workload_mix", "config must not be NULL");
    ESP_RETURN_ON_FALSE(*slot == NULL, ESP_ERR_INVALID_STATE, "workload_mix", "ctx already initialized");

    mixed_workload_ctx_t *state = calloc(1, sizeof(mixed_workload_ctx_t));
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_NO_MEM, "workload_mix", "failed to allocate ctx");

    state->total_ops = get_optional_u32(config, "total_ops", 0);
    if (state->total_ops == 0) {
        state->total_ops = get_optional_u32(config, "total_writes", 0);
    }
    state->data_len = get_optional_u32(config, "data_len", 0);
    if (state->data_len == 0) {
        state->data_len = get_optional_u32(config, "write_size_bytes", 0);
    }
    state->num_sectors = get_optional_u32(config, "num_sectors", state->total_ops);
    state->seed = get_optional_u32(config, "seed", 42);
    state->write_ratio_percent = get_optional_u32(config, "write_ratio_percent", 70);

    ESP_GOTO_ON_FALSE(state->total_ops > 0, ESP_ERR_INVALID_ARG, fail, "workload_mix",
                      "total_ops/total_writes must be > 0");
    ESP_GOTO_ON_FALSE(state->data_len > 0, ESP_ERR_INVALID_ARG, fail, "workload_mix",
                      "data_len/write_size_bytes must be > 0");
    ESP_GOTO_ON_FALSE(state->num_sectors > 0, ESP_ERR_INVALID_ARG, fail, "workload_mix",
                      "num_sectors must be > 0");
    ESP_GOTO_ON_FALSE(state->write_ratio_percent <= 100, ESP_ERR_INVALID_ARG, fail, "workload_mix",
                      "write_ratio_percent must be <= 100");

    state->buffer = malloc(state->data_len);
    ESP_GOTO_ON_FALSE(state->buffer != NULL, ESP_ERR_NO_MEM, fail, "workload_mix",
                      "failed to allocate data buffer");

    state->written_bitmap = calloc(state->num_sectors, sizeof(uint8_t));
    ESP_GOTO_ON_FALSE(state->written_bitmap != NULL, ESP_ERR_NO_MEM, fail, "workload_mix",
                      "failed to allocate written bitmap");

    *slot = state;
    return ESP_OK;

fail:
    free(state->written_bitmap);
    free(state->buffer);
    free(state);
    return ret;
}

static esp_err_t mixed_next_op(void *ctx, workload_op_t *op)
{
    mixed_workload_ctx_t *state = mixed_ctx_from_slot(ctx);
    bool is_write;
    uint32_t sector;

    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_STATE, "workload_mix", "ctx is not initialized");
    ESP_RETURN_ON_FALSE(op != NULL, ESP_ERR_INVALID_ARG, "workload_mix", "op must not be NULL");
    ESP_RETURN_ON_FALSE(state->current_op < state->total_ops, ESP_ERR_NOT_FINISHED, "workload_mix",
                        "workload already completed");

    is_write = (state->written_count == 0) || ((state->current_op % 100u) < state->write_ratio_percent);
    if (is_write) {
        sector = (uint32_t)(rand_r(&state->seed) % state->num_sectors);
        if (state->written_bitmap[sector] == 0) {
            state->written_bitmap[sector] = 1;
            state->written_count++;
        }
        fill_data_pattern(state->buffer, state->data_len, sector, state->current_op);
    } else {
        sector = pick_written_sector(state);
        memset(state->buffer, 0, state->data_len);
    }

    op->sector = sector;
    op->is_write = is_write;
    op->data = state->buffer;
    op->data_len = state->data_len;

    state->current_op++;
    return ESP_OK;
}

static bool mixed_is_done(void *ctx)
{
    mixed_workload_ctx_t *state = mixed_ctx_from_slot(ctx);
    return (state == NULL) ? true : (state->current_op >= state->total_ops);
}

static esp_err_t mixed_deinit(void *ctx)
{
    mixed_workload_ctx_t **slot = mixed_ctx_slot(ctx);

    ESP_RETURN_ON_FALSE(slot != NULL, ESP_ERR_INVALID_ARG, "workload_mix", "ctx must not be NULL");
    if (*slot == NULL) {
        return ESP_OK;
    }

    free((*slot)->written_bitmap);
    free((*slot)->buffer);
    free(*slot);
    *slot = NULL;
    return ESP_OK;
}

workload_ops_t *workload_mixed_get_ops(void)
{
    static workload_ops_t s_ops = {
        .init = mixed_init,
        .next_op = mixed_next_op,
        .is_done = mixed_is_done,
        .deinit = mixed_deinit,
    };

    return &s_ops;
}
