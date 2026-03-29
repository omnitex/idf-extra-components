/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Sparse hash metadata backend.
 *
 * Stores per-block and per-page metadata in two separate hash tables
 * (block_table and page_table).  Only blocks/pages that have actually been
 * erased or programmed/read consume memory, making this efficient for large
 * flash devices in host-side tests.
 *
 * Erase-fold semantics:
 *   On block erase, for every page in the block the backend:
 *     1. Adds page.program_count -> page.program_count_total
 *     2. Adds page.read_count    -> page.read_count_total
 *     3. Resets page.program_count and page.read_count to 0
 *
 * The page_table key is the absolute page number.
 * The block_table key is the block number.
 */

#include "nand_emul_advanced.h"
#include "backends/hash_table.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal context
 * ---------------------------------------------------------------------- */

typedef struct {
    hash_table_t *block_table;  /* key: block_num,  value: block_metadata_t */
    hash_table_t *page_table;   /* key: page_num,   value: page_metadata_t  */
    uint32_t      pages_per_block; /* cached; set via on_block_erase first call */
} sparse_hash_ctx_t;

/* Default capacity / load factor when caller passes NULL config */
#define DEFAULT_INITIAL_CAPACITY 16u
#define DEFAULT_LOAD_FACTOR      0.75f

/* -------------------------------------------------------------------------
 * Helper: fold all pages in a block (called during on_block_erase)
 * ---------------------------------------------------------------------- */

typedef struct {
    hash_table_t *page_table;
    uint32_t      block_num;
    uint32_t      pages_per_block;
} fold_ctx_t;

static bool fold_page_node(hash_node_t *node, void *user_data)
{
    fold_ctx_t *fc = (fold_ctx_t *)user_data;
    page_metadata_t *pm = (page_metadata_t *)node->data;
    if (pm->page_num / fc->pages_per_block == fc->block_num) {
        pm->program_count_total += pm->program_count;
        pm->program_count        = 0;
        pm->read_count_total    += pm->read_count;
        pm->read_count           = 0;
    }
    return true; /* continue iteration */
}

/* -------------------------------------------------------------------------
 * Backend vtable implementation
 * ---------------------------------------------------------------------- */

static esp_err_t sparse_init(void **handle_out, const void *config)
{
    const sparse_hash_backend_config_t *cfg = (const sparse_hash_backend_config_t *)config;

    size_t cap = cfg ? cfg->initial_capacity : DEFAULT_INITIAL_CAPACITY;
    float  lf  = (cfg && cfg->load_factor > 0.0f) ? cfg->load_factor : DEFAULT_LOAD_FACTOR;
    if (cap == 0) {
        cap = DEFAULT_INITIAL_CAPACITY;
    }

    sparse_hash_ctx_t *ctx = calloc(1, sizeof(sparse_hash_ctx_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    ctx->block_table = hash_table_create(cap, sizeof(block_metadata_t), lf);
    if (!ctx->block_table) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    ctx->page_table = hash_table_create(cap, sizeof(page_metadata_t), lf);
    if (!ctx->page_table) {
        hash_table_destroy(ctx->block_table);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    ctx->pages_per_block = cfg ? cfg->pages_per_block : 0;
    *handle_out = ctx;
    return ESP_OK;
}

static esp_err_t sparse_deinit(void *handle)
{
    sparse_hash_ctx_t *ctx = (sparse_hash_ctx_t *)handle;
    if (!ctx) {
        return ESP_OK;
    }
    hash_table_destroy(ctx->block_table);
    hash_table_destroy(ctx->page_table);
    free(ctx);
    return ESP_OK;
}

static esp_err_t sparse_on_block_erase(void *handle, uint32_t block_num, uint64_t timestamp)
{
    sparse_hash_ctx_t *ctx = (sparse_hash_ctx_t *)handle;
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    hash_node_t *node = hash_table_get_or_insert(ctx->block_table, block_num);
    if (!node) {
        return ESP_ERR_NO_MEM;
    }

    block_metadata_t *bm = (block_metadata_t *)node->data;

    /* Set block_num on first insertion (node->data is zero-initialised) */
    bm->block_num = block_num;

    if (bm->erase_count == 0) {
        bm->first_erase_timestamp = timestamp;
    }
    bm->last_erase_timestamp = timestamp;
    bm->erase_count++;

    /* Fold page counts for all pages in this block */
    if (ctx->pages_per_block > 0) {
        fold_ctx_t fc = {
            .page_table      = ctx->page_table,
            .block_num       = block_num,
            .pages_per_block = ctx->pages_per_block,
        };
        hash_table_iterate(ctx->page_table, fold_page_node, &fc);
    }

    return ESP_OK;
}

static esp_err_t sparse_on_page_program(void *handle, uint32_t page_num, uint64_t timestamp)
{
    sparse_hash_ctx_t *ctx = (sparse_hash_ctx_t *)handle;
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    hash_node_t *node = hash_table_get_or_insert(ctx->page_table, page_num);
    if (!node) {
        return ESP_ERR_NO_MEM;
    }

    page_metadata_t *pm = (page_metadata_t *)node->data;
    pm->page_num = page_num;

    if (pm->program_count == 0) {
        pm->first_program_timestamp = timestamp;
    }
    pm->last_program_timestamp = timestamp;
    pm->program_count++;

    return ESP_OK;
}

static esp_err_t sparse_on_page_read(void *handle, uint32_t page_num, uint64_t timestamp)
{
    (void)timestamp;
    sparse_hash_ctx_t *ctx = (sparse_hash_ctx_t *)handle;
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    hash_node_t *node = hash_table_get_or_insert(ctx->page_table, page_num);
    if (!node) {
        return ESP_ERR_NO_MEM;
    }

    page_metadata_t *pm = (page_metadata_t *)node->data;
    pm->page_num = page_num;
    pm->read_count++;

    return ESP_OK;
}

static esp_err_t sparse_get_block_info(void *handle, uint32_t block_num, block_metadata_t *out)
{
    sparse_hash_ctx_t *ctx = (sparse_hash_ctx_t *)handle;
    /* out is already zeroed and block_num set by the caller (nand_emul_advanced.c) */
    if (!ctx) {
        return ESP_OK;
    }

    hash_node_t *node = hash_table_get(ctx->block_table, block_num);
    if (node) {
        memcpy(out, node->data, sizeof(block_metadata_t));
    }
    /* If not found, leave out as zeros (never erased) */
    return ESP_OK;
}

static esp_err_t sparse_get_page_info(void *handle, uint32_t page_num, page_metadata_t *out)
{
    sparse_hash_ctx_t *ctx = (sparse_hash_ctx_t *)handle;
    if (!ctx) {
        return ESP_OK;
    }

    hash_node_t *node = hash_table_get(ctx->page_table, page_num);
    if (node) {
        memcpy(out, node->data, sizeof(page_metadata_t));
    }
    return ESP_OK;
}

static esp_err_t sparse_set_bad_block(void *handle, uint32_t block_num, bool is_bad)
{
    sparse_hash_ctx_t *ctx = (sparse_hash_ctx_t *)handle;
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    hash_node_t *node = hash_table_get_or_insert(ctx->block_table, block_num);
    if (!node) {
        return ESP_ERR_NO_MEM;
    }

    block_metadata_t *bm = (block_metadata_t *)node->data;
    bm->block_num  = block_num;
    bm->is_bad_block = is_bad;
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Iteration helpers
 * ---------------------------------------------------------------------- */

typedef struct {
    bool (*user_cb)(uint32_t block_num, block_metadata_t *meta, void *user_data);
    void *user_data;
} block_iter_adapter_t;

static bool block_iter_node(hash_node_t *node, void *user_data)
{
    block_iter_adapter_t *a = (block_iter_adapter_t *)user_data;
    block_metadata_t *bm = (block_metadata_t *)node->data;
    return a->user_cb(bm->block_num, bm, a->user_data);
}

static esp_err_t sparse_iterate_blocks(void *handle,
                                        bool (*callback)(uint32_t block_num,
                                                         block_metadata_t *meta,
                                                         void *user_data),
                                        void *user_data)
{
    sparse_hash_ctx_t *ctx = (sparse_hash_ctx_t *)handle;
    if (!ctx || !callback) {
        return ESP_ERR_INVALID_ARG;
    }
    block_iter_adapter_t a = { .user_cb = callback, .user_data = user_data };
    hash_table_iterate(ctx->block_table, block_iter_node, &a);
    return ESP_OK;
}

typedef struct {
    bool (*user_cb)(uint32_t page_num, page_metadata_t *meta, void *user_data);
    void *user_data;
} page_iter_adapter_t;

static bool page_iter_node(hash_node_t *node, void *user_data)
{
    page_iter_adapter_t *a = (page_iter_adapter_t *)user_data;
    page_metadata_t *pm = (page_metadata_t *)node->data;
    return a->user_cb(pm->page_num, pm, a->user_data);
}

static esp_err_t sparse_iterate_pages(void *handle,
                                       bool (*callback)(uint32_t page_num,
                                                        page_metadata_t *meta,
                                                        void *user_data),
                                       void *user_data)
{
    sparse_hash_ctx_t *ctx = (sparse_hash_ctx_t *)handle;
    if (!ctx || !callback) {
        return ESP_ERR_INVALID_ARG;
    }
    page_iter_adapter_t a = { .user_cb = callback, .user_data = user_data };
    hash_table_iterate(ctx->page_table, page_iter_node, &a);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Aggregate statistics
 * ---------------------------------------------------------------------- */

typedef struct {
    uint32_t min_erases;
    uint32_t max_erases;
    uint64_t sum_erases;
    uint32_t count;
} stats_acc_t;

static bool stats_block_node(hash_node_t *node, void *user_data)
{
    stats_acc_t *acc = (stats_acc_t *)user_data;
    block_metadata_t *bm = (block_metadata_t *)node->data;
    uint32_t e = bm->erase_count;
    if (acc->count == 0 || e < acc->min_erases) {
        acc->min_erases = e;
    }
    if (e > acc->max_erases) {
        acc->max_erases = e;
    }
    acc->sum_erases += e;
    acc->count++;
    return true;
}

static esp_err_t sparse_get_stats(void *handle, nand_wear_stats_t *out)
{
    sparse_hash_ctx_t *ctx = (sparse_hash_ctx_t *)handle;
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    stats_acc_t acc = { 0 };
    hash_table_iterate(ctx->block_table, stats_block_node, &acc);

    if (acc.count == 0) {
        out->min_block_erases       = 0;
        out->max_block_erases       = 0;
        out->avg_block_erases       = 0;
        out->wear_leveling_variation = 0.0;
    } else {
        uint32_t avg = (uint32_t)(acc.sum_erases / acc.count);
        out->min_block_erases       = acc.min_erases;
        out->max_block_erases       = acc.max_erases;
        out->avg_block_erases       = avg;
        out->wear_leveling_variation = (avg > 0)
            ? (double)(acc.max_erases - acc.min_erases) / (double)avg
            : 0.0;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Public vtable
 * ---------------------------------------------------------------------- */

const nand_metadata_backend_ops_t nand_sparse_hash_backend = {
    .init           = sparse_init,
    .deinit         = sparse_deinit,
    .on_block_erase = sparse_on_block_erase,
    .on_page_program = sparse_on_page_program,
    .on_page_read   = sparse_on_page_read,
    .get_block_info = sparse_get_block_info,
    .get_page_info  = sparse_get_page_info,
    .set_bad_block  = sparse_set_bad_block,
    .iterate_blocks = sparse_iterate_blocks,
    .iterate_pages  = sparse_iterate_pages,
    .get_stats      = sparse_get_stats,
    .get_histograms = NULL,
    .save_snapshot  = NULL,
    .load_snapshot  = NULL,
    .export_json    = NULL,
};
