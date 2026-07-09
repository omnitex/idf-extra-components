/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "ptest_ubi.h"

#define DATA_MAGIC 0x31445450u /* "PTD1" */
#define JREC_MAGIC 0x314a5450u /* "PTJ1" */
#define NO_LEB     UINT32_MAX

enum { JREC_SNAPSHOT = 1, JREC_INTENT = 2, JREC_DONE = 3 };

/* Start of every data page; the rest of the page is a PRNG stream seeded from it, so
 * the whole page can be regenerated from (lnum, gen, page). */
typedef struct {
    uint32_t magic;
    uint32_t lnum;
    uint32_t gen;
    uint32_t page;
} data_hdr_t;

/* One journal record per page: header, then for SNAPSHOT `count` generations, then a
 * CRC32 over everything before it. INTENT/DONE use lnum/gen for the LEB being
 * rewritten; SNAPSHOT uses them for the rewrite in progress (lnum = NO_LEB if none). */
typedef struct {
    uint32_t magic;
    uint32_t jseq;
    uint32_t kind;
    uint32_t lnum;
    uint32_t gen;
    uint32_t count;
} jrec_hdr_t;

struct ptest_ubi {
    esp_blockdev_handle_t vol;
    ptest_ubi_cfg_t cfg;
    uint32_t page_size;
    uint32_t leb_size;
    uint32_t pages_per_leb;
    uint32_t pages_per_step;
    uint32_t prng;
    uint32_t *gen;          /* committed generation per data LEB, 0 = never written */
    uint32_t *top;          /* highest generation seen per data LEB, next write uses top+1 */
    uint32_t pending_lnum;  /* LEB whose rewrite has an INTENT but no DONE, or NO_LEB */
    uint32_t pending_gen;
    uint32_t jseq_next;
    uint32_t j_active;      /* journal LEB (0 or 1) that receives the next record */
    uint32_t j_next_page;   /* next free page in the active journal LEB */
    uint8_t *page;
    uint8_t *expect;
};

static uint32_t xorshift32(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void fill_data_page(uint8_t *buf, uint32_t page_size, uint32_t lnum, uint32_t gen, uint32_t page)
{
    data_hdr_t h = { DATA_MAGIC, lnum, gen, page };
    memcpy(buf, &h, sizeof(h));
    uint32_t x = (lnum * 0x9E3779B1u) ^ (gen * 0x85EBCA77u) ^ (page * 0xC2B2AE3Du) ^ 0x27D4EB2Fu;
    if (x == 0) {
        x = 1;
    }
    for (uint32_t i = sizeof(h); i + sizeof(x) <= page_size; i += sizeof(x)) {
        x = xorshift32(x);
        memcpy(buf + i, &x, sizeof(x));
    }
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

static uint64_t leb_addr(const ptest_ubi_t *t, uint32_t lnum)
{
    return (uint64_t)lnum * t->leb_size;
}

static uint64_t journal_page_addr(const ptest_ubi_t *t, uint32_t j, uint32_t page)
{
    return leb_addr(t, t->cfg.data_lebs + j) + (uint64_t)page * t->page_size;
}

static esp_err_t read_page(ptest_ubi_t *t, uint64_t addr, uint8_t *buf)
{
    return t->vol->ops->read(t->vol, buf, t->page_size, addr, t->page_size);
}

static void fail(ptest_ubi_result_t *res, const char *fmt, uint32_t a, uint32_t b, uint32_t c)
{
    if (res->first_error[0] == '\0') {
        snprintf(res->first_error, sizeof(res->first_error), fmt, (unsigned)a, (unsigned)b, (unsigned)c);
    }
}

esp_err_t ptest_ubi_create(esp_blockdev_handle_t vol, const ptest_ubi_cfg_t *cfg, ptest_ubi_t **out)
{
    if (vol == NULL || cfg == NULL || out == NULL || cfg->data_lebs == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t page_size = (uint32_t)vol->geometry.write_size;
    uint32_t leb_size = (uint32_t)vol->geometry.erase_size;
    if (page_size < sizeof(data_hdr_t) + 4 || page_size % 4 != 0 || leb_size % page_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t pages_per_leb = leb_size / page_size;
    uint32_t pages_per_step = cfg->pages_per_step == 0 ? pages_per_leb : cfg->pages_per_step;
    uint64_t needed = (uint64_t)(cfg->data_lebs + PTEST_UBI_JOURNAL_LEBS) * leb_size;
    if (pages_per_step > pages_per_leb || pages_per_leb < 2 || vol->geometry.disk_size < needed ||
            sizeof(jrec_hdr_t) + (uint64_t)cfg->data_lebs * 4 + 4 > page_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    ptest_ubi_t *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return ESP_ERR_NO_MEM;
    }
    t->vol = vol;
    t->cfg = *cfg;
    t->page_size = page_size;
    t->leb_size = leb_size;
    t->pages_per_leb = pages_per_leb;
    t->pages_per_step = pages_per_step;
    t->prng = cfg->seed != 0 ? cfg->seed : 1;
    t->pending_lnum = NO_LEB;
    t->jseq_next = 1;
    t->gen = calloc(cfg->data_lebs, sizeof(*t->gen));
    t->top = calloc(cfg->data_lebs, sizeof(*t->top));
    t->page = malloc(page_size);
    t->expect = malloc(page_size);
    if (t->gen == NULL || t->top == NULL || t->page == NULL || t->expect == NULL) {
        ptest_ubi_destroy(t);
        return ESP_ERR_NO_MEM;
    }
    *out = t;
    return ESP_OK;
}

void ptest_ubi_destroy(ptest_ubi_t *t)
{
    if (t == NULL) {
        return;
    }
    free(t->gen);
    free(t->top);
    free(t->page);
    free(t->expect);
    free(t);
}

void ptest_ubi_set_volume(ptest_ubi_t *t, esp_blockdev_handle_t vol)
{
    t->vol = vol;
}

/* ---- journal ---- */

static bool jrec_parse(const ptest_ubi_t *t, const uint8_t *page, jrec_hdr_t *h)
{
    memcpy(h, page, sizeof(*h));
    if (h->magic != JREC_MAGIC || h->kind < JREC_SNAPSHOT || h->kind > JREC_DONE) {
        return false;
    }
    uint32_t count = h->kind == JREC_SNAPSHOT ? h->count : 0;
    if (h->kind == JREC_SNAPSHOT && count != t->cfg.data_lebs) {
        return false;
    }
    uint32_t len = sizeof(*h) + count * 4;
    if (len + 4 > t->page_size) {
        return false;
    }
    uint32_t crc;
    memcpy(&crc, page + len, sizeof(crc));
    if (crc != esp_rom_crc32_le(0, page, len)) {
        return false;
    }
    if (h->lnum != NO_LEB && h->lnum >= t->cfg.data_lebs) {
        return false;
    }
    return h->kind == JREC_SNAPSHOT || h->lnum != NO_LEB;
}

static esp_err_t jrec_program(ptest_ubi_t *t, uint32_t j, uint32_t page_idx, uint32_t kind,
                              uint32_t lnum, uint32_t gen)
{
    jrec_hdr_t h = { JREC_MAGIC, t->jseq_next, kind, lnum, gen, kind == JREC_SNAPSHOT ? t->cfg.data_lebs : 0 };
    memset(t->page, 0xFF, t->page_size);
    memcpy(t->page, &h, sizeof(h));
    uint32_t len = sizeof(h);
    if (kind == JREC_SNAPSHOT) {
        memcpy(t->page + len, t->gen, t->cfg.data_lebs * 4);
        len += t->cfg.data_lebs * 4;
    }
    uint32_t crc = esp_rom_crc32_le(0, t->page, len);
    memcpy(t->page + len, &crc, sizeof(crc));
    esp_err_t ret = t->vol->ops->write(t->vol, t->page, journal_page_addr(t, j, page_idx), t->page_size);
    t->jseq_next++;
    return ret;
}

/* Appends one record. When the active journal LEB is full, erases the other one and
 * starts it with a SNAPSHOT of the committed state, so one journal LEB always holds a
 * complete history even if a cut lands during the switch. */
static esp_err_t journal_append(ptest_ubi_t *t, uint32_t kind, uint32_t lnum, uint32_t gen)
{
    if (t->j_next_page >= t->pages_per_leb) {
        uint32_t other = t->j_active ^ 1u;
        esp_err_t ret = t->vol->ops->erase(t->vol, leb_addr(t, t->cfg.data_lebs + other), t->leb_size);
        if (ret == ESP_OK) {
            ret = jrec_program(t, other, 0, JREC_SNAPSHOT, t->pending_lnum, t->pending_gen);
        }
        if (ret != ESP_OK) {
            return ret;
        }
        t->j_active = other;
        t->j_next_page = 1;
    }
    esp_err_t ret = jrec_program(t, t->j_active, t->j_next_page, kind, lnum, gen);
    t->j_next_page++;
    return ret;
}

typedef struct {
    bool     has_valid;
    uint32_t first_jseq;
    uint32_t last_jseq;
    uint32_t used_pages;  /* non-blank pages, torn ones included */
} journal_info_t;

/* Scans one journal LEB. With apply set, also replays its records into t. */
static esp_err_t journal_scan(ptest_ubi_t *t, uint32_t j, bool apply, journal_info_t *info,
                              ptest_ubi_result_t *res)
{
    memset(info, 0, sizeof(*info));
    for (uint32_t p = 0; p < t->pages_per_leb; p++) {
        esp_err_t ret = read_page(t, journal_page_addr(t, j, p), t->page);
        if (ret != ESP_OK) {
            return ret;
        }
        if (is_blank(t->page, t->page_size)) {
            continue;
        }
        info->used_pages = p + 1;
        jrec_hdr_t h;
        if (!jrec_parse(t, t->page, &h)) {
            /* Each cut can leave one torn record; writing resumes on the next page. */
            if (!apply) {
                res->journal_torn++;
            }
            continue;
        }
        if (!apply) {
            if (info->has_valid && h.jseq <= info->last_jseq) {
                res->corrupt++;
                fail(res, "journal %u: jseq %u not increasing at page %u", j, h.jseq, p);
            }
            res->journal_records++;
        }
        if (!info->has_valid) {
            info->first_jseq = h.jseq;
        }
        info->has_valid = true;
        info->last_jseq = h.jseq;
        if (!apply) {
            continue;
        }
        switch (h.kind) {
        case JREC_SNAPSHOT:
            memcpy(t->gen, t->page + sizeof(h), t->cfg.data_lebs * 4);
            t->pending_lnum = h.lnum;
            t->pending_gen = h.gen;
            break;
        case JREC_INTENT:
            t->pending_lnum = h.lnum;
            t->pending_gen = h.gen;
            break;
        case JREC_DONE:
            t->gen[h.lnum] = h.gen;
            if (t->pending_lnum == h.lnum && t->pending_gen == h.gen) {
                t->pending_lnum = NO_LEB;
            }
            break;
        default:
            break;
        }
    }
    return ESP_OK;
}

static esp_err_t journal_rebuild(ptest_ubi_t *t, ptest_ubi_result_t *res)
{
    journal_info_t info[PTEST_UBI_JOURNAL_LEBS];
    for (uint32_t j = 0; j < PTEST_UBI_JOURNAL_LEBS; j++) {
        esp_err_t ret = journal_scan(t, j, false, &info[j], res);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    memset(t->gen, 0, t->cfg.data_lebs * sizeof(*t->gen));
    t->pending_lnum = NO_LEB;
    /* Each journal LEB holds a contiguous jseq range; replay the older one first. */
    uint32_t order[2] = { 0, 1 };
    if (info[0].has_valid && info[1].has_valid && info[1].first_jseq < info[0].first_jseq) {
        order[0] = 1;
        order[1] = 0;
    }
    for (uint32_t i = 0; i < 2; i++) {
        journal_info_t again;
        if (info[order[i]].has_valid) {
            esp_err_t ret = journal_scan(t, order[i], true, &again, res);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }

    uint32_t newest = info[1].has_valid && (!info[0].has_valid || info[1].last_jseq > info[0].last_jseq) ? 1 : 0;
    t->j_active = newest;
    t->j_next_page = info[newest].used_pages;
    uint32_t max_jseq = info[0].last_jseq > info[1].last_jseq ? info[0].last_jseq : info[1].last_jseq;
    t->jseq_next = max_jseq + 1;
    return ESP_OK;
}

/* ---- data LEBs ---- */

typedef struct {
    uint32_t gen;      /* generation of page 0, 0 if page 0 is blank or unreadable as data */
    uint32_t valid;    /* leading pages that match (lnum, gen, page) exactly */
    bool     torn;     /* one non-matching, non-blank page right after the valid ones */
    bool     garbage;  /* any non-blank page after that */
} leb_scan_t;

static esp_err_t scan_leb(ptest_ubi_t *t, uint32_t lnum, leb_scan_t *s)
{
    memset(s, 0, sizeof(*s));
    bool ended = false;
    for (uint32_t p = 0; p < t->pages_per_leb; p++) {
        esp_err_t ret = read_page(t, leb_addr(t, lnum) + (uint64_t)p * t->page_size, t->page);
        if (ret != ESP_OK) {
            return ret;
        }
        bool blank = is_blank(t->page, t->page_size);
        if (ended) {
            s->garbage |= !blank;
            continue;
        }
        if (blank) {
            ended = true;
            continue;
        }
        data_hdr_t h;
        memcpy(&h, t->page, sizeof(h));
        uint32_t want_gen = p == 0 ? h.gen : s->gen;
        bool match = false;
        if (h.magic == DATA_MAGIC && h.lnum == lnum && h.page == p && h.gen == want_gen && want_gen != 0) {
            fill_data_page(t->expect, t->page_size, lnum, want_gen, p);
            match = memcmp(t->page, t->expect, t->page_size) == 0;
        }
        if (match) {
            s->gen = want_gen;
            s->valid++;
        } else {
            s->torn = true;
            ended = true;
        }
    }
    return ESP_OK;
}

esp_err_t ptest_ubi_verify(ptest_ubi_t *t, ptest_ubi_result_t *res)
{
    memset(res, 0, sizeof(*res));
    esp_err_t ret = journal_rebuild(t, res);
    if (ret != ESP_OK) {
        return ret;
    }

    for (uint32_t l = 0; l < t->cfg.data_lebs; l++) {
        leb_scan_t s;
        ret = scan_leb(t, l, &s);
        if (ret != ESP_OK) {
            return ret;
        }
        uint32_t committed = t->gen[l];
        bool erased = s.valid == 0 && !s.torn && !s.garbage;
        bool complete = s.gen != 0 && s.valid == t->pages_per_step && !s.torn && !s.garbage;
        bool pending = t->pending_lnum == l;

        uint32_t top = committed > s.gen ? committed : s.gen;
        if (pending && t->pending_gen > top) {
            top = t->pending_gen;
        }
        t->top[l] = top;

        if (committed == 0 ? erased : (complete && s.gen == committed)) {
            res->lebs_matching++;
            continue;
        }
        if (pending) {
            /* The cut hit this LEB's rewrite: it may hold a prefix of any generation after
             * the committed one, up to the pending one. Older uncommitted generations
             * remain when an earlier cut interrupted this LEB and a later cut landed
             * before the redo's erase completed. */
            bool prefix_of_new = !s.garbage &&
                                 (s.valid == 0 || (s.gen > committed && s.gen <= t->pending_gen));
            if (prefix_of_new) {
                res->in_flight = 1;
                continue;
            }
            res->corrupt++;
            fail(res, "LEB %u: in-flight gen %u but found gen %u with stray data", l, t->pending_gen, s.gen);
            continue;
        }
        if (!s.garbage && !s.torn && (erased || (complete && s.gen < committed))) {
            res->lost++;
            fail(res, "LEB %u: committed gen %u lost (found gen %u)", l, committed, s.gen);
        } else {
            res->corrupt++;
            fail(res, "LEB %u: committed gen %u, found gen %u corrupt or mixed", l, committed, s.gen);
        }
    }

    res->ok = res->lost == 0 && res->corrupt == 0;
    return ESP_OK;
}

esp_err_t ptest_ubi_step(ptest_ubi_t *t)
{
    /* A rewrite cut before its DONE is redone first; otherwise the next INTENT would
     * hide which LEB the cut left in an intermediate state. */
    uint32_t lnum;
    if (t->pending_lnum != NO_LEB) {
        lnum = t->pending_lnum;
    } else {
        t->prng = xorshift32(t->prng);
        lnum = t->prng % t->cfg.data_lebs;
    }
    uint32_t gen = t->top[lnum] + 1;

    esp_err_t ret = journal_append(t, JREC_INTENT, lnum, gen);
    if (ret != ESP_OK) {
        return ret;
    }
    t->pending_lnum = lnum;
    t->pending_gen = gen;
    t->top[lnum] = gen;

    ret = t->vol->ops->erase(t->vol, leb_addr(t, lnum), t->leb_size);
    for (uint32_t p = 0; p < t->pages_per_step && ret == ESP_OK; p++) {
        fill_data_page(t->page, t->page_size, lnum, gen, p);
        ret = t->vol->ops->write(t->vol, t->page, leb_addr(t, lnum) + (uint64_t)p * t->page_size, t->page_size);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = journal_append(t, JREC_DONE, lnum, gen);
    if (ret != ESP_OK) {
        return ret;
    }
    t->gen[lnum] = gen;
    t->pending_lnum = NO_LEB;
    return ESP_OK;
}
