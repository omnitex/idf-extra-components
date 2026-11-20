/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "spi_nand_flash_test_helpers.h"
#include "nand_onfi_crc.h"
#include "nand.h"
#if !CONFIG_IDF_TARGET_LINUX
#include "spi_nand_oper.h"
#endif

#define SPI_NAND_FLASH_PATTERN_SEED  0x12345678U

void spi_nand_flash_fill_buffer(uint8_t *dst, size_t count)
{
    uint32_t *p = (uint32_t *)dst;
    for (size_t i = 0; i < count; ++i) {
        p[i] = SPI_NAND_FLASH_PATTERN_SEED + (uint32_t)i;
    }
}

void spi_nand_flash_fill_buffer_seeded(uint8_t *dst, size_t count, uint32_t seed)
{
    uint32_t *p = (uint32_t *)dst;
    for (size_t i = 0; i < count; ++i) {
        p[i] = seed + (uint32_t)i;
    }
}

int spi_nand_flash_check_buffer(const uint8_t *src, size_t count)
{
    const uint32_t *p = (const uint32_t *)src;
    for (size_t i = 0; i < count; ++i) {
        if (p[i] != SPI_NAND_FLASH_PATTERN_SEED + (uint32_t)i) {
            return (int)(i + 1);
        }
    }
    return 0;
}

int spi_nand_flash_check_buffer_seeded(const uint8_t *src, size_t count, uint32_t seed)
{
    const uint32_t *p = (const uint32_t *)src;
    for (size_t i = 0; i < count; ++i) {
        if (p[i] != seed + (uint32_t)i) {
            return (int)(i + 1);
        }
    }
    return 0;
}

uint16_t spi_nand_test_onfi_param_page_crc16(const uint8_t *data, size_t length)
{
    return nand_onfi_param_page_crc16(data, length);
}

bool spi_nand_test_onfi_param_page_crc_valid(const uint8_t *page_data, size_t page_size)
{
    return nand_onfi_param_page_crc_valid(page_data, page_size);
}

void spi_nand_test_get_device_info(spi_nand_flash_device_t *dev, nand_device_info_t *out)
{
    *out = dev->device_info;
}

nand_ecc_status_t spi_nand_test_get_last_ecc_status(spi_nand_flash_device_t *dev)
{
    return dev->chip.ecc_data.ecc_corrected_bits_status;
}

#if !CONFIG_IDF_TARGET_LINUX
esp_err_t spi_nand_test_get_on_die_ecc(spi_nand_flash_device_t *dev, bool *enabled)
{
    uint8_t cfg;
    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    esp_err_t ret = spi_nand_read_register(dev, REG_CONFIG, &cfg);
    xSemaphoreGive(dev->mutex);
    if (ret == ESP_OK) {
        *enabled = (cfg & REG_CONFIG_ECC_EN) != 0;
    }
    return ret;
}

esp_err_t spi_nand_test_set_on_die_ecc(spi_nand_flash_device_t *dev, bool enable)
{
    uint8_t cfg;
    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    esp_err_t ret = spi_nand_read_register(dev, REG_CONFIG, &cfg);
    if (ret == ESP_OK) {
        cfg = enable ? (cfg | REG_CONFIG_ECC_EN) : (cfg & ~REG_CONFIG_ECC_EN);
        ret = spi_nand_write_register(dev, REG_CONFIG, cfg);
    }
    if (ret == ESP_OK) {
        ret = spi_nand_read_register(dev, REG_CONFIG, &cfg);
    }
    xSemaphoreGive(dev->mutex);
    if (ret == ESP_OK && ((cfg & REG_CONFIG_ECC_EN) != 0) != enable) {
        ret = ESP_ERR_INVALID_STATE;
    }
    return ret;
}
#endif /* !CONFIG_IDF_TARGET_LINUX */
