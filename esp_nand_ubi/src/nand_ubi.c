/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
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

static bool vtbl_is_valid(const nand_ubi_vtbl_record_t *vtbl, uint32_t slots)
{
    for (uint32_t i = 0; i < slots; i++) {
        const uint8_t *record = (const uint8_t *)&vtbl[i];
        if (!page_is_blank(record, sizeof(vtbl[i])) && !nand_ubi_vtbl_record_valid(&vtbl[i])) {
            return false;
        }
    }
    return true;
}

static esp_err_t read_vtbl_copy(nand_ubi_device_t *dev, uint32_t pnum,
                                 nand_ubi_vtbl_record_t *vtbl, bool *blank, bool *valid,
                                 uint64_t *sqnum)
{
    size_t bytes = (size_t)dev->vtbl_slots * sizeof(*vtbl);
    uint8_t *page = ubi_alloc(dev->page_size);
    if (page == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = dev->nand_bdl->ops->read(dev->nand_bdl, page, dev->page_size,
                                             (uint64_t)pnum * dev->peb_size, dev->page_size);
    if (ret != ESP_OK) {
        free(page);
        return ret;
    }
    if (page_is_blank(page, dev->page_size)) {
        *blank = true;
        *valid = false;
        memset(vtbl, 0xFF, bytes);
        free(page);
        return ESP_OK;
    }

    const nand_ubi_ec_hdr_t *ec = (const nand_ubi_ec_hdr_t *)page;
    if (!nand_ubi_ec_hdr_valid(ec)) {
        *blank = false;
        *valid = false;
        free(page);
        return ESP_OK;
    }
    uint32_t vid_offset = nand_ubi_be32(ec->vid_hdr_offset);
    uint32_t data_offset = nand_ubi_be32(ec->data_offset);
    if (vid_offset != dev->vid_hdr_offset || data_offset != dev->data_offset) {
        *blank = false;
        *valid = false;
        free(page);
        return ESP_OK;
    }

    ret = dev->nand_bdl->ops->read(dev->nand_bdl, page, dev->page_size,
                                    (uint64_t)pnum * dev->peb_size + vid_offset, dev->page_size);
    if (ret != ESP_OK) {
        free(page);
        return ret;
    }
    const nand_ubi_vid_hdr_t *vid = (const nand_ubi_vid_hdr_t *)page;
    if (!nand_ubi_vid_hdr_valid(vid) || nand_ubi_be32(vid->vol_id) != UBI_LAYOUT_VOL_ID ||
            nand_ubi_be32(vid->lnum) != pnum) {
        *blank = false;
        *valid = false;
        free(page);
        return ESP_OK;
    }
    *sqnum = nand_ubi_be64(vid->sqnum);

    uint32_t read_len = (uint32_t)(((bytes + dev->page_size - 1u) / dev->page_size) * dev->page_size);
    ret = dev->nand_bdl->ops->read(dev->nand_bdl, (uint8_t *)vtbl, read_len,
                                    (uint64_t)pnum * dev->peb_size + data_offset, read_len);
    free(page);
    if (ret != ESP_OK) {
        return ret;
    }
    *blank = false;
    *valid = vtbl_is_valid(vtbl, dev->vtbl_slots);
    return ESP_OK;
}

static esp_err_t load_volume_table(nand_ubi_device_t *dev)
{
    size_t bytes = (size_t)dev->vtbl_slots * sizeof(*dev->vtbl);
    uint32_t io_bytes = ((uint32_t)bytes + dev->page_size - 1u) / dev->page_size * dev->page_size;
    nand_ubi_vtbl_record_t *first = ubi_alloc(io_bytes);
    nand_ubi_vtbl_record_t *second = ubi_alloc(io_bytes);
    if (first == NULL || second == NULL) {
        free(first);
        free(second);
        return ESP_ERR_NO_MEM;
    }

    bool first_blank = false;
    bool second_blank = false;
    bool first_valid = false;
    bool second_valid = false;
    uint64_t first_sqnum = 0;
    uint64_t second_sqnum = 0;
    esp_err_t ret = read_vtbl_copy(dev, 0, first, &first_blank, &first_valid, &first_sqnum);
    if (ret == ESP_OK) {
        ret = read_vtbl_copy(dev, 1, second, &second_blank, &second_valid, &second_sqnum);
    }
    if (ret != ESP_OK) {
        free(first);
        free(second);
        return ret;
    }
    if (!first_valid && !second_valid) {
        if (!first_blank || !second_blank) {
            ESP_LOGE(TAG, "both volume-table mirrors are invalid");
        }
        memset(dev->vtbl, 0xFF, bytes);
        free(first);
        free(second);
        return ESP_OK;
    }

    const nand_ubi_vtbl_record_t *selected = first_valid ? first : second;
    if (first_valid != second_valid) {
        ESP_LOGW(TAG, "only volume-table mirror %u is valid", first_valid ? 0u : 1u);
    } else if (memcmp(first, second, bytes) != 0) {
        selected = second_sqnum > first_sqnum ? second : first;
        ESP_LOGW(TAG, "volume-table mirrors disagree; using newer mirror %u",
                 second_sqnum > first_sqnum ? 1u : 0u);
    }
    memcpy(dev->vtbl, selected, bytes);

    uint32_t eba_offset = 0;
    for (uint32_t slot = 0; slot < dev->vtbl_slots; slot++) {
        if (page_is_blank((const uint8_t *)&selected[slot], sizeof(selected[slot])) ||
                nand_ubi_be32(selected[slot].reserved_pebs) == 0) {
            continue;
        }
        uint32_t leb_count = nand_ubi_be32(selected[slot].reserved_pebs);
        if (leb_count == 0 || leb_count > dev->peb_count || eba_offset > dev->peb_count - leb_count) {
            ESP_LOGE(TAG, "vtbl slot %" PRIu32 " has invalid LEB count %" PRIu32, slot, leb_count);
            continue;
        }
        dev->volumes[dev->vol_count++] = (nand_ubi_volume_info_t) {
            .vol_id = slot,
            .leb_count = leb_count,
            .eba_offset = eba_offset,
        };
        eba_offset += leb_count;
    }
    free(first);
    free(second);
    return ESP_OK;
}

static void fill_ec_header(nand_ubi_device_t *dev, nand_ubi_ec_hdr_t *ec_hdr)
{
    memset(ec_hdr, 0, sizeof(*ec_hdr));
    ec_hdr->magic = nand_ubi_be32(UBI_EC_HDR_MAGIC);
    ec_hdr->version = UBI_VERSION;
    ec_hdr->ec = nand_ubi_be64(0);
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

static esp_err_t write_vtbl_copy(nand_ubi_device_t *dev, uint32_t pnum)
{
    uint32_t vtbl_bytes = dev->vtbl_slots * sizeof(*dev->vtbl);
    uint32_t write_len = ((vtbl_bytes + dev->page_size - 1u) / dev->page_size) * dev->page_size;
    uint8_t *page = ubi_alloc(dev->page_size);
    if (page == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = dev->nand_bdl->ops->erase(dev->nand_bdl,
                                               (uint64_t)pnum * dev->peb_size, dev->peb_size);
    if (ret != ESP_OK) {
        free(page);
        return ret;
    }

    memset(page, 0xFF, dev->page_size);
    fill_ec_header(dev, (nand_ubi_ec_hdr_t *)page);
    ret = dev->nand_bdl->ops->write(dev->nand_bdl, page,
                                     (uint64_t)pnum * dev->peb_size, dev->page_size);
    if (ret == ESP_OK) {
        memset(page, 0xFF, dev->page_size);
        fill_vid_header(dev, (nand_ubi_vid_hdr_t *)page, UBI_LAYOUT_VOL_ID, pnum, UBI_VID_DYNAMIC);
        ((nand_ubi_vid_hdr_t *)page)->compat = UBI_COMPAT_REJECT;
        ((nand_ubi_vid_hdr_t *)page)->hdr_crc = nand_ubi_be32(nand_ubi_crc32(page, UBI_VID_HDR_SIZE_CRC));
        ret = dev->nand_bdl->ops->write(dev->nand_bdl, page,
                                        (uint64_t)pnum * dev->peb_size + dev->vid_hdr_offset,
                                        dev->page_size);
    }
    if (ret == ESP_OK) {
        ret = dev->nand_bdl->ops->write(dev->nand_bdl, (const uint8_t *)dev->vtbl,
                                        (uint64_t)pnum * dev->peb_size + dev->data_offset, write_len);
    }
    free(page);
    return ret;
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
    if (next_vol_id >= ubi_dev->vtbl_slots || leb_count > ubi_dev->peb_count ||
            used_lebs > ubi_dev->peb_count - leb_count ||
            used_lebs + leb_count > ubi_dev->peb_count - UBI_LAYOUT_VOLUME_EBS ||
            ubi_dev->reserved_pebs > ubi_dev->peb_count - UBI_LAYOUT_VOLUME_EBS - used_lebs - leb_count) {
        xSemaphoreGive(ubi_dev->lock);
        return ESP_ERR_NO_MEM;
    }

    nand_ubi_vtbl_record_t old_record = ubi_dev->vtbl[next_vol_id];
    nand_ubi_vtbl_record_t *record = &ubi_dev->vtbl[next_vol_id];
    memset(record, 0, sizeof(*record));
    record->reserved_pebs = nand_ubi_be32(leb_count);
    record->alignment = nand_ubi_be32(1);
    record->vol_type = vol_type;
    record->name_len = nand_ubi_be16((uint16_t)name_len);
    if (name_len > 0) {
        memcpy(record->name, name, name_len);
    }
    record->crc = nand_ubi_be32(nand_ubi_crc32(record, UBI_VTBL_RECORD_SIZE_CRC));

    esp_err_t ret = write_vtbl_copy(ubi_dev, 0);
    if (ret == ESP_OK) {
        ret = write_vtbl_copy(ubi_dev, 1);
    }
    if (ret != ESP_OK) {
        *record = old_record;
        xSemaphoreGive(ubi_dev->lock);
        return ret;
    }

    ubi_dev->volumes[ubi_dev->vol_count++] = (nand_ubi_volume_info_t) {
        .vol_id = next_vol_id,
        .leb_count = leb_count,
        .eba_offset = used_lebs,
    };
    ubi_dev->leb_count += leb_count;
    *out_vol_id = next_vol_id;
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

/* lizard flags this function for cyclomatic complexity (43) and length (232 lines).
 * Accepted as-is for now rather than decomposed: this is the single per-PEB scan that
 * builds the initial EBA table, its ~15 early-continue branches are a flat sequence of
 * independent validation steps (bad block / EC header / image_seq / header offsets /
 * VID header / lnum range / sqnum ordering / copy_flag verification) each of which
 * mutates dev->eba directly, and it is exercised by 10 dedicated test cases in
 * host_test/main/test_nand_ubi_attach.cpp, including the interrupted-allocation
 * recovery case fixed after an earlier review round. Splitting it into helper
 * functions now - while attach() semantics are still being hardened - would mean
 * threading peb_count/page_size/peb_size/dev->eba plus several loop-local flags
 * across new call boundaries purely to satisfy the linter, at real risk of
 * reintroducing the kind of subtle state-classification bug this function has
 * already had. Revisit once Phase 1's on-flash format and recovery rules settle. */
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

    uint64_t *sqnum_seen = NULL;
    uint8_t *page_buf = NULL;

    /* leb_count is unknown until the scan below finds max_lnum, so eba[] is
     * over-allocated to peb_count (leb_count <= peb_count always). */
    esp_err_t ret = nand_ubi_eba_alloc(peb_count, peb_count, &dev->eba);
    if (ret != ESP_OK) {
        free(dev);
        return ret;
    }

    uint32_t vtbl_bytes = dev->vtbl_slots * sizeof(nand_ubi_vtbl_record_t);
    uint32_t vtbl_io_bytes = ((vtbl_bytes + page_size - 1u) / page_size) * page_size;
    dev->volumes = calloc(dev->vtbl_slots, sizeof(*dev->volumes));
    dev->vtbl = ubi_alloc(vtbl_io_bytes);
    if (dev->volumes == NULL || dev->vtbl == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    memset(dev->vtbl, 0xFF, vtbl_io_bytes);

    ret = load_volume_table(dev);
    if (ret != ESP_OK) {
        goto fail;
    }

    sqnum_seen = ubi_alloc((size_t)peb_count * sizeof(uint64_t));
    page_buf = ubi_alloc(page_size);
    if (!sqnum_seen || !page_buf) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    memset(sqnum_seen, 0, (size_t)peb_count * sizeof(uint64_t));

    uint32_t image_seq = 0;
    bool have_image_seq = false;

    for (uint32_t pnum = 0; pnum < peb_count; pnum++) {
        esp_blockdev_cmd_arg_status_t bad_arg = { .num = pnum, .status = false };
        ret = nand_bdl->ops->ioctl(nand_bdl, ESP_BLOCKDEV_CMD_IS_BAD_BLOCK, &bad_arg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "IS_BAD_BLOCK ioctl failed for pnum=%" PRIu32 ": 0x%x", pnum, ret);
            goto fail;
        }
        if (bad_arg.status) {
            nand_ubi_eba_peb_set_bad(&dev->eba, pnum);
            continue;
        }
        if (pnum < UBI_LAYOUT_VOLUME_EBS) {
            nand_ubi_eba_peb_set_used(&dev->eba, pnum);
            continue;
        }

        ret = nand_bdl->ops->read(nand_bdl, page_buf, page_size, (uint64_t)pnum * peb_size, page_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "EC header read failed for pnum=%" PRIu32 ": 0x%x", pnum, ret);
            goto fail;
        }
        if (page_is_blank(page_buf, page_size)) {
            continue; /* peb_state defaults to FREE */
        }

        const nand_ubi_ec_hdr_t *ec_hdr = (const nand_ubi_ec_hdr_t *)page_buf;
        if (!nand_ubi_ec_hdr_valid(ec_hdr)) {
            ESP_LOGW(TAG, "pnum=%" PRIu32 ": corrupt EC header, scheduling erase", pnum);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            continue;
        }

        uint32_t peb_image_seq = nand_ubi_be32(ec_hdr->image_seq);
        if (!have_image_seq) {
            image_seq = peb_image_seq;
            have_image_seq = true;
        } else if (peb_image_seq != image_seq) {
            ESP_LOGW(TAG, "pnum=%" PRIu32 ": image_seq 0x%08" PRIx32 " != 0x%08" PRIx32 ", treating as stale",
                     pnum, peb_image_seq, image_seq);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            continue;
        }

        uint32_t vid_hdr_offset = nand_ubi_be32(ec_hdr->vid_hdr_offset);
        uint32_t data_offset = nand_ubi_be32(ec_hdr->data_offset);
        if (vid_hdr_offset == 0 || vid_hdr_offset % page_size != 0 ||
                data_offset % page_size != 0 || data_offset < vid_hdr_offset + page_size ||
                data_offset >= peb_size) {
            ESP_LOGW(TAG, "pnum=%" PRIu32 ": bad header offsets vid=%" PRIu32 " data=%" PRIu32
                     ", scheduling erase", pnum, vid_hdr_offset, data_offset);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            continue;
        }
        if (vid_hdr_offset != dev->vid_hdr_offset || data_offset != dev->data_offset) {
            ESP_LOGW(TAG, "pnum=%" PRIu32 ": header layout differs from volume table, scheduling erase", pnum);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            continue;
        }

        ret = nand_bdl->ops->read(nand_bdl, page_buf, page_size,
                                   (uint64_t)pnum * peb_size + vid_hdr_offset, page_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "VID header read failed for pnum=%" PRIu32 ": 0x%x", pnum, ret);
            goto fail;
        }
        if (page_is_blank(page_buf, page_size)) {
            /* EC header was written but the VID header write never landed: an
             * allocation was interrupted (e.g. power loss) between the two writes
             * in nand_ubi_vol_alloc_peb(). Page 0 is NOT blank, so this PEB does
             * not satisfy the "free PEBs are physically erased" invariant that
             * nand_ubi_vol_alloc_peb() relies on to skip a read-before-write check.
             * Schedule it for erase instead of leaving the default FREE state. */
            ESP_LOGW(TAG, "pnum=%" PRIu32 ": EC header present but VID header blank "
                     "(interrupted allocation), scheduling erase", pnum);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            continue;
        }

        const nand_ubi_vid_hdr_t *vid_hdr = (const nand_ubi_vid_hdr_t *)page_buf;
        if (!nand_ubi_vid_hdr_valid(vid_hdr)) {
            ESP_LOGW(TAG, "pnum=%" PRIu32 ": corrupt VID header, scheduling erase", pnum);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            continue;
        }

        uint32_t vol_id = nand_ubi_be32(vid_hdr->vol_id);
        const nand_ubi_volume_info_t *volume = find_volume(dev, vol_id);
        if (volume == NULL) {
            ESP_LOGW(TAG, "pnum=%" PRIu32 ": unknown vol_id=%" PRIu32 ", scheduling erase", pnum, vol_id);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            continue;
        }

        uint32_t lnum = nand_ubi_be32(vid_hdr->lnum);
        uint64_t sqnum = nand_ubi_be64(vid_hdr->sqnum);
        if (lnum >= volume->leb_count) {
            ESP_LOGW(TAG, "pnum=%" PRIu32 ": vol_id=%" PRIu32 " lnum=%" PRIu32
                     " out of range, scheduling erase", pnum, vol_id, lnum);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            continue;
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
                continue;
            }
            nand_ubi_eba_set(&dev->eba, eba_index, (int32_t)pnum);
            sqnum_seen[eba_index] = sqnum;
            nand_ubi_eba_peb_set_used(&dev->eba, pnum);
        } else if (sqnum > sqnum_seen[eba_index]) {
            uint32_t old_pnum = (uint32_t)existing_pnum;
            if (vid_hdr->copy_flag && !verify_copy_data(dev, pnum, dev->data_offset, vid_hdr)) {
                ESP_LOGW(TAG, "pnum=%" PRIu32 " lnum=%" PRIu32 ": copy_flag data_crc mismatch, keeping pnum %" PRIu32,
                         pnum, lnum, old_pnum);
                nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            } else {
                nand_ubi_eba_peb_set_erase_pending(&dev->eba, old_pnum);
                nand_ubi_eba_set(&dev->eba, eba_index, (int32_t)pnum);
                sqnum_seen[eba_index] = sqnum;
                nand_ubi_eba_peb_set_used(&dev->eba, pnum);
            }
        } else {
            ESP_LOGW(TAG, "pnum=%" PRIu32 ": stale duplicate of lnum=%" PRIu32 ", scheduling erase", pnum, lnum);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
        }
    }

    free(page_buf);
    free(sqnum_seen);
    page_buf = NULL;
    sqnum_seen = NULL;

    dev->image_seq = image_seq;
    dev->leb_count = 0;
    for (uint32_t i = 0; i < dev->vol_count; i++) {
        dev->leb_count += dev->volumes[i].leb_count;
    }
    dev->leb_capacity = peb_count - UBI_LAYOUT_VOLUME_EBS;

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
    free(sqnum_seen);
    free(dev->vtbl);
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

    uint32_t open_volumes = 0;
    xSemaphoreTake(ubi_dev->lock, portMAX_DELAY);
    open_volumes = ubi_dev->open_volumes;
    xSemaphoreGive(ubi_dev->lock);
    if (open_volumes > 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (ubi_dev->lock) {
        vSemaphoreDelete(ubi_dev->lock);
    }
    free(ubi_dev->vtbl);
    free(ubi_dev->volumes);
    nand_ubi_eba_free(&ubi_dev->eba);
    free(ubi_dev);
    return ESP_OK;
}

/**************************************************************************************
 * Volume-level: BDL ops vtable for a handle returned by nand_ubi_open_volume().
 **************************************************************************************
 */

/* Allocates a free PEB for lnum, writes fresh EC+VID headers onto it, and records the
 * lnum->pnum mapping. Caller must hold dev->lock.
 *
 * A PEB only ever re-enters the free pool already physically erased (attach()'s scan
 * marks blank PEBs free; nand_ubi_vol_erase() below only marks a PEB free right after
 * erasing it), so page 0 and vid_hdr_offset are always blank here - no header exists
 * yet to conflict with, and no read-before-write check is needed. This is also why
 * nand_ubi_vol_erase() does not itself write a fresh EC header: Phase 1 has no EC
 * table to make erase-count tracking meaningful (nand_ubi_eba_find_free_peb() does an
 * unweighted linear scan), so writing it once here covers every allocation path. */
static esp_err_t nand_ubi_vol_alloc_peb(nand_ubi_device_t *dev, uint32_t vol_id,
                                        uint32_t local_lnum, uint32_t eba_index,
                                        uint8_t vol_type, int32_t *out_pnum)
{
    int32_t pnum = nand_ubi_eba_find_free_peb(&dev->eba, dev->peb_count);
    if (pnum < 0) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *page_buf = ubi_alloc(dev->page_size);
    if (page_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(page_buf, 0xFF, dev->page_size);
    fill_ec_header(dev, (nand_ubi_ec_hdr_t *)page_buf);

    esp_err_t ret = dev->nand_bdl->ops->write(dev->nand_bdl, page_buf,
                                               (uint64_t)pnum * dev->peb_size, dev->page_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pnum=%" PRIi32 ": EC header write failed: 0x%x", pnum, ret);
        free(page_buf);
        return ret;
    }

    memset(page_buf, 0xFF, dev->page_size);
    fill_vid_header(dev, (nand_ubi_vid_hdr_t *)page_buf, vol_id, local_lnum, vol_type);

    ret = dev->nand_bdl->ops->write(dev->nand_bdl, page_buf,
                                     (uint64_t)pnum * dev->peb_size + dev->vid_hdr_offset, dev->page_size);
    free(page_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pnum=%" PRIi32 ": VID header write failed: 0x%x", pnum, ret);
        return ret;
    }

    nand_ubi_eba_set(&dev->eba, eba_index, pnum);
    nand_ubi_eba_peb_set_used(&dev->eba, (uint32_t)pnum);
    *out_pnum = pnum;
    return ESP_OK;
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
        ret = ESP_ERR_NOT_FOUND;
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
    if (handle->device_flags.read_only) {
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
        uint8_t vol_type = dev->vtbl[vol_ctx->vol_id].vol_type;
        ret = nand_ubi_vol_alloc_peb(dev, vol_ctx->vol_id, lnum, eba_index, vol_type, &pnum);
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
    if (handle->device_flags.read_only) {
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
    for (uint32_t lnum = start_lnum; lnum < start_lnum + leb_span; lnum++) {
        uint32_t eba_index = vol_ctx->eba_offset + lnum;
        int32_t pnum = nand_ubi_eba_get_pnum(&dev->eba, eba_index);
        if (pnum == UBI_LEB_UNMAPPED) {
            continue; /* already logically erased: idempotent no-op for this LEB */
        }
        ret = dev->nand_bdl->ops->erase(dev->nand_bdl, (uint64_t)pnum * dev->peb_size, dev->peb_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "lnum=%" PRIu32 " pnum=%" PRIi32 ": erase failed: 0x%x", lnum, pnum, ret);
            break;
        }
        nand_ubi_eba_set(&dev->eba, eba_index, UBI_LEB_UNMAPPED);
        nand_ubi_eba_peb_set_free(&dev->eba, (uint32_t)pnum);
    }
    xSemaphoreGive(dev->lock);
    return ret;
}

static esp_err_t nand_ubi_vol_sync(esp_blockdev_handle_t handle)
{
    nand_ubi_vol_ctx_t *vol_ctx = (nand_ubi_vol_ctx_t *)handle->ctx;
    esp_blockdev_handle_t nand_bdl = vol_ctx->dev->nand_bdl;
    return nand_bdl->ops->sync(nand_bdl);
}

static esp_err_t nand_ubi_vol_ioctl(esp_blockdev_handle_t handle, const uint8_t cmd, void *args)
{
    nand_ubi_vol_ctx_t *vol_ctx = (nand_ubi_vol_ctx_t *)handle->ctx;
    esp_blockdev_handle_t nand_bdl = vol_ctx->dev->nand_bdl;
    return nand_bdl->ops->ioctl(nand_bdl, cmd, args);
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
        uint32_t capacity = 0;
        if (dev->peb_count > dev->reserved_pebs + UBI_LAYOUT_VOLUME_EBS) {
            capacity = dev->peb_count - dev->reserved_pebs - UBI_LAYOUT_VOLUME_EBS;
        }
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
