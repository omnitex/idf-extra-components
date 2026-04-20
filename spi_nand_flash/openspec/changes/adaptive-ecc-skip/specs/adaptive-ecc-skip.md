# Spec: Page-Level Relief Wear Leveling

Status: ADDED

---

## 1. ECC Classification

### 1.1 Status mapping

After each successful `dhara_nand_read()`, the hardware ECC status is read from
`dev_handle->chip.ecc_data.ecc_corrected_bits_status` and classified as:

| Class | Condition |
|-------|-----------|
| CLEAN | `NAND_ECC_OK` |
| MID   | status >= `config.mid_threshold` and < `config.high_threshold` |
| HIGH  | status >= `config.high_threshold` (but still correctable) |

`NAND_ECC_NOT_CORRECTED` is an existing hard error handled by the caller — unchanged.

### 1.2 Configurable thresholds

```c
typedef struct {
    bool     enabled;              /*!< Feature on/off. Default: false */
    uint8_t  mid_threshold;        /*!< ecc_corrected_bits_status value counted as MID */
    uint8_t  high_threshold;       /*!< ecc_corrected_bits_status value for immediate relief flag */
    uint8_t  mid_count_limit;      /*!< MID hits before page is flagged for relief */
    uint8_t  max_consecutive_relief; /*!< Max pages relieved in a row before forcing a write */
    uint16_t map_capacity;         /*!< Max entries in relief map (power of 2). Default: 512 */
} spi_nand_ecc_relief_config_t;
```

Embedded as `ecc_relief` field in `spi_nand_flash_config_t`.

---

## 2. Sparse Relief Map

### 2.1 Structure

An in-RAM open-addressing hash table, allocated at `dhara_init()` if
`config.ecc_relief.enabled == true`.

```c
typedef struct {
    dhara_page_t page;       // physical page number; INVALID_PAGE = empty slot
    uint8_t      mid_count;  // accumulated MID-level ECC hits
    uint8_t      flags;      // ECC_RELIEF_FLAG_PENDING (bit 0)
} ecc_relief_entry_t;
```

- Key: physical page number (`dhara_page_t`)
- Miss = CLEAN; no entry is created for CLEAN reads
- `ECC_RELIEF_FLAG_PENDING` = this page should be skipped on the next write targeting it
- After a page is relieved (skipped), `ECC_RELIEF_FLAG_PENDING` is cleared. The entry
  may remain for `mid_count` bookkeeping and be re-flagged on future reads.

### 2.2 Lookup

Hash: `page % capacity`. Linear probing on collision.

### 2.3 Eviction

When a successful `dhara_nand_erase()` completes on a block, all map entries for pages
in that block are removed (their physical page addresses will be reused in the new
erase cycle and have no prior ECC history).

---

## 3. ECC Observation Callback — `nand_read()` in `nand_impl.c`

To capture ECC events from **all** read paths — Dhara-internal GC reads, Dhara map
traversal reads, and consumer reads through the WL BDL — the observation point is
`nand_read()` in `nand_impl.c`, which is the single convergence point for all NAND
page reads regardless of caller.

### 3.1 Callback field on `spi_nand_flash_device_t`

ADDED to `struct spi_nand_flash_device_t` in `priv_include/nand.h`:

```c
void (*on_page_read_ecc)(uint32_t page, nand_ecc_status_t status, void *ctx);
void  *on_page_read_ecc_ctx;
```

Both fields are `NULL` when the feature is disabled; `nand_read()` checks for `NULL`
before calling. No behavior change for existing code paths when feature is off.

### 3.2 Firing the callback in `nand_read()`

MODIFIED in `src/nand_impl.c`, inside `nand_read()`, after `is_ecc_error()` runs:

```c
nand_ecc_status_t status = handle->chip.ecc_data.ecc_corrected_bits_status;
if (status != NAND_ECC_OK && status != NAND_ECC_NOT_CORRECTED) {
    if (handle->on_page_read_ecc) {
        handle->on_page_read_ecc(page, status, handle->on_page_read_ecc_ctx);
    }
}
```

`NAND_ECC_NOT_CORRECTED` is excluded — that is the existing hard-error path, handled
separately upstream. `NAND_ECC_OK` is excluded — no action needed for clean reads.
Only correctable non-zero ECC levels fire the callback.

### 3.3 Callback implementation in `dhara_glue.c`

ADDED: `dhara_ecc_read_cb()` registered on `handle->on_page_read_ecc` at WL init:

```c
static void dhara_ecc_read_cb(uint32_t page, nand_ecc_status_t status, void *ctx)
{
    spi_nand_flash_dhara_priv_data_t *priv = ctx;
    if (status >= priv->config.ecc_relief.high_threshold) {
        relief_map_flag(priv, (dhara_page_t)page);
    } else if (status >= priv->config.ecc_relief.mid_threshold) {
        relief_map_increment(priv, (dhara_page_t)page);
    }
}
```

Registration at init (replaces the former read-hook in `dhara_nand_read()`):

```c
handle->on_page_read_ecc     = dhara_ecc_read_cb;
handle->on_page_read_ecc_ctx = priv;
```

Deregistration at deinit:

```c
handle->on_page_read_ecc     = NULL;
handle->on_page_read_ecc_ctx = NULL;
```

### 3.4 BDL compatibility

Because the callback fires inside `nand_read()`, it is invoked regardless of whether
the caller is `dhara_nand_read()` (Dhara HAL, both BDL and non-BDL branches) or
`nand_flash_blockdev_read()` (Flash BDL) or `spi_nand_flash_read_page()` (WL BDL
consumer path). No `#ifdef CONFIG_NAND_FLASH_ENABLE_BDL` branching is needed anywhere
in the relief implementation.

---

## 4. Relief Mechanism — inside `dhara_journal_enqueue()`

MODIFIED behavior in `dhara/dhara/dhara/journal.c` (when relief is enabled via
callback/hook registered at journal init):

After `prepare_head()` succeeds and `j->head` is known, before calling
`dhara_nand_prog()`:

1. Invoke the relief check: `is_relief_pending(j->head)` via a function pointer stored
   in `struct dhara_journal` (or equivalent hook mechanism).
2. If relief is pending for `j->head`:
   a. Do NOT call `dhara_nand_prog()` — the page is left erased (unprogrammed).
   b. Call `push_meta()` with `meta = NULL` (filler entry), consuming this page slot.
   c. Clear `ECC_RELIEF_FLAG_PENDING` for this page in the map.
   d. Increment a consecutive-relief counter.
   e. If consecutive-relief counter < `max_consecutive_relief`: retry the outer loop
      to find a new `j->head` for the actual data write.
   f. If counter reaches `max_consecutive_relief`: force the write to the next page
      regardless of its relief status.
3. If no relief pending: proceed with `dhara_nand_prog()` as normal.

### 4.1 Hook interface

A new field is added to `struct dhara_journal` in `journal.h`:

```c
struct dhara_journal {
    /* ... existing fields ... */
    int (*relief_check)(dhara_page_t page, void *ctx); // NULL = feature disabled
    void *relief_ctx;
};
```

`dhara_journal_set_relief_hook()` is added to set these at runtime after init.

---

## 5. Capacity Overhead

Each relieved page consumes one journal slot as a filler. The consecutive-skip cap
(`max_consecutive_relief`, default 4) bounds the worst-case overhead per write
operation. Under normal conditions (only occasional pages flagged), overhead is
negligible.

Warning: if relieved-filler pages exceed 10% of journal capacity in a sliding window,
log `ESP_LOGW`.

---

## 6. Diagnostic API

ADDED:

```c
typedef struct {
    uint32_t pages_pending_relief;  /*!< Pages currently flagged ECC_RELIEF_FLAG_PENDING */
    uint32_t total_pages_relieved;  /*!< Cumulative pages relieved since init */
    uint32_t map_entries_used;      /*!< Current occupied map entries */
    uint32_t map_capacity;          /*!< Total map capacity */
    uint32_t consecutive_cap_hits;  /*!< Times the consecutive-skip cap was reached */
} spi_nand_ecc_relief_stats_t;

esp_err_t spi_nand_flash_get_ecc_relief_stats(spi_nand_flash_device_t *handle,
                                               spi_nand_ecc_relief_stats_t *stats);
```

Returns `ESP_ERR_NOT_SUPPORTED` if `config.ecc_relief.enabled == false`.

---

## 7. Kconfig

```
config NAND_FLASH_ECC_RELIEF_DEFAULT_MAP_CAPACITY
    int "Default ECC relief map capacity (entries)"
    default 512
    range 64 4096

config NAND_FLASH_ECC_RELIEF_ENABLE_DEFAULT
    bool "Enable page-level ECC relief by default"
    default n
```

---

## Scenarios

### SC-01: Clean read — no map entry created
Given ECC relief is enabled and a page is read with `NAND_ECC_OK`,
when `dhara_nand_read()` completes,
then no entry is added to the relief map.

### SC-02: HIGH ECC read — page immediately flagged
Given ECC relief is enabled,
when a page read returns status >= `high_threshold`,
then `ECC_RELIEF_FLAG_PENDING` is set for that page.

### SC-03: Repeated MID ECC reads reach limit
Given `mid_count_limit = 3`,
when the same page is read 3 times each returning MID-level ECC status,
then on the 3rd hit `ECC_RELIEF_FLAG_PENDING` is set.

### SC-04: Flagged page is relieved on next journal write
Given page P has `ECC_RELIEF_FLAG_PENDING` set,
when `dhara_journal_enqueue()` selects P as `j->head`,
then `dhara_nand_prog()` is NOT called for P,
a filler entry is recorded for P's slot,
and `ECC_RELIEF_FLAG_PENDING` is cleared for P.

### SC-05: Data is written to the next page after relief
Given page P was just relieved,
when the journal retries for the actual data write,
then the data is programmed to page P+1 (or next non-flagged page).

### SC-06: Consecutive-skip cap is enforced
Given `max_consecutive_relief = 2` and pages P, P+1, P+2 are all flagged,
when `dhara_journal_enqueue()` runs,
then P and P+1 are relieved, but P+2 is written to regardless of its flag.

### SC-07: Successful erase evicts map entries for that block
Given block B contains map entries for pages B*ppb .. B*ppb+ppb-1,
when `dhara_nand_erase(B)` completes successfully,
then all those entries are removed from the map.

### SC-08: Feature disabled — no behavior change
Given `config.ecc_relief.enabled == false`,
when any read or journal write occurs,
then no map operations are performed and `dhara_journal_enqueue()` is unchanged.

### SC-09: Diagnostic stats reflect relieved pages
Given 5 pages have been relieved,
when `spi_nand_flash_get_ecc_relief_stats()` is called,
then `stats.total_pages_relieved == 5`.
