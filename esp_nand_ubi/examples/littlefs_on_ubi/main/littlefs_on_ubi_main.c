/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/**
 * LittleFS-on-UBI example.
 *
 * Mounts joltwallet/littlefs directly on the esp_nand_ubi volume block device
 * (nand_ubi_get_blockdev()) -- no Dhara wear-leveling BDL anywhere in this
 * chain, and no filesystem adapter/shim to write: joltwallet/littlefs already
 * speaks esp_blockdev_t natively (esp_vfs_littlefs_conf_t.blockdev).
 *
 * Stack:
 *   SPI NAND hardware
 *     -> nand_flash_get_blockdev()        raw flash BDL         [spi_nand_flash]
 *     -> nand_ubi_get_blockdev()          UBI volume BDL        [esp_nand_ubi]
 *     -> esp_vfs_littlefs_register()      POSIX file API        [joltwallet/littlefs]
 *
 * See this example's README.md for the block_cycles decision (left at
 * joltwallet's default -- it only relocates LittleFS's metadata pair, and is
 * currently the only active wear-spreading mechanism in this stack since
 * esp_nand_ubi has no wear-leveling yet) and the known LFS_ERR_CORRUPT
 * mapping gap in joltwallet's littlefs_bdl.c adapter (also documented in
 * esp_nand_ubi/README.md).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "soc/spi_pins.h"

#include "spi_nand_flash.h"
#include "esp_nand_blockdev.h"
#include "esp_nand_ubi.h"
#include "esp_littlefs.h"
#include "nand_ubi_ram_stats.h"

static const char *TAG = "example";

/* Pin mapping mirrors examples/nand_ubi_example: ESP32 uses VSPI (SPI3_HOST)
 * since HSPI/SPI2 is often reserved for PSRAM; every other target uses
 * SPI2_HOST. */
#ifdef CONFIG_IDF_TARGET_ESP32
#define HOST_ID  SPI3_HOST
#define PIN_MOSI SPI3_IOMUX_PIN_NUM_MOSI
#define PIN_MISO SPI3_IOMUX_PIN_NUM_MISO
#define PIN_CLK  SPI3_IOMUX_PIN_NUM_CLK
#define PIN_CS   SPI3_IOMUX_PIN_NUM_CS
#define PIN_WP   SPI3_IOMUX_PIN_NUM_WP
#define PIN_HD   SPI3_IOMUX_PIN_NUM_HD
#else
#define HOST_ID  SPI2_HOST
#define PIN_MOSI SPI2_IOMUX_PIN_NUM_MOSI
#define PIN_MISO SPI2_IOMUX_PIN_NUM_MISO
#define PIN_CLK  SPI2_IOMUX_PIN_NUM_CLK
#define PIN_CS   SPI2_IOMUX_PIN_NUM_CS
#define PIN_WP   SPI2_IOMUX_PIN_NUM_WP
#define PIN_HD   SPI2_IOMUX_PIN_NUM_HD
#endif
#define SPI_DMA_CHAN         SPI_DMA_CH_AUTO
#define EXAMPLE_FLASH_FREQ_HZ (40 * 1000 * 1000)

static const char *base_path = "/nand";

/* Rewrites of the file per boot. Each rewrite adds one commit to LittleFS's metadata
 * block, so 40 fill it within one boot and trigger a compaction. */
#define WRITES_PER_BOOT 40

static esp_err_t init_spi_and_nand(spi_device_handle_t *out_spi, esp_blockdev_handle_t *out_nand_bdl)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
        .max_transfer_sz = 4096 * 2,
    };
    ESP_LOGI(TAG, "DMA CHANNEL: %d", SPI_DMA_CHAN);
    esp_err_t ret = spi_bus_initialize(HOST_ID, &bus_config, SPI_DMA_CHAN);
    if (ret != ESP_OK) {
        return ret;
    }

    /* SPI_DEVICE_HALFDUPLEX -> half duplex; 0 would mean full-duplex. */
    const uint32_t spi_flags = SPI_DEVICE_HALFDUPLEX;
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EXAMPLE_FLASH_FREQ_HZ,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 10,
        .flags = spi_flags,
    };
    spi_device_handle_t spi;
    ret = spi_bus_add_device(HOST_ID, &devcfg, &spi);
    if (ret != ESP_OK) {
        spi_bus_free(HOST_ID);
        return ret;
    }

    spi_nand_flash_config_t nand_flash_config = {
        .device_handle = spi,
        .io_mode = SPI_NAND_IO_MODE_SIO,
        .flags = spi_flags,
    };

    /* Raw flash BDL, NOT spi_nand_flash_wl_get_blockdev(). esp_nand_ubi is an
     * alternative FTL to the Dhara wear-leveling layer; stacking LittleFS on
     * Dhara AND UBI at once would be a double FTL with incompatible geometry
     * contracts -- pick one. This example demonstrates UBI. */
    esp_blockdev_handle_t nand_bdl = NULL;
    ret = nand_flash_get_blockdev(&nand_flash_config, &nand_bdl);
    if (ret != ESP_OK) {
        spi_bus_remove_device(spi);
        spi_bus_free(HOST_ID);
        return ret;
    }

    *out_spi = spi;
    *out_nand_bdl = nand_bdl;
    return ESP_OK;
}

#ifdef CONFIG_EXAMPLE_WIPE_NAND_ON_BOOT
static void wipe_nand(esp_blockdev_handle_t nand_bdl)
{
    uint32_t peb_size = (uint32_t)nand_bdl->geometry.erase_size;
    uint32_t peb_count = (uint32_t)(nand_bdl->geometry.disk_size / peb_size);
    uint32_t bad = 0;
    ESP_LOGW(TAG, "Wiping all %" PRIu32 " NAND blocks (CONFIG_EXAMPLE_WIPE_NAND_ON_BOOT)", peb_count);
    for (uint32_t pnum = 0; pnum < peb_count; pnum++) {
        esp_blockdev_cmd_arg_status_t bad_arg = { .num = pnum, .status = false };
        ESP_ERROR_CHECK(nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &bad_arg));
        if (bad_arg.status) {
            bad++;
            continue;
        }
        ESP_ERROR_CHECK(nand_bdl->ops->erase(nand_bdl, (uint64_t)pnum * peb_size, peb_size));
        if ((pnum & 0x3FU) == 0) {
            vTaskDelay(1);
        }
    }
    ESP_LOGW(TAG, "Wipe done, %" PRIu32 " bad blocks skipped", bad);
}
#endif

/* TEMPORARY: counts raw NAND operations per stage. */
static esp_blockdev_ops_t s_count_ops;
static const esp_blockdev_ops_t *s_real_ops;
static uint32_t s_reads, s_writes, s_erases;

static esp_err_t count_read(esp_blockdev_handle_t h, uint8_t *dst, size_t dst_size, uint64_t addr, size_t len)
{
    s_reads++;
    return s_real_ops->read(h, dst, dst_size, addr, len);
}

static esp_err_t count_write(esp_blockdev_handle_t h, const uint8_t *src, uint64_t addr, size_t len)
{
    s_writes++;
    ESP_LOGI(TAG, "  write PEB %" PRIu32 " page %" PRIu32 " (%u B)", (uint32_t)(addr / h->geometry.erase_size),
             (uint32_t)(addr % h->geometry.erase_size / h->geometry.write_size), (unsigned)len);
    return s_real_ops->write(h, src, addr, len);
}

static esp_err_t count_erase(esp_blockdev_handle_t h, uint64_t addr, size_t len)
{
    s_erases++;
    ESP_LOGI(TAG, "  erase PEB %" PRIu32, (uint32_t)(addr / h->geometry.erase_size));
    return s_real_ops->erase(h, addr, len);
}

static void count_install(esp_blockdev_handle_t nand_bdl)
{
    s_real_ops = nand_bdl->ops;
    s_count_ops = *nand_bdl->ops;
    s_count_ops.read = count_read;
    s_count_ops.write = count_write;
    s_count_ops.erase = count_erase;
    nand_bdl->ops = &s_count_ops;
}

static void count_report(const char *stage)
{
    ESP_LOGI(TAG, "[NAND OPS %-22s] reads %" PRIu32 " writes %" PRIu32 " erases %" PRIu32,
             stage, s_reads, s_writes, s_erases);
    s_reads = s_writes = s_erases = 0;
}

static void deinit_spi_bus(spi_device_handle_t spi)
{
    ESP_ERROR_CHECK(spi_bus_remove_device(spi));
    ESP_ERROR_CHECK(spi_bus_free(HOST_ID));
}

/* Reads up to len-1 bytes of path into buf as a NUL-terminated string.
 *
 * POSIX open()/read()/write() instead of fopen(): the libc gives every FILE a buffer of
 * st_blksize bytes, and esp_littlefs reports st_blksize as the LittleFS block size. On
 * UBI that block is a whole LEB (~124 KiB), so each fopen() would allocate ~124 KiB. */
static bool read_text_file(const char *path, char *buf, size_t len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0) {
        return false;
    }
    buf[n] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return true;
}

static bool write_text_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    size_t len = strlen(text);
    bool ok = write(fd, text, len) == (ssize_t)len && write(fd, "\n", 1) == 1;
    return close(fd) == 0 && ok;
}

void app_main(void)
{
    nand_ubi_log_ram(TAG, "before init");

    spi_device_handle_t spi;
    esp_blockdev_handle_t nand_bdl = NULL;
    nand_ubi_ram_stage_t stage = nand_ubi_ram_stage_begin("raw NAND init");
    ESP_ERROR_CHECK(init_spi_and_nand(&spi, &nand_bdl));
    nand_ubi_ram_stage_end(TAG, &stage);
    nand_ubi_log_ram(TAG, "after raw NAND BDL init");

    ESP_LOGI(TAG, "Raw NAND: page_size=%" PRIu32 " peb_size=%" PRIu32 " disk_size=%" PRIu64,
             (uint32_t)nand_bdl->geometry.read_size, (uint32_t)nand_bdl->geometry.erase_size,
             nand_bdl->geometry.disk_size);

#ifdef CONFIG_EXAMPLE_WIPE_NAND_ON_BOOT
    wipe_nand(nand_bdl);
#endif

    count_install(nand_bdl);
    ESP_LOGI(TAG, "Attaching NAND UBI volume 0 (raw BDL, not Dhara WL)");
    nand_ubi_config_t ubi_cfg = NAND_UBI_CONFIG_DEFAULT();
    esp_blockdev_handle_t vol_bdl = NULL;
    stage = nand_ubi_ram_stage_begin("UBI attach");
    ESP_ERROR_CHECK(nand_ubi_get_blockdev(nand_bdl, &ubi_cfg, &vol_bdl));
    nand_ubi_ram_stage_end(TAG, &stage);
    count_report("UBI attach");
    /* "min" here captures nand_ubi_attach()'s temporary scan buffers,
     * already freed by the time this line runs -- "free now"
     * alone would hide their peak cost. */
    nand_ubi_log_ram(TAG, "after UBI attach (scan done)");

    ESP_LOGI(TAG, "UBI volume ready: leb_size=%" PRIu32 " leb_count=%" PRIu64 " volume_size=%" PRIu64,
             (uint32_t)vol_bdl->geometry.erase_size,
             vol_bdl->geometry.disk_size / vol_bdl->geometry.erase_size, vol_bdl->geometry.disk_size);

    ESP_LOGI(TAG, "Mounting LittleFS on the UBI volume BDL");
    esp_vfs_littlefs_conf_t conf = {
        .base_path = base_path,
        .partition_label = NULL,
        .partition = NULL,
        .blockdev = vol_bdl,
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };

    /* CONFIG_LITTLEFS_WDT_RESET resets the TWDT from this task, which only works once it
     * is subscribed. Not before attach: attach does not reset the TWDT and can outlast it. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    stage = nand_ubi_ram_stage_begin("LittleFS mount");
    esp_err_t mount_ret = esp_vfs_littlefs_register(&conf);
    nand_ubi_ram_stage_end(TAG, &stage);
    count_report("LittleFS mount");
    if (mount_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS on UBI volume (%s)", esp_err_to_name(mount_ret));
        /* vol_bdl->ops->release() also detaches the UBI device
         * (nand_ubi_get_blockdev() sets owns_device = true). */
        vol_bdl->ops->release(vol_bdl);
        nand_bdl->ops->release(nand_bdl);
        deinit_spi_bus(spi);
        ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
        return;
    }
    /* This delta on top of the previous checkpoint is LittleFS's own
     * mount-time cost (cache_size + lookahead_size buffers) stacked on top
     * of whatever UBI already holds resident -- the number that matters for
     * "can this stack fit on a no-PSRAM target." */
    nand_ubi_log_ram(TAG, "after LittleFS mount");

    size_t total = 0, used = 0;
    ESP_ERROR_CHECK(esp_littlefs_blockdev_info(vol_bdl, &total, &used));
    ESP_LOGI(TAG, "LittleFS: %u kB total, %u kB used", (unsigned)(total / 1024), (unsigned)(used / 1024));

    /* A boot counter in the file shows that data written on the previous boot
     * survived detach, power-off and re-attach, not just the filesystem. */
    unsigned boot = 0;
    char line[128];
    stage = nand_ubi_ram_stage_begin("first read");
    bool have_prev = read_text_file("/nand/hello.txt", line, sizeof(line));
    nand_ubi_ram_stage_end(TAG, &stage);
    count_report("first read");
    if (have_prev) {
        if (sscanf(line, "boot=%u", &boot) == 1) {
            ESP_LOGI(TAG, "Previous boot left: '%s'", line);
        }
    } else {
        ESP_LOGI(TAG, "No file from a previous boot");
    }
    boot++;

    ESP_LOGI(TAG, "Writing boot=%u, %d times", boot, WRITES_PER_BOOT);
    char expected[128];
    snprintf(expected, sizeof(expected), "boot=%u written using ESP-IDF %s over esp_nand_ubi",
             boot, esp_get_idf_version());
    stage = nand_ubi_ram_stage_begin("first write");
    bool write_ok = true;
    for (int i = 0; i < WRITES_PER_BOOT && write_ok; i++) {
        write_ok = write_text_file("/nand/hello.txt", expected);
    }
    nand_ubi_ram_stage_end(TAG, &stage);
    count_report("file write");
    if (!write_ok) {
        ESP_LOGE(TAG, "Failed to write the file");
        esp_vfs_littlefs_unregister_blockdev(vol_bdl);
        nand_bdl->ops->release(nand_bdl);
        deinit_spi_bus(spi);
        ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
        return;
    }

    /* The file was closed after writing, so this read comes back through UBI's
     * LEB->PEB mapping, not from an in-RAM buffer. */
    ESP_LOGI(TAG, "Reading file back");
    if (!read_text_file("/nand/hello.txt", line, sizeof(line))) {
        ESP_LOGE(TAG, "Failed to read the file back");
        esp_vfs_littlefs_unregister_blockdev(vol_bdl);
        nand_bdl->ops->release(nand_bdl);
        deinit_spi_bus(spi);
        esp_task_wdt_delete(NULL);
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    ESP_LOGI(TAG, "Read from file: '%s'", line);
    if (strcmp(line, expected) != 0) {
        ESP_LOGE(TAG, "Read-back mismatch");
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    ESP_ERROR_CHECK(esp_littlefs_blockdev_info(vol_bdl, &total, &used));
    ESP_LOGI(TAG, "LittleFS: %u kB total, %u kB used", (unsigned)(total / 1024), (unsigned)(used / 1024));
    nand_ubi_log_ram(TAG, "after write/read round-trip");

    /* Unregister unmounts LittleFS and releases vol_bdl via ops->release,
     * which also detaches the UBI device. nand_bdl is released separately:
     * esp_nand_ubi never owns the raw flash BDL it was attached to. */
    stage = nand_ubi_ram_stage_begin("unmount + UBI detach");
    count_report("read back + info");
    ESP_ERROR_CHECK(esp_vfs_littlefs_unregister_blockdev(vol_bdl));
    nand_ubi_ram_stage_end(TAG, &stage);
    count_report("unmount + UBI detach");
    ESP_ERROR_CHECK(nand_bdl->ops->release(nand_bdl));
    deinit_spi_bus(spi);
    /* Should be back near the "before init" baseline; a persistent gap here
     * would indicate a leak in the unmount/release/detach path. */
    nand_ubi_log_ram(TAG, "after unmount+release (should ~= baseline)");

    ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
    ESP_LOGI(TAG, "LittleFS-on-UBI example finished successfully");
}
