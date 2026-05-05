/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_system.h"
#include "soc/spi_pins.h"
#include "spi_nand_flash.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "perf_benchmarks.h"

#define EXAMPLE_FLASH_FREQ_KHZ      40000

#define BENCH_DEFAULT_NUM_PAGES     2048
#define BENCH_DEFAULT_NUM_PASSES    5

static const char *TAG = "perf_app";

#ifdef CONFIG_IDF_TARGET_ESP32
#define HOST_ID  SPI3_HOST
#define PIN_MOSI SPI3_IOMUX_PIN_NUM_MOSI
#define PIN_MISO SPI3_IOMUX_PIN_NUM_MISO
#define PIN_CLK  SPI3_IOMUX_PIN_NUM_CLK
#define PIN_CS   SPI3_IOMUX_PIN_NUM_CS
#define PIN_WP   SPI3_IOMUX_PIN_NUM_WP
#define PIN_HD   SPI3_IOMUX_PIN_NUM_HD
#define SPI_DMA_CHAN SPI_DMA_CH_AUTO
#else
#define HOST_ID  SPI2_HOST
#define PIN_MOSI SPI2_IOMUX_PIN_NUM_MOSI
#define PIN_MISO SPI2_IOMUX_PIN_NUM_MISO
#define PIN_CLK  SPI2_IOMUX_PIN_NUM_CLK
#define PIN_CS   SPI2_IOMUX_PIN_NUM_CS
#define PIN_WP   SPI2_IOMUX_PIN_NUM_WP
#define PIN_HD   SPI2_IOMUX_PIN_NUM_HD
#define SPI_DMA_CHAN SPI_DMA_CH_AUTO
#endif

static void example_init_nand_flash(spi_nand_flash_device_t **out_handle, spi_device_handle_t *spi_handle)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
        .max_transfer_sz = 4096 * 2,
    };

    ESP_LOGI(TAG, "DMA CHANNEL: %d", SPI_DMA_CHAN);
    ESP_ERROR_CHECK(spi_bus_initialize(HOST_ID, &bus_config, SPI_DMA_CHAN));

    const uint32_t spi_flags = SPI_DEVICE_HALFDUPLEX;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EXAMPLE_FLASH_FREQ_KHZ * 1000,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 10,
        .flags = spi_flags,
    };

    spi_device_handle_t spi;
    ESP_ERROR_CHECK(spi_bus_add_device(HOST_ID, &devcfg, &spi));

    spi_nand_flash_config_t nand_flash_config = {
        .device_handle = spi,
        .io_mode = SPI_NAND_IO_MODE_SIO,
        .flags = spi_flags
    };
    spi_nand_flash_device_t *nand_flash_device_handle;
    ESP_ERROR_CHECK(spi_nand_flash_init_device(&nand_flash_config, &nand_flash_device_handle));

    *out_handle = nand_flash_device_handle;
    *spi_handle = spi;
}

static void example_deinit_nand_flash(spi_nand_flash_device_t *flash, spi_device_handle_t spi)
{
    ESP_ERROR_CHECK(spi_nand_flash_deinit_device(flash));
    ESP_ERROR_CHECK(spi_bus_remove_device(spi));
    ESP_ERROR_CHECK(spi_bus_free(HOST_ID));
}

void app_main(void)
{
    spi_device_handle_t spi;
    spi_nand_flash_device_t *flash;
    example_init_nand_flash(&flash, &spi);

    uint32_t num_pages, page_size;
    ESP_ERROR_CHECK(spi_nand_flash_get_page_count(flash, &num_pages));
    ESP_ERROR_CHECK(spi_nand_flash_get_page_size(flash, &page_size));

    ESP_LOGI(TAG, "Flash: %" PRIu32 " pages x %" PRIu32 " bytes = %" PRIu32 " KB total",
             num_pages, page_size, num_pages * page_size / 1024);

    uint32_t bench_pages = (num_pages < BENCH_DEFAULT_NUM_PAGES) ? num_pages : BENCH_DEFAULT_NUM_PAGES;
    ESP_LOGI(TAG, "Benchmarking %" PRIu32 " of %" PRIu32 " logical pages (%"PRIu32" KB)",
             bench_pages, num_pages, bench_pages * page_size / 1024);

    ESP_LOGI(TAG, "Erasing chip...");
    ESP_ERROR_CHECK(spi_nand_erase_chip(flash));

    bench_cfg_t cfg = {
        .flash       = flash,
        .num_pages   = bench_pages,
        .num_passes  = BENCH_DEFAULT_NUM_PASSES,
        .page_size   = page_size,
        .verify_data = false,
    };

    if (cfg.num_passes > PERF_MAX_PASSES) {
        cfg.num_passes = PERF_MAX_PASSES;
        ESP_LOGW(TAG, "num_passes (%d) exceeds PERF_MAX_PASSES (%d), setting to %d", cfg.num_passes, PERF_MAX_PASSES, PERF_MAX_PASSES);
    }

    ESP_LOGI(TAG, "Warmup pass (not timed)...");
    ESP_ERROR_CHECK(perf_warmup(&cfg));

    bench_result_t results[2] = {0};

    results[0].name = "Sequential";
    ESP_LOGI(TAG, "Running sequential benchmark...");
    ESP_ERROR_CHECK(run_sequential_bench(&cfg, &results[0]));
    perf_print_result(&results[0]);

    results[1].name = "Random";
    ESP_LOGI(TAG, "Running random benchmark...");
    ESP_ERROR_CHECK(run_random_bench(&cfg, &results[1]));
    perf_print_result(&results[1]);

    perf_print_summary_table(results, 2);

    example_deinit_nand_flash(flash, spi);
    ESP_LOGI(TAG, "Done.");
}
