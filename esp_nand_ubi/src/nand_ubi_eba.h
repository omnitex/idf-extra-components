/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UBI_PEB_FREE            0u
#define UBI_PEB_USED            1u
#define UBI_PEB_BAD             2u
#define UBI_PEB_ERASE_PENDING   3u

/* EBA entries are 16-bit to halve the table; attach rejects chips with more PEBs. */
#define UBI_EBA_MAX_PEBS        0xFFFFu
#define UBI_EBA_UNMAPPED_ENTRY  0xFFFFu

typedef struct {
    uint16_t *eba;          /* [leb_count]: lnum → pnum; UBI_EBA_UNMAPPED_ENTRY if unmapped */
    uint8_t  *peb_state;    /* (peb_count + 3) / 4 bytes; 2 bits per PEB (UBI_PEB_*) */
    uint32_t  free_count;   /* PEBs in state UBI_PEB_FREE */
    uint32_t  rng;          /* xorshift32 state for nand_ubi_eba_sample_free(); never 0 */
    uint32_t  leb_count;
    uint32_t  peb_count;
} nand_ubi_eba_t;

esp_err_t nand_ubi_eba_alloc(uint32_t peb_count, uint32_t leb_count,
                             nand_ubi_eba_t *out_eba);
/* Extends eba[] to leb_count entries; the new ones are unmapped. */
esp_err_t nand_ubi_eba_grow(nand_ubi_eba_t *eba, uint32_t leb_count);
void      nand_ubi_eba_free(nand_ubi_eba_t *eba);

int32_t   nand_ubi_eba_get_pnum(const nand_ubi_eba_t *eba, uint32_t lnum);
void      nand_ubi_eba_set(nand_ubi_eba_t *eba, uint32_t lnum, int32_t pnum);

bool      nand_ubi_eba_peb_is_free(const nand_ubi_eba_t *eba, uint32_t pnum);
void      nand_ubi_eba_peb_set_free(nand_ubi_eba_t *eba, uint32_t pnum);
void      nand_ubi_eba_peb_set_used(nand_ubi_eba_t *eba, uint32_t pnum);
void      nand_ubi_eba_peb_set_bad(nand_ubi_eba_t *eba, uint32_t pnum);
void      nand_ubi_eba_peb_set_erase_pending(nand_ubi_eba_t *eba, uint32_t pnum);
uint8_t   nand_ubi_eba_peb_get_state(const nand_ubi_eba_t *eba, uint32_t pnum);

/* Seeds the free-PEB sampler. A zero seed is replaced by a fixed non-zero one. */
void      nand_ubi_eba_seed(nand_ubi_eba_t *eba, uint32_t seed);

/* Picks up to k distinct free PEBs numbered lo or higher, uniformly at random, into out[]
 * and returns how many were picked: k, or fewer only when fewer than k such PEBs are free.
 * Erase counts are not kept in RAM; the caller reads the candidates' EC headers. */
uint32_t  nand_ubi_eba_sample_free(nand_ubi_eba_t *eba, uint32_t lo, int32_t *out, uint32_t k);

/* Recomputes free_count after peb_state[] was written directly. */
void      nand_ubi_eba_recount_free(nand_ubi_eba_t *eba);

#ifdef __cplusplus
}
#endif
