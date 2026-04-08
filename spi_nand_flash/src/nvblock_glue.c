/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <inttypes.h>
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

#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
#include "esp_nand_blockdev.h"
#endif

static const char *TAG = "nvblock_glue";

// Error code mapping (nvblock uses negative errno values)
#define NVB_EIO     5   // I/O error (maps to EIO)
#define NVB_EFAULT  14  // Bad address / bad block (maps to EFAULT)

/**
 * @brief nvblock context structure
 * 
 * This structure holds the nvblock instance and associated metadata for
 * integration with the spi_nand_flash component.
 */
typedef struct {
    spi_nand_flash_device_t *parent_handle;  ///< Pointer back to parent device
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    esp_blockdev_handle_t bdl_handle;       ///< Flash BDL (raw NAND); required when BDL is enabled
#endif
    struct nvb_info nvb_info;                 ///< nvblock instance
    struct nvb_config nvb_config;             ///< nvblock configuration
    uint8_t *meta_buf;                        ///< Metadata buffer for nvblock (runtime-sized)
    size_t meta_buf_size;                     ///< Size of metadata buffer
} nvblock_context_t;

/** Map nvblock read-style negative errno to esp_err_t */
static esp_err_t nvb_err_to_esp_read(int ret)
{
    if (ret == -NVB_EINVAL) {
        return ESP_ERR_INVALID_ARG;
    } else if (ret == -NVB_ENOENT) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_FAIL;
}

/** Map nvblock write-style negative errno to esp_err_t */
static esp_err_t nvb_err_to_esp_write(int ret)
{
    if (ret == -NVB_EINVAL) {
        return ESP_ERR_INVALID_ARG;
    } else if (ret == -NVB_ENOSPC) {
        return ESP_ERR_NO_MEM;
    } else if (ret == -NVB_EROFS) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
}

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
 * Read a page from NAND flash.
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

    esp_err_t ret;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(nvb_ctx->bdl_handle != NULL);
    esp_blockdev_handle_t bdl = nvb_ctx->bdl_handle;
    size_t page_size = dev_handle->chip.page_size;
    ret = bdl->ops->read(bdl, buffer, page_size,
                         (uint64_t)p * bdl->geometry.read_size, page_size);
#else
    ret = nand_read(dev_handle, p, 0, dev_handle->chip.page_size, buffer);
#endif
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvb_read_cb FAILED: page=%"PRIu32" ret=%d", p, ret);
        return -NVB_EIO;  // I/O error
    }

    return 0;
}

/**
 * @brief nvblock program callback - write a virtual block (page)
 * 
 * Write a page to NAND flash.
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
    uint32_t block = p / pages_per_block;
    uint32_t pg_in_blk = p % pages_per_block;

    // Check if this is the first page in a block (needs erase first)
    if (pg_in_blk == 0) {
        esp_err_t ret;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
        assert(nvb_ctx->bdl_handle != NULL);
        esp_blockdev_handle_t bdl = nvb_ctx->bdl_handle;
        ret = bdl->ops->erase(bdl, (uint64_t)block * bdl->geometry.erase_size,
                              bdl->geometry.erase_size);
#else
        ret = nand_erase_block(dev_handle, block);
#endif
        if (ret == ESP_ERR_NOT_FINISHED) {
            // Bad block detected during erase
            ESP_LOGW(TAG, "nvb_prog_cb: bad block detected at block=%"PRIu32, block);
            return -NVB_EFAULT;
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvb_prog_cb: nand_erase_block FAILED block=%"PRIu32" ret=%d", block, ret);
            return -NVB_EIO;
        }
    }

    // Program the page
    esp_err_t ret;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(nvb_ctx->bdl_handle != NULL);
    esp_blockdev_handle_t bdl = nvb_ctx->bdl_handle;
    ret = bdl->ops->write(bdl, buffer, (uint64_t)p * bdl->geometry.write_size,
                          bdl->geometry.write_size);
#else
    ret = nand_prog(dev_handle, p, buffer);
#endif
    if (ret == ESP_ERR_NOT_FINISHED) {
        // Bad block detected during program
        ESP_LOGW(TAG, "nvb_prog_cb: bad block on prog page=%"PRIu32" block=%"PRIu32, p, block);
        return -NVB_EFAULT;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvb_prog_cb: nand_prog FAILED page=%"PRIu32" ret=%d", p, ret);
        return -NVB_EIO;
    }

    return 0;
}

/**
 * @brief nvblock move callback - optimized page copy
 * 
 * Copy a page from one location to another.
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
    uint32_t dst_block = pt / pages_per_block;
    uint32_t dst_pg_in_blk = pt % pages_per_block;

    // If writing to first page of block, need to erase first
    if (dst_pg_in_blk == 0) {
        esp_err_t eret;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
        assert(nvb_ctx->bdl_handle != NULL);
        esp_blockdev_handle_t bdl_e = nvb_ctx->bdl_handle;
        eret = bdl_e->ops->erase(bdl_e, (uint64_t)dst_block * bdl_e->geometry.erase_size,
                                 bdl_e->geometry.erase_size);
#else
        eret = nand_erase_block(dev_handle, dst_block);
#endif
        if (eret == ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "nvb_move_cb: bad block at dst block=%"PRIu32, dst_block);
            return -NVB_EFAULT;
        } else if (eret != ESP_OK) {
            ESP_LOGE(TAG, "nvb_move_cb: nand_erase_block FAILED block=%"PRIu32" ret=%d", dst_block, eret);
            return -NVB_EIO;
        }
    }

    // Use optimized HAL copy operation (or Flash BDL COPY_PAGE ioctl)
    esp_err_t ret;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(nvb_ctx->bdl_handle != NULL);
    esp_blockdev_handle_t bdl = nvb_ctx->bdl_handle;
    esp_blockdev_cmd_arg_copy_page_t copy_arg = {.src_page = pf, .dst_page = pt};
    ret = bdl->ops->ioctl(bdl, ESP_BLOCKDEV_CMD_COPY_PAGE, &copy_arg);
#else
    ret = nand_copy(dev_handle, pf, pt);
#endif
    if (ret == ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "nvb_move_cb: bad block on copy src=%"PRIu32" dst=%"PRIu32, pf, pt);
        return -NVB_EFAULT;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvb_move_cb: nand_copy FAILED src=%"PRIu32" dst=%"PRIu32" ret=%d", pf, pt, ret);
        return -NVB_EIO;
    }

    return 0;
}

/**
 * @brief nvblock is-bad callback - check if a virtual block is in a bad block
 * 
 * Check if the page belongs to a factory-marked bad block.
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

    bool is_bad = false;
    esp_err_t ret;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(nvb_ctx->bdl_handle != NULL);
    esp_blockdev_handle_t bdl = nvb_ctx->bdl_handle;
    esp_blockdev_cmd_arg_is_bad_block_t bad_st = {.num = block, .status = false};
    ret = bdl->ops->ioctl(bdl, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &bad_st);
    is_bad = bad_st.status;
#else
    ret = nand_is_bad(dev_handle, block, &is_bad);
#endif
    if (ret != ESP_OK) {
        // On error, assume bad for safety
        ESP_LOGW(TAG, "nvb_isbad_cb: nand_is_bad FAILED page=%"PRIu32
                 " block=%"PRIu32" ret=%d, assuming bad", p, block, ret);
        return true;
    }

    if (is_bad) {
        ESP_LOGW(TAG, "nvb_isbad_cb: page=%"PRIu32" block=%"PRIu32" is BAD", p, block);
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

    bool is_free = false;
    esp_err_t ret;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    (void)dev_handle; // suppress warning about unused variable
    assert(nvb_ctx->bdl_handle != NULL);
    esp_blockdev_handle_t bdl = nvb_ctx->bdl_handle;
    esp_blockdev_cmd_arg_is_free_page_t free_st = {.num = p, .status = true};
    ret = bdl->ops->ioctl(bdl, ESP_BLOCKDEV_CMD_IS_FREE_PAGE, &free_st);
    is_free = free_st.status;
#else
    ret = nand_is_free(dev_handle, p, &is_free);
#endif
    if (ret != ESP_OK) {
        // On error, assume not free
        ESP_LOGW(TAG, "nvb_isfree_cb: nand_is_free FAILED page=%"PRIu32
                 " ret=%d, assuming not free", p, ret);
        return false;
    }

    return is_free;
}

/**
 * @brief nvblock mark-bad callback - mark a virtual block's parent block as bad
 * 
 * Mark the NAND block containing this page as bad.
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

    ESP_LOGW(TAG, "nvb_markbad_cb: marking block %"PRIu32" as bad (page=%"PRIu32")", block, p);

#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(nvb_ctx->bdl_handle != NULL);
    esp_blockdev_handle_t bdl = nvb_ctx->bdl_handle;
    uint32_t blk = block;
    esp_err_t ret = bdl->ops->ioctl(bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &blk);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvb_markbad_cb: failed to mark block %"PRIu32" as bad: %d", block, ret);
    }
#else
    esp_err_t ret = nand_mark_bad(dev_handle, block);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvb_markbad_cb: failed to mark block %"PRIu32" as bad: %d", block, ret);
    }
#endif
}

// ============================================================================
// spi_nand_ops Interface Implementation
// ============================================================================

/**
 * @brief Initialize nvblock wear leveling layer
 *
 * Allocates the nvblock context, computes configuration parameters from the
 * chip descriptor, allocates the metadata buffer, wires up HAL callbacks, and
 * calls nvb_init() to scan/resume the on-flash wear leveling state.
 *
 * @param handle Device handle (chip params must be populated before calling)
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails,
 *         ESP_ERR_INVALID_ARG / ESP_FAIL if nvb_init fails
 */
static esp_err_t nvblock_init(spi_nand_flash_device_t *handle, void *bdl_handle)
{
    ESP_LOGI(TAG, "Initializing nvblock wear leveling");
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(bdl_handle != NULL);
#endif

    // Allocate nvblock context
    nvblock_context_t *nvb_ctx = calloc(1, sizeof(nvblock_context_t));
    if (!nvb_ctx) {
        ESP_LOGE(TAG, "Failed to allocate nvblock context");
        return ESP_ERR_NO_MEM;
    }

    handle->ops_priv_data = nvb_ctx;
    nvb_ctx->parent_handle = handle;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    nvb_ctx->bdl_handle = (esp_blockdev_handle_t)bdl_handle;
#endif

    // Calculate nvblock configuration from chip parameters.
    // Map nvblock terminology to NAND flash:
    //   bsize  (virtual block size)   = NAND page size (e.g. 2048 bytes)
    //   bpg    (blocks per group)     = NAND pages per block (e.g. 64)
    //   gcnt   (total groups)         = NAND total blocks (e.g. 1024)
    //   spgcnt (spare groups)         = derived from gc_factor percentage
    uint32_t bsize = handle->chip.page_size;
    uint32_t bpg = 1 << handle->chip.log2_ppb;
    uint32_t gcnt = handle->chip.num_blocks;

    // gc_factor is a percentage (e.g. 4 means 4% spare).
    // Minimum 2 spare groups for wear leveling to function correctly.
    uint32_t spgcnt = (gcnt * handle->config.gc_factor) / 100;
    if (spgcnt < 2) {
        spgcnt = 2;
    }

    ESP_LOGI(TAG, "nvblock config: bsize=%"PRIu32", bpg=%"PRIu32", gcnt=%"PRIu32", spgcnt=%"PRIu32,
             bsize, bpg, gcnt, spgcnt);

    // Allocate the metadata buffer.
    //
    // nvblock reads and writes full pages (bsize bytes) into the meta buffer
    // via its internal pb_read/pb_write functions. The buffer must be exactly
    // bsize bytes — NOT just the header + address-table size.
    //
    // The direct-map slot count (meta_dmmsk+1) is derived from bsize:
    //   meta_dmmsk = largest (2^n - 1) such that
    //   (bsize - NVB_META_DMP_START) >= 2 * (meta_dmmsk+1) * NVB_META_ADDRESS_SIZE
    // For bsize=2048 this requires ~1072 bytes for the DMP section alone,
    // far exceeding a naive bpg*NVB_META_ADDRESS_SIZE estimate (128 bytes).
    // Under-sizing the buffer causes a heap overflow on every metadata read.
    nvb_ctx->meta_buf_size = bsize;
    nvb_ctx->meta_buf = calloc(1, nvb_ctx->meta_buf_size);
    if (!nvb_ctx->meta_buf) {
        ESP_LOGE(TAG, "Failed to allocate metadata buffer (%zu bytes)", nvb_ctx->meta_buf_size);
        free(nvb_ctx);
        handle->ops_priv_data = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Allocated metadata buffer: %zu bytes", nvb_ctx->meta_buf_size);

    // Configure nvblock instance
    memset(&nvb_ctx->nvb_config, 0, sizeof(struct nvb_config));
    nvb_ctx->nvb_config.context = nvb_ctx;
    nvb_ctx->nvb_config.meta = nvb_ctx->meta_buf;
    nvb_ctx->nvb_config.bsize = bsize;
    nvb_ctx->nvb_config.bpg = bpg;
    nvb_ctx->nvb_config.gcnt = gcnt;
    nvb_ctx->nvb_config.spgcnt = spgcnt;

    // Wire up HAL callbacks (see callbacks section above for details)
    nvb_ctx->nvb_config.read = nvb_read_cb;
    nvb_ctx->nvb_config.prog = nvb_prog_cb;
    nvb_ctx->nvb_config.move = nvb_move_cb;
    nvb_ctx->nvb_config.is_bad = nvb_isbad_cb;
    nvb_ctx->nvb_config.is_free = nvb_isfree_cb;
    nvb_ctx->nvb_config.mark_bad = nvb_markbad_cb;
    nvb_ctx->nvb_config.sync = NULL;    // Optional: not needed (SPI NAND is synchronous)
    nvb_ctx->nvb_config.comp = NULL;    // Optional: compression not implemented
    nvb_ctx->nvb_config.init = NULL;    // Optional: no per-group init needed
    nvb_ctx->nvb_config.lock = NULL;    // Optional: thread locking handled by spi_nand_flash layer
    nvb_ctx->nvb_config.unlock = NULL;  // Optional: see lock above

    // Initialize nvblock — scans flash to build in-RAM state
    int ret = nvb_init(&nvb_ctx->nvb_info, &nvb_ctx->nvb_config);
    if (ret != 0) {
        ESP_LOGE(TAG, "nvb_init failed: %d", ret);
        free(nvb_ctx->meta_buf);
        free(nvb_ctx);
        handle->ops_priv_data = NULL;
        if (ret == -NVB_EINVAL) {
            return ESP_ERR_INVALID_ARG;
        } else if (ret == -NVB_ENOSPC) {
            return ESP_ERR_NO_MEM;
        } else {
            return ESP_FAIL;
        }
    }

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
 * 
 * Read logical sector via nvblock wear leveling layer.
 * nvblock manages logical-to-physical translation.
 * 
 * @param handle Device handle
 * @param buffer Output buffer (size = page_size = sector size)
 * @param sector_id Logical sector number
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t nvblock_read(spi_nand_flash_device_t *handle, uint8_t *buffer, uint32_t sector_id)
{
    ESP_LOGD(TAG, "nvblock_read: sector=%"PRIu32, sector_id);
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)handle->ops_priv_data;
    
    if (!nvb_ctx) {
        ESP_LOGE(TAG, "nvblock context not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // nvblock terminology: "block" = our sector (both are page_size)
    // Read 1 block starting at sector_id
    int ret = nvb_read(&nvb_ctx->nvb_info, buffer, sector_id, 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "nvblock_read FAILED: sector=%"PRIu32" nvb_ret=%d (head=%"PRIu32" gc_head=%"PRIu32")",
                 sector_id, ret, nvb_ctx->nvb_info.head, nvb_ctx->nvb_info.gc_head);
        return nvb_err_to_esp_read(ret);
    }
    
    return ESP_OK;
}

/**
 * @brief Write a sector through nvblock
 * 
 * Write logical sector via nvblock wear leveling layer.
 * nvblock handles wear leveling, bad block remapping, and physical allocation.
 * 
 * @param handle Device handle
 * @param buffer Input buffer (size = page_size = sector size)
 * @param sector_id Logical sector number
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t nvblock_write(spi_nand_flash_device_t *handle, const uint8_t *buffer, uint32_t sector_id)
{
    ESP_LOGD(TAG, "nvblock_write: sector=%"PRIu32, sector_id);
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)handle->ops_priv_data;
    
    if (!nvb_ctx) {
        ESP_LOGE(TAG, "nvblock context not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // nvblock terminology: "block" = our sector (both are page_size)
    // Write 1 block starting at sector_id
    int ret = nvb_write(&nvb_ctx->nvb_info, buffer, sector_id, 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "nvblock_write FAILED: sector=%"PRIu32" nvb_ret=%d (head=%"PRIu32" gc_head=%"PRIu32" epoch=%"PRIu32")",
                 sector_id, ret, nvb_ctx->nvb_info.head, nvb_ctx->nvb_info.gc_head, nvb_ctx->nvb_info.epoch);
        return nvb_err_to_esp_write(ret);
    }
    
    return ESP_OK;
}

/**
 * @brief Erase the entire chip
 *
 * Erase all physical blocks and reinitialize nvblock state from scratch.
 * Required when switching from Dhara to nvblock (or resetting the device),
 * because the on-flash metadata formats are incompatible.
 *
 * Note: nvblock does NOT have a separate erase callback. Block erasing is
 * handled internally inside nvb_prog_cb when writing the first page of a
 * group (pg_in_blk == 0). This function is only for a full chip wipe.
 *
 * @param handle Device handle
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t nvblock_erase_chip(spi_nand_flash_device_t *handle)
{
    ESP_LOGI(TAG, "nvblock_erase_chip");

#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    esp_blockdev_handle_t saved_bdl = NULL;
    nvblock_context_t *nvb_ctx_pre = (nvblock_context_t *)handle->ops_priv_data;
    if (nvb_ctx_pre) {
        saved_bdl = nvb_ctx_pre->bdl_handle;
    }
#endif

    // Erase all physical blocks via HAL
    esp_err_t ret = nand_erase_chip(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nand_erase_chip failed: %d", ret);
        return ret;
    }

    // Deinitialize and reinitialize nvblock
    nvblock_deinit(handle);
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    if (saved_bdl == NULL) {
        ESP_LOGE(TAG, "nvblock_erase_chip: BDL handle missing (nvblock not initialized?)");
        return ESP_ERR_INVALID_STATE;
    }
    return nvblock_init(handle, saved_bdl);
#else
    return nvblock_init(handle, NULL);
#endif
}

/**
 * @brief Erase a logical block (not typically used with wear leveling)
 * 
 * This function is provided for compatibility but direct block erase
 * is not recommended with wear leveling. Use trim instead.
 * 
 * @param handle Device handle
 * @param block Block number (physical, not logical)
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t nvblock_erase_block(spi_nand_flash_device_t *handle, uint32_t block)
{
    ESP_LOGD(TAG, "nvblock_erase_block: block=%"PRIu32, block);
    // Direct physical block erase (bypasses wear leveling)
    return nand_erase_block(handle, block);
}

/**
 * @brief Trim a sector (mark as no longer needed)
 * 
 * Trim a sector (mark as no longer needed) using nvb_write with NULL data.
 * According to nvblock documentation, writing NULL deletes blocks.
 * 
 * @param handle Device handle
 * @param sector_id Logical sector number
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t nvblock_trim(spi_nand_flash_device_t *handle, uint32_t sector_id)
{
    ESP_LOGD(TAG, "nvblock_trim: sector=%"PRIu32, sector_id);
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)handle->ops_priv_data;
    
    if (!nvb_ctx) {
        ESP_LOGE(TAG, "nvblock context not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // nvblock trim: write NULL data to delete the block
    int ret = nvb_write(&nvb_ctx->nvb_info, NULL, sector_id, 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "nvb_write(NULL) failed for sector %"PRIu32": %d", sector_id, ret);
        return nvb_err_to_esp_write(ret);
    }
    
    return ESP_OK;
}

/**
 * @brief Sync/flush pending operations
 * 
 * Flush nvblock state to ensure persistence.
 * Uses nvblock's IOCTL interface with NVB_CMD_CTRL_SYNC command.
 * 
 * @param handle Device handle
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t nvblock_sync(spi_nand_flash_device_t *handle)
{
    ESP_LOGD(TAG, "nvblock_sync");
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)handle->ops_priv_data;
    
    if (!nvb_ctx) {
        ESP_LOGE(TAG, "nvblock context not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Use nvblock IOCTL to sync/flush
    int ret = nvb_ioctl(&nvb_ctx->nvb_info, NVB_CMD_CTRL_SYNC, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "nvb_ioctl(NVB_CMD_CTRL_SYNC) failed: %d", ret);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

/**
 * @brief Copy a sector
 * 
 * Copy data from one logical sector to another.
 * Implemented as read + write operation.
 * 
 * @param handle Device handle
 * @param src_sec Source sector ID
 * @param dst_sec Destination sector ID
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t nvblock_copy_sector(spi_nand_flash_device_t *handle, uint32_t src_sec, uint32_t dst_sec)
{
    ESP_LOGD(TAG, "nvblock_copy_sector: src=%"PRIu32", dst=%"PRIu32, src_sec, dst_sec);
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)handle->ops_priv_data;
    
    if (!nvb_ctx) {
        ESP_LOGE(TAG, "nvblock context not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Allocate temporary buffer for sector data
    uint8_t *temp_buf = malloc(handle->chip.page_size);
    if (!temp_buf) {
        ESP_LOGE(TAG, "Failed to allocate temp buffer for copy");
        return ESP_ERR_NO_MEM;
    }
    
    // Read source sector
    int ret = nvb_read(&nvb_ctx->nvb_info, temp_buf, src_sec, 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "nvb_read failed for src sector %"PRIu32": %d", src_sec, ret);
        free(temp_buf);
        return nvb_err_to_esp_read(ret);
    }
    
    // Write to destination sector
    ret = nvb_write(&nvb_ctx->nvb_info, temp_buf, dst_sec, 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "nvb_write failed for dst sector %"PRIu32": %d", dst_sec, ret);
        free(temp_buf);
        return nvb_err_to_esp_write(ret);
    }
    
    free(temp_buf);
    return ESP_OK;
}

/**
 * @brief Get capacity in sectors
 * 
 * Query nvblock capacity and convert to sector count.
 * Uses nvblock's IOCTL interface with NVB_CMD_GET_BLK_COUNT command.
 * 
 * @param handle Device handle
 * @param number_of_sectors Output: total number of logical sectors
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t nvblock_get_capacity(spi_nand_flash_device_t *handle, uint32_t *number_of_sectors)
{
    ESP_LOGD(TAG, "nvblock_get_capacity");
    nvblock_context_t *nvb_ctx = (nvblock_context_t *)handle->ops_priv_data;
    
    if (!nvb_ctx) {
        ESP_LOGE(TAG, "nvblock context not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!number_of_sectors) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Query nvblock for total block count
    uint32_t block_count = 0;
    int ret = nvb_ioctl(&nvb_ctx->nvb_info, NVB_CMD_GET_BLK_COUNT, &block_count);
    if (ret != 0) {
        ESP_LOGE(TAG, "nvb_ioctl(NVB_CMD_GET_BLK_COUNT) failed: %d", ret);
        return ESP_FAIL;
    }
    
    // In our implementation: 1 nvblock "block" = 1 sector (both are page_size)
    *number_of_sectors = block_count;
    ESP_LOGD(TAG, "nvblock capacity: %"PRIu32" sectors", *number_of_sectors);
    
    return ESP_OK;
}

// ============================================================================
// nvblock Operations Table Export
// ============================================================================

const spi_nand_ops nvblock_ops = {
    .init = &nvblock_init,
    .deinit = &nvblock_deinit,
    .read = &nvblock_read,
    .write = &nvblock_write,
    .erase_chip = &nvblock_erase_chip,
    .erase_block = &nvblock_erase_block,
    .trim = &nvblock_trim,
    .sync = &nvblock_sync,
    .copy_sector = &nvblock_copy_sector,
    .get_capacity = &nvblock_get_capacity,
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
    // Use nvblock_deinit to properly free all nested allocations (meta_buf, context)
    nvblock_deinit(handle);
    handle->ops = NULL;
    return ESP_OK;
}
