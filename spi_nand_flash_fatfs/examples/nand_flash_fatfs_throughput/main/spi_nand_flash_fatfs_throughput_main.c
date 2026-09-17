/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Testing-only example: sequential FatFs write/read throughput on SPI NAND
 * (legacy path). Runs a baseline (stdio, unbuffered) and an optimized path
 * (POSIX read/write) so the effect of I/O size and API choice is visible.
 * Compare with nand_flash_debug_app for Dhara/page-layer numbers.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat_nand.h"
#include "nand_diag_api.h"
#include "soc/spi_pins.h"

/* Edit these to match board / chip limits when comparing SPI link speed. */
#define EXAMPLE_FLASH_FREQ_KHZ      80000
#define EXAMPLE_IO_MODE             SPI_NAND_IO_MODE_QIO

#define EXAMPLE_TEST_FILE_SIZE      (2 * 1024 * 1024)
/* Larger cluster → fewer FAT walks on sequential I/O (reformat required). */
#define EXAMPLE_ALLOC_UNIT_SIZE     (32 * 1024)
#define EXAMPLE_TEST_FILE_PATH      "/nandflash/tp.bin"

static const char *TAG = "fatfs_tp";

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

static const char *base_path = "/nandflash";

static const size_t s_chunk_sizes[] = {
    512,
    4 * 1024,
    16 * 1024,
    EXAMPLE_ALLOC_UNIT_SIZE,
};

static const char *io_mode_str(spi_nand_flash_io_mode_t mode)
{
    switch (mode) {
    case SPI_NAND_IO_MODE_SIO:
        return "SIO";
    case SPI_NAND_IO_MODE_DOUT:
        return "DOUT";
    case SPI_NAND_IO_MODE_DIO:
        return "DIO";
    case SPI_NAND_IO_MODE_QOUT:
        return "QOUT";
    case SPI_NAND_IO_MODE_QIO:
        return "QIO";
    default:
        return "?";
    }
}

static void example_init_nand_flash(spi_nand_flash_device_t **out_handle, spi_device_handle_t *spi_handle)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadhd_io_num = PIN_HD,
        .quadwp_io_num = PIN_WP,
        .max_transfer_sz = 4096 * 2,
    };

    ESP_LOGI(TAG, "DMA CHANNEL: %d", SPI_DMA_CHAN);
    ESP_ERROR_CHECK(spi_bus_initialize(HOST_ID, &bus_config, SPI_DMA_CHAN));

    /* Half-duplex required for DIO/DOUT/QIO/QOUT. */
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
        .io_mode = EXAMPLE_IO_MODE,
        .flags = spi_flags,
    };
    assert(devcfg.flags == nand_flash_config.flags);

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

static void log_throughput(const char *label, size_t chunk_size, int64_t write_time, int64_t read_time)
{
    ESP_LOGI(TAG, "[%s] chunk=%u: Wrote %d bytes in %" PRId64 " us, avg %.2f kB/s",
             label, (unsigned)chunk_size, EXAMPLE_TEST_FILE_SIZE, write_time,
             (float)EXAMPLE_TEST_FILE_SIZE / write_time * 1000.0f);
    ESP_LOGI(TAG, "[%s] chunk=%u: Read %d bytes in %" PRId64 " us, avg %.2f kB/s\n",
             label, (unsigned)chunk_size, EXAMPLE_TEST_FILE_SIZE, read_time,
             (float)EXAMPLE_TEST_FILE_SIZE / read_time * 1000.0f);
}

static void log_perf_stats(const char *operation, const spi_nand_flash_perf_stats_t *stats)
{
    uint64_t cache_accesses = stats->metadata_cache_hits + stats->metadata_cache_misses;
    double hit_rate = cache_accesses ?
                      (double)stats->metadata_cache_hits * 100.0 / (double)cache_accesses : 0.0;

    ESP_LOGI(TAG, "%s stats: reads=%" PRIu64 ", programs=%" PRIu64
             ", copies=%" PRIu64 ", erases=%" PRIu64
             ", metadata hits=%" PRIu64 ", misses=%" PRIu64 " (%.1f%% hit)",
             operation, stats->physical_reads, stats->physical_programs,
             stats->physical_copies, stats->physical_erases,
             stats->metadata_cache_hits, stats->metadata_cache_misses, hit_rate);
}

/* Baseline: stdio + unbuffered — each fwrite/fread hits FatFs (small-I/O stress). */
static esp_err_t fatfs_throughput_stdio(spi_nand_flash_device_t *flash, size_t chunk_size)
{
    esp_err_t ret = ESP_OK;
    uint8_t *buf = NULL;
    FILE *f = NULL;
    spi_nand_flash_perf_stats_t write_stats = {0};
    spi_nand_flash_perf_stats_t read_stats = {0};

    ESP_RETURN_ON_FALSE(EXAMPLE_TEST_FILE_SIZE % chunk_size == 0, ESP_ERR_INVALID_ARG, TAG,
                        "file size must be a multiple of chunk size");

    buf = (uint8_t *)heap_caps_aligned_alloc(64, chunk_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_NO_MEM, TAG, "nomem");
    memset(buf, 0xA5, chunk_size);
    unlink(EXAMPLE_TEST_FILE_PATH);

    ESP_GOTO_ON_ERROR(nand_reset_perf_stats(flash), fail, TAG, "");
    int64_t start = esp_timer_get_time();
    f = fopen(EXAMPLE_TEST_FILE_PATH, "wb");
    ESP_GOTO_ON_FALSE(f != NULL, ESP_FAIL, fail, TAG, "fopen(wb) failed");
    setvbuf(f, NULL, _IONBF, 0);

    for (size_t done = 0; done < EXAMPLE_TEST_FILE_SIZE; done += chunk_size) {
        ESP_GOTO_ON_FALSE(fwrite(buf, 1, chunk_size, f) == chunk_size, ESP_FAIL, fail, TAG, "fwrite short");
    }
    ESP_GOTO_ON_FALSE(fclose(f) == 0, ESP_FAIL, fail, TAG, "fclose(write) failed");
    f = NULL;
    int64_t write_time = esp_timer_get_time() - start;
    ESP_GOTO_ON_ERROR(nand_get_perf_stats(flash, &write_stats), fail, TAG, "");

    memset(buf, 0x00, chunk_size);
    ESP_GOTO_ON_ERROR(nand_reset_perf_stats(flash), fail, TAG, "");
    start = esp_timer_get_time();
    f = fopen(EXAMPLE_TEST_FILE_PATH, "rb");
    ESP_GOTO_ON_FALSE(f != NULL, ESP_FAIL, fail, TAG, "fopen(rb) failed");
    setvbuf(f, NULL, _IONBF, 0);

    for (size_t done = 0; done < EXAMPLE_TEST_FILE_SIZE; done += chunk_size) {
        ESP_GOTO_ON_FALSE(fread(buf, 1, chunk_size, f) == chunk_size, ESP_FAIL, fail, TAG, "fread short");
    }
    ESP_GOTO_ON_FALSE(fclose(f) == 0, ESP_FAIL, fail, TAG, "fclose(read) failed");
    f = NULL;
    int64_t read_time = esp_timer_get_time() - start;
    ESP_GOTO_ON_ERROR(nand_get_perf_stats(flash, &read_stats), fail, TAG, "");

    log_throughput("stdio/_IONBF", chunk_size, write_time, read_time);
    log_perf_stats("write", &write_stats);
    log_perf_stats("read", &read_stats);
    unlink(EXAMPLE_TEST_FILE_PATH);

fail:
    if (f != NULL) {
        fclose(f);
    }
    free(buf);
    return ret;
}

/*
 * Optimized: POSIX read/write (no stdio layer). Prefer chunk == cluster size.
 * close() is included in the timed path so FatFs/Dhara flush is counted.
 */
static esp_err_t fatfs_throughput_posix(spi_nand_flash_device_t *flash, size_t chunk_size)
{
    esp_err_t ret = ESP_OK;
    uint8_t *buf = NULL;
    int fd = -1;
    spi_nand_flash_perf_stats_t write_stats = {0};
    spi_nand_flash_perf_stats_t read_stats = {0};

    ESP_RETURN_ON_FALSE(EXAMPLE_TEST_FILE_SIZE % chunk_size == 0, ESP_ERR_INVALID_ARG, TAG,
                        "file size must be a multiple of chunk size");

    buf = (uint8_t *)heap_caps_aligned_alloc(64, chunk_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_NO_MEM, TAG, "nomem");
    memset(buf, 0xA5, chunk_size);
    unlink(EXAMPLE_TEST_FILE_PATH);

    ESP_GOTO_ON_ERROR(nand_reset_perf_stats(flash), fail, TAG, "");
    int64_t start = esp_timer_get_time();
    fd = open(EXAMPLE_TEST_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    ESP_GOTO_ON_FALSE(fd >= 0, ESP_FAIL, fail, TAG, "open(O_WRONLY) failed: %d", errno);

    for (size_t done = 0; done < EXAMPLE_TEST_FILE_SIZE; done += chunk_size) {
        ssize_t n = write(fd, buf, chunk_size);
        ESP_GOTO_ON_FALSE(n == (ssize_t)chunk_size, ESP_FAIL, fail, TAG, "write short (%d)", (int)n);
    }
    ESP_GOTO_ON_FALSE(close(fd) == 0, ESP_FAIL, fail, TAG, "close(write) failed");
    fd = -1;
    int64_t write_time = esp_timer_get_time() - start;
    ESP_GOTO_ON_ERROR(nand_get_perf_stats(flash, &write_stats), fail, TAG, "");

    memset(buf, 0x00, chunk_size);
    ESP_GOTO_ON_ERROR(nand_reset_perf_stats(flash), fail, TAG, "");
    start = esp_timer_get_time();
    fd = open(EXAMPLE_TEST_FILE_PATH, O_RDONLY);
    ESP_GOTO_ON_FALSE(fd >= 0, ESP_FAIL, fail, TAG, "open(O_RDONLY) failed: %d", errno);

    for (size_t done = 0; done < EXAMPLE_TEST_FILE_SIZE; done += chunk_size) {
        ssize_t n = read(fd, buf, chunk_size);
        ESP_GOTO_ON_FALSE(n == (ssize_t)chunk_size, ESP_FAIL, fail, TAG, "read short (%d)", (int)n);
    }
    ESP_GOTO_ON_FALSE(close(fd) == 0, ESP_FAIL, fail, TAG, "close(read) failed");
    fd = -1;
    int64_t read_time = esp_timer_get_time() - start;
    ESP_GOTO_ON_ERROR(nand_get_perf_stats(flash, &read_stats), fail, TAG, "");

    log_throughput("posix read/write", chunk_size, write_time, read_time);
    log_perf_stats("write", &write_stats);
    log_perf_stats("read", &read_stats);
    unlink(EXAMPLE_TEST_FILE_PATH);

fail:
    if (fd >= 0) {
        close(fd);
    }
    free(buf);
    return ret;
}

static esp_err_t run_chunk_matrix(spi_nand_flash_device_t *flash,
                                  esp_err_t (*fn)(spi_nand_flash_device_t *, size_t),
                                  const char *phase)
{
    ESP_LOGI(TAG, "======== %s ========", phase);
    for (size_t i = 0; i < sizeof(s_chunk_sizes) / sizeof(s_chunk_sizes[0]); i++) {
        /* Skip duplicate if 16 KiB == cluster size. */
        if (i > 0 && s_chunk_sizes[i] == s_chunk_sizes[i - 1]) {
            continue;
        }
        esp_err_t ret = fn(flash, s_chunk_sizes[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s failed for chunk %u: %s",
                     phase, (unsigned)s_chunk_sizes[i], esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
}

void app_main(void)
{
    spi_device_handle_t spi;
    spi_nand_flash_device_t *flash;
    example_init_nand_flash(&flash, &spi);
    if (flash == NULL) {
        return;
    }

    ESP_LOGW(TAG, "Erasing the entire NAND so this destructive benchmark starts from a known state");
    ESP_ERROR_CHECK(spi_nand_erase_chip(flash));

    esp_vfs_fat_mount_config_t config = {
        .max_files = 4,
        /* true so EXAMPLE_ALLOC_UNIT_SIZE applies on first bring-up / failed mount. */
        .format_if_mount_failed = true,
        .allocation_unit_size = EXAMPLE_ALLOC_UNIT_SIZE,
    };

    esp_err_t ret = esp_vfs_fat_nand_mount(base_path, flash, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_nand_mount failed: %s", esp_err_to_name(ret));
        example_deinit_nand_flash(flash, spi);
        return;
    }

    uint64_t bytes_total, bytes_free;
    esp_vfs_fat_info(base_path, &bytes_total, &bytes_free);
    ESP_LOGI(TAG, "FAT FS: %" PRIu64 " kB total, %" PRIu64 " kB free",
             bytes_total / 1024, bytes_free / 1024);
    ESP_LOGI(TAG, "SPI: %d kHz, %s; cluster=%d; test file=%d bytes",
             EXAMPLE_FLASH_FREQ_KHZ, io_mode_str(EXAMPLE_IO_MODE),
             EXAMPLE_ALLOC_UNIT_SIZE, EXAMPLE_TEST_FILE_SIZE);
    ESP_LOGI(TAG, "Volume was erased before mount; format uses the configured allocation unit");

    ret = run_chunk_matrix(flash, fatfs_throughput_stdio, "baseline: stdio unbuffered");
    if (ret == ESP_OK) {
        ret = run_chunk_matrix(flash, fatfs_throughput_posix, "optimized: POSIX read/write");
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "throughput suite failed: %s", esp_err_to_name(ret));
    }

    esp_vfs_fat_nand_unmount(base_path, flash);
    example_deinit_nand_flash(flash, spi);
}
