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

/* Verifies a copy_flag PEB's LEB data against vid_hdr->data_crc. Reads in page_size
 * multiples (nand_bdl->ops->read() rejects a partial-page tail on a multi-page span)
 * and hashes only the first data_size bytes, matching how data_crc was computed.
 * peb_size/page_size come from nand_bdl->geometry rather than as extra parameters:
 * attach() hasn't finished populating dev's copies of them at the point this is
 * called, and re-deriving avoids stacking more same-typed uint32_t parameters. */
static bool verify_copy_data(esp_blockdev_handle_t nand_bdl, uint32_t pnum,
                              uint32_t data_offset, const nand_ubi_vid_hdr_t *vid_hdr)
{
    uint32_t peb_size = (uint32_t)nand_bdl->geometry.erase_size;
    uint32_t page_size = (uint32_t)nand_bdl->geometry.read_size;
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

    esp_err_t ret = nand_bdl->ops->read(nand_bdl, buf, read_len,
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

    /* leb_count is unknown until the scan below finds max_lnum, so eba[] is
     * over-allocated to peb_count (leb_count <= peb_count always). */
    esp_err_t ret = nand_ubi_eba_alloc(peb_count, peb_count, &dev->eba);
    if (ret != ESP_OK) {
        free(dev);
        return ret;
    }

    uint64_t *sqnum_seen = ubi_alloc((size_t)peb_count * sizeof(uint64_t));
    uint8_t *page_buf = ubi_alloc(page_size);
    if (!sqnum_seen || !page_buf) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    memset(sqnum_seen, 0, (size_t)peb_count * sizeof(uint64_t));

    uint32_t image_seq = 0;
    bool have_image_seq = false;
    int32_t max_lnum = -1;

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
        if (dev->vid_hdr_offset == 0) {
            /* First valid EC header seen defines this attach's header layout. */
            dev->vid_hdr_offset = vid_hdr_offset;
            dev->data_offset = data_offset;
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

        uint32_t lnum = nand_ubi_be32(vid_hdr->lnum);
        uint64_t sqnum = nand_ubi_be64(vid_hdr->sqnum);
        if (lnum >= peb_count) {
            ESP_LOGW(TAG, "pnum=%" PRIu32 ": lnum %" PRIu32 " out of range, scheduling erase", pnum, lnum);
            nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            continue;
        }

        /* This lnum exists in the volume's logical address space regardless of which
         * physical replica ultimately wins below, so leb_count/global_sqnum track it
         * unconditionally rather than only on the accepted branch. */
        if ((int32_t)lnum > max_lnum) {
            max_lnum = (int32_t)lnum;
        }
        if (sqnum > dev->global_sqnum) {
            dev->global_sqnum = sqnum;
        }

        int32_t existing_pnum = nand_ubi_eba_get_pnum(&dev->eba, lnum);
        if (existing_pnum == UBI_LEB_UNMAPPED) {
            if (vid_hdr->copy_flag && !verify_copy_data(nand_bdl, pnum, dev->data_offset, vid_hdr)) {
                ESP_LOGW(TAG, "pnum=%" PRIu32 " lnum=%" PRIu32 ": copy_flag data_crc mismatch, scheduling erase",
                         pnum, lnum);
                nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
                continue;
            }
            nand_ubi_eba_set(&dev->eba, lnum, (int32_t)pnum);
            sqnum_seen[lnum] = sqnum;
            nand_ubi_eba_peb_set_used(&dev->eba, pnum);
        } else if (sqnum > sqnum_seen[lnum]) {
            uint32_t old_pnum = (uint32_t)existing_pnum;
            if (vid_hdr->copy_flag && !verify_copy_data(nand_bdl, pnum, dev->data_offset, vid_hdr)) {
                ESP_LOGW(TAG, "pnum=%" PRIu32 " lnum=%" PRIu32 ": copy_flag data_crc mismatch, keeping pnum %" PRIu32,
                         pnum, lnum, old_pnum);
                nand_ubi_eba_peb_set_erase_pending(&dev->eba, pnum);
            } else {
                nand_ubi_eba_peb_set_erase_pending(&dev->eba, old_pnum);
                nand_ubi_eba_set(&dev->eba, lnum, (int32_t)pnum);
                sqnum_seen[lnum] = sqnum;
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

    if (dev->vid_hdr_offset == 0) {
        /* Blank/unformatted chip: no valid EC header was ever found. Fall back to the
         * same layout esp_ubinize.py uses so leb_size is still well-defined. */
        dev->vid_hdr_offset = page_size;
        dev->data_offset = 2u * page_size;
    }

    dev->nand_bdl = nand_bdl;
    dev->peb_count = peb_count;
    dev->peb_size = peb_size;
    dev->page_size = page_size;
    dev->leb_size = dev->peb_size - dev->data_offset;
    dev->image_seq = image_seq;
    dev->leb_count = (uint32_t)(max_lnum + 1);
    dev->read_only = effective_config.read_only;

    /* Capacity, not the scan-derived leb_count, bounds geometry.disk_size and the write()
     * path (see nand_ubi_open_volume()): otherwise a blank chip (leb_count == 0) would
     * expose zero writable space. Never shrinks below leb_count: reserved_pebs must not
     * make already-mapped LEBs unreachable through the volume's own geometry. */
    uint32_t capacity = (peb_count > effective_config.reserved_pebs) ? (peb_count - effective_config.reserved_pebs) : 0;
    if (capacity < dev->leb_count) {
        capacity = dev->leb_count;
    }
    dev->leb_capacity = capacity;

    dev->lock = xSemaphoreCreateMutex();
    if (dev->lock == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    ESP_LOGI(TAG, "attached: peb_count=%" PRIu32 " leb_count=%" PRIu32 " leb_capacity=%" PRIu32
             " peb_size=%" PRIu32 " leb_size=%" PRIu32 " image_seq=0x%08" PRIx32,
             dev->peb_count, dev->leb_count, dev->leb_capacity, dev->peb_size, dev->leb_size, dev->image_seq);

    *out_ubi_dev = dev;
    return ESP_OK;

fail:
    free(page_buf);
    free(sqnum_seen);
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
static esp_err_t nand_ubi_vol_alloc_peb(nand_ubi_device_t *dev, uint32_t lnum, int32_t *out_pnum)
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
    nand_ubi_ec_hdr_t *ec_hdr = (nand_ubi_ec_hdr_t *)page_buf;
    memset(ec_hdr, 0, sizeof(*ec_hdr));
    ec_hdr->magic = nand_ubi_be32(UBI_EC_HDR_MAGIC);
    ec_hdr->version = UBI_VERSION;
    ec_hdr->ec = nand_ubi_be64(0);
    ec_hdr->vid_hdr_offset = nand_ubi_be32(dev->vid_hdr_offset);
    ec_hdr->data_offset = nand_ubi_be32(dev->data_offset);
    ec_hdr->image_seq = nand_ubi_be32(dev->image_seq);
    ec_hdr->hdr_crc = nand_ubi_be32(nand_ubi_crc32(ec_hdr, UBI_EC_HDR_SIZE_CRC));

    esp_err_t ret = dev->nand_bdl->ops->write(dev->nand_bdl, page_buf,
                                               (uint64_t)pnum * dev->peb_size, dev->page_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pnum=%" PRIi32 ": EC header write failed: 0x%x", pnum, ret);
        free(page_buf);
        return ret;
    }

    memset(page_buf, 0xFF, dev->page_size);
    nand_ubi_vid_hdr_t *vid_hdr = (nand_ubi_vid_hdr_t *)page_buf;
    memset(vid_hdr, 0, sizeof(*vid_hdr));
    vid_hdr->magic = nand_ubi_be32(UBI_VID_HDR_MAGIC);
    vid_hdr->version = UBI_VERSION;
    vid_hdr->vol_type = UBI_VID_DYNAMIC;
    vid_hdr->vol_id = nand_ubi_be32(0);
    vid_hdr->lnum = nand_ubi_be32(lnum);
    vid_hdr->sqnum = nand_ubi_be64(++dev->global_sqnum);
    vid_hdr->hdr_crc = nand_ubi_be32(nand_ubi_crc32(vid_hdr, UBI_VID_HDR_SIZE_CRC));

    ret = dev->nand_bdl->ops->write(dev->nand_bdl, page_buf,
                                     (uint64_t)pnum * dev->peb_size + dev->vid_hdr_offset, dev->page_size);
    free(page_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pnum=%" PRIi32 ": VID header write failed: 0x%x", pnum, ret);
        return ret;
    }

    nand_ubi_eba_set(&dev->eba, lnum, pnum);
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
    uint32_t offset = (uint32_t)(src_addr % dev->leb_size);
    if (offset + data_read_len > dev->leb_size) {
        /* Crosses into the next LEB, which is not necessarily the next PEB: nand_bdl has
         * no notion of LEB boundaries and would happily read into unrelated data. */
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(dev->lock, portMAX_DELAY);
    int32_t pnum = nand_ubi_eba_get_pnum(&dev->eba, lnum);
    esp_err_t ret;
    if (pnum == UBI_LEB_UNMAPPED) {
        ret = ESP_ERR_NOT_FOUND;
    } else {
        uint64_t phys_addr = (uint64_t)pnum * dev->peb_size + dev->data_offset + offset;
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
    uint32_t offset = (uint32_t)(dst_addr % dev->leb_size);
    if (offset + data_write_len > dev->leb_size) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(dev->lock, portMAX_DELAY);
    int32_t pnum = nand_ubi_eba_get_pnum(&dev->eba, lnum);
    esp_err_t ret = ESP_OK;
    if (pnum == UBI_LEB_UNMAPPED) {
        ret = nand_ubi_vol_alloc_peb(dev, lnum, &pnum);
    }
    if (ret == ESP_OK) {
        uint64_t phys_addr = (uint64_t)pnum * dev->peb_size + dev->data_offset + offset;
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
        int32_t pnum = nand_ubi_eba_get_pnum(&dev->eba, lnum);
        if (pnum == UBI_LEB_UNMAPPED) {
            continue; /* already logically erased: idempotent no-op for this LEB */
        }
        ret = dev->nand_bdl->ops->erase(dev->nand_bdl, (uint64_t)pnum * dev->peb_size, dev->peb_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "lnum=%" PRIu32 " pnum=%" PRIi32 ": erase failed: 0x%x", lnum, pnum, ret);
            break;
        }
        nand_ubi_eba_set(&dev->eba, lnum, UBI_LEB_UNMAPPED);
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
    if (vol_id != 0) {
        /* Phase 1: whole chip is one volume, no volume table yet; vtbl-backed
         * multi-volume lookup is Task 12. */
        return ESP_ERR_NOT_FOUND;
    }

    nand_ubi_vol_ctx_t *vol_ctx = calloc(1, sizeof(*vol_ctx));
    if (vol_ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    vol_ctx->dev = ubi_dev;
    vol_ctx->vol_id = vol_id;
    vol_ctx->leb_count = ubi_dev->leb_capacity;
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
    ret = nand_ubi_open_volume(dev, 0, &vol_bdl);
    if (ret != ESP_OK) {
        nand_ubi_detach(dev);
        return ret;
    }

    ((nand_ubi_vol_ctx_t *)vol_bdl->ctx)->owns_device = true;

    *out_vol_bdl = vol_bdl;
    return ESP_OK;
}
