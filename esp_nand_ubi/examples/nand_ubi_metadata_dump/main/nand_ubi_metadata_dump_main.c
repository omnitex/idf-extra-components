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
 * one-line-per-interesting-PEB table plus an end-of-scan summary. After the scan it
 * decodes the volume table and the newest attach checkpoint and checks them against
 * what the scan found.
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
    PEB_VTBL,   /* volume-table copy (vol_id UBI_LAYOUT_VOL_ID) */
    PEB_CKPT,   /* attach checkpoint (vol_id UBI_CKPT_VOL_ID) */
} peb_status_t;

/* An internal-volume PEB found by the scan, with the VID fields needed to decode it. */
typedef struct {
    int32_t  pnum;
    uint64_t sqnum;
    uint32_t data_size;
    uint32_t data_crc;
} meta_peb_t;

typedef struct {
    uint32_t empty;
    uint32_t non_ubi;
    uint32_t free_peb;
    uint32_t mapped;
    uint32_t vtbl;
    uint32_t ckpt;
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
    meta_peb_t vtbl_copy[UBI_LAYOUT_VOLUME_EBS]; /* newest PEB of each volume-table copy */
    meta_peb_t newest_ckpt;
    uint8_t  *status;                            /* [peb_count] peb_status_t, for the cross-checks */
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
    uint8_t  compat;
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
    case PEB_VTBL:
        return "VTBL";
    case PEB_CKPT:
        return "CKPT";
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
    printf("PEB   STATUS   EC       image_seq   vol                lnum  sqnum      copy  notes\n");
    printf("----  -------  -------  ----------  -----------------  ----  ---------  ----  -----\n");
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
     * silently truncate on a chip with millions of LEBs or a long-lived erase count.
     * vol_buf is sized for "UBI_LAYOUT_VOL_ID" (17 chars), the longest symbolic name
     * printed for an internal volume (see below). */
    char ec_buf[24];
    char seq_buf[16];
    char vol_buf[24];
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
        /* Internal volumes use reserved IDs near 0x7FFFF000: the volume-table copies
         * (UBI_LAYOUT_VOL_ID, Linux UBI's UBI_LAYOUT_VOLUME_ID, on any good PEB) and the
         * attach checkpoints (UBI_CKPT_VOL_ID). They are printed by name. */
        if (row->vol_id == UBI_LAYOUT_VOL_ID) {
            snprintf(vol_buf, sizeof(vol_buf), "UBI_LAYOUT_VOL_ID");
        } else if (row->vol_id == UBI_CKPT_VOL_ID) {
            snprintf(vol_buf, sizeof(vol_buf), "UBI_CKPT_VOL_ID");
        } else {
            snprintf(vol_buf, sizeof(vol_buf), "%" PRIu32, row->vol_id);
        }
        snprintf(lnum_buf, sizeof(lnum_buf), "%" PRIu32, row->lnum);
        snprintf(sq_buf, sizeof(sq_buf), "%" PRIu64, row->sqnum);
        snprintf(copy_buf, sizeof(copy_buf), "%" PRIu8, row->copy_flag);
    } else {
        snprintf(vol_buf, sizeof(vol_buf), "-");
        snprintf(lnum_buf, sizeof(lnum_buf), "-");
        snprintf(sq_buf, sizeof(sq_buf), "-");
        snprintf(copy_buf, sizeof(copy_buf), "-");
    }

    printf("%-5" PRIu32 " %-7s  %-7s  %-10s  %-17s  %-4s  %-9s  %-4s  %s\n",
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
        stats->status[pnum] = PEB_BAD;
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
        stats->status[pnum] = PEB_EMPTY;
        return;
    }

    const nand_ubi_ec_hdr_t *ec_hdr = (const nand_ubi_ec_hdr_t *)page_buf;
    uint32_t magic = nand_ubi_be32(ec_hdr->magic);
    if (magic != UBI_EC_HDR_MAGIC) {
        /* Written, but not by UBI (e.g. a Dhara/FatFS image). Counted, not listed:
         * a chip full of foreign data would otherwise print a row per PEB. */
        stats->non_ubi++;
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
        stats->status[pnum] = PEB_FREE;
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
    row.compat = vid_hdr->compat;

    if (row.lnum >= peb_count) {
        stats->corrupt++;
        print_peb_line(pnum, PEB_CORRUPT, &row, "lnum out of range", header_printed);
        return;
    }
    if (row.sqnum > stats->max_sqnum) {
        stats->max_sqnum = row.sqnum;
    }

    meta_peb_t meta = {
        .pnum = (int32_t)pnum, .sqnum = row.sqnum,
        .data_size = nand_ubi_be32(vid_hdr->data_size), .data_crc = nand_ubi_be32(vid_hdr->data_crc),
    };
    if (row.vol_id == UBI_LAYOUT_VOL_ID) {
        stats->vtbl++;
        stats->status[pnum] = PEB_VTBL;
        char notes[48] = "";
        if (row.lnum < UBI_LAYOUT_VOLUME_EBS) {
            meta_peb_t *copy = &stats->vtbl_copy[row.lnum];
            if (copy->pnum < 0 || row.sqnum > copy->sqnum) {
                *copy = meta;
            }
            snprintf(notes, sizeof(notes), "volume table copy %" PRIu32, row.lnum);
        } else {
            snprintf(notes, sizeof(notes), "unexpected copy index");
        }
        print_peb_line(pnum, PEB_VTBL, &row, notes, header_printed);
        return;
    }
    if (row.vol_id == UBI_CKPT_VOL_ID) {
        stats->ckpt++;
        stats->status[pnum] = PEB_CKPT;
        if (stats->newest_ckpt.pnum < 0 || row.sqnum > stats->newest_ckpt.sqnum) {
            stats->newest_ckpt = meta;
        }
        char notes[48];
        snprintf(notes, sizeof(notes), "checkpoint, %" PRIu32 " B, compat %" PRIu8, meta.data_size, row.compat);
        print_peb_line(pnum, PEB_CKPT, &row, notes, header_printed);
        return;
    }

    stats->mapped++;
    stats->status[pnum] = PEB_MAPPED;
    if ((int32_t)row.lnum > stats->max_lnum) {
        stats->max_lnum = (int32_t)row.lnum;
    }

    print_peb_line(pnum, PEB_MAPPED, &row, "", header_printed);
}

/* Reads `len` bytes of pnum's LEB data a page at a time and hands each page to fn, with
 * the running UBI CRC. Returns the read error, if any. */
typedef void (*leb_chunk_fn)(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len);

static esp_err_t read_leb_data(esp_blockdev_handle_t nand_bdl, const dump_geometry_t *geom, uint32_t pnum,
                               uint32_t data_offset, uint32_t len, uint8_t *page_buf,
                               leb_chunk_fn fn, void *ctx, uint32_t *crc)
{
    uint64_t base = (uint64_t)pnum * geom->peb_size + data_offset;
    uint32_t running = UBI_CRC32_START;
    for (uint32_t off = 0; off < len; off += geom->page_size) {
        esp_err_t ret = nand_bdl->ops->read(nand_bdl, page_buf, geom->page_size, base + off, geom->page_size);
        if (ret != ESP_OK) {
            return ret;
        }
        uint32_t chunk = len - off < geom->page_size ? len - off : geom->page_size;
        running = nand_ubi_crc32_update(running, page_buf, chunk);
        fn(ctx, off, page_buf, chunk);
    }
    *crc = running;
    return ESP_OK;
}

/* ---- volume table ---- */

typedef struct {
    bool     print;
    uint32_t valid;
    uint32_t bad_crc;
    uint32_t total_lebs;
    nand_ubi_vtbl_record_t rec;
    uint32_t filled;
    uint32_t slot;
} vtbl_ctx_t;

static void vtbl_chunk(void *arg, uint32_t off, const uint8_t *buf, uint32_t len)
{
    (void)off;
    vtbl_ctx_t *c = arg;
    const uint32_t rec_size = sizeof(nand_ubi_vtbl_record_t);
    for (uint32_t i = 0; i < len;) {
        uint32_t take = len - i < rec_size - c->filled ? len - i : rec_size - c->filled;
        memcpy((uint8_t *)&c->rec + c->filled, buf + i, take);
        c->filled += take;
        i += take;
        if (c->filled < rec_size) {
            continue;
        }
        c->filled = 0;
        uint32_t slot = c->slot++;
        if (page_is_blank((const uint8_t *)&c->rec, rec_size)) {
            continue;
        }
        if (!nand_ubi_vtbl_record_valid(&c->rec)) {
            c->bad_crc++;
            if (c->print) {
                printf("  slot %-3" PRIu32 " bad record CRC\n", slot);
            }
            continue;
        }
        c->valid++;
        uint32_t lebs = nand_ubi_be32(c->rec.reserved_pebs);
        c->total_lebs += lebs;
        if (c->print) {
            uint16_t name_len = nand_ubi_be16(c->rec.name_len);
            if (name_len > UBI_VOL_NAME_MAX) {
                name_len = UBI_VOL_NAME_MAX;
            }
            printf("  slot %-3" PRIu32 " %-7s LEBs %-6" PRIu32 " name \"%.*s\"\n", slot,
                   c->rec.vol_type == UBI_VID_STATIC ? "static" : "dynamic", lebs, (int)name_len,
                   (const char *)c->rec.name);
        }
    }
}

/* Decodes both volume-table copies, prints the records of the first good one, and returns
 * the LEB count its volumes reserve (UINT32_MAX if no copy is good). */
static uint32_t dump_volume_table(esp_blockdev_handle_t nand_bdl, const dump_geometry_t *geom,
                                  const dump_stats_t *stats, uint8_t *page_buf)
{
    uint32_t data_offset = 2u * geom->page_size;
    uint32_t slots = (geom->peb_size - data_offset) / (uint32_t)sizeof(nand_ubi_vtbl_record_t);
    if (slots > UBI_MAX_VOLUMES) {
        slots = UBI_MAX_VOLUMES;
    }
    uint32_t table_bytes = slots * (uint32_t)sizeof(nand_ubi_vtbl_record_t);
    uint32_t result = UINT32_MAX;
    uint32_t crc_seen[UBI_LAYOUT_VOLUME_EBS] = { 0 };
    bool good[UBI_LAYOUT_VOLUME_EBS] = { false };

    ESP_LOGI(TAG, "=== volume table (%" PRIu32 " slots, %" PRIu32 " B) ===", slots, table_bytes);
    for (uint32_t copy = 0; copy < UBI_LAYOUT_VOLUME_EBS; copy++) {
        const meta_peb_t *m = &stats->vtbl_copy[copy];
        if (m->pnum < 0) {
            ESP_LOGW(TAG, "copy %" PRIu32 ": not found", copy);
            continue;
        }
        vtbl_ctx_t c = { .print = result == UINT32_MAX };
        uint32_t crc = 0;
        if (c.print) {
            printf("copy %" PRIu32 " on PEB %" PRIi32 ", sqnum %" PRIu64 ":\n", copy, m->pnum, m->sqnum);
        }
        esp_err_t ret = read_leb_data(nand_bdl, geom, (uint32_t)m->pnum, data_offset, table_bytes, page_buf,
                                      vtbl_chunk, &c, &crc);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "copy %" PRIu32 " on PEB %" PRIi32 ": read failed: 0x%x", copy, m->pnum, ret);
            continue;
        }
        bool table_crc_ok = m->data_size == 0 || (m->data_size == table_bytes && crc == m->data_crc);
        good[copy] = c.bad_crc == 0 && table_crc_ok;
        crc_seen[copy] = crc;
        ESP_LOGI(TAG, "copy %" PRIu32 " on PEB %" PRIi32 ": %" PRIu32 " volumes, %" PRIu32 " LEBs reserved, "
                 "%" PRIu32 " bad records, table CRC %s", copy, m->pnum, c.valid, c.total_lebs, c.bad_crc,
                 m->data_size == 0 ? "not recorded" : table_crc_ok ? "ok" : "MISMATCH");
        if (good[copy] && result == UINT32_MAX) {
            result = c.total_lebs;
        }
    }
    if (good[0] && good[1] && crc_seen[0] != crc_seen[1]) {
        ESP_LOGW(TAG, "the two copies differ; attach uses the one with the higher sqnum");
    }
    return result;
}

/* ---- attach checkpoint ---- */

typedef struct {
    const dump_stats_t *stats;
    uint32_t peb_count;
    nand_ubi_ckpt_hdr_t hdr;
    uint32_t pool_count;
    uint32_t prev_count;
    uint32_t state_bytes;
    uint16_t value;           /* 16-bit value being assembled */
    uint32_t state_count[4];  /* free, used, bad, erase pending */
    uint32_t bad_mismatch;    /* BAD in one of checkpoint and scan only */
    uint32_t pending_done;    /* erase pending in the checkpoint, already erased on flash */
    uint32_t mapped;
    uint32_t map_out_of_range;
    uint32_t map_not_used;    /* mapped PEB whose checkpoint state is not USED */
    uint32_t map_region;      /* mapped PEB inside the region (data from before checkpoints) */
    uint32_t map_changed;     /* mapped PEB that the scan does not see as MAPPED */
    uint32_t pool_unused;
    uint32_t pool_written;
    uint32_t pool_other;
    uint8_t  *states;         /* [peb_count / 4], for checking eba[] against states */
} ckpt_ctx_t;

static uint8_t ckpt_state_of(const ckpt_ctx_t *c, uint32_t pnum)
{
    return (c->states[pnum / 4u] >> ((pnum % 4u) * 2u)) & 0x3u;
}

static void ckpt_list_entry(ckpt_ctx_t *c, bool prev, uint32_t idx, uint16_t pnum)
{
    if (prev) {
        printf("%s%" PRIu16, idx == 0 ? "  previous pool: " : " ", pnum);
        return;
    }
    printf("%s%" PRIu16, idx == 0 ? "  pool:          " : " ", pnum);
    uint8_t st = pnum < c->peb_count ? c->stats->status[pnum] : PEB_IO_ERR;
    if (st == PEB_FREE || st == PEB_EMPTY) {
        c->pool_unused++;
    } else if (st == PEB_MAPPED) {
        c->pool_written++;
        printf("*");
    } else {
        c->pool_other++;
        printf("!");
    }
}

static void ckpt_chunk(void *arg, uint32_t off, const uint8_t *buf, uint32_t len)
{
    ckpt_ctx_t *c = arg;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t o = off + i;
        uint8_t b = buf[i];
        if (o < sizeof(nand_ubi_ckpt_hdr_t)) {
            ((uint8_t *)&c->hdr)[o] = b;
            if (o + 1u == sizeof(nand_ubi_ckpt_hdr_t)) {
                c->pool_count = nand_ubi_be32(c->hdr.pool_count);
                c->prev_count = nand_ubi_be32(c->hdr.prev_count);
            }
            continue;
        }
        o -= sizeof(nand_ubi_ckpt_hdr_t);
        uint32_t lists = 2u * (c->pool_count + c->prev_count);
        if (o < lists) {
            if (o % 2u == 0) {
                c->value = (uint16_t)(b << 8);
                continue;
            }
            c->value |= b;
            uint32_t idx = o / 2u;
            bool prev = idx >= c->pool_count;
            if (idx == c->pool_count && c->pool_count > 0) {
                printf("\n");
            }
            ckpt_list_entry(c, prev, prev ? idx - c->pool_count : idx, c->value);
            if (idx + 1u == c->pool_count + c->prev_count) {
                printf("\n");
            }
            continue;
        }
        o -= lists;
        if (o < c->state_bytes) {
            c->states[o] = b;
            for (uint32_t k = 0; k < 4u; k++) {
                uint32_t pnum = o * 4u + k;
                if (pnum >= c->peb_count) {
                    break;
                }
                uint8_t st = (b >> (k * 2u)) & 0x3u;
                c->state_count[st]++;
                c->pending_done += st == 3u && c->stats->status[pnum] == PEB_FREE;
                c->bad_mismatch += (st == 2u) != (c->stats->status[pnum] == PEB_BAD);
            }
            continue;
        }
        o -= c->state_bytes;
        if (o % 2u == 0) {
            c->value = (uint16_t)(b << 8);
            continue;
        }
        c->value |= b;
        if (c->value == 0xFFFFu) {
            continue;
        }
        c->mapped++;
        if (c->value >= c->peb_count) {
            c->map_out_of_range++;
            continue;
        }
        c->map_not_used += ckpt_state_of(c, c->value) != 1u;
        c->map_region += c->value < nand_ubi_be32(c->hdr.region_pebs);
        c->map_changed += c->stats->status[c->value] != PEB_MAPPED;
    }
}

static void dump_checkpoint(esp_blockdev_handle_t nand_bdl, const dump_geometry_t *geom,
                            const dump_stats_t *stats, uint32_t vtbl_lebs, uint8_t *page_buf)
{
    const meta_peb_t *m = &stats->newest_ckpt;
    if (m->pnum < 0) {
        ESP_LOGI(TAG, "no attach checkpoint on the chip");
        return;
    }
    ESP_LOGI(TAG, "=== attach checkpoint on PEB %" PRIi32 ", sqnum %" PRIu64 " (newest of %" PRIu32 ") ===",
             m->pnum, m->sqnum, stats->ckpt);
    if (stats->ckpt > 1) {
        ESP_LOGI(TAG, "%" PRIu32 " older checkpoint PEB(s): attach erases them", stats->ckpt - 1);
    }
    uint32_t data_offset = 2u * geom->page_size;
    if (m->data_size < sizeof(nand_ubi_ckpt_hdr_t) || m->data_size > geom->peb_size - data_offset) {
        ESP_LOGE(TAG, "data_size %" PRIu32 " out of range: attach does a full scan", m->data_size);
        return;
    }

    ckpt_ctx_t c = {
        .stats = stats,
        .peb_count = geom->peb_count,
        .state_bytes = (geom->peb_count + 3u) / 4u,
        .states = calloc((geom->peb_count + 3u) / 4u, 1),
    };
    if (c.states == NULL) {
        ESP_LOGE(TAG, "no memory to decode the checkpoint");
        return;
    }
    uint32_t crc = 0;
    esp_err_t ret = read_leb_data(nand_bdl, geom, (uint32_t)m->pnum, data_offset, m->data_size, page_buf,
                                  ckpt_chunk, &c, &crc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read failed: 0x%x", ret);
        free(c.states);
        return;
    }

    const nand_ubi_ckpt_hdr_t *h = &c.hdr;
    uint32_t leb_count = nand_ubi_be32(h->leb_count);
    uint32_t region = nand_ubi_be32(h->region_pebs);
    uint32_t expect_size = (uint32_t)sizeof(nand_ubi_ckpt_hdr_t) + 2u * (c.pool_count + c.prev_count) +
                           c.state_bytes + 2u * leb_count;
    bool magic_ok = nand_ubi_be32(h->magic) == UBI_CKPT_MAGIC && nand_ubi_be32(h->version) == UBI_CKPT_VERSION;
    ESP_LOGI(TAG, "header: magic %s, version %" PRIu32 ", image_seq 0x%08" PRIx32 ", %" PRIu32 " PEBs, "
             "%" PRIu32 " LEBs, region %" PRIu32 " PEBs, pool %" PRIu32 "/%" PRIu32 ", previous pool %" PRIu32
             ", mean EC %" PRIu64 " (%" PRIu64 " / %" PRIu32 " PEBs)", magic_ok ? "ok" : "BAD", nand_ubi_be32(h->version), nand_ubi_be32(h->image_seq),
             nand_ubi_be32(h->peb_count), leb_count, region, c.pool_count, nand_ubi_be32(h->pool_max),
             c.prev_count, nand_ubi_be64(h->mean_ec), nand_ubi_be64(h->ec_sum), nand_ubi_be32(h->ec_count));
    ESP_LOGI(TAG, "pool PEBs: %" PRIu32 " unused, %" PRIu32 " written since the checkpoint (*), %" PRIu32
             " in another state (!)", c.pool_unused, c.pool_written, c.pool_other);
    ESP_LOGI(TAG, "PEB states: %" PRIu32 " free, %" PRIu32 " used, %" PRIu32 " bad, %" PRIu32 " erase pending "
             "(%" PRIu32 " already erased, attach marks them free; %" PRIu32 " still to erase)",
             c.state_count[0], c.state_count[1], c.state_count[2], c.state_count[3], c.pending_done,
             c.state_count[3] - c.pending_done);
    ESP_LOGI(TAG, "LEB map: %" PRIu32 " of %" PRIu32 " LEBs mapped, %" PRIu32 " into the region", c.mapped,
             leb_count, c.map_region);

    /* The checks nand_ubi_attach() makes before using the checkpoint, plus consistency with
     * the scan. A failed attach check means the next attach does a full scan. */
    uint32_t failed = 0;
#define CKPT_CHECK(cond, ...) do { if (!(cond)) { failed++; ESP_LOGW(TAG, "check failed: " __VA_ARGS__); } } while (0)
    CKPT_CHECK(magic_ok, "magic or version");
    CKPT_CHECK(crc == m->data_crc, "payload CRC 0x%08" PRIx32 " != VID data_crc 0x%08" PRIx32, crc, m->data_crc);
    CKPT_CHECK(m->data_size == expect_size, "data_size %" PRIu32 " != %" PRIu32 " from the header",
               m->data_size, expect_size);
    CKPT_CHECK(nand_ubi_be32(h->pnum) == (uint32_t)m->pnum, "header pnum %" PRIu32, nand_ubi_be32(h->pnum));
    CKPT_CHECK(nand_ubi_be32(h->peb_count) == geom->peb_count, "PEB count differs from the chip");
    CKPT_CHECK(!stats->have_image_seq || nand_ubi_be32(h->image_seq) == stats->image_seq,
               "image_seq differs from the EC headers");
    CKPT_CHECK(vtbl_lebs == UINT32_MAX || leb_count == vtbl_lebs,
               "LEB count %" PRIu32 " != %" PRIu32 " reserved by the volume table", leb_count, vtbl_lebs);
    CKPT_CHECK((uint32_t)m->pnum < region, "checkpoint outside its region");
    CKPT_CHECK(c.pool_count <= nand_ubi_be32(h->pool_max) && c.prev_count <= nand_ubi_be32(h->pool_max),
               "pool lists longer than pool_max");
    CKPT_CHECK(c.map_out_of_range == 0 && c.map_not_used == 0,
               "%" PRIu32 " LEBs map to a PEB that is out of range or not USED",
               c.map_out_of_range + c.map_not_used);
    CKPT_CHECK(c.pool_other == 0, "%" PRIu32 " pool PEBs are neither free nor data", c.pool_other);
#undef CKPT_CHECK
    if (c.bad_mismatch > 0) {
        ESP_LOGI(TAG, "%" PRIu32 " PEBs differ in BAD state between the checkpoint and the scan "
                 "(blocks retired since it was written are expected)", c.bad_mismatch);
    }
    if (c.map_changed > c.map_region) {
        ESP_LOGI(TAG, "%" PRIu32 " mapped LEBs point to a PEB the scan does not see as data; expected only "
                 "for region PEBs, which attach rescans", c.map_changed - c.map_region);
    }
    if (failed == 0) {
        ESP_LOGI(TAG, "checkpoint is valid: attach reads it, rescans %" PRIu32 " region and %" PRIu32
                 " pool PEBs", region, c.pool_count);
    } else {
        ESP_LOGW(TAG, "%" PRIu32 " check(s) failed: attach would do a full scan", failed);
    }
    free(c.states);
}

static void print_summary(const dump_stats_t *stats, uint32_t page_size, uint32_t peb_size, uint32_t peb_count)
{
    ESP_LOGI(TAG, "=== UBI metadata dump summary ===");
    ESP_LOGI(TAG, "geometry: page_size=%" PRIu32 " peb_size=%" PRIu32 " peb_count=%" PRIu32,
             page_size, peb_size, peb_count);
    ESP_LOGI(TAG, "counts: EMPTY=%" PRIu32 " NON_UBI=%" PRIu32 " FREE=%" PRIu32 " MAPPED=%" PRIu32
             " VTBL=%" PRIu32 " CKPT=%" PRIu32 " CORRUPT=%" PRIu32 " BAD=%" PRIu32 " IO_ERR=%" PRIu32,
             stats->empty, stats->non_ubi, stats->free_peb, stats->mapped, stats->vtbl, stats->ckpt,
             stats->corrupt, stats->bad, stats->io_err);
    if (stats->non_ubi > 0 || stats->corrupt > 0) {
        ESP_LOGW(TAG, "a read-write nand_ubi_attach() will erase the %" PRIu32 " NON_UBI and %" PRIu32
                 " CORRUPT PEBs", stats->non_ubi, stats->corrupt);
    }

    if (stats->mapped > 0 || stats->free_peb > 0 || stats->vtbl > 0) {
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
        .vtbl_copy = { { .pnum = -1 }, { .pnum = -1 } },
        .newest_ckpt = { .pnum = -1 },
        .status = calloc(peb_count, 1),
    };
    if (stats.status == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    for (uint32_t pnum = 0; pnum < peb_count; pnum++) {
        stats.status[pnum] = PEB_CORRUPT; /* overwritten by every other outcome */
    }
    bool header_printed = false;

    for (uint32_t pnum = 0; pnum < peb_count; pnum++) {
        classify_and_print_peb(nand_bdl, pnum, &geom, page_buf, &stats, &header_printed);
    }

    if (!header_printed) {
        ESP_LOGI(TAG, "(no interesting PEBs — table omitted)");
    }

    print_summary(&stats, page_size, peb_size, peb_count);
    uint32_t vtbl_lebs = UINT32_MAX;
    if (stats.vtbl > 0) {
        vtbl_lebs = dump_volume_table(nand_bdl, &geom, &stats, page_buf);
    }
    dump_checkpoint(nand_bdl, &geom, &stats, vtbl_lebs, page_buf);

    free(stats.status);
    free(page_buf);

    ESP_ERROR_CHECK(nand_bdl->ops->release(nand_bdl));
    spi_bus_remove_device(spi);
    spi_bus_free(HOST_ID);

    ESP_LOGI(TAG, "NAND UBI metadata dump finished successfully");
}
