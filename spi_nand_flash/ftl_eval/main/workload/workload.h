/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sector;
    bool is_write;
    uint8_t *data;
    size_t data_len;
} workload_op_t;

typedef struct {
    esp_err_t (*init)(void *ctx, const cJSON *config);
    esp_err_t (*next_op)(void *ctx, workload_op_t *op);
    bool      (*is_done)(void *ctx);
    esp_err_t (*deinit)(void *ctx);
} workload_ops_t;

workload_ops_t *workload_create(const char *type);

#ifdef __cplusplus
}
#endif
