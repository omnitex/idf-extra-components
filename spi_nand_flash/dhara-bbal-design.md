# Dhara Bad Block Abstraction Layer (BBAL) — Design Plan

**Date:** 2026-03-18  
**Repo:** `spi_nand_flash`  
**Branch:** `feature/dhara-bbal`

---

## 1. Problem Statement

Dhara's journal and radix-tree map use raw physical block/page numbers internally. Because bad blocks occupy different physical locations on every chip, the logical sector space (capacity, page addresses stored in metadata) varies between devices. This makes it impossible to migrate logical content from one chip to another without re-writing every sector.

The goal is to insert a **Bad Block Abstraction Layer (BBAL)** between the raw NAND HAL and Dhara that presents a contiguous, bad-block-free block address space `0..N-1` to Dhara, where `N = total_blocks - bad_block_count`. Any two chips with the same total block count and same `N` will appear identical to Dhara, enabling logical content migration.

---

## 2. Scope

| In scope | Out of scope |
|----------|-------------|
| BBAL shim (HAL wrapper) | Modifying upstream `dhara/` submodule |
| Startup OOB scan to build remapping table | A dedicated BBT block / reserved block scheme |
| 7 HAL callback wrappers | ECC implementation (stays in user HAL) |
| Logical content migration utility | Raw bit-copy between chips with different bad blocks |
| Unit test with two sim NAND instances | Multi-chip / interleaved NAND |

---

## 3. Architecture

```
 Application
     │
     ▼
┌─────────────────────────────────────────────────┐
│  dhara_map  (unchanged upstream)                │
└───────────────┬─────────────────────────────────┘
                │ calls dhara_nand_* callbacks
┌───────────────▼─────────────────────────────────┐
│  BBAL  (new)                                    │
│  Translates logical block/page → physical       │
│  Presents num_blocks = total - num_bad to Dhara │
└───────────────┬─────────────────────────────────┘
                │ calls real_* HAL functions
┌───────────────▼─────────────────────────────────┐
│  User NAND HAL  (existing, unchanged)           │
│  Talks to actual SPI NAND hardware              │
└─────────────────────────────────────────────────┘
```

---

## 4. File Layout

```
spi_nand_flash/
├── include/
│   └── dhara_bbal.h          # public API
├── src/
│   └── dhara_bbal.c          # implementation
├── migration/
│   ├── dhara_migration.h     # public API
│   └── dhara_migration.c     # implementation
└── CMakeLists.txt            # add dhara_bbal.c + dhara_migration.c
```

---

## 5. Data Structures

### `dhara_bbal_t`  (`dhara_bbal.h`)

```c
/**
 * Bad Block Abstraction Layer context.
 *
 * Embed this in your application struct or allocate statically.
 * Pass &bbal.logical_nand (NOT the phys_nand) to dhara_map_init().
 */
typedef struct {
    /**
     * Modified nand descriptor presented to Dhara.
     *   .log2_page_size  == phys_nand->log2_page_size  (unchanged)
     *   .log2_ppb        == phys_nand->log2_ppb         (unchanged)
     *   .num_blocks      == phys_nand->num_blocks - num_bad
     *
     * MUST be the first field (enables cast-based container_of).
     */
    dhara_nand_t         logical_nand;

    /** The underlying physical NAND descriptor (user-provided). */
    const dhara_nand_t  *phys_nand;

    /**
     * Remapping table: logical_to_phys[logical_block] = physical_block.
     * Length = logical_nand.num_blocks.
     * Heap-allocated by dhara_bbal_init(); freed by dhara_bbal_deinit().
     */
    dhara_block_t       *logical_to_phys;

    uint16_t             num_logical;  /**< = phys_nand->num_blocks - num_bad */
    uint16_t             num_bad;      /**< bad blocks found during init scan  */
} dhara_bbal_t;
```

---

## 6. Public API

### `dhara_bbal.h`

```c
/**
 * @brief  Initialise the BBAL and build the remapping table.
 *
 * Scans every physical block by calling dhara_nand_is_bad() on each.
 * Builds logical_to_phys[] containing only good physical block indices,
 * in ascending order.
 *
 * Sets logical_nand.num_blocks = total - num_bad so that Dhara sees a
 * perfectly contiguous, bad-block-free address space.
 *
 * After this call, pass &bbal->logical_nand to dhara_map_init().
 *
 * @param bbal       Caller-allocated context struct (uninitialised).
 * @param phys_nand  Physical NAND descriptor (must outlive bbal).
 * @return  0 on success, -1 if malloc fails (errno = ENOMEM).
 */
int dhara_bbal_init(dhara_bbal_t *bbal, const dhara_nand_t *phys_nand);

/**
 * @brief  Free heap resources allocated by dhara_bbal_init().
 */
void dhara_bbal_deinit(dhara_bbal_t *bbal);
```

The 7 `dhara_nand_*` callback implementations are provided by `dhara_bbal.c`. No explicit registration is needed — the linker resolves them. Because `logical_nand` is the first field of `dhara_bbal_t`, any `dhara_nand_t *n` passed by Dhara can be cast directly to `dhara_bbal_t *`.

### `dhara_migration.h`

```c
/**
 * @brief  Copy all mapped logical sectors from src_map to dst_map.
 *
 * Iterates sectors 0 .. min(capacity_src, capacity_dst) - 1.
 * For each mapped sector on src, reads it and writes it to dst.
 * Unmapped sectors on src are trimmed on dst.
 * Calls dhara_map_sync() on dst after all sectors are transferred.
 *
 * Both maps must already be initialised and resumed before calling this.
 * dst_map should be freshly formatted (dhara_map_clear + dhara_map_sync)
 * before calling, or existing sectors will be overwritten.
 *
 * @param src_map     Source Dhara map (read-only during migration).
 * @param dst_map     Destination Dhara map.
 * @param page_buf    Scratch buffer; must be >= (1 << log2_page_size) bytes.
 * @param progress_cb Optional callback invoked after each sector transfer.
 *                    current: sectors processed so far, total: total to process.
 * @param user_data   Passed through to progress_cb unchanged.
 * @param err         Receives the first error code on failure.
 * @return  0 on success, -1 on error (details in *err).
 */
int dhara_migrate(
    dhara_map_t  *src_map,
    dhara_map_t  *dst_map,
    uint8_t      *page_buf,
    void        (*progress_cb)(uint32_t current, uint32_t total, void *user_data),
    void         *user_data,
    dhara_error_t *err
);
```

---

## 7. Implementation Details

### 7.1 HAL Callback Wrappers (`dhara_bbal.c`)

All 7 callbacks follow the same pattern: cast `n` to `dhara_bbal_t *`, translate the logical block/page to physical, delegate to the real HAL.

**Block translation:**
```c
static inline dhara_bbal_t *bbal_from_nand(const dhara_nand_t *n)
{
    /* logical_nand is the first field of dhara_bbal_t */
    return (dhara_bbal_t *)(uintptr_t)n;
}

static inline dhara_block_t phys_block(const dhara_bbal_t *b, dhara_block_t logical)
{
    return b->logical_to_phys[logical];
}
```

**Page translation:**
```c
static inline dhara_page_t phys_page(const dhara_bbal_t *b, dhara_page_t logical_p)
{
    const uint8_t log2_ppb = b->phys_nand->log2_ppb;
    dhara_block_t lb = logical_p >> log2_ppb;
    dhara_page_t  page_within_block = logical_p & ((1u << log2_ppb) - 1u);
    return (phys_block(b, lb) << log2_ppb) | page_within_block;
}
```

**All 7 wrappers:**

```c
int dhara_nand_is_bad(const dhara_nand_t *n, dhara_block_t b)
{
    /* After BBAL init, all logical blocks are good — but forward anyway
     * so Dhara's own internal checks remain consistent. */
    dhara_bbal_t *bbal = bbal_from_nand(n);
    return dhara_nand_is_bad(bbal->phys_nand, phys_block(bbal, b));
}

void dhara_nand_mark_bad(const dhara_nand_t *n, dhara_block_t b)
{
    dhara_bbal_t *bbal = bbal_from_nand(n);
    dhara_nand_mark_bad(bbal->phys_nand, phys_block(bbal, b));
    /* Note: logical_to_phys still contains this entry until next reboot.
     * Dhara handles the block at the journal level; on next init the OOB
     * marker will cause this block to be excluded from the table. */
}

int dhara_nand_erase(const dhara_nand_t *n, dhara_block_t b, dhara_error_t *err)
{
    dhara_bbal_t *bbal = bbal_from_nand(n);
    return dhara_nand_erase(bbal->phys_nand, phys_block(bbal, b), err);
}

int dhara_nand_prog(const dhara_nand_t *n, dhara_page_t p,
                    const uint8_t *data, dhara_error_t *err)
{
    dhara_bbal_t *bbal = bbal_from_nand(n);
    return dhara_nand_prog(bbal->phys_nand, phys_page(bbal, p), data, err);
}

int dhara_nand_is_free(const dhara_nand_t *n, dhara_page_t p)
{
    dhara_bbal_t *bbal = bbal_from_nand(n);
    return dhara_nand_is_free(bbal->phys_nand, phys_page(bbal, p));
}

int dhara_nand_read(const dhara_nand_t *n, dhara_page_t p,
                    size_t offset, size_t length,
                    uint8_t *data, dhara_error_t *err)
{
    dhara_bbal_t *bbal = bbal_from_nand(n);
    return dhara_nand_read(bbal->phys_nand, phys_page(bbal, p),
                           offset, length, data, err);
}

int dhara_nand_copy(const dhara_nand_t *n,
                    dhara_page_t src, dhara_page_t dst,
                    dhara_error_t *err)
{
    dhara_bbal_t *bbal = bbal_from_nand(n);
    return dhara_nand_copy(bbal->phys_nand,
                           phys_page(bbal, src),
                           phys_page(bbal, dst), err);
}
```

> **Name collision note:** Since `dhara_nand_*` are free functions resolved by the linker, the BBAL *is* the implementation of those functions. The underlying hardware HAL must use differently-named functions (e.g. `spi_nand_is_bad`, `spi_nand_erase`, etc.) or be separated into a different translation unit. The wrappers call `bbal->phys_nand`'s operations via whatever real HAL the user registered.
>
> In practice for `spi_nand_flash`, the existing SPI NAND driver functions can be called directly by name inside these wrappers.

### 7.2 Init — Startup Scan

```c
int dhara_bbal_init(dhara_bbal_t *bbal, const dhara_nand_t *phys_nand)
{
    const unsigned int total = phys_nand->num_blocks;

    bbal->phys_nand = phys_nand;
    bbal->num_bad   = 0;

    bbal->logical_to_phys = malloc(total * sizeof(dhara_block_t));
    if (!bbal->logical_to_phys) return -1;

    unsigned int logical = 0;
    for (unsigned int phys = 0; phys < total; phys++) {
        if (dhara_nand_is_bad(phys_nand, (dhara_block_t)phys)) {
            bbal->num_bad++;
        } else {
            bbal->logical_to_phys[logical++] = (dhara_block_t)phys;
        }
    }

    bbal->num_logical = (uint16_t)logical;

    /* Shrink allocation to actual size (optional, saves memory) */
    bbal->logical_to_phys = realloc(bbal->logical_to_phys,
                                    logical * sizeof(dhara_block_t));

    /* Build the logical nand descriptor Dhara will use */
    bbal->logical_nand = *phys_nand;               /* copy geometry */
    bbal->logical_nand.num_blocks = logical;       /* override block count */

    return 0;
}

void dhara_bbal_deinit(dhara_bbal_t *bbal)
{
    free(bbal->logical_to_phys);
    bbal->logical_to_phys = NULL;
}
```

### 7.3 Migration Utility

```c
int dhara_migrate(dhara_map_t *src_map, dhara_map_t *dst_map,
                  uint8_t *page_buf,
                  void (*progress_cb)(uint32_t, uint32_t, void *),
                  void *user_data, dhara_error_t *err)
{
    dhara_sector_t cap_src = dhara_map_capacity(src_map);
    dhara_sector_t cap_dst = dhara_map_capacity(dst_map);

    if (cap_dst < cap_src) {
        /* Destination has less capacity — caller must decide how to handle */
        if (err) *err = DHARA_E_MAP_FULL;
        return -1;
    }

    const dhara_sector_t total = cap_src;

    for (dhara_sector_t s = 0; s < total; s++) {
        dhara_page_t loc;
        int found = dhara_map_find(src_map, s, &loc, err);

        if (found == 0) {
            /* Sector is mapped on src — read and write to dst */
            if (dhara_map_read(src_map, s, page_buf, err) < 0) return -1;
            if (dhara_map_write(dst_map, s, page_buf, err) < 0) return -1;
        } else {
            /* Sector is unmapped on src — trim it on dst */
            if (dhara_map_trim(dst_map, s, err) < 0) return -1;
        }

        if (progress_cb) progress_cb(s + 1, total, user_data);
    }

    return dhara_map_sync(dst_map, err);
}
```

---

## 8. Usage Example

```c
/* 1. Describe the physical chip (geometry only, unchanged from today) */
static const dhara_nand_t phys_nand = {
    .log2_page_size = 11,   /* 2048 bytes */
    .log2_ppb       = 6,    /* 64 pages/block */
    .num_blocks     = 1024,
};

/* 2. BBAL context + page buffer */
static dhara_bbal_t bbal;
static uint8_t page_buf[2048];
static struct dhara_map map;

void app_init(void)
{
    /* Scans all blocks, builds logical_to_phys[] */
    dhara_bbal_init(&bbal, &phys_nand);

    /* Pass the LOGICAL nand descriptor to Dhara — not the physical one */
    dhara_map_init(&map, &bbal.logical_nand, page_buf, /*gc_ratio=*/4);
    dhara_map_resume(&map, NULL);
}
```

---

## 9. Portability & Limitations

### What works

- **Logical migration between any two chips** via `dhara_migrate()`. Each chip has its own BBAL that hides its bad blocks. Content is transferred sector by sector at the logical level, so the bad block layout of source and destination are irrelevant.

### What does NOT work

- **Any form of raw bit-copy of flash contents between chips** (even chips from the same lot, same SKU, or the same physical chip after a new bad block develops): all `dhara_page_t` values stored in Dhara's radix-tree metadata and journal checkpoints are logical BBAL addresses, not raw physical addresses. The BBAL's `logical_to_phys` table is rebuilt from OOB bad-block markers at every `dhara_bbal_init()`. If the bad block layout changes at all — a new runtime bad block, a different chip, a reflash — the table will differ and every stored page pointer will silently refer to the wrong physical location. **Always use `dhara_migrate()` to move content between any two BBAL-managed devices.**

---

## 10. RAM Usage

### Remapping table

The only heap allocation made by the BBAL is `logical_to_phys[]`:

```
heap bytes = (total_blocks − num_bad) × sizeof(dhara_block_t)
           = num_logical × 4
```

Typical figures (assuming ≤ 2 % factory bad blocks, 64 pages/block, 2 KiB pages):

| Chip capacity | Total blocks | Table size |
|---|---|---|
| 128 MiB | 1 024 | ≈ 4 KiB |
| 256 MiB | 2 048 | ≈ 8 KiB |
| 512 MiB | 4 096 | ≈ 16 KiB |
| 1 GiB | 8 192 | ≈ 32 KiB |

The `dhara_bbal_t` struct itself is negligible (two pointers, two `uint16_t`s, < 32 bytes). The allocation is resized with `realloc` after the scan so the final size reflects only the good blocks actually found.

### Init scan cost

`dhara_bbal_init()` performs one OOB read per physical block (`dhara_nand_is_bad`) to build the table — an O(N) linear scan on every boot. There is no persistent bad block table (BBT) and no caching across reboots. At typical SPI NAND read latencies (50–200 µs/page) this adds roughly 50–200 ms of blocking init time for a 128 MiB device, scaling linearly with block count.

### Flash writes

The BBAL itself issues no flash writes. `mark_bad` forwards a write initiated by Dhara's journal recovery, not by the BBAL. `dhara_migrate()` writes to the destination map, but only when explicitly called.

---

## 11. Implementation Task List

| # | File | Task |
|---|------|------|
| 1 | `include/dhara_bbal.h` | Struct and API declarations |
| 2 | `src/dhara_bbal.c` | `dhara_bbal_init`, `dhara_bbal_deinit`, 7 HAL wrappers |
| 3 | `migration/dhara_migration.h` | Migration API declaration |
| 4 | `migration/dhara_migration.c` | `dhara_migrate` implementation |
| 5 | `CMakeLists.txt` | Add `src/dhara_bbal.c` and `migration/dhara_migration.c` |
| 6 | `host_test/` | Test: two sim NAND instances with different bad block patterns, migrate, verify all sectors match |

---

## 12. Key Constraints

- The upstream `dhara/` submodule is **not modified**.
- `logical_nand` must be the **first field** of `dhara_bbal_t` to allow the cast-based `bbal_from_nand()` pattern.
- The underlying SPI NAND HAL functions must not share the same names as the Dhara `dhara_nand_*` callbacks (linker will conflict). In `spi_nand_flash` they already use different names (`spi_nand_read`, etc.) so this is not an issue.
- `logical_nand.num_blocks` is set to `total - num_bad` at init. If more bad blocks are discovered at runtime (and `mark_bad` is called), `num_logical` does not decrease until the next `dhara_bbal_init()`. Dhara's own journal recovery handles this correctly within the current session.
