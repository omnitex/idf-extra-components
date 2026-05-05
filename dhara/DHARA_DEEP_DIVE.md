# Dhara NAND FTL — Deep-Dive Technical Analysis

> **Author of analysis:** synthesized from source code, design documents, upstream issues, and academic literature, April 2026.  
> **Dhara upstream:** https://github.com/dlbeer/dhara — Daniel Beer, ISC licence, ~2 026 lines of C.

---

## Table of Contents

1. [What is Dhara?](#1-what-is-dhara)
2. [Architecture Overview](#2-architecture-overview)
3. [Data Structures](#3-data-structures)
4. [On-Flash Layout](#4-on-flash-layout)
5. [Core Algorithms](#5-core-algorithms)
   - 5.1 [Journal enqueue / checkpoint](#51-journal-enqueue--checkpoint)
   - 5.2 [Radix-tree map (alt-pointer queue)](#52-radix-tree-map-alt-pointer-queue)
   - 5.3 [Garbage collection (repack + dequeue)](#53-garbage-collection-repack--dequeue)
   - 5.4 [Wear levelling](#54-wear-levelling)
   - 5.5 [Bad-block handling & recovery](#55-bad-block-handling--recovery)
   - 5.6 [Startup / resume scan](#56-startup--resume-scan)
6. [Power-Loss Safety & Atomicity](#6-power-loss-safety--atomicity)
7. [Performance Analysis](#7-performance-analysis)
8. [Memory Overhead](#8-memory-overhead)
9. [Capacity Model](#9-capacity-model)
10. [Pros](#10-pros)
11. [Cons & Known Weaknesses](#11-cons--known-weaknesses)
12. [Identified Weak Points — Improvement Opportunities](#12-identified-weak-points--improvement-opportunities)
    - 12.1 [Post-checkpoint orphan pages (the "between-checkpoint" gap)](#121-post-checkpoint-orphan-pages-the-between-checkpoint-gap)
    - 12.2 [Metadata I/O dominates read cost](#122-metadata-io-dominates-read-cost)
    - 12.3 [No use of OOB — wasted opportunity](#123-no-use-of-oob--wasted-opportunity)
    - 12.4 [Static gc_ratio cannot adapt](#124-static-gc_ratio-cannot-adapt)
    - 12.5 [Single-page-buffer design limits concurrency](#125-single-page-buffer-design-limits-concurrency)
    - 12.6 [Checkpoint period is fixed at init, not tunable at runtime](#126-checkpoint-period-is-fixed-at-init-not-tunable-at-runtime)
    - 12.7 [32-bit sector address space wastes alt-pointer storage](#127-32-bit-sector-address-space-wastes-alt-pointer-storage)
    - 12.8 [Trim is expensive (copy + pointer surgery)](#128-trim-is-expensive-copy--pointer-surgery)
    - 12.9 [No ordered/transactional multi-sector writes](#129-no-ordered--transactional-multi-sector-writes)
    - 12.10 [bb_last estimate can be wildly wrong](#1210-bb_last-estimate-can-be-wildly-wrong)
    - 12.11 [Recovery is O(block_size) reads per bad-block event](#1211-recovery-is-oblocksize-reads-per-bad-block-event)
    - 12.12 [No wear-aware GC — oldest-first is not always best](#1212-no-wear-aware-gc--oldest-first-is-not-always-best)
13. [Comparison with Alternatives](#13-comparison-with-alternatives)
14. [References](#14-references)

---

## 1. What is Dhara?

Dhara is a **flash translation layer (FTL)** for raw NAND flash aimed at microcontrollers. It exposes a simple **logical sector → physical page** mapping (a mutable block device) on top of a NAND abstraction layer you provide.

**What it is NOT:**
- Not a filesystem. It does not implement directories, files, or POSIX semantics.
- Not NOR-flash specific (unlike SPIFFS or LittleFS without an FTL).
- Not an OS driver. It is a pure-C algorithm library.

**Published guarantees (from README/map_internals.txt):**

| Property | Claim |
|---|---|
| Wear levelling | *Perfect* — erase counts of any two blocks differ by at most 1 |
| Atomicity | `write()` and `trim()` of logical sectors are atomic |
| Durability | Changes are persistent once `sync()` returns successfully |
| Startup time | O(log N) in number of pages |
| All operations | O(log N) worst-case in chip size |
| ECC | Fully delegated to NAND layer; no OOB bytes consumed |
| Bad blocks | Handled transparently; no BBT in RAM required |

---

## 2. Architecture Overview

```
  ┌──────────────────────────────────────────────────────────┐
  │                    Your application                       │
  └───────────────────┬──────────────────────────────────────┘
                      │  dhara_map_read / write / trim / sync
  ┌───────────────────▼──────────────────────────────────────┐
  │   dhara_map   (map.c / map.h)                            │
  │   • Functional radix tree (alt-pointer queue)            │
  │   • GC ratio enforcement                                 │
  │   • Sector count (cookie)                                │
  └───────────────────┬──────────────────────────────────────┘
                      │  enqueue / dequeue / copy / peek
  ┌───────────────────▼──────────────────────────────────────┐
  │   dhara_journal   (journal.c / journal.h)                │
  │   • Log-structured write queue (circular over blocks)    │
  │   • Checkpoint groups (data pages + metadata page)       │
  │   • Bad-block skip & recovery state machine              │
  └───────────────────┬──────────────────────────────────────┘
                      │  is_bad / mark_bad / erase / prog / read / copy
  ┌───────────────────▼──────────────────────────────────────┐
  │   dhara_nand   (nand.h — YOU implement this)             │
  │   • Physical NAND driver                                 │
  │   • ECC, OOB layout, bad-block markers                   │
  └──────────────────────────────────────────────────────────┘
```

The design separates three concerns cleanly:

1. **Journal** — manages the ring-buffer-like write log over physical blocks, handles block erasure, bad blocks, and checkpoint persistence.
2. **Map** — implements the logical-to-physical mapping on top of the journal, using an immutable functional radix tree encoded as a sequence of "alt-pointer" records.
3. **NAND driver** — completely user-supplied; Dhara makes no assumptions about OOB layout, ECC scheme, or partial-page programming.

---

## 3. Data Structures

### `struct dhara_nand` (nand.h)

```c
struct dhara_nand {
    uint8_t  log2_page_size;  // e.g. 11 for 2048-byte pages
    uint8_t  log2_ppb;        // e.g. 6 for 64 pages/block
    unsigned int num_blocks;  // total erase-blocks on chip
};
```

This is the chip geometry descriptor. No pointers to driver functions — those are free-standing functions you implement (`dhara_nand_prog`, `dhara_nand_read`, etc.).

### `struct dhara_journal` (journal.h)

```c
struct dhara_journal {
    const struct dhara_nand  *nand;
    uint8_t                  *page_buf;     // ONE page buffer (all I/O goes here)

    uint8_t   log2_ppc;     // log2(pages per checkpoint group)
    uint8_t   epoch;        // wraps around counter
    uint8_t   flags;        // dirty, bad_meta, recovery, enum_done

    dhara_block_t  bb_current;  // bad blocks before current head
    dhara_block_t  bb_last;     // best estimate of total bad blocks

    dhara_page_t   tail_sync;   // tail at last checkpoint (durable)
    dhara_page_t   tail;        // current logical tail (in-RAM)
    dhara_page_t   head;        // next writable user page
    dhara_page_t   root;        // most recently written user page

    // Recovery mode fields:
    dhara_page_t   recover_next;
    dhara_page_t   recover_root;
    dhara_page_t   recover_meta;
};
```

**Key insight:** `tail` vs `tail_sync`. The tail can advance (via `dequeue`) in RAM before a checkpoint. Space is only actually freed to the write head after the next checkpoint commits the new tail. This is the core mechanism for atomicity.

### `struct dhara_map` (map.h)

```c
struct dhara_map {
    struct dhara_journal  journal;
    uint8_t               gc_ratio;   // auto-GC aggressiveness
    dhara_sector_t        count;      // number of live logical sectors
};
```

Tiny. All the real state lives in `dhara_journal`.

### Metadata record (per written user page) — `DHARA_META_SIZE = 132 bytes`

```
 0       3: id        — 32-bit logical sector number (DHARA_SECTOR_NONE = 0xffffffff for filler)
 4     131: alt[0..31] — 32 × 4-byte alt-pointers (physical page numbers)
```

32 alt-pointers = 32 levels of the radix tree = 32-bit sector addresses. This is why `DHARA_META_SIZE = 4 + 32×4 = 132`.

### Checkpoint page layout (stored in the last page of each checkpoint group)

```
 0    15: Header     (16 bytes)  — magic "Dha", epoch, tail, bb_current, bb_last
16    19: Cookie     (4 bytes)   — map sector count (from higher layer)
20   end: Metadata[] — (2^log2_ppc − 1) × 132 bytes, one per user page in group
```

The page size must be large enough to hold all of this. For a 2048-byte page with `log2_ppc = 3` (8-page groups = 7 user pages + 1 checkpoint):

```
Required: 16 + 4 + 7×132 = 944 bytes  ≤ 2048 ✓
```

`choose_ppc()` automatically picks the maximum `log2_ppc` that fits.

---

## 4. On-Flash Layout

### Block layout (example: log2_ppc=2, log2_ppb=4)

```
Block N:
  Page 0  [user data]
  Page 1  [user data]
  Page 2  [user data]
  Page 3  [checkpoint: header + cookie + meta for pages 0,1,2]

  Page 4  [user data]
  Page 5  [user data]
  Page 6  [user data]
  Page 7  [checkpoint: header + cookie + meta for pages 4,5,6]

  Page 8  [user data]
  ...
```

Each **checkpoint group** is `2^log2_ppc` pages wide. The last page of the group is the checkpoint (metadata) page. The preceding `2^log2_ppc − 1` pages are user data pages.

### Journal as a ring

The journal advances `head` forward through blocks in page-number order. When `head` reaches the end of the chip, it wraps to 0 and increments `epoch`. The `tail` (oldest live data) follows behind. The invariant is: **head never catches tail_sync within the same block**.

```
  Block 0   Block 1   Block 2   Block 3   Block 4
  [tail]───────────────────────────────[head]────►
   oldest data                          newest data
```

On wrap-around, the epoch byte in the checkpoint header is the distinguishing mechanism during resume to tell new data from stale old data.

---

## 5. Core Algorithms

### 5.1 Journal enqueue / checkpoint

```
dhara_journal_enqueue(data, meta):
    1. prepare_head()  → erase block if block-aligned & good; skip bad blocks
    2. dhara_nand_prog(head, data)
    3. push_meta(meta):
         a. copy meta into page_buf at the correct slot offset
         b. if NOT last slot in group: root = head; head++; return (no I/O)
         c. if LAST slot: write checkpoint page (page_buf) to head+1
            → sets dirty=false, advances head, updates tail_sync
```

The checkpoint is **lazy**: metadata accumulates in a single RAM page buffer and is flushed to NAND only when the checkpoint group is full. Between flushes, data pages are already committed but metadata is only in RAM — the "orphan pages" window.

### 5.2 Radix-tree map (alt-pointer queue)

The mapping is a **functional radix tree** encoded as a sequence of update records, each appended to the journal. Each record contains:
- `id`: the 32-bit logical sector number
- `alt[0..31]`: for level `k`, `alt[k]` points to the most-recent prior record whose sector address shares its first `k` bits with this record's sector address

**Lookup** (`trace_path`):

```c
p = root;  // most recently written user page
for depth = 0..31:
    read meta(p) → id, alt[]
    if bit[depth] of (target XOR id) != 0:
        // must follow the alt-pointer at this level
        p = alt[depth]
        if p == NONE: return NOT_FOUND
    // else: stay on current record, copy alt[depth] to new_meta
return p  // physical page holding the logical sector's data
```

This is O(32) = O(1) hops in the worst case *for a fixed 32-bit address space*. But each hop is a metadata read — a full or partial NAND page read (see §12.2).

**Update** (write): `prepare_write` calls `trace_path` with `new_meta` output enabled, building the alt-pointer array for the new record. Then it enqueues `(data, new_meta)`.

### 5.3 Garbage collection (repack + dequeue)

```
raw_gc(src):
    read meta(src) → target sector id
    if id == SECTOR_NONE: dequeue (it's filler/deleted)
    find current physical page of target via trace_path
    if current != src: dequeue (obsolete copy)
    else: copy(src → head, updated meta)  // "repack"
```

Called automatically in `auto_gc()` before each write when `journal_size >= map_capacity`.

**GC ratio** controls the balance between GC work and user writes:
- `gc_ratio = 1`: for every write that triggers GC, do 2 GC steps (aggressively reclaim)
- `gc_ratio = N`: do N+1 GC steps, freeing N+1 old pages, then write 1 new page

**Sync** (`dhara_map_sync`): drains the queue from the tail, repacking live pages and discarding garbage, until the journal reaches a checkpoint.

### 5.4 Wear levelling

Because the journal is a **strict ring** (head always advances; erased blocks are reused in order), every block is erased approximately equally often. The claim "erase counts differ by at most 1" is correct under the assumption that:
- Bad blocks are uniformly distributed
- The chip is always exercised in ring order (no preferential treatment of some blocks)

This is **static wear levelling** — cold data is eventually forced to move by GC as the ring advances. There is no explicit "cold data migration" beyond what GC naturally provides.

### 5.5 Bad-block handling & recovery

**Initial skip:** `prepare_head()` checks `is_bad()` and skips bad blocks before each write, incrementing `bb_current`.

**In-block failure (mid-block write fails):** This is the complex case. The journal must rescue pages already written to the failing block:

```
recover_from():
    1. Record recover_root = last known good root (= last checkpoint in bad block)
    2. If buffered metadata exists in RAM, dump it to next good block
    3. Set RECOVERY flag; return E_RECOVER to map layer

try_recover() loop (in map.c):
    while in_recovery:
        p = next_recoverable()   // enumerate user pages in the bad block
        if p == NONE: pad_queue()
        else: raw_gc(p)          // repack live pages to new location
    // when journal reaches checkpoint, finish_recovery() marks block bad
```

This is a stateless recovery: it can be restarted if a second bad block occurs mid-recovery. Up to `DHARA_MAX_RETRIES = 8` nested bad blocks are tolerated.

### 5.6 Startup / resume scan

```
dhara_journal_resume():
    1. find_checkblock(0)        — scan forward to find first block with "Dha" magic
    2. read epoch from checkpoint
    3. find_last_checkblock()    — binary search for last block with same epoch
    4. find_last_group()         — binary search within that block for last programmed group
    5. find_root()               — linear scan within group for last valid checkpoint
    6. find_head()               — linear scan from root forward to next free page
```

The binary searches exploit the **monotonic write order** of the ring: all checkpoints in the current epoch appear before all checkpoints of other epochs, and within an epoch, checkpoint groups are written in ascending address order. This gives **O(log N)** reads at startup.

---

## 6. Power-Loss Safety & Atomicity

### What is durable?

**Durable = at the last checkpoint.** A checkpoint is written when a checkpoint group (of `2^log2_ppc − 1` user pages) is full. Data written between two checkpoints exists on NAND but is **not covered by any valid checkpoint page** — it is "orphaned".

On power loss between checkpoints:
- User data pages **are physically written** to NAND (NAND writes are atomic at the page level via the NAND program algorithm, assuming hardware completes)
- But the **metadata page** for that group was never written
- On resume, `find_root()` stops at the last *valid* checkpoint
- Pages after that checkpoint are simply **abandoned** — they remain on flash but are unreachable and become garbage for the next erase cycle

**From the user's perspective:** writes appear atomic at the granularity of a `sync()` call. `write()` alone does not guarantee durability; `sync()` does.

### Checkpoint period and durability window

With `log2_ppc = 3` (8-page groups, 7 user writes between checkpoints):

- **Worst-case data loss:** up to 7 sector writes since last checkpoint
- **In bytes:** up to 7 × page_size bytes (e.g. 7 × 2048 = 14 336 bytes)

Calling `sync()` explicitly after critical writes reduces this to zero, at the cost of I/O overhead.

### What about partial page writes?

Dhara passes ECC entirely to the NAND driver. The assumption is that `dhara_nand_prog()` either:
- Completes successfully (page is valid)
- Fails and returns `E_BAD_BLOCK`

There is **no intermediate state** handling within Dhara. If the NAND program operation is interrupted by power loss *during* the write, the driver's `is_free()` and `read()` with ECC error detection are expected to identify and report the corrupted page appropriately.

### Summary table

| Event | Effect |
|---|---|
| Power loss during a user write (prog) | Lost: that write. Map rolls back to last checkpoint. |
| Power loss during checkpoint page write | Same as above — checkpoint is not valid unless written completely. |
| Power loss during block erase | That block is skipped/bad-flagged on next use. |
| Power loss during bad-block recovery | Recovery is stateless; resumes from scratch on next mount. |
| `sync()` completes successfully | All prior writes are durable. |

---

## 7. Performance Analysis

### Theoretical complexity

| Operation | Complexity | Comment |
|---|---|---|
| `write(sector, data)` | O(log N) amortized | log N metadata reads (trace_path) + 1 prog + possible GC |
| `read(sector, data)` | O(log N) | log N metadata reads + 1 data read |
| `trim(sector)` | O(log N) | Same as write + cousin rewrite |
| `sync()` | O(k × log N) | k = pages to drain until checkpoint |
| `resume()` | O(log N) | Binary search + linear scan within last block |
| GC step | O(log N) | One trace_path + one copy |

N = total pages on chip. For a 128 MiB chip with 2048-byte pages: N = 65 536. log₂(N) = 16 hops.

### Practical bottleneck: metadata reads

Each hop in `trace_path` is a `dhara_nand_read()` call. With `DHARA_META_SIZE = 132` bytes, and assuming partial-read is supported, this is 132 bytes of payload. However:

- Many NAND drivers and controllers do not support sub-page reads; they read an entire page (e.g., 2048 bytes) even for 132 bytes.
- Even with sub-page reads, each transaction has fixed command + address overhead (~hundreds of nanoseconds).
- A write of one sector therefore causes **up to 32 metadata reads** (one per radix tree level), plus the actual data write.

**Measured example (from upstream issue #21):** on a system where NAND transactions dominate, metadata reads were responsible for >90% of write latency. The author's suggestion was driver-level caching.

### Write amplification

Every user write generates exactly one physical page write (data) plus one slot in a checkpoint page. Checkpoint pages themselves are written every `2^log2_ppc − 1` user pages — so the write amplification from checkpointing is:

```
WA_checkpoint = (2^log2_ppc) / (2^log2_ppc − 1) ≈ 1 + 1/7  ≈ 1.14  (for ppc=3)
```

GC adds further amplification. In the steady state with gc_ratio = 1, for every user write there are up to 1 GC copies. Real WA depends on fragmentation, but in the worst case (all sectors live):

```
WA_gc_worst = gc_ratio + 1  (ratio of GC writes to user writes)
```

Total WA ≈ `(gc_ratio + 1) × (1 + 1/(2^log2_ppc − 1))` ≈ 2.3× for gc_ratio=1, ppc=3.

---

## 8. Memory Overhead

### RAM (at runtime)

| Item | Size | Notes |
|---|---|---|
| `struct dhara_journal` | ~60 bytes | Fixed fields, no dynamic alloc |
| `struct dhara_map` | ~68 bytes | Includes journal |
| `page_buf` | 1 × page_size | Allocated by caller, used for checkpoint I/O and all reads/writes |
| Stack in `trace_path` | `DHARA_META_SIZE = 132` bytes | One meta buffer on stack per call |
| Stack in `raw_gc` | `DHARA_META_SIZE × 2 = 264` bytes | Two meta buffers |
| Stack in `try_delete` | `DHARA_META_SIZE × 2 = 264` bytes | Two meta buffers |

**Total RAM:** `~68 + page_size + ~300 bytes stack` at peak. For a 2048-byte page: **~2.5 KiB**.

This is remarkably small. No in-RAM bad-block table, no mapping cache, no separate metadata buffer.

### Flash overhead

Checkpoints consume `1/(2^log2_ppc)` of flash capacity. For ppc=3: 12.5% of raw capacity is used for checkpoints.

Plus the GC reserve: `1/(gc_ratio + 1)` of usable capacity. For gc_ratio=1: 50% reserve.

Total usable fraction of raw NAND:
```
usable = (1 − 1/2^ppc) × (gc_ratio/(gc_ratio+1)) − safety_margin
       ≈ 0.875 × 0.5 ≈ 43.75%   (ppc=3, gc_ratio=1)
```

For gc_ratio=4: `0.875 × 0.8 ≈ 70%` usable — but GC is less frequent and burst write latency increases.

---

## 9. Capacity Model

From `dhara_map_capacity()`:

```c
dhara_sector_t dhara_map_capacity(const struct dhara_map *m)
{
    cap     = dhara_journal_capacity();          // good pages
    reserve = cap / (gc_ratio + 1);             // GC headroom
    safety  = DHARA_MAX_RETRIES << log2_ppb;    // 8 blocks for recovery
    return cap - reserve - safety;
}
```

`dhara_journal_capacity()` itself accounts for bad blocks using `bb_last` (the running maximum of observed bad blocks). This can be **pessimistic** early in the device life (see §12.10).

---

## 10. Pros

1. **Tiny footprint.** ~2 KiB RAM + 1 page buffer. One of the smallest NAND FTLs available.
2. **Genuinely O(log N) all operations.** Unlike many embedded FTLs that have O(1) amortized but unbounded worst-case (e.g., JFFS2 mount).
3. **Perfect static wear levelling.** The ring structure guarantees equal erase counts across all blocks. No hot-spot accumulation.
4. **Clean layering.** The NAND driver contract is minimal (7 functions). ECC and OOB are entirely in the driver's domain.
5. **Robust bad-block handling.** In-block failures are transparently recovered with a stateless algorithm. No external BBT needed.
6. **Portable.** Pure C99, no libc beyond `<string.h>`, no RTOS dependencies.
7. **Well-reasoned design.** The alt-pointer queue concept is a novel and elegant encoding of a functional radix tree with constant allocation per update.
8. **Proven.** Adopted by Zephyr RTOS (2025–2026), deployed in commercial products.
9. **Trim support.** Allows filesystems to reclaim deleted sectors efficiently.
10. **No external bad-block table needed.** Bad-block markers are read from OOB by the driver; Dhara only counts them.

---

## 11. Cons & Known Weaknesses

### 11.1 The "orphan pages" problem (durability gap)

Between checkpoints, user data pages are **physically written** but **not mapped** in any durable checkpoint. After a power failure, these writes are silently discarded on resume. The user must call `sync()` to ensure durability, but the API does not enforce this, and the latency of `sync()` is unbounded in the worst case (must drain all dirty pages).

This is the **most significant correctness concern** for users who do not read the documentation carefully.

### 11.2 No thread safety

Dhara has a single `page_buf`, single journal state, no locking. Concurrent access from multiple threads or interrupt contexts will corrupt the state. The user must serialize all operations externally.

### 11.3 Metadata read amplification

Every `read()` or `write()` requires up to 32 sequential metadata page reads through the radix tree. On NAND chips where each read transaction has high overhead (command + address bytes, bus latency), this dominates total latency. There is no metadata cache.

### 11.4 No read-back verification

After `dhara_nand_prog()`, Dhara does not perform a read-back to verify the written data. ECC is delegated to the driver, but if the driver's ECC is too weak for the error rate at end of life, Dhara cannot detect the corruption.

### 11.5 gc_ratio is static

The GC ratio is set at init and never changes. It cannot adapt to wear state, workload patterns, or urgent reclamation needs. Very small `gc_ratio` (e.g., 1) is safe but conservative in capacity; large `gc_ratio` risks bursty GC pauses.

### 11.6 Not actively maintained

Last upstream commit: March 2022. The project is stable but may not receive bug fixes or improvements for newer NAND devices.

### 11.7 Capacity is pessimistic early in life

`bb_last` is a running maximum of observed bad blocks. Before the first wrap-around (first full scan of the device), the estimate defaults to `num_blocks >> 6` (1.56%). If the chip has fewer bad blocks, reported capacity is artificially reduced.

### 11.8 Checkpoint period is not tunable at runtime

`log2_ppc` is computed once at `init` time from page size and maximum ppb, and cannot be changed without reformatting. A smaller checkpoint period (more frequent checkpoints) reduces the orphan window but increases write amplification.

---

## 12. Identified Weak Points — Improvement Opportunities

These are concrete weaknesses where Dhara can be improved — even if doing so requires breaking the existing API, removing the "no OOB" restriction, or restructuring on-flash layout.

---

### 12.1 Post-checkpoint orphan pages (the "between-checkpoint" gap)

**Root cause:** Dhara's checkpoint page is written only when a full checkpoint group is programmed. User pages between the last checkpoint and the write head are durably on NAND but not covered by any checkpoint metadata. They are abandoned on resume.

**Impact:** Up to `(2^log2_ppc − 1) × page_size` bytes of writes can be silently lost after a power failure without calling `sync()`.

**Improvement option A — OOB LPN replay (described in `dhara_oob_metadata_replay_plan.md`):**

Write the logical sector number (LPN) into OOB for every user page at program time. On resume, after finding the last valid checkpoint, scan forward through any user pages in the current (incomplete) checkpoint group. For each page, read its LPN from OOB, reconstruct the metadata slice (same radix-tree update logic), and patch it into the RAM checkpoint buffer. The map then reflects all durably-written pages, not just those covered by a full checkpoint.

```
resume():
    1. normal journal resume → finds last checkpoint → loads metadata buffer
    2. extend_head := scan forward for any programmed user pages beyond last root
    3. for each such page p in order:
         lpn = oob_read_lpn(p)
         recompute meta slice for (p, lpn) using same trace_path logic
         patch page_buf[slot(p)] := meta
    4. update count cookie
```

This requires:
- A small OOB layout extension (e.g., 4 bytes for LPN + 1 byte magic/validity)
- A new `dhara_nand_read_oob(p)` and `dhara_nand_prog_with_lpn(p, data, lpn)` driver contract
- Replay logic in `dhara_journal_resume()` or `dhara_map_resume()`

**Effect:** Effectively zero data loss on power failure, even without explicit `sync()`. This is the most impactful single improvement possible to Dhara.

**Improvement option B — Smaller checkpoint period:**

Reduce the default `log2_ppc` to 1 (2-page groups: 1 user + 1 checkpoint). Every user write is immediately followed by a checkpoint. Near-zero durability window, but write amplification doubles (50% of flash used for checkpoints).

---

### 12.2 Metadata I/O dominates read cost

**Root cause:** `trace_path()` performs up to 32 `dhara_nand_read()` calls (one per radix level). Each call is an independent NAND transaction. On SPI NAND (common in ESP32 designs), each transaction is ~10–50 µs. 32 hops = 320–1600 µs per write *just for metadata*, before the actual data write.

**Why 32 hops in practice?** In a cold journal (few writes), most pages will have many NULL alt-pointers, meaning the hop terminates early. But in a hot journal (many sectors mapped), all 32 levels may be traversed.

**Improvement option A — In-RAM metadata cache (LRU, small):**

Cache the 10–20 most recently read metadata records (132 bytes each = 1.3–2.6 KiB). Given the radix tree access pattern (always starting from `root` and descending via alt-pointers), recently accessed nodes are highly likely to be needed again. Upstream issue #21 reports 92% cache hit rate with just 10 entries.

Implementation: a simple direct-mapped or LRU array keyed by `dhara_page_t`, placed between `dhara_journal_read_meta()` and the NAND driver. This can be done entirely outside Dhara core by wrapping the journal struct.

**Improvement option B — Alt-pointer skip hints in OOB:**

At each hop, if the alt-pointer at level k is NULL (not needed to traverse further), record the number of NULL levels in OOB as a skip count. On lookup, read OOB first (single small read) to determine how many levels to skip before fetching full metadata. Reduces average metadata reads for sparse trees.

**Improvement option C — Wider checkpoint groups with metadata batching:**

If `log2_ppc` is larger, the checkpoint page can hold more metadata, but this doesn't help the lookup path since lookups still traverse alt-pointers one hop at a time.

---

### 12.3 No use of OOB — wasted opportunity

**Dhara's design decision:** "No OOB data is consumed." This is intended to keep ECC fully in the driver's hands. But it means:

- Every user page has no self-describing identity on flash
- After power loss, there is no way to know what logical sector a physical page contains without following the radix tree from the root
- OOB bytes are completely wasted from Dhara's perspective (the driver can use them for ECC, but nothing else)

**Opportunities if OOB is used:**

| OOB field | Benefit |
|---|---|
| LPN (4 bytes) | Enable orphan-page replay (§12.1) |
| Write generation counter (2 bytes) | Detect outdated copies without full trace_path |
| Checkpoint sequence number (2 bytes) | Faster resume: identify orphan pages by epoch/sequence |
| Physical erase count (4 bytes) | Enable wear-aware GC (§12.12) |
| Bad block mark (1 byte, standard) | Already used by driver; Dhara is unaware |

**Constraint:** OOB is limited (typically 64 bytes per 2KiB page). ECC may consume 28–32 bytes, leaving ~32 bytes. LPN + generation fits easily.

**Breaking change required:** `dhara_nand_prog` must accept an OOB buffer or LPN argument; `dhara_nand_read` must return OOB data.

---

### 12.4 Static gc_ratio cannot adapt

**Root cause:** `gc_ratio` is a compile/init-time constant. The map simply enforces `journal_size < map_capacity` before each write, triggering exactly `gc_ratio + 1` GC steps if needed.

**Problems:**
- Cannot respond to bursty writes that temporarily overfill capacity
- Cannot slow GC when journal is nearly empty (wasting erase cycles)
- Cannot prioritize blocks with higher erase counts (wear equalization could be improved)

**Improvement:** Dynamic GC scheduling.

```c
// Current approach:
if (journal_size >= map_capacity) do (gc_ratio+1) steps;

// Improved: proportional controller
int gc_pressure = (journal_size * 100) / map_capacity;  // 0..100
int steps = gc_pressure > 80 ? (gc_ratio + 2) :
            gc_pressure > 50 ? (gc_ratio + 1) :
            (gc_ratio);
```

Or: expose `gc_ratio` as a settable runtime parameter (`dhara_map_set_gc_ratio()`), allowing the application to dial it up before a big write burst and down during idle.

---

### 12.5 Single-page-buffer design limits concurrency

**Root cause:** `dhara_journal` uses a single `page_buf` for all I/O — checkpoint accumulation, reads for `trace_path`, and write buffers all share this one allocation. This means:

- No pipelining of reads while a write is in progress
- No concurrent map operations
- The buffer must be pinned during any journal operation

**Improvement:** Separate the checkpoint accumulation buffer from the scratch-read buffer. This requires two page-sized buffers instead of one, but enables:
- Non-blocking reads (separate buffer not disturbed by in-progress checkpoint)
- Future pipelined writes (though NAND hardware would also need to support this)

This is a moderate breaking change to the `dhara_journal_init()` API.

---

### 12.6 Checkpoint period is fixed at init, not tunable at runtime

**Root cause:** `log2_ppc` is computed once in `dhara_journal_init()` by `choose_ppc()`. It depends only on `page_size`. There is no mechanism to use a smaller `log2_ppc` for more frequent checkpoints.

**Impact:** For applications that need stronger durability guarantees, the only option is to call `sync()` frequently, which drains the entire dirty queue (potentially many GC steps).

**Improvement:** Add a configurable `max_log2_ppc` parameter to `dhara_journal_init()`. Setting it to 0 forces a checkpoint after every single user page (effectively making every write immediately durable, at heavy WA cost). The page buffer size must accommodate `(2^max_log2_ppc − 1) × 132 + 20` bytes.

---

### 12.7 32-bit sector address space wastes alt-pointer storage

**Root cause:** `DHARA_RADIX_DEPTH = 32` is hardcoded. Each metadata record holds 32 alt-pointers × 4 bytes = 128 bytes, regardless of how many sectors the device actually needs to address.

**Example:** A 128 MiB NAND with 2 KiB pages has 65 536 pages. Dhara's max addressable sectors ≈ 65 536 / 2 ≈ 32 768. This requires only 15 bits to address, yet Dhara stores 32 alt-pointers.

**Wasted space:** 17 × 4 = 68 bytes per metadata record are always NULL alt-pointers. With `log2_ppc = 3`, a checkpoint page holds 7 metadata records × 68 bytes = 476 wasted bytes per checkpoint group.

**Improvement:** Make `DHARA_RADIX_DEPTH` a runtime parameter or compute it from `log2_page_size + log2_ppb + log2(num_blocks)`. Shorten metadata records accordingly. This reduces checkpoint overhead, increases effective `log2_ppc`, and saves RAM in `trace_path` stack frames.

This is a significant breaking change to the on-flash format.

---

### 12.8 Trim is expensive (copy + pointer surgery)

**Root cause:** `dhara_map_trim()` calls `try_delete()`, which:
1. Calls `trace_path()` to find the victim sector (O(32) metadata reads)
2. Identifies the "cousin" node at the delete-root level
3. Reads the cousin's metadata
4. Synthesizes a new metadata record that nulls out the pointer to the victim
5. Calls `dhara_journal_copy()` to rewrite the cousin at the head

This is at minimum 2 metadata reads + 1 data copy operation. On a large tree, it can be 33 metadata reads.

**Additionally:** deleted sectors don't free space until GC processes the obsolete page. Trim just marks the sector logically absent; the physical page persists until evicted from the tail.

**Improvement:** Batch trim hints. Instead of immediately rewriting the cousin, accumulate trim requests in a small in-RAM set. On the next GC cycle, apply the trims to the cousin as it is repacked. This amortizes the cost of trim operations over GC, which is already touching those pages.

---

### 12.9 No ordered / transactional multi-sector writes

**Root cause:** Dhara writes sectors individually. There is no way to atomically commit a group of sectors (e.g., "write sectors 5, 6, 7 atomically"). Each `write()` is individually atomic, but there is no cross-sector transaction.

**Impact:** Filesystems layered on Dhara (e.g., LittleFS or FAT) must implement their own journaling or copy-on-write for multi-sector updates. This means two layers of journaling overhead.

**Improvement option:** A lightweight "transaction begin/commit" API:

```c
dhara_map_txn_begin(m);      // snapshot current root
dhara_map_write(m, s1, d1);
dhara_map_write(m, s2, d2);
dhara_map_txn_commit(m, err); // sync to checkpoint; on failure, roll back to snapshot
```

Roll-back would be trivial: just reset the journal's in-RAM root and head to the snapshot. Data pages written during the failed transaction are abandoned (become GC fodder), which Dhara already handles.

---

### 12.10 bb_last estimate can be wildly wrong

**Root cause:** `bb_last` is initialized to `num_blocks >> 6` (1/64 of total blocks ≈ 1.5%) and updated to `max(bb_last, bb_current)` once per epoch (ring traversal). Early in device life, before the first full wrap-around, this estimate may be far from reality.

```c
j->bb_last = j->nand->num_blocks >> 6;  // conservative guess
```

**Impact:** `dhara_journal_capacity()` subtracts `max(bb_last, bb_current)` from total blocks. If the chip has only 0.1% bad blocks but `bb_last` estimates 1.5%, the reported capacity is ~1.4% lower than actual. For a 1 GiB chip with 1024 blocks, this is ~14 phantom "reserved" blocks = ~14 MiB of capacity loss.

**More seriously:** If the chip develops more bad blocks than `bb_last`, the head could overlap the tail without being detected, leading to data loss.

**Improvement:** Scan the entire chip for bad blocks at first-time format, storing the true count durably. Add a `dhara_nand_scan_bad_blocks()` call in `dhara_map_init()` when initializing a fresh device.

---

### 12.11 Recovery is O(block_size) reads per bad-block event

**Root cause:** When a block goes bad mid-write, Dhara must enumerate all user pages in the bad block and repack live ones. The number of user pages per block is `(2^log2_ppb) × (1 − 1/2^log2_ppc)`. For log2_ppb=6, log2_ppc=3: `64 × 7/8 = 56` user pages to enumerate.

For each of those 56 pages: one metadata read (to find the sector ID) + one `trace_path` (O(32) reads) to determine if it's still live = up to 56 × 33 = 1 848 NAND reads per bad-block event.

**Improvement:** Use OOB LPN (§12.3) to skip the metadata read portion of recovery. If LPN is in OOB, recovery needs only 56 OOB reads + at most 56 `trace_path` calls. For sparse journals, many pages will be garbage and `trace_path` can early-exit.

---

### 12.12 No wear-aware GC — oldest-first is not always best

**Root cause:** `raw_gc()` always processes the tail (oldest page). This is correct for wear levelling (erases blocks in round-robin order), but it is not optimal for:
- **Hot/cold data separation:** frequently rewritten sectors stay near the head; cold sectors drift to the tail and get unnecessarily repacked.
- **Erase count balancing across bad-block gaps:** if several bad blocks cluster together, one region of the ring gets erased less frequently than others.

**Improvement option A:** Track per-block erase counts in OOB (4 bytes per block). In GC, prefer to repack pages from the block with the lowest erase count — classic static wear levelling migration for cold data.

**Improvement option B:** Segment the journal into "hot" and "cold" regions. New writes go to the hot region. GC from cold is cheaper (cold data is rarely rewritten). This is similar to what modern SSDs call "multi-stream."

Both options significantly increase complexity. Option A via OOB is feasible within Dhara's architecture. Option B likely requires a more fundamental restructuring.

---

## 13. Comparison with Alternatives

| | **Dhara** | **LittleFS** | **YAFFS2** | **JFFS2** |
|---|---|---|---|---|
| Target hardware | Raw NAND | NOR / NAND | Raw NAND | Raw NAND / NOR |
| Abstraction level | FTL (block device) | Filesystem | Filesystem | Filesystem |
| RAM usage | ~2.5 KiB | ~4–8 KiB | ~8–16 KiB (in-RAM index) | ~100s KiB (node index) |
| Wear levelling | Perfect static | Dynamic (approx) | Log-structured (approx) | Log-structured (approx) |
| Power-loss safety | Checkpoint-based | COW per block | Good | Good (scan-based) |
| Startup time | O(log N) | O(1) | O(N) scan | O(N) scan |
| OOB use | None | None (driver) | Uses OOB (tags) | Uses OOB |
| GC overhead | Moderate | Low | Low | High (fragmentation) |
| Maintained? | Last: 2022 | Active | Active | Active (kernel) |

LittleFS + Dhara as a stack (LittleFS on top of the block device Dhara presents) is a common ESP-IDF pattern and is arguably the best option for most applications: LittleFS provides COW filesystem safety; Dhara provides NAND wear levelling and bad-block management.

---

## 14. References

1. Daniel Beer, *Dhara internals* — `map_internals.txt` in source tree
2. Daniel Beer, *Dhara README* — upstream https://github.com/dlbeer/dhara
3. Upstream issue #21 — performance/metadata caching: https://github.com/dlbeer/dhara/issues/21
4. Upstream issue #30 — thread safety: https://github.com/dlbeer/dhara/issues/30
5. Zephyr integration PR #100858: https://github.com/zephyrproject-rtos/zephyr/pull/100858
6. `dhara_oob_metadata_replay_plan.md` — OOB LPN replay design in this repo
7. LittleFS NAND discussion: https://github.com/littlefs-project/littlefs/issues/11
8. Superblock FTL (OOB mapping): https://dl.acm.org/doi/10.1145/1721695.1721706
9. 3DFTL (OOB mapping table): https://www.cp.eng.chula.ac.th/~prabhas/paper/2015/3DFTL-ELEX%206.pdf
10. LazyFTL (update buffer FTL): https://dbgroup.cs.tsinghua.edu.cn/ligl/papers/sigmod2011-lazyftl.pdf
