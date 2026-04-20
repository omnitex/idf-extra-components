# Design: Page-Level Relief Wear Leveling

## Architecture Overview

```
spi_nand_flash_config_t
  └── spi_nand_ecc_relief_config_t ecc_relief

dhara_init() / nand_wl_attach_ops()
  └── if enabled:
        allocate ecc_relief_map[]
        handle->on_page_read_ecc     = dhara_ecc_read_cb   ← fires for ALL read paths
        handle->on_page_read_ecc_ctx = priv
        dhara_journal_set_relief_hook(&journal, relief_check_cb, priv)

nand_read() [nand_impl.c]           dhara_journal_enqueue() [journal.c]
  │                                    │
  ├─ after is_ecc_error():             ├─ prepare_head() → j->head known
  │   if status is correctable         ├─ call relief_check(j->head, ctx)
  │   and non-zero:                    │    ├─ PENDING: skip prog, write filler,
  │   fire on_page_read_ecc()          │    │  clear flag, retry
  │   → dhara_ecc_read_cb()            │    └─ not pending: dhara_nand_prog()
  │   → updates relief map             └─ push_meta() → advance head
  │
  │  Called from ALL paths:
  │  - dhara_nand_read() (BDL + non-BDL)
  │  - nand_flash_blockdev_read() (Flash BDL)
  │  - spi_nand_flash_read_page() (WL BDL consumer)

dhara_nand_erase()
  └─ on success: evict map entries for block's page range
```

## Data Structures

### Relief Map

Open-addressing hash table in `spi_nand_flash_dhara_priv_data_t`:

```c
typedef struct {
    dhara_page_t page;       // INVALID_PAGE = empty slot
    uint8_t      mid_count;  // accumulated MID-level ECC read count
    uint8_t      flags;      // bit 0 = ECC_RELIEF_FLAG_PENDING
} ecc_relief_entry_t;

// Inside spi_nand_flash_dhara_priv_data_t:
ecc_relief_entry_t *ecc_relief_map;       // NULL if feature disabled
uint16_t            ecc_relief_capacity;  // power of 2, from config
uint32_t            stat_pages_relieved;
uint32_t            stat_consecutive_cap_hits;
```

Memory: `6 bytes * 512 entries` = **3072 bytes** at default capacity.

### Hash Function

```c
static uint16_t relief_hash(dhara_page_t page, uint16_t capacity) {
    return (uint16_t)(page % capacity);  // capacity is power of 2: use & (cap-1)
}
```

Linear probing with wrap-around. Tombstone slots needed for correct probe chains
after eviction — use a dedicated `TOMBSTONE_PAGE` sentinel value.

## Dhara Changes

### `dhara/dhara/dhara/journal.h`

Add two fields to `struct dhara_journal`:

```c
struct dhara_journal {
    /* ... existing fields unchanged ... */

    /* Page relief hook. NULL = disabled.
     * Called inside dhara_journal_enqueue() after prepare_head() succeeds.
     * Returns 1 if j->head should be relieved (skipped), 0 otherwise.
     */
    int  (*relief_check)(dhara_page_t page, void *ctx);
    void  *relief_ctx;
};
```

New function declared in `journal.h`:

```c
void dhara_journal_set_relief_hook(struct dhara_journal *j,
                                   int (*check)(dhara_page_t, void *),
                                   void *ctx);
```

### `dhara/dhara/dhara/journal.c` — `dhara_journal_enqueue()`

Current logic (simplified):

```c
for (i = 0; i < DHARA_MAX_RETRIES; i++) {
    if (!(prepare_head(j, &my_err) ||
            (data && dhara_nand_prog(j->nand, j->head, data, &my_err)))) {
        return push_meta(j, meta, err);
    }
    if (recover_from(j, my_err, err) < 0) return -1;
}
```

Modified logic:

```c
int relief_consecutive = 0;
for (i = 0; i < DHARA_MAX_RETRIES; i++) {
    if (prepare_head(j, &my_err) < 0) {
        if (recover_from(j, my_err, err) < 0) return -1;
        continue;
    }

    /* Relief check: skip prog for this page if flagged */
    if (j->relief_check &&
        relief_consecutive < MAX_CONSECUTIVE_RELIEF &&
        j->relief_check(j->head, j->relief_ctx)) {
        /* Relieve this page: write filler, don't prog */
        if (push_meta(j, NULL, err) < 0) return -1;
        relief_consecutive++;
        continue;  /* retry loop: prepare_head() will give next page */
    }

    /* Normal write */
    if (!(data && dhara_nand_prog(j->nand, j->head, data, &my_err))) {
        return push_meta(j, meta, err);
    }
    if (recover_from(j, my_err, err) < 0) return -1;
}
```

Note: `MAX_CONSECUTIVE_RELIEF` sourced from `j->relief_ctx` at call time.
`push_meta(j, NULL, err)` records a filler slot — this is identical to what
`pad_queue()` already does, so no new code path in the journal metadata layer.

### `dhara_journal_copy()` — same pattern

`dhara_journal_copy()` (used by GC) should also check relief, since GC copying
a live page to a relieved slot should be redirected. Same modification applies.

## Integration in `nand_impl.c` and `nand.h`

### `priv_include/nand.h` — callback field on device struct

```c
struct spi_nand_flash_device_t {
    /* ... existing fields unchanged ... */
    void (*on_page_read_ecc)(uint32_t page, nand_ecc_status_t status, void *ctx);
    void  *on_page_read_ecc_ctx;
};
```

### `src/nand_impl.c` — fire callback in `nand_read()`

After `is_ecc_error()` runs and before returning on success:

```c
nand_ecc_status_t ecc_status = handle->chip.ecc_data.ecc_corrected_bits_status;
if (ecc_status != NAND_ECC_OK && ecc_status != NAND_ECC_NOT_CORRECTED) {
    if (handle->on_page_read_ecc) {
        handle->on_page_read_ecc(page, ecc_status, handle->on_page_read_ecc_ctx);
    }
}
```

This is the only change to `nand_impl.c`. No BDL awareness needed.

## Integration in `dhara_glue.c`

### Init

```c
if (config->ecc_relief.enabled) {
    priv->ecc_relief_map = heap_caps_calloc(config->ecc_relief.map_capacity,
                                            sizeof(ecc_relief_entry_t),
                                            MALLOC_CAP_INTERNAL);
    priv->ecc_relief_capacity = config->ecc_relief.map_capacity;

    /* Wire ECC observation into nand_read() for all callers (BDL + non-BDL) */
    handle->on_page_read_ecc     = dhara_ecc_read_cb;
    handle->on_page_read_ecc_ctx = priv;

    /* Wire relief decision into Dhara's journal enqueue */
    dhara_journal_set_relief_hook(&priv->dhara_map.journal,
                                  dhara_relief_check_cb,
                                  priv);
}
```

### `dhara_ecc_read_cb()` — replaces former `dhara_nand_read()` read hook

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

No ECC classification code lives in `dhara_nand_read()` anymore — that function
only handles hard errors (`DHARA_E_ECC`) as before.

### Eviction in `dhara_nand_erase()`

After successful `nand_erase_block()`:

```c
if (priv->ecc_relief_map) {
    dhara_page_t first = (dhara_page_t)block << n->log2_ppb;
    dhara_page_t count = 1 << n->log2_ppb;
    relief_map_evict_range(priv, first, count);
}
```

## BDL Compatibility

No `#ifdef CONFIG_NAND_FLASH_ENABLE_BDL` branching is needed anywhere in the relief
implementation. The `on_page_read_ecc` callback fires inside `nand_read()`, which is
the terminal function called by all read paths:

| Caller | Path | Callback fires? |
|--------|------|-----------------|
| `dhara_nand_read()` non-BDL | → `nand_read()` directly | Yes |
| `dhara_nand_read()` BDL | → `flash_bdl->ops->read()` → `nand_flash_blockdev_read()` → `nand_read()` | Yes |
| `spi_nand_flash_read_page()` WL BDL consumer | → `nand_read()` | Yes |

The Dhara journal relief hook is also BDL-agnostic — `dhara_journal_enqueue()` and
`dhara_journal_copy()` are called identically in both modes. Thread safety is
inherited from the existing `handle->mutex` held by all callers above `nand_read()`.

## Memory Sizing

| `map_capacity` | RAM      |
|----------------|----------|
| 64             | 384 B    |
| 256            | 1536 B   |
| 512 (default)  | 3072 B   |
| 1024           | 6144 B   |

## Risk / Open Questions

1. **`push_meta()` inside the relief retry loop**: `push_meta()` is `static` in
   `journal.c`. The modified `dhara_journal_enqueue()` already calls it, so no
   visibility issue — but the filler slot must not corrupt the metadata buffer
   state. Must verify `push_meta(j, NULL, err)` with `meta=NULL` works at
   mid-block positions (not just at checkpoint boundaries).

2. **`dhara_journal_copy()` relief**: GC uses `dhara_journal_copy()` not
   `dhara_journal_enqueue()`. If the head lands on a relieved page during GC,
   the copy is also skipped and the live sector is re-copied to the next page.
   This is correct behavior but must be explicitly implemented.

3. **Tombstone handling in hash table**: Evicting a range of entries mid-table
   (on erase) can break open-addressing probe chains. Must use tombstone slots
   or rehash on eviction. Tombstone approach is simpler.

4. **Consecutive cap source**: `MAX_CONSECUTIVE_RELIEF` needs to be accessible
   inside the journal callback. Pass via `relief_ctx` struct pointer.

5. **Journal capacity**: Each relieved page consumes one slot. Under heavy relief
   activity this slightly reduces effective capacity. The consecutive cap bounds
   worst-case overhead per write to `max_consecutive_relief` extra slots.
