/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nand_oob_device.h"

#include <assert.h>
#include <string.h>

#include "esp_check.h"
#include "nand.h"
#include "nand_oob_field.h"
#include "nand_oob_layout_default.h"
#include "nand_oob_xfer.h"

static const char *TAG = "nand_oob_dev";

static esp_err_t nand_oob_spare_bytes_for_handle(const spi_nand_flash_device_t *handle, uint16_t *spare_out)
{
#ifdef CONFIG_IDF_TARGET_LINUX
    *spare_out = (uint16_t)handle->chip.emulated_page_oob;
    return ESP_OK;
#else
    switch (handle->chip.page_size) {
    case 512:
        *spare_out = 16;
        return ESP_OK;
    case 2048:
        /* TODO: Some chips (e.g. all GigaDevice GD5F* models) have 2048+128
         * layout, not 2048+64. When per-vendor layouts are added, this should
         * come from the chip database instead of a page_size switch. */
        *spare_out = 64;
        return ESP_OK;
    case 4096:
        *spare_out = 128;
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
}

esp_err_t nand_oob_device_layout_init(spi_nand_flash_device_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    memset(handle->oob_fields, 0, sizeof(handle->oob_fields));
    memset(handle->oob_cached_regs_free_ecc, 0, sizeof(handle->oob_cached_regs_free_ecc));
    memset(handle->oob_cached_regs_free_no_ecc, 0, sizeof(handle->oob_cached_regs_free_no_ecc));
    handle->oob_cached_reg_count_free_ecc = 0;
    handle->oob_cached_reg_count_free_no_ecc = 0;
    handle->oob_total_logical_len_free_ecc = 0;
    handle->oob_total_logical_len_free_no_ecc = 0;

    if (handle->oob_layout == NULL) {
        handle->oob_layout = nand_oob_layout_get_default();
    }

    uint16_t spare = handle->oob_layout->oob_bytes;
    if (spare == 0) {
        ESP_RETURN_ON_ERROR(nand_oob_spare_bytes_for_handle(handle, &spare), TAG, "spare size");
    }
    ESP_RETURN_ON_FALSE(spare >= 4, ESP_ERR_INVALID_SIZE, TAG, "spare too small for marker layout");

    /*
     * Cache free_region() per class once (two walks at init). Runtime nand_oob_xfer_ctx_bind()
     * copies these arrays instead of calling layout ops again on every field read/write.
     */
    ESP_RETURN_ON_ERROR(
        nand_oob_layout_regions_for_class(handle->oob_layout, handle, SPI_NAND_OOB_CLASS_FREE_ECC, spare,
                                          handle->oob_cached_regs_free_ecc, SPI_NAND_OOB_MAX_REGIONS,
                                          &handle->oob_cached_reg_count_free_ecc,
                                          &handle->oob_total_logical_len_free_ecc),
        TAG, "FREE_ECC regions");
    ESP_RETURN_ON_ERROR(
        nand_oob_layout_regions_for_class(handle->oob_layout, handle, SPI_NAND_OOB_CLASS_FREE_NOECC, spare,
                                          handle->oob_cached_regs_free_no_ecc, SPI_NAND_OOB_MAX_REGIONS,
                                          &handle->oob_cached_reg_count_free_no_ecc,
                                          &handle->oob_total_logical_len_free_no_ecc),
        TAG, "FREE_NOECC regions");

    /* Driver-owned default field; WL may add more via nand_oob_field_assign later. */
    ESP_RETURN_ON_ERROR(nand_oob_field_assign(handle, SPI_NAND_OOB_FIELD_PAGE_USED, 2,
                                              SPI_NAND_OOB_CLASS_FREE_ECC, 0),
                        TAG, "PAGE_USED field");

    /* logical_offset + length must fit in the packed stream for that field's class. */
    for (int i = 0; i < SPI_NAND_OOB_FIELD_COUNT; i++) {
        spi_nand_oob_field_spec_t *f = &handle->oob_fields[i];
        if (!f->assigned) {
            continue;
        }
        uint16_t total = (f->oob_class == SPI_NAND_OOB_CLASS_FREE_ECC)
                             ? handle->oob_total_logical_len_free_ecc
                             : handle->oob_total_logical_len_free_no_ecc;
        ESP_RETURN_ON_FALSE((uint32_t)f->logical_offset + (uint32_t)f->length <= (uint32_t)total,
                            ESP_ERR_INVALID_SIZE, TAG, "OOB field %d overflows its class layout", i);
    }

#ifndef NDEBUG
    if (handle->oob_layout == nand_oob_layout_get_default()) {
        assert(handle->oob_total_logical_len_free_ecc == 2);
        assert(handle->oob_cached_reg_count_free_ecc == 1);
        assert(handle->oob_cached_regs_free_ecc[0].offset == 2 && handle->oob_cached_regs_free_ecc[0].length == 2);
        assert(handle->oob_total_logical_len_free_no_ecc == 0);
        assert(handle->oob_cached_reg_count_free_no_ecc == 0);
    }
#endif

    return ESP_OK;
}
