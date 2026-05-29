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

esp_err_t nand_oob_field_scatter(const struct spi_nand_flash_device_t *handle,
                                  spi_nand_oob_field_id_t field,
                                  uint8_t *oob_raw, uint16_t oob_raw_len,
                                  const void *src, size_t src_len);

esp_err_t nand_oob_field_gather(const struct spi_nand_flash_device_t *handle,
                                 spi_nand_oob_field_id_t field,
                                 const uint8_t *oob_raw, uint16_t oob_raw_len,
                                 void *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif
