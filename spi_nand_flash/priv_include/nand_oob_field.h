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

struct spi_nand_flash_device_t;

/** Dhara page-used value after @c nand_prog (logical field bytes). */
#define NAND_OOB_PAGE_USED_PROG_BYTE 0x00
/** Erased / free page pattern for @ref SPI_NAND_OOB_FIELD_PAGE_USED. */
#define NAND_OOB_PAGE_USED_FREE_BYTE 0xFF

/**
 * @brief Register an OOB field slot at init (logical placement only; no NAND I/O).
 *
 * Sets handle->oob_fields[id] metadata (length, class, logical_offset). Does not touch
 * spare bytes; use nand_oob_field_write for RAM buffers. Call after
 * nand_oob_device_layout_init has cached free regions so overflow checks can run.
 */
esp_err_t nand_oob_field_assign(struct spi_nand_flash_device_t *handle,
                                spi_nand_oob_field_id_t id,
                                uint8_t length,
                                spi_nand_oob_class_t oob_class,
                                uint16_t logical_offset);

/**
 * @brief Read one assigned OOB field from a raw spare buffer (no NAND I/O).
 *
 * Binds a stack xfer ctx from the handle region cache, then gather. Convenience wrapper;
 * for several fields in the same class and same @p oob_raw, bind once and use
 * nand_oob_field_read_ctx instead.
 *
 * @param handle Device with layout and fields initialized.
 * @param id Field slot (must match @c oob_fields[id].id and @c assigned).
 * @param oob_raw Spare bytes (length @p oob_raw_len), e.g. first bytes at column page_size.
 * @param oob_raw_len Size of @p oob_raw (must cover all regions for this field).
 * @param dst Output buffer (at least @p dst_len bytes; only @c field.length bytes are written).
 * @param dst_len Size of @p dst (must be >= field length).
 */
esp_err_t nand_oob_field_read(const struct spi_nand_flash_device_t *handle,
                              spi_nand_oob_field_id_t id,
                              const uint8_t *oob_raw,
                              uint16_t oob_raw_len,
                              void *dst,
                              size_t dst_len);

/**
 * @brief Write one assigned OOB field into a raw spare buffer (no NAND I/O).
 *
 * Same bind-once pattern as read; see nand_oob_field_write_ctx for multi-field updates.
 *
 * @param handle Device with layout and fields initialized.
 * @param id Field slot.
 * @param oob_raw Spare buffer to update in RAM.
 * @param oob_raw_len Size of @p oob_raw.
 * @param src Source bytes (only @c field.length bytes are copied).
 * @param src_len Size of @p src (must be >= field length).
 */
esp_err_t nand_oob_field_write(const struct spi_nand_flash_device_t *handle,
                               spi_nand_oob_field_id_t id,
                               uint8_t *oob_raw,
                               uint16_t oob_raw_len,
                               const void *src,
                               size_t src_len);

/**
 * @brief Read a field using an already-bound xfer context (same class and @p oob_raw).
 *
 * @p ctx must come from nand_oob_xfer_ctx_bind with field->oob_class and the same spare
 * buffer that will be passed to program_load / was filled by read.
 */
esp_err_t nand_oob_field_read_ctx(const struct spi_nand_flash_device_t *handle,
                                  const spi_nand_oob_xfer_ctx_t *ctx,
                                  spi_nand_oob_field_id_t id,
                                  void *dst,
                                  size_t dst_len);

/**
 * @brief Write a field using an already-bound xfer context (same class and @p oob_raw).
 *
 * Typical session: bind -> nand_oob_bbm_fill_good -> write_ctx(PAGE_USED) -> ... -> program_load.
 */
esp_err_t nand_oob_field_write_ctx(const struct spi_nand_flash_device_t *handle,
                                   spi_nand_oob_xfer_ctx_t *ctx,
                                   spi_nand_oob_field_id_t id,
                                   const void *src,
                                   size_t src_len);

#ifdef __cplusplus
}
#endif
