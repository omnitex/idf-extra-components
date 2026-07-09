/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include <assert.h>
#include "nand_ubi_eba.h"
#include "esp_nand_ubi_media.h"
#include "nand_ubi_priv.h"

static inline uint8_t peb_state_get(const uint8_t *arr, uint32_t pnum)
{
    return (arr[pnum / 4u] >> ((pnum % 4u) * 2u)) & 0x3u;
}

static inline void peb_state_set(nand_ubi_eba_t *eba, uint32_t pnum, uint8_t state)
{
    uint8_t old = peb_state_get(eba->peb_state, pnum);
    eba->free_count += (state == UBI_PEB_FREE) - (old == UBI_PEB_FREE);
    uint32_t shift = (pnum % 4u) * 2u;
    eba->peb_state[pnum / 4u] = (uint8_t)((eba->peb_state[pnum / 4u] & ~(uint8_t)(0x3u << shift))
                                          | (uint8_t)((state & 0x3u) << shift));
}

esp_err_t nand_ubi_eba_alloc(uint32_t peb_count, uint32_t leb_count,
                             nand_ubi_eba_t *out_eba)
{
    if (peb_count > UBI_EBA_MAX_PEBS) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint16_t *eba       = ubi_alloc((size_t)leb_count * sizeof(uint16_t));
    uint8_t *peb_state  = ubi_alloc((size_t)(peb_count + 3u) / 4u);

    if ((!eba && leb_count > 0) || !peb_state) {
        free(eba);
        free(peb_state);
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t i = 0; i < leb_count; i++) {
        eba[i] = UBI_EBA_UNMAPPED_ENTRY;
    }
    memset(peb_state, 0, (size_t)(peb_count + 3u) / 4u);

    memset(out_eba, 0, sizeof(*out_eba));
    out_eba->eba        = eba;
    out_eba->peb_state  = peb_state;
    out_eba->free_count = peb_count;
    nand_ubi_eba_seed(out_eba, 0);
    out_eba->leb_count  = leb_count;
    out_eba->peb_count  = peb_count;
    return ESP_OK;
}

esp_err_t nand_ubi_eba_grow(nand_ubi_eba_t *eba, uint32_t leb_count)
{
    assert(leb_count >= eba->leb_count);
    uint16_t *grown = ubi_alloc((size_t)leb_count * sizeof(uint16_t));
    if (grown == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (eba->leb_count > 0) {
        memcpy(grown, eba->eba, (size_t)eba->leb_count * sizeof(uint16_t));
    }
    for (uint32_t i = eba->leb_count; i < leb_count; i++) {
        grown[i] = UBI_EBA_UNMAPPED_ENTRY;
    }
    free(eba->eba);
    eba->eba = grown;
    eba->leb_count = leb_count;
    return ESP_OK;
}

void nand_ubi_eba_free(nand_ubi_eba_t *eba)
{
    free(eba->eba);
    free(eba->peb_state);
    eba->eba        = NULL;
    eba->peb_state  = NULL;
    eba->leb_count  = 0;
    eba->peb_count  = 0;
}

int32_t nand_ubi_eba_get_pnum(const nand_ubi_eba_t *eba, uint32_t lnum)
{
    assert(lnum < eba->leb_count);
    uint16_t entry = eba->eba[lnum];
    return entry == UBI_EBA_UNMAPPED_ENTRY ? UBI_LEB_UNMAPPED : (int32_t)entry;
}

void nand_ubi_eba_set(nand_ubi_eba_t *eba, uint32_t lnum, int32_t pnum)
{
    assert(lnum < eba->leb_count);
    assert(pnum == UBI_LEB_UNMAPPED || (pnum >= 0 && (uint32_t)pnum < eba->peb_count));
    eba->eba[lnum] = pnum == UBI_LEB_UNMAPPED ? UBI_EBA_UNMAPPED_ENTRY : (uint16_t)pnum;
}

bool nand_ubi_eba_peb_is_free(const nand_ubi_eba_t *eba, uint32_t pnum)
{
    assert(pnum < eba->peb_count);
    return peb_state_get(eba->peb_state, pnum) == UBI_PEB_FREE;
}

void nand_ubi_eba_peb_set_free(nand_ubi_eba_t *eba, uint32_t pnum)
{
    assert(pnum < eba->peb_count);
    peb_state_set(eba, pnum, UBI_PEB_FREE);
}

void nand_ubi_eba_peb_set_used(nand_ubi_eba_t *eba, uint32_t pnum)
{
    assert(pnum < eba->peb_count);
    peb_state_set(eba, pnum, UBI_PEB_USED);
}

void nand_ubi_eba_peb_set_bad(nand_ubi_eba_t *eba, uint32_t pnum)
{
    assert(pnum < eba->peb_count);
    peb_state_set(eba, pnum, UBI_PEB_BAD);
}

void nand_ubi_eba_peb_set_erase_pending(nand_ubi_eba_t *eba, uint32_t pnum)
{
    assert(pnum < eba->peb_count);
    peb_state_set(eba, pnum, UBI_PEB_ERASE_PENDING);
}

uint8_t nand_ubi_eba_peb_get_state(const nand_ubi_eba_t *eba, uint32_t pnum)
{
    assert(pnum < eba->peb_count);
    return peb_state_get(eba->peb_state, pnum);
}

void nand_ubi_eba_recount_free(nand_ubi_eba_t *eba)
{
    eba->free_count = 0;
    for (uint32_t p = 0; p < eba->peb_count; p++) {
        eba->free_count += peb_state_get(eba->peb_state, p) == UBI_PEB_FREE;
    }
}

void nand_ubi_eba_seed(nand_ubi_eba_t *eba, uint32_t seed)
{
    eba->rng = seed ? seed : 0x9E3779B9u;
}

static uint32_t rng_next(nand_ubi_eba_t *eba)
{
    uint32_t x = eba->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    eba->rng = x;
    return x;
}

static bool picked(const int32_t *out, uint32_t n, uint32_t pnum)
{
    for (uint32_t i = 0; i < n; i++) {
        if (out[i] == (int32_t)pnum) {
            return true;
        }
    }
    return false;
}

uint32_t nand_ubi_eba_sample_free(nand_ubi_eba_t *eba, uint32_t lo, int32_t *out, uint32_t k)
{
    uint32_t n = 0;
    if (lo >= eba->peb_count || eba->free_count == 0) {
        return 0;
    }
    uint32_t span = eba->peb_count - lo;
    /* Rejection sampling costs span / free draws per pick. With few free PEBs, or once the
     * cap is reached on a nearly full chip, the scan below fills in from a random start. */
    if (eba->free_count > k) {
        for (uint32_t tries = 16u * k; tries > 0 && n < k; tries--) {
            uint32_t p = lo + rng_next(eba) % span;
            if (peb_state_get(eba->peb_state, p) == UBI_PEB_FREE && !picked(out, n, p)) {
                out[n++] = (int32_t)p;
            }
        }
    }
    uint32_t start = n < k && eba->free_count > k ? rng_next(eba) % span : 0;
    for (uint32_t i = 0; i < span && n < k; i++) {
        uint32_t p = lo + (start + i) % span;
        if (peb_state_get(eba->peb_state, p) == UBI_PEB_FREE && !picked(out, n, p)) {
            out[n++] = (int32_t)p;
        }
    }
    return n;
}
