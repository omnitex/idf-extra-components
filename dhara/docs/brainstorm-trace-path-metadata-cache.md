# Brainstorm: `trace_path` Metadata Read Performance

> **Date:** 2026-04-28
> **Scope:** idf-extra-components fork of Dhara, ESP32 / SPI NAND
> **Reference:** DHARA_DEEP_DIVE.md §7 + §11.3, upstream [issue #21](https://github.com/dlbeer/dhara/issues/21)

---

## Table of Contents

1. [Root Cause Dissection](#1-root-cause-dissection)
2. [The Three-Layer Optimization Plan](#2-the-three-layer-optimization-plan)
   - 2.1 [Layer 1 — NAND page-register cache (nand_impl.c)](#21-layer-1--nand-page-register-cache-nand_implc)
   - 2.2 [Layer 2 — Multi-slot metadata cache in dhara_journal (journal.c/h)](#22-layer-2--multi-slot-metadata-cache-in-dhara_journal-journalch)
   - 2.3 [Layer 3 — Sequential path memoization in trace_path (map.c)](#23-layer-3--sequential-path-memoization-in-trace_path-mapc)
3. [Expected Impact](#3-expected-impact)
4. [Architecture Summary](#4-architecture-summary)
5. [ESP32-Specific Implementation Notes](#5-esp32-specific-implementation-notes)
6. [Design Decisions & Trade-offs](#6-design-decisions--trade-offs)
7. [Known Gaps & Open Questions](#7-known-gaps--open-questions)

---

## 1. Root Cause Dissection

The problem has **two additive components** that compound each other.

### Component 1 — Repeated reads of the same checkpoint page

In `dhara_journal_read_meta()` (journal.c:515–541), the general-case path:

```c
return dhara_nand_read(j->nand, p | ppc_mask, offset, DHARA_META_SIZE, buf, err);
```

This issues a fresh `READ PAGE ADDRESS` SPI command + busy-poll + `READ FROM CACHE` **every single call**, even when multiple consecutive calls refer to the same checkpoint page (`p | ppc_mask`).

From the upstream issue #21 log:

```
blk 1, pg 15 (pg_addr 0x4F, offset 1076)  ← READ PAGE ADDRESS issued
blk 1, pg 15 (pg_addr 0x4F, offset  548)  ← READ PAGE ADDRESS issued AGAIN
blk 1, pg 15 (pg_addr 0x4F, offset  284)  ← READ PAGE ADDRESS issued AGAIN
blk 1, pg 15 (pg_addr 0x4F, offset  152)  ← READ PAGE ADDRESS issued AGAIN
```

Four identical `READ PAGE ADDRESS` commands for data already sitting in the NAND's internal page register.

There is already **one** cache slot: `page_buf` is checked via `align_eq(p, j->head, j->log2_ppc)`. But this only covers the current write head's checkpoint group — pages written since the last checkpoint. All historical metadata gets no caching whatsoever.

### Component 2 — Up to 32 distinct checkpoint page reads per `trace_path`

`trace_path()` (map.c:124–190) follows alt-pointers that point to physically diverse pages across the journal ring. Even when each hop is to a unique page, each triggers a separate SPI `READ PAGE ADDRESS` (25–100 µs NAND array access) before the subsequent `READ FROM CACHE`.

```c
// trace_path() hot path — one dhara_journal_read_meta per node:
while (depth < DHARA_RADIX_DEPTH) {
    ...
    p = meta_get_alt(meta, depth);
    if (dhara_journal_read_meta(&m->journal, p, meta, err) < 0)  // ← flash read
        return -1;
    ...
    depth++;
}
```

`DHARA_RADIX_DEPTH` is `sizeof(dhara_sector_t) << 3` = 32. For a hot journal (many written sectors), all 32 levels may be traversed → **32 separate NAND array accesses** just for metadata, before the 1 actual data read.

### The SPI NAND latency model

On SPI NAND as used in ESP32 designs:

| Operation | Latency |
|---|---|
| `READ PAGE ADDRESS` (array → internal register) | 25–100 µs |
| `READ FROM CACHE` (register → SPI bus, 132 bytes) | ~1 µs |
| `READ FROM CACHE` (register → SPI bus, 2048 bytes) | ~8 µs |

**Key insight:** The expensive operation is `READ PAGE ADDRESS`, not how many bytes you read afterward. Reading 132 bytes vs 2048 bytes costs only ~7 µs extra but the array access dominates at 25–100 µs. This has two consequences:

1. Caching at the full-page level is nearly free — loading 2048 bytes costs essentially the same as loading 132 bytes.
2. Eliminating repeated `READ PAGE ADDRESS` for the same physical page is the highest-leverage optimization available.

**Measured data point (upstream issue #21):** The reporter implemented an 11-entry metadata-slice cache (1.3 KiB) and achieved a **2.52× read speedup** (from 271 KB/s to ~682 KB/s). The upstream author (dlbeer) indicated willingness to accept a patch "with user-supplied memory" for the cache.

---

## 2. The Three-Layer Optimization Plan

Three independent, additive layers. Each can be deployed separately. Combined, they cover all access patterns.

### 2.1 Layer 1 — NAND page-register cache (`nand_impl.c`)

**Scope:** `spi_nand_flash/src/nand_impl.c`, `spi_nand_flash/priv_include/nand.h` (or equivalent struct header)
**RAM cost:** ~6 bytes (`uint32_t` + `uint8_t` + `bool` in `spi_nand_flash_device_t`)
**Dhara core changes:** None

#### Problem

`read_page_and_wait()` is called unconditionally on every NAND array access — it issues `READ PAGE ADDRESS` and busy-polls regardless of whether that page is already sitting in the NAND's internal register from the previous call. Every function in `nand_impl.c` that touches the NAND hardware goes through it: `nand_read`, `nand_prog`, `nand_is_bad`, `nand_is_free`, `nand_copy`, `nand_get_ecc_status`, and `nand_mark_bad`. There is no shared mechanism to skip the array access when the register already holds the requested page.

#### BDL path analysis — Layer 1 is effective for both modes

> ⚠️ **Important correction from earlier analysis:** Layer 1 at `nand_impl.c` level **does work with BDL enabled**. The call chain in BDL mode for a Dhara metadata read is:
>
> ```
> dhara_nand_read()  [dhara_glue.c]
>   └─ bdl_handle->ops->read()  [nand_flash_blockdev.c: nand_flash_blockdev_read()]
>        └─ nand_read()  [nand_impl.c]
>             └─ read_page_and_wait()  ← Layer 1 cache lives here
> ```
>
> `nand_flash_blockdev_read()` calls `nand_read()`, which calls `read_page_and_wait()` — the same function that Layer 1 patches. The cache fires on repeated same-page reads whether the call originates from `dhara_glue.c` directly (non-BDL) or via `nand_flash_blockdev_read` (BDL).
>
> The additional indirection through `nand_flash_blockdev_read` adds negligible overhead (one function call, a few pointer dereferences). Layer 1 is therefore **fully effective in BDL mode** with no structural changes needed.

#### ECC status note for BDL mode

In `dhara_nand_read()` (dhara_glue.c), the ECC error check after a BDL read is:

```c
ret = bdl_handle->ops->read(bdl_handle, data, length,
                            (p * bdl_handle->geometry.read_size) + offset, length);
// ...
if (dev_handle->chip.ecc_data.ecc_corrected_bits_status == NAND_ECC_NOT_CORRECTED) {
    dhara_set_error(err, DHARA_E_ECC);
}
```

`nand_flash_blockdev_read()` calls `nand_read()`, which calls `is_ecc_error()` and sets `ecc_corrected_bits_status` in the device struct. The glue layer then reads that field. This chain is intact regardless of whether Layer 1 skips `read_page_and_wait` — when a cache hit occurs, the ECC status was already evaluated on the original page load. The cached-hit path must **not** re-read or reset `ecc_corrected_bits_status`; it is still valid from the prior load. See §5 ECC interaction note.

#### Fix

The cache belongs inside `read_page_and_wait()` itself — not in `nand_read()`.

`read_page_and_wait()` is the single static function in `nand_impl.c` that issues the expensive `READ PAGE ADDRESS` SPI command and busy-polls for completion. **Every** path that triggers a NAND array access calls it: `nand_read`, `nand_prog`, `nand_is_bad`, `nand_is_free`, `nand_copy`, `nand_get_ecc_status`, and `nand_mark_bad`. Placing the cache here means:

- All callers automatically update `last_loaded_page` as a side effect — no per-caller instrumentation needed.
- Invalidation collapses to exactly two sites: `program_execute_and_wait()` and `spi_nand_erase_block()`, both static functions in the same file.
- The status byte from `wait_for_ready` is cached alongside the page number, so callers that need it (ECC checks in `nand_read`, `nand_copy`) still receive it correctly on a hit.

```c
// Add to spi_nand_flash_device_t:
uint32_t last_loaded_page;    // UINT32_MAX = invalid / not cached
uint8_t  last_loaded_status;  // STATUS register value from last load
bool     nand_page_cache_valid;

// Modified read_page_and_wait():
static esp_err_t read_page_and_wait(spi_nand_flash_device_t *dev,
                                     uint32_t page, uint8_t *status_out)
{
    if (dev->nand_page_cache_valid && dev->last_loaded_page == page) {
        // Page already in NAND register — skip READ PAGE ADDRESS entirely
        if (status_out) *status_out = dev->last_loaded_status;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(spi_nand_read_page(dev, page), TAG, "");
    uint8_t status = 0;
    esp_err_t ret = wait_for_ready(dev, dev->chip.read_page_delay_us, &status);
    if (ret == ESP_OK) {
        dev->last_loaded_page   = page;
        dev->last_loaded_status = status;
        dev->nand_page_cache_valid = true;
    }
    if (status_out) *status_out = status;
    return ret;
}
```

`nand_read()`, `nand_is_bad()`, `nand_is_free()`, `nand_copy()`, `nand_get_ecc_status()`, and `nand_mark_bad()` all call `read_page_and_wait()` and require **no changes** — they gain the cache transparently.

#### Invalidation

Only two operations destroy the register contents after `read_page_and_wait` has loaded a page:

1. **`program_execute_and_wait()`** — after `PROGRAM EXECUTE` the register state is chip-dependent (some chips leave the last-loaded page intact, others do not — see G2). Safe default: invalidate unconditionally.

2. **`spi_nand_erase_block()`** — erase leaves the register undefined.

Both are static functions in `nand_impl.c`:

```c
static esp_err_t program_execute_and_wait(spi_nand_flash_device_t *dev,
                                           uint32_t page, uint8_t *status_out)
{
    ESP_RETURN_ON_ERROR(spi_nand_program_execute(dev, page), TAG, "");
    esp_err_t ret = wait_for_ready(dev, dev->chip.program_page_delay_us, status_out);
    dev->nand_page_cache_valid = false;  // register state undefined post-PROGRAM EXECUTE
    return ret;
}
```

```c
// In nand_erase_block(), after spi_nand_erase_block() call:
dev->nand_page_cache_valid = false;  // register state undefined post-ERASE
```

```c
// In nand_init_device(), after struct is zeroed:
handle->last_loaded_page    = UINT32_MAX;
handle->nand_page_cache_valid = false;
```

That is the **complete invalidation surface** — three one-liners in `nand_impl.c`, no cross-file changes required.

> ⚠️ **`nand_prog` ordering — no hazard with this approach:**
>
> `nand_prog` calls `read_page_and_wait(page)` (sets cache valid), then `program_execute_and_wait(page)` (sets `valid = false`). Between those two calls the cache is transiently valid, but no concurrent reader can observe this because all operations are serialized under `handle->mutex`. After `program_execute_and_wait` returns, `valid = false` and the next `read_page_and_wait` for any page will perform a real array access. No window exists where stale data could be served.

> ⚠️ **`nand_is_bad` / `nand_is_free` interleaving — handled automatically:**
>
> These functions call `read_page_and_wait` for a bad-block-marker or page-status page, which updates `last_loaded_page` to that page. A subsequent `nand_read` for a different page will see a miss and reload correctly. A subsequent `nand_read` for the *same* page will see a hit — which is correct, since the register still holds that page. No special handling needed; the single cache field in `read_page_and_wait` keeps itself consistent.
>
> **In BDL mode** these same paths are routed through `ioctl(IS_BAD_BLOCK)` and `ioctl(IS_FREE_PAGE)` in `nand_flash_blockdev.c`, which call `nand_is_bad()` / `nand_is_free()` — the same `nand_impl.c` functions. Behaviour is identical for both modes.

The existing mutex in `spi_nand_flash_device_t` already serializes all NAND operations, so this is safe with no additional locking.

#### Impact

From the issue's log: block 1, pg 15 is hit 4 consecutive times during a single `trace_path` traversal. With this fix: **1 `READ PAGE ADDRESS` + 3 `READ FROM CACHE`** instead of 4 `READ PAGE ADDRESS`. For sequential access workloads this alone gives 2–3× speedup on the metadata-read path.

The benefit is **identical for BDL and non-BDL modes** because the cache sits in `read_page_and_wait()`, the single chokepoint through which every NAND array access passes in both paths.

---

### 2.2 Layer 2 — Multi-slot metadata cache in `dhara_journal` (`journal.c/h`)

**Scope:** `dhara/dhara/dhara/journal.h`, `dhara/dhara/dhara/journal.c`, `spi_nand_flash/src/dhara_glue.c`
**RAM cost:** `N × page_size` bytes, DMA-aligned DRAM. Default: 4 slots × 2048 bytes = **8 KiB**. Kconfig-tunable.
**Dhara core changes:** `journal.h` (new fields), `journal.c` (`dhara_journal_read_meta` + init/clear), optional new init API

#### Problem

`dhara_journal_read_meta()` has only one cache slot (`page_buf` for the head's checkpoint group). All other checkpoint groups are read from flash unconditionally. When `trace_path` visits nodes across multiple checkpoint groups in a single call, each group's checkpoint page is re-loaded from NAND on every `trace_path` invocation.

#### BDL path analysis — Layer 2 is effective for both modes

Layer 2 operates entirely at the Dhara journal level — above `dhara_nand_read`. It intercepts calls in `dhara_journal_read_meta()` before they ever reach `dhara_nand_read`, let alone the BDL/non-BDL fork. A cache hit in Layer 2 performs a `memcpy` from DRAM and returns immediately; neither BDL nor `nand_read()` is called at all. Layer 2 is therefore **fully effective regardless of BDL mode**, by construction.

#### Design: N-slot direct-mapped cache, full checkpoint-page granularity

**Why cache full pages (2048 bytes) instead of 132-byte meta slices?**

On SPI NAND, `READ PAGE ADDRESS` (array access) costs the same whether you subsequently read 132 or 2048 bytes. A single full-page load on a cache miss amortizes the array-access cost across all `(2^log2_ppc - 1)` metadata slots in the checkpoint group — typically 7. Any subsequent access to another user page in the same group is served from RAM.

**Cache allocation: external, caller-supplied buffers**

Following the same pattern as the existing `page_buf`, the cache buffers are allocated by the caller (ESP-IDF glue layer) and passed to the journal. This keeps `dhara_journal` free of dynamic allocation and lets the caller control memory placement (internal DRAM, DMA-aligned).

```c
// journal.h — new fields in struct dhara_journal:

#ifndef DHARA_META_CACHE_SLOTS
#define DHARA_META_CACHE_SLOTS  4  /* override via Kconfig */
#endif

struct dhara_journal {
    /* ... existing fields unchanged ... */

#if DHARA_META_CACHE_SLOTS > 0
    /* Multi-slot checkpoint-page cache.
     * cache_bufs[i]  — pointer to a page-sized DMA buffer (caller-allocated)
     * cache_keys[i]  — checkpoint page index stored in slot i (DHARA_PAGE_NONE = empty)
     * cache_slots    — actual number of slots (may be < DHARA_META_CACHE_SLOTS)
     * cache_hand     — round-robin eviction counter
     */
    uint8_t        **cache_bufs;
    dhara_page_t    *cache_keys;
    uint8_t          cache_slots;
    uint8_t          cache_hand;
#endif
};
```

**New optional init call (backward-compatible):**

```c
// journal.h — new declaration:

/* Attach an external metadata cache to the journal.
 * cache_bufs: array of cache_slots pointers, each pointing to a
 *             DMA-aligned buffer of (1 << nand->log2_page_size) bytes.
 * cache_keys: array of cache_slots dhara_page_t values (caller-zeroed to
 *             DHARA_PAGE_NONE before first call).
 * Must be called after dhara_journal_init() and before dhara_journal_resume().
 */
void dhara_journal_set_meta_cache(struct dhara_journal *j,
                                   uint8_t **cache_bufs,
                                   dhara_page_t *cache_keys,
                                   uint8_t cache_slots);
```

**Modified `dhara_journal_read_meta()`:**

```c
int dhara_journal_read_meta(struct dhara_journal *j, dhara_page_t p,
                             uint8_t *buf, dhara_error_t *err)
{
    const dhara_page_t ppc_mask = (1 << j->log2_ppc) - 1;
    const size_t offset = hdr_user_offset(p & ppc_mask);

    /* 1. Buffered write path — head group in page_buf (existing, unchanged) */
    if (align_eq(p, j->head, j->log2_ppc)) {
        memcpy(buf, j->page_buf + offset, DHARA_META_SIZE);
        return 0;
    }

    /* 2. Recovery path (existing, unchanged) */
    if ((j->recover_meta != DHARA_PAGE_NONE) &&
            align_eq(p, j->recover_root, j->log2_ppc))
        return dhara_nand_read(j->nand, j->recover_meta,
                               offset, DHARA_META_SIZE, buf, err);

#if DHARA_META_CACHE_SLOTS > 0
    if (j->cache_slots > 0) {
        const dhara_page_t cp_page = p | ppc_mask;

        /* 3. Cache lookup (linear — slots count is 2–8, branch-predictor friendly) */
        for (int i = 0; i < j->cache_slots; i++) {
            if (j->cache_keys[i] == cp_page) {
                memcpy(buf, j->cache_bufs[i] + offset, DHARA_META_SIZE);
                return 0;  /* cache hit — zero NAND I/O */
            }
        }

        /* 4. Cache miss — evict via round-robin, load full checkpoint page */
        const int slot = j->cache_hand;
        j->cache_hand = (j->cache_hand + 1) % j->cache_slots;

        /* Loading full page costs same as 132 bytes on SPI NAND.
         * Amortizes array-read cost across all 7 metadata slots in group. */
        if (dhara_nand_read(j->nand, cp_page,
                            0, 1 << j->nand->log2_page_size,
                            j->cache_bufs[slot], err) < 0) {
            j->cache_keys[slot] = DHARA_PAGE_NONE;  /* invalidate on error */
            return -1;
        }
        j->cache_keys[slot] = cp_page;
        memcpy(buf, j->cache_bufs[slot] + offset, DHARA_META_SIZE);
        return 0;
    }
#endif

    /* 5. No cache — original path */
    return dhara_nand_read(j->nand, p | ppc_mask,
                           offset, DHARA_META_SIZE, buf, err);
}
```

**Cache invalidation:**

> ⚠️ **Hook point note:** The plan references a function called `push_meta()` for the checkpoint-write invalidation hook. This function does **not exist by that name** in the actual `journal.c`. The checkpoint write logic is buried inside `dhara_journal_enqueue` and its internal helpers (the file is 919 lines). The implementer must locate the exact point where a new checkpoint page is committed to flash — likely inside a static function such as `checkpoint()` or similar — and add the invalidation there. Grep for `dhara_nand_prog` calls in `journal.c` to find the correct hook sites.

```c
// In dhara_journal_clear() — add after existing logic:
#if DHARA_META_CACHE_SLOTS > 0
    for (int i = 0; i < j->cache_slots; i++)
        j->cache_keys[i] = DHARA_PAGE_NONE;
    j->cache_hand = 0;
#endif

// At the checkpoint-page write site (locate via dhara_nand_prog calls in journal.c):
// When a checkpoint page is written, invalidate any cache slot holding the old version
// of that physical page (the same physical page may have been cached from a prior read).
#if DHARA_META_CACHE_SLOTS > 0
    const dhara_page_t written_cp = old_head | ((1 << j->log2_ppc) - 1);
    for (int i = 0; i < j->cache_slots; i++)
        if (j->cache_keys[i] == written_cp)
            j->cache_keys[i] = DHARA_PAGE_NONE;
#endif
```

> **Recovery path and cache interaction:** The recovery fast path (step 2 in `dhara_journal_read_meta`) uses `recover_meta` — a page specially dumped at recovery start — and bypasses the cache entirely. This is correct: `recover_meta` is a one-off page whose identity changes every recovery cycle, and the `align_eq` guard ensures it's served before the cache lookup. A stale cache entry for `recover_meta` would never be served because recovery path is checked first. No additional handling required.

> **Erase and stale cache entries:** When Dhara erases a block (via `dhara_nand_erase`), the physical pages in that block are effectively freed. Cache entries for checkpoint pages in erased blocks will contain stale (all-0xFF) data after erase. Dhara's ring structure ensures erased blocks are only reused after the journal tail has advanced past them, meaning no live `trace_path` call will reference those pages. However, if a cache slot still holds the old key for an erased page and Dhara later writes a *new* checkpoint to the same physical page address (ring wrap), the cache would serve the old data until the key is explicitly invalidated. The checkpoint-write invalidation above handles this case: the newly written `written_cp` key will be invalidated before the cache is populated with the fresh version on the next read.

**Allocation in `dhara_glue.c`:**

> ⚠️ **`spi_nand_flash_dhara_priv_data_t` must be extended.** The plan references `dhara_priv_data->cache_bufs` and `dhara_priv_data->cache_keys`, but these fields do not exist in the current struct. The struct definition in `dhara_glue.c` must be updated:

```c
// Extended spi_nand_flash_dhara_priv_data_t (dhara_glue.c):
typedef struct {
    struct dhara_nand dhara_nand;
    struct dhara_map  dhara_map;
#ifdef CONFIG_NAND_FLASH_ENABLE_BDL
    esp_blockdev_handle_t bdl_handle;
#endif
    spi_nand_flash_device_t *parent_handle;
#if DHARA_META_CACHE_SLOTS > 0
    /* Persistent storage for cache pointer and key arrays.
     * Must outlive dhara_init() — cannot be stack-local. */
    uint8_t     *meta_cache_bufs[DHARA_META_CACHE_SLOTS];
    dhara_page_t meta_cache_keys[DHARA_META_CACHE_SLOTS];
#endif
} spi_nand_flash_dhara_priv_data_t;
```

> ⚠️ **Stack lifetime bug to avoid:** `cache_bufs` and `cache_keys` must **not** be declared as local arrays in `dhara_init()`. They would go out of scope when `dhara_init` returns, leaving the journal with dangling pointers. They must live in the `dhara_priv_data` struct (heap-allocated via `heap_caps_calloc`).

```c
// In dhara_init():
#if DHARA_META_CACHE_SLOTS > 0
    size_t dma_alignment = spi_nand_get_dma_alignment();
    bool cache_ok = true;

    for (int i = 0; i < DHARA_META_CACHE_SLOTS; i++) {
        dhara_priv_data->meta_cache_bufs[i] = heap_caps_aligned_alloc(
            dma_alignment, handle->chip.page_size,
            MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        dhara_priv_data->meta_cache_keys[i] = DHARA_PAGE_NONE;
        if (!dhara_priv_data->meta_cache_bufs[i]) { cache_ok = false; break; }
    }

    if (cache_ok) {
        dhara_journal_set_meta_cache(
            &dhara_priv_data->dhara_map.journal,
            dhara_priv_data->meta_cache_bufs,
            dhara_priv_data->meta_cache_keys,
            DHARA_META_CACHE_SLOTS);
    }
    /* If allocation failed, journal runs without cache — graceful degradation */
#endif
```

> ⚠️ **`dhara_init()` is called by both BDL and non-BDL init paths.** In non-BDL mode, `dhara_init` is called from `nand.c` (via `dev->ops->init`). In BDL mode, it is called from `spi_nand_flash_wl_get_blockdev()` → `dev->ops->init(dev, nand_bdl)`. Both paths invoke the same `dhara_init` function pointer; the Layer 2 cache allocation code inside `dhara_init` therefore runs for both modes without any `#ifdef CONFIG_NAND_FLASH_ENABLE_BDL` gating. This is correct — Layer 2 benefits both paths equally.

> **Free in `dhara_deinit()`:** The loop that allocated `meta_cache_bufs[i]` must be mirrored with a free loop. The existing `free(handle->ops_priv_data)` only frees the struct shell — it does not free the individually heap-allocated DMA buffers pointed to by `meta_cache_bufs[i]`.

```c
// In dhara_deinit(), before free(dhara_priv_data):
#if DHARA_META_CACHE_SLOTS > 0
    for (int i = 0; i < DHARA_META_CACHE_SLOTS; i++)
        free(dhara_priv_data->meta_cache_bufs[i]);
#endif
```

> ⚠️ **`dhara_deinit()` path differs between modes.** In non-BDL mode, `dhara_deinit` is called from `nand_wl_detach_ops()` → `free(handle->ops_priv_data)`. In BDL mode, `spi_nand_flash_wl_blockdev_release()` calls `nand_wl_detach_ops(dev_handle)` which calls `dhara_deinit` via `dev->ops->deinit`. Both paths converge on the same `dhara_deinit` function, so the free loop above covers both.

#### Why NOT PSRAM

Cache buffers must live in **internal DRAM**. PSRAM has ~50–100 ns cache-miss penalty on ESP32-S3. A `memcpy(132 bytes from PSRAM)` would be slower than just re-reading from NAND. Only IRAM/DRAM gives the 10–30 ns access times that make this cache worthwhile. The `MALLOC_CAP_INTERNAL` flag in the allocation above enforces this.

#### RAM accounting

The existing `spi_nand_flash_device_t` already allocates three DMA-aligned internal DRAM buffers (nand_impl.c lines 111–117):

| Buffer | Size |
|---|---|
| `work_buffer` | `page_size` = 2048 bytes |
| `read_buffer` | `page_size` = 2048 bytes |
| `temp_buffer` | `page_size + dma_alignment` ≈ 2064+ bytes |

**Existing DMA DRAM footprint: ~6.1 KiB** before this change.

Layer 2 adds `DHARA_META_CACHE_SLOTS × page_size` bytes:

| Slots | Additional DRAM | Total DMA DRAM |
|---|---|---|
| 0 (disabled) | 0 | ~6.1 KiB |
| 2 | 4 KiB | ~10.1 KiB |
| 4 (default) | 8 KiB | ~14.1 KiB |
| 8 | 16 KiB | ~22.1 KiB |

On ESP32 (520 KB SRAM) or ESP32-S3 (512 KB SRAM), the default 4-slot configuration consumes ~2.7% of total SRAM in DMA flash buffers.

#### Cache slot count vs. hit-rate analysis

The benefit of additional slots depends on `log2_ppc` (checkpoint period):

- With `log2_ppc = 3` (default: 8 pages/checkpoint, 7 user pages per group), each checkpoint page covers 7 `trace_path` hops.
- A 32-level `trace_path` traversal can visit up to 32 distinct checkpoint pages (worst case: each hop is in a different group).
- **4 slots** helps on repeated calls to `trace_path` for nearby sectors (warm cache across calls), but a single cold `trace_path` still hits up to 32 distinct pages → 28 misses.
- The primary benefit of Layer 2 is **cross-call warm-up**: the checkpoint pages visited by sector N are likely revisited by sector N+1. 4 slots retains the most-recently-visited pages, which are the ones Layer 3 would skip anyway.
- For workloads with poor locality (random reads), even 8 slots may not warm the cache sufficiently. The default of 4 is a practical middle ground; `DHARA_META_CACHE_SLOTS = 0` should be the recommended setting for severely RAM-constrained targets.

---

### 2.3 Layer 3 — Sequential path memoization in `trace_path()` (`map.c`)

**Scope:** `dhara/dhara/dhara/map.c`, `dhara/dhara/dhara/map.h`
**RAM cost:** `DHARA_RADIX_DEPTH × 4 + 4 + 4 = 136 bytes` in `struct dhara_map`
**Dhara core changes:** `map.h` (new fields), `map.c` (`trace_path` + invalidation in `dhara_map_write`, `dhara_map_gc`)

#### Problem

For sequential reads (sector N, N+1, N+2...) — the most common access pattern in filesystem workloads — every `trace_path` call starts over from the journal root and re-traverses the entire path from the top. Sectors N and N+1 differ only in their least-significant bit, meaning their radix-tree paths are identical for the first 31 levels and diverge only at depth 31. Yet 31 redundant `dhara_journal_read_meta` calls are issued for every sector.

#### BDL path analysis — Layer 3 is effective for both modes

Layer 3 operates entirely within `trace_path()` in `map.c` — above the journal layer and completely above the BDL/non-BDL split. It reduces the number of calls to `dhara_journal_read_meta`, which in turn reduces calls to `dhara_nand_read`, regardless of which I/O path `dhara_nand_read` takes. Layer 3 is **fully effective regardless of BDL mode**, by construction.

#### Design: memoize the last successful trace path

```c
// map.h — add to struct dhara_map:
#ifndef DHARA_MAP_PATH_CACHE
#define DHARA_MAP_PATH_CACHE  1  /* disable via Kconfig if needed */
#endif

#if DHARA_MAP_PATH_CACHE
    dhara_sector_t  prev_target;                    /* DHARA_SECTOR_NONE = invalid */
    dhara_page_t    prev_path[DHARA_RADIX_DEPTH];   /* physical page at each depth */
    dhara_page_t    prev_root;                      /* journal root at time of trace */
#endif
```

**Modified `trace_path()` — read-only path optimization:**

```c
static int trace_path(struct dhara_map *m, dhara_sector_t target,
                       dhara_page_t *loc, uint8_t *new_meta,
                       dhara_error_t *err)
{
    uint8_t meta[DHARA_META_SIZE];
    int depth = 0;
    dhara_page_t p = dhara_journal_root(&m->journal);

    if (new_meta) meta_set_id(new_meta, target);
    if (p == DHARA_PAGE_NONE) goto not_found;

#if DHARA_MAP_PATH_CACHE
    /* Path shortcut: only safe for pure lookups (new_meta == NULL).
     * Writes must rebuild the full alt-pointer array from scratch.
     * Also requires the journal root to be unchanged since last trace. */
    if (!new_meta &&
        m->prev_root == p &&
        m->prev_target != DHARA_SECTOR_NONE) {

        /* Find the first bit where target differs from prev_target (MSB first).
         * d_bit(0) = MSB (bit 31), d_bit(31) = LSB (bit 0).
         * diverge = 31 for consecutive sectors (differ only in LSB). */
        const dhara_sector_t xor_val = target ^ m->prev_target;
        int diverge = DHARA_RADIX_DEPTH;
        if (xor_val != 0) {
            /* __builtin_clz gives the number of leading zeros = index of highest set bit.
             * d_bit(d) = 1 << (31 - d), so the MSB of xor_val corresponds to diverge = (31 - clz). */
            diverge = 31 - __builtin_clz(xor_val);
        }

        if (diverge > 0) {
            /* Jump to the divergence point — skip diverge metadata reads */
            p = m->prev_path[diverge - 1];
            depth = diverge;

            if (p == DHARA_PAGE_NONE) goto not_found;

            if (dhara_journal_read_meta(&m->journal, p, meta, err) < 0)
                return -1;
        }
    }
#endif

    /* Standard traversal from `depth` (0 if no shortcut, diverge otherwise) */
    while (depth < DHARA_RADIX_DEPTH) {
        const dhara_sector_t id = meta_get_id(meta);
        if (id == DHARA_SECTOR_NONE) goto not_found;

        if ((target ^ id) & d_bit(depth)) {
            if (new_meta) meta_set_alt(new_meta, depth, p);
            p = meta_get_alt(meta, depth);
            if (p == DHARA_PAGE_NONE) { depth++; goto not_found; }
            if (dhara_journal_read_meta(&m->journal, p, meta, err) < 0)
                return -1;
        } else {
            if (new_meta)
                meta_set_alt(new_meta, depth, meta_get_alt(meta, depth));
        }

#if DHARA_MAP_PATH_CACHE
        if (!new_meta) m->prev_path[depth] = p;
#endif
        depth++;
    }

    if (loc) *loc = p;

#if DHARA_MAP_PATH_CACHE
    if (!new_meta) {
        m->prev_target = target;
        m->prev_root   = dhara_journal_root(&m->journal);
    }
#endif
    return 0;

not_found:
    ...
}
```

> **`diverge` calculation — use `__builtin_clz` instead of a linear scan:**
>
> The original draft used a 32-iteration loop scanning for the first set bit. On every `trace_path` call for a random-access workload (the common case), this loop runs to completion and wastes ~32 branch-and-test operations. `__builtin_clz` compiles to a single `CLZ` instruction on Xtensa (ESP32) and RISC-V (ESP32-C/H/P series). The revised code above uses `__builtin_clz` and correctly maps the leading-zero count to the `d_bit`-indexed diverge depth.
>
> Note: `__builtin_clz(0)` is undefined behavior. The `xor_val != 0` guard above prevents this — when `target == prev_target`, `diverge` stays at `DHARA_RADIX_DEPTH` and the shortcut (correctly) does nothing.

**Invalidation — critical correctness requirement:**

The memoized path is only valid while the journal root is unchanged. The root changes after every successful `dhara_journal_enqueue()` or `dhara_journal_copy()`. The cheapest invalidation: store `prev_root` and compare at the top of `trace_path`. If `prev_root != current root`, fall through to full traversal. No explicit invalidation calls needed.

This is already handled in the design above: the guard `m->prev_root == p` (where `p` is the current root) rejects stale cached paths automatically.

> **GC interaction note:** `raw_gc()` in `map.c` calls `trace_path` with both `new_meta == NULL` (find step) and non-NULL (rewrite step). The find step will use the path cache; the rewrite step is blocked by `new_meta != NULL`. After the GC rewrite, `dhara_journal_copy` changes the root, so the path cache is automatically stale-detected on the next call. `pad_queue()` calls `dhara_journal_read_meta` directly (bypassing `trace_path`) — this is benign, as it doesn't touch the path cache. No additional invalidation is needed.

#### Impact

For purely sequential reads (sector N → N+1):
- XOR of consecutive sectors differs only in bit 0 → `diverge = 31`
- 31 of 32 `dhara_journal_read_meta` calls skipped
- Combined with Layer 2's cache warming, even the 1 remaining call is likely a RAM hit

For random reads: `diverge = 0` → no skip → full traversal (no regression, just no gain).
For writes: `new_meta != NULL` → shortcut disabled → no regression on write path.

---

## 3. Expected Impact

| Scenario | Current (SPI txns per `trace_path`) | After L1 | After L1 + L2 | After L1 + L2 + L3 |
|---|---|---|---|---|
| Sequential reads (N, N+1...) | ~11 SPI txns | ~4–5 | ~2 | **~1–2** |
| Random reads (scattered sectors) | ~10–32 SPI txns | ~10 (no page repeats) | ~2–3 (warm cache) | ~10 (no locality) |
| Writes (`prepare_write`) | ~10–32 SPI txns | ~10 | ~2–3 (cache warm from prior read) | ~3–5 |
| GC (`raw_gc` per page) | ~2 × 32 SPI txns | ~2 × 10 | ~2 × 2 | ~2 × 2 |

These figures apply **identically to BDL and non-BDL modes**. All three layers are positioned above or at the lowest common function in both call paths (`nand_read()` for L1; above `dhara_nand_read` entirely for L2 and L3).

**Measured reference point:** The issue #21 reporter achieved **2.52× speedup** (271 → ~682 KB/s) with a simpler 11-entry meta-slice cache. A full-page checkpoint cache (Layer 2) + NAND register cache (Layer 1) should equal or exceed this because:
- Full-page loads on miss are no more expensive than slice loads on SPI NAND
- Each miss amortizes cost across 7 slots (vs 1 with slice cache)
- Layer 1 eliminates the repeated array-load overhead that the slice cache doesn't address

---

## 4. Architecture Summary

```
dhara_map_read(sector)
  │
  ├─ [Layer 3] trace_path() — start from divergence if prev_path valid
  │    │  (sequential: skip 31/32 levels; random: full traversal)
  │    │  [BDL + non-BDL: identical — above dhara_nand_read]
  │    │
  │    └─ dhara_journal_read_meta(p)  ← called 1–32 times
  │         │
  │         ├─ align_eq(p, head)? → memcpy from page_buf (existing fast path)
  │         │
  │         └─ [Layer 2] cp_page in meta_cache? → memcpy from DRAM (new)
  │              │  [BDL + non-BDL: identical — above dhara_nand_read]
  │              │  miss: load full cp_page → cache it → memcpy
  │              │
  │              └─ dhara_nand_read(cp_page, ...)  ← BDL/non-BDL fork here
  │                   │
  │                   ├─ [BDL]     bdl_handle->ops->read()
  │                   │              └─ nand_flash_blockdev_read()
  │                   │                   └─ nand_read()
  │                   │                        └─ read_page_and_wait()  ← [Layer 1]
  │                   │
  │                   └─ [non-BDL] nand_read()
  │                        └─ read_page_and_wait()  ← [Layer 1]
  │                             │  hit: return last_loaded_status, skip SPI command
  │                             │  miss: READ PAGE ADDRESS → wait → cache page+status
  │                             └─ SPI NAND hardware (~25–100 µs miss, ~1 µs hit)
  │
  └─ dhara_nand_read(data_page, 0, page_size) — the actual sector data read
```

**All three layers work for both BDL and non-BDL.** Layers 2 and 3 sit above the BDL/non-BDL fork entirely. Layer 1 sits in `nand_read()`, which is the terminal read function called by both `nand_flash_blockdev_read()` (BDL path) and directly by `dhara_glue.c` (non-BDL path).

All three layers are **independent and additive**. Layer 1 can be deployed with zero Dhara changes. Layer 2 requires only journal changes (backward-compatible with optional init). Layer 3 requires map changes but adds only 136 bytes to `dhara_map`.

---

## 5. ESP32-Specific Implementation Notes

### DMA alignment

Cache buffers (Layer 2) must be `heap_caps_aligned_alloc(spi_nand_get_dma_alignment(), page_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)`. This matches the existing pattern for `work_buffer` and `read_buffer` in `nand_impl.c`. The `MALLOC_CAP_INTERNAL` flag is critical — see PSRAM note below.

### No PSRAM for cache buffers

PSRAM (when available) must **not** be used for Layer 2 cache buffers. Cache hits would incur 50–100 ns PSRAM access latency per byte, making a `memcpy(132 bytes)` from PSRAM competitive with — or slower than — just re-reading from NAND. Only internal DRAM (~30 ns) provides the speedup.

### Kconfig options

> ⚠️ **Build system note:** `DHARA_META_CACHE_SLOTS` controls a `#define` in `dhara/journal.h` (part of the vendored Dhara subcomponent). The Kconfig option must be defined in the `spi_nand_flash` component's `Kconfig` file, and the generated `sdkconfig.h` value must be forwarded to `journal.h` via a wrapper header or a compile-time `-D` flag in `CMakeLists.txt`. The struct layout of `dhara_journal` changes based on this value, so it must be consistent between the Dhara compilation unit and all users of `struct dhara_journal`. Verify this propagation works correctly, especially if Dhara is compiled as a separate CMake target.

Three new Kconfig entries in `spi_nand_flash`:

```kconfig
config DHARA_META_CACHE_SLOTS
    int "Number of metadata checkpoint-page cache slots (0 = disabled)"
    range 0 8
    default 4
    help
        Each slot holds one full NAND page of metadata (typically 2048 bytes),
        DMA-aligned in internal DRAM. Total cost: SLOTS × page_size bytes.
        Set 0 to disable entirely (no RAM overhead, original behavior).
        Recommended: 0 for severely RAM-constrained targets (<256 KB free SRAM).

config DHARA_MAP_PATH_CACHE
    bool "Enable trace_path sequential locality shortcut"
    default y
    help
        Caches the last successful trace_path result (136 bytes in dhara_map).
        Skips up to 31 of 32 metadata reads for sequential sector access.
        Disable only if reproducibility testing requires identical behavior.
```

### Thread safety

The existing `handle->mutex` in `spi_nand_flash_device_t` already serializes all NAND operations. Layer 1's `last_loaded_page` and Layer 2's `cache_keys/cache_bufs` are accessed only under this lock. No additional synchronization required.

### ECC interaction (Layer 1)

When `read_page_and_wait()` returns a cached hit, it returns `last_loaded_status` — the STATUS register value captured when the page was originally loaded. The NAND's internal ECC was applied at that time and is reflected in that status byte. Callers that check ECC (`nand_read`, `nand_copy`) pass this status to `is_ecc_error()` and get the correct result; no real register read occurs.

The key invariant: `is_ecc_error()` is only ever called with a status value obtained from an actual `wait_for_ready` poll — either the real one on a miss, or the cached one on a hit. It is never called with a stale or zero-initialised value.

**BDL ECC path:** In BDL mode, `dhara_glue.c`'s `dhara_nand_read` checks `dev_handle->chip.ecc_data.ecc_corrected_bits_status` after `bdl_handle->ops->read()` returns. `nand_flash_blockdev_read()` calls `nand_read()`, which calls `is_ecc_error()` (using the status from `read_page_and_wait`) and sets `ecc_corrected_bits_status`. On a Layer 1 cache hit, `read_page_and_wait` returns `last_loaded_status`, `is_ecc_error()` runs with that value, and the field is set correctly. No special BDL-specific ECC handling is required.

---

## 6. Design Decisions & Trade-offs

### Why not a metadata-slice cache (Approach B from brainstorm) instead of full-page cache (Layer 2)?

A slice cache stores 132-byte entries keyed by `dhara_page_t`. On a miss, it still issues a `dhara_nand_read(..., offset, DHARA_META_SIZE, ...)` — which causes a full `READ PAGE ADDRESS` anyway (you pay the expensive part regardless of slice vs full-page). The full-page cache:

- Same or cheaper miss cost (NAND array access dominates, not the byte count)
- 7× more hits per loaded page (covers the entire checkpoint group, not just one slot)
- Simpler invalidation (keyed by checkpoint page, not individual user page)

The only advantage of slice cache: smaller per-entry RAM (132 bytes vs 2048 bytes). Valid for extremely RAM-constrained devices. Default choice here is full-page since budget allows.

### Why round-robin eviction instead of LRU?

`trace_path` visits nodes in a deterministic pattern: high address-space levels (near root) are shared across many sectors; low levels are unique per sector. True LRU would prefer the shared high-level nodes, which is slightly better. However:

- LRU requires a timestamp or linked list per slot — adds code complexity
- With only 4 slots, the benefit over round-robin is moderate

> **Note on round-robin behavior:** The claim that "round-robin naturally retains root-adjacent pages" only holds for a cold-cache single traversal. After the eviction ring wraps, the oldest-loaded pages (which tend to be root-adjacent, loaded first) are evicted first — the opposite of ideal. LRU would genuinely outperform round-robin for repeated traversals of the same sector range. Consider LRU for a follow-up if profiling shows meaningful improvement with ≥4 slots.

### Why is the path cache (`Layer 3`) disabled for writes?

During a write, `trace_path` is called with `new_meta != NULL` — it must build a complete alt-pointer array for the new record. The alt-pointer at each level is either copied from the current node's meta (if the path continues in the same direction) or set to the current page `p` (if the path diverges). Computing this correctly requires visiting every level from the root. Skipping levels via the shortcut would produce an incorrect `new_meta`.

For pure lookups (`dhara_map_find`, `dhara_map_read`), `new_meta` is always `NULL`, so the shortcut is safe.

### Upstream compatibility

Layers 1 and 2 (with the optional-init API design) are strictly additive:

- Layer 1: new fields in `spi_nand_flash_device_t` (ESP-IDF only), no Dhara ABI change
- Layer 2: `dhara_journal_init()` signature unchanged; `dhara_journal_set_meta_cache()` is new (additive); fields added to `dhara_journal` struct are `#if`-gated
- Layer 3: new fields in `dhara_map` struct, `#if`-gated; `trace_path` signature unchanged

A patch to upstream Dhara could be offered for Layers 2+3 stripped of the ESP32-specific allocation logic, consistent with the upstream author's stated willingness to accept it.

---

## 7. Known Gaps & Open Questions

This section records implementation questions that remain open and must be resolved before or during coding.

### G1 — ~~Locate the checkpoint-write hook in `journal.c`~~ (RESOLVED)

**Resolved.** `push_meta()` exists at `journal.c:798`. The checkpoint page write is at line 831:

```c
if (dhara_nand_prog(j->nand, j->head + 1, j->page_buf, &my_err) < 0) {
```

The written checkpoint page address is `j->head + 1`. At this point `j->head` is the last user page of the group, so `j->head + 1` is the checkpoint page — identical to `old_head | ppc_mask`. The Layer 2 cache invalidation hook belongs immediately after this `dhara_nand_prog` call succeeds:

```c
// In push_meta(), after the dhara_nand_prog succeeds (i.e. inside the success path):
#if DHARA_META_CACHE_SLOTS > 0
    {
        const dhara_page_t written_cp = j->head + 1;  // = old_head | ppc_mask
        for (int i = 0; i < j->cache_slots; i++)
            if (j->cache_keys[i] == written_cp)
                j->cache_keys[i] = DHARA_PAGE_NONE;
    }
#endif
```

There is also a second `dhara_nand_prog` call at `journal.c:705` (inside `dhara_journal_enqueue` for the raw data page write) — that one writes a user data page, not a checkpoint page, and does **not** need a cache invalidation hook.

### G2 — `nand_prog` register state after `PROGRAM EXECUTE` (Layer 1)

After `program_execute_and_wait`, is the NAND internal register still valid for the programmed page, or indeterminate? This is chip-dependent. If valid, `last_loaded_page` can remain set (no invalidation needed). If indeterminate, `valid = false` is the safe default. Check datasheets for the specific chips supported (Winbond, GigaDevice, Micron, Alliance, Zetta, XTX).

### G3 — ~~BDL path and Layer 1~~ (RESOLVED)

**Resolved.** Layer 1 is effective in both BDL and non-BDL modes. In BDL mode the call chain is `dhara_nand_read` → `nand_flash_blockdev_read()` → `nand_read()` — the same terminal `nand_read()` that Layer 1 patches. The BDL indirection adds negligible overhead (one extra function call). No BDL-specific variant of Layer 1 is needed.

### G4 — `DHARA_META_CACHE_SLOTS` Kconfig → `#define` forwarding (Layer 2, build system)

Determine the correct CMake mechanism to forward `CONFIG_DHARA_META_CACHE_SLOTS` from `sdkconfig.h` into `dhara/journal.h` as `DHARA_META_CACHE_SLOTS`. Options: (a) add `-DDHARA_META_CACHE_SLOTS=$(CONFIG_DHARA_META_CACHE_SLOTS)` to the Dhara compile target's `COMPILE_DEFINITIONS`, or (b) add an include of `sdkconfig.h` at the top of `journal.h` (invasive upstream change). Option (a) is preferred.

### G5 — Test plan

The existing test suite in `dhara/tests/` uses a NAND simulator and exercises the map and journal layers. A test run with the cache enabled vs. disabled should produce identical logical read/write results (same sector → same data). A regression test matrix should cover:

- Sequential reads (primary beneficiary of L3)
- Random reads (L2 warm-up path)
- Mixed read/write workloads (L2 invalidation correctness)
- Recovery scenario (ensure `recover_meta` path still works with L2 enabled)
- Wrap-around (ring epoch rollover, verify no stale cache entries survive)
- Bad-block injection (verify L1 invalidation after `nand_is_bad` calls)
- **BDL mode end-to-end:** run the same test matrix with `CONFIG_NAND_FLASH_ENABLE_BDL=y` and verify identical results. The host-test suite (`host_test/`) already has separate BDL test files (`test_nand_flash_bdl.cpp`); extend these with the read-performance scenarios above.

### G6 — `nand_flash_blockdev_read` page-loop and Layer 1 interaction

`nand_flash_blockdev_read()` loops over pages calling `nand_read()` sequentially. For a multi-page read, each iteration lands on a different page, so Layer 1 provides no cross-iteration benefit there. This is expected — multi-page data reads are not the hot path being optimized (metadata reads are single-page). Confirm that no code path calls `nand_flash_blockdev_read` for metadata in a way that would create an unintended interaction with the Layer 1 cache state.
