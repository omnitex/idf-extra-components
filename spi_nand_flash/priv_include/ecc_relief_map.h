/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "dhara/nand.h"  /* dhara_page_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel page values used in the open-addressing hash table */
#define ECC_RELIEF_INVALID_PAGE    ((dhara_page_t)0xFFFFFFFF)
#define ECC_RELIEF_TOMBSTONE_PAGE  ((dhara_page_t)0xFFFFFFFE)

/* Flag bits stored in ecc_relief_entry_t::flags */
#define ECC_RELIEF_FLAG_PENDING    0x01u   /*!< Page should be skipped on next journal write */

/**
 * @brief One entry in the open-addressing relief map.
 *
 * Memory: 6 bytes per entry.  Default capacity 512 => 3072 B.
 */
typedef struct {
    dhara_page_t page;       /*!< Physical page number; ECC_RELIEF_INVALID_PAGE = empty slot */
    uint8_t      mid_count;  /*!< Accumulated MID-level ECC read count */
    uint8_t      flags;      /*!< ECC_RELIEF_FLAG_PENDING (bit 0) */
} ecc_relief_entry_t;

#ifdef __cplusplus
}
#endif
