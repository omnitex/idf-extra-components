/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ftl_ops.h"

#include <stdbool.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_check.h"

typedef struct {
    spi_nand_flash_device_t *nand;
    uint32_t sector_count;
    bool initialized;
} dhara_ftl_ctx_t;

static dhara_ftl_ctx_t **dhara_ftl_ctx_slot(void *ctx)
{
    return (dhara_ftl_ctx_t **)ctx;
}

static dhara_ftl_ctx_t *dhara_ftl_ctx_from_slot(void *ctx)
{
    dhara_ftl_ctx_t **slot = dhara_ftl_ctx_slot(ctx);

    if (slot == NULL) {
        return NULL;
    }

    return *slot;
}

static uint8_t dhara_ftl_gc_factor_from_config(const cJSON *ftl_config)
{
    cJSON *gc_factor;

    if (ftl_config == NULL) {
        return 45;
    }

    gc_factor = cJSON_GetObjectItemCaseSensitive((cJSON *)ftl_config, "gc_factor");
    if (!cJSON_IsNumber(gc_factor) || gc_factor->valuedouble <= 0 || gc_factor->valuedouble > UINT8_MAX) {
        return 45;
    }

    return (uint8_t)gc_factor->valuedouble;
}

static esp_err_t dhara_ftl_init(void *ctx, spi_nand_flash_device_t *nand, const cJSON *ftl_config)
{
    dhara_ftl_ctx_t **slot = dhara_ftl_ctx_slot(ctx);
    dhara_ftl_ctx_t *state;

    ESP_RETURN_ON_FALSE(slot != NULL, ESP_ERR_INVALID_ARG, "dhara_ftl", "ctx must not be NULL");
    ESP_RETURN_ON_FALSE(nand != NULL, ESP_ERR_INVALID_ARG, "dhara_ftl", "nand must not be NULL");
    ESP_RETURN_ON_FALSE(*slot == NULL, ESP_ERR_INVALID_STATE, "dhara_ftl", "ctx already initialized");

    (void)ftl_config;

    state = calloc(1, sizeof(dhara_ftl_ctx_t));
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_NO_MEM, "dhara_ftl", "failed to allocate ctx");

    if (spi_nand_flash_get_capacity(nand, &state->sector_count) != ESP_OK) {
        free(state);
        return ESP_FAIL;
    }

    state->nand = nand;
    state->initialized = true;
    *slot = state;
    return ESP_OK;
}

static esp_err_t dhara_ftl_write(void *ctx, uint32_t sector, const uint8_t *data)
{
    dhara_ftl_ctx_t *state = dhara_ftl_ctx_from_slot(ctx);
    ESP_RETURN_ON_FALSE(state != NULL && state->initialized, ESP_ERR_INVALID_STATE, "dhara_ftl",
                        "ctx is not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, "dhara_ftl", "data must not be NULL");
    ESP_RETURN_ON_FALSE(sector < state->sector_count, ESP_ERR_INVALID_ARG, "dhara_ftl",
                        "sector out of range");

    return spi_nand_flash_write_page(state->nand, data, sector);
}

static esp_err_t dhara_ftl_read(void *ctx, uint32_t sector, uint8_t *data)
{
    dhara_ftl_ctx_t *state = dhara_ftl_ctx_from_slot(ctx);
    ESP_RETURN_ON_FALSE(state != NULL && state->initialized, ESP_ERR_INVALID_STATE, "dhara_ftl",
                        "ctx is not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, "dhara_ftl", "data must not be NULL");
    ESP_RETURN_ON_FALSE(sector < state->sector_count, ESP_ERR_INVALID_ARG, "dhara_ftl",
                        "sector out of range");

    return spi_nand_flash_read_page(state->nand, data, sector);
}

static esp_err_t dhara_ftl_sync(void *ctx)
{
    dhara_ftl_ctx_t *state = dhara_ftl_ctx_from_slot(ctx);

    ESP_RETURN_ON_FALSE(state != NULL && state->initialized, ESP_ERR_INVALID_STATE, "dhara_ftl",
                        "ctx is not initialized");

    return spi_nand_flash_sync(state->nand);
}

static esp_err_t dhara_ftl_deinit(void *ctx)
{
    dhara_ftl_ctx_t **slot = dhara_ftl_ctx_slot(ctx);

    ESP_RETURN_ON_FALSE(slot != NULL, ESP_ERR_INVALID_ARG, "dhara_ftl", "ctx must not be NULL");
    if (*slot == NULL) {
        return ESP_OK;
    }

    free(*slot);
    *slot = NULL;
    return ESP_OK;
}

ftl_ops_t *dhara_ftl_get_ops(void)
{
    static ftl_ops_t s_ops = {
        .init = dhara_ftl_init,
        .write = dhara_ftl_write,
        .read = dhara_ftl_read,
        .sync = dhara_ftl_sync,
        .deinit = dhara_ftl_deinit,
    };

    return &s_ops;
}
