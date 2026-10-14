/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* Per-boot result records of the power-cut test, kept on the stats volume as a ring of
 * one record per NAND page (no page is programmed twice). The newest valid record
 * carries campaign totals, so older records may be overwritten without losing the
 * result. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_blockdev.h"
#include "esp_err.h"

#define PTEST_STATS_MAGIC 0x31535450u /* "PTS1" */

#define PTEST_FLAG_NEW_CAMPAIGN  (1u << 0)
#define PTEST_FLAG_VERIFY_OK     (1u << 1)
#define PTEST_FLAG_ACCOUNTING_OK (1u << 2)
#define PTEST_FLAG_VTBL_OK       (1u << 3)

typedef struct {
    uint32_t magic;
    uint8_t  campaign[8];      /* first bytes of the app ELF SHA-256 */
    uint32_t boot_no;          /* 1 for the first boot of a campaign */
    uint32_t reset_reason;     /* esp_reset_reason() at this boot */
    uint32_t planned_reset_ms; /* auto-reset delay chosen for this boot, 0 in manual mode */
    uint32_t attach_ms;
    uint32_t verify_ms;
    uint32_t flags;            /* PTEST_FLAG_* */
    uint32_t lebs_matching;
    uint32_t lost;
    uint32_t corrupt;
    uint32_t in_flight;
    uint32_t journal_torn;
    uint32_t free_pebs;
    uint32_t bad_pebs;
    uint32_t mean_ec;
    uint32_t stats_torn;       /* torn stats records found while loading */
    uint32_t total_failures;   /* campaign totals, including this boot */
    uint32_t total_lost;
    uint32_t total_corrupt;
    uint32_t total_in_flight;
    uint32_t crc;
} ptest_stats_rec_t;

typedef struct {
    esp_blockdev_handle_t vol;
    uint32_t page_size;
    uint32_t pages_per_leb;
    uint32_t slots;            /* pages in the whole ring */
    uint32_t next_slot;
    uint32_t torn;
    bool have_last;
    ptest_stats_rec_t last;
} ptest_stats_t;

/* Scans the ring: finds the newest valid record and the slot for the next one. */
esp_err_t ptest_stats_load(ptest_stats_t *s, esp_blockdev_handle_t vol);

/* Fills in magic and CRC, writes rec to the next slot and makes it the newest record. */
esp_err_t ptest_stats_append(ptest_stats_t *s, ptest_stats_rec_t *rec);

/* Prints up to n newest records, oldest first. */
void ptest_stats_print_recent(ptest_stats_t *s, uint32_t n);

const char *ptest_reset_reason_name(uint32_t reason);
