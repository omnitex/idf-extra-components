/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nand_oob_xfer.h"

#include <assert.h>
#include <string.h>

#include "nand.h"
#include "nand_oob_layout_default.h"

static bool region_matches_class(const spi_nand_oob_region_desc_t *r, spi_nand_oob_class_t cls)
{
    if (!r->programmable) {
        return false;
    }
    if (cls == SPI_NAND_OOB_CLASS_FREE_ECC) {
        return r->ecc_protected;
    }
    return !r->ecc_protected;
}

/*
 * Shared layout walk: maps logical class (FREE_ECC vs FREE_NOECC) to physical spare slices.
 * Callers pass an output array; device init stores into handle->oob_cached_regs_* so runtime
 * field I/O can use nand_oob_xfer_ctx_bind() without repeating this loop.
 */
esp_err_t nand_oob_layout_regions_for_class(const spi_nand_oob_layout_t *layout,
                                            const void *chip_ctx,
                                            spi_nand_oob_class_t cls,
                                            uint16_t oob_size,
                                            spi_nand_oob_region_desc_t *regs,
                                            uint8_t max_regs,
                                            uint8_t *reg_count_out,
                                            uint16_t *total_logical_len_out)
{
    if (layout == NULL || layout->ops == NULL || layout->ops->free_region == NULL || regs == NULL
        || reg_count_out == NULL || total_logical_len_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *reg_count_out = 0;
    *total_logical_len_out = 0;

    for (int section = 0;; section++) {
        spi_nand_oob_region_desc_t desc;
        esp_err_t err = layout->ops->free_region(chip_ctx, section, &desc);
        if (err == ESP_ERR_NOT_FOUND) {
            break;
        }
        if (err != ESP_OK) {
            return err;
        }
        if (!region_matches_class(&desc, cls)) {
            continue;
        }
        if (*reg_count_out >= max_regs) {
            return ESP_ERR_NO_MEM;
        }
        if ((uint32_t)desc.offset + (uint32_t)desc.length > (uint32_t)oob_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        regs[*reg_count_out] = desc;
        *total_logical_len_out += desc.length;
        (*reg_count_out)++;
    }

    return ESP_OK;
}

/* Slow path: walk layout when there is no device handle cache (tests, pre-init tools). */
esp_err_t nand_oob_xfer_ctx_init(spi_nand_oob_xfer_ctx_t *ctx,
                                 const spi_nand_oob_layout_t *layout,
                                 const void *chip_ctx,
                                 spi_nand_oob_class_t cls,
                                 uint8_t *oob_raw,
                                 uint16_t oob_size)
{
    if (ctx == NULL || oob_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->layout = layout;
    ctx->cls = cls;
    ctx->oob_raw = oob_raw;
    ctx->oob_size = oob_size;

    uint16_t total_logical_len = 0;
    esp_err_t err = nand_oob_layout_regions_for_class(layout, chip_ctx, cls, oob_size, ctx->regs,
                                                      SPI_NAND_OOB_MAX_REGIONS, &ctx->reg_count,
                                                      &total_logical_len);
    if (err != ESP_OK) {
        memset(ctx, 0, sizeof(*ctx));
        return err;
    }
    ctx->total_logical_len = total_logical_len;

#ifndef NDEBUG
    if (layout == nand_oob_layout_get_default() && cls == SPI_NAND_OOB_CLASS_FREE_ECC) {
        assert(ctx->total_logical_len == 2);
        assert(ctx->reg_count == 1);
        assert(ctx->regs[0].offset == 2 && ctx->regs[0].length == 2);
    }
#endif

    return ESP_OK;
}

/*
 * Fast path for nand_oob_field_* and multi-field spare composition: region list is copied
 * from handle init cache. Only oob_raw/oob_size change per page (e.g. temp_buffer vs
 * read_buffer). No heap; ctx is stack-local. Re-bind after switching spare buffer or class.
 */
esp_err_t nand_oob_xfer_ctx_bind(spi_nand_oob_xfer_ctx_t *ctx,
                                 const spi_nand_flash_device_t *handle,
                                 spi_nand_oob_class_t cls,
                                 uint8_t *oob_raw,
                                 uint16_t oob_size)
{
    if (ctx == NULL || handle == NULL || handle->oob_layout == NULL || oob_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const spi_nand_oob_region_desc_t *cached_regs;
    uint8_t cached_count;
    uint16_t cached_total;

    if (cls == SPI_NAND_OOB_CLASS_FREE_ECC) {
        cached_regs = handle->oob_cached_regs_free_ecc;
        cached_count = handle->oob_cached_reg_count_free_ecc;
        cached_total = handle->oob_total_logical_len_free_ecc;
    } else {
        cached_regs = handle->oob_cached_regs_free_no_ecc;
        cached_count = handle->oob_cached_reg_count_free_no_ecc;
        cached_total = handle->oob_total_logical_len_free_no_ecc;
    }

    /* Cached geometry is fixed; caller buffer may be shorter (partial OOB read). */
    for (unsigned i = 0; i < cached_count; i++) {
        const spi_nand_oob_region_desc_t *r = &cached_regs[i];
        if ((uint32_t)r->offset + (uint32_t)r->length > (uint32_t)oob_size) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->layout = handle->oob_layout;
    ctx->cls = cls;
    ctx->oob_raw = oob_raw;
    ctx->oob_size = oob_size;
    ctx->reg_count = cached_count;
    ctx->total_logical_len = cached_total;
    if (cached_count > 0) {
        memcpy(ctx->regs, cached_regs, (size_t)cached_count * sizeof(ctx->regs[0]));
    }

    return ESP_OK;
}

esp_err_t nand_oob_gather(const spi_nand_oob_xfer_ctx_t *ctx,
                          size_t logical_off,
                          void *dst,
                          size_t len)
{
    if (ctx == NULL || dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (logical_off > ctx->total_logical_len || len > ctx->total_logical_len - logical_off) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *dst_u8 = (uint8_t *)dst;
    const uint8_t *oob = ctx->oob_raw;
    size_t logical_base = 0;

    for (unsigned i = 0; i < ctx->reg_count; i++) {
        const spi_nand_oob_region_desc_t *r = &ctx->regs[i];
        size_t next_base = logical_base + r->length;
        if (logical_off + len <= logical_base) {
            break;
        }
        if (logical_off >= next_base) {
            logical_base = next_base;
            continue;
        }
        size_t seg_start = logical_off > logical_base ? logical_off : logical_base;
        size_t seg_end = logical_off + len < next_base ? logical_off + len : next_base;
        if (seg_start < seg_end) {
            size_t local = seg_start - logical_base;
            size_t run = seg_end - seg_start;
            size_t dst_off = seg_start - logical_off;
            memcpy(dst_u8 + dst_off, oob + r->offset + local, run);
        }
        logical_base = next_base;
    }
    return ESP_OK;
}

esp_err_t nand_oob_scatter(spi_nand_oob_xfer_ctx_t *ctx,
                           size_t logical_off,
                           const void *src,
                           size_t len)
{
    if (ctx == NULL || src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (logical_off > ctx->total_logical_len || len > ctx->total_logical_len - logical_off) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *src_u8 = (const uint8_t *)src;
    uint8_t *oob = ctx->oob_raw;
    size_t logical_base = 0;

    for (unsigned i = 0; i < ctx->reg_count; i++) {
        const spi_nand_oob_region_desc_t *r = &ctx->regs[i];
        size_t next_base = logical_base + r->length;
        if (logical_off + len <= logical_base) {
            break;
        }
        if (logical_off >= next_base) {
            logical_base = next_base;
            continue;
        }
        size_t seg_start = logical_off > logical_base ? logical_off : logical_base;
        size_t seg_end = logical_off + len < next_base ? logical_off + len : next_base;
        if (seg_start < seg_end) {
            size_t local = seg_start - logical_base;
            size_t run = seg_end - seg_start;
            size_t src_off = seg_start - logical_off;
            memcpy(oob + r->offset + local, src_u8 + src_off, run);
        }
        logical_base = next_base;
    }
    return ESP_OK;
}

void nand_oob_bbm_fill_good(const spi_nand_oob_layout_t *layout,
                             uint8_t *buf, uint16_t len)
{
    if (buf == NULL || layout == NULL) {
        return;
    }
    memset(buf, 0xFF, len);
    if (layout->bbm.bbm_offset + layout->bbm.bbm_length <= len) {
        memcpy(buf + layout->bbm.bbm_offset, layout->bbm.good_pattern, layout->bbm.bbm_length);
    }
}

void nand_oob_bbm_fill_bad(const spi_nand_oob_layout_t *layout,
                            uint8_t *buf, uint16_t len)
{
    if (buf == NULL || layout == NULL) {
        return;
    }
    memset(buf, 0xFF, len);
    for (unsigned i = 0; i < layout->bbm.bbm_length && (layout->bbm.bbm_offset + i) < len; i++) {
        buf[layout->bbm.bbm_offset + i] = (uint8_t)(layout->bbm.good_pattern[i] ^ 0xFF);
    }
}

bool nand_oob_bbm_is_good(const spi_nand_oob_layout_t *layout,
                           const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || layout == NULL) {
        return false;
    }
    if (layout->bbm.bbm_offset + layout->bbm.bbm_length > len) {
        return false;
    }
    return memcmp(buf + layout->bbm.bbm_offset, layout->bbm.good_pattern, layout->bbm.bbm_length) == 0;
}
