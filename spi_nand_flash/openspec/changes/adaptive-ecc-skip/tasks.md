# Tasks: Page-Level Relief Wear Leveling

## T0 — HAL: ECC observation callback in `nand_impl.c`

- [x] T0.1 Add `on_page_read_ecc` function pointer and `on_page_read_ecc_ctx` to
      `struct spi_nand_flash_device_t` in `priv_include/nand.h`; initialize both
      to `NULL` in `nand_init_device()` so existing behavior is unchanged
- [x] T0.2 In `nand_read()` (`src/nand_impl.c`): after `is_ecc_error()` runs, if
      `ecc_corrected_bits_status` is neither `NAND_ECC_OK` nor `NAND_ECC_NOT_CORRECTED`,
      call `handle->on_page_read_ecc(page, status, ctx)` when non-NULL
- [x] T0.3 Verify the callback does NOT fire for `NAND_ECC_NOT_CORRECTED` — that
      is the existing hard-error path handled upstream; no double-handling

- [x] T1.1 Read `dhara/dhara/dhara/journal.h` and `journal.c` in full — understand
      `dhara_journal_enqueue()`, `dhara_journal_copy()`, `push_meta()`, and the
      existing `pad_queue()` filler mechanism before touching anything
- [x] T1.2 Add `relief_check` function pointer and `relief_ctx` to `struct dhara_journal`
      in `journal.h`
- [x] T1.3 Add `dhara_journal_set_relief_hook()` declaration to `journal.h` and
      implementation to `journal.c`

## T2 — Dhara: Implement page relief inside `dhara_journal_enqueue()`

- [x] T2.1 Modify `dhara_journal_enqueue()`: after `prepare_head()` succeeds, call
      `relief_check(j->head, j->relief_ctx)` if the hook is set
- [x] T2.2 If relief is signalled: call `push_meta(j, NULL, err)` (filler),
      increment consecutive counter, continue the retry loop
- [x] T2.3 Enforce consecutive-skip cap: if counter reaches the limit, proceed with
      normal `dhara_nand_prog()` regardless of relief flag
- [x] T2.4 Apply the same relief check to `dhara_journal_copy()` (used by GC)

## T3 — Public API: Config and diagnostic structs

- [x] T3.1 Add `spi_nand_ecc_relief_config_t` struct to `include/spi_nand_flash.h`
      (fields: `enabled`, `mid_threshold`, `high_threshold`, `mid_count_limit`,
      `max_consecutive_relief`, `map_capacity`)
- [x] T3.2 Add `ecc_relief` field of type `spi_nand_ecc_relief_config_t` to
      `spi_nand_flash_config_t`
- [x] T3.3 Add `spi_nand_ecc_relief_stats_t` struct to `include/spi_nand_flash.h`
      (fields: `pages_pending_relief`, `total_pages_relieved`, `map_entries_used`,
      `map_capacity`, `consecutive_cap_hits`)
- [x] T3.4 Declare `spi_nand_flash_get_ecc_relief_stats()` in `include/spi_nand_flash.h`

## T4 — Core: Relief map implementation in `dhara_glue.c`

- [x] T4.1 Define `ecc_relief_entry_t` and constants (`ECC_RELIEF_FLAG_PENDING`,
      `INVALID_PAGE`, `TOMBSTONE_PAGE`) in a new `priv_include/ecc_relief_map.h`
- [x] T4.2 Add `ecc_relief_map`, `ecc_relief_capacity`, `stat_pages_relieved`,
      `stat_consecutive_cap_hits` to `spi_nand_flash_dhara_priv_data_t`
- [x] T4.3 Implement `relief_map_lookup()` — open-addressing hash lookup with
      linear probing, skipping tombstone slots
- [x] T4.4 Implement `relief_map_flag()` — insert or update entry, set
      `ECC_RELIEF_FLAG_PENDING`; handle map-full by evicting a non-PENDING
      non-tombstone entry; log warning if none found
- [x] T4.5 Implement `relief_map_increment()` — increment `mid_count`; call
      `relief_map_flag()` when `mid_count >= mid_count_limit`
- [x] T4.6 Implement `relief_map_clear_pending()` — clear `ECC_RELIEF_FLAG_PENDING`
      for a single page; leave entry with `mid_count` intact for future reads
- [x] T4.7 Implement `relief_map_evict_range()` — mark all entries in a page range
      as tombstones (called on successful erase)
- [x] T4.8 Implement `dhara_relief_check_cb()` — the callback registered with the
      journal; calls `relief_map_lookup()` and clears PENDING flag if set

## T5 — Init / deinit wiring in `dhara_glue.c`

- [x] T5.1 In `dhara_init()`: if `enabled`, allocate map with `heap_caps_calloc()`
      (MALLOC_CAP_INTERNAL); register `dhara_ecc_read_cb` on `handle->on_page_read_ecc`
      and `dhara_relief_check_cb` on the journal hook
- [x] T5.2 In `dhara_deinit()` / detach path: free map if allocated; set
      `handle->on_page_read_ecc = NULL` and `handle->on_page_read_ecc_ctx = NULL`

## T6 — ECC read callback implementation in `dhara_glue.c`

- [x] T6.1 Implement `dhara_ecc_read_cb(page, status, ctx)`: classify `status`
      against `mid_threshold` and `high_threshold` from config; call
      `relief_map_flag()` on HIGH or `relief_map_increment()` on MID
- [x] T6.2 Confirm no ECC classification logic remains in `dhara_nand_read()` —
      that function only handles `DHARA_E_ECC` (uncorrectable) as before

## T7 — Erase eviction in `dhara_nand_erase()`

- [x] T7.1 After a successful non-BDL `nand_erase_block()`: call
      `relief_map_evict_range(priv, block << log2_ppb, 1 << log2_ppb)`

## T8 — Diagnostic API

- [x] T8.1 Implement `spi_nand_flash_get_ecc_relief_stats()` in `src/dhara_glue.c`
- [x] T8.2 Scan map to count `pages_pending_relief` (entries with PENDING flag)
- [x] T8.3 Return `ESP_ERR_NOT_SUPPORTED` if feature is disabled

## T9 — Kconfig

- [x] T9.1 Add `NAND_FLASH_ECC_RELIEF_DEFAULT_MAP_CAPACITY` (int, default 512,
      range 64–4096) to `Kconfig`
- [x] T9.2 Add `NAND_FLASH_ECC_RELIEF_ENABLE_DEFAULT` (bool, default n) to `Kconfig`

## T10 — Tests

- [x] T10.1 SC-01: clean read → no map entry created
- [x] T10.2 SC-02: HIGH ECC read → `ECC_RELIEF_FLAG_PENDING` set immediately
- [x] T10.3 SC-03: repeated MID reads reach `mid_count_limit` → PENDING set
- [x] T10.4 SC-04: flagged page at `j->head` → `dhara_nand_prog()` not called,
      filler written, flag cleared
- [x] T10.5 SC-05: data written to page after the relieved one
- [x] T10.6 SC-06: consecutive-skip cap enforced — write forced at cap limit
- [x] T10.7 SC-07: successful erase evicts map entries for that block
- [x] T10.8 SC-08: feature disabled → no map ops, journal unchanged,
      `on_page_read_ecc` is NULL (callback never registered)
- [x] T10.9 SC-09: diagnostic stats reflect relieved page count
- [x] T10.10 BDL path: ECC event from a consumer read via WL BDL (`spi_nand_flash_read_page`)
      reaches `on_page_read_ecc` and updates the relief map (verifies BDL compatibility)

## T11 — Documentation

- [x] T11.1 Add `docs/ecc-page-relief.md` explaining the feature, configuration,
      threshold guidance, and diagnostic API
- [x] T11.2 Update `CHANGELOG.md`
