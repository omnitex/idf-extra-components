/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nand_oob_field.h"

#include <string.h>

#include "nand.h"
#include "nand_oob_xfer.h"

static const char *TAG = "nand_oob_field";

static const spi_nand_oob_field_spec_t *get_field_spec(const spi_nand_flash_device_t *handle,
                                                         spi_nand_oob_field_id_t field)
{
    if ((int)field < 0 || (int)field >= SPI_NAND_OOB_FIELD_COUNT) {
        return NULL;
    }
    const spi_nand_oob_field_spec_t *spec = &handle->oob_fields[field];
    if (!spec->assigned) {
        return NULL;
    }
    return spec;
}

esp_err_t nand_oob_field_scatter(const spi_nand_flash_device_t *handle,
                                  spi_nand_oob_field_id_t field,
                                  uint8_t *oob_raw, uint16_t oob_raw_len,
                                  const void *src, size_t src_len)
{
    if (handle == NULL || oob_raw == NULL || src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const spi_nand_oob_field_spec_t *spec = get_field_spec(handle, field);
    if (spec == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (src_len != spec->length) {
        return ESP_ERR_INVALID_SIZE;
    }

    spi_nand_oob_xfer_ctx_t ctx;
    esp_err_t err = nand_oob_xfer_ctx_init(&ctx, handle->oob_layout, handle,
                                            spec->oob_class, oob_raw, oob_raw_len);
    if (err != ESP_OK) {
        return err;
    }
    return nand_oob_scatter(&ctx, spec->logical_offset, src, src_len);
}

esp_err_t nand_oob_field_gather(const spi_nand_flash_device_t *handle,
                                 spi_nand_oob_field_id_t field,
                                 const uint8_t *oob_raw, uint16_t oob_raw_len,
                                 void *dst, size_t dst_len)
{
    if (handle == NULL || oob_raw == NULL || dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const spi_nand_oob_field_spec_t *spec = get_field_spec(handle, field);
    if (spec == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dst_len != spec->length) {
        return ESP_ERR_INVALID_SIZE;
    }

    spi_nand_oob_xfer_ctx_t ctx;
    esp_err_t err = nand_oob_xfer_ctx_init(&ctx, handle->oob_layout, handle,
                                            spec->oob_class, (uint8_t *)oob_raw, oob_raw_len);
    if (err != ESP_OK) {
        return err;
    }
    return nand_oob_gather(&ctx, spec->logical_offset, dst, dst_len);
}
