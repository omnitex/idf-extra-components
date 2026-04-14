/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sentinel stored in the OOB LPN field when no logical page number
 *        has been written, matching the erased-NAND state (all bits 1).
 *
 * This is the nand_impl-layer counterpart of dhara's DHARA_OOB_LPN_NONE and
 * must stay numerically equal to it (0xFFFFFFFF).
 */
#define NAND_OOB_LPN_NONE 0xFFFFFFFFu

/**
 * @brief Pack a 32-bit logical page number (LPN) into a 4-byte little-endian
 *        buffer for storage at CONFIG_NAND_FLASH_OOB_LPN_OFFSET in a page's
 *        OOB spare area.
 *
 * This is the exact inverse of the byte reconstruction performed by
 * nand_read_lpn() in nand_impl.c and nand_impl_linux.c:
 *   lpn = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24)
 *
 * @param[in]  lpn  Logical page number to pack.
 * @param[out] buf  Destination buffer, must be at least 4 bytes.
 */
static inline void nand_oob_pack_lpn_le(uint32_t lpn, uint8_t *buf)
{
    buf[0] = (uint8_t)(lpn & 0xFF);
    buf[1] = (uint8_t)((lpn >> 8) & 0xFF);
    buf[2] = (uint8_t)((lpn >> 16) & 0xFF);
    buf[3] = (uint8_t)((lpn >> 24) & 0xFF);
}

#ifdef __cplusplus
}
#endif
