/*
 * SPDX-FileCopyrightText: 2022 mikkeldamsgaard project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPDX-FileContributor: 2015-2026 Espressif Systems (Shanghai) CO LTD
 */

#include <string.h>
#include <inttypes.h>
#include <sys/lock.h>
#include "dhara/nand.h"
#include "dhara/map.h"
#include "dhara/journal.h"
#include "dhara/error.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#ifndef CONFIG_IDF_TARGET_LINUX
#include "spi_nand_oper.h"
#endif
#include "nand_impl.h"
#include "nand_oob.h"
#include "nand.h"
#include "nand_device_types.h"

#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
#include "esp_nand_blockdev.h"
#endif

static const char *TAG = "dhara_glue";

/* Any page-read failure — ECC corruption, bus error, or timeout — maps to
 * DHARA_E_ECC because dhara's recovery path ("treat this page as untrustworthy
 * and recover") is correct regardless of the root cause.  If dhara ever adds a
 * dedicated DHARA_E_IO code, change only this definition. */
#define DHARA_E_PAGE_UNREADABLE  DHARA_E_ECC

typedef struct {
    struct dhara_nand dhara_nand;
    struct dhara_map dhara_map;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    esp_blockdev_handle_t bdl_handle;
#endif
    spi_nand_flash_device_t *parent_handle;
} spi_nand_flash_dhara_priv_data_t;

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

    return ESP_OK;
}

static esp_err_t dhara_deinit(spi_nand_flash_device_t *handle)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = (spi_nand_flash_dhara_priv_data_t *)handle->ops_priv_data;
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

    const struct dhara_journal *j = &dhara_priv_data->dhara_map.journal;
    if (dhara_journal_is_clean(j)) {
        ESP_LOGD(TAG, "sync: journal clean (no-op)");
    } else {
        const uint32_t ppc            = 1u << j->log2_ppc;
        const uint32_t num_user_slots = ppc - 1u;
        const uint32_t group_offset   = (uint32_t)j->head & (ppc - 1u);
        const uint32_t remaining_slots = num_user_slots - group_offset;
        ESP_LOGD(TAG, "sync: dirty head=%" PRIu32 " ppc=%" PRIu32
                 " slot=%" PRIu32 "/%" PRIu32 " remaining_slots=%" PRIu32,
                 (uint32_t)j->head, ppc, group_offset, num_user_slots - 1u, remaining_slots);
    }

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

int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p, const uint8_t *data,
                    dhara_sector_t oob_lpn, dhara_error_t *err)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
    esp_err_t ret = ESP_OK;
    uint8_t lpn_buf[4];
    uint16_t oob_len = 0;
    if (oob_lpn != DHARA_OOB_LPN_NONE) {
        nand_oob_pack_lpn_le(oob_lpn, lpn_buf);
        oob_len = 4;
    }
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(dhara_priv_data->bdl_handle != NULL);
    esp_blockdev_handle_t bdl_handle = dhara_priv_data->bdl_handle;
    esp_blockdev_cmd_arg_prog_page_ext_t prog_arg = {
        .page_num   = p,
        .data       = data,
        .oob_offset = CONFIG_NAND_FLASH_OOB_LPN_OFFSET,
        .oob_len    = oob_len,
        .oob_data   = oob_len ? lpn_buf : NULL,
    };
    ret = bdl_handle->ops->ioctl(bdl_handle, ESP_BLOCKDEV_CMD_PROG_PAGE_EXT, &prog_arg);
#else
    spi_nand_flash_device_t *dev_handle = dhara_priv_data->parent_handle;
    ret = nand_prog_ext(dev_handle, p, data,
                        CONFIG_NAND_FLASH_OOB_LPN_OFFSET, oob_len,
                        oob_len ? lpn_buf : NULL);
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

int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src, dhara_page_t dst,
                    dhara_sector_t oob_lpn, dhara_error_t *err)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
    spi_nand_flash_device_t *dev_handle = NULL;
    esp_err_t ret = ESP_OK;
    uint8_t lpn_buf[4];
    uint16_t oob_len = 0;
    if (oob_lpn != DHARA_OOB_LPN_NONE) {
        nand_oob_pack_lpn_le(oob_lpn, lpn_buf);
        oob_len = 4;
    }

#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    assert(dhara_priv_data->bdl_handle != NULL);
    dev_handle = (spi_nand_flash_device_t *)dhara_priv_data->bdl_handle->ctx;
    esp_blockdev_handle_t bdl_handle = dhara_priv_data->bdl_handle;
    esp_blockdev_cmd_arg_copy_page_ext_t copy_arg = {
        .src_page   = src,
        .dst_page   = dst,
        .oob_offset = CONFIG_NAND_FLASH_OOB_LPN_OFFSET,
        .oob_len    = oob_len,
        .oob_data   = oob_len ? lpn_buf : NULL,
    };
    ret = bdl_handle->ops->ioctl(bdl_handle, ESP_BLOCKDEV_CMD_COPY_PAGE_EXT, &copy_arg);
#else
    dev_handle = dhara_priv_data->parent_handle;
    ret = nand_copy_ext(dev_handle, src, dst,
                        CONFIG_NAND_FLASH_OOB_LPN_OFFSET, oob_len,
                        oob_len ? lpn_buf : NULL);
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

int dhara_nand_read_lpn(const struct dhara_nand *n, dhara_page_t p,
                        dhara_sector_t *oob_lpn_out, dhara_error_t *err)
{
    spi_nand_flash_dhara_priv_data_t *dhara_priv_data = __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    if (dhara_priv_data->bdl_handle == NULL) {
        dhara_set_error(err, DHARA_E_PAGE_UNREADABLE);
        return -1;
    }
    {
        esp_blockdev_handle_t bdl_handle = dhara_priv_data->bdl_handle;
        esp_blockdev_cmd_arg_read_page_lpn_t read_arg = { .page_num = p, .oob_lpn = ESP_BLOCKDEV_LPN_NONE };
        esp_err_t ret = bdl_handle->ops->ioctl(bdl_handle, ESP_BLOCKDEV_CMD_READ_PAGE_LPN, &read_arg);
        if (ret != ESP_OK) {
            dhara_set_error(err, DHARA_E_PAGE_UNREADABLE);
            return -1;
        }
        *oob_lpn_out = read_arg.oob_lpn;
    }
    return 0;
#else
    spi_nand_flash_device_t *dev_handle = dhara_priv_data->parent_handle;
    esp_err_t ret = nand_read_lpn(dev_handle, p, oob_lpn_out);
    if (ret != ESP_OK) {
        dhara_set_error(err, DHARA_E_PAGE_UNREADABLE);
        return -1;
    }
    return 0;
#endif
}
