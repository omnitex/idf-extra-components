/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#ifndef CONFIG_IDF_TARGET_LINUX
#include "spi_nand_oper.h"
#endif
#include "nand_impl.h"
#include "nand.h"

// Note: nvblock header will be included once nvblock component is available
// #include "nvblock/nvblock.h"

static const char *TAG = "nvblock_glue";

/**
 * @brief nvblock context structure
 * 
 * This structure holds the nvblock instance and associated metadata for
 * integration with the spi_nand_flash component.
 */
typedef struct {
    spi_nand_flash_device_t *parent_handle;  ///< Pointer back to parent device
    // nvb_t nvb;                              ///< nvblock instance (TODO: add when nvblock available)
    uint8_t *meta_buf;                        ///< Metadata buffer for nvblock (runtime-sized)
    size_t meta_buf_size;                     ///< Size of metadata buffer
} nvblock_context_t;

// ============================================================================
// nvblock HAL Callbacks
// ============================================================================
// These callbacks bridge nvblock to the SPI NAND HAL (nand_impl.h)

/**
 * @brief nvblock read callback - read a page from a group (block)
 */
static int nvb_read_cb(void *ctx, uint32_t group, uint32_t page, void *buf, uint32_t len)
{
    ESP_LOGD(TAG, "nvb_read_cb: group=%lu, page=%lu, len=%lu", group, page, len);
    // TODO: Implement when nvblock is available
    // nvblock_context_t *nvb_ctx = (nvblock_context_t *)ctx;
    // Call HAL: nand_read_page()
    return -1; // Placeholder
}

/**
 * @brief nvblock write callback - write a page to a group (block)
 */
static int nvb_write_cb(void *ctx, uint32_t group, uint32_t page, const void *buf, uint32_t len)
{
    ESP_LOGD(TAG, "nvb_write_cb: group=%lu, page=%lu, len=%lu", group, page, len);
    // TODO: Implement when nvblock is available
    return -1; // Placeholder
}

/**
 * @brief nvblock erase callback - erase a group (block)
 */
static int nvb_erase_cb(void *ctx, uint32_t group)
{
    ESP_LOGD(TAG, "nvb_erase_cb: group=%lu", group);
    // TODO: Implement when nvblock is available
    return -1; // Placeholder
}

/**
 * @brief nvblock is-bad callback - check if a group (block) is bad
 */
static int nvb_isbad_cb(void *ctx, uint32_t group)
{
    ESP_LOGD(TAG, "nvb_isbad_cb: group=%lu", group);
    // TODO: Implement when nvblock is available
    return -1; // Placeholder
}

/**
 * @brief nvblock mark-bad callback - mark a group (block) as bad
 */
static int nvb_markbad_cb(void *ctx, uint32_t group)
{
    ESP_LOGD(TAG, "nvb_markbad_cb: group=%lu", group);
    // TODO: Implement when nvblock is available
    return -1; // Placeholder
}

/**
 * @brief nvblock move callback - optimized block copy operation
 */
static int nvb_move_cb(void *ctx, uint32_t from_grp, uint32_t from_pg,
                      uint32_t to_grp, uint32_t to_pg, uint32_t len)
{
    ESP_LOGD(TAG, "nvb_move_cb: from=%lu:%lu to=%lu:%lu len=%lu",
             from_grp, from_pg, to_grp, to_pg, len);
    // TODO: Implement using HAL nand_copy() for optimization
    return -1; // Placeholder
}

// ============================================================================
// spi_nand_ops Interface Implementation
// ============================================================================

/**
 * @brief Initialize nvblock wear leveling layer
 */
static esp_err_t nvblock_init(spi_nand_flash_device_t *handle)
{
    ESP_LOGI(TAG, "Initializing nvblock wear leveling");

    // Allocate nvblock context
    nvblock_context_t *nvb_ctx = calloc(1, sizeof(nvblock_context_t));
    if (!nvb_ctx) {
        ESP_LOGE(TAG, "Failed to allocate nvblock context");
        return ESP_ERR_NO_MEM;
    }

    handle->ops_priv_data = nvb_ctx;
    nvb_ctx->parent_handle = handle;

    // TODO: Calculate nvblock configuration from chip parameters
    // - bsize = chip.page_size
    // - bpg = chip.pages_per_block (1 << chip.log2_ppb)
    // - gcnt = chip.num_blocks
    // - spgcnt = calculated spare blocks

    // TODO: Allocate metadata buffer (48 + bpg*2 bytes)
    // nvb_ctx->meta_buf_size = 48 + (bpg * 2);
    // nvb_ctx->meta_buf = malloc(nvb_ctx->meta_buf_size);

    // TODO: Configure nvblock callbacks and initialize
    // nvb_config.cfg.read = nvb_read_cb;
    // nvb_config.cfg.write = nvb_write_cb;
    // ...
    // nvb_init(&nvb_ctx->nvb, &nvb_config);

    ESP_LOGI(TAG, "nvblock initialized successfully");
    return ESP_OK;
}

/**
 * @brief Deinitialize nvblock
 */
static esp_err_t nvblock_deinit(spi_nand_flash_device_t *handle)
{
    ESP_LOGI(TAG, "Deinitializing nvblock");
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)handle->ops_priv_data;

    if (nvb_ctx) {
        if (nvb_ctx->meta_buf) {
            free(nvb_ctx->meta_buf);
        }
        free(nvb_ctx);
        handle->ops_priv_data = NULL;
    }

    return ESP_OK;
}

/**
 * @brief Read a sector through nvblock
 */
static esp_err_t nvblock_read(spi_nand_flash_device_t *handle, uint8_t *buffer, uint32_t sector_id)
{
    ESP_LOGD(TAG, "nvblock_read: sector=%lu", sector_id);
    // TODO: Implement logical sector read via nvblock
    // nvblock_context_t *nvb_ctx = (nvblock_context_t *)handle->ops_priv_data;
    // Call nvb_read() with logical address translation
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Write a sector through nvblock
 */
static esp_err_t nvblock_write(spi_nand_flash_device_t *handle, const uint8_t *buffer, uint32_t sector_id)
{
    ESP_LOGD(TAG, "nvblock_write: sector=%lu", sector_id);
    // TODO: Implement logical sector write via nvblock
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Erase the entire chip
 */
static esp_err_t nvblock_erase_chip(spi_nand_flash_device_t *handle)
{
    ESP_LOGI(TAG, "nvblock_erase_chip");
    // TODO: Implement chip erase (erase all blocks, reinitialize nvblock)
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Erase a logical block
 */
static esp_err_t nvblock_erase_block(spi_nand_flash_device_t *handle, uint32_t block)
{
    ESP_LOGD(TAG, "nvblock_erase_block: block=%lu", block);
    // TODO: Map to nvblock trim operation
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Trim a sector (mark as no longer needed)
 */
static esp_err_t nvblock_trim(spi_nand_flash_device_t *handle, uint32_t sector_id)
{
    ESP_LOGD(TAG, "nvblock_trim: sector=%lu", sector_id);
    // TODO: Call nvb_trim()
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Sync/flush pending operations
 */
static esp_err_t nvblock_sync(spi_nand_flash_device_t *handle)
{
    ESP_LOGD(TAG, "nvblock_sync");
    // TODO: Call nvb_flush()
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Copy a sector
 */
static esp_err_t nvblock_copy_sector(spi_nand_flash_device_t *handle, uint32_t src_sec, uint32_t dst_sec)
{
    ESP_LOGD(TAG, "nvblock_copy_sector: src=%lu, dst=%lu", src_sec, dst_sec);
    // TODO: Implement sector copy (read + write)
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief Get capacity in sectors
 */
static esp_err_t nvblock_get_capacity(spi_nand_flash_device_t *handle, uint32_t *number_of_sectors)
{
    ESP_LOGD(TAG, "nvblock_get_capacity");
    // TODO: Query nvb_capacity() and convert to sectors
    if (number_of_sectors) {
        *number_of_sectors = 0; // Placeholder
    }
    return ESP_ERR_NOT_SUPPORTED;
}

// ============================================================================
// nvblock Operations Table Export
// ============================================================================

const spi_nand_ops nvblock_ops = {
    .init = nvblock_init,
    .deinit = nvblock_deinit,
    .read = nvblock_read,
    .write = nvblock_write,
    .erase_chip = nvblock_erase_chip,
    .erase_block = nvblock_erase_block,
    .trim = nvblock_trim,
    .sync = nvblock_sync,
    .copy_sector = nvblock_copy_sector,
    .get_capacity = nvblock_get_capacity,
};

// ============================================================================
// Device Registration Functions
// ============================================================================

esp_err_t nand_register_dev(spi_nand_flash_device_t *handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->ops = &nvblock_ops;
    return ESP_OK;
}

esp_err_t nand_unregister_dev(spi_nand_flash_device_t *handle)
{
    free(handle->ops_priv_data);
    handle->ops = NULL;
    return ESP_OK;
}
