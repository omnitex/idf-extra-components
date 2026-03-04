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
#include "nvblock/nvblock.h"
#ifndef CONFIG_IDF_TARGET_LINUX
#include "spi_nand_oper.h"
#endif
#include "nand_impl.h"
#include "nand.h"

static const char *TAG = "nvblock_glue";

// Task 5.7: Error code mapping (nvblock uses negative errno values)
#define NVB_EIO     5   // I/O error
#define NVB_EFAULT  14  // Bad address (already defined in nvblock.h as NVB_EFAULT)

/**
 * @brief nvblock context structure
 * 
 * This structure holds the nvblock instance and associated metadata for
 * integration with the spi_nand_flash component.
 */
typedef struct {
    spi_nand_flash_device_t *parent_handle;  ///< Pointer back to parent device
    struct nvb_info nvb_info;                 ///< nvblock instance
    struct nvb_config nvb_config;             ///< nvblock configuration
    uint8_t *meta_buf;                        ///< Metadata buffer for nvblock (runtime-sized)
    size_t meta_buf_size;                     ///< Size of metadata buffer
} nvblock_context_t;

// ============================================================================
// nvblock HAL Callbacks
// ============================================================================
// These callbacks bridge nvblock to the SPI NAND HAL (nand_impl.h)
//
// NOTE: nvblock terminology mapping to NAND:
// - p (virtual block location) = NAND absolute page number
// - bsize (virtual block size) = NAND page size
// - bpg (blocks per group) = NAND pages per block
// - group = bpg consecutive virtual blocks = 1 NAND erase block

/**
 * @brief nvblock read callback - read a virtual block (page)
 * 
 * Task 5.1: Read a page from NAND flash.
 * 
 * @param cfg nvblock configuration
 * @param p Virtual block location (absolute NAND page number)
 * @param buffer Output buffer (size = bsize = page_size)
 * @return 0 on success, negative errno on failure
 */
static int nvb_read_cb(const struct nvb_config *cfg, uint32_t p, void *buffer)
{
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)cfg->context;
    spi_nand_flash_device_t *dev_handle = nvb_ctx->parent_handle;
    
    ESP_LOGD(TAG, "nvb_read_cb: page=%lu", p);
    
    // Read the page via HAL (offset=0, length=full page)
    esp_err_t ret = nand_read(dev_handle, p, 0, dev_handle->chip.page_size, buffer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nand_read failed for page %lu: %d", p, ret);
        return -NVB_EIO;  // I/O error
    }
    
    return 0;
}

/**
 * @brief nvblock program callback - write a virtual block (page)
 * 
 * Task 5.2: Write a page to NAND flash.
 * nvblock expects the callback to handle erase-before-write if needed
 * (erase when writing first page of a group/block).
 * 
 * @param cfg nvblock configuration
 * @param p Virtual block location (absolute NAND page number)
 * @param buffer Input buffer (size = bsize = page_size)
 * @return 0 on success, -NVB_EFAULT on bad block, negative errno on other failure
 */
static int nvb_prog_cb(const struct nvb_config *cfg, uint32_t p, const void *buffer)
{
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)cfg->context;
    spi_nand_flash_device_t *dev_handle = nvb_ctx->parent_handle;
    uint32_t pages_per_block = 1 << dev_handle->chip.log2_ppb;
    
    ESP_LOGD(TAG, "nvb_prog_cb: page=%lu", p);
    
    // Check if this is the first page in a block (needs erase first)
    if ((p % pages_per_block) == 0) {
        uint32_t block = p / pages_per_block;
        ESP_LOGD(TAG, "Erasing block %lu before programming", block);
        
        esp_err_t ret = nand_erase_block(dev_handle, block);
        if (ret == ESP_ERR_NOT_FINISHED) {
            // Bad block detected during erase
            ESP_LOGW(TAG, "Bad block detected: %lu", block);
            return -NVB_EFAULT;
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nand_erase_block failed for block %lu: %d", block, ret);
            return -NVB_EIO;
        }
    }
    
    // Program the page
    esp_err_t ret = nand_prog(dev_handle, p, buffer);
    if (ret == ESP_ERR_NOT_FINISHED) {
        // Bad block detected during program
        ESP_LOGW(TAG, "Bad block detected during prog: page %lu", p);
        return -NVB_EFAULT;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nand_prog failed for page %lu: %d", p, ret);
        return -NVB_EIO;
    }
    
    return 0;
}

/**
 * @brief nvblock move callback - optimized page copy
 * 
 * Task 5.6: Copy a page from one location to another.
 * nvblock uses this for wear leveling block moves.
 * 
 * @param cfg nvblock configuration
 * @param pf Source virtual block (page)
 * @param pt Destination virtual block (page)
 * @return 0 on success, -NVB_EFAULT on bad block, negative errno on failure
 */
static int nvb_move_cb(const struct nvb_config *cfg, uint32_t pf, uint32_t pt)
{
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)cfg->context;
    spi_nand_flash_device_t *dev_handle = nvb_ctx->parent_handle;
    uint32_t pages_per_block = 1 << dev_handle->chip.log2_ppb;
    
    ESP_LOGD(TAG, "nvb_move_cb: from=%lu to=%lu", pf, pt);
    
    // If writing to first page of block, need to erase first
    if ((pt % pages_per_block) == 0) {
        uint32_t block = pt / pages_per_block;
        ESP_LOGD(TAG, "Erasing block %lu before move", block);
        
        esp_err_t ret = nand_erase_block(dev_handle, block);
        if (ret == ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "Bad block detected: %lu", block);
            return -NVB_EFAULT;
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nand_erase_block failed: %d", ret);
            return -NVB_EIO;
        }
    }
    
    // Use optimized HAL copy operation
    esp_err_t ret = nand_copy(dev_handle, pf, pt);
    if (ret == ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "Bad block detected during copy: src=%lu dst=%lu", pf, pt);
        return -NVB_EFAULT;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nand_copy failed: %d", ret);
        return -NVB_EIO;
    }
    
    return 0;
}

/**
 * @brief nvblock is-bad callback - check if a virtual block is in a bad block
 * 
 * Task 5.4: Check if the page belongs to a factory-marked bad block.
 * 
 * @param cfg nvblock configuration
 * @param p Virtual block location (page)
 * @return true if bad, false if good
 */
static bool nvb_isbad_cb(const struct nvb_config *cfg, uint32_t p)
{
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)cfg->context;
    spi_nand_flash_device_t *dev_handle = nvb_ctx->parent_handle;
    uint32_t pages_per_block = 1 << dev_handle->chip.log2_ppb;
    uint32_t block = p / pages_per_block;
    
    ESP_LOGD(TAG, "nvb_isbad_cb: page=%lu (block=%lu)", p, block);
    
    bool is_bad = false;
    esp_err_t ret = nand_is_bad(dev_handle, block, &is_bad);
    if (ret != ESP_OK) {
        // On error, assume bad for safety
        ESP_LOGW(TAG, "nand_is_bad check failed for block %lu, assuming bad", block);
        return true;
    }
    
    return is_bad;
}

/**
 * @brief nvblock is-free callback - check if a virtual block is erased
 * 
 * Check if a page is in erased state (all 0xFF).
 * 
 * @param cfg nvblock configuration
 * @param p Virtual block location (page)
 * @return true if erased, false if programmed
 */
static bool nvb_isfree_cb(const struct nvb_config *cfg, uint32_t p)
{
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)cfg->context;
    spi_nand_flash_device_t *dev_handle = nvb_ctx->parent_handle;
    
    ESP_LOGD(TAG, "nvb_isfree_cb: page=%lu", p);
    
    bool is_free = false;
    esp_err_t ret = nand_is_free(dev_handle, p, &is_free);
    if (ret != ESP_OK) {
        // On error, assume not free
        ESP_LOGW(TAG, "nand_is_free check failed for page %lu, assuming not free", p);
        return false;
    }
    
    return is_free;
}

/**
 * @brief nvblock mark-bad callback - mark a virtual block's parent block as bad
 * 
 * Task 5.5: Mark the NAND block containing this page as bad.
 * 
 * @param cfg nvblock configuration
 * @param p Virtual block location (page)
 */
static void nvb_markbad_cb(const struct nvb_config *cfg, uint32_t p)
{
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)cfg->context;
    spi_nand_flash_device_t *dev_handle = nvb_ctx->parent_handle;
    uint32_t pages_per_block = 1 << dev_handle->chip.log2_ppb;
    uint32_t block = p / pages_per_block;
    
    ESP_LOGW(TAG, "nvb_markbad_cb: marking block %lu as bad (page=%lu)", block, p);
    
    esp_err_t ret = nand_mark_bad(dev_handle, block);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mark block %lu as bad: %d", block, ret);
    }
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

    // Task 4.3: Calculate nvblock configuration from chip parameters
    // Map nvblock terminology to NAND flash:
    // - bsize (virtual block size) = NAND page size
    // - bpg (blocks per group) = NAND pages per block
    // - gcnt (total groups) = NAND total blocks
    // - spgcnt (spare groups) = calculated from gc_factor
    
    uint32_t bsize = handle->chip.page_size;                  // e.g., 2048 bytes
    uint32_t bpg = 1 << handle->chip.log2_ppb;                // e.g., 64 pages/block
    uint32_t gcnt = handle->chip.num_blocks;                  // e.g., 1024 blocks
    
    // Calculate spare group count for wear leveling
    // gc_factor is percentage (e.g., 4 means 4% spare)
    // Minimum 2 spare groups for wear leveling to function
    uint32_t spgcnt = (gcnt * handle->config.gc_factor) / 100;
    if (spgcnt < 2) {
        spgcnt = 2;  // Ensure minimum spare blocks
    }
    
    ESP_LOGI(TAG, "nvblock config: bsize=%lu, bpg=%lu, gcnt=%lu, spgcnt=%lu",
             bsize, bpg, gcnt, spgcnt);

    // Task 4.4: Allocate metadata buffer with runtime sizing
    // According to nvblock documentation and comprehensive-spec.md:
    // Metadata size = NVB_META_DMP_START + (bpg * NVB_META_ADDRESS_SIZE)
    // where NVB_META_DMP_START = 48 bytes (magic + version + epoch + crc + tgt + alt)
    // and NVB_META_ADDRESS_SIZE = 2 bytes per block address
    nvb_ctx->meta_buf_size = NVB_META_DMP_START + (bpg * NVB_META_ADDRESS_SIZE);
    nvb_ctx->meta_buf = calloc(1, nvb_ctx->meta_buf_size);
    if (!nvb_ctx->meta_buf) {
        ESP_LOGE(TAG, "Failed to allocate metadata buffer (%zu bytes)", nvb_ctx->meta_buf_size);
        free(nvb_ctx);
        handle->ops_priv_data = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Allocated metadata buffer: %zu bytes", nvb_ctx->meta_buf_size);

    // Configure nvblock structure
    memset(&nvb_ctx->nvb_config, 0, sizeof(struct nvb_config));
    nvb_ctx->nvb_config.context = nvb_ctx;
    nvb_ctx->nvb_config.meta = nvb_ctx->meta_buf;
    nvb_ctx->nvb_config.bsize = bsize;
    nvb_ctx->nvb_config.bpg = bpg;
    nvb_ctx->nvb_config.gcnt = gcnt;
    nvb_ctx->nvb_config.spgcnt = spgcnt;
    
    // Task 5.7: Wire up nvblock HAL callbacks
    nvb_ctx->nvb_config.read = nvb_read_cb;
    nvb_ctx->nvb_config.prog = nvb_prog_cb;
    nvb_ctx->nvb_config.move = nvb_move_cb;
    nvb_ctx->nvb_config.is_bad = nvb_isbad_cb;
    nvb_ctx->nvb_config.is_free = nvb_isfree_cb;
    nvb_ctx->nvb_config.mark_bad = nvb_markbad_cb;
    nvb_ctx->nvb_config.sync = NULL;  // Optional - not needed for SPI NAND
    nvb_ctx->nvb_config.comp = NULL;  // Optional - not implemented
    nvb_ctx->nvb_config.init = NULL;  // Optional - not needed
    nvb_ctx->nvb_config.lock = NULL;  // Optional - not using thread locking
    nvb_ctx->nvb_config.unlock = NULL; // Optional - not using thread locking
    
    // TODO: Initialize nvblock (Section 6)
    // int ret = nvb_init(&nvb_ctx->nvb_info, &nvb_ctx->nvb_config);
    // if (ret != 0) {
    //     ESP_LOGE(TAG, "nvb_init failed: %d", ret);
    //     free(nvb_ctx->meta_buf);
    //     free(nvb_ctx);
    //     handle->ops_priv_data = NULL;
    //     return ESP_FAIL;
    // }

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
