# Linux Wear Simulation Demo — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a standalone Linux host demo that runs Dhara-managed NAND flash under a 10 K-cycle simulated workload, injects wear-out failures via the threshold model, and reports a per-block wear histogram and write-amplification factor at the end — demonstrating why Dhara's wear-leveling matters.

**Architecture:** Wire failure-model checks (`should_fail_erase/write/read`) and read-data corruption into the core emulator operation handlers (`nand_linux_mmap_emul.c`, `nand_impl_linux.c`); implement the threshold failure model; add JSON export to the sparse hash backend; build the demo app under `examples/linux_wear_simulation/` using `spi_nand_flash_write/read` (Dhara layer) over `nand_emul_advanced_init`.

**Tech Stack:** C17, ESP-IDF build system (idf.py, linux target), Catch2 (host tests), custom sparse-hash metadata backend, Dhara FTL (via `spi_nand_flash.h`).

---

## Context Snapshot

### What exists today

| Component | Status |
|-----------|--------|
| `include/nand_emul_advanced.h` | Complete — all public types, vtables, config |
| `src/nand_emul_advanced.c` | Complete lifecycle + query API; notify hooks (`notify_erase/program/read`) wired |
| `src/backends/sparse_hash_backend.c` | Complete — block/page tracking, stats, bad-block, iteration; `save_snapshot`, `load_snapshot`, `export_json` are NULL in vtable |
| `src/backends/hash_table.c` | Complete |
| `src/failure_models/noop_failure_model.c` | Complete |
| `priv_include/nand_emul_advanced_priv.h` | Has only `notify_*` declarations — no failure-check functions yet |
| `src/nand_linux_mmap_emul.c` (`nand_emul_erase_block`) | Calls `notify_erase` after success; **no failure check before erase** |
| `src/nand_impl_linux.c` (`nand_prog`, `nand_read`) | Calls `notify_program`/`notify_read` after success; **no failure check before op, no `corrupt_read_data`** |
| `host_test/main/` | 23 passing tests (P0–P2 complete) |

### What is missing

1. **P3** — Failure-model wiring: `should_fail_erase/write/read` + `corrupt_read_data` are never called.
2. **P5.T1–T4** — Threshold failure model (`src/failure_models/threshold_failure_model.c`).
3. **P4.T11–T12** — JSON export in the sparse backend (vtable slot is NULL).
4. **P6** — Demo app (`examples/linux_wear_simulation/`).

### Key files to touch

| File | Change |
|------|--------|
| `priv_include/nand_emul_advanced_priv.h` | Add `nand_emul_advanced_should_fail_erase/write/read` + `nand_emul_advanced_corrupt_read` declarations |
| `src/nand_emul_advanced.c` | Implement the four new private functions |
| `src/nand_linux_mmap_emul.c` | Call `should_fail_erase` before memset in `nand_emul_erase_block` |
| `src/nand_impl_linux.c` | Call `should_fail_write` before `nand_emul_write` in `nand_prog`; call `should_fail_read` + `corrupt_read_data` in `nand_read` |
| `src/failure_models/threshold_failure_model.c` | New file |
| `CMakeLists.txt` | Add `threshold_failure_model.c` to `srcs` |
| `src/backends/sparse_hash_backend.c` | Add `sparse_export_json`, point `export_json` in vtable |
| `host_test/main/test_integration.cpp` | New file — threshold + failure-wiring tests |
| `host_test/main/CMakeLists.txt` | Add `test_integration.cpp` |
| `examples/linux_wear_simulation/` | New example app |

### Build and test commands

```bash
# Build host tests (from spi_nand_flash/host_test/)
idf.py build

# Run all tests
./build/nand_flash_host_test.elf

# Run only integration tests
./build/nand_flash_host_test.elf "[integration]"

# Run backward-compat check (must always pass)
./build/nand_flash_host_test.elf "[backward-compat]"

# Build demo app (from spi_nand_flash/examples/linux_wear_simulation/)
idf.py build
./build/linux_wear_simulation.elf
```

### ESP_ERR codes used
- `ESP_ERR_FLASH_OP_FAIL` — operation failed (erase/write failure injection)
- `ESP_ERR_FLASH_BAD_BLOCK` — block is bad (bad-block failure injection)
- `ESP_ERR_INVALID_STATE` — advanced tracking not active
- `ESP_ERR_NOT_SUPPORTED` — backend does not implement the operation

---

## Task 1: Wire failure model into erase (P3.T2)

**Goal:** `nand_emul_erase_block` checks the failure model before erasing. If the model says fail, the operation returns an error and, if `is_block_bad` also fires, marks the block bad in the backend.

### Files
- Modify: `priv_include/nand_emul_advanced_priv.h`
- Modify: `src/nand_emul_advanced.c`
- Modify: `src/nand_linux_mmap_emul.c`
- Create: `host_test/main/test_integration.cpp`
- Modify: `host_test/main/CMakeLists.txt`

### Step 1: Write the failing test

Create `host_test/main/test_integration.cpp`:

```cpp
/*
 * Integration tests: failure model wiring into core emulator operations.
 * These tests verify that the failure model's should_fail_* callbacks are
 * actually called during erase/write/read, causing operations to fail.
 */
#include <string.h>
#include <stdlib.h>
#include "nand_emul_advanced.h"
#include "spi_nand_flash.h"
#include "nand_linux_mmap_emul.h"
#include "nand_private/nand_impl_wrap.h"
#include <catch2/catch_test_macros.hpp>

/* -----------------------------------------------------------------------
 * Helper: make a device with the threshold failure model.
 * max_erases: block fails after this many erases (0 = never).
 * max_programs: page fails after this many programs (0 = never / large).
 * ---------------------------------------------------------------------- */
static spi_nand_flash_device_t *make_threshold_dev(uint32_t max_erases,
                                                    uint32_t max_programs)
{
    static threshold_failure_config_t th;
    th.max_block_erases  = max_erases;
    th.max_page_programs = max_programs ? max_programs : 100000u;
    th.fail_over_limit   = true;

    static sparse_hash_backend_config_t be;
    be.initial_capacity      = 16;
    be.load_factor           = 0.75f;
    be.enable_histogram_query = false;

    static nand_emul_advanced_config_t cfg;
    cfg = {};
    cfg.base_config              = {"", 32 * 1024 * 1024, true};
    cfg.metadata_backend         = &nand_sparse_hash_backend;
    cfg.metadata_backend_config  = &be;
    cfg.failure_model            = &nand_threshold_failure_model;
    cfg.failure_model_config     = &th;
    cfg.track_block_level        = true;
    cfg.track_page_level         = true;

    spi_nand_flash_device_t *dev;
    REQUIRE(nand_emul_advanced_init(&dev, &cfg) == ESP_OK);
    return dev;
}

/* -----------------------------------------------------------------------
 * Erase failure tests
 * ---------------------------------------------------------------------- */

TEST_CASE("erase succeeds up to threshold, fails on threshold+1",
          "[advanced][integration][threshold-erase]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(3, 0);

    for (int i = 0; i < 3; i++) {
        REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    }
    // 4th erase must fail (threshold = 3)
    esp_err_t ret = nand_wrap_erase_block(dev, 0);
    REQUIRE(ret != ESP_OK);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("block is marked bad in metadata after erase failure",
          "[advanced][integration][threshold-erase]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(1, 0);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK); // at limit
    REQUIRE(nand_wrap_erase_block(dev, 0) != ESP_OK); // over limit → bad

    block_metadata_t meta = {};
    REQUIRE(nand_emul_get_block_wear(dev, 0, &meta) == ESP_OK);
    REQUIRE(meta.is_bad_block == true);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("other blocks unaffected when one block exceeds erase threshold",
          "[advanced][integration][threshold-erase]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(1, 0);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 0) != ESP_OK); // block 0: over limit
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK); // block 1: unaffected

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

Add `test_integration.cpp` to `host_test/main/CMakeLists.txt` SRCS list.

### Step 2: Build and verify RED

```bash
# From spi_nand_flash/host_test/
idf.py build 2>&1 | grep "error:"
```

Expected: `nand_threshold_failure_model` undefined (or header missing).

### Step 3: Add `should_fail_erase` to private header and implement it

In `priv_include/nand_emul_advanced_priv.h`, add after the existing `notify_*` declarations:

```c
/**
 * @brief Check whether the failure model wants to fail an erase on @p block_num.
 *
 * If the model says fail AND its is_block_bad() also fires, the block is
 * marked bad in the backend.
 *
 * @return true  → caller must NOT erase and must return an error code.
 * @return false → proceed normally.
 */
bool nand_emul_advanced_should_fail_erase(spi_nand_flash_device_t *dev,
                                          uint32_t block_num);
```

In `src/nand_emul_advanced.c`, implement:

```c
bool nand_emul_advanced_should_fail_erase(spi_nand_flash_device_t *dev,
                                          uint32_t block_num)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL || ctx->failure_ops == NULL) {
        return false;
    }
    if (ctx->failure_ops->should_fail_erase == NULL) {
        return false;
    }

    /* Build operation context with current block metadata */
    block_metadata_t bm = {};
    if (ctx->backend_ops && ctx->backend_ops->get_block_info) {
        ctx->backend_ops->get_block_info(ctx->backend_handle, block_num, &bm);
    }

    nand_operation_context_t op = {
        .block_num       = block_num,
        .page_num        = block_num * ctx->pages_per_block,
        .timestamp       = ctx->get_timestamp(),
        .total_blocks    = ctx->total_blocks,
        .pages_per_block = ctx->pages_per_block,
        .page_size       = ctx->page_size,
        .block_meta      = &bm,
        .page_meta       = NULL,
    };

    bool fail = ctx->failure_ops->should_fail_erase(ctx->failure_handle, &op);

    if (fail) {
        /* If the model also marks the block bad, persist that in the backend */
        if (ctx->failure_ops->is_block_bad &&
            ctx->failure_ops->is_block_bad(ctx->failure_handle, block_num, &bm) &&
            ctx->backend_ops && ctx->backend_ops->set_bad_block) {
            ctx->backend_ops->set_bad_block(ctx->backend_handle, block_num, true);
        }
    }

    return fail;
}
```

In `src/nand_linux_mmap_emul.c`, in `nand_emul_erase_block()`, add the check **before** `memset`:

```c
    /* Advanced: check failure model before erasing */
    if (nand_emul_advanced_should_fail_erase(handle, block_num)) {
        ESP_LOGW(TAG, "Simulated erase failure at block %" PRIu32, block_num);
        return ESP_ERR_FLASH_OP_FAIL;
    }
```

Place this block right after the `offset + handle->chip.block_size > flash_file_size` range check and before `void *dst_addr = ...`.

### Step 4: Build and verify GREEN

```bash
idf.py build && ./build/nand_flash_host_test.elf "[threshold-erase]"
```

Also run: `./build/nand_flash_host_test.elf "[backward-compat]"` — must still pass.

### Step 5: Commit

```bash
git add priv_include/nand_emul_advanced_priv.h \
        src/nand_emul_advanced.c \
        src/nand_linux_mmap_emul.c \
        host_test/main/test_integration.cpp \
        host_test/main/CMakeLists.txt
git commit -m "feat(nand-tracking): wire failure-model erase check (P3.T2)"
```

---

## Task 2: Threshold failure model (P5.T1–T4)

**Goal:** Implement `nand_threshold_failure_model` so blocks/pages fail after a configurable number of operations.

### Files
- Create: `src/failure_models/threshold_failure_model.c`
- Modify: `CMakeLists.txt` (component root) — add to srcs
- Modify: `include/nand_emul_advanced.h` — add `threshold_failure_config_t` and `extern` declaration

### Step 1: Write additional failing tests (append to `test_integration.cpp`)

```cpp
TEST_CASE("erase succeeds for exactly max_block_erases operations",
          "[advanced][integration][threshold-model]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(10, 0);

    for (int i = 0; i < 10; i++) {
        REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    }
    REQUIRE(nand_wrap_erase_block(dev, 0) != ESP_OK);

    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

### Step 2: Verify RED — `nand_threshold_failure_model` still undefined

### Step 3: Add config type and extern to `include/nand_emul_advanced.h`

Add after the `nand_noop_failure_model` extern (around line 332):

```c
/* ---------------------------------------------------------------------------
 * Threshold failure model
 * -------------------------------------------------------------------------*/

/**
 * @brief Configuration for the threshold failure model.
 *
 * A block fails (returns an error) after @c max_block_erases successful erases.
 * A page fails after @c max_page_programs successful programs.
 * When @c fail_over_limit is true the operation returns an error; otherwise the
 * block is silently marked bad but operations succeed (useful for testing
 * bad-block detection without hard failures).
 */
typedef struct {
    uint32_t max_block_erases;   /**< Max erases before block failure (0 = never fail) */
    uint32_t max_page_programs;  /**< Max programs before page failure (0 = never fail) */
    bool     fail_over_limit;    /**< If true, return error; if false, mark bad silently */
} threshold_failure_config_t;

/** Threshold-based failure model. */
extern const nand_failure_model_ops_t nand_threshold_failure_model;
```

### Step 4: Implement `src/failure_models/threshold_failure_model.c`

```c
/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Threshold failure model.
 *
 * Fails a block erase once the block's erase_count reaches max_block_erases.
 * Fails a page program once the page's lifetime program count reaches
 * max_page_programs.  Read operations are never failed by this model.
 *
 * The model reads current wear counts from the operation context metadata
 * pointers provided by the core (block_meta and page_meta in
 * nand_operation_context_t).
 */

#include "nand_emul_advanced.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    threshold_failure_config_t cfg;
} threshold_ctx_t;

static esp_err_t threshold_init(void **handle_out, const void *config)
{
    threshold_ctx_t *ctx = calloc(1, sizeof(threshold_ctx_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    if (config) {
        memcpy(&ctx->cfg, config, sizeof(threshold_failure_config_t));
    }
    *handle_out = ctx;
    return ESP_OK;
}

static esp_err_t threshold_deinit(void *handle)
{
    free(handle);
    return ESP_OK;
}

static bool threshold_should_fail_erase(void *handle,
                                        const nand_operation_context_t *ctx)
{
    if (!handle || !ctx) return false;
    threshold_ctx_t *th = (threshold_ctx_t *)handle;
    if (th->cfg.max_block_erases == 0) return false;

    uint32_t erase_count = 0;
    if (ctx->block_meta) {
        erase_count = ctx->block_meta->erase_count;
    }
    /* Fail if the count has already reached (or exceeded) the limit */
    return (erase_count >= th->cfg.max_block_erases);
}

static bool threshold_should_fail_write(void *handle,
                                        const nand_operation_context_t *ctx)
{
    if (!handle || !ctx) return false;
    threshold_ctx_t *th = (threshold_ctx_t *)handle;
    if (th->cfg.max_page_programs == 0) return false;

    uint32_t prog_count = 0;
    if (ctx->page_meta) {
        prog_count = ctx->page_meta->program_count_total +
                     ctx->page_meta->program_count;
    }
    return (prog_count >= th->cfg.max_page_programs);
}

static bool threshold_should_fail_read(void *handle,
                                       const nand_operation_context_t *ctx)
{
    (void)handle;
    (void)ctx;
    return false; /* Threshold model never fails reads */
}

static void threshold_corrupt_read_data(void *handle,
                                        const nand_operation_context_t *ctx,
                                        uint8_t *data, size_t len)
{
    (void)handle;
    (void)ctx;
    (void)data;
    (void)len;
    /* No bit-flip injection in threshold model */
}

static bool threshold_is_block_bad(void *handle, uint32_t block_num,
                                   const block_metadata_t *meta)
{
    if (!handle || !meta) return false;
    threshold_ctx_t *th = (threshold_ctx_t *)handle;
    if (th->cfg.max_block_erases == 0) return false;
    /* A block is bad once its erase count has reached the limit */
    return meta->erase_count >= th->cfg.max_block_erases;
}

const nand_failure_model_ops_t nand_threshold_failure_model = {
    .init              = threshold_init,
    .deinit            = threshold_deinit,
    .should_fail_erase = threshold_should_fail_erase,
    .should_fail_write = threshold_should_fail_write,
    .should_fail_read  = threshold_should_fail_read,
    .corrupt_read_data = threshold_corrupt_read_data,
    .is_block_bad      = threshold_is_block_bad,
};
```

Add `src/failure_models/threshold_failure_model.c` to the `srcs` list in the component `CMakeLists.txt`. Look for the block that sets the `srcs` variable — add it alongside `noop_failure_model.c`.

### Step 5: Build and verify GREEN

```bash
idf.py build && ./build/nand_flash_host_test.elf "[threshold-model]"
./build/nand_flash_host_test.elf "[threshold-erase]"
./build/nand_flash_host_test.elf "[backward-compat]"
```

### Step 6: Commit

```bash
git add include/nand_emul_advanced.h \
        src/failure_models/threshold_failure_model.c \
        CMakeLists.txt
git commit -m "feat(nand-tracking): add threshold failure model (P5.T1-T4)"
```

---

## Task 3: Wire failure model into write and read (P3.T4–T7)

**Goal:** `nand_prog` checks `should_fail_write` before programming. `nand_read` checks `should_fail_read` before reading and calls `corrupt_read_data` after a successful read.

### Files
- Modify: `priv_include/nand_emul_advanced_priv.h` — add `should_fail_write`, `should_fail_read`, `corrupt_read`
- Modify: `src/nand_emul_advanced.c` — implement the three new private functions
- Modify: `src/nand_impl_linux.c` — call them in `nand_prog` and `nand_read`

### Step 1: Write failing tests (append to `test_integration.cpp`)

```cpp
TEST_CASE("write succeeds up to threshold, fails on threshold+1",
          "[advanced][integration][threshold-write]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(100, 2);

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);

    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    // First two programs succeed
    REQUIRE(nand_wrap_prog(dev, 0, buf) == ESP_OK);
    REQUIRE(nand_wrap_prog(dev, 0, buf) == ESP_OK);
    // Third must fail (max_page_programs = 2)
    REQUIRE(nand_wrap_prog(dev, 0, buf) != ESP_OK);

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("read is never failed by threshold model",
          "[advanced][integration][threshold-read]")
{
    spi_nand_flash_device_t *dev = make_threshold_dev(1, 1);

    uint32_t sector_size;
    REQUIRE(spi_nand_flash_get_sector_size(dev, &sector_size) == ESP_OK);

    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    uint8_t *buf = (uint8_t *)calloc(1, sector_size);
    REQUIRE(buf != NULL);

    // Threshold model never fails reads
    for (int i = 0; i < 100; i++) {
        REQUIRE(nand_wrap_read(dev, 0, buf) == ESP_OK);
    }

    free(buf);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}
```

### Step 2: Verify RED

```bash
idf.py build && ./build/nand_flash_host_test.elf "[threshold-write]"
```

Expected: FAIL — write does not check failure model yet.

### Step 3: Add declarations to private header

In `priv_include/nand_emul_advanced_priv.h`, add:

```c
/**
 * Check whether the failure model wants to fail a page program on @p page_num.
 * @return true → caller must NOT program and must return an error code.
 */
bool nand_emul_advanced_should_fail_write(spi_nand_flash_device_t *dev,
                                          uint32_t page_num);

/**
 * Check whether the failure model wants to fail a page read on @p page_num.
 * @return true → caller must NOT copy read data and must return an error code.
 */
bool nand_emul_advanced_should_fail_read(spi_nand_flash_device_t *dev,
                                         uint32_t page_num);

/**
 * Optionally corrupt the read buffer in-place (e.g. bit flips).
 * Called after a successful memcpy from flash.  No-op if no failure model.
 */
void nand_emul_advanced_corrupt_read(spi_nand_flash_device_t *dev,
                                     uint32_t page_num,
                                     uint8_t *data, size_t len);
```

### Step 4: Implement in `src/nand_emul_advanced.c`

Pattern identical to `should_fail_erase` — retrieve page metadata via `get_page_info`, build `nand_operation_context_t`, call the vtable function:

```c
bool nand_emul_advanced_should_fail_write(spi_nand_flash_device_t *dev,
                                          uint32_t page_num)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL || ctx->failure_ops == NULL ||
        ctx->failure_ops->should_fail_write == NULL) {
        return false;
    }

    uint32_t block_num = (ctx->pages_per_block > 0)
                         ? page_num / ctx->pages_per_block : 0;

    block_metadata_t bm = {};
    page_metadata_t  pm = {};
    if (ctx->backend_ops) {
        if (ctx->backend_ops->get_block_info)
            ctx->backend_ops->get_block_info(ctx->backend_handle, block_num, &bm);
        if (ctx->backend_ops->get_page_info)
            ctx->backend_ops->get_page_info(ctx->backend_handle, page_num, &pm);
    }

    nand_operation_context_t op = {
        .block_num       = block_num,
        .page_num        = page_num,
        .timestamp       = ctx->get_timestamp(),
        .total_blocks    = ctx->total_blocks,
        .pages_per_block = ctx->pages_per_block,
        .page_size       = ctx->page_size,
        .block_meta      = &bm,
        .page_meta       = &pm,
    };

    return ctx->failure_ops->should_fail_write(ctx->failure_handle, &op);
}

bool nand_emul_advanced_should_fail_read(spi_nand_flash_device_t *dev,
                                         uint32_t page_num)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL || ctx->failure_ops == NULL ||
        ctx->failure_ops->should_fail_read == NULL) {
        return false;
    }

    uint32_t block_num = (ctx->pages_per_block > 0)
                         ? page_num / ctx->pages_per_block : 0;

    block_metadata_t bm = {};
    page_metadata_t  pm = {};
    if (ctx->backend_ops) {
        if (ctx->backend_ops->get_block_info)
            ctx->backend_ops->get_block_info(ctx->backend_handle, block_num, &bm);
        if (ctx->backend_ops->get_page_info)
            ctx->backend_ops->get_page_info(ctx->backend_handle, page_num, &pm);
    }

    nand_operation_context_t op = {
        .block_num       = block_num,
        .page_num        = page_num,
        .timestamp       = ctx->get_timestamp(),
        .total_blocks    = ctx->total_blocks,
        .pages_per_block = ctx->pages_per_block,
        .page_size       = ctx->page_size,
        .block_meta      = &bm,
        .page_meta       = &pm,
    };

    return ctx->failure_ops->should_fail_read(ctx->failure_handle, &op);
}

void nand_emul_advanced_corrupt_read(spi_nand_flash_device_t *dev,
                                     uint32_t page_num,
                                     uint8_t *data, size_t len)
{
    nand_advanced_context_t *ctx = get_ctx(dev);
    if (ctx == NULL || ctx->failure_ops == NULL ||
        ctx->failure_ops->corrupt_read_data == NULL) {
        return;
    }

    uint32_t block_num = (ctx->pages_per_block > 0)
                         ? page_num / ctx->pages_per_block : 0;

    block_metadata_t bm = {};
    page_metadata_t  pm = {};
    if (ctx->backend_ops) {
        if (ctx->backend_ops->get_block_info)
            ctx->backend_ops->get_block_info(ctx->backend_handle, block_num, &bm);
        if (ctx->backend_ops->get_page_info)
            ctx->backend_ops->get_page_info(ctx->backend_handle, page_num, &pm);
    }

    nand_operation_context_t op = {
        .block_num       = block_num,
        .page_num        = page_num,
        .timestamp       = ctx->get_timestamp(),
        .total_blocks    = ctx->total_blocks,
        .pages_per_block = ctx->pages_per_block,
        .page_size       = ctx->page_size,
        .block_meta      = &bm,
        .page_meta       = &pm,
    };

    ctx->failure_ops->corrupt_read_data(ctx->failure_handle, &op, data, len);
}
```

### Step 5: Wire into `src/nand_impl_linux.c`

In `nand_prog()`, add before the first `nand_emul_write` call:

```c
    /* Advanced: check failure model before programming */
    if (nand_emul_advanced_should_fail_write(handle, page)) {
        ESP_LOGW(TAG, "Simulated write failure at page %" PRIu32, page);
        return ESP_ERR_FLASH_OP_FAIL;
    }
```

In `nand_read()`, after `ESP_RETURN_ON_ERROR(nand_emul_read(...))` and before `nand_emul_advanced_notify_read`, add:

```c
    /* Advanced: optionally corrupt the read buffer (bit-flip injection) */
    nand_emul_advanced_corrupt_read(handle, page, data, length);
```

For `should_fail_read` — wrap the existing `nand_emul_read` call:

```c
    /* Advanced: check failure model before reading */
    if (nand_emul_advanced_should_fail_read(handle, page)) {
        ESP_LOGW(TAG, "Simulated read failure at page %" PRIu32, page);
        return ESP_ERR_FLASH_OP_FAIL;
    }
```

Place the failure check **before** `ESP_RETURN_ON_ERROR(nand_emul_read(...))`.

### Step 6: Build and verify GREEN

```bash
idf.py build
./build/nand_flash_host_test.elf "[threshold-write]"
./build/nand_flash_host_test.elf "[threshold-read]"
./build/nand_flash_host_test.elf "[backward-compat]"
./build/nand_flash_host_test.elf  # all tests
```

### Step 7: Commit

```bash
git add priv_include/nand_emul_advanced_priv.h \
        src/nand_emul_advanced.c \
        src/nand_impl_linux.c
git commit -m "feat(nand-tracking): wire failure-model write/read checks (P3.T4-T7)"
```

---

## Task 4: JSON export in sparse hash backend (P4.T11–T12)

**Goal:** `nand_emul_export_json` produces a valid JSON file with block and page wear data for offline analysis.

### Files
- Modify: `src/backends/sparse_hash_backend.c` — add `sparse_export_json`, point vtable slot
- Modify: `host_test/main/test_sparse_hash_backend.cpp` — add export test

### Step 1: Write the failing test (append to `test_sparse_hash_backend.cpp`)

```cpp
#include <cstdio>

TEST_CASE("export_json creates a non-empty JSON file starting with '{'",
          "[advanced][sparse-backend][json]")
{
    spi_nand_flash_device_t *dev = make_advanced_dev_with_sparse_backend();

    // Perform some operations so there's data to export
    REQUIRE(nand_wrap_erase_block(dev, 0) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);
    REQUIRE(nand_wrap_erase_block(dev, 1) == ESP_OK);

    const char *path = "/tmp/test_nand_export.json";
    REQUIRE(nand_emul_export_json(dev, path) == ESP_OK);

    FILE *fp = fopen(path, "r");
    REQUIRE(fp != NULL);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    int first = fgetc(fp);
    fclose(fp);

    REQUIRE(sz > 0);
    REQUIRE(first == '{');

    remove(path);
    REQUIRE(nand_emul_advanced_deinit(dev) == ESP_OK);
}

TEST_CASE("export_json returns NOT_SUPPORTED without advanced init",
          "[advanced][sparse-backend][json]")
{
    nand_file_mmap_emul_config_t conf = {"", 32 * 1024 * 1024, true};
    spi_nand_flash_config_t nand_cfg  = {&conf, 0, SPI_NAND_IO_MODE_SIO, 0};
    spi_nand_flash_device_t *dev;
    REQUIRE(spi_nand_flash_init_device(&nand_cfg, &dev) == ESP_OK);
    REQUIRE(nand_emul_export_json(dev, "/tmp/x.json") == ESP_ERR_INVALID_STATE);
    REQUIRE(spi_nand_flash_deinit_device(dev) == ESP_OK);
}
```

### Step 2: Verify RED

```bash
idf.py build && ./build/nand_flash_host_test.elf "[json]"
```

Expected: FAIL — `export_json` returns `ESP_ERR_NOT_SUPPORTED` (vtable slot is NULL).

### Step 3: Implement `sparse_export_json` in `src/backends/sparse_hash_backend.c`

Add at end of file, before the vtable definition:

```c
/* -------------------------------------------------------------------------
 * JSON export
 * ---------------------------------------------------------------------- */

typedef struct {
    FILE    *fp;
    bool     first;
} json_iter_ctx_t;

static bool json_block_cb(hash_node_t *node, void *user_data)
{
    json_iter_ctx_t  *jc = (json_iter_ctx_t *)user_data;
    block_metadata_t *bm = (block_metadata_t *)node->data;

    if (!jc->first) fprintf(jc->fp, ",\n");
    jc->first = false;

    fprintf(jc->fp,
            "    {\"block\":%"PRIu32",\"erases\":%"PRIu32
            ",\"bad\":%s,\"total_progs\":%"PRIu32"}",
            bm->block_num, bm->erase_count,
            bm->is_bad_block ? "true" : "false",
            bm->total_page_programs_total);
    return true;
}

static bool json_page_cb(hash_node_t *node, void *user_data)
{
    json_iter_ctx_t *jc = (json_iter_ctx_t *)user_data;
    page_metadata_t *pm = (page_metadata_t *)node->data;

    if (!jc->first) fprintf(jc->fp, ",\n");
    jc->first = false;

    uint32_t lifetime_progs = pm->program_count_total + pm->program_count;
    uint32_t lifetime_reads = pm->read_count_total    + pm->read_count;
    fprintf(jc->fp,
            "    {\"page\":%"PRIu32",\"lifetime_progs\":%"PRIu32
            ",\"lifetime_reads\":%"PRIu32"}",
            pm->page_num, lifetime_progs, lifetime_reads);
    return true;
}

static esp_err_t sparse_export_json(void *handle, const char *filename)
{
    sparse_hash_ctx_t *ctx = (sparse_hash_ctx_t *)handle;
    if (!ctx || !filename) return ESP_ERR_INVALID_ARG;

    FILE *fp = fopen(filename, "w");
    if (!fp) return ESP_FAIL;

    fprintf(fp, "{\n  \"blocks\": [\n");
    json_iter_ctx_t jc_b = { .fp = fp, .first = true };
    hash_table_iterate(ctx->block_table, json_block_cb, &jc_b);
    fprintf(fp, "\n  ],\n  \"pages\": [\n");

    json_iter_ctx_t jc_p = { .fp = fp, .first = true };
    hash_table_iterate(ctx->page_table, json_page_cb, &jc_p);
    fprintf(fp, "\n  ]\n}\n");

    fclose(fp);
    return ESP_OK;
}
```

Then in the `nand_sparse_hash_backend` vtable at end of file, change `.export_json = NULL` to `.export_json = sparse_export_json`.

Add `#include <inttypes.h>` and `#include <stdio.h>` at the top of `sparse_hash_backend.c` if not already present.

### Step 4: Build and verify GREEN

```bash
idf.py build && ./build/nand_flash_host_test.elf "[json]"
./build/nand_flash_host_test.elf "[backward-compat]"
./build/nand_flash_host_test.elf  # all tests
```

### Step 5: Commit

```bash
git add src/backends/sparse_hash_backend.c \
        host_test/main/test_sparse_hash_backend.cpp
git commit -m "feat(nand-tracking): add JSON export to sparse hash backend (P4.T11-T12)"
```

---

## Task 5: Linux wear simulation demo app (P6)

**Goal:** A standalone example app that:
1. Inits a 32 MB emulated NAND with sparse hash backend + threshold failure model (max 200 erases/block).
2. Runs a Dhara-layer workload: 50 K random logical sector writes over 500 sectors (simulating filesystem traffic). Tracks logical bytes via `nand_emul_record_logical_write`.
3. Every 5 000 writes, prints a mid-run wear summary (max/min/avg erases, WAF so far).
4. At the end: prints an ASCII block-erase histogram, final WAF, top-10 most-worn blocks.
5. Exports wear data to `/tmp/nand_wear_simulation.json`.

### Files
- Create: `examples/linux_wear_simulation/CMakeLists.txt`
- Create: `examples/linux_wear_simulation/main/CMakeLists.txt`
- Create: `examples/linux_wear_simulation/main/idf_component.yml`
- Create: `examples/linux_wear_simulation/main/main.c`

### Step 1: No test needed (it's an example app — we verify by running it)

### Step 2: Create `examples/linux_wear_simulation/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(linux_wear_simulation)
```

### Step 3: Create `examples/linux_wear_simulation/main/CMakeLists.txt`

```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES spi_nand_flash
                       )
```

### Step 4: Create `examples/linux_wear_simulation/main/idf_component.yml`

```yaml
dependencies:
  espressif/spi_nand_flash:
    version: '*'
    override_path: '../../../../'
```

### Step 5: Create `examples/linux_wear_simulation/main/main.c`

```c
/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * linux_wear_simulation — Demo application
 *
 * Runs a Dhara-managed NAND flash under 50 000 random logical sector writes
 * with the threshold failure model active (blocks wear out after 200 erases).
 * Prints a wear histogram and write-amplification factor at the end, then
 * exports the full per-block/page data to a JSON file.
 *
 * Build (from this directory):
 *   idf.py build
 *   ./build/linux_wear_simulation.elf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "nand_emul_advanced.h"
#include "spi_nand_flash.h"

/* -----------------------------------------------------------------------
 * Simulation parameters
 * --------------------------------------------------------------------- */
#define FLASH_SIZE_MB        32
#define FLASH_SIZE_BYTES     ((size_t)(FLASH_SIZE_MB) * 1024 * 1024)
#define TOTAL_LOGICAL_WRITES 50000
#define LOGICAL_SECTOR_SIZE  512           /* bytes per logical write */
#define NUM_LOGICAL_SECTORS  500           /* address space for random writes */
#define REPORT_INTERVAL      5000          /* print stats every N writes */
#define MAX_BLOCK_ERASES     200           /* threshold: blocks fail after this */
#define JSON_OUTPUT_PATH     "/tmp/nand_wear_simulation.json"

/* -----------------------------------------------------------------------
 * ASCII histogram helpers
 * --------------------------------------------------------------------- */
#define HIST_BINS   16
#define HIST_WIDTH  60  /* max bar width in chars */

static void print_histogram(spi_nand_flash_device_t *dev,
                             uint32_t max_erases)
{
    /* Build a manual histogram by iterating worn blocks */
    uint32_t bin_width = (max_erases / HIST_BINS) + 1;
    uint32_t counts[HIST_BINS] = {0};
    uint32_t total = 0;

    nand_emul_iterate_worn_blocks(dev,
        (bool (*)(uint32_t, block_metadata_t *, void *))
        [](uint32_t bn, block_metadata_t *bm, void *ud) -> bool {
            (void)bn;
            uint32_t (*c)[HIST_BINS] = (uint32_t (*)[HIST_BINS])((void **)ud)[0];
            uint32_t  bw             = *(uint32_t *)((void **)ud)[1];
            uint32_t  *tot           = (uint32_t *)((void **)ud)[2];
            uint32_t bin = bm->erase_count / bw;
            if (bin >= HIST_BINS) bin = HIST_BINS - 1;
            (*c)[bin]++;
            (*tot)++;
            return true;
        },
        (void *[]){counts, &bin_width, &total});

    printf("\n  Block erase count distribution (%"PRIu32" tracked blocks):\n",
           total);
    uint32_t peak = 1;
    for (int i = 0; i < HIST_BINS; i++) if (counts[i] > peak) peak = counts[i];

    for (int i = 0; i < HIST_BINS; i++) {
        uint32_t lo = (uint32_t)i * bin_width;
        uint32_t hi = lo + bin_width - 1;
        int bar_len = (int)((uint64_t)counts[i] * HIST_WIDTH / peak);
        printf("  [%4"PRIu32"-%4"PRIu32"] %5"PRIu32" |", lo, hi, counts[i]);
        for (int j = 0; j < bar_len; j++) putchar('#');
        putchar('\n');
    }
}

/* NOTE: the lambda-in-C trick above won't compile in plain C17 (lambdas are
 * C++ only).  Replace with a proper callback struct pattern in actual code.
 * See the corrected implementation note at the end of this task. */

/* -----------------------------------------------------------------------
 * Main entry point
 * --------------------------------------------------------------------- */
void app_main(void)
{
    printf("=== NAND Flash Wear Simulation ===\n");
    printf("Flash: %d MB | Logical writes: %d | Threshold: %d erases/block\n\n",
           FLASH_SIZE_MB, TOTAL_LOGICAL_WRITES, MAX_BLOCK_ERASES);

    /* ---- Setup ---- */
    threshold_failure_config_t th_cfg = {
        .max_block_erases  = MAX_BLOCK_ERASES,
        .max_page_programs = 0,   /* no write limit */
        .fail_over_limit   = true,
    };

    sparse_hash_backend_config_t be_cfg = {
        .initial_capacity       = 64,
        .load_factor            = 0.75f,
        .enable_histogram_query = false,
    };

    nand_emul_advanced_config_t adv_cfg = {
        .base_config = {
            .flash_file_name = "",      /* tempfile */
            .flash_file_size = FLASH_SIZE_BYTES,
            .keep_dump       = false,
        },
        .metadata_backend        = &nand_sparse_hash_backend,
        .metadata_backend_config = &be_cfg,
        .failure_model           = &nand_threshold_failure_model,
        .failure_model_config    = &th_cfg,
        .track_block_level       = true,
        .track_page_level        = true,
        .get_timestamp           = NULL,  /* use default monotonic counter */
    };

    spi_nand_flash_device_t *dev;
    ESP_ERROR_CHECK(nand_emul_advanced_init(&dev, &adv_cfg));

    uint32_t sector_size, block_size, sector_num;
    ESP_ERROR_CHECK(spi_nand_flash_get_sector_size(dev, &sector_size));
    ESP_ERROR_CHECK(spi_nand_flash_get_block_size(dev, &block_size));
    ESP_ERROR_CHECK(spi_nand_flash_get_capacity(dev, &sector_num));
    printf("Device: %"PRIu32" sectors × %"PRIu32" B/sector, block size %"PRIu32" B\n\n",
           sector_num, sector_size, block_size);

    /* ---- Workload ---- */
    uint8_t *write_buf = malloc(LOGICAL_SECTOR_SIZE);
    if (!write_buf) {
        printf("ERROR: out of memory\n");
        return;
    }

    srand(0xDEADBEEF);  /* reproducible */
    uint32_t write_errors = 0;
    uint32_t bad_blocks   = 0;

    for (int op = 0; op < TOTAL_LOGICAL_WRITES; op++) {
        uint32_t lsector = (uint32_t)(rand() % NUM_LOGICAL_SECTORS);
        uint32_t addr    = lsector * LOGICAL_SECTOR_SIZE;

        memset(write_buf, (uint8_t)(op & 0xFF), LOGICAL_SECTOR_SIZE);

        esp_err_t ret = spi_nand_flash_write(dev, write_buf, addr,
                                             LOGICAL_SECTOR_SIZE);
        if (ret == ESP_OK) {
            /* Track logical bytes for WAF calculation */
            nand_emul_record_logical_write(dev, LOGICAL_SECTOR_SIZE);
        } else {
            write_errors++;
        }

        /* Periodic stats */
        if ((op + 1) % REPORT_INTERVAL == 0) {
            nand_wear_stats_t stats = {};
            nand_emul_get_wear_stats(dev, &stats);
            printf("[%6d writes] max_erases=%"PRIu32"  min=%"PRIu32
                   "  avg=%"PRIu32"  variation=%.2f  WAF=%.2f  errors=%"PRIu32"\n",
                   op + 1,
                   stats.max_block_erases,
                   stats.min_block_erases,
                   stats.avg_block_erases,
                   stats.wear_leveling_variation,
                   stats.write_amplification,
                   write_errors);
        }
    }

    /* ---- Final report ---- */
    nand_wear_stats_t final_stats = {};
    nand_emul_get_wear_stats(dev, &final_stats);

    printf("\n=== Final Wear Report ===\n");
    printf("  Total logical writes : %d\n",     TOTAL_LOGICAL_WRITES);
    printf("  Write errors         : %"PRIu32"\n", write_errors);
    printf("  Max block erases     : %"PRIu32"\n", final_stats.max_block_erases);
    printf("  Min block erases     : %"PRIu32"\n", final_stats.min_block_erases);
    printf("  Avg block erases     : %"PRIu32"\n", final_stats.avg_block_erases);
    printf("  Wear variation       : %.4f\n",    final_stats.wear_leveling_variation);
    printf("  Write amplification  : %.2fx\n",   final_stats.write_amplification);

    /* Count bad blocks */
    nand_emul_iterate_worn_blocks(dev,
        /* inline via wrapper; see implementation note */
        NULL, &bad_blocks);  /* replaced with proper callback — see below */

    /* ASCII histogram */
    print_histogram(dev, final_stats.max_block_erases);

    /* JSON export */
    esp_err_t jret = nand_emul_export_json(dev, JSON_OUTPUT_PATH);
    if (jret == ESP_OK) {
        printf("\nWear data exported to %s\n", JSON_OUTPUT_PATH);
    }

    free(write_buf);
    nand_emul_advanced_deinit(dev);
    printf("\nSimulation complete.\n");
}
```

**Implementation note (C vs C++):** The lambda trick in `print_histogram` and the inline callback above are placeholders for clarity. In the actual `.c` file, replace them with named static callback functions and a context struct, as shown in the existing `sparse_hash_backend.c` patterns. The final `main.c` must compile cleanly as C17 — no C++ lambdas.

Concrete pattern to use instead of lambdas:

```c
typedef struct {
    uint32_t  counts[HIST_BINS];
    uint32_t  bin_width;
    uint32_t  total;
} hist_ctx_t;

static bool hist_block_cb(uint32_t bn, block_metadata_t *bm, void *ud)
{
    (void)bn;
    hist_ctx_t *hc  = (hist_ctx_t *)ud;
    uint32_t    bin = bm->erase_count / hc->bin_width;
    if (bin >= HIST_BINS) bin = HIST_BINS - 1;
    hc->counts[bin]++;
    hc->total++;
    return true;
}

typedef struct { uint32_t *count; } bad_ctx_t;

static bool bad_block_cb(uint32_t bn, block_metadata_t *bm, void *ud)
{
    (void)bn;
    bad_ctx_t *bc = (bad_ctx_t *)ud;
    if (bm->is_bad_block) (*bc->count)++;
    return true;
}
```

### Step 6: Build the demo app

```bash
# From spi_nand_flash/examples/linux_wear_simulation/
idf.py build
```

Fix any compile errors. Expected: clean build.

### Step 7: Run the demo app

```bash
./build/linux_wear_simulation.elf
```

Expected output (approximate):
```
=== NAND Flash Wear Simulation ===
Flash: 32 MB | Logical writes: 50000 | Threshold: 200 erases/block

Device: 16384 sectors × 2048 B/sector, block size 131072 B

[  5000 writes] max_erases=12  min=0  avg=3  variation=3.00  WAF=...  errors=0
[ 10000 writes] max_erases=25  min=0  avg=6  variation=...
...
=== Final Wear Report ===
  Total logical writes : 50000
  Write errors         : ...
  Max block erases     : ...
  ...
  Write amplification  : ...x

  Block erase count distribution (...):
  [   0-  12]   ... |###...
  ...

Wear data exported to /tmp/nand_wear_simulation.json

Simulation complete.
```

### Step 8: Commit

```bash
git add examples/linux_wear_simulation/
git commit -m "feat(nand-tracking): add Linux wear simulation demo app (P6)"
```

---

## Summary

| Task | Files changed | Tests added | Commit message |
|------|---------------|-------------|----------------|
| 1. Wire erase failure | `priv_include/nand_emul_advanced_priv.h`, `src/nand_emul_advanced.c`, `src/nand_linux_mmap_emul.c`, `host_test/main/test_integration.cpp`, `host_test/main/CMakeLists.txt` | `[threshold-erase]` | `feat(nand-tracking): wire failure-model erase check (P3.T2)` |
| 2. Threshold model | `include/nand_emul_advanced.h`, `src/failure_models/threshold_failure_model.c`, `CMakeLists.txt` | `[threshold-model]` | `feat(nand-tracking): add threshold failure model (P5.T1-T4)` |
| 3. Wire write/read | `priv_include/nand_emul_advanced_priv.h`, `src/nand_emul_advanced.c`, `src/nand_impl_linux.c` | `[threshold-write]`, `[threshold-read]` | `feat(nand-tracking): wire failure-model write/read checks (P3.T4-T7)` |
| 4. JSON export | `src/backends/sparse_hash_backend.c`, `host_test/main/test_sparse_hash_backend.cpp` | `[json]` | `feat(nand-tracking): add JSON export to sparse hash backend (P4.T11-T12)` |
| 5. Demo app | `examples/linux_wear_simulation/` | (run manually) | `feat(nand-tracking): add Linux wear simulation demo app (P6)` |

**Dependency order:** Task 1 must precede Task 2 (test uses `nand_threshold_failure_model`). Task 2 must precede Task 3 (shares same test file). Task 4 is independent. Task 5 requires Tasks 1–4.

After all tasks, run the full test suite one final time:

```bash
# From spi_nand_flash/host_test/
./build/nand_flash_host_test.elf
# Expected: all tests pass (≥ 28 test cases)
```
