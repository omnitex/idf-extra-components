/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nand_oob_field.h"

#include "esp_check.h"
#include "nand.h"
#include "nand_oob_xfer.h"

static const char *TAG = "nand_oob_field";

static const spi_nand_oob_field_spec_t *field_spec_for_id(const spi_nand_flash_device_t *handle,
                                                           spi_nand_oob_field_id_t id)
{
    if (handle == NULL || handle->oob_layout == NULL) {
        return NULL;
    }
    if ((unsigned)id >= SPI_NAND_OOB_FIELD_COUNT) {
        return NULL;
    }
    const spi_nand_oob_field_spec_t *field = &handle->oob_fields[id];
    if (!field->assigned || field->id != id) {
        return NULL;
    }
    return field;
}

esp_err_t nand_oob_field_read(const spi_nand_flash_device_t *handle,
                              spi_nand_oob_field_id_t id,
                              const uint8_t *oob_raw,
                              uint16_t oob_raw_len,
                              void *dst,
                              size_t dst_len)
{
    const spi_nand_oob_field_spec_t *field = field_spec_for_id(handle, id);
    ESP_RETURN_ON_FALSE(field != NULL, ESP_ERR_INVALID_STATE, TAG, "field not assigned");
    ESP_RETURN_ON_FALSE(oob_raw != NULL && dst != NULL, ESP_ERR_INVALID_ARG, TAG, "null buffer");
    ESP_RETURN_ON_FALSE(dst_len >= field->length, ESP_ERR_INVALID_SIZE, TAG, "dst too small");

    spi_nand_oob_xfer_ctx_t ctx;
    ESP_RETURN_ON_ERROR(nand_oob_xfer_ctx_init(&ctx, handle->oob_layout, handle, field->oob_class,
                                               (uint8_t *)oob_raw, oob_raw_len),
                        TAG, "xfer ctx");
    return nand_oob_gather(&ctx, field->logical_offset, dst, field->length);
}

esp_err_t nand_oob_field_write(const struct spi_nand_flash_device_t *handle,
                               spi_nand_oob_field_id_t id,
                               uint8_t *oob_raw,
                               uint16_t oob_raw_len,
                               const void *src,
                               size_t src_len)
{
    const spi_nand_oob_field_spec_t *field = field_spec_for_id(handle, id);
    ESP_RETURN_ON_FALSE(field != NULL, ESP_ERR_INVALID_STATE, TAG, "field not assigned");
    ESP_RETURN_ON_FALSE(oob_raw != NULL && src != NULL, ESP_ERR_INVALID_ARG, TAG, "null buffer");
    ESP_RETURN_ON_FALSE(src_len >= field->length, ESP_ERR_INVALID_SIZE, TAG, "src too small");

    spi_nand_oob_xfer_ctx_t ctx;
    ESP_RETURN_ON_ERROR(nand_oob_xfer_ctx_init(&ctx, handle->oob_layout, handle, field->oob_class,
                                               oob_raw, oob_raw_len),
                        TAG, "xfer ctx");
    return nand_oob_scatter(&ctx, field->logical_offset, src, field->length);
}
