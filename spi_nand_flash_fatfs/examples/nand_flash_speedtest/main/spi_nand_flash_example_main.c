/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "soc/spi_pins.h"
#include "esp_vfs_fat_nand.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spi_nand_flash.h"

#include <sys/stat.h>

//#define BOARD_NAND_FLASH_TEST

#ifndef BOARD_NAND_FLASH_TEST
#define EXAMPLE_FLASH_FREQ_KHZ      80000
#else
#define EXAMPLE_FLASH_FREQ_KHZ      80000
#endif
#define PATTERN_SEED    0x12345678

/* 1 = sequential R/W throughput, 0 = original small-file fill loop */
#define EXAMPLE_MEASURE_THROUGHPUT  1

#if EXAMPLE_MEASURE_THROUGHPUT
#define THROUGHPUT_CHUNK_SIZE       (16 * 1024)          /* match allocation_unit_size */
#define THROUGHPUT_TOTAL_BYTES      (4 * 1024 * 1024)    /* 4 MiB per file */
#define THROUGHPUT_READ_BURST_EVERY 5                    /* burst-read all files after N writes */
#define THROUGHPUT_MAX_TRACKED      64                  /* max paths kept for burst reads */
#endif

static const char *TAG = "example";

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
#ifndef BOARD_NAND_FLASH_TEST
#define PIN_MOSI SPI2_IOMUX_PIN_NUM_MOSI
#define PIN_MISO SPI2_IOMUX_PIN_NUM_MISO
#define PIN_CLK  SPI2_IOMUX_PIN_NUM_CLK
#define PIN_CS   SPI2_IOMUX_PIN_NUM_CS
#define PIN_WP   SPI2_IOMUX_PIN_NUM_WP
#define PIN_HD   SPI2_IOMUX_PIN_NUM_HD
#else
#define PIN_MOSI 11
#define PIN_MISO 13
#define PIN_CLK  12
#define PIN_CS   10
#define PIN_WP   14
#define PIN_HD   9
#endif
#define SPI_DMA_CHAN SPI_DMA_CH_AUTO
#endif

// Mount path for the partition
const char *base_path = "/sdcard";

static void example_init_nand_flash(spi_nand_flash_device_t **out_handle, spi_device_handle_t *spi_handle)
{
#ifndef BOARD_NAND_FLASH_TEST
    const spi_bus_config_t bus_config = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
        .max_transfer_sz = 4096 * 2,
    };
#else
    const spi_bus_config_t bus_config = {
        .data0_io_num = PIN_MOSI,
        .data1_io_num = PIN_MISO,
        .data2_io_num = PIN_WP,
        .data3_io_num = PIN_HD,
        .sclk_io_num = PIN_CLK,
        .max_transfer_sz = 4096 * 2,
    };
#endif

    // Initialize the SPI bus
    ESP_LOGI(TAG, "DMA CHANNEL: %d", SPI_DMA_CHAN);
    ESP_ERROR_CHECK(spi_bus_initialize(HOST_ID, &bus_config, SPI_DMA_CHAN));

    // spi_flags = SPI_DEVICE_HALFDUPLEX -> half duplex
    // spi_flags = 0 -> full_duplex
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
#ifndef BOARD_NAND_FLASH_TEST
        .io_mode = SPI_NAND_IO_MODE_SIO,
#else
        .io_mode = SPI_NAND_IO_MODE_QIO,
#endif
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

static void reclaim_space(const char *dir_path, spi_nand_flash_device_t *flash)
{
    ESP_LOGI(TAG, "Low free space, reclaiming...");

    // Delete all files in the data directory
    DIR *d = opendir(dir_path);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_type == DT_REG) {
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, ent->d_name);
                if (remove(filepath) == 0) {
                    ESP_LOGI(TAG, "Deleted: %s", filepath);
                } else {
                    ESP_LOGW(TAG, "Failed to delete: %s, errno=%d", filepath, errno);
                }
            }
        }
        closedir(d);
    }

    // Sync flushes cache and triggers Dhara GC on remaining garbage pages
    spi_nand_flash_sync(flash);

    uint64_t bytes_total, bytes_free;
    esp_vfs_fat_info(base_path, &bytes_total, &bytes_free);
    ESP_LOGI(TAG, "After reclaim: %" PRIu64 " kB total, %" PRIu64 " kB free",
             bytes_total / 1024, bytes_free / 1024);

    // If still too little free space, erase chip and reformat
    if (bytes_free < bytes_total / 10) {
        ESP_LOGW(TAG, "Still low space after sync, erasing chip and reformatting...");
        esp_vfs_fat_nand_unmount(base_path, flash);

        esp_err_t erase_ret = spi_nand_erase_chip(flash);
        if (erase_ret == ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "Some bad blocks during erase, continuing...");
        } else if (erase_ret != ESP_OK) {
            ESP_ERROR_CHECK(erase_ret);
        }

        esp_vfs_fat_mount_config_t config = {
            .max_files = 20,
            .format_if_mount_failed = true,
            .allocation_unit_size = 16 * 1024
        };
        ESP_ERROR_CHECK(esp_vfs_fat_nand_mount(base_path, flash, &config));

        esp_vfs_fat_info(base_path, &bytes_total, &bytes_free);
        ESP_LOGI(TAG, "After reformat: %" PRIu64 " kB total, %" PRIu64 " kB free",
                 bytes_total / 1024, bytes_free / 1024);
    }
}

void app_main(void)
{
    esp_err_t ret;
    // Set up SPI bus and initialize the external SPI Flash chip
    spi_device_handle_t spi;
    spi_nand_flash_device_t *flash;
    example_init_nand_flash(&flash, &spi);
    if (flash == NULL) {
        return;
    }

    esp_vfs_fat_mount_config_t config = {
        .max_files = 20,
        .format_if_mount_failed = true,
        .allocation_unit_size = 16 * 1024
    };

    #if 1
    // Erase chip before first use to ensure clean state
    ESP_LOGI(TAG, "Erasing NAND flash chip...");
    esp_err_t erase_ret = spi_nand_erase_chip(flash);
    if (erase_ret == ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "Some bad blocks encountered during erase, continuing...");
    } else if (erase_ret != ESP_OK) {
        ESP_ERROR_CHECK(erase_ret);
    }
    ESP_LOGI(TAG, "NAND flash chip erased");
    #else
    ESP_LOGI(TAG, "Skipping NAND flash chip erase");
    #endif

    ret = esp_vfs_fat_nand_mount(base_path, flash, &config);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want the flash memory to be formatted, set the CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        }
        return;
    }

    // Print FAT FS size information
    uint64_t bytes_total, bytes_free;
    esp_vfs_fat_info(base_path, &bytes_total, &bytes_free);
    ESP_LOGI(TAG, "FAT FS: %" PRIu64 " kB total, %" PRIu64 " kB free", bytes_total / 1024, bytes_free / 1024);

    const char *data_dir = "/sdcard/data";

    struct stat st = {0};
    if (stat(data_dir, &st) == -1) {
        mkdir(data_dir, 0777);
    }
    ESP_LOGI(TAG, "Directory created");

#if EXAMPLE_MEASURE_THROUGHPUT
    static uint8_t chunk[THROUGHPUT_CHUNK_SIZE];
    static char path0[64];
    static char tracked[THROUGHPUT_MAX_TRACKED][64];
    int tracked_count = 0;
    int tick;
    int round = 0;
    int writes_since_burst = 0;

    memset(chunk, 0xAA, sizeof(chunk));

    while (1) {
        tick = (int)xTaskGetTickCount();
        ESP_LOGI(TAG, "=== Throughput write round %d, tick %d ===", round, tick);

        snprintf(path0, sizeof(path0), "%s/%d.bin", data_dir, tick);

        /* Reclaim before write if not enough room for another full file */
        esp_vfs_fat_info(base_path, &bytes_total, &bytes_free);
        if (bytes_free < THROUGHPUT_TOTAL_BYTES) {
            reclaim_space(data_dir, flash);
            tracked_count = 0;
            writes_since_burst = 0;
        }

        /* --- write --- */
        FILE *f = fopen(path0, "wb");
        if (f == NULL) {
            ESP_LOGE(TAG, "fopen write failed: errno=%d", errno);
            reclaim_space(data_dir, flash);
            tracked_count = 0;
            writes_since_burst = 0;
            continue;
        }

        int64_t t0 = esp_timer_get_time();
        size_t written = 0;
        while (written < THROUGHPUT_TOTAL_BYTES) {
            size_t n = fwrite(chunk, 1, sizeof(chunk), f);
            if (n != sizeof(chunk)) {
                ESP_LOGE(TAG, "fwrite failed at %u: errno=%d", (unsigned)written, errno);
                fclose(f);
                reclaim_space(data_dir, flash);
                tracked_count = 0;
                writes_since_burst = 0;
                break;
            }
            written += n;
        }
        if (written < THROUGHPUT_TOTAL_BYTES) {
            round++;
            continue;
        }
        fflush(f);
        fsync(fileno(f));
        fclose(f);
        int64_t write_us = esp_timer_get_time() - t0;

        ESP_LOGI(TAG, "Wrote %s (%u bytes) in %" PRId64 " us, avg %.2f kB/s",
                 path0, (unsigned)written, write_us,
                 (float)written / (float)write_us * 1000.0f);

        /* --- immediate read-back of this file --- */
        f = fopen(path0, "rb");
        if (f == NULL) {
            ESP_LOGE(TAG, "fopen read failed: errno=%d", errno);
            return;
        }

        t0 = esp_timer_get_time();
        size_t read_bytes = 0;
        while (read_bytes < THROUGHPUT_TOTAL_BYTES) {
            size_t n = fread(chunk, 1, sizeof(chunk), f);
            if (n != sizeof(chunk)) {
                ESP_LOGE(TAG, "fread failed at %u: errno=%d", (unsigned)read_bytes, errno);
                fclose(f);
                return;
            }
            read_bytes += n;
        }
        fclose(f);
        int64_t read_us = esp_timer_get_time() - t0;

        ESP_LOGI(TAG, "Read  %s (%u bytes) in %" PRId64 " us, avg %.2f kB/s",
                 path0, (unsigned)read_bytes, read_us,
                 (float)read_bytes / (float)read_us * 1000.0f);

        if (tracked_count < THROUGHPUT_MAX_TRACKED) {
            strncpy(tracked[tracked_count], path0, sizeof(tracked[0]) - 1);
            tracked[tracked_count][sizeof(tracked[0]) - 1] = '\0';
            tracked_count++;
        } else {
            ESP_LOGW(TAG, "Tracked file list full (%d), burst will skip older files",
                     THROUGHPUT_MAX_TRACKED);
        }
        writes_since_burst++;

        esp_vfs_fat_info(base_path, &bytes_total, &bytes_free);
        ESP_LOGI(TAG, "FAT FS: %" PRIu64 " kB total, %" PRIu64 " kB free",
                 bytes_total / 1024, bytes_free / 1024);

        /* Every N successful writes: burst-read all tracked files */
        if (writes_since_burst >= THROUGHPUT_READ_BURST_EVERY && tracked_count > 0) {
            ESP_LOGI(TAG, "=== Read burst: %d files ===", tracked_count);

            int64_t burst_t0 = esp_timer_get_time();
            size_t burst_bytes = 0;
            int burst_ok = 1;

            for (int i = 0; i < tracked_count; i++) {
                ESP_LOGI(TAG, "Burst read %d/%d: %s ...", i + 1, tracked_count, tracked[i]);

                f = fopen(tracked[i], "rb");
                if (f == NULL) {
                    ESP_LOGE(TAG, "fopen read failed (%s): errno=%d", tracked[i], errno);
                    burst_ok = 0;
                    break;
                }

                int64_t file_t0 = esp_timer_get_time();
                read_bytes = 0;
                while (read_bytes < THROUGHPUT_TOTAL_BYTES) {
                    size_t n = fread(chunk, 1, sizeof(chunk), f);
                    if (n != sizeof(chunk)) {
                        ESP_LOGE(TAG, "fread failed (%s) at %u: errno=%d",
                                 tracked[i], (unsigned)read_bytes, errno);
                        fclose(f);
                        burst_ok = 0;
                        break;
                    }
                    read_bytes += n;
                }
                fclose(f);
                if (!burst_ok) {
                    break;
                }

                int64_t file_us = esp_timer_get_time() - file_t0;
                ESP_LOGI(TAG, "Burst read %d/%d: %s done (%u bytes) in %" PRId64 " us, avg %.2f kB/s",
                         i + 1, tracked_count, tracked[i], (unsigned)read_bytes, file_us,
                         (float)read_bytes / (float)file_us * 1000.0f);

                burst_bytes += read_bytes;
            }

            if (burst_ok) {
                int64_t burst_us = esp_timer_get_time() - burst_t0;
                ESP_LOGI(TAG, "Read burst done: %d files, %u bytes in %" PRId64 " us, avg %.2f kB/s",
                         tracked_count, (unsigned)burst_bytes, burst_us,
                         (float)burst_bytes / (float)burst_us * 1000.0f);
            }

            writes_since_burst = 0;
        }

        round++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    static char path0[64];
    static char line[128];
    char *pos;
    int tick;
    int round = 0;
    FILE *File;

    while (1) {
        tick = (int)xTaskGetTickCount();
        ESP_LOGI(TAG, "=== Round %d, tick %d ===", round, tick);

        snprintf(path0, 64, "%s/%d.txt", data_dir, tick);

        // Write file
        File = fopen(path0, "wb");
        if (File)
        {
            static char buffer[1024];
            memset(buffer, 0xAA, sizeof(buffer));
            size_t written = fwrite(buffer, 1, sizeof(buffer), File);
            ESP_LOGI(TAG, "Write size: %d", (int)written);
            fclose(File);
            ESP_LOGI(TAG, "File written: %s", path0);
        }
        else
        {
            ESP_LOGE(TAG, "fopen failed: errno=%d", errno);
            while(1)
            {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        esp_vfs_fat_info(base_path, &bytes_total, &bytes_free);
        ESP_LOGI(TAG, "FAT FS: %" PRIu64 " kB total, %" PRIu64 " kB free", bytes_total / 1024, bytes_free / 1024);

        // Proactively reclaim space when free space drops below 10%
        if (bytes_free == 0) {
            reclaim_space(data_dir, flash);
        }

        round++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}
