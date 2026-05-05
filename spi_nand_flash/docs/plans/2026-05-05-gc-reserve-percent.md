# GC Reserve Percent Kconfig Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the opaque `gc_factor = 45` hardcoded default with a Kconfig option `NAND_GC_RESERVE_PERCENT` (integer percentage) that is converted to `gc_ratio` in `dhara_glue.c`, making the GC headroom setting human-readable and configurable without changing the public API.

**Architecture:** The conversion formula `gc_ratio = (100 / reserve_pct) - 1` lives entirely in `nand.c` (the two places that apply the default when `gc_factor == 0`). Kconfig provides the default percentage. User-supplied `gc_factor` in `spi_nand_flash_config_t` is untouched — it still overrides everything. No changes to Dhara library internals.

**Tech Stack:** C (ESP-IDF), Kconfig, spi_nand_flash component (`spi_nand_flash/`)

---

## Background: the conversion math

```
dhara_map_capacity() reserves:  cap / (gc_ratio + 1)
reserve_pct = 100 / (gc_ratio + 1)
→ gc_ratio   = (100 / reserve_pct) - 1
```

Integer division means the actual reserved percentage is `100 / (gc_ratio + 1)`, which is ≥ the requested percentage (rounds up headroom). This is documented explicitly in the Kconfig help text.

Examples:
| `NAND_GC_RESERVE_PERCENT` | `gc_ratio` | actual reserve |
|---|---|---|
| 10 | 9 | 10.0% |
| 15 | 5 | 16.7% |
| 20 | 4 | 20.0% |
| 25 | 3 | 25.0% |
| 45 | 1 | 50.0% (old default mapped back) |

Default is **10** (gc_ratio = 9), replacing the previous hardcoded 45 (which gave only ~2.2% headroom).

---

## Files to touch

| File | Change |
|---|---|
| `spi_nand_flash/Kconfig` | Add `NAND_GC_RESERVE_PERCENT` config option |
| `spi_nand_flash/src/nand.c` | Replace both `gc_factor = 45` with the conversion formula |
| `spi_nand_flash/include/spi_nand_flash.h` | Update `gc_factor` field doc comment |

---

### Task 1: Add Kconfig option

**Files:**
- Modify: `spi_nand_flash/Kconfig`

**Step 1: Add the new config entry**

Insert after `config NAND_ENABLE_STATS` block, before `endmenu`:

```kconfig
    config NAND_GC_RESERVE_PERCENT
        int "GC reserve headroom (% of flash capacity)"
        range 2 50
        default 10
        help
            Percentage of total flash capacity reserved as GC (garbage
            collection) runway. This is converted to Dhara's internal
            gc_ratio using: gc_ratio = (100 / NAND_GC_RESERVE_PERCENT) - 1.

            Due to integer division the actual reserved percentage is
            rounded up to the nearest value where (gc_ratio + 1) divides
            100 evenly. For example, requesting 15% yields gc_ratio = 5,
            which actually reserves 16.7%.

            Lower values maximize usable capacity but cause larger write
            latency spikes when GC triggers. Higher values give smoother
            GC at the cost of usable space.

            This default only applies when gc_factor is 0 (not set by
            the application) in spi_nand_flash_config_t. A non-zero
            gc_factor in the config struct always takes precedence.

            Recommended values:
              10  - general purpose (default, gc_ratio = 9)
              20  - write-heavy / latency-sensitive (gc_ratio = 4)
              50  - maximum smoothness, half capacity lost (gc_ratio = 1)
```

**Step 2: Verify Kconfig builds**

```bash
# In any ESP-IDF project using this component, run:
idf.py menuconfig
# Navigate to "SPI NAND Flash configuration" and verify the new option appears.
# No build errors expected at this stage.
```

**Step 3: Commit**

```bash
git add spi_nand_flash/Kconfig
git commit -m "kconfig: add NAND_GC_RESERVE_PERCENT option for human-readable GC headroom"
```

---

### Task 2: Apply conversion in `nand.c`

**Files:**
- Modify: `spi_nand_flash/src/nand.c`

There are **two** independent locations that set the default `gc_factor = 45`:
- Line ~31: inside `spi_nand_flash_init_device()` (legacy path, no BDL)
- Line ~255: inside `spi_nand_flash_init_with_layers()` (BDL path)

Both need the same replacement.

**Step 1: Replace first occurrence (legacy path, ~line 30)**

Old:
```c
    if (!config->gc_factor) {
        config->gc_factor = 45;
    }
```

New:
```c
    if (!config->gc_factor) {
        config->gc_factor = (100 / CONFIG_NAND_GC_RESERVE_PERCENT) - 1;
    }
```

**Step 2: Replace second occurrence (BDL path, ~line 254)**

Same replacement — identical pattern.

**Step 3: Build to verify no compile errors**

```bash
idf.py build
# Expected: clean build, no errors or warnings on nand.c
```

**Step 4: Commit**

```bash
git add spi_nand_flash/src/nand.c
git commit -m "feat(nand): derive gc_factor default from NAND_GC_RESERVE_PERCENT Kconfig"
```

---

### Task 3: Update the public header doc comment

**Files:**
- Modify: `spi_nand_flash/include/spi_nand_flash.h`

**Step 1: Update the `gc_factor` field comment**

Old:
```c
    uint8_t gc_factor;                       ///< The gc factor controls the number of blocks to spare block ratio.
    ///< Lower values will reduce the available space but increase performance
```

New:
```c
    uint8_t gc_factor; ///< Dhara gc_ratio: controls GC headroom as 100/(gc_factor+1) percent of capacity.
    ///< Set to 0 to use the Kconfig default (CONFIG_NAND_GC_RESERVE_PERCENT).
    ///< Higher values leave less headroom (more usable space, larger GC latency spikes).
    ///< Lower values reserve more headroom (less usable space, smoother GC latency).
    ///< Example: gc_factor=9 reserves 10%, gc_factor=4 reserves 20%, gc_factor=1 reserves 50%.
```

**Step 2: Commit**

```bash
git add spi_nand_flash/include/spi_nand_flash.h
git commit -m "docs: clarify gc_factor field semantics and Kconfig fallback"
```

---

## Done criteria

- [ ] `idf.py menuconfig` shows `NAND_GC_RESERVE_PERCENT` under "SPI NAND Flash configuration" with default 10
- [ ] `idf.py build` succeeds with no new warnings
- [ ] Setting `gc_factor = 0` in config uses Kconfig-derived value
- [ ] Setting `gc_factor = 4` in config still overrides to exactly 4 (user override unchanged)
- [ ] `gc_factor` doc comment correctly describes the percentage relationship and the 0-means-use-Kconfig behaviour
