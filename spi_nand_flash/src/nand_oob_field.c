/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nand_oob_field.h"

#include <string.h>

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

/* Field class selects which handle cache (FREE_ECC vs FREE_NOECC) bind copies. */
static esp_err_t field_xfer_ctx_bind(spi_nand_oob_xfer_ctx_t *ctx,
                                     const spi_nand_flash_device_t *handle,
                                     const spi_nand_oob_field_spec_t *field,
                                     uint8_t *oob_raw,
                                     uint16_t oob_raw_len)
{
    return nand_oob_xfer_ctx_bind(ctx, handle, field->oob_class, oob_raw, oob_raw_len);
}

/* Init-time only: records logical placement; runtime I/O uses nand_oob_field_read/write. */
esp_err_t nand_oob_field_assign(spi_nand_flash_device_t *handle,
                                spi_nand_oob_field_id_t id,
                                uint8_t length,
                                spi_nand_oob_class_t oob_class,
                                uint16_t logical_offset)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE((unsigned)id < SPI_NAND_OOB_FIELD_COUNT, ESP_ERR_INVALID_ARG, TAG, "bad field id");
    ESP_RETURN_ON_FALSE(length > 0, ESP_ERR_INVALID_ARG, TAG, "zero length");

    spi_nand_oob_field_spec_t *field = &handle->oob_fields[id];
    field->id = id;
    field->length = length;
    field->oob_class = oob_class;
    field->logical_offset = logical_offset;
    field->assigned = true;
    return ESP_OK;
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

    /* One-shot bind; see nand_oob_field_read_ctx to reuse ctx across fields. */
    spi_nand_oob_xfer_ctx_t ctx;
    ESP_RETURN_ON_ERROR(field_xfer_ctx_bind(&ctx, handle, field, (uint8_t *)oob_raw, oob_raw_len), TAG, "xfer bind");
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
    ESP_RETURN_ON_ERROR(field_xfer_ctx_bind(&ctx, handle, field, oob_raw, oob_raw_len), TAG, "xfer bind");
    return nand_oob_scatter(&ctx, field->logical_offset, src, field->length);
}

/* Caller must have bound ctx once for this spare buffer and field->oob_class. */
esp_err_t nand_oob_field_read_ctx(const spi_nand_flash_device_t *handle,
                                  const spi_nand_oob_xfer_ctx_t *ctx,
                                  spi_nand_oob_field_id_t id,
                                  void *dst,
                                  size_t dst_len)
{
    const spi_nand_oob_field_spec_t *field = field_spec_for_id(handle, id);
    ESP_RETURN_ON_FALSE(field != NULL, ESP_ERR_INVALID_STATE, TAG, "field not assigned");
    ESP_RETURN_ON_FALSE(ctx != NULL && dst != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(dst_len >= field->length, ESP_ERR_INVALID_SIZE, TAG, "dst too small");
    ESP_RETURN_ON_FALSE(field->oob_class == ctx->cls, ESP_ERR_INVALID_STATE, TAG, "ctx class mismatch");

    return nand_oob_gather(ctx, field->logical_offset, dst, field->length);
}

/* scatter updates oob_raw in place; ctx must be non-const for API symmetry with scatter(). */
esp_err_t nand_oob_field_write_ctx(const struct spi_nand_flash_device_t *handle,
                                   spi_nand_oob_xfer_ctx_t *ctx,
                                   spi_nand_oob_field_id_t id,
                                   const void *src,
                                   size_t src_len)
{
    const spi_nand_oob_field_spec_t *field = field_spec_for_id(handle, id);
    ESP_RETURN_ON_FALSE(field != NULL, ESP_ERR_INVALID_STATE, TAG, "field not assigned");
    ESP_RETURN_ON_FALSE(ctx != NULL && src != NULL, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(src_len >= field->length, ESP_ERR_INVALID_SIZE, TAG, "src too small");
    ESP_RETURN_ON_FALSE(field->oob_class == ctx->cls, ESP_ERR_INVALID_STATE, TAG, "ctx class mismatch");

    return nand_oob_scatter(ctx, field->logical_offset, src, field->length);
}
