/*
 * SPDX-FileCopyrightText: 2026 Martin Havlik <omnitex.git@gmail.com>
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
    uint32_t pre_warm_erase_cycles; /*!< Pre-populate all erase_count[] to this value at init (0 = off).
                                         Use to simulate a flash that has already consumed part of its
                                         erase budget before the test starts.  Combined with
                                         ecc_prog_*_erase_threshold this causes page relief to fire
                                         from the very first write. */

    /* --- Program wear-out --------------------------------------------- */
    uint32_t max_prog_cycles;    /*!< Page wears out after this many programs (0 = unlimited) */

    /* --- Grave page / retention failure -------------------------------- */
    uint32_t grave_page_threshold; /*!< prog_count > this → NAND_ECC_NOT_CORRECTED on read (0 = disabled) */

    /* --- Per-operation probabilistic failures -------------------------- */
    float    read_fail_prob;    /*!< Probability [0.0, 1.0] that nand_read() returns an error */
    float    prog_fail_prob;    /*!< Probability [0.0, 1.0] that nand_prog() returns an error */
    float    erase_fail_prob;   /*!< Probability [0.0, 1.0] that nand_erase_block() returns an error */
    float    copy_fail_prob;    /*!< Probability [0.0, 1.0] that nand_copy() returns an error */
    float    copy_ecc_fail_prob; /*!< Probability [0.0, 1.0] that nand_copy() returns ESP_FAIL with
                                      ecc_corrected_bits_status set to NAND_ECC_NOT_CORRECTED.
                                      Unlike copy_fail_prob (which leaves ecc status unchanged), this
                                      variant makes the DHARA_E_ECC branch in dhara_glue.c reachable
                                      independently of write-wear thresholds. */
    unsigned int op_fail_seed;  /*!< Seed for the per-op failure PRNG */

    /* --- Power-loss crash --------------------------------------------- */
    uint32_t crash_after_ops_min; /*!< Crash fires after op count in [min, max] (0 = disabled) */
    uint32_t crash_after_ops_max; /*!< Upper bound for crash range; == min for deterministic mode */
    float    crash_probability;   /*!< Per-op crash probability (0.0 = disabled; mutually exclusive with range mode) */
    unsigned int crash_seed;      /*!< Seed for crash-point selection and torn-write offset */

    /* --- ECC threshold alignment -------------------------------------- */
    /* ecc_data_refresh_threshold is the ECC severity level (as a
     * nand_ecc_status_t integer value) at or above which the block-health
     * layer considers a page to need data refresh / scrubbing.  It must
     * be consistent with CONFIG_NAND_FLASH_PROG_PAGE_RELIEF_MIN_ECC and
     * with the ecc_prog_*_erase_threshold values chosen below — otherwise
     * relief will either never fire or fire too aggressively.
     *
     * Default (0): the simulator uses 4 (NAND_ECC_4_TO_6_BITS_CORRECTED),
     * which matches the real-driver default and
     * CONFIG_NAND_FLASH_PROG_PAGE_RELIEF_MIN_ECC = 2 (same level).
     * Set explicitly in sweep configs where a different threshold is needed. */
    uint8_t  ecc_data_refresh_threshold; /*!< ECC data-refresh threshold for chip.ecc_data (0 = use default 4) */

    /* --- ECC read-disturb simulation ---------------------------------- */
    /* Models BER increase caused by repeated reads of the same page
     * (read-disturb effect).  Thresholds are per-page READ counts.
     * Fires through the on_page_read_ecc callback inside nand_read().
     * Consumer: scrubbing / data-refresh logic.                        */
    uint32_t ecc_mid_threshold;  /*!< Reads before NAND_ECC_1_TO_3_BITS_CORRECTED (0 = disabled) */
    uint32_t ecc_high_threshold; /*!< Reads before NAND_ECC_4_TO_6_BITS_CORRECTED (0 = disabled) */
    uint32_t ecc_fail_threshold; /*!< Reads before NAND_ECC_NOT_CORRECTED (0 = disabled) */

    /* --- ECC write-wear simulation ------------------------------------ */
    /* Models BER increase caused by repeated erase/program cycling of a
     * block (write wear).  Thresholds are per-BLOCK ERASE counts.
     * Fires through nand_get_ecc_status(), which sets
     * handle->chip.ecc_data.ecc_corrected_bits_status before every
     * nand_prog() and nand_copy() when CONFIG_NAND_FLASH_PROG_PAGE_RELIEF
     * is enabled.
     * Consumer: page-write relief gate in nand_impl.c.
     *
     * Threshold alignment: choose values consistent with
     * CONFIG_NAND_FLASH_PROG_PAGE_RELIEF_MIN_ECC so that relief fires at
     * the intended ECC severity level.                                   */
    uint32_t ecc_prog_mid_erase_threshold;  /*!< Block erases before NAND_ECC_1_TO_3_BITS_CORRECTED (0 = disabled) */
    uint32_t ecc_prog_high_erase_threshold; /*!< Block erases before NAND_ECC_4_TO_6_BITS_CORRECTED (0 = disabled) */
    uint32_t ecc_prog_fail_erase_threshold; /*!< Block erases before NAND_ECC_NOT_CORRECTED (0 = disabled) */
    float    ecc_prog_noise_prob;           /*!< Per-page probability [0,1] of bumping ECC one level higher
                                                 (models page-to-page BER variation within a worn block;
                                                 0.0 = deterministic, disabled) */

    /* --- ECC callback ------------------------------------------------- */
    nand_fault_sim_ecc_cb_t on_page_read_ecc; /*!< Invoked on ECC events from read-disturb model; NULL = silently skip */
    void                   *ecc_cb_ctx;       /*!< Passed verbatim to on_page_read_ecc */

    /* --- Page-relief callback ----------------------------------------- */
    nand_fault_sim_ecc_cb_t on_page_relief;      /*!< Invoked from nand_get_ecc_status() when ecc_corrected_bits_status
                                                       is elevated (NAND_ECC_1_TO_3_BITS_CORRECTED or higher).
                                                       Same signature as on_page_read_ecc; page is the destination
                                                       page being checked before prog/copy.  NULL = silently skip. */
    void                   *page_relief_cb_ctx;  /*!< Passed verbatim to on_page_relief */
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

/**
 * @brief Return whether the simulated device is in a crashed state.
 *
 * Returns true after a power-loss crash has fired (either via crash_after_ops
 * range or crash_probability). Resets to false after nand_fault_sim_reset().
 */
bool nand_fault_sim_is_crashed(void);

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
