# Configurable `DHARA_RADIX_DEPTH` — Build-Time Parameterization Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the hardcoded `DHARA_RADIX_DEPTH = 32` constant with a build-time configurable value via `Kconfig`/`sdkconfig`, reducing metadata record size, checkpoint overhead, lookup I/O, and stack usage for devices that require fewer than 32 address bits.

**Architecture:** `DHARA_RADIX_DEPTH` and the derived `DHARA_META_SIZE` are converted from hardcoded C preprocessor constants to `Kconfig`-driven values. Because both are compile-time constants, all existing code (loops, stack arrays, bit arithmetic) remains structurally unchanged — only the numeric value varies. A helper macro is added to `map.h` so users can compute the correct depth for their device. An on-flash format version byte is added to the checkpoint header to detect mismatches at mount time, preventing silent data corruption when the depth setting has been changed.

**Tech Stack:** C99, ESP-IDF component Kconfig, Dhara upstream source files (`journal.h`, `journal.c`, `map.c`, `map.h`). No new dependencies.

---

## Background & Motivation

### Why this exists

Every Dhara metadata record (written alongside each user data page) contains:

```
Bytes  0–3:    id      — 32-bit logical sector number
Bytes  4–131:  alt[0..31] — 32 × 4-byte physical page numbers
```

`DHARA_META_SIZE = 132` is derived from `DHARA_RADIX_DEPTH = 32`:

```c
// map.c:26
#define DHARA_RADIX_DEPTH  (sizeof(dhara_sector_t) << 3)   // always 32

// journal.h:35
#define DHARA_META_SIZE    132   // = 4 + 32×4, hardcoded independently
```

Note: `DHARA_META_SIZE` is currently a *separate* hardcoded constant, not derived from `DHARA_RADIX_DEPTH`. This plan makes it derived.

### The waste

A 128 MiB NAND with 2 KiB pages and 64 pages/block has:
- `num_blocks = 1024`, `pages_per_block = 64`
- `max_sectors ≈ 1024 × 64 / 2 = 32 768`
- Bits required: `ceil(log2(32768)) = 15`

With `DHARA_RADIX_DEPTH = 32`, alt-pointers 15–31 are **always `0xFFFFFFFF`** (NONE). That is `17 × 4 = 68 bytes of dead padding per record`.

### Impact on `choose_ppc`

`choose_ppc()` in `journal.c` computes the maximum checkpoint period (`log2_ppc`) that fits in one page:

```
Available bytes per checkpoint page = page_size − HEADER(16) − COOKIE(4) = page_size − 20
Records that fit                    = floor(available / DHARA_META_SIZE)
log2_ppc                            = floor(log2(records + 1))
```

For **2 KiB pages**:

| `DHARA_RADIX_DEPTH` | `DHARA_META_SIZE` | Records fit | `log2_ppc` | User pages per cp group | Checkpoint overhead |
|---|---|---|---|---|---|
| 32 (current) | 132 B | 15 | **3** | 7 | **12.5 %** |
| 15 (optimal 128 MiB) | 64 B | 31 | **4** | 15 | **6.25 %** |
| 16 | 68 B | 29 | **4** | 15 | **6.25 %** |
| 20 | 84 B | 24 | **4** | 15 | **6.25 %** |
| 8 | 36 B | 56 | **5** | 31 | **3.1 %** |

For **512-byte pages** (tiny NAND):

| `DHARA_RADIX_DEPTH` | `DHARA_META_SIZE` | Records fit | `log2_ppc` | User pages per cp group | Checkpoint overhead |
|---|---|---|---|---|---|
| 32 (current) | 132 B | 3 | **1** | **1** | **50 %** |
| 15 | 64 B | 7 | **2** | **3** | **25 %** |

The improvement on small-page NAND is dramatic.

### Cascade of improvements

For a correctly tuned depth on a 128 MiB / 2 KiB-page NAND:

1. **Checkpoint overhead halved** (12.5 % → 6.25 %) — more user pages per erase block
2. **Metadata lookup hops halved** — `trace_path` loop runs 15 iterations instead of 32
3. **Stack per `trace_path` frame: −68 bytes** — `uint8_t meta[DHARA_META_SIZE]` shrinks from 132 B to 64 B; `try_delete` which allocates two such buffers saves 136 B of stack
4. **GC efficiency** — same amount of data, fewer metadata pages, fewer GC passes

---

## Constraints & Risks

### Breaking on-flash format change

Changing `DHARA_RADIX_DEPTH` changes `DHARA_META_SIZE`, which changes `hdr_user_offset(which)`:

```c
// journal.c:94–97
static inline size_t hdr_user_offset(uint8_t which)
{
    return DHARA_HEADER_SIZE + DHARA_COOKIE_SIZE + which * DHARA_META_SIZE;
}
```

Every metadata record in every checkpoint page is at a different byte offset. A flash image written with depth=32 is **completely unreadable** with depth=15 and vice versa — it will produce garbage sector mappings or fail validation silently.

**Mitigation:** Store `DHARA_RADIX_DEPTH` in the checkpoint header and verify it on `resume()`. See Task 3.

### The 16-byte header has no spare bytes

Current layout (all fields 100 % occupied):
```
buf[0..2]   = "Dha"  (magic)
buf[3]      = epoch
buf[4..7]   = tail   (32-bit page number)
buf[8..11]  = bb_current
buf[12..15] = bb_last
```

Adding a `radix_depth` byte requires **extending the header to 17 bytes** (or repurposing the padding in `epoch`, which is only 1 byte but fully used). The cleanest solution: extend `DHARA_HEADER_SIZE` from 16 to 20 bytes (keeps 4-byte alignment), using the extra 3 bytes as:
- `buf[16]` = `radix_depth` (uint8_t)
- `buf[17..19]` = reserved / zero

This reduces available space for metadata by 4 bytes per checkpoint page — negligible.

**Alternative:** Leave `DHARA_HEADER_SIZE = 16` and encode `radix_depth` in the high nibble of `epoch` (epoch only needs ~4 bits for reasonable flash sizes). This is clever but fragile. Prefer the clean header extension.

### No C code logic changes required

Because `DHARA_RADIX_DEPTH` and `DHARA_META_SIZE` are compile-time constants, every use site — loops, stack arrays, bit shifts — remains valid C. There are no VLAs, no heap allocations, no runtime branches. This is the entire appeal of the build-time approach.

### Must not break default configuration

Default `CONFIG_DHARA_RADIX_DEPTH = 32` preserves current behavior (modulo the header extension). Existing Dhara users who never set the option see no change in behavior.

---

## Pros and Cons

### Pros

| # | Benefit | Magnitude |
|---|---------|-----------|
| 1 | Halved checkpoint overhead on typical 128 MiB NAND (2 KiB pages) | High |
| 2 | 3× lower checkpoint overhead on 512-byte-page NAND | Very High |
| 3 | Halved worst-case lookup I/O (15 vs 32 metadata reads per `trace_path`) | Medium |
| 4 | −68 to −136 bytes of stack per write/find operation | Medium (MCU matters) |
| 5 | Zero code logic changes — only constant values change | Low risk |
| 6 | Natural ESP-IDF integration via `sdkconfig` | Good DX |
| 7 | Self-documenting: `DHARA_RADIX_DEPTH_FOR()` helper tells users what value to use | Good DX |

### Cons

| # | Drawback | Mitigation |
|---|----------|------------|
| 1 | **Breaking on-flash format change** — existing flash images are unreadable | Header version byte (Task 3); document clearly |
| 2 | Header extends by 4 bytes — loses 4 bytes of metadata space per checkpoint page | Immaterial: saves 64+ bytes per record |
| 3 | Two independent constants (`DHARA_RADIX_DEPTH` in map.c, `DHARA_META_SIZE` in journal.h) must stay in sync | Make `DHARA_META_SIZE` derived from `DHARA_RADIX_DEPTH` (Task 1) |
| 4 | User must know their device geometry to pick the right value | `DHARA_RADIX_DEPTH_FOR()` helper macro; Kconfig help text |
| 5 | Wrong value (too small) causes `trace_path` to stop short — silent data loss | Assertion in `dhara_map_init()` + Kconfig range validation |
| 6 | Slightly harder to reason about firmware updates that change the setting | Document: "changing this requires reformatting the flash" |

---

## File Map

All changes are in the **upstream Dhara source** tree (vendored into this ESP-IDF component at `dhara/dhara/`). The component wrapper (`CMakeLists.txt`) gains a `Kconfig` file.

```
idf-extra-components/dhara/
├── CMakeLists.txt          [MODIFY] — add Kconfig
├── Kconfig                 [CREATE] — CONFIG_DHARA_RADIX_DEPTH
├── dhara/dhara/
│   ├── journal.h           [MODIFY] — DHARA_HEADER_SIZE, DHARA_META_SIZE derived
│   ├── journal.c           [MODIFY] — hdr_set/get radix_depth, choose_ppc, resume check
│   ├── map.h               [MODIFY] — DHARA_RADIX_DEPTH_FOR() helper macro
│   └── map.c               [MODIFY] — DHARA_RADIX_DEPTH from config, assertion
└── docs/plans/
    └── 2026-04-29-configurable-radix-depth.md  [this file]
```

---

## Implementation Plan

### Task 1 — Kconfig entry and `DHARA_RADIX_DEPTH` macro

**Files:**
- Create: `Kconfig`
- Modify: `dhara/dhara/map.c` (lines 26–30, the `DHARA_RADIX_DEPTH` define and `d_bit`)
- Modify: `dhara/dhara/journal.h` (line 35, `DHARA_META_SIZE`)

**Goal:** Wire `DHARA_RADIX_DEPTH` to `CONFIG_DHARA_RADIX_DEPTH` (with fallback to 32 for builds outside ESP-IDF). Make `DHARA_META_SIZE` a derived expression, not an independent constant.

**Step 1: Create `Kconfig`**

```kconfig
menu "Dhara NAND FTL"

config DHARA_RADIX_DEPTH
    int "Radix tree depth (address bits)"
    default 32
    range 8 32
    help
      Number of bits used to address logical sectors in the Dhara radix
      tree map. Each metadata record stores this many 4-byte alt-pointers
      plus a 4-byte sector ID, so DHARA_META_SIZE = 4 + DHARA_RADIX_DEPTH * 4.

      Set this to ceil(log2(max_sectors)) for your device to reduce
      metadata overhead. For a device with N blocks of P pages each,
      max_sectors ≈ N*P/2, and the required depth is ceil(log2(max_sectors)).

      Use the DHARA_RADIX_DEPTH_FOR(num_blocks, log2_ppb) macro in map.h
      to compute the correct value.

      Default (32) is backward-compatible with all existing on-flash images
      that were written without a radix_depth header field.

      WARNING: Changing this value makes existing on-flash data unreadable.
      You must erase and reformat the NAND after changing this setting.

endmenu
```

**Step 2: Modify `dhara/dhara/map.c` — change the `DHARA_RADIX_DEPTH` define**

Replace:
```c
#define DHARA_RADIX_DEPTH	(sizeof(dhara_sector_t) << 3)
```
With:
```c
#ifdef CONFIG_DHARA_RADIX_DEPTH
#define DHARA_RADIX_DEPTH	CONFIG_DHARA_RADIX_DEPTH
#else
#define DHARA_RADIX_DEPTH	(sizeof(dhara_sector_t) << 3)
#endif
```

**Step 3: Modify `dhara/dhara/journal.h` — make `DHARA_META_SIZE` derived**

Replace:
```c
#define DHARA_META_SIZE			132
```
With:
```c
/* Size of one metadata record: 4-byte sector ID + DHARA_RADIX_DEPTH 4-byte alt-pointers.
 * DHARA_RADIX_DEPTH must be defined before including this header (from map.c/map.h).
 * For code that includes journal.h without map.h, falls back to 32-bit depth. */
#ifndef DHARA_RADIX_DEPTH
#define DHARA_RADIX_DEPTH		32
#endif
#define DHARA_META_SIZE			(4 + (DHARA_RADIX_DEPTH) * 4)
```

> **Note on include order:** `journal.h` is included by `journal.c` directly (without going through `map.h`). The `#ifndef DHARA_RADIX_DEPTH` fallback ensures `journal.c` still compiles standalone with the correct depth from `Kconfig`, because `map.c`-only defines `DHARA_RADIX_DEPTH` inside its own translation unit. The fix: move the `DHARA_RADIX_DEPTH` define to `journal.h` (or a new shared `dhara_config.h`) so both `map.c` and `journal.c` see the same value. See Step 3 addendum below.

**Step 3 addendum — move depth definition to `journal.h` as the single source of truth**

In `journal.h`, before `DHARA_META_SIZE`:
```c
/* Radix tree depth = number of address bits = number of alt-pointers per record.
 * Configurable at build time via CONFIG_DHARA_RADIX_DEPTH (see Kconfig).
 * Must be in range [8, 32]. Changing this is an on-flash format break. */
#ifdef CONFIG_DHARA_RADIX_DEPTH
#define DHARA_RADIX_DEPTH		CONFIG_DHARA_RADIX_DEPTH
#else
#define DHARA_RADIX_DEPTH		32
#endif

#define DHARA_META_SIZE			(4 + (DHARA_RADIX_DEPTH) * 4)
```

In `map.c`, remove the local `DHARA_RADIX_DEPTH` define entirely (it will come from `journal.h` via `map.h → journal.h` or `map.c → map.h → journal.h`). Verify the include chain: `map.c` includes `map.h`; check whether `map.h` includes `journal.h`. If not, add `#include "journal.h"` to `map.h`.

**Step 4: Verify compilation**

```bash
cd /path/to/dhara/tests   # or ESP-IDF project
idf.py build
```
Expected: builds cleanly with no warnings about implicit `DHARA_META_SIZE` or `DHARA_RADIX_DEPTH`.

**Step 5: Sanity-check the values**

Add a `static_assert` in `map.c` (after the define):
```c
_Static_assert(DHARA_RADIX_DEPTH >= 8 && DHARA_RADIX_DEPTH <= 32,
    "DHARA_RADIX_DEPTH must be in [8, 32]");
_Static_assert(DHARA_META_SIZE == 4 + DHARA_RADIX_DEPTH * 4,
    "DHARA_META_SIZE derivation mismatch");
```

**Step 6: Commit**

```bash
git add Kconfig dhara/dhara/journal.h dhara/dhara/map.c
git commit -m "feat(dhara): make DHARA_RADIX_DEPTH build-time configurable via Kconfig

DHARA_META_SIZE is now derived as (4 + DHARA_RADIX_DEPTH * 4) instead of
the hardcoded 132. Default remains 32 (backward-compatible constant value).

No on-flash format change at this step (header version byte added in next task)."
```

---

### Task 2 — Helper macro `DHARA_RADIX_DEPTH_FOR()`

**Files:**
- Modify: `dhara/dhara/map.h`

**Goal:** Give users a way to compute the correct depth for their device without doing the math by hand.

**Step 1: Add the macro to `map.h`**

After the existing `#include`s and before the typedefs:
```c
/**
 * @brief Compute the minimum DHARA_RADIX_DEPTH for a given device geometry.
 *
 * Returns the number of bits needed to address all logical sectors on a device
 * with num_blocks erase-blocks and 2^log2_ppb pages per block.
 *
 * max_sectors ≈ (num_blocks * 2^log2_ppb) / 2   (half reserved for GC)
 * required depth = ceil(log2(max_sectors))
 *
 * Example: 1024 blocks, 64 pages/block (log2_ppb=6):
 *   max_sectors = 1024 * 64 / 2 = 32768 → depth = 15
 *
 * Use this value as CONFIG_DHARA_RADIX_DEPTH in your sdkconfig or Kconfig.default.
 */
#define DHARA_RADIX_DEPTH_FOR(num_blocks, log2_ppb) \
    __builtin_clz(1) - __builtin_clz(((num_blocks) << (log2_ppb)) / 2 - 1)
```

> **Note:** `__builtin_clz` is GCC/Clang only — universally available on ESP-IDF. For portability outside ESP-IDF, provide a fallback using a compile-time loop.
>
> The formula: `bits = 32 - __builtin_clz(max_sectors - 1)` computes ceil(log2).
> Rewritten: `DHARA_RADIX_DEPTH_FOR(nb, ppb) = 32 - __builtin_clz(((nb) << (ppb)) / 2 - 1)`.
> Edge case: if `max_sectors` is already a power of 2, `max_sectors - 1` keeps the `__builtin_clz` correct (ceil not floor).
> Add a `_Static_assert` check in test or documentation comment.

**Step 2: Add usage example in Kconfig help text** (edit the Kconfig from Task 1 to reference this macro by name).

**Step 3: Commit**

```bash
git add dhara/dhara/map.h Kconfig
git commit -m "feat(dhara): add DHARA_RADIX_DEPTH_FOR() helper macro

Allows users to compute the optimal CONFIG_DHARA_RADIX_DEPTH for
their specific device geometry at compile time."
```

---

### Task 3 — Checkpoint header version byte (format guard)

**Files:**
- Modify: `dhara/dhara/journal.h` — extend `DHARA_HEADER_SIZE` to 20
- Modify: `dhara/dhara/journal.c` — add `hdr_set/get_radix_depth()`, write on checkpoint, verify on resume

**Goal:** Store `DHARA_RADIX_DEPTH` in the checkpoint header so `dhara_journal_resume()` can detect a depth mismatch and return an error instead of producing corrupt mappings.

**Step 1: Understand the current 16-byte header layout**

```
buf[0..2]  = "Dha" (magic)
buf[3]     = epoch (uint8_t)
buf[4..7]  = tail (uint32_t, little-endian)
buf[8..11] = bb_current (uint32_t)
buf[12..15]= bb_last (uint32_t)
```

All 16 bytes are occupied. We must extend to 20 bytes.

**Step 2: Extend `DHARA_HEADER_SIZE` in `journal.h`**

```c
// Before:
#define DHARA_HEADER_SIZE  16

// After:
#define DHARA_HEADER_SIZE  20
```

The new bytes `buf[16..19]`:
```
buf[16]    = radix_depth (uint8_t) — DHARA_RADIX_DEPTH at write time
buf[17..19]= reserved, written as 0x00, ignored on read
```

**Step 3: Add accessor functions in `journal.c`** (after the existing `hdr_set_bb_last`):

```c
static inline uint8_t hdr_get_radix_depth(const uint8_t *buf)
{
    return buf[16];
}

static inline void hdr_set_radix_depth(uint8_t *buf, uint8_t depth)
{
    buf[16] = depth;
    buf[17] = 0;
    buf[18] = 0;
    buf[19] = 0;
}
```

**Step 4: Write `radix_depth` when a checkpoint is flushed**

Find where `hdr_set_bb_last()` is called (the checkpoint-write path in `push_meta()` / `flush_meta()`). Immediately after it, add:

```c
hdr_set_radix_depth(j->page_buf, (uint8_t)DHARA_RADIX_DEPTH);
```

Search for the call site:
```bash
grep -n "hdr_set_bb_last\|hdr_put_magic\|hdr_set_epoch" /path/to/journal.c
```
All three are called together in the checkpoint-flush function — add `hdr_set_radix_depth` in the same block.

**Step 5: Verify on `dhara_journal_resume()`**

In `dhara_journal_resume()`, after a valid checkpoint header is found and validated (after `hdr_has_magic()` passes), add:

```c
const uint8_t stored_depth = hdr_get_radix_depth(j->page_buf);
/* Images written before this change have buf[16]=0xFF (erased).
 * Treat 0xFF as "unknown / legacy depth=32". */
if (stored_depth != 0xFF && stored_depth != (uint8_t)DHARA_RADIX_DEPTH) {
    dhara_set_error(err, DHARA_E_BAD_BLOCK); /* reuse closest error, or add new code */
    return -1;
}
```

> **On legacy images (depth=32 written before this change):** `buf[16]` was part of `hdr_clear_user()` which zeros/0xFF-fills the page. On unerased flash it will be `0xFF`. Since the default depth is 32, and `0xFF != 32`, this would falsely reject old images. **Two options:**
> 1. **Treat `0xFF` as "legacy, assume depth=32"** — safe only when `DHARA_RADIX_DEPTH == 32`. If the user has configured a non-32 depth, legacy images must be rejected anyway (they are). If they're using depth=32, it's a transparent upgrade.
> 2. **Only check when `DHARA_RADIX_DEPTH != 32`** — simpler but less safe.
>
> Option 1 is recommended. Add a comment explaining this.

**Step 6: Verify the available space for metadata is not violated**

`choose_ppc()` uses `DHARA_HEADER_SIZE` in its calculation:
```c
const int max_meta = (1 << log2_page_size) - DHARA_HEADER_SIZE - DHARA_COOKIE_SIZE;
```

With `DHARA_HEADER_SIZE = 20` (was 16), available space decreases by 4 bytes. Recheck that `log2_ppc` values don't regress for common page sizes:

| Page size | DHARA_RADIX_DEPTH | Old max_meta | New max_meta | Old log2_ppc | New log2_ppc |
|-----------|---|---|---|---|---|
| 2048 | 32 | 2028 | 2024 | 3 (7 records × 132 = 924) | 3 (unchanged) |
| 2048 | 15 | 2028 | 2024 | 4 (15 records × 64 = 960) | 4 (unchanged) |
| 512  | 32 | 492  | 488  | 1 (1 record × 132 = 132) | 1 (unchanged) |
| 512  | 15 | 492  | 488  | 2 (3 records × 64 = 192) | 2 (unchanged) |

No regression. The −4 bytes is absorbed by the slack in each checkpoint page.

**Step 7: Compile and run existing tests**

```bash
# In the upstream dhara test suite:
make -C /Users/martinhavlik/personal/dhara tests
./tests/run_all.sh
```
Expected: all tests pass.

**Step 8: Commit**

```bash
git add dhara/dhara/journal.h dhara/dhara/journal.c
git commit -m "feat(dhara): store radix_depth in checkpoint header

Extends DHARA_HEADER_SIZE from 16 to 20 bytes. buf[16] now stores the
DHARA_RADIX_DEPTH used when the image was written. On resume(), a mismatch
returns an error, preventing silent data corruption when the depth setting
has been changed between firmware versions.

Legacy images (buf[16] == 0xFF) are accepted only when DHARA_RADIX_DEPTH == 32."
```

---

### Task 4 — Assertion in `dhara_map_init()`

**Files:**
- Modify: `dhara/dhara/map.c`

**Goal:** Catch misconfiguration at mount time: if `DHARA_RADIX_DEPTH` is too small to address all sectors on the attached device, assert loudly rather than silently losing data.

**Step 1: Add the capacity check in `dhara_map_init()`**

After `dhara_journal_init()` returns:
```c
void dhara_map_init(struct dhara_map *m, const struct dhara_nand *n,
                    uint8_t *page_buf, uint8_t gc_ratio)
{
    /* ... existing code ... */
    dhara_journal_init(&m->journal, n, page_buf);

    /* Verify that DHARA_RADIX_DEPTH is sufficient for this device.
     * max_sectors = (num_blocks * pages_per_block) / 2 (conservative GC reserve).
     * We need: (1u << DHARA_RADIX_DEPTH) >= max_sectors
     */
    {
        const uint32_t max_sectors =
            ((uint32_t)n->num_blocks << n->log2_ppb) >> 1;
        /* If DHARA_RADIX_DEPTH >= 32, skip: 2^32 always >= any 32-bit value */
#if DHARA_RADIX_DEPTH < 32
        assert((1u << DHARA_RADIX_DEPTH) >= max_sectors &&
               "DHARA_RADIX_DEPTH too small for this device geometry. "
               "Increase CONFIG_DHARA_RADIX_DEPTH in sdkconfig.");
#endif
    }

    m->gc_ratio = gc_ratio ? gc_ratio : 1;
}
```

**Step 2: Commit**

```bash
git add dhara/dhara/map.c
git commit -m "feat(dhara): assert DHARA_RADIX_DEPTH is sufficient for device geometry

Catches misconfiguration at init time rather than producing silent
incorrect mappings when too few address bits are configured."
```

---

### Task 5 — Documentation update

**Files:**
- Modify: `README.md` (or create `docs/configuration.md`)
- Modify: `DHARA_DEEP_DIVE.md` §12.7 — mark as implemented

**Step 1: Add a "Configuration" section to `README.md`**

```markdown
## Configuration

### `CONFIG_DHARA_RADIX_DEPTH` (default: 32)

Controls the number of bits used to address logical sectors in Dhara's
radix-tree map. This directly determines the size of every metadata record
stored in checkpoint pages:

    DHARA_META_SIZE = 4 + DHARA_RADIX_DEPTH × 4 bytes

**Why reduce it?** The default of 32 supports devices with up to 2³² sectors
(~4 TiB at 1-byte sector size), far beyond any real embedded NAND. For a
typical 128 MiB NAND with 2 KiB pages, only 15 bits are needed. Using 15
instead of 32 halves the checkpoint page overhead and halves the worst-case
lookup I/O.

**How to compute the right value:**

```c
// In your app or board header:
#include "dhara/map.h"
// Then in sdkconfig or Kconfig.projdefaults:
// CONFIG_DHARA_RADIX_DEPTH = DHARA_RADIX_DEPTH_FOR(num_blocks, log2_ppb)
```

Or use the formula directly:
- `max_sectors = num_blocks × pages_per_block / 2`
- `depth = ceil(log2(max_sectors))`

| Device | num_blocks | PPB | depth |
|--------|-----------|-----|-------|
| 128 MiB, 2 KiB page, 64 ppb | 1024 | 64 | 15 |
| 256 MiB, 2 KiB page, 64 ppb | 2048 | 64 | 16 |
| 1 GiB, 4 KiB page, 64 ppb | 2048 | 64 | 16 |
| 4 GiB, 4 KiB page, 128 ppb | 8192 | 128 | 19 |

**⚠️ Warning:** Changing this setting after formatting is a **breaking on-flash format change**. You must erase and reformat the NAND. Dhara will return an error on `resume()` if the stored depth doesn't match the compiled depth.
```

**Step 2: Update `DHARA_DEEP_DIVE.md` §12.7**

Append `> ✅ Implemented: see [docs/plans/2026-04-29-configurable-radix-depth.md]` to the section.

**Step 3: Commit**

```bash
git add README.md DHARA_DEEP_DIVE.md
git commit -m "docs(dhara): document CONFIG_DHARA_RADIX_DEPTH configuration option"
```

---

### Task 6 — Verification pass

**Goal:** Confirm nothing is broken at default config and that the optimization is measurable.

**Step 1: Build with default config (depth=32) — must be identical behavior**

```bash
idf.py fullclean && idf.py build
```
Expected: clean build, `DHARA_META_SIZE = 132`, `log2_ppc` unchanged from before.

**Step 2: Build with depth=15, verify `choose_ppc` gives `log2_ppc=4`**

Add a temporary `printf` or `ESP_LOGI` in `dhara_journal_init()` printing `log2_ppc`. Set `CONFIG_DHARA_RADIX_DEPTH=15`, rebuild, run.
Expected: `log2_ppc = 4` on a 2 KiB page device.

**Step 3: Test format-mismatch detection**

1. Format with depth=32.
2. Rebuild firmware with depth=15.
3. Call `dhara_map_resume()`.
Expected: returns `-1`, error is set (not silent success with garbage data).

**Step 4: Test legacy image handling**

1. Flash an image written by old Dhara (no radix_depth in header, `buf[16] = 0xFF`).
2. Rebuild with depth=32 (default).
3. Call `dhara_map_resume()`.
Expected: succeeds (legacy compatibility path).

**Step 5: Run upstream test suite**

```bash
make -C /Users/martinhavlik/personal/dhara tests && ./tests/run_all.sh
```
Expected: all tests pass with both depth=32 and depth=15.

---

## Summary of Changes

| File | Change type | Description |
|------|-------------|-------------|
| `Kconfig` | Create | `CONFIG_DHARA_RADIX_DEPTH` with help text |
| `dhara/dhara/journal.h` | Modify | Move `DHARA_RADIX_DEPTH` define here; derive `DHARA_META_SIZE`; extend `DHARA_HEADER_SIZE` to 20 |
| `dhara/dhara/journal.c` | Modify | Add `hdr_get/set_radix_depth()`; write depth on checkpoint; verify depth on resume |
| `dhara/dhara/map.h` | Modify | Add `DHARA_RADIX_DEPTH_FOR()` macro |
| `dhara/dhara/map.c` | Modify | Remove local `DHARA_RADIX_DEPTH` define; add geometry assertion in `dhara_map_init()` |
| `README.md` | Modify | Configuration section |
| `DHARA_DEEP_DIVE.md` | Modify | Mark §12.7 as implemented |

**Total new code: ~40 lines. Total modified lines: ~15. Zero logic changes to the core algorithms.**
