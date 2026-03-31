/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Threshold failure model.
 *
 * Fails a block erase once the block's erase_count reaches max_block_erases.
 * Fails a page program once the page's lifetime program count reaches
 * max_page_programs.  Read operations are never failed by this model.
 *
 * The model reads current wear counts from the operation context metadata
 * pointers provided by the core (block_meta and page_meta in
 * nand_operation_context_t).
 */

#include "nand_emul_advanced.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    threshold_failure_config_t cfg;
} threshold_ctx_t;

static esp_err_t threshold_init(void **handle_out, const void *config)
{
    threshold_ctx_t *ctx = calloc(1, sizeof(threshold_ctx_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    if (config) {
        memcpy(&ctx->cfg, config, sizeof(threshold_failure_config_t));
    }
    *handle_out = ctx;
    return ESP_OK;
}

static esp_err_t threshold_deinit(void *handle)
{
    free(handle);
    return ESP_OK;
}

static bool threshold_should_fail_erase(void *handle,
                                        const nand_operation_context_t *ctx)
{
    if (!handle || !ctx) return false;
    threshold_ctx_t *th = (threshold_ctx_t *)handle;
    if (th->cfg.max_block_erases == 0) return false;

    uint32_t erase_count = 0;
    if (ctx->block_meta) {
        erase_count = ctx->block_meta->erase_count;
    }
    /* Fail if the count has already reached (or exceeded) the limit */
    return (erase_count >= th->cfg.max_block_erases);
}

static bool threshold_should_fail_write(void *handle,
                                        const nand_operation_context_t *ctx)
{
    if (!handle || !ctx) return false;
    threshold_ctx_t *th = (threshold_ctx_t *)handle;
    if (th->cfg.max_page_programs == 0) return false;

    uint32_t prog_count = 0;
    if (ctx->page_meta) {
        prog_count = ctx->page_meta->program_count_total +
                     ctx->page_meta->program_count;
    }
    return (prog_count >= th->cfg.max_page_programs);
}

static bool threshold_should_fail_read(void *handle,
                                       const nand_operation_context_t *ctx)
{
    (void)handle;
    (void)ctx;
    return false; /* Threshold model never fails reads */
}

static void threshold_corrupt_read_data(void *handle,
                                        const nand_operation_context_t *ctx,
                                        uint8_t *data, size_t len)
{
    (void)handle;
    (void)ctx;
    (void)data;
    (void)len;
    /* No bit-flip injection in threshold model */
}

static bool threshold_is_block_bad(void *handle, uint32_t block_num,
                                   const block_metadata_t *meta)
{
    (void)block_num;
    if (!handle || !meta) return false;
    threshold_ctx_t *th = (threshold_ctx_t *)handle;
    if (th->cfg.max_block_erases == 0) return false;
    /* A block is bad once its erase count has reached the limit */
    return meta->erase_count >= th->cfg.max_block_erases;
}

const nand_failure_model_ops_t nand_threshold_failure_model = {
    .init              = threshold_init,
    .deinit            = threshold_deinit,
    .should_fail_erase = threshold_should_fail_erase,
    .should_fail_write = threshold_should_fail_write,
    .should_fail_read  = threshold_should_fail_read,
    .corrupt_read_data = threshold_corrupt_read_data,
    .is_block_bad      = threshold_is_block_bad,
};
