/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdbool.h>
#include "test_spi_nand_common.h"
#include "unity.h"

static bool s_spi_bus_initialized;
static spi_host_device_t s_spi_host = HOST_ID;
static spi_device_handle_t s_spi_handle;

void spi_nand_test_setup_bus(spi_host_device_t host, const spi_bus_config_t *bus_cfg)
{
    TEST_ESP_OK(spi_bus_initialize(host, bus_cfg, SPI_DMA_CHAN));
    s_spi_host = host;
    s_spi_bus_initialized = true;
}

void spi_nand_test_add_device(int cs, uint8_t flags, int clock_hz, spi_device_handle_t *spi)
{
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = clock_hz,
        .mode = 0,
        .spics_io_num = cs,
        .queue_size = 10,
        .flags = flags,
    };
    TEST_ESP_OK(spi_bus_add_device(s_spi_host, &devcfg, spi));
    s_spi_handle = *spi;
}

void spi_nand_test_setup_chip(spi_device_handle_t *spi, uint8_t flags)
{
    spi_bus_config_t spi_bus_cfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadhd_io_num = PIN_HD,
        .quadwp_io_num = PIN_WP,
        .max_transfer_sz = 64,
    };
    spi_nand_test_setup_bus(HOST_ID, &spi_bus_cfg);
    spi_nand_test_add_device(PIN_CS, flags, 40 * 1000 * 1000, spi);
}

void spi_nand_test_mark_bus_initialized(void)
{
    s_spi_host = HOST_ID;
    s_spi_bus_initialized = true;
}

void spi_nand_test_set_spi_handle(spi_device_handle_t spi)
{
    s_spi_handle = spi;
}

void spi_nand_flash_test_teardown(void)
{
    if (s_spi_handle) {
        spi_bus_remove_device(s_spi_handle);
        s_spi_handle = NULL;
    }
    if (s_spi_bus_initialized) {
        spi_bus_free(s_spi_host);
        s_spi_bus_initialized = false;
    }
}
