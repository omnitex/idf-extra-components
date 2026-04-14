# Dhara OOB LPN Replay — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** After an unclean shutdown, reconstruct the in-RAM radix-map metadata for user pages written after the last checkpoint ("orphan pages") by storing the logical page number (LPN = sector id) in NAND OOB at program time and replaying it on mount.

**Architecture:** Extend `dhara_nand_prog` and `dhara_nand_copy` signatures to carry a `dhara_sector_t sector` argument (the LPN); the driver writes it to OOB atomically with each user-page program. On `dhara_map_resume`, after `dhara_journal_resume` establishes `j->root` and `j->head`, scan forward from `j->root+1` to the first free page: for each non-checkpoint page, read LPN via a new `dhara_nand_read_lpn` callback, reconstruct the 132-byte metadata blob using the existing `trace_path` logic (updating `j->root` after each page), patch it into the in-RAM `page_buf` at the correct slot, and reconcile `m->count`.

**Tech Stack:** C11, Dhara FTL library (vendored at `dhara/dhara/`), ESP-IDF host-test framework (IDF_TARGET=linux), Catch2 v3, mmap-backed NAND emulator (`spi_nand_flash/src/nand_impl_linux.c` + `nand_linux_mmap_emul.c`), pytest-embedded.

---

## Background for the implementer

### How Dhara works (must read before touching code)

Dhara is a NAND FTL at `dhara/dhara/dhara/`. It has three layers:

1. **`nand.h`** — seven driver callbacks you provide; Dhara calls them. Currently no OOB in any signature.
2. **`journal.h/.c`** — manages a circular write queue partitioned into *checkpoint groups*. Each group has `2^log2_ppc` pages: `(2^log2_ppc - 1)` user pages followed by one checkpoint (CP) page. A CP page stores a 16-byte header (magic, epoch, tail, bb_current, bb_last) + 4-byte cookie + `N × 132`-byte metadata blobs. Between checkpoints, metadata lives only in RAM (`j->page_buf`).
3. **`map.h/.c`** — a persistent functional radix tree over the journal. Maps `dhara_sector_t` (32-bit logical sector) → `dhara_page_t` (32-bit physical page). Each update record (132 bytes = `DHARA_META_SIZE`) stores `sector_id` + 32 alt-pointers (one per bit of the sector address).

### The orphan page problem

`dhara_map_resume()` calls `dhara_journal_resume()`, which:
1. Binary-searches for the last valid CP page → sets `j->root` (last user page of that CP group), `j->tail`, `j->head`.
2. Clears `page_buf` metadata slots to `0xFF`.

Any user pages written **after** `j->root` but **before** power loss exist physically on NAND but are not in the radix tree. The map does not know which logical sector they represent. These are orphan pages.

### What this plan adds

- **Write path:** `dhara_nand_prog(nand, page, data, sector, err)` — new `sector` arg. Driver writes `sector` (4 bytes LE) at OOB offset 0. For CP pages, Dhara passes `DHARA_SECTOR_NONE` (0xFFFFFFFF); driver may skip OOB write or write the sentinel.
- **Write path (GC):** `dhara_nand_copy(nand, src, dst, sector, err)` — same pattern.
- **Read path (replay only):** new `dhara_nand_read_lpn(nand, page, &sector, err)` callback. Driver reads OOB offset 0, returns 4-byte LE as `sector`. Returns non-zero if ECC error or unsupported; `DHARA_SECTOR_NONE` means "no LPN / erased."
- **Replay engine:** `dhara_map_replay_orphans()` — called from `dhara_map_resume()` after `journal_resume`. Scans `[j->root+1 .. first_free_page)`, skipping CP pages, calling `read_lpn`, reconstructing metadata, patching `page_buf`, updating `j->root`.

### Key code locations

| File | What to read |
|---|---|
| `dhara/dhara/dhara/nand.h` | Current callback signatures (you will change 2 + add 1) |
| `dhara/dhara/dhara/journal.h` | `struct dhara_journal`, constants (`DHARA_META_SIZE=132`, `DHARA_HEADER_SIZE=16`, `DHARA_COOKIE_SIZE=4`, `DHARA_SECTOR_NONE=0xFFFFFFFF`) |
| `dhara/dhara/dhara/journal.c` | `dhara_journal_resume()`, `push_meta()`, `next_upage()`, `align_eq()` |
| `dhara/dhara/dhara/map.c` | `dhara_map_resume()`, `trace_path()`, `prepare_write()`, `push_meta_buf()` |
| `dhara/dhara/dhara/bytes.h` | `dhara_r32()`, `dhara_w32()` — portable LE read/write |
| `spi_nand_flash/src/dhara_glue.c` | Implements all 7 Dhara callbacks; uses `__containerof` to get device handle from `dhara_nand*` |
| `spi_nand_flash/src/nand_impl_linux.c` | `nand_prog()`, `nand_read()`, `nand_copy()` for Linux target |
| `spi_nand_flash/src/nand_linux_mmap_emul.c` | mmap-backed NAND storage; emulated page size = page_size + oob_size |
| `spi_nand_flash/host_test/main/test_nand_flash_bdl.cpp` | Existing Catch2 tests — follow this style exactly |

### Invariants you must never break

- A page `p` is a checkpoint page if and only if `(p & ppc_mask) == ppc_mask` where `ppc_mask = (1 << j->log2_ppc) - 1`.
- Metadata slot for user page `p` within its CP page: slot = `p & ppc_mask`, byte offset = `DHARA_HEADER_SIZE + DHARA_COOKIE_SIZE + slot * DHARA_META_SIZE`.
- `j->root` must always point to the last user page in the logical journal chain. After replaying an orphan page at physical page `P`, set `j->root = P`.
- `trace_path()` is a static function in `map.c`. To call it during replay you will either make it non-static (preferred: add a `dhara_map_replay_orphans` function in `map.c` itself) or duplicate its logic (forbidden).
- Never program flash during replay — only RAM operations.

---

## Phase 0 — Spec and OOB layout (no code)

### Task 0.1: Read and understand all key files

Before writing any code, read these files completely:

- `dhara/dhara/dhara/nand.h`
- `dhara/dhara/dhara/journal.h`
- `dhara/dhara/dhara/journal.c` (all of it, ~900 lines)
- `dhara/dhara/dhara/map.c` (all of it, ~700 lines)
- `dhara/dhara/dhara/bytes.h`
- `spi_nand_flash/src/dhara_glue.c`
- `spi_nand_flash/src/nand_impl_linux.c`
- `spi_nand_flash/src/nand_linux_mmap_emul.c`
- `spi_nand_flash/host_test/main/test_nand_flash_bdl.cpp`

**Done when:** you can answer without looking: (a) what is `log2_ppc`, (b) how `push_meta` decides to flush a CP page, (c) what `trace_path` returns when the sector is not found, (d) how `dhara_nand_copy` is used in `raw_gc`.

**No commit for this task.**

---

## Phase 1 — Extend NAND interface and write path

### Task 1.1: Extend `dhara_nand_prog` signature in `nand.h`

**Files:**
- Modify: `dhara/dhara/dhara/nand.h`

**Step 1: Read current `dhara_nand_prog` declaration**

Open `dhara/dhara/dhara/nand.h`, find the `dhara_nand_prog` declaration:
```c
int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p,
                    const uint8_t *data,
                    dhara_error_t *err);
```

**Step 2: Change signature to add sector parameter**

Change to:
```c
int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p,
                    const uint8_t *data, dhara_sector_t sector,
                    dhara_error_t *err);
```

The `sector` is `DHARA_SECTOR_NONE` (0xFFFFFFFF) when programming a CP page.

**Step 3: Extend `dhara_nand_copy` signature**

In the same file, change:
```c
int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src, dhara_page_t dst,
                    dhara_error_t *err);
```
to:
```c
int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src, dhara_page_t dst,
                    dhara_sector_t sector,
                    dhara_error_t *err);
```

**Step 4: Add `dhara_nand_read_lpn` declaration**

After the `dhara_nand_copy` declaration, add:
```c
/* Read the logical page number (sector id) stored in OOB for page p.
 * Returns 0 and writes the sector to *sector_out on success.
 * Returns 0 and writes DHARA_SECTOR_NONE if the OOB is erased or carries no LPN.
 * Returns -1 and sets *err on ECC/hardware error.
 * If OOB-LPN is not supported by the driver, implement as: *sector_out = DHARA_SECTOR_NONE; return 0;
 */
int dhara_nand_read_lpn(const struct dhara_nand *n, dhara_page_t p,
                         dhara_sector_t *sector_out,
                         dhara_error_t *err);
```

**Step 5: Verify the file compiles**

Run (from the `dhara/dhara/` directory):
```bash
gcc -fsyntax-only -I. dhara/journal.c
```
Expected: compile error because `journal.c` still calls old signature. That is expected — proceed to Task 1.2.

**No commit yet.**

---

### Task 1.2: Update all call sites in `journal.c`

**Files:**
- Modify: `dhara/dhara/dhara/journal.c`

There are exactly two places where `dhara_nand_prog` is called in `journal.c`:
1. In `journal_enqueue` (or `push_meta`): programs the user data page.
2. In `push_meta`: programs the CP page when the group is full.
There is one place where `dhara_nand_copy` is called.

**Step 1: Find all call sites**

Search for `dhara_nand_prog` and `dhara_nand_copy` in `journal.c`:
```bash
grep -n "dhara_nand_prog\|dhara_nand_copy" dhara/dhara/dhara/journal.c
```

**Step 2: Update user page prog call**

The call that programs user data (in `dhara_journal_enqueue` or `prepare_head` — check which) should look like:
```c
// Before:
if (dhara_nand_prog(j->nand, j->head, data, err) < 0)
// After:
if (dhara_nand_prog(j->nand, j->head, data, meta_get_id(meta), err) < 0)
```
Where `meta_get_id(meta)` reads the 4-byte LE sector id from the meta blob. Check `bytes.h` — `dhara_r32(meta)` reads at offset 0. Alternatively, `meta_get_id` may already be a macro or inline in journal.c — grep for it.

**Step 3: Update CP page prog call**

The call that programs the CP page (the `page_buf` flush in `push_meta`) passes `DHARA_SECTOR_NONE`:
```c
// Before:
if (dhara_nand_prog(j->nand, j->head, j->page_buf, err) < 0)
// After:
if (dhara_nand_prog(j->nand, j->head, j->page_buf, DHARA_SECTOR_NONE, err) < 0)
```

**Step 4: Update `dhara_nand_copy` call**

Find the GC copy call (in `dhara_journal_copy` or `raw_copy` — check). It programs a user page at the destination, so the sector is available from the meta blob being processed:
```c
// Before:
if (dhara_nand_copy(j->nand, src, j->head, err) < 0)
// After:
if (dhara_nand_copy(j->nand, src, j->head, meta_get_id(meta), err) < 0)
```
The `meta` argument is the reconstructed metadata being passed into `dhara_journal_copy` by `raw_gc` in `map.c`. Verify by tracing the call chain.

**Step 5: Verify compilation**

```bash
gcc -fsyntax-only -I dhara/dhara dhara/dhara/dhara/journal.c
```
Expected: may still fail on undefined `dhara_nand_prog` (correct) or may pass syntax check.

**No commit yet.**

---

### Task 1.3: Update `dhara_nand_prog` and `dhara_nand_copy` in the test simulator

**Files:**
- Modify: `dhara/dhara/tests/sim.c`

The upstream test suite's simulator implements `dhara_nand_prog` and `dhara_nand_copy`. They need the new signatures.

**Step 1: Find and update `dhara_nand_prog` in `sim.c`**

```bash
grep -n "dhara_nand_prog\|dhara_nand_copy" dhara/dhara/tests/sim.c
```

Add the `sector` parameter; the test simulator ignores it (no OOB simulation needed at this stage):
```c
int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p,
                    const uint8_t *data, dhara_sector_t sector,
                    dhara_error_t *err)
{
    /* existing body unchanged — sector is intentionally unused in sim */
    (void)sector;
    ...
}
```

**Step 2: Same for `dhara_nand_copy`**

```c
int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src, dhara_page_t dst,
                    dhara_sector_t sector,
                    dhara_error_t *err)
{
    (void)sector;
    ...
}
```

**Step 3: Add stub `dhara_nand_read_lpn` to sim.c**

```c
int dhara_nand_read_lpn(const struct dhara_nand *n, dhara_page_t p,
                         dhara_sector_t *sector_out,
                         dhara_error_t *err)
{
    (void)n; (void)p; (void)err;
    *sector_out = DHARA_SECTOR_NONE;  /* OOB not simulated */
    return 0;
}
```

**Step 4: Build and run upstream tests to verify nothing is broken**

```bash
make -C dhara/dhara
```
Expected: all tests pass (the existing tests should be unaffected).

**Step 5: Commit**

```bash
git add dhara/dhara/dhara/nand.h dhara/dhara/dhara/journal.c dhara/dhara/tests/sim.c
git commit -m "feat(dhara): extend nand_prog/nand_copy with sector arg, add read_lpn callback"
```

---

### Task 1.4: Update `dhara_glue.c` in `spi_nand_flash`

**Files:**
- Modify: `spi_nand_flash/src/dhara_glue.c`

This is where the Dhara callbacks are implemented for the actual ESP-IDF component.

**Step 1: Update `dhara_nand_prog` in `dhara_glue.c`**

Find the current implementation:
```c
int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p,
                    const uint8_t *data, dhara_error_t *err)
```

Change to accept the new `sector` parameter. Store it for use by `nand_prog`:
```c
int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p,
                    const uint8_t *data, dhara_sector_t sector,
                    dhara_error_t *err)
{
    spi_nand_flash_dhara_priv_data_t *priv =
        __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
    esp_err_t ret = nand_prog(priv->parent_handle, p, data, sector);
    if (ret != ESP_OK) {
        *err = DHARA_E_BAD_BLOCK;
        return -1;
    }
    return 0;
}
```

Note: `nand_prog` in `nand_impl.h` / `nand_impl_linux.c` currently has signature `nand_prog(handle, page, data)`. You will update it in Task 1.5.

**Step 2: Update `dhara_nand_copy` in `dhara_glue.c`**

Find:
```c
int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src, dhara_page_t dst,
                    dhara_error_t *err)
```

Change to:
```c
int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src, dhara_page_t dst,
                    dhara_sector_t sector, dhara_error_t *err)
{
    spi_nand_flash_dhara_priv_data_t *priv =
        __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
    esp_err_t ret = nand_copy(priv->parent_handle, src, dst, sector);
    if (ret != ESP_OK) {
        *err = DHARA_E_BAD_BLOCK;
        return -1;
    }
    return 0;
}
```

**Step 3: Add `dhara_nand_read_lpn` in `dhara_glue.c`**

After the copy function, add:
```c
int dhara_nand_read_lpn(const struct dhara_nand *n, dhara_page_t p,
                         dhara_sector_t *sector_out, dhara_error_t *err)
{
    spi_nand_flash_dhara_priv_data_t *priv =
        __containerof(n, spi_nand_flash_dhara_priv_data_t, dhara_nand);
    esp_err_t ret = nand_read_lpn(priv->parent_handle, p, sector_out);
    if (ret != ESP_OK) {
        *err = DHARA_E_ECC;
        return -1;
    }
    return 0;
}
```

**No commit yet** — wait until nand_impl functions are updated.

---

### Task 1.5: Add LPN to `nand_impl_linux.c` and `nand_impl.h`

**Files:**
- Modify: `spi_nand_flash/priv_include/nand_impl.h`
- Modify: `spi_nand_flash/src/nand_impl_linux.c`

The Linux emulator already simulates OOB: each emulated page is `page_size + oob_size` bytes in the mmap file. OOB bytes 0–1 are bad-block indicator, 2–3 are free marker. **LPN will use OOB bytes 4–7** (4 bytes LE = `dhara_sector_t`).

**Step 1: Check the existing emulated_page_size and OOB layout in `nand_impl_linux.c`**

Look for `emulated_page_size`, `oob_size`, and the existing OOB byte assignments. Confirm OOB bytes 4–7 are free.

**Step 2: Update `nand_prog` signature in `nand_impl.h`**

```c
// Before:
esp_err_t nand_prog(spi_nand_flash_device_t *handle, uint32_t p, const uint8_t *data);
// After:
esp_err_t nand_prog(spi_nand_flash_device_t *handle, uint32_t p, const uint8_t *data, uint32_t sector);
```

**Step 3: Update `nand_copy` signature in `nand_impl.h`**

```c
// Before:
esp_err_t nand_copy(spi_nand_flash_device_t *handle, uint32_t src, uint32_t dst);
// After:
esp_err_t nand_copy(spi_nand_flash_device_t *handle, uint32_t src, uint32_t dst, uint32_t sector);
```

**Step 4: Add `nand_read_lpn` to `nand_impl.h`**

```c
esp_err_t nand_read_lpn(spi_nand_flash_device_t *handle, uint32_t p, uint32_t *sector_out);
```

**Step 5: Implement `nand_prog` update in `nand_impl_linux.c`**

Inside the existing `nand_prog` body, after writing the main page data, write the sector as 4 bytes LE to OOB offset 4:

```c
esp_err_t nand_prog(spi_nand_flash_device_t *handle, uint32_t p, const uint8_t *data, uint32_t sector)
{
    /* existing main page write code */
    ...

    /* Write LPN to OOB bytes 4-7 (little-endian) */
    uint8_t *oob_base = /* pointer to OOB area of page p in mmap */;
    oob_base[4] = (uint8_t)(sector & 0xFF);
    oob_base[5] = (uint8_t)((sector >> 8) & 0xFF);
    oob_base[6] = (uint8_t)((sector >> 16) & 0xFF);
    oob_base[7] = (uint8_t)((sector >> 24) & 0xFF);

    return ESP_OK;
}
```

Look at how `nand_prog` in `nand_impl_linux.c` currently accesses the mmap buffer (via `nand_emul_write` from `nand_linux_mmap_emul.c`). You may need to call a separate emul helper for the OOB write, or compute the offset directly.

**Step 6: Implement `nand_copy` update in `nand_impl_linux.c`**

Add the `sector` parameter and write it to OOB bytes 4–7 of the destination page, same as `nand_prog`.

**Step 7: Implement `nand_read_lpn` in `nand_impl_linux.c`**

```c
esp_err_t nand_read_lpn(spi_nand_flash_device_t *handle, uint32_t p, uint32_t *sector_out)
{
    /* Read OOB bytes 4-7 from page p in the mmap buffer */
    uint8_t *oob_base = /* pointer to OOB area of page p */;
    uint32_t sector = (uint32_t)oob_base[4]
                    | ((uint32_t)oob_base[5] << 8)
                    | ((uint32_t)oob_base[6] << 16)
                    | ((uint32_t)oob_base[7] << 24);
    *sector_out = sector;
    return ESP_OK;
}
```

`0xFFFFFFFF` will be returned if the OOB bytes are in the erased state (all 0xFF).

**Step 8: Build the host test to confirm it compiles**

```bash
cd spi_nand_flash/host_test
idf.py --preview set-target linux
idf.py build
```
Expected: build succeeds (no test failures yet — the new functions exist but aren't exercised).

**Step 9: Commit**

```bash
git add spi_nand_flash/src/dhara_glue.c \
        spi_nand_flash/priv_include/nand_impl.h \
        spi_nand_flash/src/nand_impl_linux.c
git commit -m "feat(spi_nand_flash): wire LPN OOB write/read through nand_impl and dhara_glue"
```

---

## Phase 2 — Replay engine in `map.c`

### Task 2.1: Add `dhara_map_replay_orphans` function to `map.c`

**Files:**
- Modify: `dhara/dhara/dhara/map.c`
- Modify: `dhara/dhara/dhara/map.h` (declaration only)

This is the heart of the feature. Read `trace_path` and `push_meta_buf` in `map.c` before writing this function.

**Step 1: Understand `trace_path` return values**

`trace_path(m, target, loc, new_meta, err)` returns:
- 0 on success, sets `*loc` to the physical page where `target` currently lives, fills `new_meta` with the 132-byte alt-pointer blob.
- -1 with `*err = DHARA_E_NOT_FOUND` if `target` has never been written (new sector).
- -1 with other error if a NAND read failed.

**Step 2: Understand how `push_meta_buf` works**

In `map.c` (or possibly inline in `journal_enqueue`), after `trace_path` builds `new_meta`, the 132 bytes are written into `page_buf` at the slot corresponding to `j->head`. The slot index is `j->head & ppc_mask`. The byte offset is `DHARA_HEADER_SIZE + DHARA_COOKIE_SIZE + slot * DHARA_META_SIZE`. Look at `push_meta` in `journal.c` to confirm these offsets.

During replay, `j->head` has already been advanced past all orphan pages (it was set by `find_head` during `journal_resume`). So the slot index for a replay page `p` is `p & ppc_mask`, and the byte offset is as above.

**Step 3: Write the declaration in `map.h`**

Add at the end of `map.h`, before the final `#endif`:
```c
/* Replay orphan user pages (written after last checkpoint, before power loss).
 * Called automatically by dhara_map_resume() after dhara_journal_resume() succeeds.
 * For each user page between j->root+1 and the first free page (exclusive),
 * reads LPN from OOB, reconstructs metadata, and patches page_buf.
 * Returns 0 on success; -1 on NAND error (not on truncation). */
int dhara_map_replay_orphans(struct dhara_map *m, dhara_error_t *err);
```

**Step 4: Write `dhara_map_replay_orphans` in `map.c`**

Add the following function (place it just before `dhara_map_resume`):

```c
int dhara_map_replay_orphans(struct dhara_map *m, dhara_error_t *err)
{
    struct dhara_journal *j = &m->journal;
    const dhara_page_t ppc_mask = (1u << j->log2_ppc) - 1u;
    dhara_page_t p;

    /* Start scanning from the page after the last checkpoint's last user page */
    p = j->root;

    for (;;) {
        dhara_sector_t sector;
        uint8_t new_meta[DHARA_META_SIZE];
        dhara_page_t slot;
        size_t offset;
        dhara_error_t my_err;

        /* Advance to next page */
        /* Use next_upage() semantics: increment, then skip if it's a CP page position */
        p++;
        /* If p is a checkpoint page, skip it */
        if ((p & ppc_mask) == ppc_mask) {
            p++;
        }

        /* Stop if we have reached the head (first free page) */
        if (p == j->head) {
            break;
        }

        /* Stop if page is free (unwritten) — truncates on invalid OOB */
        if (dhara_nand_is_free(j->nand, p)) {
            break;
        }

        /* Read LPN from OOB */
        if (dhara_nand_read_lpn(j->nand, p, &sector, err) < 0) {
            return -1;  /* hard NAND error */
        }

        /* Truncate if no valid LPN (erased OOB or unsupported driver) */
        if (sector == DHARA_SECTOR_NONE) {
            break;
        }

        /* Build the 132-byte metadata blob for this sector at the current root */
        /* trace_path walks from j->root; new_meta gets the alt-pointers */
        if (trace_path(m, sector, NULL, new_meta, &my_err) < 0) {
            if (my_err == DHARA_E_NOT_FOUND) {
                /* New sector — not previously mapped, increment count */
                m->count++;
                /* trace_path fills new_meta correctly even on NOT_FOUND */
            } else {
                /* Propagate real NAND read errors */
                if (err) *err = my_err;
                return -1;
            }
        }

        /* Set sector id in the meta blob (trace_path may not set it on NOT_FOUND) */
        meta_set_id(new_meta, sector);

        /* Patch the meta blob into page_buf at the slot for page p */
        slot = p & ppc_mask;
        offset = DHARA_HEADER_SIZE + DHARA_COOKIE_SIZE + slot * DHARA_META_SIZE;
        memcpy(j->page_buf + offset, new_meta, DHARA_META_SIZE);

        /* Advance j->root to this orphan page so the next trace_path starts here */
        j->root = p;
    }

    /* Update the cookie in page_buf with the reconciled count */
    ck_set_count(dhara_journal_cookie(j), m->count);

    return 0;
}
```

**Important:** `trace_path`, `meta_set_id`, `ck_set_count`, and `hdr_user_offset` are static functions or macros inside `map.c` and `journal.c`. Since `dhara_map_replay_orphans` lives in `map.c`, it has access to all of them. Do NOT move it to a separate file.

Check `map.c` carefully:
- `meta_set_id(meta, id)` — look for this macro/inline; it writes `id` to `meta[0..3]`
- `ck_set_count(cookie, count)` — writes `count` to the 4-byte cookie area
- `dhara_journal_cookie(j)` — returns `j->page_buf + DHARA_HEADER_SIZE`

If `trace_path` fills `new_meta` correctly on `DHARA_E_NOT_FOUND` (including the sector id), you do not need the explicit `meta_set_id` call. Verify by reading `trace_path` — it initializes `new_meta` to `DHARA_PAGE_NONE` for all alt-pointers and sets `meta_set_id(new_meta, target)` at the start.

**Step 5: Verify syntax**

```bash
gcc -fsyntax-only -I dhara/dhara dhara/dhara/dhara/map.c
```

**No commit yet.**

---

### Task 2.2: Call `dhara_map_replay_orphans` from `dhara_map_resume`

**Files:**
- Modify: `dhara/dhara/dhara/map.c`

**Step 1: Find `dhara_map_resume`**

```c
int dhara_map_resume(struct dhara_map *m, dhara_error_t *err)
{
    if (dhara_journal_resume(&m->journal, err) < 0) {
        m->count = 0;
        return -1;
    }
    m->count = ck_get_count(dhara_journal_cookie(&m->journal));
    return 0;
}
```

**Step 2: Add replay call**

```c
int dhara_map_resume(struct dhara_map *m, dhara_error_t *err)
{
    if (dhara_journal_resume(&m->journal, err) < 0) {
        m->count = 0;
        return -1;
    }

    m->count = ck_get_count(dhara_journal_cookie(&m->journal));

    if (dhara_map_replay_orphans(m, err) < 0) {
        m->count = 0;
        return -1;
    }

    return 0;
}
```

**Step 3: Build the upstream tests**

```bash
make -C dhara/dhara
```
Expected: all existing tests still pass (replay does nothing when there are no orphan pages — `p = j->root`, first iteration: `p++`, then `p == j->head`, breaks immediately).

**Step 4: Commit**

```bash
git add dhara/dhara/dhara/map.c dhara/dhara/dhara/map.h
git commit -m "feat(dhara): add orphan page replay on resume via OOB LPN"
```

---

## Phase 3 — Tests

### Task 3.1: Add OOB LPN round-trip test to `spi_nand_flash/host_test`

**Files:**
- Modify: `spi_nand_flash/host_test/main/test_nand_flash_bdl.cpp`

This test verifies that `nand_prog` writes LPN to OOB and `nand_read_lpn` reads it back correctly.

**Step 1: Write the failing test**

Add a new `TEST_CASE` at the end of `test_nand_flash_bdl.cpp`:

```cpp
TEST_CASE("OOB LPN round-trip: prog writes LPN, read_lpn reads it back", "[dhara_oob]")
{
    // Create a 50 MB emulated NAND (same pattern as existing tests)
    nand_file_mmap_emul_config_t emul_cfg = {
        .storage_size_bytes = 50 * 1024 * 1024,
        .keep_dump = false,
    };
    spi_nand_flash_config_t flash_cfg = {
        .emul_cfg = &emul_cfg,
    };
    spi_nand_flash_device_t *handle = nullptr;
    CHECK(nand_init_device(&flash_cfg, &handle) == ESP_OK);

    // Program page 0 with sector = 42
    std::vector<uint8_t> data(handle->chip.page_size, 0xAB);
    CHECK(nand_wrap_prog(handle, 0, data.data(), 42) == ESP_OK);

    // Read LPN back from page 0
    uint32_t sector_out = 0xDEADBEEF;
    CHECK(nand_read_lpn(handle, 0, &sector_out) == ESP_OK);
    CHECK(sector_out == 42);

    // Erased page (page 1) should return DHARA_SECTOR_NONE (0xFFFFFFFF)
    CHECK(nand_read_lpn(handle, 1, &sector_out) == ESP_OK);
    CHECK(sector_out == 0xFFFFFFFF);

    // DHARA_SECTOR_NONE as sector value should round-trip
    CHECK(nand_wrap_prog(handle, 2, data.data(), 0xFFFFFFFF) == ESP_OK);
    CHECK(nand_read_lpn(handle, 2, &sector_out) == ESP_OK);
    CHECK(sector_out == 0xFFFFFFFF);

    nand_deinit_device(handle);
}
```

Note: `nand_wrap_prog` wraps `nand_prog` — check `nand_impl_wrap.h` for the current signature. If it doesn't yet pass `sector`, update it in the same patch.

**Step 2: Run the test to verify it fails** (because `nand_read_lpn` doesn't exist yet in the public test API, or `nand_wrap_prog` doesn't accept sector):

```bash
cd spi_nand_flash/host_test && idf.py build 2>&1 | head -30
```
Expected: compile error.

**Step 3: Update `nand_impl_wrap.h` and `nand_impl_wrap.c`**

The `nand_wrap_prog` wrapper (used by tests) must also pass `sector`:
```c
// nand_impl_wrap.h
esp_err_t nand_wrap_prog(spi_nand_flash_device_t *handle, uint32_t p, const uint8_t *data, uint32_t sector);
```

Update `nand_impl_wrap.c` to forward `sector` to `nand_prog`.

**Step 4: Build and run**

```bash
cd spi_nand_flash/host_test
idf.py build
./build/nand_flash_host_test.elf "[dhara_oob]"
```
Expected: test passes.

**Step 5: Commit**

```bash
git add spi_nand_flash/host_test/main/test_nand_flash_bdl.cpp \
        spi_nand_flash/include/nand_private/nand_impl_wrap.h \
        spi_nand_flash/src/nand_impl_wrap.c
git commit -m "test(spi_nand_flash): add OOB LPN round-trip test"
```

---

### Task 3.2: Add orphan page replay integration test

**Files:**
- Modify: `spi_nand_flash/host_test/main/test_nand_flash_bdl.cpp`

This is the main correctness test. It simulates a power loss after user writes but before a checkpoint, then remounts and verifies data is recovered.

**Step 1: Write the failing test**

```cpp
TEST_CASE("Dhara orphan replay: writes after checkpoint are recovered on remount", "[dhara_oob][replay]")
{
    // Use a named file so we can remount with the same data
    nand_file_mmap_emul_config_t emul_cfg = {
        .storage_size_bytes = 50 * 1024 * 1024,
        .keep_dump = true,
        .flash_file_name = "/tmp/dhara_replay_test.nand",
    };
    spi_nand_flash_config_t flash_cfg = {.emul_cfg = &emul_cfg};

    // --- First mount: write sectors, simulate power loss before next checkpoint ---
    {
        spi_nand_flash_device_t *handle = nullptr;
        REQUIRE(nand_init_device(&flash_cfg, &handle) == ESP_OK);

        // Attach Dhara WL ops (creates dhara_map, calls dhara_map_resume)
        esp_blockdev_handle_t bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(handle, &bdl) == ESP_OK);

        // Write enough sectors to flush at least one checkpoint (fill one checkpoint group)
        // Then write N more sectors (orphan pages — no checkpoint yet)
        uint32_t cap = 0;
        REQUIRE(bdl->ops->get_capacity(bdl, &cap) == ESP_OK);

        std::vector<uint8_t> pattern(handle->chip.page_size, 0);
        // Write sectors 0..3 (likely covers one or more checkpoint groups)
        for (uint32_t s = 0; s < 4; s++) {
            std::fill(pattern.begin(), pattern.end(), (uint8_t)(0xA0 + s));
            REQUIRE(bdl->ops->write(bdl, pattern.data(), s) == ESP_OK);
        }

        // Force a checkpoint flush
        REQUIRE(spi_nand_flash_sync(handle) == ESP_OK);

        // Write 2 more sectors AFTER the checkpoint — these are the orphan pages
        for (uint32_t s = 4; s < 6; s++) {
            std::fill(pattern.begin(), pattern.end(), (uint8_t)(0xA0 + s));
            REQUIRE(bdl->ops->write(bdl, pattern.data(), s) == ESP_OK);
        }

        // Power loss: do NOT call sync. Just destroy the handle.
        // (The emul file persists because keep_dump=true)
        nand_flash_release_blockdev(bdl);
        nand_deinit_device(handle);
    }

    // --- Second mount: verify orphan pages are recovered ---
    {
        spi_nand_flash_device_t *handle = nullptr;
        REQUIRE(nand_init_device(&flash_cfg, &handle) == ESP_OK);

        esp_blockdev_handle_t bdl = nullptr;
        REQUIRE(nand_flash_get_blockdev(handle, &bdl) == ESP_OK);

        std::vector<uint8_t> buf(handle->chip.page_size, 0);

        // Sectors 0..3 (written before checkpoint): must be readable
        for (uint32_t s = 0; s < 4; s++) {
            REQUIRE(bdl->ops->read(bdl, buf.data(), s) == ESP_OK);
            CHECK(buf[0] == (uint8_t)(0xA0 + s));
        }

        // Sectors 4..5 (orphan pages after checkpoint): must also be readable after replay
        for (uint32_t s = 4; s < 6; s++) {
            REQUIRE(bdl->ops->read(bdl, buf.data(), s) == ESP_OK);
            CHECK(buf[0] == (uint8_t)(0xA0 + s));
        }

        nand_flash_release_blockdev(bdl);
        nand_deinit_device(handle);

        // Cleanup the file
        ::unlink("/tmp/dhara_replay_test.nand");
    }
}
```

**Step 2: Run to verify it fails** (orphan pages not yet recovered without the replay engine):

```bash
cd spi_nand_flash/host_test && idf.py build && ./build/nand_flash_host_test.elf "[replay]"
```
Expected: FAIL — sectors 4 and 5 return wrong data or `DHARA_E_NOT_FOUND`.

**Step 3: Implement the replay engine** (Task 2.1 + 2.2 should already be done; if not, do them now).

**Step 4: Re-run the test**

```bash
./build/nand_flash_host_test.elf "[replay]"
```
Expected: PASS.

**Step 5: Commit**

```bash
git add spi_nand_flash/host_test/main/test_nand_flash_bdl.cpp
git commit -m "test(spi_nand_flash): add orphan page replay integration test"
```

---

### Task 3.3: Edge case tests

**Files:**
- Modify: `spi_nand_flash/host_test/main/test_nand_flash_bdl.cpp`

Add table-driven edge case tests for the scenarios in the original design note. Each test is a separate `TEST_CASE`.

**Step 1: Write edge case tests**

```cpp
TEST_CASE("Replay: no orphans (clean shutdown)", "[dhara_oob][replay]")
{
    // Write sectors, call sync, remount — sectors must be present.
    // This verifies replay doesn't corrupt already-checkpointed data.
}

TEST_CASE("Replay: last write exactly on checkpoint boundary (zero orphans)", "[dhara_oob][replay]")
{
    // Fill exactly one checkpoint group, call sync.
    // Remount — verify all sectors present, count is correct.
}

TEST_CASE("Replay: single orphan page (one write after last checkpoint)", "[dhara_oob][replay]")
{
    // Write to fill checkpoint group, sync, write exactly one more sector, power-loss.
    // Remount — verify the one orphan sector is recovered.
}

TEST_CASE("Replay: OOB invalid mid-extent truncates replay safely", "[dhara_oob][replay]")
{
    // This test requires the ability to corrupt OOB of a specific page in the emulator.
    // If the mmap emulator exposes direct OOB access, zero out OOB bytes 4-7 of an
    // orphan page. Remount — replay should truncate at that page, earlier orphans recovered.
    // (Skip if emulator does not expose OOB directly.)
}
```

Implement the bodies following the pattern from Task 3.2 (named file, keep_dump, two-mount pattern).

**Step 2: Run all edge case tests**

```bash
./build/nand_flash_host_test.elf "[replay]"
```
Expected: all pass.

**Step 3: Run full test suite to check for regressions**

```bash
./build/nand_flash_host_test.elf
```
Expected: `All tests passed`.

**Step 4: Commit**

```bash
git add spi_nand_flash/host_test/main/test_nand_flash_bdl.cpp
git commit -m "test(spi_nand_flash): add edge case tests for OOB replay (boundary, truncation)"
```

---

## Phase 4 — Hardening and diagnostics

### Task 4.1: Add compile-time trace logging

**Files:**
- Modify: `dhara/dhara/dhara/map.c`

Gate verbose replay tracing behind a preprocessor flag so production builds are silent.

**Step 1: Add trace macro**

At the top of `map.c`, after includes:
```c
#ifdef DHARA_TRACE_REPLAY
#define REPLAY_TRACE(fmt, ...) fprintf(stderr, "[dhara replay] " fmt "\n", ##__VA_ARGS__)
#else
#define REPLAY_TRACE(fmt, ...) do {} while (0)
#endif
```

**Step 2: Add trace calls in `dhara_map_replay_orphans`**

```c
REPLAY_TRACE("scanning page %u: sector=%u", (unsigned)p, (unsigned)sector);
REPLAY_TRACE("replayed %u orphan page(s), new count=%u", replay_count, (unsigned)m->count);
```

**Step 3: Add a counter for diagnostic purposes**

Track how many pages were replayed. Log at the end:
```c
unsigned replay_count = 0;
/* ... inside loop ... */
replay_count++;
/* ... after loop ... */
REPLAY_TRACE("replay complete: %u orphan(s) applied", replay_count);
```

**Step 4: Commit**

```bash
git add dhara/dhara/dhara/map.c
git commit -m "feat(dhara): add DHARA_TRACE_REPLAY compile-time diagnostics for orphan replay"
```

---

### Task 4.2: Handle block boundaries in replay scan

**Files:**
- Modify: `dhara/dhara/dhara/map.c`

The current `dhara_map_replay_orphans` uses a simplified page advance (`p++`, then skip if CP). This works within a single block. Verify it handles the case where orphan pages span a block boundary (the head advanced to a new block).

**Step 1: Review `next_upage()` in `journal.c`**

`next_upage(j, p)` is the canonical way to advance to the next user page. It skips CP pages and handles block wrap. Check if it also handles bad blocks (it should not — bad block discovery is done at write time, not at read time).

**Step 2: Replace manual advance in `dhara_map_replay_orphans` with `next_upage`**

If `next_upage` is static in `journal.c`, either:
- Move the replay loop into `journal.c` (less clean), or
- Duplicate the `next_upage` logic in `map.c` as a local static helper (2-3 lines, acceptable), or
- Expose `next_upage` from `journal.c` with a non-static declaration in `journal.h` (cleanest).

Preferred: expose `next_upage` via `journal.h` as `dhara_journal_next_upage(j, p)`.

**Step 3: Add a test with orphans spanning a block boundary**

In `test_nand_flash_bdl.cpp`, write a test that fills an entire block with checkpoint groups, then writes orphan pages at the start of the next block. Verify replay picks them up.

**Step 4: Run tests, commit**

```bash
./build/nand_flash_host_test.elf
git add dhara/dhara/dhara/map.c dhara/dhara/dhara/journal.h dhara/dhara/dhara/journal.c \
        spi_nand_flash/host_test/main/test_nand_flash_bdl.cpp
git commit -m "fix(dhara): use next_upage for block-boundary-safe orphan scan"
```

---

### Task 4.3: Verify `nand_impl.c` (hardware path) compiles with new signatures

**Files:**
- Modify: `spi_nand_flash/src/nand_impl.c` (hardware SPI implementation)

The hardware implementation `nand_impl.c` also implements `nand_prog`, `nand_copy`, and now needs `nand_read_lpn`.

**Step 1: Update `nand_prog` in `nand_impl.c`**

Add `sector` parameter. For real SPI NAND hardware, the LPN should be written to the OOB spare area using the chip's program sequence. The exact implementation depends on the SPI NAND driver's spare-write API. For now, a stub is acceptable:

```c
esp_err_t nand_prog(spi_nand_flash_device_t *handle, uint32_t p, const uint8_t *data, uint32_t sector)
{
    /* TODO: write sector to OOB bytes 4-7 in the same PROGRAM EXECUTE command */
    /* Existing implementation writes main area; OOB write to be added per-chip */
    ...existing code...
}
```

**Step 2: Update `nand_copy` in `nand_impl.c`** — same pattern.

**Step 3: Add `nand_read_lpn` stub in `nand_impl.c`**

```c
esp_err_t nand_read_lpn(spi_nand_flash_device_t *handle, uint32_t p, uint32_t *sector_out)
{
    /* TODO: read OOB bytes 4-7 from real NAND hardware */
    /* Stub: return SECTOR_NONE until hardware OOB read is implemented */
    *sector_out = 0xFFFFFFFF;
    return ESP_OK;
}
```

This stub means replay will truncate immediately on real hardware until the hardware OOB read is implemented. That is safe — it's the same behavior as before this feature.

**Step 4: Verify firmware build compiles** (requires IDF toolchain):

```bash
# Inside the spi_nand_flash component, just check syntax against the hardware target
# (Actual firmware build requires full IDF + target SDK)
echo "Verify nand_impl.c compiles with new signatures"
```

**Step 5: Commit**

```bash
git add spi_nand_flash/src/nand_impl.c
git commit -m "feat(spi_nand_flash): stub OOB LPN write/read in hardware nand_impl (TODO: per-chip impl)"
```

---

## Phase 5 — Run full test suite and verify

### Task 5.1: Run all tests

**Step 1: Run upstream native tests**

```bash
make -C dhara/dhara
```
Expected: all tests pass.

**Step 2: Run host tests**

```bash
cd spi_nand_flash/host_test
idf.py build
./build/nand_flash_host_test.elf
```
Expected: `All tests passed` (or Catch2 equivalent summary).

**Step 3: Run pytest**

```bash
cd spi_nand_flash/host_test
pytest pytest_nand_flash_linux.py -v
```
Expected: all pass.

**Step 4: Fix any failures before declaring done.**

---

## Appendix: Decisions Made

| Decision | Choice | Rationale |
|---|---|---|
| OOB interface | Extend `dhara_nand_prog/copy` signatures + add `dhara_nand_read_lpn` callback | Dhara has LPN at program time (in meta blob); passing it directly is clean. No separate vtable needed. |
| OOB layout | 4 bytes LE at OOB offset 4 (bytes 0–3 reserved for bad-block/free markers per nand_impl_linux convention) | Minimal. `0xFFFFFFFF` = erased/no-LPN. |
| Extent discovery | Forward scan from `j->root+1` until `dhara_nand_is_free` returns true | Simple and correct for sequential-write journals. |
| Invalid OOB mid-extent | Truncate (stop replay) | Conservative, safe. |
| Replay metadata | Full `trace_path` per orphan, updating `j->root` after each | Correct radix-tree semantics. |
| Sector count | Reconcile via `trace_path` return value (`DHARA_E_NOT_FOUND` = new sector, increment `m->count`) | Same logic as normal write path. |
| Test location | Extend `spi_nand_flash/host_test/` | Already has mmap NAND emulator + IDF pytest infra. |
| Feature enable | Always enabled; driver stubs `read_lpn` returning `DHARA_SECTOR_NONE` to opt out | Zero overhead for non-OOB drivers. No Kconfig needed. |
| Diagnostics | `DHARA_TRACE_REPLAY` compile-time flag in `map.c` | Quiet in production, verbose in debug. |

## Appendix: Edge Cases (test checklist)

| Scenario | Expected behavior | Test? |
|---|---|---|
| Clean shutdown (sync before power loss) | No orphans, replay loop exits immediately | Task 3.3 |
| Last write on checkpoint boundary | Zero orphans | Task 3.3 |
| Single orphan page | One sector recovered | Task 3.3 |
| Invalid OOB mid-extent | Replay truncates, earlier orphans recovered | Task 3.3 |
| Orphans span block boundary | All orphans recovered | Task 4.2 |
| `read_lpn` returns `DHARA_SECTOR_NONE` (driver opt-out) | Replay truncates immediately, no crash | Implicit in round-trip test |
| Duplicate LPN (should not occur normally) | Last physical page wins (trace_path is idempotent: last `j->root` update wins) | Not tested (undefined behavior per design) |
| Fresh device (no checkpoint) | `journal_resume` returns -1; `map_resume` returns -1 before replay is called | Existing upstream tests |
