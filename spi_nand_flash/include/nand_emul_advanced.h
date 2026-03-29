/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Core metadata types
 * -------------------------------------------------------------------------*/

/**
 * @brief Per-page wear and program/read tracking metadata.
 *
 * program_count and read_count track activity in the CURRENT erase cycle and
 * are reset to 0 on block erase.  The _total variants accumulate across the
 * lifetime of the device and are never reset.
 *
 * Lifetime programs = program_count_total + program_count.
 * Lifetime reads    = read_count_total    + read_count.
 */
typedef struct {
    uint32_t page_num;                  /**< Page number (for iteration / hashing) */
    uint32_t program_count;             /**< Programs in current erase cycle (reset on block erase) */
    uint32_t program_count_total;       /**< Lifetime cumulative programs (never reset) */
    uint32_t read_count;                /**< Successful reads in current erase cycle (reset on block erase) */
    uint32_t read_count_total;          /**< Lifetime cumulative reads (never reset) */
    uint32_t _reserved;                 /**< Padding / future use */
    uint64_t first_program_timestamp;   /**< Timestamp of first program in current erase cycle */
    uint64_t last_program_timestamp;    /**< Timestamp of most recent program in current erase cycle */
} page_metadata_t;

/**
 * @brief Per-block erase and wear tracking metadata.
 *
 * total_page_programs is reset on each erase; total_page_programs_total
 * accumulates across the device lifetime.
 */
typedef struct {
    uint32_t block_num;                     /**< Block number (for iteration / hashing) */
    uint32_t erase_count;                   /**< Number of times this block has been erased */
    uint64_t first_erase_timestamp;         /**< Timestamp of the first erase */
    uint64_t last_erase_timestamp;          /**< Timestamp of the most recent erase */
    uint32_t total_page_programs;           /**< Page programs in current erase cycle (reset on erase) */
    uint32_t total_page_programs_total;     /**< Lifetime cumulative page programs (never reset) */
    bool     is_bad_block;                  /**< Simulated bad block flag */
    uint8_t  _padding[3];                   /**< Explicit padding */
} block_metadata_t;

/**
 * @brief Aggregate wear-leveling statistics across all tracked blocks/pages.
 */
typedef struct {
    uint32_t total_blocks;              /**< Total blocks in the flash device */
    uint32_t total_pages;               /**< Total pages in the flash device */
    uint64_t total_bytes_written;       /**< Cumulative bytes successfully programmed */
    uint32_t min_block_erases;          /**< Minimum erase count across all tracked blocks */
    uint32_t max_block_erases;          /**< Maximum erase count across all tracked blocks */
    uint32_t avg_block_erases;          /**< Average erase count across all tracked blocks */
    uint32_t min_page_programs;         /**< Minimum lifetime programs across tracked pages */
    uint32_t max_page_programs;         /**< Maximum lifetime programs across tracked pages */
    uint32_t min_page_reads;            /**< Min lifetime reads among pages with metadata */
    uint32_t max_page_reads;            /**< Max lifetime reads among pages with metadata */
    uint32_t blocks_never_erased;       /**< Blocks that have never been erased */
    uint32_t pages_never_written;       /**< Pages that have never been programmed */
    double   wear_leveling_variation;   /**< (max - min) / avg erase counts; 0.0 if avg == 0 */

    /** @brief Sum of nbytes from nand_emul_record_logical_write(); 0 if never called. */
    uint64_t logical_write_bytes_recorded;
    /** @brief total_bytes_written / logical_write_bytes_recorded when logical > 0; else 0.0. */
    double   write_amplification;
} nand_wear_stats_t;

/**
 * @brief Context passed to failure-model callbacks describing the current operation.
 */
typedef struct {
    uint32_t block_num;             /**< Block number being operated on */
    uint32_t page_num;              /**< Page number being operated on */
    size_t   byte_offset;           /**< Byte offset within the page */
    size_t   operation_size;        /**< Number of bytes in the operation */
    uint64_t timestamp;             /**< Current timestamp */
    uint32_t total_blocks;          /**< Total blocks in the device */
    uint32_t pages_per_block;       /**< Pages per block */
    uint32_t page_size;             /**< Page size in bytes */
    const block_metadata_t *block_meta; /**< Block metadata at operation time (may be NULL) */
    const page_metadata_t  *page_meta;  /**< Page metadata at operation time (may be NULL) */
} nand_operation_context_t;

/* ---------------------------------------------------------------------------
 * Histogram types (optional analytics, computed on demand)
 * -------------------------------------------------------------------------*/

/** Maximum number of histogram bins. */
#define NAND_WEAR_HIST_MAX_BINS 32

/**
 * @brief Uniform linear histogram with one overflow bucket.
 *
 * Caller sets @c n_bins (>= 2, <= NAND_WEAR_HIST_MAX_BINS) and @c bin_width
 * (> 0) before calling the query function.  For sample value @c v, bin @c i
 * (0 <= i < n_bins - 1) counts samples where @c i*bin_width <= v < (i+1)*bin_width;
 * bin @c n_bins-1 counts @c v >= (n_bins-1)*bin_width.
 */
typedef struct {
    uint32_t bin_width;                         /**< Width of each bin (caller-set before query) */
    uint16_t n_bins;                            /**< Number of bins including overflow (caller-set before query) */
    uint16_t reserved;                          /**< Padding */
    uint64_t count[NAND_WEAR_HIST_MAX_BINS];    /**< Per-bin counts, filled by the query function */
} nand_wear_histogram_t;

/**
 * @brief Pair of histograms filled by nand_emul_get_wear_histograms().
 */
typedef struct {
    nand_wear_histogram_t block_erase_count;        /**< Samples: erase_count per block in block table */
    nand_wear_histogram_t page_lifetime_programs;   /**< Samples: lifetime programs per written page */
} nand_wear_histograms_t;

/* ---------------------------------------------------------------------------
 * Snapshot header format (64 bytes, version 1)
 * -------------------------------------------------------------------------*/

/**
 * @brief Binary snapshot file header.
 *
 * Must be exactly 64 bytes.  CRC32 covers bytes 0..59 (header only,
 * excluding the checksum field).
 */
typedef struct {
    uint32_t magic;                 /**< 0x4E414E44 ("NAND") */
    uint8_t  version;               /**< Snapshot format version (1) */
    uint8_t  flags;                 /**< Bit 0: page tracking, Bit 1: block tracking */
    uint16_t reserved0;             /**< Reserved, must be 0 */
    uint64_t timestamp;             /**< Snapshot creation timestamp */
    uint32_t total_blocks;          /**< Total blocks in the flash device */
    uint32_t pages_per_block;       /**< Pages per block */
    uint32_t page_size;             /**< Page size in bytes */
    uint32_t block_metadata_count;  /**< Number of block metadata records */
    uint32_t page_metadata_count;   /**< Number of page metadata records */
    uint32_t reserved1;             /**< Explicit padding to align uint64_t fields */
    uint64_t block_metadata_offset; /**< File offset of block metadata section */
    uint64_t page_metadata_offset;  /**< File offset of page metadata section */
    uint32_t reserved2;             /**< Reserved, must be 0 */
    uint32_t checksum;              /**< CRC32 of header bytes 0..59 (this field excluded) */
} snapshot_header_t;

_Static_assert(sizeof(snapshot_header_t) == 64,
               "snapshot_header_t must be exactly 64 bytes");

/* ---------------------------------------------------------------------------
 * Metadata backend interface (vtable)
 * -------------------------------------------------------------------------*/

/**
 * @brief Vtable for metadata storage backends.
 *
 * Optional function pointers may be NULL; the core checks for NULL before
 * calling them.  A no-op backend that returns ESP_OK for everything is
 * provided via @c nand_noop_backend.
 */
typedef struct {
    /** Initialize the backend.  Returns ESP_OK on success. */
    esp_err_t (*init)(void **backend_handle, const void *config);

    /** Release all resources held by the backend. */
    esp_err_t (*deinit)(void *backend_handle);

    /** Called after a block erase completes successfully. */
    esp_err_t (*on_block_erase)(void *backend_handle, uint32_t block_num, uint64_t timestamp);

    /** Called after a page program completes successfully. */
    esp_err_t (*on_page_program)(void *backend_handle, uint32_t page_num, uint64_t timestamp);

    /**
     * Called after a successful read, once per page overlapped by the read.
     * Optional — set to NULL if read tracking is not supported.
     */
    esp_err_t (*on_page_read)(void *backend_handle, uint32_t page_num, uint64_t timestamp);

    /**
     * Query block metadata.  If the block was never erased, zeros are returned
     * and the function still returns ESP_OK.
     */
    esp_err_t (*get_block_info)(void *backend_handle, uint32_t block_num, block_metadata_t *out);

    /**
     * Query page metadata.  If the page was never programmed, zeros are returned
     * and the function still returns ESP_OK.
     */
    esp_err_t (*get_page_info)(void *backend_handle, uint32_t page_num, page_metadata_t *out);

    /** Mark (or unmark) a block as bad. */
    esp_err_t (*set_bad_block)(void *backend_handle, uint32_t block_num, bool is_bad);

    /**
     * Iterate over all tracked blocks.  The callback receives the block number
     * and metadata; returning false stops iteration.
     */
    esp_err_t (*iterate_blocks)(void *backend_handle,
                                bool (*callback)(uint32_t block_num, block_metadata_t *meta, void *user_data),
                                void *user_data);

    /**
     * Iterate over all tracked pages.  The callback receives the page number
     * and metadata; returning false stops iteration.
     */
    esp_err_t (*iterate_pages)(void *backend_handle,
                               bool (*callback)(uint32_t page_num, page_metadata_t *meta, void *user_data),
                               void *user_data);

    /** Return aggregate statistics.  May be NULL if not supported. */
    esp_err_t (*get_stats)(void *backend_handle, nand_wear_stats_t *out);

    /**
     * Fill wear histograms.  Optional — NULL means not supported.
     * Caller sets bin_width and n_bins in each sub-histogram before calling.
     */
    esp_err_t (*get_histograms)(void *backend_handle, nand_wear_histograms_t *out);

    /** Save current metadata state to a binary snapshot file. */
    esp_err_t (*save_snapshot)(void *backend_handle, const char *filename, uint64_t timestamp);

    /** Load metadata state from a previously saved binary snapshot file. */
    esp_err_t (*load_snapshot)(void *backend_handle, const char *filename);

    /** Export current metadata to a JSON file. */
    esp_err_t (*export_json)(void *backend_handle, const char *filename);
} nand_metadata_backend_ops_t;

/* ---------------------------------------------------------------------------
 * Failure model interface (vtable)
 * -------------------------------------------------------------------------*/

/**
 * @brief Vtable for pluggable failure injection models.
 *
 * A no-op model that never injects failures is provided via
 * @c nand_noop_failure_model.
 */
typedef struct {
    /** Initialize the failure model.  Returns ESP_OK on success. */
    esp_err_t (*init)(void **model_handle, const void *config);

    /** Release all resources held by the model. */
    esp_err_t (*deinit)(void *model_handle);

    /** Return true if the read operation at @p ctx should fail. */
    bool (*should_fail_read)(void *model_handle, const nand_operation_context_t *ctx);

    /** Return true if the write operation at @p ctx should fail. */
    bool (*should_fail_write)(void *model_handle, const nand_operation_context_t *ctx);

    /** Return true if the erase operation at @p ctx should fail. */
    bool (*should_fail_erase)(void *model_handle, const nand_operation_context_t *ctx);

    /**
     * Optionally corrupt the read data buffer in-place (e.g. inject bit flips).
     * Called after a successful read.  May be a no-op.
     * @p data points to the read buffer; @p len is the number of bytes read.
     * The model SHALL NOT write outside [0, len) and SHALL NOT change len.
     */
    void (*corrupt_read_data)(void *model_handle, const nand_operation_context_t *ctx,
                              uint8_t *data, size_t len);

    /**
     * Return true if the block at @p block_num should be treated as bad.
     * When this returns true, the core SHALL call backend set_bad_block() and
     * return ESP_ERR_FLASH_BAD_BLOCK without modifying flash contents.
     */
    bool (*is_block_bad)(void *model_handle, uint32_t block_num, const block_metadata_t *meta);
} nand_failure_model_ops_t;

/* ---------------------------------------------------------------------------
 * Advanced emulator configuration
 * -------------------------------------------------------------------------*/

/**
 * @brief Configuration for the advanced NAND emulator.
 *
 * Pass to nand_emul_advanced_init() instead of spi_nand_flash_init_device().
 */
typedef struct {
    /** Base file-mapped emulator configuration (required). */
    nand_file_mmap_emul_config_t base_config;

    /** Pointer to metadata backend vtable (NULL to disable tracking). */
    const nand_metadata_backend_ops_t *metadata_backend;

    /** Config blob passed verbatim to metadata_backend->init() (may be NULL). */
    void *metadata_backend_config;

    /** Pointer to failure model vtable (NULL to disable failure injection). */
    const nand_failure_model_ops_t *failure_model;

    /** Config blob passed verbatim to failure_model->init() (may be NULL). */
    void *failure_model_config;

    /** Enable block-level erase tracking. */
    bool track_block_level;

    /** Enable page-level program/read tracking. */
    bool track_page_level;

    /**
     * Optional user-supplied timestamp function.
     * If NULL, a monotonic counter is used.
     */
    uint64_t (*get_timestamp)(void);
} nand_emul_advanced_config_t;

/* ---------------------------------------------------------------------------
 * Built-in no-op backend and failure model
 * -------------------------------------------------------------------------*/

/** No-op metadata backend: all ops return ESP_OK; no state is stored. */
extern const nand_metadata_backend_ops_t nand_noop_backend;

/** No-op failure model: never fails anything; corrupt_read_data is a no-op. */
extern const nand_failure_model_ops_t nand_noop_failure_model;

/* ---------------------------------------------------------------------------
 * Sparse hash backend
 * -------------------------------------------------------------------------*/

/**
 * @brief Configuration for the sparse hash metadata backend.
 *
 * Pass as metadata_backend_config in nand_emul_advanced_config_t when
 * using &nand_sparse_hash_backend as the metadata_backend.
 */
typedef struct {
    /** Initial hash table capacity (rounded up to next power of two). */
    size_t initial_capacity;
    /** Load-factor threshold at which the table rehashes (e.g. 0.75f). */
    float  load_factor;
    /** Reserved for future histogram query support (ignored for now). */
    bool   enable_histogram_query;
    /**
     * Pages per block — populated automatically by nand_emul_advanced_init()
     * from the device geometry.  Caller should leave this as 0; it is set
     * before backend->init() is called.
     */
    uint32_t pages_per_block;
} sparse_hash_backend_config_t;

/**
 * @brief Sparse hash metadata backend.
 *
 * Stores per-block and per-page metadata in two separate hash tables.
 * Only blocks/pages that have actually been erased or programmed consume
 * memory, making this suitable for large flash devices in host tests.
 */
extern const nand_metadata_backend_ops_t nand_sparse_hash_backend;

/* ---------------------------------------------------------------------------
 * Advanced emulator lifecycle API
 * -------------------------------------------------------------------------*/

/**
 * @brief Initialize the advanced NAND flash emulator.
 *
 * Calls the base spi_nand_flash_init_device(), allocates the advanced context,
 * caches device geometry, and calls backend/failure-model init() if provided.
 *
 * @param[out] dev_out  Receives the initialized device handle.
 * @param[in]  cfg      Advanced configuration (must not be NULL).
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_ARG if cfg is NULL
 *         ESP_ERR_NO_MEM if context allocation fails
 *         Any error returned by spi_nand_flash_init_device() or backend init()
 */
esp_err_t nand_emul_advanced_init(spi_nand_flash_device_t **dev_out,
                                  const nand_emul_advanced_config_t *cfg);

/**
 * @brief Clean up the advanced NAND flash emulator.
 *
 * Calls backend and failure model deinit() if present, frees the advanced
 * context, then calls spi_nand_flash_deinit_device().
 *
 * @param dev  Device handle created by nand_emul_advanced_init().
 * @return ESP_OK on success
 */
esp_err_t nand_emul_advanced_deinit(spi_nand_flash_device_t *dev);

/**
 * @brief Returns true if the device was initialized with nand_emul_advanced_init().
 *
 * Safe to call on any device handle; returns false for handles initialized via
 * the standard spi_nand_flash_init_device() path.
 *
 * @param dev  Device handle.
 * @return true if advanced tracking is active, false otherwise.
 */
bool nand_emul_has_advanced_tracking(spi_nand_flash_device_t *dev);

/* ---------------------------------------------------------------------------
 * Query API
 * -------------------------------------------------------------------------*/

/**
 * @brief Get wear metadata for a specific block.
 *
 * For blocks that have never been erased, returns ESP_OK and fills @p out with zeros.
 *
 * @return ESP_ERR_INVALID_STATE if advanced tracking is not active.
 */
esp_err_t nand_emul_get_block_wear(spi_nand_flash_device_t *dev,
                                   uint32_t block_num,
                                   block_metadata_t *out);

/**
 * @brief Get wear metadata for a specific page.
 *
 * For pages that have never been programmed, returns ESP_OK and fills @p out with zeros.
 *
 * @return ESP_ERR_INVALID_STATE if advanced tracking is not active.
 */
esp_err_t nand_emul_get_page_wear(spi_nand_flash_device_t *dev,
                                  uint32_t page_num,
                                  page_metadata_t *out);

/**
 * @brief Get aggregate wear statistics.
 *
 * @return ESP_ERR_INVALID_STATE if advanced tracking is not active.
 */
esp_err_t nand_emul_get_wear_stats(spi_nand_flash_device_t *dev,
                                   nand_wear_stats_t *out);

/**
 * @brief Fill wear histograms.
 *
 * Caller must set bin_width and n_bins in each sub-histogram before calling.
 *
 * @return ESP_ERR_INVALID_STATE if advanced tracking is not active.
 * @return ESP_ERR_NOT_SUPPORTED if the backend does not implement get_histograms.
 */
esp_err_t nand_emul_get_wear_histograms(spi_nand_flash_device_t *dev,
                                        nand_wear_histograms_t *out);

/**
 * @brief Record logical bytes written by the host/filesystem layer for WAF.
 *
 * @return ESP_ERR_NOT_SUPPORTED if advanced tracking is not active.
 */
esp_err_t nand_emul_record_logical_write(spi_nand_flash_device_t *dev, size_t nbytes);

/**
 * @brief Iterate over all blocks that have wear metadata.
 *
 * @return ESP_ERR_INVALID_STATE if advanced tracking is not active.
 */
esp_err_t nand_emul_iterate_worn_blocks(spi_nand_flash_device_t *dev,
                                        bool (*callback)(uint32_t block_num,
                                                         block_metadata_t *meta,
                                                         void *user_data),
                                        void *user_data);

/**
 * @brief Iterate over all pages that have wear metadata.
 *
 * @return ESP_ERR_INVALID_STATE if advanced tracking is not active.
 */
esp_err_t nand_emul_iterate_worn_pages(spi_nand_flash_device_t *dev,
                                       bool (*callback)(uint32_t page_num,
                                                        page_metadata_t *meta,
                                                        void *user_data),
                                       void *user_data);

/**
 * @brief Mark a block as a simulated bad block.
 *
 * @return ESP_ERR_INVALID_STATE if advanced tracking is not active.
 */
esp_err_t nand_emul_mark_bad_block(spi_nand_flash_device_t *dev, uint32_t block_num);

/**
 * @brief Save current metadata state to a binary snapshot file.
 *
 * @return ESP_ERR_INVALID_STATE if advanced tracking is not active.
 */
esp_err_t nand_emul_save_snapshot(spi_nand_flash_device_t *dev, const char *filename);

/**
 * @brief Load metadata state from a previously saved binary snapshot file.
 *
 * On error, backend state is left unchanged (no partial updates).
 *
 * @return ESP_ERR_INVALID_STATE if advanced tracking is not active.
 * @return ESP_ERR_INVALID_CRC if header checksum fails.
 * @return ESP_ERR_NOT_SUPPORTED if snapshot version is unrecognized.
 */
esp_err_t nand_emul_load_snapshot(spi_nand_flash_device_t *dev, const char *filename);

/**
 * @brief Export current metadata to a JSON file for analysis.
 *
 * @return ESP_ERR_INVALID_STATE if advanced tracking is not active.
 */
esp_err_t nand_emul_export_json(spi_nand_flash_device_t *dev, const char *filename);

#ifdef __cplusplus
}
#endif
