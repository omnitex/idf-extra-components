# ECC Page Relief Wear Leveling

## Overview

Page-level ECC relief is an optional wear-leveling enhancement that uses the
NAND chip's on-chip ECC engine as an early-warning sensor for individual page
aging.

When a page reports elevated correctable ECC bit errors — indicating wear or
read-disturb stress — it is marked for **relief**: on the next journal write
that would target that physical page, the write is redirected to the next
available page instead. The worn page is left unprogrammed and will be erased
as part of its block's normal erase cycle.

This mechanism layers on top of Dhara's existing FIFO wear leveling. It does
not replace block-level wear leveling; it adds per-page granularity using
hardware feedback available with every read operation.

## How It Works

1. After every successful `nand_read()` (regardless of caller — Dhara GC,
   Dhara map traversal, or consumer reads through the WL BDL), the ECC
   status is checked.
2. If the status is at or above `high_threshold`, the page is immediately
   flagged for relief.
3. If the status is at or above `mid_threshold` but below `high_threshold`,
   a counter is incremented. When the counter reaches `mid_count_limit`,
   the page is flagged.
4. Before each `dhara_journal_enqueue()` write, the target physical page is
   checked against the relief map. If flagged, a filler (empty) journal slot
   is written, the flag is cleared, and the actual data write retries on the
   next available page.
5. After a successful block erase, all relief map entries for that block are
   removed (the erase resets physical wear history).

## Configuration

Add an `ecc_relief` sub-struct to your `spi_nand_flash_config_t`:

```c
spi_nand_flash_config_t config = {
    .device_handle = spi_dev,
    .gc_factor     = 45,
    .ecc_relief = {
        .enabled               = true,
        .mid_threshold         = 2,   // NAND_ECC_1_TO_3_BITS_CORRECTED or similar
        .high_threshold        = 4,   // NAND_ECC_4_TO_6_BITS_CORRECTED or similar
        .mid_count_limit       = 5,   // flag after 5 MID-level reads
        .max_consecutive_relief = 4,  // max pages to skip in one write
        .map_capacity          = 512, // power of 2; 0 = use Kconfig default
    },
};
```

**Threshold values** correspond to the `nand_ecc_status_t` enumeration values
reported by your specific NAND chip. Refer to your chip's datasheet for the
mapping between bit-error counts and status register values.

**Feature is disabled by default.** Existing code requires no changes.

## Memory Usage

The relief map is an in-RAM open-addressing hash table:

| `map_capacity` | RAM    |
|----------------|--------|
| 64             | 384 B  |
| 256            | 1.5 KB |
| 512 (default)  | 3 KB   |
| 1024           | 6 KB   |

The map is allocated from internal RAM (`MALLOC_CAP_INTERNAL`) at init time.

## Diagnostic API

```c
spi_nand_ecc_relief_stats_t stats;
esp_err_t ret = spi_nand_flash_get_ecc_relief_stats(handle, &stats);
if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Pages pending relief:  %" PRIu32, stats.pages_pending_relief);
    ESP_LOGI(TAG, "Total pages relieved:  %" PRIu32, stats.total_pages_relieved);
    ESP_LOGI(TAG, "Map entries used:      %" PRIu32 " / %" PRIu32,
             stats.map_entries_used, stats.map_capacity);
    ESP_LOGI(TAG, "Consecutive cap hits:  %" PRIu32, stats.consecutive_cap_hits);
} else if (ret == ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(TAG, "ECC relief feature is not enabled");
}
```

Returns `ESP_ERR_NOT_SUPPORTED` when `ecc_relief.enabled == false`.

## Kconfig

| Symbol | Default | Description |
|--------|---------|-------------|
| `NAND_FLASH_ECC_RELIEF_DEFAULT_MAP_CAPACITY` | 512 | Default map capacity when `map_capacity == 0` in config |
| `NAND_FLASH_ECC_RELIEF_ENABLE_DEFAULT` | n | Enable feature by default for all devices |

## Performance Impact

- **Read path:** One function pointer call per correctable-ECC read event.
  No impact on clean reads.
- **Write path:** One hash-table lookup per journal page write when the feature
  is enabled. O(1) amortized; negligible latency.
- **Capacity overhead:** Each relieved page consumes one extra journal slot.
  The `max_consecutive_relief` cap bounds worst-case overhead to
  `max_consecutive_relief` extra slots per write.

## Limitations

- The relief map is **not persistent** across reboots. State is rebuilt
  gradually as reads observe ECC levels on the next power cycle.
- The feature works on physical page granularity; it does not directly control
  which logical sectors land on worn pages (Dhara's map layer handles that).
- Under heavy wear, if the map fills up, a non-pending entry is evicted to
  make room (with an `ESP_LOGW` warning). Increase `map_capacity` if this
  occurs frequently.
