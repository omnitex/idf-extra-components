/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "spi_nand_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_err_t (*init)(void *ctx, spi_nand_flash_device_t *nand, const cJSON *ftl_config);
    esp_err_t (*write)(void *ctx, uint32_t sector, const uint8_t *data);
    esp_err_t (*read)(void *ctx, uint32_t sector, uint8_t *data);
    esp_err_t (*sync)(void *ctx);
    esp_err_t (*deinit)(void *ctx);
} ftl_ops_t;

#ifdef __cplusplus
}
#endif
