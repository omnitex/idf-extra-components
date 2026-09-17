/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "spi_nand_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

// These API used for diagnostic purpose of SPI NAND Flash

/** @brief Per-device Dhara physical operation and metadata cache counters. */
typedef struct {
    uint64_t metadata_cache_hits;
    uint64_t metadata_cache_misses;
    uint64_t physical_reads;
    uint64_t physical_programs;
    uint64_t physical_copies;
    uint64_t physical_erases;
} spi_nand_flash_perf_stats_t;

/** @brief Snapshot performance counters.
 *
 * This task-context API takes the device mutex and must not be called from an ISR.
 *
 * @param flash The handle to the SPI NAND flash chip.
 * @param[out] stats Destination for the counter snapshot.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG for a null argument.
 */
esp_err_t nand_get_perf_stats(spi_nand_flash_device_t *flash, spi_nand_flash_perf_stats_t *stats);

/** @brief Reset performance counters and invalidate the metadata cache.
 *
 * This task-context API takes the device mutex and must not be called from an ISR.
 *
 * @param flash The handle to the SPI NAND flash chip.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG for a null handle.
 */
esp_err_t nand_reset_perf_stats(spi_nand_flash_device_t *flash);

/** @brief Get bad block statistics for the NAND Flash.
 *
 * This function scans all the blocks in the NAND Flash and returns the total count of bad blocks.
 *
 * @param flash The handle to the SPI nand flash chip.
 * @param[out] bad_block_count A pointer of where to put the return value
 * @return ESP_OK on success, or a flash error code if it fails to get bad block statistics.
 */
esp_err_t nand_get_bad_block_stats(spi_nand_flash_device_t *flash, uint32_t *bad_block_count);

/** @brief Get ECC error statistics for the NAND Flash.
 *
 * This function displays the total ECC errors reported, ECC not corrected error count and ECC error count exceeding threshold.
 *
 * @param flash The handle to the SPI nand flash chip.
 * @return ESP_OK on success, or a flash error code if it failed to read the page.
 */
esp_err_t nand_get_ecc_stats(spi_nand_flash_device_t *flash);

#ifdef __cplusplus
}
#endif
