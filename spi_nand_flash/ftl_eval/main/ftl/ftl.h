/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "ftl_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

ftl_ops_t *ftl_create(const char *name);

#ifdef __cplusplus
}
#endif
