/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/**
 * NAND UBI metadata dump.
 *
 * Read-only debug tool: walks every physical erase block on the raw SPI NAND
 * BDL, decodes EC/VID headers via esp_nand_ubi_media.h, and prints a progressive
 * one-line-per-interesting-PEB table plus an end-of-scan summary.
 *
 * Does NOT call nand_ubi_attach(), and does not erase or write flash.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "soc/spi_pins.h"

#include "spi_nand_flash.h"
#include "esp_nand_blockdev.h"
#include "esp_nand_ubi_media.h"

static const char *TAG = "ubi_dump";

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

typedef enum {
    PEB_EMPTY = 0,
    PEB_BAD,
    PEB_IO_ERR,
    PEB_CORRUPT,
    PEB_FREE,
    PEB_MAPPED,
} peb_status_t;

typedef struct {
    uint32_t empty;
    uint32_t free_peb;
    uint32_t mapped;
    uint32_t corrupt;
    uint32_t bad;
    uint32_t io_err;
    bool     have_image_seq;
    uint32_t image_seq;
    bool     have_ec;
    uint64_t ec_min;
    uint64_t ec_max;
    uint64_t max_sqnum;
    int32_t  max_lnum;
} dump_stats_t;

/* Fixed chip geometry, bundled to keep classify_and_print_peb()'s parameter count down. */
typedef struct {
    uint32_t page_size;
    uint32_t peb_size;
    uint32_t peb_count;
} dump_geometry_t;

/* Parsed EC/VID header fields for one table row, bundled to keep print_peb_line()'s
 * parameter count down. "have_ec"/"have_vid" gate which fields are meaningful: a PEB
 * can have a valid EC header but no VID header yet (still FREE). */
typedef struct {
    bool     have_ec;
    uint64_t ec;
    uint32_t image_seq;
    bool     have_vid;
    uint32_t vol_id;
    uint32_t lnum;
    uint64_t sqnum;
    uint8_t  copy_flag;
} peb_row_t;

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

static bool page_is_blank(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

static const char *status_str(peb_status_t s)
{
    switch (s) {
    case PEB_EMPTY:
        return "EMPTY";
    case PEB_BAD:
        return "BAD";
    case PEB_IO_ERR:
        return "IO_ERR";
    case PEB_CORRUPT:
        return "CORRUPT";
    case PEB_FREE:
        return "FREE";
    case PEB_MAPPED:
        return "MAPPED";
    default:
        return "?";
    }
}

static void note_ec(dump_stats_t *stats, uint64_t ec)
{
    if (!stats->have_ec) {
        stats->ec_min = ec;
        stats->ec_max = ec;
        stats->have_ec = true;
    } else {
        if (ec < stats->ec_min) {
            stats->ec_min = ec;
        }
        if (ec > stats->ec_max) {
            stats->ec_max = ec;
        }
    }
}

static void print_table_header(void)
{
    printf("PEB   STATUS   EC       image_seq   vol  lnum  sqnum      copy  notes\n");
    printf("----  -------  -------  ----------  ---  ----  ---------  ----  -----\n");
}

static void print_peb_line(uint32_t pnum, peb_status_t status, const peb_row_t *row,
                           const char *notes, bool *header_printed)
{
    if (!*header_printed) {
        print_table_header();
        *header_printed = true;
    }

    /* Sized for the full-width decimal (or "0x"+hex) representation of each field's
     * actual type, not just what fits today's Phase-1 single-volume, small-chip
     * values: ec/sqnum are uint64_t (up to 20 digits), vol_id/lnum are uint32_t
     * (up to 10 digits) -- a size that "happens to work" for small test chips would
     * silently truncate on a chip with millions of LEBs or a long-lived erase count. */
    char ec_buf[24];
    char seq_buf[16];
    char vol_buf[16];
    char lnum_buf[16];
    char sq_buf[24];
    char copy_buf[8];

    if (row->have_ec) {
        snprintf(ec_buf, sizeof(ec_buf), "%" PRIu64, row->ec);
        snprintf(seq_buf, sizeof(seq_buf), "0x%08" PRIx32, row->image_seq);
    } else {
        snprintf(ec_buf, sizeof(ec_buf), "-");
        snprintf(seq_buf, sizeof(seq_buf), "-");
    }

    if (row->have_vid) {
        snprintf(vol_buf, sizeof(vol_buf), "%" PRIu32, row->vol_id);
        snprintf(lnum_buf, sizeof(lnum_buf), "%" PRIu32, row->lnum);
        snprintf(sq_buf, sizeof(sq_buf), "%" PRIu64, row->sqnum);
        snprintf(copy_buf, sizeof(copy_buf), "%" PRIu8, row->copy_flag);
    } else {
        snprintf(vol_buf, sizeof(vol_buf), "-");
        snprintf(lnum_buf, sizeof(lnum_buf), "-");
        snprintf(sq_buf, sizeof(sq_buf), "-");
        snprintf(copy_buf, sizeof(copy_buf), "-");
    }

    printf("%-5" PRIu32 " %-7s  %-7s  %-10s  %-3s  %-4s  %-9s  %-4s  %s\n",
           pnum, status_str(status), ec_buf, seq_buf, vol_buf, lnum_buf, sq_buf, copy_buf,
           notes != NULL ? notes : "");
}

/* lizard flags this function for cyclomatic complexity (20) and length (121 lines):
 * it is intentionally one linear "read this PEB's headers, classify into exactly one
 * of 6 mutually-exclusive states, print that row" state machine, mirroring the
 * equivalent scan step in nand_ubi_attach() (src/nand_ubi.c) that this read-only
 * debug tool must stay consistent with. Splitting the EC/VID stages into separate
 * helper functions would require threading page_buf, ec, image_seq, vid_hdr_offset
 * and data_offset across an extra call boundary purely to satisfy the linter, without
 * reducing the actual number of distinct outcomes a reader needs to hold in mind.
 * Accepted as-is; revisit if this logic grows additional states. */
static void classify_and_print_peb(esp_blockdev_handle_t nand_bdl, uint32_t pnum,
                                   const dump_geometry_t *geom, uint8_t *page_buf,
                                   dump_stats_t *stats, bool *header_printed)
{
    peb_row_t row = { 0 };
    uint32_t page_size = geom->page_size;
    uint32_t peb_size = geom->peb_size;
    uint32_t peb_count = geom->peb_count;

    esp_blockdev_cmd_arg_status_t bad_arg = { .num = pnum, .status = false };
    esp_err_t ret = nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &bad_arg);
    if (ret != ESP_OK) {
        stats->io_err++;
        char notes[48];
        snprintf(notes, sizeof(notes), "IS_BAD ioctl 0x%x", ret);
        print_peb_line(pnum, PEB_IO_ERR, &row, notes, header_printed);
        return;
    }
    if (bad_arg.status) {
        stats->bad++;
        print_peb_line(pnum, PEB_BAD, &row, "factory bad", header_printed);
        return;
    }

    ret = nand_bdl->ops->read(nand_bdl, page_buf, page_size, (uint64_t)pnum * peb_size, page_size);
    if (ret != ESP_OK) {
        stats->io_err++;
        char notes[48];
        snprintf(notes, sizeof(notes), "EC read 0x%x", ret);
        print_peb_line(pnum, PEB_IO_ERR, &row, notes, header_printed);
        return;
    }

    if (page_is_blank(page_buf, page_size)) {
        stats->empty++;
        return;
    }

    const nand_ubi_ec_hdr_t *ec_hdr = (const nand_ubi_ec_hdr_t *)page_buf;
    uint32_t magic = nand_ubi_be32(ec_hdr->magic);
    if (magic != UBI_EC_HDR_MAGIC) {
        /* Non-blank page without UBI# — not UBI metadata; treat as EMPTY for the table. */
        stats->empty++;
        return;
    }

    if (!nand_ubi_ec_hdr_valid(ec_hdr)) {
        stats->corrupt++;
        print_peb_line(pnum, PEB_CORRUPT, &row, "bad EC CRC", header_printed);
        return;
    }

    row.have_ec = true;
    row.ec = nand_ubi_be64(ec_hdr->ec);
    row.image_seq = nand_ubi_be32(ec_hdr->image_seq);
    uint32_t vid_hdr_offset = nand_ubi_be32(ec_hdr->vid_hdr_offset);
    uint32_t data_offset = nand_ubi_be32(ec_hdr->data_offset);

    note_ec(stats, row.ec);

    if (!stats->have_image_seq) {
        stats->image_seq = row.image_seq;
        stats->have_image_seq = true;
    } else if (row.image_seq != stats->image_seq) {
        stats->corrupt++;
        char notes[64];
        snprintf(notes, sizeof(notes), "stale image_seq (want 0x%08" PRIx32 ")", stats->image_seq);
        print_peb_line(pnum, PEB_CORRUPT, &row, notes, header_printed);
        return;
    }

    if (vid_hdr_offset == 0 || vid_hdr_offset % page_size != 0 ||
            data_offset % page_size != 0 || data_offset < vid_hdr_offset + page_size ||
            data_offset >= peb_size) {
        stats->corrupt++;
        char notes[64];
        snprintf(notes, sizeof(notes), "bad offsets vid=%" PRIu32 " data=%" PRIu32,
                 vid_hdr_offset, data_offset);
        print_peb_line(pnum, PEB_CORRUPT, &row, notes, header_printed);
        return;
    }

    ret = nand_bdl->ops->read(nand_bdl, page_buf, page_size,
                              (uint64_t)pnum * peb_size + vid_hdr_offset, page_size);
    if (ret != ESP_OK) {
        stats->io_err++;
        char notes[48];
        snprintf(notes, sizeof(notes), "VID read 0x%x", ret);
        print_peb_line(pnum, PEB_IO_ERR, &row, notes, header_printed);
        return;
    }

    if (page_is_blank(page_buf, page_size)) {
        stats->free_peb++;
        print_peb_line(pnum, PEB_FREE, &row, "EC only", header_printed);
        return;
    }

    const nand_ubi_vid_hdr_t *vid_hdr = (const nand_ubi_vid_hdr_t *)page_buf;
    if (!nand_ubi_vid_hdr_valid(vid_hdr)) {
        stats->corrupt++;
        print_peb_line(pnum, PEB_CORRUPT, &row, "bad VID CRC", header_printed);
        return;
    }

    row.have_vid = true;
    row.vol_id = nand_ubi_be32(vid_hdr->vol_id);
    row.lnum = nand_ubi_be32(vid_hdr->lnum);
    row.sqnum = nand_ubi_be64(vid_hdr->sqnum);
    row.copy_flag = vid_hdr->copy_flag;

    if (row.lnum >= peb_count) {
        stats->corrupt++;
        print_peb_line(pnum, PEB_CORRUPT, &row, "lnum out of range", header_printed);
        return;
    }

    stats->mapped++;
    if ((int32_t)row.lnum > stats->max_lnum) {
        stats->max_lnum = (int32_t)row.lnum;
    }
    if (row.sqnum > stats->max_sqnum) {
        stats->max_sqnum = row.sqnum;
    }

    print_peb_line(pnum, PEB_MAPPED, &row, "", header_printed);
}

static void print_summary(const dump_stats_t *stats, uint32_t page_size, uint32_t peb_size, uint32_t peb_count)
{
    ESP_LOGI(TAG, "=== UBI metadata dump summary ===");
    ESP_LOGI(TAG, "geometry: page_size=%" PRIu32 " peb_size=%" PRIu32 " peb_count=%" PRIu32,
             page_size, peb_size, peb_count);
    ESP_LOGI(TAG, "counts: EMPTY=%" PRIu32 " FREE=%" PRIu32 " MAPPED=%" PRIu32
             " CORRUPT=%" PRIu32 " BAD=%" PRIu32 " IO_ERR=%" PRIu32,
             stats->empty, stats->free_peb, stats->mapped, stats->corrupt, stats->bad, stats->io_err);

    if (stats->mapped > 0 || stats->free_peb > 0) {
        if (stats->have_image_seq) {
            ESP_LOGI(TAG, "image_seq=0x%08" PRIx32, stats->image_seq);
        }
        if (stats->have_ec) {
            ESP_LOGI(TAG, "EC min=%" PRIu64 " max=%" PRIu64, stats->ec_min, stats->ec_max);
        }
        if (stats->mapped > 0) {
            ESP_LOGI(TAG, "max sqnum=%" PRIu64 " max lnum=%" PRId32,
                     stats->max_sqnum, stats->max_lnum);
        }
    }
}

void app_main(void)
{
    spi_device_handle_t spi;
    esp_blockdev_handle_t nand_bdl = NULL;
    ESP_ERROR_CHECK(init_spi_and_nand(&spi, &nand_bdl));

    uint32_t page_size = (uint32_t)nand_bdl->geometry.read_size;
    uint32_t peb_size = (uint32_t)nand_bdl->geometry.erase_size;
    uint32_t peb_count = (uint32_t)(nand_bdl->geometry.disk_size / peb_size);

    ESP_LOGI(TAG, "Raw NAND: page_size=%" PRIu32 " peb_size=%" PRIu32 " peb_count=%" PRIu32
             " disk_size=%" PRIu64,
             page_size, peb_size, peb_count, nand_bdl->geometry.disk_size);
    ESP_LOGI(TAG, "Scanning %" PRIu32 " PEBs for UBI EC/VID metadata (read-only)", peb_count);

    uint8_t *page_buf = malloc(page_size);
    if (page_buf == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    dump_geometry_t geom = { .page_size = page_size, .peb_size = peb_size, .peb_count = peb_count };
    dump_stats_t stats = {
        .max_lnum = -1,
    };
    bool header_printed = false;

    for (uint32_t pnum = 0; pnum < peb_count; pnum++) {
        classify_and_print_peb(nand_bdl, pnum, &geom, page_buf, &stats, &header_printed);
    }

    free(page_buf);

    if (!header_printed) {
        ESP_LOGI(TAG, "(no interesting PEBs — table omitted)");
    }

    print_summary(&stats, page_size, peb_size, peb_count);

    ESP_ERROR_CHECK(nand_bdl->ops->release(nand_bdl));
    spi_bus_remove_device(spi);
    spi_bus_free(HOST_ID);

    ESP_LOGI(TAG, "NAND UBI metadata dump finished successfully");
}
