/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ftl.h"

#include <string.h>

ftl_ops_t *dhara_ftl_get_ops(void);

ftl_ops_t *ftl_create(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    if (strcmp(name, "dhara") == 0) {
        return dhara_ftl_get_ops();
    }

    return NULL;
}
