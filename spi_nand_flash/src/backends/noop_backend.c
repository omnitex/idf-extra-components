/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * No-op metadata backend: all callbacks return ESP_OK and store nothing.
 * Useful as a placeholder when only failure injection (not wear tracking) is needed,
 * or as a test double.
 */

#include "nand_emul_advanced.h"

static esp_err_t noop_init(void **backend_handle, const void *config)
{
    (void)config;
    *backend_handle = NULL;
    return ESP_OK;
}

static esp_err_t noop_deinit(void *backend_handle)
{
    (void)backend_handle;
    return ESP_OK;
}

static esp_err_t noop_on_block_erase(void *backend_handle, uint32_t block_num, uint64_t timestamp)
{
    (void)backend_handle;
    (void)block_num;
    (void)timestamp;
    return ESP_OK;
}

static esp_err_t noop_on_page_program(void *backend_handle, uint32_t page_num, uint64_t timestamp)
{
    (void)backend_handle;
    (void)page_num;
    (void)timestamp;
    return ESP_OK;
}

static esp_err_t noop_on_page_read(void *backend_handle, uint32_t page_num, uint64_t timestamp)
{
    (void)backend_handle;
    (void)page_num;
    (void)timestamp;
    return ESP_OK;
}

static esp_err_t noop_get_block_info(void *backend_handle, uint32_t block_num, block_metadata_t *out)
{
    (void)backend_handle;
    (void)block_num;
    (void)out;
    return ESP_OK;
}

static esp_err_t noop_get_page_info(void *backend_handle, uint32_t page_num, page_metadata_t *out)
{
    (void)backend_handle;
    (void)page_num;
    (void)out;
    return ESP_OK;
}

static esp_err_t noop_set_bad_block(void *backend_handle, uint32_t block_num, bool is_bad)
{
    (void)backend_handle;
    (void)block_num;
    (void)is_bad;
    return ESP_OK;
}

static esp_err_t noop_iterate_blocks(void *backend_handle,
                                     bool (*callback)(uint32_t, block_metadata_t *, void *),
                                     void *user_data)
{
    /* No entries tracked — iteration immediately completes. */
    (void)backend_handle;
    (void)callback;
    (void)user_data;
    return ESP_OK;
}

static esp_err_t noop_iterate_pages(void *backend_handle,
                                    bool (*callback)(uint32_t, page_metadata_t *, void *),
                                    void *user_data)
{
    (void)backend_handle;
    (void)callback;
    (void)user_data;
    return ESP_OK;
}

static esp_err_t noop_get_stats(void *backend_handle, nand_wear_stats_t *out)
{
    (void)backend_handle;
    (void)out;
    return ESP_OK;
}

const nand_metadata_backend_ops_t nand_noop_backend = {
    .init            = noop_init,
    .deinit          = noop_deinit,
    .on_block_erase  = noop_on_block_erase,
    .on_page_program = noop_on_page_program,
    .on_page_read    = noop_on_page_read,
    .get_block_info  = noop_get_block_info,
    .get_page_info   = noop_get_page_info,
    .set_bad_block   = noop_set_bad_block,
    .iterate_blocks  = noop_iterate_blocks,
    .iterate_pages   = noop_iterate_pages,
    .get_stats       = noop_get_stats,
    .get_histograms  = NULL, /* not supported */
    .save_snapshot   = NULL,
    .load_snapshot   = NULL,
    .export_json     = NULL,
};
