/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**
 * @file dhara_bbal.h
 * @brief Bad Block Abstraction Layer (BBAL) for Dhara NAND flash management.
 *
 * The BBAL sits between a raw NAND HAL and Dhara's map/journal layers.
 * It scans all physical blocks at init time, builds a heap-allocated
 * logical→physical remapping table (skipping bad blocks), and presents
 * a contiguous, bad-block-free logical address space to Dhara.
 *
 * Because Dhara's seven dhara_nand_* callbacks are resolved by the linker
 * as free functions, this translation unit IS the implementation of those
 * seven functions.  The underlying physical HAL must use differently-named
 * entry points (e.g. spi_nand_read / spi_nand_prog / …).
 *
 * Usage:
 * @code
 *   dhara_bbal_t bbal;
 *   if (dhara_bbal_init(&bbal, &phys_nand) != 0) { handle_error(); }
 *
 *   struct dhara_map map;
 *   dhara_map_init(&map, &bbal.logical_nand, journal_buf, gc_ratio);
 *   dhara_map_resume(&map, &err);
 *   // … use map normally …
 *   dhara_bbal_deinit(&bbal);
 * @endcode
 */

#include <stdint.h>
#include <stddef.h>
#include "dhara/nand.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bad Block Abstraction Layer context.
 *
 * @note  `logical_nand` MUST remain the first field.  Dhara passes a
 *        `const struct dhara_nand *` to every callback; the BBAL
 *        implementations cast that pointer directly to `dhara_bbal_t *`
 *        (container-of pattern with zero offset).
 */
typedef struct {
    /**
     * Logical NAND descriptor presented to Dhara.
     * Geometry fields (`log2_page_size`, `log2_ppb`) are copied verbatim
     * from `phys_nand`; only `num_blocks` is overridden to the count of
     * good physical blocks.
     *
     * MUST be the first field — see note above.
     */
    struct dhara_nand       logical_nand;

    /** Pointer to the underlying physical NAND descriptor. */
    const struct dhara_nand *phys_nand;

    /**
     * Heap-allocated remapping table.
     * `logical_to_phys[logical_block]` → physical block index.
     * Length is `num_logical` entries of type `dhara_block_t`.
     * Allocated by `dhara_bbal_init`, freed by `dhara_bbal_deinit`.
     */
    dhara_block_t           *logical_to_phys;

    /** Number of good (logical) blocks — equals `logical_nand.num_blocks`. */
    unsigned int             num_logical;

    /** Number of bad blocks found during the last `dhara_bbal_init` scan. */
    unsigned int             num_bad;
} dhara_bbal_t;

/**
 * @brief Initialise the BBAL and build the logical→physical remapping table.
 *
 * Scans every physical block of `phys_nand` by calling
 * `dhara_nand_is_bad()` on the physical descriptor.  Good blocks are
 * collected in ascending order into a heap-allocated `logical_to_phys[]`
 * table.  The table is right-sized with `realloc` after the scan so it
 * occupies exactly `num_logical * sizeof(dhara_block_t)` bytes.
 *
 * On success `bbal->logical_nand` is ready to be passed to
 * `dhara_map_init()` / `dhara_journal_init()`.
 *
 * @param[out] bbal      Caller-allocated BBAL context to initialise.
 * @param[in]  phys_nand Physical NAND descriptor (must remain valid for
 *                       the lifetime of `bbal`).
 *
 * @return  0  on success.
 * @return -1  on allocation failure; `errno` is set to `ENOMEM`.
 */
int dhara_bbal_init(dhara_bbal_t *bbal, const struct dhara_nand *phys_nand);

/**
 * @brief Release all resources held by the BBAL.
 *
 * Frees the heap-allocated `logical_to_phys[]` table and sets the
 * pointer to NULL.  The BBAL context itself is caller-owned and is not
 * freed.
 *
 * @param[in,out] bbal  BBAL context previously initialised with
 *                      `dhara_bbal_init`.
 */
void dhara_bbal_deinit(dhara_bbal_t *bbal);

#ifdef __cplusplus
}
#endif
