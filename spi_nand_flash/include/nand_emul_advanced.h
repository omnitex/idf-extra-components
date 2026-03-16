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
#include "nand_linux_mmap_emul.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Core metadata types
 * -------------------------------------------------------------------------*/

/**
 * @brief Per-byte write tracking metadata (one entry per outlier byte).
 *
 * Tracks how many additional times a specific byte offset was written
 * relative to the page's program_count (i.e. the delta).  An entry is only
 * created when a byte is written more often than the surrounding page, so
 * zero-delta bytes are never stored.
 *
 * Size: 16 bytes (explicit padding for 64-bit alignment).
 */
typedef struct {
    uint16_t byte_offset;           /**< Byte offset within the page (0 .. page_size-1) */
    int16_t  write_count_delta;     /**< Extra writes beyond the page program_count */
    uint32_t _reserved;             /**< Explicit padding for 16-byte struct size */
    uint64_t last_write_timestamp;  /**< Timestamp of the most recent write to this byte */
} byte_delta_metadata_t;

/**
 * @brief Per-page wear and program tracking metadata.
 *
 * program_count is reset to 0 on every block erase; program_count_total
 * accumulates across the lifetime of the device.
 *
 * The byte_deltas dynamic array is owned by this struct and must be freed
 * before the struct is discarded.  It is NULL when byte-level tracking is
 * disabled or when no outlier writes have occurred in the current erase cycle.
 */
typedef struct {
    uint32_t page_num;                      /**< Page number (for iteration / hashing) */
    uint32_t program_count;                 /**< Programs in current erase cycle (reset to 0 on block erase) */
    uint32_t program_count_total;           /**< Lifetime total programs (never reset) */
    uint32_t _reserved;                     /**< Padding */
    uint64_t first_program_timestamp;       /**< Timestamp of first program in current erase cycle */
    uint64_t last_program_timestamp;        /**< Timestamp of most recent program in current erase cycle */
    uint16_t byte_delta_count;              /**< Number of valid entries in byte_deltas */
    uint16_t byte_delta_capacity;           /**< Allocated capacity of byte_deltas array */
    byte_delta_metadata_t *byte_deltas;     /**< Dynamic array of per-byte deltas (may be NULL) */
    uint32_t _reserved2;                    /**< Padding */
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
    uint8_t  is_bad_block;                  /**< Non-zero if this block has been marked bad */
    uint8_t  _padding[7];                   /**< Explicit padding */
} block_metadata_t;

/**
 * @brief Aggregate wear-leveling statistics across all tracked blocks/pages.
 */
typedef struct {
    uint32_t min_block_erases;          /**< Minimum erase count across all tracked blocks */
    uint32_t max_block_erases;          /**< Maximum erase count across all tracked blocks */
    double   avg_block_erases;          /**< Average erase count across all tracked blocks */
    double   wear_leveling_variation;   /**< (max - min) / avg; 0 if avg == 0 */
    uint32_t total_blocks_tracked;      /**< Number of blocks with at least one erase */
    uint32_t total_pages_tracked;       /**< Number of pages with at least one program */
    uint32_t min_page_programs;         /**< Minimum program count across all tracked pages */
    uint32_t max_page_programs;         /**< Maximum program count across all tracked pages */
} nand_wear_stats_t;

/**
 * @brief Context passed to failure-model callbacks describing the current operation.
 */
typedef struct {
    uint32_t block_num;             /**< Block number being operated on */
    uint32_t page_num;              /**< Page number being operated on */
    uint16_t byte_offset;           /**< Byte offset within the page */
    uint32_t operation_size;        /**< Number of bytes in the operation */
    uint64_t timestamp;             /**< Current timestamp */
    uint32_t total_blocks;          /**< Total blocks in the device */
    uint32_t pages_per_block;       /**< Pages per block */
    uint32_t page_size;             /**< Page size in bytes */
    const block_metadata_t *block_meta; /**< Block metadata at operation time (may be NULL) */
    const page_metadata_t  *page_meta;  /**< Page metadata at operation time (may be NULL) */
} nand_operation_context_t;

/* ---------------------------------------------------------------------------
 * Metadata backend interface (vtable)
 * -------------------------------------------------------------------------*/

/**
 * @brief Vtable for metadata storage backends.
 *
 * All function pointers are required.  A no-op backend that returns ESP_OK
 * for everything is provided via @c nand_noop_metadata_backend_ops.
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
     * Called after a write that affects bytes within a single page.
     * @p byte_offset is the offset within the page; @p len is the number of bytes.
     */
    esp_err_t (*on_byte_write_range)(void *backend_handle, uint32_t page_num,
                                     uint16_t byte_offset, size_t len, uint64_t timestamp);

    /**
     * Query block metadata.  If the block was never erased, zeros are returned
     * and the function still returns ESP_OK.
     */
    esp_err_t (*get_block_info)(void *backend_handle, uint32_t block_num,
                                block_metadata_t *out);

    /**
     * Query page metadata.  If the page was never programmed, zeros are returned
     * and the function still returns ESP_OK.
     */
    esp_err_t (*get_page_info)(void *backend_handle, uint32_t page_num,
                               page_metadata_t *out);

    /**
     * Return a pointer into backend-owned storage for the byte-delta array of
     * the given page.  The pointer is valid until the next modifying operation
     * (write, erase, snapshot load) or deinit.  Caller must NOT free it.
     *
     * @p out_deltas is set to NULL and @p out_count to 0 if no deltas exist.
     */
    esp_err_t (*get_byte_deltas)(void *backend_handle, uint32_t page_num,
                                 byte_delta_metadata_t **out_deltas, uint16_t *out_count);

    /**
     * Iterate over all tracked blocks.  The callback is called for each block;
     * returning false stops iteration.
     */
    esp_err_t (*iterate_blocks)(void *backend_handle,
                                bool (*callback)(const block_metadata_t *meta, void *user_data),
                                void *user_data);

    /**
     * Iterate over all tracked pages.  The callback is called for each page;
     * returning false stops iteration.
     */
    esp_err_t (*iterate_pages)(void *backend_handle,
                               bool (*callback)(const page_metadata_t *meta, void *user_data),
                               void *user_data);

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
 * @c nand_noop_failure_model_ops.
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
     */
    void (*corrupt_read_data)(void *model_handle, const nand_operation_context_t *ctx,
                              void *data, size_t len);

    /** Return true if the block at @p block_num should be treated as bad. */
    bool (*is_block_bad)(void *model_handle, uint32_t block_num);
} nand_failure_model_ops_t;

/* ---------------------------------------------------------------------------
 * Advanced emulator configuration
 * -------------------------------------------------------------------------*/

/**
 * @brief Configuration for the advanced NAND emulator.
 *
 * Pass to nand_emul_advanced_init() instead of nand_emul_init().
 */
typedef struct {
    /** Base file-mapped emulator configuration (required). */
    nand_file_mmap_emul_config_t base;

    /** Pointer to metadata backend vtable (required).  Use
     *  @c &nand_noop_metadata_backend_ops to disable tracking. */
    const nand_metadata_backend_ops_t *metadata_ops;

    /** Config blob passed verbatim to metadata_ops->init() (may be NULL). */
    const void *metadata_config;

    /** Pointer to failure model vtable (optional, NULL to disable). */
    const nand_failure_model_ops_t *failure_ops;

    /** Config blob passed verbatim to failure_ops->init() (may be NULL). */
    const void *failure_config;

    /** Enable block-level erase tracking. */
    bool track_block_level;

    /** Enable page-level program tracking. */
    bool track_page_level;

    /** Enable byte-level write-range tracking (requires track_page_level). */
    bool track_byte_level;

    /**
     * Optional user-supplied timestamp function.
     * If NULL, a monotonic counter is used.
     */
    uint64_t (*get_timestamp)(void);
} nand_emul_advanced_config_t;

/* ---------------------------------------------------------------------------
 * Advanced emulator public API (declared here; implemented in nand_linux_mmap_emul.c)
 * -------------------------------------------------------------------------*/

/**
 * @brief Initialize the advanced NAND flash emulator.
 *
 * Internally calls nand_emul_init() and then sets up the metadata backend
 * and failure model according to @p cfg.
 *
 * @param handle  spi_nand_flash_device_t handle
 * @param cfg     Advanced configuration
 * @return ESP_OK on success
 *         ESP_ERR_NO_MEM if context allocation fails
 *         Any error returned by the underlying nand_emul_init()
 */
esp_err_t nand_emul_advanced_init(spi_nand_flash_device_t *handle,
                                  const nand_emul_advanced_config_t *cfg);

/**
 * @brief Clean up the advanced NAND flash emulator.
 *
 * Calls the backend and failure model deinit callbacks, frees the advanced
 * context, then calls nand_emul_deinit().
 *
 * @param handle  spi_nand_flash_device_t handle
 * @return ESP_OK on success
 */
esp_err_t nand_emul_advanced_deinit(spi_nand_flash_device_t *handle);

#ifdef __cplusplus
}
#endif
