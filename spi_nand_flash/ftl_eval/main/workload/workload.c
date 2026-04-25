/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "workload.h"

#include <string.h>

workload_ops_t *workload_sequential_get_ops(void);
workload_ops_t *workload_random_get_ops(void);
workload_ops_t *workload_mixed_get_ops(void);

workload_ops_t *workload_create(const char *type)
{
    if (type == NULL) {
        return NULL;
    }

    if (strcmp(type, "sequential") == 0) {
        return workload_sequential_get_ops();
    }

    if (strcmp(type, "random") == 0) {
        return workload_random_get_ops();
    }

    if (strcmp(type, "mixed") == 0) {
        return workload_mixed_get_ops();
    }

    return NULL;
}
