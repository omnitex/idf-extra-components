# Proposal: Page-Level Relief Wear Leveling

## Problem

Conventional wear leveling — including Dhara's FIFO circular log — assumes uniform
lifetime across all NAND pages. In practice this is not true. Manufacturing process
variation, excessive read operations (read disturb), and uneven program/erase stress
cause individual pages to age at different rates.

By the time a page fails with uncorrectable ECC, data integrity is already at risk.
NAND flash signals per-page aging earlier through correctable ECC bit errors, which are
reported by the on-chip ECC engine via the status register after every read. This signal
is already retrieved in `spi_nand_flash` (`nand_impl.c`, `dhara_glue.c`) but currently
ignored beyond detecting uncorrectable failures.

## Proposed Solution

Implement **page-level relief** (also called "wear un-leveling" in literature), a
technique where individual pages that show elevated correctable ECC error counts are
deliberately skipped (not programmed) during write operations. A skipped page is still
erased as part of its block's normal erase cycle, but is left unprogrammed — this is
called a **relief cycle** (Jimenez et al., FAST 2014).

The mechanism:

1. After every `nand_read()` in `nand_impl.c` — regardless of whether the call came
   from Dhara's internal GC/map traversal or from a consumer read through the WL BDL —
   a registered ECC callback fires with the physical page number and corrected-bit status.
   If the status exceeds a threshold, that page is marked for relief in an in-RAM sparse
   map. The callback is registered on `spi_nand_flash_device_t` at WL init time, making
   the mechanism work identically for both BDL and non-BDL paths with no `#ifdef`.
2. When Dhara's journal is about to program a page (`dhara_journal_enqueue()` in
   `journal.c`), check whether the target physical page (`j->head`) is marked for relief.
3. If yes: skip the `dhara_nand_prog()` call for this page — the page slot is consumed
   (head advances, filler metadata recorded) but the cell is left unprogrammed. The data
   is written to the next available page instead.
4. Multiple consecutive pages can be relieved in a row until a non-flagged page is found.
5. The relief flag is cleared once a page has been relieved (single-cycle relief).

This maps directly onto an existing Dhara mechanism: `dhara_journal_enqueue()` already
accepts `data = NULL` to write a filler page (used by `pad_queue()`). A relieved page
is a filler page inserted deliberately at a flagged position.

This approach does not replace Dhara's existing wear leveling — it layers on top of it,
extending the "level out program/erase cycles" guarantee with "also level out actual
measurable per-page wear" using hardware ECC feedback.

## Non-Goals

- Persistence of the relief map across reboots (deferred; state rediscovered via ECC
  reads on next boot).
- Sophisticated threshold prediction models (P/E cycle + retention time models as in
  literature); initial threshold is a simple configurable ECC bit count limit, with
  more complex models deferred to the evaluation phase.

## Key Decisions

- **Disabled by default** — opt-in via `spi_nand_flash_config_t`. No behavior change
  for existing users.
- **Universal ECC observation via `nand_read()` callback** — a new
  `on_page_read_ecc(page, status, ctx)` function pointer is added to
  `spi_nand_flash_device_t`. `nand_read()` fires it after every correctable-ECC event.
  This captures reads from all callers — Dhara GC, Dhara map traversal, and consumer
  reads through the WL BDL — without any `#ifdef CONFIG_NAND_FLASH_ENABLE_BDL` branching.
  BDL and non-BDL paths are therefore handled identically.
- **Intervention inside Dhara's journal** — the relief check must happen inside
  `dhara_journal_enqueue()` in `journal.c`, after `prepare_head()` resolves `j->head`,
  because the physical target page is only known at that point. A new callback or inline
  check is added there.
- **RAM-only sparse map** — fixed-capacity open-addressing hash table keyed by physical
  page number (default 512 entries, ~3 KB). Only pages with elevated ECC are stored;
  a miss means clean.
- **Single-cycle relief** — the relief flag is cleared after one skipped write. The page
  re-enters the pool and may be flagged again if its next read returns elevated ECC.
- **Consecutive skip cap** — a configurable limit prevents relieving more than N pages
  in a row, bounding the capacity overhead of relief.
- **ECC thresholds configurable** — mid-level accumulation count and high-level
  immediate-flag threshold are set in `spi_nand_ecc_relief_config_t`.
- **Capacity overhead warning** — if the fraction of relieved pages in the active
  journal window exceeds a threshold, log a warning.

## Affected Files

| File | Change |
|------|--------|
| `priv_include/nand.h` | Add `on_page_read_ecc` callback + `on_page_read_ecc_ctx` to `spi_nand_flash_device_t` |
| `src/nand_impl.c` | Fire `on_page_read_ecc` in `nand_read()` after correctable-ECC events |
| `dhara/dhara/dhara/journal.h` | Add relief callback field and `ctx` to `struct dhara_journal` |
| `dhara/dhara/dhara/journal.c` | Call relief check inside `dhara_journal_enqueue()` and `dhara_journal_copy()` before `dhara_nand_prog()`; skip prog and retry if flagged |
| `src/dhara_glue.c` | Sparse relief map; register `on_page_read_ecc` callback on device; register relief callback with journal at init |
| `include/spi_nand_flash.h` | `spi_nand_ecc_relief_config_t` nested in `spi_nand_flash_config_t`; diagnostic API |
| `Kconfig` | Default map capacity and enable flag |
