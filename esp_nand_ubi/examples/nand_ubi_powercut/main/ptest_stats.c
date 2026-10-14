/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "esp_system.h"
#include "ptest_stats.h"

static uint32_t rec_crc(const ptest_stats_rec_t *r)
{
    return esp_rom_crc32_le(0, (const uint8_t *)r, offsetof(ptest_stats_rec_t, crc));
}

static bool is_blank(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static uint64_t slot_addr(const ptest_stats_t *s, uint32_t slot)
{
    return (uint64_t)slot * s->page_size;
}

/* Reads one slot. Returns 1 for a valid record, 0 for a blank page, -1 for a torn one. */
static int read_slot(ptest_stats_t *s, uint32_t slot, uint8_t *page, ptest_stats_rec_t *rec, esp_err_t *err)
{
    *err = s->vol->ops->read(s->vol, page, s->page_size, slot_addr(s, slot), s->page_size);
    if (*err != ESP_OK) {
        return -1;
    }
    if (is_blank(page, s->page_size)) {
        return 0;
    }
    memcpy(rec, page, sizeof(*rec));
    return rec->magic == PTEST_STATS_MAGIC && rec->crc == rec_crc(rec) ? 1 : -1;
}

esp_err_t ptest_stats_load(ptest_stats_t *s, esp_blockdev_handle_t vol)
{
    memset(s, 0, sizeof(*s));
    s->vol = vol;
    s->page_size = (uint32_t)vol->geometry.write_size;
    s->pages_per_leb = (uint32_t)(vol->geometry.erase_size / s->page_size);
    s->slots = (uint32_t)(vol->geometry.disk_size / s->page_size);
    if (s->page_size < sizeof(ptest_stats_rec_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *page = malloc(s->page_size);
    if (page == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t newest_slot = 0;
    uint32_t first_blank = UINT32_MAX;
    esp_err_t err = ESP_OK;
    for (uint32_t slot = 0; slot < s->slots; slot++) {
        ptest_stats_rec_t rec;
        int kind = read_slot(s, slot, page, &rec, &err);
        if (err != ESP_OK) {
            break;
        }
        if (kind == 0 && first_blank == UINT32_MAX) {
            first_blank = slot;
        } else if (kind < 0) {
            s->torn++;
        } else if (kind > 0 && (!s->have_last || rec.boot_no > s->last.boot_no)) {
            s->have_last = true;
            s->last = rec;
            newest_slot = slot;
        }
    }

    if (err == ESP_OK) {
        if (s->have_last) {
            s->next_slot = (newest_slot + 1) % s->slots;
        } else {
            s->next_slot = first_blank == UINT32_MAX ? 0 : first_blank;
        }
        /* Skip torn pages left by a cut during an earlier append; a page may only be
         * programmed once until its LEB is erased. The LEB is erased when the ring
         * reaches its first page. */
        while (s->next_slot % s->pages_per_leb != 0) {
            ptest_stats_rec_t rec;
            int kind = read_slot(s, s->next_slot, page, &rec, &err);
            if (err != ESP_OK || kind == 0) {
                break;
            }
            s->next_slot = (s->next_slot + 1) % s->slots;
        }
    }
    free(page);
    return err;
}

esp_err_t ptest_stats_append(ptest_stats_t *s, ptest_stats_rec_t *rec)
{
    rec->magic = PTEST_STATS_MAGIC;
    rec->crc = rec_crc(rec);
    uint32_t slot = s->next_slot;
    if (slot % s->pages_per_leb == 0) {
        uint64_t leb_size = (uint64_t)s->pages_per_leb * s->page_size;
        esp_err_t err = s->vol->ops->erase(s->vol, slot_addr(s, slot), (size_t)leb_size);
        if (err != ESP_OK) {
            return err;
        }
    }
    uint8_t *page = malloc(s->page_size);
    if (page == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(page, 0xFF, s->page_size);
    memcpy(page, rec, sizeof(*rec));
    esp_err_t err = s->vol->ops->write(s->vol, page, slot_addr(s, slot), s->page_size);
    free(page);
    if (err == ESP_OK) {
        s->last = *rec;
        s->have_last = true;
        s->next_slot = (slot + 1) % s->slots;
    }
    return err;
}

const char *ptest_reset_reason_name(uint32_t reason)
{
    switch ((esp_reset_reason_t)reason) {
    case ESP_RST_POWERON:
        return "POWERON";
    case ESP_RST_EXT:
        return "EXT";
    case ESP_RST_SW:
        return "SW";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "INT_WDT";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT";
    case ESP_RST_WDT:
        return "WDT";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT";
    case ESP_RST_USB:
        return "USB";
    default:
        return "OTHER";
    }
}

void ptest_stats_print_recent(ptest_stats_t *s, uint32_t n)
{
    ptest_stats_rec_t *best = calloc(n, sizeof(*best));
    uint8_t *page = malloc(s->page_size);
    if (best == NULL || page == NULL || n == 0) {
        free(best);
        free(page);
        return;
    }
    uint32_t have = 0;
    for (uint32_t slot = 0; slot < s->slots; slot++) {
        ptest_stats_rec_t rec;
        esp_err_t err;
        if (read_slot(s, slot, page, &rec, &err) != 1) {
            continue;
        }
        /* Keep the n highest boot numbers, sorted ascending; best[0] is the lowest kept. */
        if (have < n) {
            best[have++] = rec;
        } else if (rec.boot_no > best[0].boot_no) {
            best[0] = rec;
        } else {
            continue;
        }
        for (uint32_t i = 1; i < have; i++) {
            ptest_stats_rec_t tmp = best[i];
            uint32_t j = i;
            while (j > 0 && best[j - 1].boot_no > tmp.boot_no) {
                best[j] = best[j - 1];
                j--;
            }
            best[j] = tmp;
        }
    }
    printf(" boot reset     next_ms attach_ms verify_ms match lost corr inflt jtorn  free bad mean_ec\n");
    for (uint32_t i = 0; i < have; i++) {
        const ptest_stats_rec_t *r = &best[i];
        printf("%5" PRIu32 " %-8s %8" PRIu32 " %9" PRIu32 " %9" PRIu32 " %5" PRIu32 " %4" PRIu32 " %4" PRIu32
               " %5" PRIu32 " %5" PRIu32 " %5" PRIu32 " %3" PRIu32 " %7" PRIu32 "%s\n",
               r->boot_no, ptest_reset_reason_name(r->reset_reason), r->planned_reset_ms, r->attach_ms,
               r->verify_ms, r->lebs_matching, r->lost, r->corrupt, r->in_flight, r->journal_torn,
               r->free_pebs, r->bad_pebs, r->mean_ec, (r->flags & PTEST_FLAG_VERIFY_OK) ? "" : "  FAIL");
    }
    free(best);
    free(page);
}
