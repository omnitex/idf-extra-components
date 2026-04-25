/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_check.h"
#include "esp_err.h"
#include "spi_nand_flash.h"
#include "nand.h"
#include "nand_linux_mmap_emul.h"
#include "nand_fault_sim.h"

static const char *TAG = "nand_fault_sim";

/* OOB markers — identical to nand_impl_linux.c */
static const uint8_t s_oob_used_page_markers[4] = { 0xFF, 0xFF, 0x00, 0x00 };
static const uint8_t s_oob_mark_bad_markers[4]  = { 0x00, 0x00, 0xFF, 0xFF };

/* -----------------------------------------------------------------------
 * Internal state (singleton — host tests are single-threaded)
 * -------------------------------------------------------------------- */

typedef struct {
    nand_fault_sim_config_t cfg;

    uint32_t  num_blocks;
    uint32_t  num_pages;          /* num_blocks * pages_per_block */
    uint32_t  pages_per_block;

    uint32_t *erase_count;        /* [num_blocks] */
    uint32_t *prog_count;         /* [num_pages]  */
    uint32_t *read_count;         /* [num_pages]  */

    bool      crashed;
    uint32_t  op_counter;
    uint32_t  crash_point;        /* resolved crash op count (range mode) */

    unsigned int op_fail_state;   /* rand_r() state for per-op failures */
    unsigned int crash_state;     /* rand_r() state for crash + torn write */

    bool      initialized;
} nand_sim_state_t;

static nand_sim_state_t s_sim;

/* -----------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------- */

static uint32_t sim_page_offset(const spi_nand_flash_device_t *h, uint32_t page)
{
    return page * h->chip.emulated_page_size;
}

static esp_err_t sim_block_file_offset(const spi_nand_flash_device_t *h,
                                        uint32_t block, size_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(h->chip.num_blocks > 0, ESP_ERR_INVALID_STATE, TAG, "num_blocks 0");
    ESP_RETURN_ON_FALSE(block < h->chip.num_blocks, ESP_ERR_INVALID_ARG, TAG, "block OOB");
    const uint64_t ppb   = 1ull << h->chip.log2_ppb;
    const uint64_t bsize = ppb * (uint64_t)h->chip.emulated_page_size;
    const uint64_t off   = (uint64_t)block * bsize;
    ESP_RETURN_ON_FALSE(off <= (uint64_t)SIZE_MAX, ESP_ERR_INVALID_SIZE, TAG, "offset overflow");
    *out = (size_t)off;
    return ESP_OK;
}

/* Roll op-fail PRNG; return true if the operation should fail. */
static bool sim_roll_op_fail(float prob)
{
    if (prob <= 0.0f) {
        return false;
    }
    if (prob >= 1.0f) {
        return true;
    }
    unsigned int r = rand_r(&s_sim.op_fail_state);
    return ((float)r / (float)RAND_MAX) < prob;
}

/* Check crash condition.  Returns true if the crash fires NOW.
 * Sets s_sim.crashed on first fire; subsequent calls return false
 * (caller detects crash via s_sim.crashed before incrementing op_counter). */
static bool sim_check_crash(void)
{
    if (s_sim.crashed) {
        return false;
    }
    s_sim.op_counter++;

    if (s_sim.cfg.crash_after_ops_min > 0) {
        /* Range / deterministic mode */
        if (s_sim.op_counter >= s_sim.crash_point) {
            s_sim.crashed = true;
            return true;
        }
    } else if (s_sim.cfg.crash_probability > 0.0f) {
        /* Per-op probability mode */
        unsigned int r = rand_r(&s_sim.crash_state);
        if (((float)r / (float)RAND_MAX) < s_sim.cfg.crash_probability) {
            s_sim.crashed = true;
            return true;
        }
    }
    return false;
}

/* Derive crash point from [min, max] using crash_state PRNG. */
static uint32_t sim_derive_crash_point(uint32_t lo, uint32_t hi, unsigned int *state)
{
    if (lo == hi) {
        return lo;
    }
    unsigned int r = rand_r(state);
    return lo + (uint32_t)((uint64_t)r % (uint64_t)(hi - lo + 1));
}

static void sim_invoke_ecc_cb(uint32_t page, nand_ecc_status_t status)
{
    if (s_sim.cfg.on_page_read_ecc) {
        s_sim.cfg.on_page_read_ecc(page, status, s_sim.cfg.ecc_cb_ctx);
    }
}

/* -----------------------------------------------------------------------
 * nand_fault_sim lifecycle
 * -------------------------------------------------------------------- */

esp_err_t nand_fault_sim_init(uint32_t num_blocks, uint32_t pages_per_block,
                               const nand_fault_sim_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg NULL");
    ESP_RETURN_ON_FALSE(num_blocks > 0 && pages_per_block > 0,
                        ESP_ERR_INVALID_ARG, TAG, "bad geometry");

    nand_fault_sim_deinit();

    s_sim.cfg            = *cfg;
    s_sim.num_blocks     = num_blocks;
    s_sim.pages_per_block = pages_per_block;
    s_sim.num_pages      = num_blocks * pages_per_block;

    s_sim.erase_count = calloc(num_blocks, sizeof(uint32_t));
    s_sim.prog_count  = calloc(s_sim.num_pages, sizeof(uint32_t));
    s_sim.read_count  = calloc(s_sim.num_pages, sizeof(uint32_t));

    if (!s_sim.erase_count || !s_sim.prog_count || !s_sim.read_count) {
        nand_fault_sim_deinit();
        return ESP_ERR_NO_MEM;
    }

    s_sim.op_fail_state = cfg->op_fail_seed;
    s_sim.crash_state   = cfg->crash_seed;
    s_sim.crashed       = false;
    s_sim.op_counter    = 0;

    if (cfg->crash_after_ops_min > 0) {
        unsigned int tmp_state = cfg->crash_seed;
        s_sim.crash_point = sim_derive_crash_point(
                                cfg->crash_after_ops_min,
                                cfg->crash_after_ops_max > cfg->crash_after_ops_min
                                ? cfg->crash_after_ops_max
                                : cfg->crash_after_ops_min,
                                &tmp_state);
    }

    s_sim.initialized = true;
    return ESP_OK;
}

void nand_fault_sim_deinit(void)
{
    free(s_sim.erase_count);
    free(s_sim.prog_count);
    free(s_sim.read_count);
    memset(&s_sim, 0, sizeof(s_sim));
}

void nand_fault_sim_reset(void)
{
    if (!s_sim.initialized) {
        return;
    }
    memset(s_sim.erase_count, 0, s_sim.num_blocks  * sizeof(uint32_t));
    memset(s_sim.prog_count,  0, s_sim.num_pages   * sizeof(uint32_t));
    memset(s_sim.read_count,  0, s_sim.num_pages   * sizeof(uint32_t));
    s_sim.crashed     = false;
    s_sim.op_counter  = 0;
    s_sim.op_fail_state = s_sim.cfg.op_fail_seed;
    s_sim.crash_state   = s_sim.cfg.crash_seed;

    if (s_sim.cfg.crash_after_ops_min > 0) {
        unsigned int tmp_state = s_sim.cfg.crash_seed;
        s_sim.crash_point = sim_derive_crash_point(
                                s_sim.cfg.crash_after_ops_min,
                                s_sim.cfg.crash_after_ops_max > s_sim.cfg.crash_after_ops_min
                                ? s_sim.cfg.crash_after_ops_max
                                : s_sim.cfg.crash_after_ops_min,
                                &tmp_state);
    }
}

uint32_t nand_fault_sim_get_erase_count(uint32_t block)
{
    if (!s_sim.initialized || block >= s_sim.num_blocks) {
        return 0;
    }
    return s_sim.erase_count[block];
}

uint32_t nand_fault_sim_get_prog_count(uint32_t page)
{
    if (!s_sim.initialized || page >= s_sim.num_pages) {
        return 0;
    }
    return s_sim.prog_count[page];
}

uint32_t nand_fault_sim_get_read_count(uint32_t page)
{
    if (!s_sim.initialized || page >= s_sim.num_pages) {
        return 0;
    }
    return s_sim.read_count[page];
}

/* -----------------------------------------------------------------------
 * Preset configurations
 * -------------------------------------------------------------------- */

static const uint32_t s_preset_lightly_used_bad[] = { 1, 5 };
static const uint32_t s_preset_aged_bad[]          = {
    1, 3, 5, 8, 12, 15, 17, 20, 25, 30
};
static const uint32_t s_preset_failing_bad[]       = {
    0, 1, 2, 3, 5, 7, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 35, 38
};

nand_fault_sim_config_t nand_fault_sim_config_preset(nand_sim_scenario_t scenario)
{
    nand_fault_sim_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    switch (scenario) {
    case NAND_SIM_SCENARIO_FRESH:
        break;

    case NAND_SIM_SCENARIO_LIGHTLY_USED:
        cfg.factory_bad_blocks      = s_preset_lightly_used_bad;
        cfg.factory_bad_block_count = 2;
        cfg.max_erase_cycles        = 10000;
        break;

    case NAND_SIM_SCENARIO_AGED:
        cfg.factory_bad_blocks      = s_preset_aged_bad;
        cfg.factory_bad_block_count = 10;
        cfg.max_erase_cycles        = 1000;
        cfg.max_prog_cycles         = 5000;
        cfg.grave_page_threshold    = 3000;
        break;

    case NAND_SIM_SCENARIO_FAILING:
        cfg.factory_bad_blocks      = s_preset_failing_bad;
        cfg.factory_bad_block_count = 20;
        cfg.max_erase_cycles        = 200;
        cfg.max_prog_cycles         = 400;
        cfg.grave_page_threshold    = 200;
        cfg.prog_fail_prob          = 0.02f;
        cfg.erase_fail_prob         = 0.01f;
        cfg.read_fail_prob          = 0.005f;
        cfg.op_fail_seed            = 42;
        break;

    case NAND_SIM_SCENARIO_POWER_LOSS:
        cfg.crash_after_ops_min     = 50;
        cfg.crash_after_ops_max     = 500;
        cfg.crash_seed              = 1;
        break;

    default:
        break;
    }
    return cfg;
}

/* -----------------------------------------------------------------------
 * detect_chip — same logic as nand_impl_linux.c
 * -------------------------------------------------------------------- */

static esp_err_t detect_chip(spi_nand_flash_device_t *dev)
{
    esp_err_t ret = ESP_OK;
    spi_nand_flash_config_t *config = &dev->config;

    ESP_GOTO_ON_ERROR(nand_emul_init(dev, config->emul_conf), fail, TAG, "");
    dev->chip.page_size = (1 << dev->chip.log2_page_size);

    dev->chip.emulated_page_oob = 64;
    if (dev->chip.page_size == 512) {
        dev->chip.emulated_page_oob = 16;
    } else if (dev->chip.page_size == 4096) {
        dev->chip.emulated_page_oob = 128;
    }
    dev->chip.emulated_page_size = dev->chip.page_size + dev->chip.emulated_page_oob;
    dev->chip.block_size         = (1u << dev->chip.log2_ppb) * dev->chip.page_size;
    const uint32_t file_bpb      = (1u << dev->chip.log2_ppb) * dev->chip.emulated_page_size;

    if (dev->chip.block_size == 0 || file_bpb == 0) {
        ESP_LOGE(TAG, "Invalid block size (0)");
        ret = ESP_ERR_INVALID_SIZE;
        goto fail;
    }
    if (config->emul_conf->flash_file_size % dev->chip.block_size != 0) {
        ESP_LOGE(TAG, "flash_file_size not a multiple of block_size");
        ret = ESP_ERR_INVALID_SIZE;
        goto fail;
    }

    dev->chip.num_blocks             = config->emul_conf->flash_file_size / file_bpb;
    dev->chip.erase_block_delay_us   = 3000;
    dev->chip.program_page_delay_us  = 630;
    dev->chip.read_page_delay_us     = 60;

    dev->device_info.manufacturer_id = 0xEF;
    dev->device_info.device_id       = 0xE100;
    strncpy(dev->device_info.chip_name, "Linux NAND fault sim",
            sizeof(dev->device_info.chip_name) - 1);
    dev->device_info.chip_name[sizeof(dev->device_info.chip_name) - 1] = '\0';

fail:
    return ret;
}

/* -----------------------------------------------------------------------
 * nand_impl interface — 9 symbols replacing nand_impl_linux.c
 * -------------------------------------------------------------------- */

esp_err_t nand_init_device(spi_nand_flash_config_t *config,
                            spi_nand_flash_device_t **handle)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(config->emul_conf != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "emul_conf NULL");

    *handle = heap_caps_calloc(1, sizeof(spi_nand_flash_device_t), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(*handle != NULL, ESP_ERR_NO_MEM, TAG, "nomem");

    memcpy(&(*handle)->config, config, sizeof(spi_nand_flash_config_t));

    (*handle)->chip.ecc_data.ecc_status_reg_len_in_bits = 2;
    (*handle)->chip.ecc_data.ecc_data_refresh_threshold = 4;
    (*handle)->chip.log2_ppb       = 6;
    (*handle)->chip.log2_page_size = 11;
    (*handle)->chip.num_planes     = 1;
    (*handle)->chip.flags          = 0;
    (*handle)->chip.page_size      = 1 << (*handle)->chip.log2_page_size;
    (*handle)->chip.block_size     = (1 << (*handle)->chip.log2_ppb) * (*handle)->chip.page_size;

    ESP_GOTO_ON_ERROR(detect_chip(*handle), fail, TAG, "detect_chip failed");

    if (s_sim.initialized) {
        /* Fault sim already configured by caller via nand_fault_sim_init() */
    } else {
        /* Auto-init with FRESH config when caller didn't pre-configure */
        nand_fault_sim_config_t fresh = nand_fault_sim_config_preset(NAND_SIM_SCENARIO_FRESH);
        uint32_t ppb = 1u << (*handle)->chip.log2_ppb;
        ESP_GOTO_ON_ERROR(
            nand_fault_sim_init((*handle)->chip.num_blocks, ppb, &fresh),
            fail, TAG, "nand_fault_sim_init failed");
    }

    (*handle)->work_buffer = heap_caps_malloc((*handle)->chip.page_size, MALLOC_CAP_DEFAULT);
    ESP_GOTO_ON_FALSE((*handle)->work_buffer != NULL, ESP_ERR_NO_MEM, fail, TAG, "nomem");

    (*handle)->read_buffer = heap_caps_malloc((*handle)->chip.page_size, MALLOC_CAP_DEFAULT);
    ESP_GOTO_ON_FALSE((*handle)->read_buffer != NULL, ESP_ERR_NO_MEM, fail, TAG, "nomem");

    (*handle)->mutex = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE((*handle)->mutex != NULL, ESP_ERR_NO_MEM, fail, TAG, "nomem mutex");

    return ret;

fail:
    free((*handle)->work_buffer);
    free((*handle)->read_buffer);
    if ((*handle)->mutex) {
        vSemaphoreDelete((*handle)->mutex);
    }
    nand_emul_deinit(*handle);
    free(*handle);
    *handle = NULL;
    return ret;
}

esp_err_t nand_is_bad(spi_nand_flash_device_t *handle, uint32_t block, bool *is_bad_status)
{
    /* Check factory bad-block list first */
    if (s_sim.initialized && s_sim.cfg.factory_bad_blocks) {
        for (uint32_t i = 0; i < s_sim.cfg.factory_bad_block_count; i++) {
            if (s_sim.cfg.factory_bad_blocks[i] == block) {
                *is_bad_status = true;
                return ESP_OK;
            }
        }
    }

    uint8_t markers[4];
    size_t block_offset = 0;
    ESP_RETURN_ON_ERROR(sim_block_file_offset(handle, block, &block_offset),
                        TAG, "nand_is_bad: block offset");
    ESP_RETURN_ON_ERROR(
        nand_emul_read(handle, block_offset + handle->chip.page_size, markers, sizeof(markers)),
        TAG, "nand_is_bad: read OOB");
    *is_bad_status = (markers[0] != 0xFF || markers[1] != 0xFF);
    return ESP_OK;
}

esp_err_t nand_mark_bad(spi_nand_flash_device_t *handle, uint32_t block)
{
    size_t block_base = 0;
    ESP_RETURN_ON_ERROR(sim_block_file_offset(handle, block, &block_base),
                        TAG, "nand_mark_bad: block offset");
    ESP_RETURN_ON_ERROR(nand_emul_erase_block(handle, block_base),
                        TAG, "nand_mark_bad: erase");
    ESP_RETURN_ON_ERROR(
        nand_emul_write(handle, block_base + handle->chip.page_size,
                        s_oob_mark_bad_markers, sizeof(s_oob_mark_bad_markers)),
        TAG, "nand_mark_bad: write OOB");
    return ESP_OK;
}

esp_err_t nand_erase_block(spi_nand_flash_device_t *handle, uint32_t block)
{
    if (s_sim.initialized && s_sim.crashed) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sim.initialized) {
        bool crash_now = sim_check_crash();
        if (crash_now) {
            /* Torn erase: clear a seeded-random prefix of pages */
            uint32_t ppb  = s_sim.pages_per_block;
            uint32_t torn = (uint32_t)(rand_r(&s_sim.crash_state) % ppb);
            uint32_t base_page = block * ppb;
            for (uint32_t p = 0; p < torn; p++) {
                size_t off = sim_page_offset(handle, base_page + p);
                /* Clear each page individually (erase = fill 0xFF for page+OOB) */
                (void)nand_emul_erase_block(handle, off); /* best-effort; ignore errors */
            }
            return ESP_FAIL;
        }

        if (sim_roll_op_fail(s_sim.cfg.erase_fail_prob)) {
            return ESP_FAIL;
        }

        if (s_sim.cfg.max_erase_cycles > 0) {
            s_sim.erase_count[block]++;
            if (s_sim.erase_count[block] > s_sim.cfg.max_erase_cycles) {
                return ESP_FAIL;
            }
        }
    }

    size_t address = 0;
    ESP_RETURN_ON_ERROR(sim_block_file_offset(handle, block, &address),
                        TAG, "nand_erase_block: offset");
    ESP_RETURN_ON_ERROR(nand_emul_erase_block(handle, address),
                        TAG, "nand_erase_block: emul");
    return ESP_OK;
}

esp_err_t nand_erase_chip(spi_nand_flash_device_t *handle)
{
    esp_err_t ret = ESP_OK;
    for (uint32_t i = 0; i < handle->chip.num_blocks; i++) {
        bool is_bad = false;
        esp_err_t err = nand_is_bad(handle, i, &is_bad);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nand_erase_chip: is_bad query failed block %" PRIu32, i);
            return err;
        }
        if (is_bad) {
            continue;
        }
        err = nand_erase_block(handle, i);
        if (err == ESP_ERR_NOT_FINISHED) {
            ret = ESP_ERR_NOT_FINISHED;
        } else if (err != ESP_OK) {
            ESP_LOGE(TAG, "nand_erase_chip: erase failed block %" PRIu32, i);
            return err;
        }
    }
    return ret;
}

esp_err_t nand_prog(spi_nand_flash_device_t *handle, uint32_t page, const uint8_t *data)
{
    if (s_sim.initialized && s_sim.crashed) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sim.initialized) {
        bool crash_now = sim_check_crash();
        if (crash_now) {
            /* Torn write: write seeded-random byte prefix */
            size_t len        = handle->chip.page_size;
            size_t torn_off   = 1 + (size_t)(rand_r(&s_sim.crash_state) % (len - 1));
            size_t data_off   = sim_page_offset(handle, page);
            (void)nand_emul_write(handle, data_off, data, torn_off);
            return ESP_FAIL;
        }

        if (sim_roll_op_fail(s_sim.cfg.prog_fail_prob)) {
            return ESP_FAIL;
        }

        if (s_sim.cfg.max_prog_cycles > 0 && page < s_sim.num_pages) {
            s_sim.prog_count[page]++;
            if (s_sim.prog_count[page] > s_sim.cfg.max_prog_cycles) {
                return ESP_FAIL;
            }
        } else if (page < s_sim.num_pages) {
            s_sim.prog_count[page]++;
        }
    }

    uint32_t data_offset = page * handle->chip.emulated_page_size;
    ESP_RETURN_ON_ERROR(
        nand_emul_write(handle, data_offset, data, handle->chip.page_size),
        TAG, "nand_prog: write data");
    ESP_RETURN_ON_ERROR(
        nand_emul_write(handle, data_offset + handle->chip.page_size,
                        s_oob_used_page_markers, sizeof(s_oob_used_page_markers)),
        TAG, "nand_prog: write OOB");
    return ESP_OK;
}

esp_err_t nand_is_free(spi_nand_flash_device_t *handle, uint32_t page, bool *is_free_status)
{
    if (s_sim.initialized && s_sim.crashed) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t markers[4];
    ESP_RETURN_ON_ERROR(
        nand_emul_read(handle,
                       page * handle->chip.emulated_page_size + handle->chip.page_size,
                       markers, sizeof(markers)),
        TAG, "nand_is_free");
    *is_free_status = (markers[2] == 0xFF && markers[3] == 0xFF);
    return ESP_OK;
}

esp_err_t nand_read(spi_nand_flash_device_t *handle, uint32_t page,
                    size_t offset, size_t length, uint8_t *data)
{
    if (s_sim.initialized && s_sim.crashed) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sim.initialized && sim_roll_op_fail(s_sim.cfg.read_fail_prob)) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(
        nand_emul_read(handle,
                       page * handle->chip.emulated_page_size + offset,
                       data, length),
        TAG, "nand_read");

    if (!s_sim.initialized || page >= s_sim.num_pages) {
        return ESP_OK;
    }

    s_sim.read_count[page]++;
    uint32_t rc = s_sim.read_count[page];
    uint32_t pc = s_sim.prog_count[page];

    if (s_sim.cfg.grave_page_threshold > 0 && pc > s_sim.cfg.grave_page_threshold) {
        sim_invoke_ecc_cb(page, NAND_ECC_NOT_CORRECTED);
        return ESP_OK;
    }

    if (s_sim.cfg.ecc_fail_threshold > 0 && rc >= s_sim.cfg.ecc_fail_threshold) {
        sim_invoke_ecc_cb(page, NAND_ECC_NOT_CORRECTED);
    } else if (s_sim.cfg.ecc_high_threshold > 0 && rc >= s_sim.cfg.ecc_high_threshold) {
        sim_invoke_ecc_cb(page, NAND_ECC_4_TO_6_BITS_CORRECTED);
    } else if (s_sim.cfg.ecc_mid_threshold > 0 && rc >= s_sim.cfg.ecc_mid_threshold) {
        sim_invoke_ecc_cb(page, NAND_ECC_1_TO_3_BITS_CORRECTED);
    }

    return ESP_OK;
}

esp_err_t nand_copy(spi_nand_flash_device_t *handle, uint32_t src, uint32_t dst)
{
    if (s_sim.initialized && s_sim.crashed) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sim.initialized && sim_roll_op_fail(s_sim.cfg.copy_fail_prob)) {
        return ESP_FAIL;
    }

    uint32_t dst_offset = dst * handle->chip.emulated_page_size;
    uint32_t src_offset = src * handle->chip.emulated_page_size;
    ESP_RETURN_ON_ERROR(
        nand_emul_read(handle, (size_t)src_offset, handle->read_buffer,
                       handle->chip.page_size),
        TAG, "nand_copy: read src");
    ESP_RETURN_ON_ERROR(
        nand_emul_write(handle, (size_t)dst_offset, handle->read_buffer,
                        handle->chip.page_size),
        TAG, "nand_copy: write dst");
    ESP_RETURN_ON_ERROR(
        nand_emul_write(handle, (size_t)dst_offset + handle->chip.page_size,
                        s_oob_used_page_markers, sizeof(s_oob_used_page_markers)),
        TAG, "nand_copy: write OOB");
    return ESP_OK;
}

esp_err_t nand_get_ecc_status(spi_nand_flash_device_t *handle, uint32_t page)
{
    (void)handle;
    (void)page;
    return ESP_OK;
}
