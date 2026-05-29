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
 * @brief Read one assigned OOB field from a raw spare buffer (no NAND I/O).
 *
 * Uses @c handle->oob_fields[id] (logical offset, class, length) and scatter/gather.
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

#ifdef __cplusplus
}
#endif
