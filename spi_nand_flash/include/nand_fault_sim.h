/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file nand_fault_sim.h
 * @brief NAND fault-injection simulator for Linux host tests.
 *
 * Replaces nand_impl_linux.c at link time by providing definitions of
 * all 9 nand_impl symbols. Non-faulted paths delegate to nand_emul_*
 * (nand_linux_mmap_emul.c). Linux target only.
 *
 * Usage:
 *   1. Call nand_fault_sim_init() once, passing the emul config and fault config.
 *   2. Call nand_init_device() as usual — it now calls nand_emul_init() and sets
 *      up the fault simulator state transparently.
 *   3. Call nand_fault_sim_deinit() after nand_emul_deinit() to free sim state.
 *
 * Threading: NOT thread-safe. Host tests are single-threaded.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "nand_device_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * ECC callback type
 * -------------------------------------------------------------------- */

/**
 * @brief Callback invoked when a read produces an ECC event.
 *
 * @param page   Physical page number that was read.
 * @param status ECC severity.
 * @param ctx    User context pointer passed to nand_fault_sim_init().
 */
typedef void (*nand_fault_sim_ecc_cb_t)(uint32_t page, nand_ecc_status_t status, void *ctx);

/* -----------------------------------------------------------------------
 * Fault scenario presets
 * -------------------------------------------------------------------- */

typedef enum {
    NAND_SIM_SCENARIO_FRESH,          /*!< No faults; clean device baseline */
    NAND_SIM_SCENARIO_LIGHTLY_USED,   /*!< ~2% factory bad blocks, low erase counts */
    NAND_SIM_SCENARIO_AGED,           /*!< ~10% bad blocks, blocks near half erase budget */
    NAND_SIM_SCENARIO_FAILING,        /*!< ~20% bad blocks, weak blocks/pages, elevated failure probs */
    NAND_SIM_SCENARIO_POWER_LOSS,     /*!< Crash range [50,500], torn writes, no other faults */
} nand_sim_scenario_t;

/* -----------------------------------------------------------------------
 * Fault configuration
 * -------------------------------------------------------------------- */

/**
 * @brief Configuration for the NAND fault simulator.
 *
 * All fault features are disabled when their controlling field is 0 / 0.0 / NULL.
 * Populate with nand_fault_sim_config_preset() or set individual fields.
 */
typedef struct {
    /* --- Factory bad blocks ------------------------------------------- */
    const uint32_t *factory_bad_blocks;   /*!< Array of block indices to mark bad at init */
    uint32_t        factory_bad_block_count; /*!< Number of entries in factory_bad_blocks */

    /* --- Erase wear-out ----------------------------------------------- */
    uint32_t max_erase_cycles;   /*!< Block wears out after this many erases (0 = unlimited) */

    /* --- Program wear-out --------------------------------------------- */
    uint32_t max_prog_cycles;    /*!< Page wears out after this many programs (0 = unlimited) */

    /* --- Grave page / retention failure -------------------------------- */
    uint32_t grave_page_threshold; /*!< prog_count > this → NAND_ECC_NOT_CORRECTED on read (0 = disabled) */

    /* --- Per-operation probabilistic failures -------------------------- */
    float    read_fail_prob;    /*!< Probability [0.0, 1.0] that nand_read() returns an error */
    float    prog_fail_prob;    /*!< Probability [0.0, 1.0] that nand_prog() returns an error */
    float    erase_fail_prob;   /*!< Probability [0.0, 1.0] that nand_erase_block() returns an error */
    float    copy_fail_prob;    /*!< Probability [0.0, 1.0] that nand_copy() returns an error */
    unsigned int op_fail_seed;  /*!< Seed for the per-op failure PRNG */

    /* --- Power-loss crash --------------------------------------------- */
    uint32_t crash_after_ops_min; /*!< Crash fires after op count in [min, max] (0 = disabled) */
    uint32_t crash_after_ops_max; /*!< Upper bound for crash range; == min for deterministic mode */
    float    crash_probability;   /*!< Per-op crash probability (0.0 = disabled; mutually exclusive with range mode) */
    unsigned int crash_seed;      /*!< Seed for crash-point selection and torn-write offset */

    /* --- ECC read-disturb simulation ---------------------------------- */
    uint32_t ecc_mid_threshold;  /*!< Reads before NAND_ECC_1_TO_3_BITS_CORRECTED (0 = disabled) */
    uint32_t ecc_high_threshold; /*!< Reads before NAND_ECC_4_TO_6_BITS_CORRECTED (0 = disabled) */
    uint32_t ecc_fail_threshold; /*!< Reads before NAND_ECC_NOT_CORRECTED (0 = disabled) */

    /* --- ECC callback ------------------------------------------------- */
    nand_fault_sim_ecc_cb_t on_page_read_ecc; /*!< Invoked on ECC events; NULL = silently skip */
    void                   *ecc_cb_ctx;       /*!< Passed verbatim to on_page_read_ecc */
} nand_fault_sim_config_t;

/* -----------------------------------------------------------------------
 * Lifecycle API
 * -------------------------------------------------------------------- */

/**
 * @brief Initialize the fault simulator.
 *
 * Must be called BEFORE nand_init_device(). Allocates per-block and per-page
 * arrays sized for the geometry in emul_conf. nand_init_device() then calls
 * nand_emul_init() as usual.
 *
 * @param num_blocks       Total number of erase blocks.
 * @param pages_per_block  Pages per erase block (must be a power of two).
 * @param cfg              Fault configuration.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails,
 *         ESP_ERR_INVALID_ARG if cfg is NULL or geometry is zero.
 */
esp_err_t nand_fault_sim_init(uint32_t num_blocks, uint32_t pages_per_block,
                               const nand_fault_sim_config_t *cfg);

/**
 * @brief Free all fault simulator state.
 *
 * Safe to call even if nand_fault_sim_init() failed or was never called.
 */
void nand_fault_sim_deinit(void);

/**
 * @brief Reset fault simulator counters and PRNG state.
 *
 * Zeroes all per-block erase counts, per-page prog/read counts, the op
 * counter, and the crashed flag. Re-seeds both PRNGs. Does NOT modify the
 * mmap backing file (flash contents and OOB bad-block markers are preserved).
 *
 * The in-memory factory-bad-block list is also re-applied from the config, so
 * after reset nand_is_bad() will return true for factory bad blocks again even
 * if they were cleared by a previous erase (OOB markers in the file may differ).
 */
void nand_fault_sim_reset(void);

/* -----------------------------------------------------------------------
 * Statistics API
 * -------------------------------------------------------------------- */

/**
 * @return Number of times block @p block has been erased (0 if out of range).
 */
uint32_t nand_fault_sim_get_erase_count(uint32_t block);

/**
 * @return Number of times page @p page has been programmed (0 if out of range).
 */
uint32_t nand_fault_sim_get_prog_count(uint32_t page);

/**
 * @return Number of times page @p page has been read (0 if out of range).
 */
uint32_t nand_fault_sim_get_read_count(uint32_t page);

/* -----------------------------------------------------------------------
 * Preset API
 * -------------------------------------------------------------------- */

/**
 * @brief Return a fully populated nand_fault_sim_config_t for a named scenario.
 *
 * The returned config can be passed directly to nand_fault_sim_init() or
 * modified before use. The factory_bad_blocks pointer in LIGHTLY_USED,
 * AGED, and FAILING presets points to a static internal array; do not free it.
 *
 * @param scenario  Scenario identifier.
 * @return Populated config; on unknown scenario returns FRESH config.
 */
nand_fault_sim_config_t nand_fault_sim_config_preset(nand_sim_scenario_t scenario);

#ifdef __cplusplus
}
#endif
