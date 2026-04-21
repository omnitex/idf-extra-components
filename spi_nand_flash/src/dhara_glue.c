/*
 * SPDX-FileCopyrightText: 2022 mikkeldamsgaard project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPDX-FileContributor: 2015-2025 Espressif Systems (Shanghai) CO LTD
 */

#include <string.h>
#include <sys/lock.h>
#include "dhara/nand.h"
#include "dhara/map.h"
#include "dhara/error.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#ifndef CONFIG_IDF_TARGET_LINUX
#include "spi_nand_oper.h"
#include "esp_heap_caps.h"
#endif
#include "nand_impl.h"
#include "nand.h"
#include "nand_device_types.h"
#include "ecc_relief_map.h"

#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
#include "esp_nand_blockdev.h"
#endif

static const char *TAG = "dhara_glue";

/*===========================================================================
 * ECC Relief Map — private implementation
 *===========================================================================*/

typedef struct {
    struct dhara_nand dhara_nand;
    struct dhara_map dhara_map;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    esp_blockdev_handle_t bdl_handle;
#endif
    spi_nand_flash_device_t *parent_handle;

    /* ECC relief map (NULL when feature is disabled) */
    ecc_relief_entry_t *ecc_relief_map;
    uint16_t            ecc_relief_capacity;   /* power of 2 */

    /* Runtime statistics */
    uint32_t            stat_pages_relieved;
    uint32_t            stat_consecutive_cap_hits;
} spi_nand_flash_dhara_priv_data_t;

/* Hash function: capacity must be a power of 2 */
static inline uint16_t relief_hash(dhara_page_t page, uint16_t capacity)
{
    return (uint16_t)(page & (capacity - 1u));
}

/**
 * Lookup an entry for @p page.  Returns a pointer to the slot (may be
 * TOMBSTONE or INVALID if not found; caller must check).
 * Returns NULL if map is empty or page not found (slot is INVALID_PAGE).
 */
static ecc_relief_entry_t *relief_map_lookup(spi_nand_flash_dhara_priv_data_t *priv,
                                              dhara_page_t page)
{
    uint16_t cap = priv->ecc_relief_capacity;
    uint16_t idx = relief_hash(page, cap);

    for (uint16_t i = 0; i < cap; i++) {
        ecc_relief_entry_t *e = &priv->ecc_relief_map[(idx + i) & (cap - 1u)];
        if (e->page == ECC_RELIEF_INVALID_PAGE) {
            return NULL;  /* not found — empty slot terminates probe */
        }
        if (e->page == ECC_RELIEF_TOMBSTONE_PAGE) {
            continue;     /* skip tombstones, keep probing */
        }
        if (e->page == page) {
            return e;
        }
    }
    return NULL;
}

/**
 * Find a slot suitable for insertion (INVALID or TOMBSTONE) starting
 * from the hash position.
 */
static ecc_relief_entry_t *relief_map_find_insert_slot(spi_nand_flash_dhara_priv_data_t *priv,
                                                        dhara_page_t page)
{
    uint16_t cap = priv->ecc_relief_capacity;
    uint16_t idx = relief_hash(page, cap);
    ecc_relief_entry_t *tombstone_slot = NULL;

    for (uint16_t i = 0; i < cap; i++) {
        ecc_relief_entry_t *e = &priv->ecc_relief_map[(idx + i) & (cap - 1u)];
        if (e->page == ECC_RELIEF_INVALID_PAGE) {
            return tombstone_slot ? tombstone_slot : e;
        }
        if (e->page == ECC_RELIEF_TOMBSTONE_PAGE) {
            if (!tombstone_slot) {
                tombstone_slot = e;  /* remember first tombstone for reuse */
            }
            continue;
        }
        if (e->page == page) {
            return e;  /* already exists — update in place */
        }
    }
    return tombstone_slot;  /* map full of tombstones (pathological) */
}

/**
 * Set ECC_RELIEF_FLAG_PENDING for a page.  Inserts if not present.
 * If map is full (no INVALID/TOMBSTONE slot), evict a non-PENDING
 * non-tombstone entry.
 */
static void relief_map_flag(spi_nand_flash_dhara_priv_data_t *priv, dhara_page_t page)
{
    ecc_relief_entry_t *slot = relief_map_find_insert_slot(priv, page);
    if (slot) {
        slot->page = page;
        slot->flags |= ECC_RELIEF_FLAG_PENDING;
        return;
    }

    /* Map is full — evict a non-PENDING non-tombstone entry */
    uint16_t cap = priv->ecc_relief_capacity;
    for (uint16_t i = 0; i < cap; i++) {
        ecc_relief_entry_t *e = &priv->ecc_relief_map[i];
        if (e->page != ECC_RELIEF_INVALID_PAGE &&
            e->page != ECC_RELIEF_TOMBSTONE_PAGE &&
            !(e->flags & ECC_RELIEF_FLAG_PENDING)) {
            e->page = page;
            e->mid_count = 0;
            e->flags = ECC_RELIEF_FLAG_PENDING;
            return;
        }
    }
    ESP_LOGW(TAG, "ECC relief map full — unable to flag page %lu", (unsigned long)page);
}

/**
 * Increment mid_count for @p page.  If it reaches the limit, flag PENDING.
 */
static void relief_map_increment(spi_nand_flash_dhara_priv_data_t *priv,
                                  dhara_page_t page,
                                  uint8_t mid_count_limit)
{
    ecc_relief_entry_t *e = relief_map_lookup(priv, page);
    if (!e) {
        /* Insert with mid_count=1 */
        ecc_relief_entry_t *slot = relief_map_find_insert_slot(priv, page);
        if (!slot) {
            return;  /* map full, silently skip */
        }
        slot->page = page;
        slot->mid_count = 1;
        slot->flags = 0;
        if (mid_count_limit <= 1) {
            slot->flags |= ECC_RELIEF_FLAG_PENDING;
        }
        return;
    }

    if (e->mid_count < 0xFF) {
        e->mid_count++;
    }
    if (e->mid_count >= mid_count_limit) {
        e->flags |= ECC_RELIEF_FLAG_PENDING;
    }
}

/**
 * Clear ECC_RELIEF_FLAG_PENDING for @p page (after relief has been applied).
 * Entry remains for mid_count bookkeeping.
 */
static void relief_map_clear_pending(spi_nand_flash_dhara_priv_data_t *priv,
                                      dhara_page_t page)
{
    ecc_relief_entry_t *e = relief_map_lookup(priv, page);
    if (e) {
        e->flags &= ~ECC_RELIEF_FLAG_PENDING;
    }
}

/**
 * Evict (tombstone) all entries whose page falls in [first_page, first_page+count).
 * Called after a successful block erase.
 */
static void relief_map_evict_range(spi_nand_flash_dhara_priv_data_t *priv,
                                    dhara_page_t first_page,
                                    dhara_page_t count)
{
    uint16_t cap = priv->ecc_relief_capacity;
    for (uint16_t i = 0; i < cap; i++) {
        ecc_relief_entry_t *e = &priv->ecc_relief_map[i];
        if (e->page == ECC_RELIEF_INVALID_PAGE ||
            e->page == ECC_RELIEF_TOMBSTONE_PAGE) {
            continue;
        }
        if (e->page >= first_page && e->page < (first_page + count)) {
            e->page = ECC_RELIEF_TOMBSTONE_PAGE;
            e->mid_count = 0;
            e->flags = 0;
        }
    }
}

/**
 * Relief check callback registered with the Dhara journal.
 * Returns 1 if the page should be relieved (PENDING flag set), 0 otherwise.
 * Clears PENDING flag and increments stat_pages_relieved on relief.
 */
static int dhara_relief_check_cb(dhara_page_t page, void *ctx)
{
    spi_nand_flash_dhara_priv_data_t *priv = ctx;
    ecc_relief_entry_t *e = relief_map_lookup(priv, page);
    if (e && (e->flags & ECC_RELIEF_FLAG_PENDING)) {
        relief_map_clear_pending(priv, page);
        priv->stat_pages_relieved++;
        return 1;
    }
    return 0;
}

/**
 * ECC read callback registered on handle->on_page_read_ecc.
 * Classifies the ECC status and updates the relief map accordingly.
 */
static void dhara_ecc_read_cb(uint32_t page, nand_ecc_status_t status, void *ctx)
{
    spi_nand_flash_dhara_priv_data_t *priv = ctx;
    const struct {
        uint8_t mid;
        uint8_t high;
        uint8_t mid_count_limit;
    } cfg = {
        .mid = priv->parent_handle->config.ecc_relief.mid_threshold,
        .high = priv->parent_handle->config.ecc_relief.high_threshold,
        .mid_count_limit = priv->parent_handle->config.ecc_relief.mid_count_limit,
    };

    if (status >= cfg.high) {
        relief_map_flag(priv, (dhara_page_t)page);
    } else if (cfg.mid_count_limit > 0 && status >= cfg.mid) {
        relief_map_increment(priv, (dhara_page_t)page, cfg.mid_count_limit);
    }
}

/*===========================================================================
 * Dhara glue operations
 *===========================================================================*/

static esp_err_t dhara_init(spi_nand_flash_device_t *handle, void *bdl_handle)
{
    // create a holder structure for dhara context
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = heap_caps_calloc(1, sizeof(spi_nand_flash_dhara_priv_data_t), MALLOC_CAP_DEFAULT);
    if (dhara_priv_data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    handle->ops_priv_data = dhara_priv_data;
    // store the pointer back to device structure in the holder structure
    dhara_priv_data->parent_handle = handle;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    dhara_priv_data->bdl_handle = (esp_blockdev_handle_t)bdl_handle;
#endif

    dhara_priv_data->dhara_nand.log2_page_size = handle->chip.log2_page_size;
    dhara_priv_data->dhara_nand.log2_ppb = handle->chip.log2_ppb;
    dhara_priv_data->dhara_nand.num_blocks = handle->chip.num_blocks;

    dhara_map_init(&dhara_priv_data->dhara_map, &dhara_priv_data->dhara_nand, handle->work_buffer, handle->config.gc_factor);
    dhara_error_t ignored;
    dhara_map_resume(&dhara_priv_data->dhara_map, &ignored);

    /* ECC relief feature init */
    if (handle->config.ecc_relief.enabled) {
        uint16_t cap = handle->config.ecc_relief.map_capacity;
        if (cap == 0) {
            cap = CONFIG_NAND_FLASH_ECC_RELIEF_DEFAULT_MAP_CAPACITY;
        }
        /* Capacity must be a power of 2 */
        if (cap & (cap - 1)) {
            ESP_LOGW(TAG, "ecc_relief.map_capacity (%u) is not a power of 2 — rounding up", cap);
            uint16_t p = 1;
            while (p < cap) p <<= 1;
            cap = p;
        }

        dhara_priv_data->ecc_relief_map = heap_caps_calloc(cap,
                                                            sizeof(ecc_relief_entry_t),
                                                            MALLOC_CAP_INTERNAL);
        if (!dhara_priv_data->ecc_relief_map) {
            ESP_LOGW(TAG, "Failed to allocate ECC relief map — feature disabled");
        } else {
            dhara_priv_data->ecc_relief_capacity = cap;

            /* Mark all slots as empty */
            for (uint16_t i = 0; i < cap; i++) {
                dhara_priv_data->ecc_relief_map[i].page = ECC_RELIEF_INVALID_PAGE;
            }

            /* Wire ECC observation into nand_read() for all callers */
            handle->on_page_read_ecc     = dhara_ecc_read_cb;
            handle->on_page_read_ecc_ctx = dhara_priv_data;

            /* Wire relief decision into Dhara's journal */
            dhara_journal_set_relief_hook(&dhara_priv_data->dhara_map.journal,
                                          dhara_relief_check_cb,
                                          dhara_priv_data,
                                          handle->config.ecc_relief.max_consecutive_relief);
        }
    }

    return ESP_OK;
}

static esp_err_t dhara_deinit(spi_nand_flash_device_t *handle)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = (spi_nand_flash_dhara_priv_data_t *)handle->ops_priv_data;

    /* Deinit ECC relief feature */
    if (dhara_priv_data->ecc_relief_map) {
        free(dhara_priv_data->ecc_relief_map);
        dhara_priv_data->ecc_relief_map = NULL;
        dhara_priv_data->ecc_relief_capacity = 0;
    }
    handle->on_page_read_ecc     = NULL;
    handle->on_page_read_ecc_ctx = NULL;

    // clear dhara map
    dhara_map_init(&dhara_priv_data->dhara_map, &dhara_priv_data->dhara_nand, handle->work_buffer, handle->config.gc_factor);
    dhara_map_clear(&dhara_priv_data->dhara_map);
    return ESP_OK;
}

static esp_err_t dhara_read(spi_nand_flash_device_t *handle, uint8_t *buffer, dhara_sector_t sector_id)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = (spi_nand_flash_dhara_priv_data_t *)handle->ops_priv_data;
    dhara_error_t err;
    if (dhara_map_read(&dhara_priv_data->dhara_map, sector_id, handle->read_buffer, &err)) {
        return ESP_ERR_FLASH_BASE + err;
    }
    memcpy(buffer, handle->read_buffer, handle->chip.page_size);
    return ESP_OK;
}

static esp_err_t dhara_write(spi_nand_flash_device_t *handle, const uint8_t *buffer, dhara_sector_t sector_id)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = (spi_nand_flash_dhara_priv_data_t *)handle->ops_priv_data;
    dhara_error_t err;
    if (dhara_map_write(&dhara_priv_data->dhara_map, sector_id, buffer, &err)) {
        return ESP_ERR_FLASH_BASE + err;
    }
    return ESP_OK;
}

static esp_err_t dhara_copy_sector(spi_nand_flash_device_t *handle, dhara_sector_t src_sec, dhara_sector_t dst_sec)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = (spi_nand_flash_dhara_priv_data_t *)handle->ops_priv_data;
    dhara_error_t err;
    if (dhara_map_copy_sector(&dhara_priv_data->dhara_map, src_sec, dst_sec, &err)) {
        return ESP_ERR_FLASH_BASE + err;
    }
    return ESP_OK;
}

static esp_err_t dhara_trim(spi_nand_flash_device_t *handle, dhara_sector_t sector_id)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = (spi_nand_flash_dhara_priv_data_t *)handle->ops_priv_data;
    dhara_error_t err;
    if (dhara_map_trim(&dhara_priv_data->dhara_map, sector_id, &err)) {
        return ESP_ERR_FLASH_BASE + err;
    }
    return ESP_OK;
}

static esp_err_t dhara_sync(spi_nand_flash_device_t *handle)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = (spi_nand_flash_dhara_priv_data_t *)handle->ops_priv_data;
    dhara_error_t err;
    if (dhara_map_sync(&dhara_priv_data->dhara_map, &err)) {
        return ESP_ERR_FLASH_BASE + err;
    }
    return ESP_OK;
}

static esp_err_t dhara_get_capacity(spi_nand_flash_device_t *handle, dhara_sector_t *number_of_sectors)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = (spi_nand_flash_dhara_priv_data_t *)handle->ops_priv_data;
    *number_of_sectors = dhara_map_capacity(&dhara_priv_data->dhara_map);
    return ESP_OK;
}

static esp_err_t dhara_gc(spi_nand_flash_device_t *handle)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = (spi_nand_flash_dhara_priv_data_t *)handle->ops_priv_data;
    dhara_error_t err;
    if (dhara_map_gc(&dhara_priv_data->dhara_map, &err)) {
        return ESP_ERR_FLASH_BASE + err;
    }
    return ESP_OK;
}

static esp_err_t dhara_erase_chip(spi_nand_flash_device_t *handle)
{
    return nand_erase_chip(handle);
}

static esp_err_t dhara_erase_block(spi_nand_flash_device_t *handle, uint32_t block)
{
    return nand_erase_block(handle, block);
}


const spi_nand_ops dhara_nand_ops = {
    .init = &dhara_init,
    .deinit = &dhara_deinit,
    .read = &dhara_read,
    .write = &dhara_write,
    .erase_chip = &dhara_erase_chip,
    .erase_block = &dhara_erase_block,
    .trim = &dhara_trim,
    .sync = &dhara_sync,
    .copy_sector = &dhara_copy_sector,
    .get_capacity = &dhara_get_capacity,
    .gc = &dhara_gc,
};

esp_err_t nand_wl_attach_ops(spi_nand_flash_device_t *handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->ops = &dhara_nand_ops;
    return ESP_OK;
}

esp_err_t nand_wl_detach_ops(spi_nand_flash_device_t *handle)
{
    free(handle->ops_priv_data);
    handle->ops = NULL;
    return ESP_OK;
}

/*------------------------------------------------------------------------------------------------------*/


// The following APIs are implementations required by the Dhara library.
// Please refer to the header file dhara/nand.h for details.

int dhara_nand_read(const struct dhara_nand *n, dhara_page_t p, size_t offset, size_t length,
                    uint8_t *data, dhara_error_t *err)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
    spi_nand_flash_device_t *dev_handle = NULL;
    esp_err_t ret = ESP_OK;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(dhara_priv_data->bdl_handle != NULL);
    esp_blockdev_handle_t bdl_handle = dhara_priv_data->bdl_handle;
    dev_handle = (spi_nand_flash_device_t *)bdl_handle->ctx;
    ret = bdl_handle->ops->read(bdl_handle, data, length,
                                (p * bdl_handle->geometry.read_size) + offset, length);
#else
    dev_handle = dhara_priv_data->parent_handle;
    ret = nand_read(dev_handle, p, offset, length, data);
#endif
    if (ret != ESP_OK) {
        if (dev_handle->chip.ecc_data.ecc_corrected_bits_status == NAND_ECC_NOT_CORRECTED) {
            dhara_set_error(err, DHARA_E_ECC);
        }
        return -1;
    }
    return 0;
}

int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p, const uint8_t *data, dhara_error_t *err)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
    esp_err_t ret = ESP_OK;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(dhara_priv_data->bdl_handle != NULL);
    esp_blockdev_handle_t bdl_handle = dhara_priv_data->bdl_handle;
    ret = bdl_handle->ops->write(bdl_handle, data, (p * bdl_handle->geometry.write_size),
                                 bdl_handle->geometry.write_size);
#else
    spi_nand_flash_device_t *dev_handle = dhara_priv_data->parent_handle;
    ret = nand_prog(dev_handle, p, data);
#endif
    if (ret) {
        if (ret == ESP_ERR_NOT_FINISHED) {
            dhara_set_error(err, DHARA_E_BAD_BLOCK);
        }
        return -1;
    }
    return 0;
}

int dhara_nand_erase(const struct dhara_nand *n, dhara_block_t b, dhara_error_t *err)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
    esp_err_t ret = ESP_OK;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(dhara_priv_data->bdl_handle != NULL);
    esp_blockdev_handle_t bdl_handle = dhara_priv_data->bdl_handle;
    ret = bdl_handle->ops->erase(bdl_handle, b * bdl_handle->geometry.erase_size,
                                 bdl_handle->geometry.erase_size);
#else
    spi_nand_flash_device_t *dev_handle = dhara_priv_data->parent_handle;
    ret = nand_erase_block(dev_handle, b);
#endif
    if (ret) {
        if (ret == ESP_ERR_NOT_FINISHED) {
            dhara_set_error(err, DHARA_E_BAD_BLOCK);
        }
        return -1;
    }

    /* On successful erase, evict all relief map entries for this block's pages */
    if (dhara_priv_data->ecc_relief_map) {
        dhara_page_t first = (dhara_page_t)b << n->log2_ppb;
        dhara_page_t count = (dhara_page_t)1 << n->log2_ppb;
        relief_map_evict_range(dhara_priv_data, first, count);
    }

    return 0;
}

int dhara_nand_is_bad(const struct dhara_nand *n, dhara_block_t b)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
    bool is_bad_status = false;
    esp_err_t ret = ESP_OK;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(dhara_priv_data->bdl_handle != NULL);
    esp_blockdev_handle_t bdl_handle = dhara_priv_data->bdl_handle;
    esp_blockdev_cmd_arg_is_bad_block_t bad_block_status = {b, false};
    ret = bdl_handle->ops->ioctl(bdl_handle, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &bad_block_status);
    is_bad_status = bad_block_status.status;
#else
    spi_nand_flash_device_t *dev_handle = dhara_priv_data->parent_handle;
    ret = nand_is_bad(dev_handle, b, &is_bad_status);
#endif
    if (ret || is_bad_status == true) {
        return 1;
    }
    return 0;
}

void dhara_nand_mark_bad(const struct dhara_nand *n, dhara_block_t b)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(dhara_priv_data->bdl_handle != NULL);
    esp_blockdev_handle_t bdl_handle = dhara_priv_data->bdl_handle;
    uint32_t block = b;
    bdl_handle->ops->ioctl(bdl_handle, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &block);
#else
    spi_nand_flash_device_t *dev_handle = dhara_priv_data->parent_handle;
    nand_mark_bad(dev_handle, b);
#endif
    return;
}

int dhara_nand_is_free(const struct dhara_nand *n, dhara_page_t p)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
    bool is_free_status = true;
    esp_err_t ret = ESP_OK;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(dhara_priv_data->bdl_handle != NULL);
    esp_blockdev_handle_t bdl_handle = dhara_priv_data->bdl_handle;
    esp_blockdev_cmd_arg_is_free_page_t page_free_status = {p, true};
    ret = bdl_handle->ops->ioctl(bdl_handle, ESP_BLOCKDEV_CMD_IS_FREE_PAGE, &page_free_status);
    is_free_status = page_free_status.status;
#else
    spi_nand_flash_device_t *dev_handle = dhara_priv_data->parent_handle;
    ret = nand_is_free(dev_handle, p, &is_free_status);
#endif

    if (ret != ESP_OK) {
        return 0;
    }
    if (is_free_status == true) {
        return 1;
    }
    return 0;
}

int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src, dhara_page_t dst, dhara_error_t *err)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
    spi_nand_flash_device_t *dev_handle = NULL;
    esp_err_t ret = ESP_OK;

#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(dhara_priv_data->bdl_handle != NULL);
    esp_blockdev_handle_t bdl_handle = dhara_priv_data->bdl_handle;
    dev_handle = (spi_nand_flash_device_t *)bdl_handle->ctx;
    esp_blockdev_cmd_arg_copy_page_t copy_arg = {
        .src_page = src,
        .dst_page = dst
    };
    ret = dhara_priv_data->bdl_handle->ops->ioctl(bdl_handle, ESP_BLOCKDEV_CMD_COPY_PAGE, &copy_arg);
#else
    dev_handle = dhara_priv_data->parent_handle;
    ret = nand_copy(dev_handle, src, dst);
#endif
    if (ret) {
        if (dev_handle->chip.ecc_data.ecc_corrected_bits_status == NAND_ECC_NOT_CORRECTED) {
            dhara_set_error(err, DHARA_E_ECC);
        }
        if (ret == ESP_ERR_NOT_FINISHED) {
            dhara_set_error(err, DHARA_E_BAD_BLOCK);
        }
        return -1;
    }
    return 0;
}

/*===========================================================================
 * Diagnostic API
 *===========================================================================*/

esp_err_t spi_nand_flash_get_ecc_relief_stats(spi_nand_flash_device_t *handle,
                                               spi_nand_ecc_relief_stats_t *stats)
{
    if (!handle || !stats) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->config.ecc_relief.enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    spi_nand_flash_dhara_priv_data_t *priv = (spi_nand_flash_dhara_priv_data_t *)handle->ops_priv_data;
    if (!priv || !priv->ecc_relief_map) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint16_t cap = priv->ecc_relief_capacity;
    uint32_t pending = 0;
    uint32_t used = 0;

    for (uint16_t i = 0; i < cap; i++) {
        const ecc_relief_entry_t *e = &priv->ecc_relief_map[i];
        if (e->page == ECC_RELIEF_INVALID_PAGE ||
            e->page == ECC_RELIEF_TOMBSTONE_PAGE) {
            continue;
        }
        used++;
        if (e->flags & ECC_RELIEF_FLAG_PENDING) {
            pending++;
        }
    }

    stats->pages_pending_relief  = pending;
    stats->total_pages_relieved  = priv->stat_pages_relieved;
    stats->map_entries_used      = used;
    stats->map_capacity          = cap;
    stats->consecutive_cap_hits  = priv->stat_consecutive_cap_hits;

    return ESP_OK;
}
