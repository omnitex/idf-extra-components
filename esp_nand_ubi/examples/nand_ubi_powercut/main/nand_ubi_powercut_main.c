/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* NAND UBI power-cut test, board side. Each boot attaches UBI, checks UBI's own state
 * and the ptest workload volume, records the result in the stats volume, then runs the
 * workload until it is interrupted (auto reset or unplugging the board). See
 * docs/plans/2026-10-08-esp-nand-ubi-powercut-stress-design.md and this example's README. */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_nand_blockdev.h"
#include "esp_nand_ubi.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nand_ubi_priv.h"
#include "ptest_stats.h"
#include "ptest_ubi.h"
#include "sdkconfig.h"
#include "soc/spi_pins.h"
#include "spi_nand_flash.h"

static const char *TAG = "ptest";

#ifdef CONFIG_IDF_TARGET_ESP32
#define HOST_ID  SPI3_HOST
#define PIN_MOSI SPI3_IOMUX_PIN_NUM_MOSI
#define PIN_MISO SPI3_IOMUX_PIN_NUM_MISO
#define PIN_CLK  SPI3_IOMUX_PIN_NUM_CLK
#define PIN_CS   SPI3_IOMUX_PIN_NUM_CS
#else
#define HOST_ID  SPI2_HOST
#define PIN_MOSI SPI2_IOMUX_PIN_NUM_MOSI
#define PIN_MISO SPI2_IOMUX_PIN_NUM_MISO
#define PIN_CLK  SPI2_IOMUX_PIN_NUM_CLK
#define PIN_CS   SPI2_IOMUX_PIN_NUM_CS
#endif
#define FLASH_FREQ_HZ (40 * 1000 * 1000)

#define STATS_VOL_NAME "ptest_stats"
#define DATA_VOL_NAME  "ptest_data"
#define DATA_VOL_LEBS  (CONFIG_PTEST_DATA_LEBS + PTEST_UBI_JOURNAL_LEBS)

static esp_err_t init_nand(esp_blockdev_handle_t *out)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
        .max_transfer_sz = 4096 * 2,
    };
    esp_err_t ret = spi_bus_initialize(HOST_ID, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        return ret;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = FLASH_FREQ_HZ,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 10,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    spi_device_handle_t spi;
    ret = spi_bus_add_device(HOST_ID, &dev, &spi);
    if (ret != ESP_OK) {
        return ret;
    }
    spi_nand_flash_config_t cfg = {
        .device_handle = spi,
        .io_mode = SPI_NAND_IO_MODE_SIO,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    return nand_flash_get_blockdev(&cfg, out);
}

/* Stops here and keeps the failure on screen; in auto mode no further reset happens. */
static void halt(const char *why)
{
    ESP_LOGE(TAG, "HALT: %s", why);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGE(TAG, "HALT: %s", why);
    }
}

static uint32_t ms_since(int64_t t0_us)
{
    return (uint32_t)((esp_timer_get_time() - t0_us) / 1000);
}

/* Erases every good PEB, so the next attach sees a blank chip. */
static void wipe_chip(esp_blockdev_handle_t nand)
{
    uint32_t peb_size = (uint32_t)nand->geometry.erase_size;
    uint32_t peb_count = (uint32_t)(nand->geometry.disk_size / peb_size);
    ESP_LOGW(TAG, "erasing all %" PRIu32 " PEBs for a new campaign", peb_count);
    for (uint32_t pnum = 0; pnum < peb_count; pnum++) {
        if ((pnum & 0x3F) == 0) {
            vTaskDelay(1);
        }
        esp_blockdev_cmd_arg_status_t bad = { .num = pnum, .status = false };
        if (nand->ops->ioctl(nand, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &bad) != ESP_OK || bad.status) {
            continue;
        }
        (void)nand->ops->erase(nand, (uint64_t)pnum * peb_size, peb_size);
    }
}

static bool layout_ok(const nand_ubi_device_t *dev)
{
    return dev->vol_count == 2 && dev->volumes[0].leb_count == CONFIG_PTEST_STATS_LEBS &&
           dev->volumes[1].leb_count == DATA_VOL_LEBS;
}

/* After a read-write attach nothing is left to erase, and every used PEB is a
 * volume-table copy, the attach checkpoint, or mapped to exactly one LEB. */
static bool peb_accounting_ok(const nand_ubi_device_t *dev, uint32_t *free_pebs, uint32_t *bad_pebs)
{
    uint32_t used = 0, pending = 0;
    *free_pebs = 0;
    *bad_pebs = 0;
    for (uint32_t pnum = 0; pnum < dev->peb_count; pnum++) {
        switch (nand_ubi_eba_peb_get_state(&dev->eba, pnum)) {
        case UBI_PEB_FREE:
            (*free_pebs)++;
            break;
        case UBI_PEB_USED:
            used++;
            break;
        case UBI_PEB_BAD:
            (*bad_pebs)++;
            break;
        default:
            pending++;
            break;
        }
    }
    uint32_t mapped = 0;
    for (uint32_t i = 0; i < dev->leb_count; i++) {
        mapped += nand_ubi_eba_get_pnum(&dev->eba, i) != UBI_LEB_UNMAPPED;
    }
    uint32_t meta = UBI_LAYOUT_VOLUME_EBS + (dev->ckpt_pnum >= 0 ? 1u : 0u);
    if (pending != 0 || used != mapped + meta) {
        ESP_LOGE(TAG, "PEB accounting: %" PRIu32 " erase-pending, %" PRIu32 " used, %" PRIu32 " mapped LEBs",
                 pending, used, mapped);
        return false;
    }
    return true;
}

#if CONFIG_PTEST_RESET_AUTO
static void auto_reset_cb(void *arg)
{
    (void)arg;
    esp_restart();
}
#endif

void app_main(void)
{
    uint8_t campaign[8];
    memcpy(campaign, esp_app_get_description()->app_elf_sha256, sizeof(campaign));
    uint32_t reset_reason = (uint32_t)esp_reset_reason();
    ESP_LOGI(TAG, "campaign %02x%02x%02x%02x, reset %s", campaign[0], campaign[1], campaign[2], campaign[3],
             ptest_reset_reason_name(reset_reason));

    esp_blockdev_handle_t nand = NULL;
    if (init_nand(&nand) != ESP_OK) {
        halt("NAND init failed");
    }

    int64_t t0 = esp_timer_get_time();
    nand_ubi_device_t *dev = NULL;
    esp_err_t err = nand_ubi_attach(nand, NULL, &dev);
    uint32_t attach_ms = ms_since(t0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "attach failed: %s", esp_err_to_name(err));
        halt("attach failed");
    }

    /* Continue the campaign only if this build created the volumes and wrote at least
     * one record; otherwise start over on a blank chip. */
    esp_blockdev_handle_t stats_vol = NULL;
    ptest_stats_t stats;
    bool new_campaign = !layout_ok(dev);
    if (!new_campaign) {
        if (nand_ubi_open_volume(dev, 0, &stats_vol) != ESP_OK || ptest_stats_load(&stats, stats_vol) != ESP_OK) {
            halt("cannot read the stats volume");
        }
        if (!stats.have_last) {
            ESP_LOGW(TAG, "no valid stats record found, starting a new campaign");
        }
        new_campaign = !stats.have_last || memcmp(stats.last.campaign, campaign, sizeof(campaign)) != 0;
        if (new_campaign) {
            stats_vol->ops->release(stats_vol);
            stats_vol = NULL;
        }
    }
    if (new_campaign) {
        nand_ubi_detach(dev);
        wipe_chip(nand);
        t0 = esp_timer_get_time();
        if (nand_ubi_attach(nand, NULL, &dev) != ESP_OK) {
            halt("attach of the wiped chip failed");
        }
        attach_ms = ms_since(t0);
        uint32_t id = 0;
        if (nand_ubi_create_volume(dev, STATS_VOL_NAME, UBI_VID_DYNAMIC, CONFIG_PTEST_STATS_LEBS, &id) != ESP_OK ||
                nand_ubi_create_volume(dev, DATA_VOL_NAME, UBI_VID_DYNAMIC, DATA_VOL_LEBS, &id) != ESP_OK ||
                nand_ubi_open_volume(dev, 0, &stats_vol) != ESP_OK || ptest_stats_load(&stats, stats_vol) != ESP_OK) {
            halt("cannot create the test volumes");
        }
    }

    ptest_stats_rec_t rec = { 0 };
    memcpy(rec.campaign, campaign, sizeof(campaign));
    rec.boot_no = stats.have_last ? stats.last.boot_no + 1 : 1;
    rec.reset_reason = reset_reason;
    rec.attach_ms = attach_ms;
    rec.stats_torn = stats.torn;
    rec.mean_ec = (uint32_t)dev->mean_ec;
    rec.flags = new_campaign ? PTEST_FLAG_NEW_CAMPAIGN : 0;
    if (dev->vtbl_copy_good[0] && dev->vtbl_copy_good[1]) {
        rec.flags |= PTEST_FLAG_VTBL_OK;
    }
    if (peb_accounting_ok(dev, &rec.free_pebs, &rec.bad_pebs)) {
        rec.flags |= PTEST_FLAG_ACCOUNTING_OK;
    }

    esp_blockdev_handle_t data_vol = NULL;
    ptest_ubi_t *work = NULL;
    ptest_ubi_cfg_t cfg = { CONFIG_PTEST_DATA_LEBS, CONFIG_PTEST_PAGES_PER_STEP, esp_random() | 1u };
    if (nand_ubi_open_volume(dev, 1, &data_vol) != ESP_OK || ptest_ubi_create(data_vol, &cfg, &work) != ESP_OK) {
        halt("cannot open the data volume");
    }
    ptest_ubi_result_t res;
    t0 = esp_timer_get_time();
    if (ptest_ubi_verify(work, &res) != ESP_OK) {
        halt("verify: I/O error");
    }
    rec.verify_ms = ms_since(t0);
    if (res.ok) {
        rec.flags |= PTEST_FLAG_VERIFY_OK;
    }
    rec.lebs_matching = res.lebs_matching;
    rec.lost = res.lost;
    rec.corrupt = res.corrupt;
    rec.in_flight = res.in_flight;
    rec.journal_torn = res.journal_torn;

    const uint32_t ok_flags = PTEST_FLAG_VERIFY_OK | PTEST_FLAG_ACCOUNTING_OK | PTEST_FLAG_VTBL_OK;
    bool boot_ok = (rec.flags & ok_flags) == ok_flags;
    const ptest_stats_rec_t *prev = stats.have_last ? &stats.last : NULL;
    rec.total_failures = (prev ? prev->total_failures : 0) + (boot_ok ? 0 : 1);
    rec.total_lost = (prev ? prev->total_lost : 0) + rec.lost;
    rec.total_corrupt = (prev ? prev->total_corrupt : 0) + rec.corrupt;
    rec.total_in_flight = (prev ? prev->total_in_flight : 0) + rec.in_flight;

#if CONFIG_PTEST_RESET_AUTO
    rec.planned_reset_ms = CONFIG_PTEST_RESET_MIN_MS +
                           esp_random() % (CONFIG_PTEST_RESET_MAX_MS - CONFIG_PTEST_RESET_MIN_MS + 1);
#endif
    if (ptest_stats_append(&stats, &rec) != ESP_OK) {
        halt("cannot write the stats record");
    }

    ptest_stats_print_recent(&stats, 10);
    ESP_LOGI(TAG, "boot %" PRIu32 ": attach %" PRIu32 " ms, verify %" PRIu32 " ms, %s%s%s",
             rec.boot_no, rec.attach_ms, rec.verify_ms, boot_ok ? "OK" : "FAIL",
             res.first_error[0] ? ": " : "", res.first_error);
    ESP_LOGI(TAG, "campaign totals over %" PRIu32 " boots: %" PRIu32 " failures, %" PRIu32 " lost, %" PRIu32
             " corrupt, %" PRIu32 " in-flight rewrites", rec.boot_no, rec.total_failures, rec.total_lost,
             rec.total_corrupt, rec.total_in_flight);
    if (!boot_ok) {
        halt("checks failed, see above");
    }

#if CONFIG_PTEST_RESET_AUTO
    const esp_timer_create_args_t targs = { .callback = auto_reset_cb, .name = "ptest_reset" };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &timer));
    ESP_ERROR_CHECK(esp_timer_start_once(timer, (uint64_t)rec.planned_reset_ms * 1000));
    ESP_LOGI(TAG, "running the workload, reset in %" PRIu32 " ms", rec.planned_reset_ms);
#else
    ESP_LOGI(TAG, "running the workload until power is removed");
#endif

    for (uint32_t steps = 1;; steps++) {
        err = ptest_ubi_step(work);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "step %" PRIu32 ": %s", steps, esp_err_to_name(err));
            halt("workload I/O error");
        }
        if (steps % 50 == 0) {
            ESP_LOGI(TAG, "%" PRIu32 " steps", steps);
        }
    }
}
