/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_nand_ubi.h"
#include "esp_nand_blockdev.h"

#include "nand_ubi_priv.h"
#include "esp_nand_ubi_media.h"
#include "nand_ubi_eba.h"

static const char *TAG = "nand_ubi";

static bool page_is_blank(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

static const nand_ubi_volume_info_t *find_volume(const nand_ubi_device_t *dev, uint32_t vol_id)
{
    for (uint32_t i = 0; i < dev->vol_count; i++) {
        if (dev->volumes[i].vol_id == vol_id) {
            return &dev->volumes[i];
        }
    }
    return NULL;
}

/* PEBs that can hold user LEBs: good PEBs outside the checkpoint region, minus the two
 * layout PEBs when there is no region to hold them. */
static uint32_t usable_pebs(const nand_ubi_device_t *dev)
{
    uint32_t lo = dev->ckpt ? dev->ckpt_region : 0;
    uint32_t good = 0;
    for (uint32_t pnum = lo; pnum < dev->peb_count; pnum++) {
        good += nand_ubi_eba_peb_get_state(&dev->eba, pnum) != UBI_PEB_BAD;
    }
    if (dev->ckpt) {
        return good;
    }
    return good > UBI_LAYOUT_VOLUME_EBS ? good - UBI_LAYOUT_VOLUME_EBS : 0;
}

typedef struct {
    nand_ubi_vtbl_entry_t *entries;
    uint32_t count;
} vtbl_list_t;

static uint32_t vtbl_bytes(const nand_ubi_device_t *dev)
{
    return dev->vtbl_slots * (uint32_t)sizeof(nand_ubi_vtbl_record_t);
}

static esp_err_t vtbl_list_push(vtbl_list_t *list, uint32_t slot, const nand_ubi_vtbl_record_t *rec)
{
    nand_ubi_vtbl_entry_t *grown = realloc(list->entries, (list->count + 1) * sizeof(*grown));
    if (grown == NULL) {
        return ESP_ERR_NO_MEM;
    }
    grown[list->count].slot = slot;
    grown[list->count].rec = *rec;
    list->entries = grown;
    list->count++;
    return ESP_OK;
}

static bool vtbl_lists_equal(const vtbl_list_t *a, const vtbl_list_t *b)
{
    if (a->count != b->count) {
        return false;
    }
    for (uint32_t i = 0; i < a->count; i++) {
        if (a->entries[i].slot != b->entries[i].slot ||
                memcmp(&a->entries[i].rec, &b->entries[i].rec, sizeof(a->entries[i].rec)) != 0) {
            return false;
        }
    }
    return true;
}

/* Renders page `page_idx` of the on-flash table image: 0xFF everywhere except the bytes
 * of records that overlap this page. */
static void vtbl_render_page(const nand_ubi_device_t *dev, uint32_t page_idx, uint8_t *buf)
{
    const uint32_t rec_size = sizeof(nand_ubi_vtbl_record_t);
    uint32_t lo = page_idx * dev->page_size;
    uint32_t hi = lo + dev->page_size;
    memset(buf, 0xFF, dev->page_size);
    for (uint32_t i = 0; i < dev->vtbl_entry_count; i++) {
        uint32_t rec_lo = dev->vtbl_entries[i].slot * rec_size;
        uint32_t from = rec_lo > lo ? rec_lo : lo;
        uint32_t to = rec_lo + rec_size < hi ? rec_lo + rec_size : hi;
        if (from < to) {
            memcpy(buf + (from - lo), (const uint8_t *)&dev->vtbl_entries[i].rec + (from - rec_lo), to - from);
        }
    }
}

/* Reads a table image at addr one page at a time, rebuilding records that straddle page
 * boundaries. Non-blank records go into list; *records_ok turns false if any of them fails
 * its CRC. *crc receives the UBI CRC of the whole image (vtbl_bytes(dev) bytes). */
static esp_err_t vtbl_read_records(nand_ubi_device_t *dev, uint64_t addr, uint8_t *page,
                                   vtbl_list_t *list, bool *records_ok, uint32_t *crc)
{
    const uint32_t rec_size = sizeof(nand_ubi_vtbl_record_t);
    uint32_t total = vtbl_bytes(dev);
    nand_ubi_vtbl_record_t rec;
    uint32_t filled = 0;
    uint32_t slot = 0;
    uint32_t running = UBI_CRC32_START;
    *records_ok = true;

    for (uint32_t off = 0; off < total; off += dev->page_size) {
        esp_err_t ret = dev->nand_bdl->ops->read(dev->nand_bdl, page, dev->page_size, addr + off, dev->page_size);
        if (ret != ESP_OK) {
            return ret;
        }
        uint32_t chunk = total - off < dev->page_size ? total - off : dev->page_size;
        running = nand_ubi_crc32_update(running, page, chunk);
        for (uint32_t i = 0; i < chunk;) {
            uint32_t take = chunk - i < rec_size - filled ? chunk - i : rec_size - filled;
            memcpy((uint8_t *)&rec + filled, page + i, take);
            filled += take;
            i += take;
            if (filled < rec_size) {
                continue;
            }
            if (!page_is_blank((const uint8_t *)&rec, rec_size)) {
                if (!nand_ubi_vtbl_record_valid(&rec)) {
                    *records_ok = false;
                } else {
                    ret = vtbl_list_push(list, slot, &rec);
                    if (ret != ESP_OK) {
                        return ret;
                    }
                }
            }
            slot++;
            filled = 0;
        }
    }
    *crc = running;
    return ESP_OK;
}

static esp_err_t read_vtbl_copy(nand_ubi_device_t *dev, uint32_t copy, uint8_t *page,
                                vtbl_list_t *list, bool *blank, bool *valid,
                                uint64_t *sqnum, uint32_t *image_seq)
{
    *blank = false;
    *valid = false;
    if (dev->vtbl_pnum[copy] < 0) {
        *blank = true;
        return ESP_OK;
    }
    uint64_t base = (uint64_t)dev->vtbl_pnum[copy] * dev->peb_size;

    esp_err_t ret = dev->nand_bdl->ops->read(dev->nand_bdl, page, dev->page_size, base, dev->page_size);
    if (ret != ESP_OK) {
        return ret;
    }
    if (page_is_blank(page, dev->page_size)) {
        *blank = true;
        return ESP_OK;
    }
    const nand_ubi_ec_hdr_t *ec = (const nand_ubi_ec_hdr_t *)page;
    if (!nand_ubi_ec_hdr_valid(ec) || nand_ubi_be32(ec->vid_hdr_offset) != dev->vid_hdr_offset ||
            nand_ubi_be32(ec->data_offset) != dev->data_offset) {
        return ESP_OK;
    }
    *image_seq = nand_ubi_be32(ec->image_seq);

    ret = dev->nand_bdl->ops->read(dev->nand_bdl, page, dev->page_size, base + dev->vid_hdr_offset, dev->page_size);
    if (ret != ESP_OK) {
        return ret;
    }
    const nand_ubi_vid_hdr_t *vid = (const nand_ubi_vid_hdr_t *)page;
    if (!nand_ubi_vid_hdr_valid(vid) || nand_ubi_be32(vid->vol_id) != UBI_LAYOUT_VOL_ID ||
            nand_ubi_be32(vid->lnum) != copy) {
        return ESP_OK;
    }
    *sqnum = nand_ubi_be64(vid->sqnum);
    /* data_size/data_crc cover the whole table. They catch a copy cut off part-way
     * through the table write: its trailing pages read as 0xFF, which would otherwise
     * pass as empty records. data_size == 0 is the older format without the CRC. */
    uint32_t data_size = nand_ubi_be32(vid->data_size);
    uint32_t data_crc = nand_ubi_be32(vid->data_crc);

    bool records_ok = false;
    uint32_t crc = 0;
    ret = vtbl_read_records(dev, base + dev->data_offset, page, list, &records_ok, &crc);
    if (ret != ESP_OK) {
        return ret;
    }
    *valid = records_ok && (data_size == 0 || (data_size == vtbl_bytes(dev) && crc == data_crc));
    return ESP_OK;
}

/** Newest checkpoint seen while locating the layout PEBs. */
typedef struct {
    int32_t  pnum;
    uint64_t sqnum;
    uint32_t data_size;
    uint32_t data_crc;
} ckpt_cand_t;

/* Finds the PEBs holding the volume-table copies by their VID header (vol_id =
 * UBI_LAYOUT_VOL_ID, lnum = copy index), so the copies may sit on any good PEB and a
 * factory-bad PEB 0/1 is no problem. Stops once both copies are found and, in checkpoint
 * mode, the whole region was read; the region's newest checkpoint goes to *cand. */
static esp_err_t locate_layout_pebs(nand_ubi_device_t *dev, ckpt_cand_t *cand)
{
    uint8_t *page = ubi_alloc(dev->page_size);
    if (page == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint64_t best_sqnum[UBI_LAYOUT_VOLUME_EBS] = {0};
    uint32_t found = 0;
    esp_err_t ret = ESP_OK;
    uint32_t min_scan = dev->ckpt ? dev->ckpt_region : 0;
    cand->pnum = -1;
    for (uint32_t pnum = 0; pnum < dev->peb_count && (found < UBI_LAYOUT_VOLUME_EBS || pnum < min_scan); pnum++) {
        if ((pnum & 0x1FU) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        esp_blockdev_cmd_arg_status_t bad_arg = { .num = pnum, .status = false };
        ret = dev->nand_bdl->ops->ioctl(dev->nand_bdl, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &bad_arg);
        if (ret != ESP_OK) {
            break;
        }
        if (bad_arg.status) {
            continue;
        }
        ret = dev->nand_bdl->ops->read(dev->nand_bdl, page, dev->page_size,
                                       (uint64_t)pnum * dev->peb_size + dev->vid_hdr_offset, dev->page_size);
        if (ret != ESP_OK) {
            break;
        }
        const nand_ubi_vid_hdr_t *vid = (const nand_ubi_vid_hdr_t *)page;
        if (!nand_ubi_vid_hdr_valid(vid)) {
            continue;
        }
        if (nand_ubi_be32(vid->vol_id) == UBI_CKPT_VOL_ID && pnum < min_scan && nand_ubi_be32(vid->lnum) == 0) {
            uint64_t sqnum = nand_ubi_be64(vid->sqnum);
            if (cand->pnum < 0 || sqnum > cand->sqnum) {
                *cand = (ckpt_cand_t) {
                    .pnum = (int32_t)pnum, .sqnum = sqnum,
                    .data_size = nand_ubi_be32(vid->data_size), .data_crc = nand_ubi_be32(vid->data_crc),
                };
            }
            if (sqnum > dev->global_sqnum) {
                dev->global_sqnum = sqnum;
            }
            continue;
        }
        if (nand_ubi_be32(vid->vol_id) != UBI_LAYOUT_VOL_ID) {
            continue;
        }
        uint32_t copy = nand_ubi_be32(vid->lnum);
        uint64_t sqnum = nand_ubi_be64(vid->sqnum);
        if (copy >= UBI_LAYOUT_VOLUME_EBS) {
            continue;
        }
        if (dev->vtbl_pnum[copy] < 0) {
            found++;
        } else if (sqnum <= best_sqnum[copy]) {
            continue;
        }
        dev->vtbl_pnum[copy] = (int32_t)pnum;
        best_sqnum[copy] = sqnum;
        if (sqnum > dev->global_sqnum) {
            dev->global_sqnum = sqnum; /* the main scan skips layout PEBs */
        }
    }
    free(page);
    return ret;
}

/* Sets *found when at least one copy is valid; dev->image_seq is then taken from it. */
static esp_err_t load_volume_table(nand_ubi_device_t *dev, bool *found)
{
    *found = false;
    uint8_t *page = ubi_alloc(dev->page_size);
    if (page == NULL) {
        return ESP_ERR_NO_MEM;
    }
    vtbl_list_t lists[UBI_LAYOUT_VOLUME_EBS] = { 0 };
    bool blank[UBI_LAYOUT_VOLUME_EBS] = { 0 };
    bool valid[UBI_LAYOUT_VOLUME_EBS] = { 0 };
    uint64_t sqnum[UBI_LAYOUT_VOLUME_EBS] = { 0 };
    uint32_t image_seq[UBI_LAYOUT_VOLUME_EBS] = { 0 };
    esp_err_t ret = ESP_OK;
    for (uint32_t copy = 0; copy < UBI_LAYOUT_VOLUME_EBS && ret == ESP_OK; copy++) {
        ret = read_vtbl_copy(dev, copy, page, &lists[copy], &blank[copy], &valid[copy],
                             &sqnum[copy], &image_seq[copy]);
    }
    free(page);
    if (ret != ESP_OK || (!valid[0] && !valid[1])) {
        if (ret == ESP_OK && (!blank[0] || !blank[1])) {
            ESP_LOGE(TAG, "both volume-table copies are invalid");
        }
        free(lists[0].entries);
        free(lists[1].entries);
        return ret;
    }

    uint32_t sel = valid[0] ? 0 : 1;
    if (valid[0] != valid[1]) {
        ESP_LOGW(TAG, "only volume-table copy %" PRIu32 " is valid", sel);
    } else if (!vtbl_lists_equal(&lists[0], &lists[1])) {
        sel = sqnum[1] > sqnum[0] ? 1 : 0;
        ESP_LOGW(TAG, "volume-table copies disagree; using newer copy %" PRIu32, sel);
    }
    for (uint32_t copy = 0; copy < UBI_LAYOUT_VOLUME_EBS; copy++) {
        dev->vtbl_copy_good[copy] = valid[copy] && vtbl_lists_equal(&lists[copy], &lists[sel]);
    }
    dev->image_seq = image_seq[sel];
    dev->vtbl_entries = lists[sel].entries;
    dev->vtbl_entry_count = lists[sel].count;
    free(lists[sel ^ 1].entries);
    *found = true;

    if (dev->vtbl_entry_count > 0) {
        dev->volumes = calloc(dev->vtbl_entry_count, sizeof(*dev->volumes));
        if (dev->volumes == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    uint32_t eba_offset = 0;
    for (uint32_t i = 0; i < dev->vtbl_entry_count; i++) {
        const nand_ubi_vtbl_entry_t *e = &dev->vtbl_entries[i];
        uint32_t leb_count = nand_ubi_be32(e->rec.reserved_pebs);
        if (leb_count == 0) {
            continue;
        }
        if (leb_count > dev->peb_count || eba_offset > dev->peb_count - leb_count) {
            ESP_LOGE(TAG, "vtbl slot %" PRIu32 " has invalid LEB count %" PRIu32, e->slot, leb_count);
            continue;
        }
        dev->volumes[dev->vol_count++] = (nand_ubi_volume_info_t) {
            .vol_id = e->slot,
            .leb_count = leb_count,
            .eba_offset = eba_offset,
            .vol_type = e->rec.vol_type,
        };
        eba_offset += leb_count;
    }
    return ESP_OK;
}

static void fill_ec_header(nand_ubi_device_t *dev, nand_ubi_ec_hdr_t *ec_hdr, uint64_t ec)
{
    memset(ec_hdr, 0, sizeof(*ec_hdr));
    ec_hdr->magic = nand_ubi_be32(UBI_EC_HDR_MAGIC);
    ec_hdr->version = UBI_VERSION;
    ec_hdr->ec = nand_ubi_be64(ec);
    ec_hdr->vid_hdr_offset = nand_ubi_be32(dev->vid_hdr_offset);
    ec_hdr->data_offset = nand_ubi_be32(dev->data_offset);
    ec_hdr->image_seq = nand_ubi_be32(dev->image_seq);
    ec_hdr->hdr_crc = nand_ubi_be32(nand_ubi_crc32(ec_hdr, UBI_EC_HDR_SIZE_CRC));
}

static void fill_vid_header(nand_ubi_device_t *dev, nand_ubi_vid_hdr_t *vid_hdr,
                            uint32_t vol_id, uint32_t lnum, uint8_t vol_type)
{
    memset(vid_hdr, 0, sizeof(*vid_hdr));
    vid_hdr->magic = nand_ubi_be32(UBI_VID_HDR_MAGIC);
    vid_hdr->version = UBI_VERSION;
    vid_hdr->vol_type = vol_type;
    vid_hdr->vol_id = nand_ubi_be32(vol_id);
    vid_hdr->lnum = nand_ubi_be32(lnum);
    vid_hdr->sqnum = nand_ubi_be64(++dev->global_sqnum);
    vid_hdr->hdr_crc = nand_ubi_be32(nand_ubi_crc32(vid_hdr, UBI_VID_HDR_SIZE_CRC));
}

/* Marks pnum bad on the chip and in RAM. Returns the MARK_BAD_BLOCK ioctl result; the PEB
 * is never handed out again in this session either way. */
static esp_err_t retire_peb(nand_ubi_device_t *dev, uint32_t pnum)
{
    nand_ubi_eba_peb_set_bad(&dev->eba, pnum);
    uint32_t block = pnum;
    esp_err_t ret = dev->nand_bdl->ops->ioctl(dev->nand_bdl, ESP_BLOCKDEV_CMD_MARK_BAD_BLOCK, &block);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pnum=%" PRIu32 ": marking bad failed: 0x%x", pnum, ret);
    } else {
        ESP_LOGW(TAG, "pnum=%" PRIu32 ": retired as bad", pnum);
    }
    return ret;
}

/* Picks the least-erased of CONFIG_ESP_NAND_UBI_ALLOC_SAMPLES randomly chosen free PEBs by
 * reading their EC headers. A PEB without a valid header is most likely factory-fresh and
 * ranks as never erased, so unused PEBs are taken first. Sets
 * *has_ec_hdr for the chosen PEB. Returns ESP_ERR_NO_MEM if no PEB is free, or the read
 * error if no candidate could be read. page_buf must hold dev->page_size bytes. */
static esp_err_t find_free_peb(nand_ubi_device_t *dev, uint8_t *page_buf, int32_t *out_pnum, bool *has_ec_hdr)
{
    int32_t cand[CONFIG_ESP_NAND_UBI_ALLOC_SAMPLES];
    uint32_t n = nand_ubi_eba_sample_free(&dev->eba, 0, cand, CONFIG_ESP_NAND_UBI_ALLOC_SAMPLES);
    esp_err_t ret = ESP_ERR_NO_MEM;
    uint64_t best_ec = UINT64_MAX;
    *out_pnum = -1;
    for (uint32_t i = 0; i < n; i++) {
        esp_err_t read_ret = dev->nand_bdl->ops->read(dev->nand_bdl, page_buf, dev->page_size,
                                                      (uint64_t)cand[i] * dev->peb_size, dev->page_size);
        if (read_ret != ESP_OK) {
            ESP_LOGW(TAG, "pnum=%" PRIi32 ": EC header read failed: 0x%x", cand[i], read_ret);
            if (*out_pnum < 0) {
                ret = read_ret;
            }
            continue;
        }
        const nand_ubi_ec_hdr_t *ec_hdr = (const nand_ubi_ec_hdr_t *)page_buf;
        bool valid = nand_ubi_ec_hdr_valid(ec_hdr);
        uint64_t ec = valid ? nand_ubi_be64(ec_hdr->ec) : 0;
        if (ec < best_ec) {
            best_ec = ec;
            *out_pnum = cand[i];
            *has_ec_hdr = valid;
            ret = ESP_OK;
        }
    }
    return ret;
}

/* Erases pnum and writes a fresh EC header with the PEB's previous ec+1. Without a usable
 * header the PEB is assumed to be average, so it gets mean_ec+1 (as in Linux UBI). Keeps
 * ec_sum and ec_count up to date. No PEB state change. */
static esp_err_t erase_and_stamp_ec(nand_ubi_device_t *dev, uint32_t pnum, uint8_t *page_buf)
{
    uint64_t base = (uint64_t)pnum * dev->peb_size;
    uint64_t ec = dev->mean_ec;
    bool had_hdr = false;
    if (dev->nand_bdl->ops->read(dev->nand_bdl, page_buf, dev->page_size, base, dev->page_size) == ESP_OK &&
            nand_ubi_ec_hdr_valid((const nand_ubi_ec_hdr_t *)page_buf)) {
        ec = nand_ubi_be64(((const nand_ubi_ec_hdr_t *)page_buf)->ec);
        had_hdr = true;
    }

    esp_err_t ret = dev->nand_bdl->ops->erase(dev->nand_bdl, base, dev->peb_size);
    if (ret == ESP_OK) {
        memset(page_buf, 0xFF, dev->page_size);
        fill_ec_header(dev, (nand_ubi_ec_hdr_t *)page_buf, ec + 1);
        ret = dev->nand_bdl->ops->write(dev->nand_bdl, page_buf, base, dev->page_size);
    }
    if (ret == ESP_OK) {
        dev->ec_sum += had_hdr ? 1u : ec + 1u;
        dev->ec_count += had_hdr ? 0u : 1u;
    }
    return ret;
}

/* Returns pnum to the free pool via erase_and_stamp_ec(). A PEB that fails to erase or to
 * take the new header is retired. Returns ESP_OK when the PEB ended up FREE or was retired;
 * an error only if retiring it failed as well. page_buf must hold dev->page_size bytes. */
static esp_err_t reclaim_peb(nand_ubi_device_t *dev, uint32_t pnum, uint8_t *page_buf)
{
    esp_err_t ret = erase_and_stamp_ec(dev, pnum, page_buf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "pnum=%" PRIu32 ": reclaim failed: 0x%x", pnum, ret);
        return retire_peb(dev, pnum);
    }
    nand_ubi_eba_peb_set_free(&dev->eba, pnum);
    return ESP_OK;
}

/* Rewrites volume-table copy `copy` in place on its PEB (allocating a free PEB the first
 * time). The EC is carried over (+1). If the PEB fails to erase or program, it is retired
 * and the copy moves to another free PEB; bounded like nand_ubi_vol_alloc_peb(). */
static esp_err_t write_vtbl_copy(nand_ubi_device_t *dev, uint32_t copy)
{
    enum { WRITE_ATTEMPTS = 3 };
    uint32_t table_bytes = vtbl_bytes(dev);
    uint32_t table_pages = (table_bytes + dev->page_size - 1u) / dev->page_size;
    uint8_t *page = ubi_alloc(dev->page_size);
    if (page == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint32_t table_crc = UBI_CRC32_START;
    for (uint32_t p = 0; p < table_pages; p++) {
        vtbl_render_page(dev, p, page);
        uint32_t chunk = table_bytes - p * dev->page_size;
        table_crc = nand_ubi_crc32_update(table_crc, page, chunk < dev->page_size ? chunk : dev->page_size);
    }

    esp_err_t ret = ESP_ERR_NO_MEM;
    for (int attempt = 0; attempt < WRITE_ATTEMPTS; attempt++) {
        int32_t pnum = dev->vtbl_pnum[copy];
        if (pnum < 0) {
            /* Lowest free PEB, not a sampled one: locate_layout_pebs() scans from PEB 0
             * and stops once both copies are found, so they must stay near the start. */
            for (uint32_t p = 0; p < dev->peb_count && pnum < 0; p++) {
                if (nand_ubi_eba_peb_is_free(&dev->eba, p)) {
                    pnum = (int32_t)p;
                }
            }
            if (pnum < 0) {
                ret = ESP_ERR_NO_MEM;
                break;
            }
            nand_ubi_eba_peb_set_used(&dev->eba, (uint32_t)pnum);
            dev->vtbl_pnum[copy] = pnum;
        }
        uint64_t base = (uint64_t)pnum * dev->peb_size;

        dev->vtbl_copy_good[copy] = false;
        ret = erase_and_stamp_ec(dev, (uint32_t)pnum, page);
        if (ret == ESP_OK) {
            memset(page, 0xFF, dev->page_size);
            fill_vid_header(dev, (nand_ubi_vid_hdr_t *)page, UBI_LAYOUT_VOL_ID, copy, UBI_VID_DYNAMIC);
            ((nand_ubi_vid_hdr_t *)page)->compat = UBI_COMPAT_REJECT;
            ((nand_ubi_vid_hdr_t *)page)->data_size = nand_ubi_be32(table_bytes);
            ((nand_ubi_vid_hdr_t *)page)->data_crc = nand_ubi_be32(table_crc);
            ((nand_ubi_vid_hdr_t *)page)->hdr_crc = nand_ubi_be32(nand_ubi_crc32(page, UBI_VID_HDR_SIZE_CRC));
            ret = dev->nand_bdl->ops->write(dev->nand_bdl, page, base + dev->vid_hdr_offset, dev->page_size);
        }
        for (uint32_t p = 0; p < table_pages && ret == ESP_OK; p++) {
            vtbl_render_page(dev, p, page);
            ret = dev->nand_bdl->ops->write(dev->nand_bdl, page,
                                            base + dev->data_offset + (uint64_t)p * dev->page_size, dev->page_size);
        }
        if (ret == ESP_OK) {
            dev->vtbl_copy_good[copy] = true;
            break;
        }

        ESP_LOGW(TAG, "volume-table copy %" PRIu32 " on pnum=%" PRIi32 " failed: 0x%x", copy, pnum, ret);
        dev->vtbl_pnum[copy] = -1;
        esp_err_t retire_ret = retire_peb(dev, (uint32_t)pnum);
        if (retire_ret != ESP_OK) {
            ret = retire_ret;
            break;
        }
    }
    free(page);
    return ret;
}

/**************************************************************************************
 * Attach checkpoint: writing. Format and invariants: docs/fastmap-design.md.
 **************************************************************************************
 */

#define CKPT_HDR_SIZE ((uint32_t)sizeof(nand_ubi_ckpt_hdr_t))

static uint32_t ckpt_state_bytes(const nand_ubi_device_t *dev)
{
    return (dev->peb_count + 3u) / 4u;
}

static uint32_t ckpt_size(const nand_ubi_device_t *dev, uint32_t pool_count, uint32_t prev_count,
                          uint32_t leb_count)
{
    return CKPT_HDR_SIZE + 2u * (pool_count + prev_count) + ckpt_state_bytes(dev) + 2u * leb_count;
}

static void ckpt_render_header(const nand_ubi_device_t *dev, uint32_t pnum, nand_ubi_ckpt_hdr_t *hdr)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = nand_ubi_be32(UBI_CKPT_MAGIC);
    hdr->version = nand_ubi_be32(UBI_CKPT_VERSION);
    hdr->image_seq = nand_ubi_be32(dev->image_seq);
    hdr->peb_count = nand_ubi_be32(dev->peb_count);
    hdr->leb_count = nand_ubi_be32(dev->eba.leb_count);
    hdr->region_pebs = nand_ubi_be32(dev->ckpt_region);
    hdr->pool_max = nand_ubi_be32(dev->ckpt_pool_max);
    hdr->pool_count = nand_ubi_be32(dev->pool_count);
    hdr->pnum = nand_ubi_be32(pnum);
    hdr->prev_count = nand_ubi_be32(dev->prev_count);
    /* The writer erases the previous checkpoint and the pending PEBs right after this one is
     * written; each adds 1 to the erase-count sum, so the stored sum includes them. */
    uint64_t sum = dev->ec_sum + (dev->ckpt_pnum >= 0 ? 1u : 0u);
    for (uint32_t p = 0; p < dev->peb_count; p++) {
        sum += nand_ubi_eba_peb_get_state(&dev->eba, p) == UBI_PEB_ERASE_PENDING;
    }
    hdr->mean_ec = nand_ubi_be64(dev->ec_count ? sum / dev->ec_count : 0);
    hdr->ec_sum = nand_ubi_be64(sum);
    hdr->ec_count = nand_ubi_be32(dev->ec_count);
}

/* Byte o of the payload: header, pool, previous pool, peb_state, eba[], 16-bit values
 * big-endian. */
static uint8_t ckpt_byte(const nand_ubi_device_t *dev, const uint8_t *hdr, uint32_t o)
{
    if (o < CKPT_HDR_SIZE) {
        return hdr[o];
    }
    o -= CKPT_HDR_SIZE;
    if (o < 2u * dev->pool_count) {
        uint16_t v = dev->pool[o / 2u];
        return o % 2u ? (uint8_t)v : (uint8_t)(v >> 8);
    }
    o -= 2u * dev->pool_count;
    if (o < 2u * dev->prev_count) {
        uint16_t v = dev->prev_pool[o / 2u];
        return o % 2u ? (uint8_t)v : (uint8_t)(v >> 8);
    }
    o -= 2u * dev->prev_count;
    if (o < ckpt_state_bytes(dev)) {
        return dev->eba.peb_state[o];
    }
    o -= ckpt_state_bytes(dev);
    uint16_t v = dev->eba.eba[o / 2u];
    return o % 2u ? (uint8_t)v : (uint8_t)(v >> 8);
}

static void ckpt_render_page(const nand_ubi_device_t *dev, const uint8_t *hdr, uint32_t total,
                             uint32_t page_idx, uint8_t *buf)
{
    uint32_t base = page_idx * dev->page_size;
    for (uint32_t i = 0; i < dev->page_size; i++) {
        buf[i] = base + i < total ? ckpt_byte(dev, hdr, base + i) : 0xFFu;
    }
}

/* Reads pnum's EC header; a PEB without a valid one counts as never erased. */
static esp_err_t read_ec(nand_ubi_device_t *dev, uint32_t pnum, uint8_t *page, uint64_t *ec, bool *valid)
{
    esp_err_t ret = dev->nand_bdl->ops->read(dev->nand_bdl, page, dev->page_size,
                                             (uint64_t)pnum * dev->peb_size, dev->page_size);
    if (ret != ESP_OK) {
        return ret;
    }
    const nand_ubi_ec_hdr_t *ec_hdr = (const nand_ubi_ec_hdr_t *)page;
    *valid = nand_ubi_ec_hdr_valid(ec_hdr);
    *ec = *valid ? nand_ubi_be64(ec_hdr->ec) : 0;
    return ESP_OK;
}

/* Keeps the pool entries that are still free, in order, and tops the pool up to
 * ckpt_pool_max with the least-erased of k sampled free PEBs outside the region. Pool PEBs
 * are marked USED while sampling so they are not drawn twice. */
static esp_err_t ckpt_fill_pool(nand_ubi_device_t *dev, uint8_t *page)
{
    esp_err_t ret = ESP_OK;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < dev->pool_count; i++) {
        if (nand_ubi_eba_peb_is_free(&dev->eba, dev->pool[i])) {
            nand_ubi_eba_peb_set_used(&dev->eba, dev->pool[i]);
            dev->pool[kept++] = dev->pool[i];
        }
    }
    dev->pool_count = kept;
    while (dev->pool_count < dev->ckpt_pool_max) {
        int32_t cand[CONFIG_ESP_NAND_UBI_ALLOC_SAMPLES];
        uint32_t n = nand_ubi_eba_sample_free(&dev->eba, dev->ckpt_region, cand, CONFIG_ESP_NAND_UBI_ALLOC_SAMPLES);
        int32_t best = -1;
        uint64_t best_ec = UINT64_MAX;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t ec = 0;
            bool valid = false;
            ret = read_ec(dev, (uint32_t)cand[i], page, &ec, &valid);
            if (ret == ESP_OK && ec < best_ec) {
                best = cand[i];
                best_ec = ec;
            }
        }
        if (best < 0) {
            break;
        }
        ret = ESP_OK;
        nand_ubi_eba_peb_set_used(&dev->eba, (uint32_t)best);
        dev->pool[dev->pool_count++] = (uint16_t)best;
    }
    for (uint32_t i = 0; i < dev->pool_count; i++) {
        nand_ubi_eba_peb_set_free(&dev->eba, dev->pool[i]);
    }
    return ret;
}

/* The free region PEB with the lowest erase count, or -1. */
static int32_t ckpt_pick_region_peb(nand_ubi_device_t *dev, uint8_t *page)
{
    int32_t best = -1;
    uint64_t best_ec = UINT64_MAX;
    for (uint32_t pnum = 0; pnum < dev->ckpt_region; pnum++) {
        uint64_t ec = 0;
        bool valid = false;
        if (nand_ubi_eba_peb_is_free(&dev->eba, pnum) &&
                read_ec(dev, pnum, page, &ec, &valid) == ESP_OK && ec < best_ec) {
            best = (int32_t)pnum;
            best_ec = ec;
        }
    }
    return best;
}

/* Writes the VID header (vol_id UBI_CKPT_VOL_ID, compat DELETE so other UBI implementations
 * erase it) and then the payload, which is rendered twice: once for its CRC, once to write. */
static esp_err_t ckpt_write_payload(nand_ubi_device_t *dev, uint32_t pnum, uint8_t *page)
{
    nand_ubi_ckpt_hdr_t hdr_struct;
    ckpt_render_header(dev, pnum, &hdr_struct);
    const uint8_t *hdr = (const uint8_t *)&hdr_struct;
    uint32_t total = ckpt_size(dev, dev->pool_count, dev->prev_count, dev->eba.leb_count);
    uint32_t pages = (total + dev->page_size - 1u) / dev->page_size;
    uint32_t crc = UBI_CRC32_START;
    for (uint32_t i = 0; i < pages; i++) {
        ckpt_render_page(dev, hdr, total, i, page);
        uint32_t chunk = total - i * dev->page_size;
        crc = nand_ubi_crc32_update(crc, page, chunk < dev->page_size ? chunk : dev->page_size);
    }

    uint64_t base = (uint64_t)pnum * dev->peb_size;
    memset(page, 0xFF, dev->page_size);
    nand_ubi_vid_hdr_t *vid = (nand_ubi_vid_hdr_t *)page;
    fill_vid_header(dev, vid, UBI_CKPT_VOL_ID, 0, UBI_VID_DYNAMIC);
    vid->compat = UBI_COMPAT_DELETE;
    vid->data_size = nand_ubi_be32(total);
    vid->data_crc = nand_ubi_be32(crc);
    vid->hdr_crc = nand_ubi_be32(nand_ubi_crc32(vid, UBI_VID_HDR_SIZE_CRC));
    esp_err_t ret = dev->nand_bdl->ops->write(dev->nand_bdl, page, base + dev->vid_hdr_offset, dev->page_size);
    for (uint32_t i = 0; i < pages && ret == ESP_OK; i++) {
        ckpt_render_page(dev, hdr, total, i, page);
        ret = dev->nand_bdl->ops->write(dev->nand_bdl, page,
                                        base + dev->data_offset + (uint64_t)i * dev->page_size, dev->page_size);
    }
    return ret;
}

static bool in_list(const uint16_t *list, uint32_t count, uint32_t pnum)
{
    for (uint32_t i = 0; i < count; i++) {
        if (list[i] == pnum) {
            return true;
        }
    }
    return false;
}

/* Erases every ERASE_PENDING PEB, oldest data first, so that a power cut part-way never
 * leaves an older copy of a LEB without the newer one for a full scan to find. PEBs in
 * late[] were allocated, in that order, after every other pending PEB was written. */
static void ckpt_reclaim_pending(nand_ubi_device_t *dev, uint8_t *page, const uint16_t *late, uint32_t late_count)
{
    for (uint32_t pnum = 0; pnum < dev->peb_count; pnum++) {
        if (nand_ubi_eba_peb_get_state(&dev->eba, pnum) == UBI_PEB_ERASE_PENDING &&
                !in_list(late, late_count, pnum)) {
            (void)reclaim_peb(dev, pnum, page);
        }
    }
    for (uint32_t i = 0; i < late_count; i++) {
        if (nand_ubi_eba_peb_get_state(&dev->eba, late[i]) == UBI_PEB_ERASE_PENDING) {
            (void)reclaim_peb(dev, late[i], page);
        }
    }
}

/* Leaves checkpoint mode for this session. The newest checkpoint is erased first, since
 * writes outside its pool would make it wrong; if that fails the device goes read-only. */
static esp_err_t ckpt_disable(nand_ubi_device_t *dev, uint8_t *page)
{
    ESP_LOGW(TAG, "attach checkpoint disabled until the next attach");
    if (dev->ckpt_pnum >= 0) {
        esp_err_t ret = reclaim_peb(dev, (uint32_t)dev->ckpt_pnum, page);
        dev->ckpt_pnum = -1;
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "could not erase the checkpoint, switching to read-only: 0x%x", ret);
            dev->read_only = true;
            return ret;
        }
    }
    dev->ckpt = false;
    /* Called before or after ckpt_write() moved the pool to prev_pool; pending PEBs of the
     * current round are in whichever list holds allocated entries. */
    const uint16_t *late = dev->prev_count > 0 ? dev->prev_pool : dev->pool;
    ckpt_reclaim_pending(dev, page, late, dev->prev_count > 0 ? dev->prev_count : dev->pool_count);
    dev->pool_count = 0;
    dev->prev_count = 0;
    return ESP_OK;
}

/* Writes a new checkpoint with a fresh pool, erases the previous one, then the PEBs whose
 * erase was deferred. If no checkpoint can be written, checkpoint mode is left. */
static esp_err_t ckpt_write(nand_ubi_device_t *dev, uint8_t *page)
{
    enum { WRITE_ATTEMPTS = 3 };
    if (ckpt_size(dev, dev->ckpt_pool_max, dev->ckpt_pool_max, dev->eba.leb_count) > dev->leb_size) {
        ESP_LOGW(TAG, "checkpoint does not fit in one LEB");
        return ckpt_disable(dev, page);
    }
    /* Pending PEBs from the pool being replaced are erased after the older ones. */
    memcpy(dev->prev_pool, dev->pool, dev->pool_count * sizeof(uint16_t));
    dev->prev_count = dev->pool_count;
    esp_err_t ret = ckpt_fill_pool(dev, page);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "checkpoint pool fill failed: 0x%x", ret);
        return ckpt_disable(dev, page);
    }

    int32_t pnum = -1;
    for (int attempt = 0; attempt < WRITE_ATTEMPTS; attempt++) {
        pnum = ckpt_pick_region_peb(dev, page);
        if (pnum < 0) {
            break;
        }
        nand_ubi_eba_peb_set_used(&dev->eba, (uint32_t)pnum);
        ret = erase_and_stamp_ec(dev, (uint32_t)pnum, page);
        if (ret == ESP_OK) {
            ret = ckpt_write_payload(dev, (uint32_t)pnum, page);
        }
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "checkpoint write to pnum=%" PRIi32 " failed: 0x%x", pnum, ret);
        (void)retire_peb(dev, (uint32_t)pnum);
        pnum = -1;
    }
    if (pnum < 0) {
        return ckpt_disable(dev, page);
    }

    int32_t old = dev->ckpt_pnum;
    dev->ckpt_pnum = pnum;
    if (old >= 0) {
        ret = reclaim_peb(dev, (uint32_t)old, page);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "could not erase the previous checkpoint, switching to read-only: 0x%x", ret);
            dev->read_only = true;
            return ret;
        }
    }
    ckpt_reclaim_pending(dev, page, dev->prev_pool, dev->prev_count);
    dev->ckpt_dirty = false;
    return ESP_OK;
}

/* Next PEB for a LEB write. In checkpoint mode it comes from the pool; an empty pool
 * writes a checkpoint first. */
static esp_err_t take_free_peb(nand_ubi_device_t *dev, uint8_t *page, int32_t *out_pnum, bool *has_ec_hdr)
{
    for (int round = 0; dev->ckpt && round < 2; round++) {
        for (uint32_t i = 0; i < dev->pool_count; i++) {
            if (nand_ubi_eba_peb_is_free(&dev->eba, dev->pool[i])) {
                uint64_t ec = 0;
                *out_pnum = dev->pool[i];
                return read_ec(dev, dev->pool[i], page, &ec, has_ec_hdr);
            }
        }
        if (round == 0) {
            esp_err_t ret = ckpt_write(dev, page);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }
    if (dev->ckpt) {
        return ESP_ERR_NO_MEM;
    }
    return find_free_peb(dev, page, out_pnum, has_ec_hdr);
}

esp_err_t nand_ubi_create_volume(nand_ubi_device_t *ubi_dev, const char *name,
                                 uint8_t vol_type, uint32_t leb_count, uint32_t *out_vol_id)
{
    if (ubi_dev == NULL || out_vol_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_vol_id = UINT32_MAX;
    if (leb_count == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (vol_type != UBI_VID_DYNAMIC && vol_type != UBI_VID_STATIC) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t name_len = name == NULL ? 0 : strlen(name);
    if (name_len > UBI_VOL_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ubi_dev->read_only) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(ubi_dev->lock, portMAX_DELAY);
    uint32_t next_vol_id = ubi_dev->vol_count == 0 ? 0 : ubi_dev->volumes[ubi_dev->vol_count - 1].vol_id + 1u;
    uint32_t used_lebs = ubi_dev->leb_count;
    uint32_t usable = usable_pebs(ubi_dev);
    if (next_vol_id >= ubi_dev->vtbl_slots || leb_count > usable ||
            used_lebs > usable - leb_count ||
            ubi_dev->reserved_pebs > usable - used_lebs - leb_count) {
        xSemaphoreGive(ubi_dev->lock);
        return ESP_ERR_NO_MEM;
    }

    /* next_vol_id is past every volume, so a record in its slot (if any) is one the
     * loader skipped (e.g. reserved_pebs == 0); replace it, else append. */
    nand_ubi_volume_info_t *grown_vols = realloc(ubi_dev->volumes, (ubi_dev->vol_count + 1) * sizeof(*grown_vols));
    if (grown_vols == NULL) {
        xSemaphoreGive(ubi_dev->lock);
        return ESP_ERR_NO_MEM;
    }
    ubi_dev->volumes = grown_vols;
    if (nand_ubi_eba_grow(&ubi_dev->eba, used_lebs + leb_count) != ESP_OK) {
        xSemaphoreGive(ubi_dev->lock);
        return ESP_ERR_NO_MEM;
    }
    uint32_t entry_idx = ubi_dev->vtbl_entry_count;
    for (uint32_t i = 0; i < ubi_dev->vtbl_entry_count; i++) {
        if (ubi_dev->vtbl_entries[i].slot == next_vol_id) {
            entry_idx = i;
        }
    }
    bool appended = entry_idx == ubi_dev->vtbl_entry_count;
    if (appended) {
        vtbl_list_t list = { ubi_dev->vtbl_entries, ubi_dev->vtbl_entry_count };
        nand_ubi_vtbl_record_t blank_rec;
        memset(&blank_rec, 0xFF, sizeof(blank_rec));
        if (vtbl_list_push(&list, next_vol_id, &blank_rec) != ESP_OK) {
            xSemaphoreGive(ubi_dev->lock);
            return ESP_ERR_NO_MEM;
        }
        ubi_dev->vtbl_entries = list.entries;
        ubi_dev->vtbl_entry_count = list.count;
    }
    nand_ubi_vtbl_record_t old_record = ubi_dev->vtbl_entries[entry_idx].rec;
    nand_ubi_vtbl_record_t *record = &ubi_dev->vtbl_entries[entry_idx].rec;
    memset(record, 0, sizeof(*record));
    record->reserved_pebs = nand_ubi_be32(leb_count);
    record->alignment = nand_ubi_be32(1);
    record->vol_type = vol_type;
    record->name_len = nand_ubi_be16((uint16_t)name_len);
    if (name_len > 0) {
        memcpy(record->name, name, name_len);
    }
    record->crc = nand_ubi_be32(nand_ubi_crc32(record, UBI_VTBL_RECORD_SIZE_CRC));

    /* Rewrite a copy that is not good first: erasing the only good copy before the
     * other one holds the new table would leave a power-cut window with no valid copy. */
    uint32_t first_copy = ubi_dev->vtbl_copy_good[0] && !ubi_dev->vtbl_copy_good[1] ? 1u : 0u;
    esp_err_t ret = write_vtbl_copy(ubi_dev, first_copy);
    if (ret == ESP_OK) {
        ret = write_vtbl_copy(ubi_dev, first_copy ^ 1u);
    }
    if (ret != ESP_OK) {
        /* Whatever was written holds the rolled-back record, so it no longer matches. */
        ubi_dev->vtbl_copy_good[0] = false;
        ubi_dev->vtbl_copy_good[1] = false;
        if (appended) {
            ubi_dev->vtbl_entry_count--;
        } else {
            *record = old_record;
        }
        xSemaphoreGive(ubi_dev->lock);
        return ret;
    }

    ubi_dev->volumes[ubi_dev->vol_count++] = (nand_ubi_volume_info_t) {
        .vol_id = next_vol_id,
        .leb_count = leb_count,
        .eba_offset = used_lebs,
        .vol_type = vol_type,
    };
    ubi_dev->leb_count += leb_count;
    *out_vol_id = next_vol_id;
    if (ubi_dev->ckpt) {
        /* Attach rejects a checkpoint whose LEB count differs from the volume table. */
        uint8_t *page = ubi_alloc(ubi_dev->page_size);
        if (page != NULL) {
            (void)ckpt_write(ubi_dev, page);
            free(page);
        }
    }
    xSemaphoreGive(ubi_dev->lock);
    return ESP_OK;
}

/* Verifies a copy_flag PEB's LEB data against vid_hdr->data_crc. Reads in page_size
 * multiples (nand_bdl->ops->read() rejects a partial-page tail on a multi-page span)
 * and hashes only the first data_size bytes, matching how data_crc was computed. */
static bool verify_copy_data(nand_ubi_device_t *dev, uint32_t pnum,
                             uint32_t data_offset, const nand_ubi_vid_hdr_t *vid_hdr)
{
    uint32_t peb_size = dev->peb_size;
    uint32_t page_size = dev->page_size;
    uint32_t data_size = nand_ubi_be32(vid_hdr->data_size);
    uint32_t data_crc = nand_ubi_be32(vid_hdr->data_crc);
    uint32_t leb_size = peb_size - data_offset;

    if (data_size == 0 || data_size > leb_size) {
        ESP_LOGW(TAG, "pnum=%" PRIu32 ": copy_flag data_size %" PRIu32 " out of range", pnum, data_size);
        return false;
    }

    uint32_t read_len = ((data_size + page_size - 1) / page_size) * page_size;
    if (read_len > leb_size) {
        read_len = leb_size;
    }

    uint8_t *buf = ubi_alloc(read_len);
    if (!buf) {
        ESP_LOGE(TAG, "pnum=%" PRIu32 ": no memory (%" PRIu32 " B) to verify copy_flag data", pnum, read_len);
        return false;
    }

    esp_err_t ret = dev->nand_bdl->ops->read(dev->nand_bdl, buf, read_len,
                                             (uint64_t)pnum * peb_size + data_offset, read_len);
    bool ok = false;
    if (ret == ESP_OK) {
        ok = (nand_ubi_crc32(buf, data_size) == data_crc);
    } else {
        ESP_LOGE(TAG, "pnum=%" PRIu32 ": LEB data read failed for copy_flag verification: 0x%x", pnum, ret);
    }
    free(buf);
    return ok;
}

typedef struct {
    uint32_t image_seq;
    bool     have_image_seq;
    uint64_t ec_sum;
    uint32_t ec_seen;
    uint32_t unknown_vol_pebs; /**< PEBs of volumes missing from the volume table */
} scan_ctx_t;

/* Attaches one PEB from its headers: sets its state and, for a data PEB, its LEB mapping.
 * The PEB's state must be FREE and no LEB may map to it on entry. Two PEBs claiming one
 * LEB are resolved by sqnum; a mapped PEB whose VID header is no longer valid loses.
 * Returns an error only for a failed read or ioctl. */
static esp_err_t scan_peb(nand_ubi_device_t *dev, uint32_t pnum, uint8_t *page_buf, scan_ctx_t *ctx)
{
    esp_blockdev_handle_t nand_bdl = dev->nand_bdl;
    uint32_t page_size = dev->page_size;
    uint32_t peb_size = dev->peb_size;

    esp_blockdev_cmd_arg_status_t bad_arg = { .num = pnum, .status = false };
    esp_err_t ret = nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &bad_arg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IS_BAD_BLOCK ioctl failed for pnum=%" PRIu32 ": 0x%x", pnum, ret);
        return ret;
    }
    if (bad_arg.status) {
        nand_ubi_eba_peb_set_bad(&dev->eba, pnum);
        return ESP_OK;
    }
    ret = nand_bdl->ops->read(nand_bdl, page_buf, page_size, (uint64_t)pnum * peb_size, page_size);
    if (ret == ESP_OK && ((int32_t)pnum == dev->vtbl_pnum[0] || (int32_t)pnum == dev->vtbl_pnum[1])) {
        nand_ubi_eba_peb_set_used(&dev->eba, pnum);
        if (nand_ubi_ec_hdr_valid((const nand_ubi_ec_hdr_t *)page_buf)) {
            ctx->ec_sum += nand_ubi_be64(((const nand_ubi_ec_hdr_t *)page_buf)->ec);
            ctx->ec_seen++;
        }
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "EC header read failed for pnum=%" PRIu32 ": 0x%x", pnum, ret);
        return ret;
    }
    if (page_is_blank(page_buf, page_size)) {
        return ESP_OK; /* FREE */
    }

    const nand_ubi_ec_hdr_t *ec_hdr = (const nand_ubi_ec_hdr_t *)page_buf;
    if (!nand_ubi_ec_hdr_valid(ec_hdr)) {
        ESP_LOGW(TAG, "pnum=%" PRIu32 ": corrupt EC header, scheduling erase", pnum);
        nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
        return ESP_OK;
    }
    ctx->ec_sum += nand_ubi_be64(ec_hdr->ec);
    ctx->ec_seen++;

    uint32_t peb_image_seq = nand_ubi_be32(ec_hdr->image_seq);
    if (!ctx->have_image_seq) {
        ctx->image_seq = peb_image_seq;
        ctx->have_image_seq = true;
    } else if (peb_image_seq != ctx->image_seq) {
        ESP_LOGW(TAG, "pnum=%" PRIu32 ": image_seq 0x%08" PRIx32 " != 0x%08" PRIx32 ", treating as stale",
                 pnum, peb_image_seq, ctx->image_seq);
        nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
        return ESP_OK;
    }

    uint32_t vid_hdr_offset = nand_ubi_be32(ec_hdr->vid_hdr_offset);
    uint32_t data_offset = nand_ubi_be32(ec_hdr->data_offset);
    if (vid_hdr_offset == 0 || vid_hdr_offset % page_size != 0 ||
            data_offset % page_size != 0 || data_offset < vid_hdr_offset + page_size ||
            data_offset >= peb_size) {
        ESP_LOGW(TAG, "pnum=%" PRIu32 ": bad header offsets vid=%" PRIu32 " data=%" PRIu32
                 ", scheduling erase", pnum, vid_hdr_offset, data_offset);
        nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
        return ESP_OK;
    }
    if (vid_hdr_offset != dev->vid_hdr_offset || data_offset != dev->data_offset) {
        ESP_LOGW(TAG, "pnum=%" PRIu32 ": header layout differs from volume table, scheduling erase", pnum);
        nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
        return ESP_OK;
    }

    ret = nand_bdl->ops->read(nand_bdl, page_buf, page_size,
                              (uint64_t)pnum * peb_size + vid_hdr_offset, page_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VID header read failed for pnum=%" PRIu32 ": 0x%x", pnum, ret);
        return ret;
    }
    if (page_is_blank(page_buf, page_size)) {
        /* Valid EC header and blank VID header: a free PEB, stamped after its last erase.
         * Allocation reads the EC header before writing, so it stays FREE. */
        return ESP_OK;
    }

    /* Copied out: page_buf is reused below to read a competing PEB's VID header. */
    nand_ubi_vid_hdr_t vid_copy = *(const nand_ubi_vid_hdr_t *)page_buf;
    const nand_ubi_vid_hdr_t *vid_hdr = &vid_copy;
    if (!nand_ubi_vid_hdr_valid(vid_hdr)) {
        ESP_LOGW(TAG, "pnum=%" PRIu32 ": corrupt VID header, scheduling erase", pnum);
        nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
        return ESP_OK;
    }

    uint32_t vol_id = nand_ubi_be32(vid_hdr->vol_id);
    const nand_ubi_volume_info_t *volume = find_volume(dev, vol_id);
    if (volume == NULL) {
        ESP_LOGW(TAG, "pnum=%" PRIu32 ": unknown vol_id=%" PRIu32 ", scheduling erase", pnum, vol_id);
        nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
        ctx->unknown_vol_pebs += vol_id != UBI_LAYOUT_VOL_ID && vol_id != UBI_CKPT_VOL_ID;
        return ESP_OK;
    }

    uint32_t lnum = nand_ubi_be32(vid_hdr->lnum);
    uint64_t sqnum = nand_ubi_be64(vid_hdr->sqnum);
    if (lnum >= volume->leb_count) {
        ESP_LOGW(TAG, "pnum=%" PRIu32 ": vol_id=%" PRIu32 " lnum=%" PRIu32
                 " out of range, scheduling erase", pnum, vol_id, lnum);
        nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
        return ESP_OK;
    }
    uint32_t eba_index = volume->eba_offset + lnum;

    if (sqnum > dev->global_sqnum) {
        dev->global_sqnum = sqnum;
    }

    int32_t existing_pnum = nand_ubi_eba_get_pnum(&dev->eba, eba_index);
    if (existing_pnum == UBI_LEB_UNMAPPED) {
        if (vid_hdr->copy_flag && !verify_copy_data(dev, pnum, dev->data_offset, vid_hdr)) {
            ESP_LOGW(TAG, "pnum=%" PRIu32 " lnum=%" PRIu32 ": copy_flag data_crc mismatch, scheduling erase",
                     pnum, lnum);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            return ESP_OK;
        }
        nand_ubi_eba_set(&dev->eba, eba_index, (int32_t)pnum);
        nand_ubi_eba_peb_set_used(&dev->eba, pnum);
        return ESP_OK;
    }

    /* Two PEBs claim this LEB (left behind by a power cut). Re-read the mapped one's
     * sqnum rather than keeping a per-LEB sqnum table for this rare case. */
    uint32_t old_pnum = (uint32_t)existing_pnum;
    ret = nand_bdl->ops->read(nand_bdl, page_buf, page_size,
                              (uint64_t)old_pnum * peb_size + dev->vid_hdr_offset, page_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VID header re-read failed for pnum=%" PRIu32 ": 0x%x", old_pnum, ret);
        return ret;
    }
    const nand_ubi_vid_hdr_t *old_vid = (const nand_ubi_vid_hdr_t *)page_buf;
    bool old_valid = nand_ubi_vid_hdr_valid(old_vid);
    uint64_t old_sqnum = old_valid ? nand_ubi_be64(old_vid->sqnum) : 0;
    if (!old_valid || sqnum > old_sqnum) {
        if (vid_hdr->copy_flag && !verify_copy_data(dev, pnum, dev->data_offset, vid_hdr)) {
            ESP_LOGW(TAG, "pnum=%" PRIu32 " lnum=%" PRIu32 ": copy_flag data_crc mismatch, keeping pnum %" PRIu32,
                     pnum, lnum, old_pnum);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
        } else {
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, old_pnum);
            nand_ubi_eba_set(&dev->eba, eba_index, (int32_t)pnum);
            nand_ubi_eba_peb_set_used(&dev->eba, pnum);
        }
    } else {
        ESP_LOGW(TAG, "pnum=%" PRIu32 ": stale duplicate of lnum=%" PRIu32 ", scheduling erase", pnum, lnum);
        nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
    }
    return ESP_OK;
}

/**************************************************************************************
 * Attach checkpoint: reading.
 **************************************************************************************
 */

static void ckpt_reset_tables(nand_ubi_device_t *dev)
{
    memset(dev->eba.peb_state, 0, ckpt_state_bytes(dev));
    for (uint32_t i = 0; i < dev->eba.leb_count; i++) {
        dev->eba.eba[i] = UBI_EBA_UNMAPPED_ENTRY;
    }
    nand_ubi_eba_recount_free(&dev->eba);
    dev->pool_count = 0;
    dev->prev_count = 0;
}

/* Stores payload byte o into pool[], peb_state[] or eba[]; the header is checked separately. */
static void ckpt_consume(nand_ubi_device_t *dev, uint32_t pool_count, uint32_t prev_count, uint32_t o, uint8_t b)
{
    if (o < CKPT_HDR_SIZE) {
        return;
    }
    o -= CKPT_HDR_SIZE;
    if (o < 2u * pool_count) {
        uint16_t *v = &dev->pool[o / 2u];
        *v = o % 2u ? (uint16_t)(*v | b) : (uint16_t)(b << 8);
        return;
    }
    o -= 2u * pool_count;
    if (o < 2u * prev_count) {
        uint16_t *v = &dev->prev_pool[o / 2u];
        *v = o % 2u ? (uint16_t)(*v | b) : (uint16_t)(b << 8);
        return;
    }
    o -= 2u * prev_count;
    if (o < ckpt_state_bytes(dev)) {
        dev->eba.peb_state[o] = b;
        return;
    }
    o -= ckpt_state_bytes(dev);
    uint16_t *v = &dev->eba.eba[o / 2u];
    *v = o % 2u ? (uint16_t)(*v | b) : (uint16_t)(b << 8);
}

static bool ckpt_header_ok(const nand_ubi_device_t *dev, const ckpt_cand_t *cand, const nand_ubi_ckpt_hdr_t *hdr)
{
    uint32_t pool_count = nand_ubi_be32(hdr->pool_count);
    uint32_t prev_count = nand_ubi_be32(hdr->prev_count);
    return nand_ubi_be32(hdr->magic) == UBI_CKPT_MAGIC && nand_ubi_be32(hdr->version) == UBI_CKPT_VERSION &&
           nand_ubi_be32(hdr->image_seq) == dev->image_seq && nand_ubi_be32(hdr->peb_count) == dev->peb_count &&
           nand_ubi_be32(hdr->leb_count) == dev->eba.leb_count &&
           nand_ubi_be32(hdr->region_pebs) == dev->ckpt_region &&
           nand_ubi_be32(hdr->pool_max) == dev->ckpt_pool_max && pool_count <= dev->ckpt_pool_max &&
           prev_count <= dev->ckpt_pool_max && nand_ubi_be32(hdr->pnum) == (uint32_t)cand->pnum &&
           cand->data_size == ckpt_size(dev, pool_count, prev_count, dev->eba.leb_count) &&
           cand->data_size <= dev->leb_size;
}

/* Every mapped PEB is a USED PEB; every pool PEB is a FREE PEB outside the region. */
static bool ckpt_tables_ok(const nand_ubi_device_t *dev)
{
    for (uint32_t i = 0; i < dev->eba.leb_count; i++) {
        uint16_t v = dev->eba.eba[i];
        if (v != UBI_EBA_UNMAPPED_ENTRY &&
                (v >= dev->peb_count || nand_ubi_eba_peb_get_state(&dev->eba, v) != UBI_PEB_USED)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < dev->pool_count; i++) {
        uint16_t v = dev->pool[i];
        if (v < dev->ckpt_region || v >= dev->peb_count || !nand_ubi_eba_peb_is_free(&dev->eba, v)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < dev->prev_count; i++) {
        if (dev->prev_pool[i] >= dev->peb_count) {
            return false;
        }
    }
    return true;
}

/* Loads the checkpoint into the RAM tables a page at a time. On any mismatch the tables
 * are reset and false is returned, and the caller does a full scan. */
static bool ckpt_load(nand_ubi_device_t *dev, const ckpt_cand_t *cand, uint8_t *page)
{
    uint64_t base = (uint64_t)cand->pnum * dev->peb_size + dev->data_offset;
    if (dev->nand_bdl->ops->read(dev->nand_bdl, page, dev->page_size, base, dev->page_size) != ESP_OK ||
            !ckpt_header_ok(dev, cand, (const nand_ubi_ckpt_hdr_t *)page)) {
        ESP_LOGW(TAG, "checkpoint on pnum=%" PRIi32 " rejected, doing a full scan", cand->pnum);
        return false;
    }
    const nand_ubi_ckpt_hdr_t *hdr = (const nand_ubi_ckpt_hdr_t *)page;
    uint32_t pool_count = nand_ubi_be32(hdr->pool_count);
    uint32_t prev_count = nand_ubi_be32(hdr->prev_count);
    uint64_t ec_sum = nand_ubi_be64(hdr->ec_sum);
    uint32_t ec_count = nand_ubi_be32(hdr->ec_count);
    uint32_t total = cand->data_size;
    uint32_t crc = UBI_CRC32_START;
    for (uint32_t off = 0; off < total; off += dev->page_size) {
        if (off > 0 && dev->nand_bdl->ops->read(dev->nand_bdl, page, dev->page_size, base + off,
                                                dev->page_size) != ESP_OK) {
            crc = ~cand->data_crc;
            break;
        }
        uint32_t chunk = total - off < dev->page_size ? total - off : dev->page_size;
        crc = nand_ubi_crc32_update(crc, page, chunk);
        for (uint32_t i = 0; i < chunk; i++) {
            ckpt_consume(dev, pool_count, prev_count, off + i, page[i]);
        }
    }
    dev->pool_count = pool_count;
    dev->prev_count = prev_count;
    nand_ubi_eba_recount_free(&dev->eba);
    if (crc != cand->data_crc || !ckpt_tables_ok(dev)) {
        ESP_LOGW(TAG, "checkpoint on pnum=%" PRIi32 " is corrupt, doing a full scan", cand->pnum);
        ckpt_reset_tables(dev);
        return false;
    }
    dev->ec_sum = ec_sum;
    dev->ec_count = ec_count;
    dev->mean_ec = ec_count ? ec_sum / ec_count : 0;
    return true;
}

/* After ckpt_load(): rescans the PEBs that may have changed since the checkpoint was
 * written, i.e. the region and the pool. */
static esp_err_t ckpt_rescan(nand_ubi_device_t *dev, const ckpt_cand_t *cand, uint8_t *page, scan_ctx_t *scan)
{
    for (uint32_t i = 0; i < dev->eba.leb_count; i++) {
        if (dev->eba.eba[i] < dev->ckpt_region) {
            dev->eba.eba[i] = UBI_EBA_UNMAPPED_ENTRY;
        }
    }
    for (uint32_t pnum = 0; pnum < dev->ckpt_region; pnum++) {
        nand_ubi_eba_peb_set_free(&dev->eba, pnum);
    }
    for (uint32_t pnum = 0; pnum < dev->ckpt_region; pnum++) {
        if ((int32_t)pnum == cand->pnum) {
            nand_ubi_eba_peb_set_used(&dev->eba, pnum);
            continue;
        }
        esp_err_t ret = scan_peb(dev, pnum, page, scan);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    for (uint32_t i = 0; i < dev->pool_count; i++) {
        esp_err_t ret = scan_peb(dev, dev->pool[i], page, scan);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

/* A checkpoint lists the PEBs whose erase was deferred, and its writer erases them right
 * after writing it. Those already erased (valid EC header, blank VID header) are only
 * marked FREE, so a reattach does not erase them again. A PEB whose erase was cut short
 * has an invalid EC header or still its old VID header, and stays pending. */
static esp_err_t ckpt_skip_done_erases(nand_ubi_device_t *dev, uint8_t *page)
{
    for (uint32_t pnum = 0; pnum < dev->peb_count; pnum++) {
        if (nand_ubi_eba_peb_get_state(&dev->eba, pnum) != UBI_PEB_ERASE_PENDING) {
            continue;
        }
        uint64_t base = (uint64_t)pnum * dev->peb_size;
        esp_err_t ret = dev->nand_bdl->ops->read(dev->nand_bdl, page, dev->page_size, base, dev->page_size);
        if (ret != ESP_OK) {
            return ret;
        }
        if (!nand_ubi_ec_hdr_valid((const nand_ubi_ec_hdr_t *)page)) {
            continue;
        }
        ret = dev->nand_bdl->ops->read(dev->nand_bdl, page, dev->page_size, base + dev->vid_hdr_offset, dev->page_size);
        if (ret != ESP_OK) {
            return ret;
        }
        if (page_is_blank(page, dev->page_size)) {
            nand_ubi_eba_peb_set_free(&dev->eba, pnum);
        }
    }
    return ESP_OK;
}

static uint32_t isqrt_ceil(uint32_t n)
{
    uint32_t r = 0;
    while (r * r < n) {
        r++;
    }
    return r;
}

esp_err_t nand_ubi_attach(esp_blockdev_handle_t nand_bdl,
                          const nand_ubi_config_t *config,
                          nand_ubi_device_t **out_ubi_dev)
{
    if (nand_bdl == NULL || out_ubi_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_ubi_dev = NULL;
    nand_ubi_config_t effective_config = config ? *config : (nand_ubi_config_t) NAND_UBI_CONFIG_DEFAULT();

    uint32_t page_size = (uint32_t)nand_bdl->geometry.read_size;
    uint32_t peb_size = (uint32_t)nand_bdl->geometry.erase_size;
    if (page_size == 0 || peb_size == 0 || peb_size % page_size != 0) {
        ESP_LOGE(TAG, "invalid geometry: read_size=%" PRIu32 " erase_size=%" PRIu32, page_size, peb_size);
        return ESP_ERR_INVALID_ARG;
    }
    if (nand_bdl->geometry.disk_size % peb_size != 0) {
        ESP_LOGW(TAG, "disk_size 0x%016" PRIx64 " is not a multiple of erase_size %" PRIu32,
                 nand_bdl->geometry.disk_size, peb_size);
    }
    uint32_t peb_count = (uint32_t)(nand_bdl->geometry.disk_size / peb_size);
    if (peb_count == 0) {
        ESP_LOGE(TAG, "disk_size 0x%016" PRIx64 " yields zero PEBs", nand_bdl->geometry.disk_size);
        return ESP_ERR_INVALID_ARG;
    }

    nand_ubi_device_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }
    /* Populated now (rather than after the scan loop) so verify_copy_data() below
     * can read them from dev instead of re-deriving from nand_bdl->geometry. */
    dev->nand_bdl = nand_bdl;
    dev->peb_count = peb_count;
    dev->peb_size = peb_size;
    dev->page_size = page_size;
    dev->vid_hdr_offset = page_size;
    dev->data_offset = 2u * page_size;
    if (dev->data_offset >= peb_size) {
        free(dev);
        return ESP_ERR_INVALID_ARG;
    }
    dev->leb_size = peb_size - dev->data_offset;
    dev->reserved_pebs = effective_config.reserved_pebs;
    dev->read_only = effective_config.read_only;
    dev->vtbl_slots = dev->leb_size / sizeof(nand_ubi_vtbl_record_t);
    if (dev->vtbl_slots > UBI_MAX_VOLUMES) {
        dev->vtbl_slots = UBI_MAX_VOLUMES;
    }
    if (peb_count < UBI_LAYOUT_VOLUME_EBS || dev->vtbl_slots == 0) {
        free(dev);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *page_buf = NULL;

    if (peb_count > UBI_EBA_MAX_PEBS) {
        ESP_LOGE(TAG, "peb_count=%" PRIu32 " exceeds the %u PEBs the EBA table can address",
                 peb_count, UBI_EBA_MAX_PEBS);
        free(dev);
        return ESP_ERR_NOT_SUPPORTED;
    }

    dev->ckpt_pnum = -1;
    if (effective_config.checkpoint) {
        uint32_t region = effective_config.ckpt_region_pebs ? effective_config.ckpt_region_pebs
                          : isqrt_ceil(peb_count);
        region = region < 8u ? 8u : region;
        uint32_t pool = effective_config.ckpt_pool_pebs ? effective_config.ckpt_pool_pebs : region;
        if (region > peb_count / 4u || pool > 1024u) {
            ESP_LOGW(TAG, "chip too small for a checkpoint region of %" PRIu32 " PEBs", region);
        } else {
            dev->ckpt = true;
            dev->ckpt_region = region;
            dev->ckpt_pool_max = pool;
        }
    }

    for (uint32_t copy = 0; copy < UBI_LAYOUT_VOLUME_EBS; copy++) {
        dev->vtbl_pnum[copy] = -1;
    }
    ckpt_cand_t cand;
    esp_err_t ret = locate_layout_pebs(dev, &cand);
    if (ret != ESP_OK) {
        goto fail;
    }

    bool vtbl_found = false;
    ret = load_volume_table(dev, &vtbl_found);
    if (ret != ESP_OK) {
        goto fail;
    }

    /* The volume table is loaded, so eba[] is sized to exactly the LEBs the volumes
     * reserve; nand_ubi_create_volume() grows it. */
    uint32_t reserved_lebs = 0;
    for (uint32_t i = 0; i < dev->vol_count; i++) {
        reserved_lebs += dev->volumes[i].leb_count;
    }
    ret = nand_ubi_eba_alloc(peb_count, reserved_lebs, &dev->eba);
    if (ret != ESP_OK) {
        goto fail;
    }
    nand_ubi_eba_seed(&dev->eba, esp_random());

    page_buf = ubi_alloc(page_size);
    if (dev->ckpt) {
        dev->pool = ubi_alloc(2u * dev->ckpt_pool_max * sizeof(uint16_t));
        dev->prev_pool = dev->pool + dev->ckpt_pool_max;
    }
    if (!page_buf || (dev->ckpt && !dev->pool)) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* The volume table's image_seq is authoritative; without one, the first data PEB's is used. */
    scan_ctx_t scan = { .image_seq = dev->image_seq, .have_image_seq = vtbl_found };
    bool from_ckpt = dev->ckpt && cand.pnum >= 0 && ckpt_load(dev, &cand, page_buf);
    if (from_ckpt) {
        dev->ckpt_pnum = cand.pnum;
        ret = ckpt_rescan(dev, &cand, page_buf, &scan);
        if (ret == ESP_OK) {
            ret = ckpt_skip_done_erases(dev, page_buf);
        }
        if (ret != ESP_OK) {
            goto fail;
        }
    }
    for (uint32_t pnum = 0; !from_ckpt && pnum < peb_count; pnum++) {
        /* The scan is a long synchronous run of SPI transactions on the calling task;
         * yield so the idle task can feed the interrupt watchdog. */
        if ((pnum & 0x1FU) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        ret = scan_peb(dev, pnum, page_buf, &scan);
        if (ret != ESP_OK) {
            goto fail;
        }
    }

    dev->image_seq = scan.image_seq;
    if (!from_ckpt) {
        dev->ec_sum = scan.ec_sum;
        dev->ec_count = scan.ec_seen;
        dev->mean_ec = scan.ec_seen ? scan.ec_sum / scan.ec_seen : 0;
    }

    /* Data PEBs but no readable volume table: draining below would erase every one of them
     * as "unknown vol_id". Refuse, as Linux UBI does, rather than destroy the data. */
    if (!vtbl_found && scan.unknown_vol_pebs > 0 && !dev->read_only) {
        ESP_LOGE(TAG, "%" PRIu32 " PEBs hold UBI data but no valid volume table was found", scan.unknown_vol_pebs);
        ret = ESP_ERR_INVALID_STATE;
        goto fail;
    }

    /* Erase every PEB the scan marked ERASE_PENDING (corrupt headers, stale duplicates,
     * failed copies, foreign volumes/images) so it rejoins the free pool. Without this the
     * free pool shrinks after every power cut that leaves such a PEB behind. */
    if (!dev->read_only && from_ckpt) {
        /* Finishes the erases the checkpoint's writer may not have completed, in its order. */
        ckpt_reclaim_pending(dev, page_buf, dev->prev_pool, dev->prev_count);
    } else if (!dev->read_only) {
        for (uint32_t pnum = 0; pnum < peb_count; pnum++) {
            if (nand_ubi_eba_peb_get_state(&dev->eba, pnum) != UBI_PEB_ERASE_PENDING) {
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            (void)reclaim_peb(dev, pnum, page_buf);
        }
    }

    /* Bring a missing, damaged, or stale volume-table copy back in line with the one
     * selected, so the next create_volume() starts with two good copies. */
    if (!dev->read_only && vtbl_found) {
        for (uint32_t copy = 0; copy < UBI_LAYOUT_VOLUME_EBS; copy++) {
            if (!dev->vtbl_copy_good[copy]) {
                ESP_LOGW(TAG, "repairing volume-table copy %" PRIu32, copy);
                (void)write_vtbl_copy(dev, copy);
            }
        }
    }

    if (dev->ckpt && !dev->read_only && !from_ckpt) {
        (void)ckpt_write(dev, page_buf);
    }

    free(page_buf);
    page_buf = NULL;
    dev->leb_count = 0;
    for (uint32_t i = 0; i < dev->vol_count; i++) {
        dev->leb_count += dev->volumes[i].leb_count;
    }
    dev->leb_capacity = peb_count - UBI_LAYOUT_VOLUME_EBS;
    if (dev->leb_count > usable_pebs(dev)) {
        ESP_LOGW(TAG, "volumes reserve %" PRIu32 " LEBs but only %" PRIu32
                 " good PEBs are left; writes may fail with ESP_ERR_NO_MEM",
                 dev->leb_count, usable_pebs(dev));
    }

    dev->lock = xSemaphoreCreateMutex();
    if (dev->lock == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    ESP_LOGI(TAG, "attached: peb_count=%" PRIu32 " vol_count=%" PRIu32 " leb_count=%" PRIu32
             " peb_size=%" PRIu32 " leb_size=%" PRIu32 " image_seq=0x%08" PRIx32,
             dev->peb_count, dev->vol_count, dev->leb_count, dev->peb_size, dev->leb_size, dev->image_seq);

    *out_ubi_dev = dev;
    return ESP_OK;

fail:
    free(page_buf);
    free(dev->pool);
    free(dev->vtbl_entries);
    free(dev->volumes);
    nand_ubi_eba_free(&dev->eba);
    free(dev);
    return ret;
}

esp_err_t nand_ubi_detach(nand_ubi_device_t *ubi_dev)
{
    if (ubi_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(ubi_dev->lock, portMAX_DELAY);
    if (ubi_dev->open_volumes > 0) {
        xSemaphoreGive(ubi_dev->lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* Without this, LEBs erased in this session would map to their old PEBs again. */
    if (ubi_dev->ckpt && ubi_dev->ckpt_dirty && !ubi_dev->read_only) {
        uint8_t *page = ubi_alloc(ubi_dev->page_size);
        if (page != NULL) {
            (void)ckpt_write(ubi_dev, page);
            free(page);
        }
    }
    xSemaphoreGive(ubi_dev->lock);

    if (ubi_dev->lock) {
        vSemaphoreDelete(ubi_dev->lock);
    }
    free(ubi_dev->vtbl_entries);
    free(ubi_dev->volumes);
    free(ubi_dev->pool);
    nand_ubi_eba_free(&ubi_dev->eba);
    free(ubi_dev);
    return ESP_OK;
}

esp_err_t nand_ubi_checkpoint(nand_ubi_device_t *ubi_dev)
{
    if (ubi_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(ubi_dev->lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    if (ubi_dev->ckpt && !ubi_dev->read_only) {
        uint8_t *page = ubi_alloc(ubi_dev->page_size);
        ret = page == NULL ? ESP_ERR_NO_MEM : ckpt_write(ubi_dev, page);
        free(page);
    }
    xSemaphoreGive(ubi_dev->lock);
    return ret;
}

/**************************************************************************************
 * Volume-level: BDL ops vtable for a handle returned by nand_ubi_open_volume().
 **************************************************************************************
 */

/* Allocates a free PEB for lnum, writes a VID header onto it, and records the lnum->pnum
 * mapping. Caller must hold dev->lock.
 *
 * A free PEB with a valid EC header was stamped right after a completed erase, so its
 * header (and erase count) is kept as is. A free PEB without one has unknown history
 * and is erased and stamped first. */
static esp_err_t nand_ubi_vol_alloc_peb(nand_ubi_device_t *dev, uint32_t vol_id,
                                        uint32_t local_lnum, uint32_t eba_index,
                                        uint8_t vol_type, int32_t *out_pnum)
{
    /* A failed header program retires the PEB (a NAND program failure means the block
     * is bad) and the allocation moves on to the next free PEB. Bounded so that a
     * systemic fault (e.g. a dead bus) cannot retire the whole chip. */
    enum { ALLOC_ATTEMPTS = 3 };

    uint8_t *page_buf = ubi_alloc(dev->page_size);
    if (page_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_ERR_NO_MEM;
    for (int attempt = 0; attempt < ALLOC_ATTEMPTS; attempt++) {
        int32_t pnum = -1;
        bool has_ec_hdr = false;
        ret = take_free_peb(dev, page_buf, &pnum, &has_ec_hdr);
        if (ret != ESP_OK) {
            break;
        }
        uint64_t base = (uint64_t)pnum * dev->peb_size;
        dev->ckpt_dirty = true;

        if (!has_ec_hdr) {
            /* No EC header: either factory-fresh, or left by an erase cut part-way,
             * which reads as 0xFF but can lose bits once programmed. Erase it first. */
            ret = erase_and_stamp_ec(dev, (uint32_t)pnum, page_buf);
        }
        if (ret == ESP_OK) {
            memset(page_buf, 0xFF, dev->page_size);
            fill_vid_header(dev, (nand_ubi_vid_hdr_t *)page_buf, vol_id, local_lnum, vol_type);
            ret = dev->nand_bdl->ops->write(dev->nand_bdl, page_buf, base + dev->vid_hdr_offset, dev->page_size);
        }
        if (ret == ESP_OK) {
            nand_ubi_eba_set(&dev->eba, eba_index, pnum);
            nand_ubi_eba_peb_set_used(&dev->eba, (uint32_t)pnum);
            *out_pnum = pnum;
            break;
        }

        ESP_LOGW(TAG, "pnum=%" PRIi32 ": header write failed: 0x%x", pnum, ret);
        esp_err_t retire_ret = retire_peb(dev, (uint32_t)pnum);
        if (retire_ret != ESP_OK) {
            ret = retire_ret;
            break;
        }
    }
    free(page_buf);
    return ret;
}

static esp_err_t nand_ubi_vol_read(esp_blockdev_handle_t handle, uint8_t *dst_buf, size_t dst_buf_size,
                                   uint64_t src_addr, size_t data_read_len)
{
    if (dst_buf == NULL || dst_buf_size < data_read_len) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Split into two comparisons (rather than src_addr + data_read_len > disk_size)
     * to avoid a uint64_t wraparound false-negative if a caller passes src_addr
     * near UINT64_MAX. */
    if (src_addr > handle->geometry.disk_size || data_read_len > handle->geometry.disk_size - src_addr) {
        return ESP_ERR_INVALID_SIZE;
    }

    nand_ubi_vol_ctx_t *vol_ctx = (nand_ubi_vol_ctx_t *)handle->ctx;
    nand_ubi_device_t *dev = vol_ctx->dev;
    uint32_t lnum = (uint32_t)(src_addr / dev->leb_size);
    uint32_t eba_index = vol_ctx->eba_offset + lnum;
    uint32_t offset = (uint32_t)(src_addr % dev->leb_size);
    if (offset + data_read_len > dev->leb_size) {
        /* Crosses into the next LEB, which is not necessarily the next PEB: nand_bdl has
         * no notion of LEB boundaries and would happily read into unrelated data. */
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(dev->lock, portMAX_DELAY);
    int32_t pnum = nand_ubi_eba_get_pnum(&dev->eba, eba_index);
    esp_err_t ret;
    if (pnum == UBI_LEB_UNMAPPED) {
        /* An unmapped LEB has never been written, i.e. it reads exactly like a blank
         * (erased) NAND block would: all 0xFF. Filesystems (LittleFS in particular)
         * probe blocks by reading them before ever writing, and expect the erased-flash
         * convention (0xFF fill, ESP_OK) rather than a hard I/O error, so we must not
         * surface ESP_ERR_NOT_FOUND here -- that used to break the very first mount of
         * a freshly-created volume (LEB 0/1 unmapped => LittleFS treated it as a real
         * read fault instead of "no filesystem here yet"). */
        memset(dst_buf, 0xFF, data_read_len);
        ret = ESP_OK;
    } else {
        uint64_t phys_addr = (uint64_t)pnum * dev->peb_size + dev->data_offset + offset;
        ESP_LOGD(TAG, "vol_read: lnum=%" PRIu32 " pnum=%" PRIi32 " offset=%" PRIu32
                 " len=%zu phys_addr=0x%016" PRIx64, lnum, pnum, offset, data_read_len, phys_addr);
        ret = dev->nand_bdl->ops->read(dev->nand_bdl, dst_buf, dst_buf_size, phys_addr, data_read_len);
    }
    xSemaphoreGive(dev->lock);
    return ret;
}

static esp_err_t nand_ubi_vol_write(esp_blockdev_handle_t handle, const uint8_t *src_buf,
                                    uint64_t dst_addr, size_t data_write_len)
{
    if (handle->device_flags.read_only || ((nand_ubi_vol_ctx_t *)handle->ctx)->dev->read_only) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (src_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dst_addr > handle->geometry.disk_size || data_write_len > handle->geometry.disk_size - dst_addr) {
        return ESP_ERR_INVALID_SIZE;
    }

    nand_ubi_vol_ctx_t *vol_ctx = (nand_ubi_vol_ctx_t *)handle->ctx;
    nand_ubi_device_t *dev = vol_ctx->dev;
    uint32_t lnum = (uint32_t)(dst_addr / dev->leb_size);
    uint32_t eba_index = vol_ctx->eba_offset + lnum;
    uint32_t offset = (uint32_t)(dst_addr % dev->leb_size);
    if (offset + data_write_len > dev->leb_size) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(dev->lock, portMAX_DELAY);
    int32_t pnum = nand_ubi_eba_get_pnum(&dev->eba, eba_index);
    esp_err_t ret = ESP_OK;
    if (pnum == UBI_LEB_UNMAPPED) {
        ret = nand_ubi_vol_alloc_peb(dev, vol_ctx->vol_id, lnum, eba_index, vol_ctx->vol_type, &pnum);
    }
    if (ret == ESP_OK) {
        uint64_t phys_addr = (uint64_t)pnum * dev->peb_size + dev->data_offset + offset;
        ESP_LOGD(TAG, "vol_write: lnum=%" PRIu32 " pnum=%" PRIi32 " offset=%" PRIu32
                 " len=%zu phys_addr=0x%016" PRIx64,
                 lnum, pnum, offset, data_write_len, phys_addr);
        ret = dev->nand_bdl->ops->write(dev->nand_bdl, src_buf, phys_addr, data_write_len);
    }
    xSemaphoreGive(dev->lock);
    return ret;
}

static esp_err_t nand_ubi_vol_erase(esp_blockdev_handle_t handle, uint64_t start_addr, size_t erase_len)
{
    if (handle->device_flags.read_only || ((nand_ubi_vol_ctx_t *)handle->ctx)->dev->read_only) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    nand_ubi_vol_ctx_t *vol_ctx = (nand_ubi_vol_ctx_t *)handle->ctx;
    nand_ubi_device_t *dev = vol_ctx->dev;
    uint32_t leb_size = dev->leb_size;

    if ((start_addr % leb_size) != 0 || erase_len == 0 || (erase_len % leb_size) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (start_addr > handle->geometry.disk_size || erase_len > handle->geometry.disk_size - start_addr) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t start_lnum = (uint32_t)(start_addr / leb_size);
    uint32_t leb_span = (uint32_t)(erase_len / leb_size);

    xSemaphoreTake(dev->lock, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    /* Reused across every PEB in the span rather than allocated per-iteration: erase
     * spans can cover many LEBs (e.g. a full-volume erase), and this buffer is only
     * ever page_size, not peb_size. */
    uint8_t *page_buf = ubi_alloc(dev->page_size);
    if (page_buf == NULL) {
        xSemaphoreGive(dev->lock);
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t lnum = start_lnum; lnum < start_lnum + leb_span; lnum++) {
        /* Same rationale as the attach scan: a long span of synchronous per-PEB erases
         * (each potentially several ms on real SPI NAND) can otherwise starve the idle
         * task long enough to trip the interrupt watchdog. */
        if ((lnum & 0x1FU) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        uint32_t eba_index = vol_ctx->eba_offset + lnum;
        int32_t pnum = nand_ubi_eba_get_pnum(&dev->eba, eba_index);
        if (pnum == UBI_LEB_UNMAPPED) {
            continue; /* already logically erased: idempotent no-op for this LEB */
        }

        /* Unmap first: whatever happens to the PEB below, the LEB's old contents are
         * no longer wanted. reclaim_peb() erases the PEB and gives it a fresh EC header
         * (ec+1), or retires it as bad if the erase or header write fails. A retired PEB
         * is skipped by the next attach, so its stale VID header cannot remap this LEB.
         * The only error left is when marking it bad failed too; that is reported. */
        nand_ubi_eba_set(&dev->eba, eba_index, UBI_LEB_UNMAPPED);
        if (dev->ckpt) {
            /* The newest checkpoint may still map this LEB to pnum: erase after the next one. */
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, (uint32_t)pnum);
            dev->ckpt_dirty = true;
            continue;
        }
        ret = reclaim_peb(dev, (uint32_t)pnum, page_buf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "lnum=%" PRIu32 " pnum=%" PRIi32 ": erase failed and PEB could not be retired: 0x%x",
                     lnum, pnum, ret);
            break;
        }
    }
    free(page_buf);
    xSemaphoreGive(dev->lock);
    return ret;
}

static esp_err_t nand_ubi_vol_sync(esp_blockdev_handle_t handle)
{
    nand_ubi_vol_ctx_t *vol_ctx = (nand_ubi_vol_ctx_t *)handle->ctx;
    esp_blockdev_handle_t nand_bdl = vol_ctx->dev->nand_bdl;
    return nand_bdl->ops->sync(nand_bdl);
}

/* Maps a volume-relative page number to a physical page. Returns ESP_ERR_NOT_FOUND
 * when the page's LEB is unmapped. Caller must hold dev->lock. */
static esp_err_t vol_page_to_phys(const nand_ubi_vol_ctx_t *vol_ctx, uint32_t page, uint32_t *out_phys)
{
    const nand_ubi_device_t *dev = vol_ctx->dev;
    uint32_t pages_per_leb = dev->leb_size / dev->page_size;
    uint32_t lnum = page / pages_per_leb;
    if (lnum >= vol_ctx->leb_count) {
        return ESP_ERR_INVALID_ARG;
    }
    int32_t pnum = nand_ubi_eba_get_pnum(&dev->eba, vol_ctx->eba_offset + lnum);
    if (pnum == UBI_LEB_UNMAPPED) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_phys = (uint32_t)pnum * (dev->peb_size / dev->page_size) +
                dev->data_offset / dev->page_size + page % pages_per_leb;
    return ESP_OK;
}

/* Volume ioctls speak LEB/volume-page numbers. Anything carrying an address is
 * translated or refused; forwarding it raw would act on an unrelated physical PEB. */
static esp_err_t nand_ubi_vol_ioctl(esp_blockdev_handle_t handle, const uint8_t cmd, void *args)
{
    nand_ubi_vol_ctx_t *vol_ctx = (nand_ubi_vol_ctx_t *)handle->ctx;
    nand_ubi_device_t *dev = vol_ctx->dev;
    esp_blockdev_handle_t nand_bdl = dev->nand_bdl;

    switch (cmd) {
    case ESP_BLOCKDEV_CMD_GET_BAD_BLOCKS_COUNT:
    case ESP_BLOCKDEV_CMD_GET_ECC_STATS:
    case ESP_BLOCKDEV_CMD_GET_NAND_FLASH_INFO:
        return nand_bdl->ops->ioctl(nand_bdl, cmd, args);
    case ESP_BLOCKDEV_CMD_ERASE_CONTENTS: {
        const esp_blockdev_cmd_arg_erase_t *arg = args;
        if (arg == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        return nand_ubi_vol_erase(handle, arg->start_addr, arg->erase_len);
    }
    case ESP_BLOCKDEV_CMD_MARK_DELETED: {
        /* Advisory (TRIM): callers such as FatFS pass sector-granular ranges, so only
         * LEBs lying entirely inside the range are unmapped; partial edges are kept. */
        const esp_blockdev_cmd_arg_erase_t *arg = args;
        if (arg == NULL || arg->start_addr > handle->geometry.disk_size ||
                arg->erase_len > handle->geometry.disk_size - arg->start_addr) {
            return ESP_ERR_INVALID_ARG;
        }
        uint64_t leb = dev->leb_size;
        uint64_t first = (arg->start_addr + leb - 1) / leb * leb;
        uint64_t end = (arg->start_addr + arg->erase_len) / leb * leb;
        return end > first ? nand_ubi_vol_erase(handle, first, (size_t)(end - first)) : ESP_OK;
    }
    case ESP_BLOCKDEV_CMD_IS_BAD_BLOCK: {
        esp_blockdev_cmd_arg_status_t *arg = args;
        if (arg == NULL || arg->num >= vol_ctx->leb_count) {
            return ESP_ERR_INVALID_ARG;
        }
        arg->status = false; /* bad PEBs never back a LEB */
        return ESP_OK;
    }
    case ESP_BLOCKDEV_CMD_IS_FREE_PAGE:
    case ESP_BLOCKDEV_CMD_GET_PAGE_ECC_STATUS: {
        if (args == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        bool is_free_cmd = cmd == ESP_BLOCKDEV_CMD_IS_FREE_PAGE;
        uint32_t page = is_free_cmd ? ((esp_blockdev_cmd_arg_status_t *)args)->num
                        : ((esp_blockdev_cmd_arg_ecc_status_t *)args)->page_num;
        uint32_t phys = 0;
        xSemaphoreTake(dev->lock, portMAX_DELAY);
        esp_err_t ret = vol_page_to_phys(vol_ctx, page, &phys);
        if (ret == ESP_OK) {
            if (is_free_cmd) {
                esp_blockdev_cmd_arg_status_t phys_arg = { .num = phys, .status = false };
                ret = nand_bdl->ops->ioctl(nand_bdl, cmd, &phys_arg);
                ((esp_blockdev_cmd_arg_status_t *)args)->status = phys_arg.status;
            } else {
                esp_blockdev_cmd_arg_ecc_status_t phys_arg = { .page_num = phys };
                ret = nand_bdl->ops->ioctl(nand_bdl, cmd, &phys_arg);
                ((esp_blockdev_cmd_arg_ecc_status_t *)args)->ecc_status = phys_arg.ecc_status;
            }
        } else if (ret == ESP_ERR_NOT_FOUND) {
            /* Unmapped LEB: reads as erased flash, so the page is free and ECC-clean. */
            if (is_free_cmd) {
                ((esp_blockdev_cmd_arg_status_t *)args)->status = true;
            } else {
                ((esp_blockdev_cmd_arg_ecc_status_t *)args)->ecc_status = NAND_ECC_OK;
            }
            ret = ESP_OK;
        }
        xSemaphoreGive(dev->lock);
        return ret;
    }
    default:
        /* MARK_BAD_BLOCK (UBI owns bad-block management), COPY_PAGE, and unknown
         * commands whose arguments may hold physical addresses. */
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t nand_ubi_vol_release(esp_blockdev_handle_t handle)
{
    nand_ubi_vol_ctx_t *vol_ctx = (nand_ubi_vol_ctx_t *)handle->ctx;
    nand_ubi_device_t *dev = vol_ctx->dev;
    bool owns_device = vol_ctx->owns_device;

    xSemaphoreTake(dev->lock, portMAX_DELAY);
    dev->open_volumes--;
    xSemaphoreGive(dev->lock);

    free(vol_ctx);
    free(handle);

    if (owns_device) {
        return nand_ubi_detach(dev);
    }
    return ESP_OK;
}

static const esp_blockdev_ops_t s_nand_ubi_vol_ops = {
    .read = nand_ubi_vol_read,
    .write = nand_ubi_vol_write,
    .erase = nand_ubi_vol_erase,
    .sync = nand_ubi_vol_sync,
    .ioctl = nand_ubi_vol_ioctl,
    .release = nand_ubi_vol_release,
};

esp_err_t nand_ubi_open_volume(nand_ubi_device_t *ubi_dev, uint32_t vol_id, esp_blockdev_handle_t *out_vol_bdl)
{
    if (ubi_dev == NULL || out_vol_bdl == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_vol_bdl = NULL;
    const nand_ubi_volume_info_t *volume = find_volume(ubi_dev, vol_id);
    if (volume == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    nand_ubi_vol_ctx_t *vol_ctx = calloc(1, sizeof(*vol_ctx));
    if (vol_ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    vol_ctx->dev = ubi_dev;
    vol_ctx->vol_id = vol_id;
    vol_ctx->leb_count = volume->leb_count;
    vol_ctx->vol_type = volume->vol_type;
    vol_ctx->eba_offset = volume->eba_offset;
    vol_ctx->owns_device = false;

    esp_blockdev_t *vol_bdl = (esp_blockdev_t *)heap_caps_calloc(1, sizeof(esp_blockdev_t), MALLOC_CAP_DEFAULT);
    if (vol_bdl == NULL) {
        free(vol_ctx);
        return ESP_ERR_NO_MEM;
    }
    vol_bdl->ctx = (void *)vol_ctx;
    vol_bdl->ops = &s_nand_ubi_vol_ops;

    /* Physical-media properties (encrypted, erase_before_write, and_type_write,
     * default_val_after_erase) pass through from the raw NAND BDL unchanged;
     * read_only is a UBI-level access mode set at attach() time instead. */
    vol_bdl->device_flags = ubi_dev->nand_bdl->device_flags;
    vol_bdl->device_flags.read_only = ubi_dev->read_only;

    vol_bdl->geometry.disk_size = (uint64_t)vol_ctx->leb_count * ubi_dev->leb_size;
    vol_bdl->geometry.read_size = ubi_dev->nand_bdl->geometry.read_size;
    vol_bdl->geometry.write_size = ubi_dev->nand_bdl->geometry.write_size;
    vol_bdl->geometry.erase_size = ubi_dev->leb_size;
    vol_bdl->geometry.recommended_write_size = vol_bdl->geometry.write_size;
    vol_bdl->geometry.recommended_read_size = vol_bdl->geometry.read_size;
    vol_bdl->geometry.recommended_erase_size = vol_bdl->geometry.erase_size;

    xSemaphoreTake(ubi_dev->lock, portMAX_DELAY);
    ubi_dev->open_volumes++;
    xSemaphoreGive(ubi_dev->lock);

    *out_vol_bdl = vol_bdl;
    return ESP_OK;
}

esp_err_t nand_ubi_get_blockdev(esp_blockdev_handle_t nand_bdl, const nand_ubi_config_t *config,
                                esp_blockdev_handle_t *out_vol_bdl)
{
    if (out_vol_bdl == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_vol_bdl = NULL;

    nand_ubi_device_t *dev = NULL;
    esp_err_t ret = nand_ubi_attach(nand_bdl, config, &dev);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_blockdev_handle_t vol_bdl = NULL;
    uint32_t vol_id = 0;
    if (dev->vol_count == 0) {
        uint32_t usable = usable_pebs(dev);
        uint32_t capacity = usable > dev->reserved_pebs ? usable - dev->reserved_pebs : 0;
        ret = capacity == 0 ? ESP_ERR_NO_MEM :
              nand_ubi_create_volume(dev, NULL, UBI_VID_DYNAMIC, capacity, &vol_id);
    } else {
        /* On a device with several existing volumes, this convenience API opens
         * the lowest ID to preserve its historical single-volume behavior. */
        vol_id = dev->volumes[0].vol_id;
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = nand_ubi_open_volume(dev, vol_id, &vol_bdl);
    }
    if (ret != ESP_OK) {
        /* dev->open_volumes is still 0 here (open_volume() only increments it on
         * success), so nand_ubi_detach() cannot return ESP_ERR_INVALID_STATE and
         * always reaches its free(ubi_dev) path: no leak, despite static analyzers
         * that can't see across this call flagging dev as unreleased. */
        (void)nand_ubi_detach(dev);
        return ret;
    }

    ((nand_ubi_vol_ctx_t *)vol_bdl->ctx)->owns_device = true;

    *out_vol_bdl = vol_bdl;
    return ESP_OK;
}
