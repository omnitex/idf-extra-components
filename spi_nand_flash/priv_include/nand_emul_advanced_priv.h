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
#include <stdbool.h>
#include <stddef.h>
#include "spi_nand_flash.h"

/* ESP_ERR_FLASH_BASE is in esp_err.h; ESP_ERR_FLASH_OP_FAIL is defined in
 * spi_flash_mmap.h which is not always available on the Linux host target.
 * Provide a fallback so the Linux emulator files can use this code. */
#ifndef ESP_ERR_FLASH_OP_FAIL
#include "esp_err.h"
#define ESP_ERR_FLASH_OP_FAIL (ESP_ERR_FLASH_BASE + 1)
#endif

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

/**
 * @brief Check whether the failure model wants to fail an erase on @p block_num.
 *
 * If the model says fail AND its is_block_bad() also fires, the block is
 * marked bad in the backend.
 *
 * @return true  → caller must NOT erase and must return an error code.
 * @return false → proceed normally.
 */
bool nand_emul_advanced_should_fail_erase(spi_nand_flash_device_t *dev,
                                          uint32_t block_num);

/**
 * Check whether the failure model wants to fail a page program on @p page_num.
 * @return true → caller must NOT program and must return an error code.
 */
bool nand_emul_advanced_should_fail_write(spi_nand_flash_device_t *dev,
                                          uint32_t page_num);

/**
 * Check whether the failure model wants to fail a page read on @p page_num.
 * @return true → caller must NOT copy read data and must return an error code.
 */
bool nand_emul_advanced_should_fail_read(spi_nand_flash_device_t *dev,
                                         uint32_t page_num);

/**
 * Optionally corrupt the read buffer in-place (e.g. bit flips).
 * Called after a successful memcpy from flash.  No-op if no failure model.
 */
void nand_emul_advanced_corrupt_read(spi_nand_flash_device_t *dev,
                                     uint32_t page_num,
                                     uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
