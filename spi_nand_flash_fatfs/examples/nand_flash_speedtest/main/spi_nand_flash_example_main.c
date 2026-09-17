/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * Benchmark for the four independently togglable read-path caches added to
 * spi_nand_flash / dhara on this branch:
 *
 *   - CONFIG_NAND_FLASH_PAGE_REGISTER_CACHE          driver level: skip the
 *     READ PAGE ADDRESS command when the requested page is already loaded
 *     in the NAND chip's internal register.
 *   - CONFIG_NAND_DHARA_FTL_MAP_PATH_CACHE           FTL level: skip
 *     radix-tree levels shared with the previous trace_path() lookup.
 *   - CONFIG_NAND_DHARA_FTL_MAP_EXACT_REPEAT_CACHE   FTL level (depends on
 *     the path cache above): fast-path an exact repeat of the same sector
 *     lookup (see dhara/dhara/dhara/map.c).
 *   - CONFIG_NAND_DHARA_JOURNAL_META_CACHE_SLOTS     journal level (below
 *     the FTL map, 0-8 slots): caches whole checkpoint pages to accelerate
 *     dhara_journal_read_meta() (see dhara/dhara/dhara/journal.c).
 *
 * Flip these options independently in menuconfig (or sdkconfig.ci for CI
 * matrices) and compare the timings this example prints for the same
 * workload.
 *
 * Two write/read workloads are run, back to back:
 *
 *   - "small files": many small files (SMALL_FILE_COUNT x SMALL_FILE_SIZE).
 *     Write throughput here is dominated by per-file FAT/journal overhead
 *     (directory entry + close-time sync paid once per file), not raw NAND
 *     bandwidth -- this workload mainly exercises the *read* caches, since
 *     ascending sector numbers across many small files share long common
 *     radix-tree prefixes in the dhara map.
 *   - "large files": few large files (LARGE_FILE_COUNT x LARGE_FILE_SIZE),
 *     each written/read in LARGE_CHUNK_SIZE chunks. Per-file overhead is
 *     amortized over much more data, so this is closer to raw sequential
 *     NAND throughput and a fairer comparison point for write performance.
 *
 * For each workload:
 *   - "cold sequential read": exercises the trace_path() prefix-cache path
 *     (ascending sector numbers share long common radix-tree prefixes).
 *   - "warm repeat read": re-reads the exact same files immediately after,
 *     exercising the page-register cache and the trace_path() exact-repeat
 *     fast path (repeated lookup of the same sector while the journal is
 *     unchanged).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "soc/spi_pins.h"
#include "esp_vfs_fat_nand.h"
#include "spi_nand_flash.h"

#define EXAMPLE_FLASH_FREQ_KHZ  40000

/* "small files" workload: small enough to keep the benchmark quick, large
 * enough (in file count) to walk many radix levels of the dhara
 * sector-to-page map. */
#define SMALL_FILE_COUNT    64
#define SMALL_FILE_SIZE     (4 * 1024)

/* "large files" workload: few files, written/read in chunks matching the
 * FAT allocation_unit_size below, to measure throughput with per-file
 * overhead amortized over more data. */
#define LARGE_FILE_COUNT    4
#define LARGE_FILE_SIZE     (256 * 1024)
#define LARGE_CHUNK_SIZE    (16 * 1024)

static const char *TAG = "speedtest";

// Pin mapping
// ESP32 (VSPI)
#ifdef CONFIG_IDF_TARGET_ESP32
#define HOST_ID  SPI3_HOST
#define PIN_MOSI SPI3_IOMUX_PIN_NUM_MOSI
#define PIN_MISO SPI3_IOMUX_PIN_NUM_MISO
#define PIN_CLK  SPI3_IOMUX_PIN_NUM_CLK
#define PIN_CS   SPI3_IOMUX_PIN_NUM_CS
#define PIN_WP   SPI3_IOMUX_PIN_NUM_WP
#define PIN_HD   SPI3_IOMUX_PIN_NUM_HD
#define SPI_DMA_CHAN SPI_DMA_CH_AUTO
#else // Other chips (SPI2/HSPI)
#define HOST_ID  SPI2_HOST
#define PIN_MOSI SPI2_IOMUX_PIN_NUM_MOSI
#define PIN_MISO SPI2_IOMUX_PIN_NUM_MISO
#define PIN_CLK  SPI2_IOMUX_PIN_NUM_CLK
#define PIN_CS   SPI2_IOMUX_PIN_NUM_CS
#define PIN_WP   SPI2_IOMUX_PIN_NUM_WP
#define PIN_HD   SPI2_IOMUX_PIN_NUM_HD
#define SPI_DMA_CHAN SPI_DMA_CH_AUTO
#endif

// Mount path for the partition
static const char *base_path = "/nandflash";

static void log_cache_config(void)
{
    ESP_LOGI(TAG, "--- Cache layers under test ---");
#ifdef CONFIG_NAND_FLASH_PAGE_REGISTER_CACHE
    ESP_LOGI(TAG, "driver page-register cache : ON");
#else
    ESP_LOGI(TAG, "driver page-register cache : off");
#endif
#ifdef CONFIG_NAND_DHARA_FTL_MAP_PATH_CACHE
    ESP_LOGI(TAG, "dhara map path cache       : ON");
#else
    ESP_LOGI(TAG, "dhara map path cache       : off");
#endif
#ifdef CONFIG_NAND_DHARA_FTL_MAP_EXACT_REPEAT_CACHE
    ESP_LOGI(TAG, "dhara map exact-repeat     : ON");
#else
    ESP_LOGI(TAG, "dhara map exact-repeat     : off");
#endif
#if defined(CONFIG_NAND_DHARA_JOURNAL_META_CACHE_SLOTS) && CONFIG_NAND_DHARA_JOURNAL_META_CACHE_SLOTS > 0
    ESP_LOGI(TAG, "dhara journal meta cache   : ON  (%d slots)", CONFIG_NAND_DHARA_JOURNAL_META_CACHE_SLOTS);
#else
    ESP_LOGI(TAG, "dhara journal meta cache   : off");
#endif
    ESP_LOGI(TAG, "--------------------------------");
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

/* Writes file_count files of file_size bytes each, sequentially, in
 * chunk_size chunks. buf must be at least chunk_size bytes. */
static int64_t write_files(const char *prefix, int file_count, size_t file_size,
                            size_t chunk_size, uint8_t *buf)
{
    memset(buf, 0xA5, chunk_size);

    char path[64];
    int64_t t0 = esp_timer_get_time();

    for (int i = 0; i < file_count; i++) {
        snprintf(path, sizeof(path), "%s/%s%03d.bin", base_path, prefix, i);
        FILE *f = fopen(path, "wb");
        if (f == NULL) {
            ESP_LOGE(TAG, "fopen write failed (%s): errno=%d", path, errno);
            return -1;
        }

        size_t remaining = file_size;
        while (remaining > 0) {
            size_t n = remaining < chunk_size ? remaining : chunk_size;
            size_t written = fwrite(buf, 1, n, f);
            if (written != n) {
                ESP_LOGE(TAG, "fwrite failed (%s): wrote %u/%u", path, (unsigned)written, (unsigned)n);
                fclose(f);
                return -1;
            }
            remaining -= written;
        }

        fclose(f);
    }

    return esp_timer_get_time() - t0;
}

/* Reads file_count files back sequentially in chunk_size chunks and
 * returns elapsed time. buf must be at least chunk_size bytes. */
static int64_t read_files(const char *prefix, int file_count, size_t file_size,
                           size_t chunk_size, uint8_t *buf)
{
    char path[64];
    int64_t t0 = esp_timer_get_time();

    for (int i = 0; i < file_count; i++) {
        snprintf(path, sizeof(path), "%s/%s%03d.bin", base_path, prefix, i);
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            ESP_LOGE(TAG, "fopen read failed (%s): errno=%d", path, errno);
            return -1;
        }

        size_t remaining = file_size;
        while (remaining > 0) {
            size_t n = remaining < chunk_size ? remaining : chunk_size;
            size_t read_bytes = fread(buf, 1, n, f);
            if (read_bytes != n) {
                ESP_LOGE(TAG, "fread failed (%s): read %u/%u", path, (unsigned)read_bytes, (unsigned)n);
                fclose(f);
                return -1;
            }
            remaining -= read_bytes;
        }

        fclose(f);
    }

    return esp_timer_get_time() - t0;
}

static void report(const char *label, int64_t us, int file_count, size_t file_size)
{
    if (us < 0) {
        ESP_LOGE(TAG, "%s: FAILED", label);
        return;
    }
    const int64_t total_bytes = (int64_t)file_count * file_size;
    ESP_LOGI(TAG, "%-24s %8" PRId64 " us  (%.1f files/s, %.1f kB/s)",
             label, us,
             (float)file_count / ((float)us / 1000000.0f),
             (float)total_bytes / 1024.0f / ((float)us / 1000000.0f));
}

/* Runs write / cold read / warm read for one workload and prints a summary. */
static void run_workload(const char *name, const char *prefix, int file_count,
                          size_t file_size, size_t chunk_size, uint8_t *buf)
{
    ESP_LOGI(TAG, "=== %s workload (%d x %u bytes) ===", name, file_count, (unsigned)file_size);

    int64_t write_us = write_files(prefix, file_count, file_size, chunk_size, buf);
    report("sequential write", write_us, file_count, file_size);

    int64_t cold_read_us = read_files(prefix, file_count, file_size, chunk_size, buf);
    report("cold sequential read", cold_read_us, file_count, file_size);

    int64_t warm_read_us = read_files(prefix, file_count, file_size, chunk_size, buf);
    report("warm repeat read", warm_read_us, file_count, file_size);

    if (cold_read_us > 0 && warm_read_us > 0) {
        ESP_LOGI(TAG, "warm/cold speedup: %.2fx", (float)cold_read_us / (float)warm_read_us);
    }
}

void app_main(void)
{
    log_cache_config();

    spi_device_handle_t spi;
    spi_nand_flash_device_t *flash;
    example_init_nand_flash(&flash, &spi);
    if (flash == NULL) {
        return;
    }

    esp_vfs_fat_mount_config_t config = {
        .max_files = 8,
        .format_if_mount_failed = true,
        .allocation_unit_size = LARGE_CHUNK_SIZE,
    };

    esp_err_t ret = esp_vfs_fat_nand_mount(base_path, flash, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount filesystem: %s", esp_err_to_name(ret));
        return;
    }

    uint64_t bytes_total, bytes_free;
    esp_vfs_fat_info(base_path, &bytes_total, &bytes_free);
    ESP_LOGI(TAG, "FAT FS: %" PRIu64 " kB total, %" PRIu64 " kB free", bytes_total / 1024, bytes_free / 1024);

    /* Shared chunk buffer, sized for the larger of the two workloads. */
    static uint8_t chunk_buf[LARGE_CHUNK_SIZE];

    run_workload("small files", "s", SMALL_FILE_COUNT, SMALL_FILE_SIZE, SMALL_FILE_SIZE, chunk_buf);
    run_workload("large files", "l", LARGE_FILE_COUNT, LARGE_FILE_SIZE, LARGE_CHUNK_SIZE, chunk_buf);

    esp_vfs_fat_nand_unmount(base_path, flash);
    example_deinit_nand_flash(flash, spi);

    ESP_LOGI(TAG, "Benchmark done");
}
