/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * Power-cut stress workload for a block device with NAND-like erase semantics
 * (an esp_nand_ubi volume). Shared by the host harness and the board app; see
 * docs/plans/2026-10-08-esp-nand-ubi-powercut-stress-design.md.
 *
 * Volume layout: LEBs 0 .. data_lebs-1 hold data, the next two LEBs hold the journal.
 * One step rewrites one data LEB: journal INTENT, erase, write pages_per_step pages,
 * journal DONE. ptest_ubi_verify() checks the volume against the journal after a cut.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_blockdev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PTEST_UBI_JOURNAL_LEBS 2u

typedef struct {
    uint32_t data_lebs;       /**< Data LEBs; the volume needs data_lebs + PTEST_UBI_JOURNAL_LEBS LEBs. */
    uint32_t pages_per_step;  /**< Pages written per LEB rewrite; 0 = every page of the LEB. */
    uint32_t seed;            /**< PRNG seed for LEB selection. */
} ptest_ubi_cfg_t;

typedef struct {
    bool     ok;
    uint32_t lebs_matching;   /**< Data LEBs exactly as the journal says. */
    uint32_t lost;            /**< LEBs whose committed (DONE) generation is missing or older. */
    uint32_t corrupt;         /**< LEBs with wrong bytes, mixed generations, or data after a blank page. */
    uint32_t in_flight;       /**< 1 if the cut hit a LEB rewrite (state of that LEB is indeterminate). */
    uint32_t journal_records; /**< Valid journal records read. */
    uint32_t journal_torn;    /**< Torn journal records; each cut can leave one. */
    char     first_error[96]; /**< Description of the first failure, empty if ok. */
} ptest_ubi_result_t;

typedef struct ptest_ubi ptest_ubi_t;

/** Allocates the workload state for vol. Does not touch the flash. */
esp_err_t ptest_ubi_create(esp_blockdev_handle_t vol, const ptest_ubi_cfg_t *cfg, ptest_ubi_t **out);
void ptest_ubi_destroy(ptest_ubi_t *t);

/** Rebinds the workload to a new handle for the same volume (after a reboot). */
void ptest_ubi_set_volume(ptest_ubi_t *t, esp_blockdev_handle_t vol);

/**
 * Reads the journal and every data LEB, checks them, and rebuilds the in-RAM state so
 * that ptest_ubi_step() can continue. Must be called once before the first step and
 * after every reboot. Returns an error only for I/O failures; check failures are
 * reported in res.
 */
esp_err_t ptest_ubi_verify(ptest_ubi_t *t, ptest_ubi_result_t *res);

/**
 * Rewrites one data LEB: the one whose rewrite a cut interrupted, if verify found one,
 * otherwise a random one. Returns the first I/O error, if any.
 */
esp_err_t ptest_ubi_step(ptest_ubi_t *t);

#ifdef __cplusplus
}
#endif
