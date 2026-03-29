/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Private interface between nand_linux_mmap_emul.c and nand_emul_advanced.c.
 *
 * These three functions are called by the low-level emulator after successful
 * erase, write, and read operations to dispatch events to the advanced
 * tracking backend.  They are no-ops if advanced tracking is not active.
 *
 * Include this header ONLY from nand_linux_mmap_emul.c.
 */

#include <stdint.h>
#include "spi_nand_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Notify the advanced backend that block @p block_num was erased.
 *
 * @param dev       Device handle (may have no advanced context — safe).
 * @param block_num Zero-based block number.
 */
void nand_emul_advanced_notify_erase(spi_nand_flash_device_t *dev, uint32_t block_num);

/**
 * @brief Notify the advanced backend that page @p page_num was programmed.
 *
 * @param dev      Device handle.
 * @param page_num Zero-based absolute page number.
 */
void nand_emul_advanced_notify_program(spi_nand_flash_device_t *dev, uint32_t page_num);

/**
 * @brief Notify the advanced backend that page @p page_num was read.
 *
 * @param dev      Device handle.
 * @param page_num Zero-based absolute page number.
 */
void nand_emul_advanced_notify_read(spi_nand_flash_device_t *dev, uint32_t page_num);

#ifdef __cplusplus
}
#endif
