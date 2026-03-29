/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include "nand_emul_advanced.h"
#include "nand_emul_advanced_priv.h"
#include "nand_linux_mmap_emul.h"
#include "nand.h"
#include "esp_log.h"

static const char *TAG = "nand_adv";

/* ---------------------------------------------------------------------------
 * Internal advanced context
 * -------------------------------------------------------------------------*/

typedef struct {
    /* Configuration (copies of the caller's cfg, so we own these) */
    const nand_metadata_backend_ops_t *backend_ops;
    void                              *backend_handle;
    const nand_failure_model_ops_t    *failure_ops;
    void                              *failure_handle;

    /* Tracking flags */
    bool track_block_level;
    bool track_page_level;

    /* Device geometry (cached at init) */
    uint32_t total_blocks;
    uint32_t pages_per_block;
    uint32_t page_size;

    /* Timestamp function */
    uint64_t (*get_timestamp)(void);

    /* Monotonic counter used by default timestamp fn */
    uint64_t timestamp_counter;

    /* WAF accounting */
    uint64_t physical_bytes_written;
    uint64_t logical_write_bytes_recorded;
} nand_advanced_context_t;

/* Default monotonic timestamp: just increment a counter */
static uint64_t s_default_timestamp_counter = 0;

static uint64_t default_get_timestamp(void)
{
    return ++s_default_timestamp_counter;
}

/* Helper: get the advanced context from a device, or NULL */
static nand_advanced_context_t *get_ctx(spi_nand_flash_device_t *dev)
{
    if (dev == NULL || dev->emul_handle == NULL) {
        return NULL;
    }
    nand_mmap_emul_handle_t *emul = (nand_mmap_emul_handle_t *)dev->emul_handle;
    return (nand_advanced_context_t *)emul->advanced;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------*/

esp_err_t nand_emul_advanced_init(spi_nand_flash_device_t **dev_out,
                                  const nand_emul_advanced_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *dev_out = NULL;

    /* Build an spi_nand_flash_config_t from the base_config in cfg */
    nand_file_mmap_emul_config_t base_cfg = cfg->base_config; /* local copy */
    spi_nand_flash_config_t nand_cfg = {
        .emul_conf = &base_cfg,
        .io_mode = SPI_NAND_IO_MODE_SIO,
    };

    /* Delegate to the standard init (which allocates and fills the device handle) */
    spi_nand_flash_device_t *dev = NULL;
    esp_err_t ret = spi_nand_flash_init_device(&nand_cfg, &dev);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Allocate the advanced context */
    nand_advanced_context_t *ctx = calloc(1, sizeof(nand_advanced_context_t));
    if (ctx == NULL) {
        spi_nand_flash_deinit_device(dev);
        return ESP_ERR_NO_MEM;
    }

    ctx->backend_ops       = cfg->metadata_backend;
    ctx->failure_ops       = cfg->failure_model;
    ctx->track_block_level = cfg->track_block_level;
    ctx->track_page_level  = cfg->track_page_level;
    ctx->get_timestamp     = cfg->get_timestamp ? cfg->get_timestamp : default_get_timestamp;

    /* Cache device geometry */
    uint32_t sector_size, block_size;
    if (spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK &&
        spi_nand_flash_get_block_size(dev, &block_size) == ESP_OK) {
        ctx->page_size       = sector_size;
        ctx->pages_per_block = (sector_size > 0) ? block_size / sector_size : 0;
    }

    uint32_t sector_num;
    if (spi_nand_flash_get_capacity(dev, &sector_num) == ESP_OK && ctx->pages_per_block > 0) {
        ctx->total_blocks = sector_num / ctx->pages_per_block;
    }

    /* Initialize backend if provided */
    if (ctx->backend_ops && ctx->backend_ops->init) {
        /* If the caller supplied a sparse_hash_backend_config_t, inject the
         * cached pages_per_block so the backend can perform erase-fold. */
        if (cfg->metadata_backend == &nand_sparse_hash_backend &&
            cfg->metadata_backend_config != NULL) {
            sparse_hash_backend_config_t *shcfg =
                (sparse_hash_backend_config_t *)cfg->metadata_backend_config;
            shcfg->pages_per_block = ctx->pages_per_block;
        }
        ret = ctx->backend_ops->init(&ctx->backend_handle, cfg->metadata_backend_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Backend init failed: %d", ret);
            free(ctx);
            spi_nand_flash_deinit_device(dev);
            return ret;
        }
    }

    /* Initialize failure model if provided */
    if (ctx->failure_ops && ctx->failure_ops->init) {
        ret = ctx->failure_ops->init(&ctx->failure_handle, cfg->failure_model_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failure model init failed: %d", ret);
            if (ctx->backend_ops && ctx->backend_ops->deinit) {
                ctx->backend_ops->deinit(ctx->backend_handle);
            }
            free(ctx);
            spi_nand_flash_deinit_device(dev);
            return ret;
        }
    }

    /* Attach context to emul handle */
    nand_mmap_emul_handle_t *emul = (nand_mmap_emul_handle_t *)dev->emul_handle;
    emul->advanced = ctx;

    *dev_out = dev;
    return ESP_OK;
}

esp_err_t nand_emul_advanced_deinit(spi_nand_flash_device_t *dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nand_advanced_context_t *ctx = get_ctx(dev);

    if (ctx != NULL) {
        /* Deinit failure model */
        if (ctx->failure_ops && ctx->failure_ops->deinit) {
            ctx->failure_ops->deinit(ctx->failure_handle);
        }
        /* Deinit backend */
        if (ctx->backend_ops && ctx->backend_ops->deinit) {
            ctx->backend_ops->deinit(ctx->backend_handle);
        }

        /* Detach and free context */
        nand_mmap_emul_handle_t *emul = (nand_mmap_emul_handle_t *)dev->emul_handle;
        emul->advanced = NULL;
        free(ctx);
    }

    return spi_nand_flash_deinit_device(dev);
}

bool nand_emul_has_advanced_tracking(spi_nand_flash_device_t *dev)
{
    return get_ctx(dev) != NULL;
}

/* ---------------------------------------------------------------------------
 * Query API stubs (return ESP_ERR_INVALID_STATE if no context)
 * -------------------------------------------------------------------------*/

esp_err_t nand_emul_get_block_wear(spi_nand_flash_device_t *dev,
                                   uint32_t block_num,
                                   block_metadata_t *out)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->block_num = block_num;
    if (ctx->backend_ops && ctx->backend_ops->get_block_info) {
        return ctx->backend_ops->get_block_info(ctx->backend_handle, block_num, out);
    }
    return ESP_OK;
}

esp_err_t nand_emul_get_page_wear(spi_nand_flash_device_t *dev,
                                  uint32_t page_num,
                                  page_metadata_t *out)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->page_num = page_num;
    if (ctx->backend_ops && ctx->backend_ops->get_page_info) {
        return ctx->backend_ops->get_page_info(ctx->backend_handle, page_num, out);
    }
    return ESP_OK;
}

esp_err_t nand_emul_get_wear_stats(spi_nand_flash_device_t *dev,
                                   nand_wear_stats_t *out)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (ctx->backend_ops && ctx->backend_ops->get_stats) {
        esp_err_t ret = ctx->backend_ops->get_stats(ctx->backend_handle, out);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    /* Merge WAF fields from context */
    out->logical_write_bytes_recorded = ctx->logical_write_bytes_recorded;
    if (ctx->logical_write_bytes_recorded > 0) {
        out->write_amplification = (double)ctx->physical_bytes_written /
                                   (double)ctx->logical_write_bytes_recorded;
    }
    return ESP_OK;
}

esp_err_t nand_emul_get_wear_histograms(spi_nand_flash_device_t *dev,
                                        nand_wear_histograms_t *out)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->backend_ops == NULL || ctx->backend_ops->get_histograms == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ctx->backend_ops->get_histograms(ctx->backend_handle, out);
}

esp_err_t nand_emul_record_logical_write(spi_nand_flash_device_t *dev, size_t nbytes)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ctx->logical_write_bytes_recorded += nbytes;
    return ESP_OK;
}

esp_err_t nand_emul_iterate_worn_blocks(spi_nand_flash_device_t *dev,
                                        bool (*callback)(uint32_t block_num,
                                                         block_metadata_t *meta,
                                                         void *user_data),
                                        void *user_data)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->backend_ops && ctx->backend_ops->iterate_blocks) {
        return ctx->backend_ops->iterate_blocks(ctx->backend_handle, callback, user_data);
    }
    return ESP_OK;
}

esp_err_t nand_emul_iterate_worn_pages(spi_nand_flash_device_t *dev,
                                       bool (*callback)(uint32_t page_num,
                                                        page_metadata_t *meta,
                                                        void *user_data),
                                       void *user_data)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->backend_ops && ctx->backend_ops->iterate_pages) {
        return ctx->backend_ops->iterate_pages(ctx->backend_handle, callback, user_data);
    }
    return ESP_OK;
}

esp_err_t nand_emul_mark_bad_block(spi_nand_flash_device_t *dev, uint32_t block_num)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->backend_ops && ctx->backend_ops->set_bad_block) {
        return ctx->backend_ops->set_bad_block(ctx->backend_handle, block_num, true);
    }
    return ESP_OK;
}

esp_err_t nand_emul_save_snapshot(spi_nand_flash_device_t *dev, const char *filename)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->backend_ops && ctx->backend_ops->save_snapshot) {
        return ctx->backend_ops->save_snapshot(ctx->backend_handle, filename,
                                               ctx->get_timestamp());
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nand_emul_load_snapshot(spi_nand_flash_device_t *dev, const char *filename)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->backend_ops && ctx->backend_ops->load_snapshot) {
        return ctx->backend_ops->load_snapshot(ctx->backend_handle, filename);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nand_emul_export_json(spi_nand_flash_device_t *dev, const char *filename)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->backend_ops && ctx->backend_ops->export_json) {
        return ctx->backend_ops->export_json(ctx->backend_handle, filename);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * Private notify API (called from nand_linux_mmap_emul.c)
 * -------------------------------------------------------------------------*/

void nand_emul_advanced_notify_erase(spi_nand_flash_device_t *dev, uint32_t block_num)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return;
    }
    if (ctx->backend_ops && ctx->backend_ops->on_block_erase) {
        ctx->backend_ops->on_block_erase(ctx->backend_handle, block_num,
                                         ctx->get_timestamp());
    }
}

void nand_emul_advanced_notify_program(spi_nand_flash_device_t *dev, uint32_t page_num)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return;
    }
    if (ctx->backend_ops && ctx->backend_ops->on_page_program) {
        ctx->backend_ops->on_page_program(ctx->backend_handle, page_num,
                                          ctx->get_timestamp());
    }
    ctx->physical_bytes_written += ctx->page_size;
}

void nand_emul_advanced_notify_read(spi_nand_flash_device_t *dev, uint32_t page_num)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL) {
        return;
    }
    if (ctx->backend_ops && ctx->backend_ops->on_page_read) {
        ctx->backend_ops->on_page_read(ctx->backend_handle, page_num,
                                       ctx->get_timestamp());
    }
}
