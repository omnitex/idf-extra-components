/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * No-op failure model: never injects failures, never corrupts data.
 * Useful as a placeholder when only wear tracking (not failure injection) is needed,
 * or as a test double.
 */

#include "nand_emul_advanced.h"

static esp_err_t noop_fm_init(void **model_handle, const void *config)
{
    (void)config;
    *model_handle = NULL;
    return ESP_OK;
}

static esp_err_t noop_fm_deinit(void *model_handle)
{
    (void)model_handle;
    return ESP_OK;
}

static bool noop_should_fail_read(void *model_handle, const nand_operation_context_t *ctx)
{
    (void)model_handle;
    (void)ctx;
    return false;
}

static bool noop_should_fail_write(void *model_handle, const nand_operation_context_t *ctx)
{
    (void)model_handle;
    (void)ctx;
    return false;
}

static bool noop_should_fail_erase(void *model_handle, const nand_operation_context_t *ctx)
{
    (void)model_handle;
    (void)ctx;
    return false;
}

static void noop_corrupt_read_data(void *model_handle, const nand_operation_context_t *ctx,
                                   uint8_t *data, size_t len)
{
    /* No-op: data is not modified */
    (void)model_handle;
    (void)ctx;
    (void)data;
    (void)len;
}

static bool noop_is_block_bad(void *model_handle, uint32_t block_num,
                               const block_metadata_t *meta)
{
    (void)model_handle;
    (void)block_num;
    (void)meta;
    return false;
}

const nand_failure_model_ops_t nand_noop_failure_model = {
    .init               = noop_fm_init,
    .deinit             = noop_fm_deinit,
    .should_fail_read   = noop_should_fail_read,
    .should_fail_write  = noop_should_fail_write,
    .should_fail_erase  = noop_should_fail_erase,
    .corrupt_read_data  = noop_corrupt_read_data,
    .is_block_bad       = noop_is_block_bad,
};
