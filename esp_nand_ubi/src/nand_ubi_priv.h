/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_blockdev.h"
#include "esp_nand_ubi.h"
#include "esp_nand_ubi_media.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nand_ubi_eba.h"

#if CONFIG_SPIRAM
#include "esp_heap_caps.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* free() works on both paths: IDF's allocator handles all heap regions. */
static inline void *ubi_alloc(size_t size)
{
#if CONFIG_SPIRAM
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        return p;
    }
#endif
    return malloc(size);
}

typedef struct {
    uint32_t vol_id;
    uint32_t leb_count;
    uint32_t eba_offset;
} nand_ubi_volume_info_t;

/**
 * @brief Device-level state: one instance per attached physical NAND chip.
 *
 * Not exposed in the public header; volume BDL handles hold a pointer to this
 * struct (via @ref nand_ubi_vol_ctx_t) instead of duplicating its fields.
 */
struct nand_ubi_device {
    esp_blockdev_handle_t nand_bdl;   /**< Raw flash BDL passed to nand_ubi_attach(); not owned. */

    uint32_t peb_count;
    uint32_t peb_size;
    uint32_t page_size;
    uint32_t vid_hdr_offset;
    uint32_t data_offset;
    uint32_t leb_size;                 /**< = peb_size - data_offset. */
    uint32_t leb_count;                /**< Sum of all user-volume LEB reservations. */
    uint32_t leb_capacity;             /**< Global EBA capacity after the two layout PEBs. */
    uint32_t reserved_pebs;

    nand_ubi_volume_info_t *volumes;   /**< Volume index, in vtbl slot order. */
    nand_ubi_vtbl_record_t *vtbl;      /**< Selected mirrored vtbl image (allocated once vtbl_slots
                                             is known; holds vtbl_slots records regardless of vol_count). */
    uint32_t vol_count;                /**< Number of valid user-volume records. */
    uint32_t vtbl_slots;               /**< min(UBI_MAX_VOLUMES, leb_size / sizeof(nand_ubi_vtbl_record_t)). */

    uint32_t image_seq;
    uint64_t global_sqnum;             /**< Highest VID sqnum observed at attach; next write uses +1. */
    bool     read_only;                /**< From nand_ubi_config_t.read_only; propagated to
                                             device_flags.read_only of every opened volume. */

    nand_ubi_eba_t eba;                /**< eba[] is sized to peb_count, not leb_count: leb_count is
                                             only known once the scan finishes, but leb_count <= peb_count
                                             always holds, so peb_count is a safe upfront allocation bound. */

    SemaphoreHandle_t lock;            /**< Serializes read/write/erase/open_volume. */
    uint32_t open_volumes;             /**< Count of open volume BDL handles (protected by lock);
                                             nand_ubi_detach() refuses with ESP_ERR_INVALID_STATE
                                             while this is nonzero. */
};

/**
 * @brief Volume-level state: one instance per handle returned by @c nand_ubi_open_volume().
 *
 * The @c ctx field of the returned @c esp_blockdev_t points at this struct; volume BDL ops
 * cast it back to reach the owning device.
 */
typedef struct {
    nand_ubi_device_t *dev;            /**< Owning device; not owned by the volume. */
    uint32_t           vol_id;          /**< Volume ID from the volume table. */
    uint32_t           leb_count;       /**< Volume's addressable LEB count. */
    uint32_t           eba_offset;      /**< Start of this volume's contiguous slice in dev->eba. */
    bool               owns_device;     /**< true when opened via nand_ubi_get_blockdev(): release()
                                             also calls nand_ubi_detach() on dev. */
} nand_ubi_vol_ctx_t;

#ifdef __cplusplus
}
#endif
