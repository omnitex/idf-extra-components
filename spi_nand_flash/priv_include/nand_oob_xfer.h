/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nand_oob_layout_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup nand_oob_xfer OOB logical ↔ physical scatter/gather
 *
 * Logical offsets are a contiguous index space over the layout’s **free_region** slices that match
 * the requested @ref spi_nand_oob_class_t (concatenated in enumeration order). Physical placement is
 * `oob_raw[region.offset + k]` where `region.offset` is within the spare area (0 = first OOB byte
 * at column `page_size`); BBM bytes live outside the free stream and must never be reached via a
 * valid logical range.
 *
 * **Single `program_execute` per logical page program (proposal §2.2):** These helpers only copy
 * bytes in RAM (`oob_raw` / caller buffers). They do **not** issue NAND I/O. Callers that split one
 * logical program into multiple `program_execute` steps must not use scatter output that way; all
 * `program_load` sequences derived from one scatter pass for a page must be followed by exactly one
 * `program_execute_and_wait` (enforced in `nand_prog` / related paths, not here).
 */

struct spi_nand_flash_device_t;

/**
 * @brief Enumerate free programmable regions for one logical class (no NAND I/O).
 *
 * Single implementation of the layout->free_region() walk filtered by class. Used at
 * device init (twice: FREE_ECC and FREE_NOECC) to populate handle->oob_cached_regs_*,
 * and by @ref nand_oob_xfer_ctx_init when callers have no device handle cache.
 */
esp_err_t nand_oob_layout_regions_for_class(const spi_nand_oob_layout_t *layout,
                                            const void *chip_ctx,
                                            spi_nand_oob_class_t cls,
                                            uint16_t oob_size,
                                            spi_nand_oob_region_desc_t *regs,
                                            uint8_t max_regs,
                                            uint8_t *reg_count_out,
                                            uint16_t *total_logical_len_out);

/**
 * @brief Build xfer context by walking the layout (no handle cache required).
 *
 * Prefer @ref nand_oob_xfer_ctx_bind on a initialized device handle; this path remains
 * for tests or code that only has layout + chip_ctx.
 */
esp_err_t nand_oob_xfer_ctx_init(spi_nand_oob_xfer_ctx_t *ctx,
                                 const spi_nand_oob_layout_t *layout,
                                 const void *chip_ctx,
                                 spi_nand_oob_class_t cls,
                                 uint8_t *oob_raw,
                                 uint16_t oob_size);

/**
 * @brief Bind xfer context from handle init-time region cache (no layout walk).
 *
 * Region geometry (regs[], total_logical_len) is fixed after nand_oob_device_layout_init();
 * only @p oob_raw and @p oob_size vary per spare buffer. Stack-local @p ctx: no alloc/free.
 * Re-bind when the RAM spare buffer pointer or length changes, or when switching class.
 * Multiple nand_oob_gather/scatter or nand_oob_field_*_ctx calls can share one bind.
 */
esp_err_t nand_oob_xfer_ctx_bind(spi_nand_oob_xfer_ctx_t *ctx,
                                 const struct spi_nand_flash_device_t *handle,
                                 spi_nand_oob_class_t cls,
                                 uint8_t *oob_raw,
                                 uint16_t oob_size);

esp_err_t nand_oob_gather(const spi_nand_oob_xfer_ctx_t *ctx,
                          size_t logical_off,
                          void *dst,
                          size_t len);

esp_err_t nand_oob_scatter(spi_nand_oob_xfer_ctx_t *ctx,
                           size_t logical_off,
                           const void *src,
                           size_t len);

void nand_oob_bbm_fill_good(const spi_nand_oob_layout_t *layout,
                             uint8_t *buf, uint16_t len);

void nand_oob_bbm_fill_bad(const spi_nand_oob_layout_t *layout,
                            uint8_t *buf, uint16_t len);

bool nand_oob_bbm_is_good(const spi_nand_oob_layout_t *layout,
                           const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif
