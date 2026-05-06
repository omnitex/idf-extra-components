# Fault Simulator & ftl\_eval Improvement Notes

Analysis of gaps and extension opportunities in `nand_fault_sim` and the `ftl_eval` app,
focused on enabling host-side exercise of the page write relief mechanism
(commit `4ab6e5fd`) and broader FTL evaluation.

---

## Fault simulator

### 1. `nand_get_ecc_status()` is a dead stub — page relief is silently broken

**The core issue.**
`nand_impl.c` calls `read_page_and_wait()` before every `nand_prog` and `nand_copy`
when `CONFIG_NAND_FLASH_PROG_PAGE_RELIEF` is enabled. `read_page_and_wait` calls
`nand_get_ecc_status()` and then the relief gate checks `ecc_corrected_bits_status`.

In the fault sim (`nand_fault_sim.c` lines 671–676) `nand_get_ecc_status` is:

```c
esp_err_t nand_get_ecc_status(spi_nand_flash_device_t *handle, uint32_t page)
{
    (void)handle;
    (void)page;
    return ESP_OK;
}
```

It never writes `handle->chip.ecc_data.ecc_corrected_bits_status`, so it stays
`NAND_ECC_OK` forever. **Page relief is permanently dead in every host test.**

**Fix:** implement `nand_get_ecc_status` to derive ECC severity from the erase count
of the block containing `page`. Three new config fields:

```c
uint32_t ecc_prog_mid_erase_threshold;   /* erases → NAND_ECC_1_TO_3_BITS_CORRECTED */
uint32_t ecc_prog_high_erase_threshold;  /* erases → NAND_ECC_4_TO_6_BITS_CORRECTED */
uint32_t ecc_prog_fail_erase_threshold;  /* erases → NAND_ECC_NOT_CORRECTED          */
float    ecc_prog_noise_prob;            /* probabilistic bump (page-to-page variation) */
```

This maps directly to flash age: a block that has been erased many times has
a higher baseline BER, so any page in it will report elevated ECC on the pre-prog
read, triggering page relief. Erase count is already tracked per-block in `s_sim`.

---

### 2. Two ECC models exist but are not clearly separated

The existing `ecc_mid/high/fail_threshold` fields (lines 98–101 of `nand_fault_sim.h`)
model **read-disturb** — ECC level rises with read count of a page. They fire through
`sim_invoke_ecc_cb()` in `nand_read()`.

The new pre-prog fields (item 1 above) model **write wear** — ECC level rises with
erase count of the block. They fire through `nand_get_ecc_status()`.

These two models are consumed by completely different code paths:

| Model | Driver path | Consumer |
|---|---|---|
| Read-disturb | `nand_read()` → `on_page_read_ecc` callback | Scrubbing / data-refresh logic |
| Write-wear | `read_page_and_wait()` → `nand_get_ecc_status()` → `ecc_corrected_bits_status` | Page relief gate in `nand_impl.c` |

The header struct should have clearly named sections for each model with a comment
explaining what code path each one targets. Currently the only comment is
`/* ECC read-disturb simulation */` which covers both, causing confusion.

---

### 3. `ecc_data_refresh_threshold` hardcoded — must agree with Kconfig

In `nand_init_device()` the sim hardcodes:

```c
(*handle)->chip.ecc_data.ecc_data_refresh_threshold = 4;
(*handle)->chip.ecc_data.ecc_status_reg_len_in_bits = 2;
```

The page relief gate in `nand_impl.c` compares against
`CONFIG_NAND_FLASH_PROG_PAGE_RELIEF_MIN_ECC`. If `ecc_data_refresh_threshold`
(used by the block-health layer) and `CONFIG_NAND_FLASH_PROG_PAGE_RELIEF_MIN_ECC`
(used by the relief gate) don't agree with the `ecc_prog_*_erase_threshold` values
you choose, relief will either never fire or fire too aggressively.

At minimum: document in the header that these values need to be consistent.
Better: expose `ecc_data_refresh_threshold` as a config field so sweep configs can
set it explicitly.

---

### 4. `nand_copy()` has no ECC simulation on the source page

`nand_copy()` in the fault sim does a bare `nand_emul_read` for the source page
with no ECC simulation, and `nand_emul_write` for the destination with no
`nand_get_ecc_status` call. In the real driver, `nand_copy` calls
`read_page_and_wait` on the **destination** page (to trigger relief on the copy
target). The sim skips this entirely.

After fixing `nand_get_ecc_status`, the copy path also needs to call it on the
destination page before writing — otherwise copy-triggered page relief remains
untestable. This is a direct mirror of the `nand_impl.c` copy path under
`CONFIG_NAND_FLASH_PROG_PAGE_RELIEF`.

---

### 5. `copy_fail_prob` error cannot reach the ECC branch in `dhara_glue.c`

When `copy_fail_prob` fires, the sim returns `ESP_FAIL` without setting
`ecc_corrected_bits_status`. In `dhara_glue.c`, the copy error handler is:

```c
if (ret == ESP_ERR_SPI_NAND_PAGE_RELIEF) {
    dhara_set_error(err, DHARA_E_PAGE_RELIEF);
} else if (dev_handle->chip.ecc_data.ecc_corrected_bits_status == NAND_ECC_NOT_CORRECTED) {
    dhara_set_error(err, DHARA_E_ECC);
} else if (ret == ESP_ERR_NOT_FINISHED) {
    dhara_set_error(err, DHARA_E_BAD_BLOCK);
}
```

Because `ecc_corrected_bits_status` is never `NAND_ECC_NOT_CORRECTED` in the sim,
the `DHARA_E_ECC` branch is unreachable in host tests. A targeted `copy_ecc_fail_prob`
field (like `read_fail_prob` but sets `ecc_corrected_bits_status = NAND_ECC_NOT_CORRECTED`
before returning fail) would make this branch testable independently of write-wear.

---

### 6. Preset scenario values are magic numbers without physical rationale

The `AGED` preset has `grave_page_threshold = 3000` and `max_prog_cycles = 5000`
with no explanation of what NAND device this models. The `FAILING` preset has
`prog_fail_prob = 0.02` — but 2% prog failure is orders of magnitude above what
real NAND specifies even at end-of-life.

Each preset should carry a comment block stating:
- What device class it approximates (e.g. "consumer MLC, 3000 P/E cycle rating")
- What fraction of device lifetime it represents (e.g. "~50% of erase budget consumed")
- Why the probabilistic failure rates are chosen (e.g. "2% models worst-case
  retention-failed pages, not in-spec prog failures")

---

### 7. No per-block ECC baseline variation (future extension)

Real flash has spatial variation: some blocks come from the factory with a slightly
higher BER. The erase-count-based model handles temporal variation well but doesn't
model spatial variation.

A future `ecc_prog_block_offset[]` array (per-block ECC severity bump, populated at
init from a distribution) would allow this, but it's low priority compared to the
items above. The erase-count model is already a good approximation because
hot-spot blocks naturally accumulate more erases.

---

## ftl\_eval app

### 8. Page relief activity is invisible in the report

The report has metrics for writes, erases, prog ops, bad blocks, WAF — but nothing
about page relief. A run with page relief enabled on an "aged" scenario is
indistinguishable in `report.json` from a run where relief never fired.

Missing metrics:

| Metric | Description |
|---|---|
| `page_relief_skips` | Total pages skipped because pre-prog ECC ≥ threshold |
| `page_relief_first_at_write` | Logical write index when first relief skip occurred |
| `page_relief_blocks_affected` | Number of distinct blocks that had at least one relief skip |

The sim's `nand_get_ecc_status()` (once fixed) is the right place to increment a
counter. Alternatively, a relief-specific callback (similar to `on_page_read_ecc`)
would let the app collect this without modifying the sim internals.

---

### 9. ECC event counters not wired up

The sim already has `on_page_read_ecc` callback support. The ftl_eval app never
sets it. Without it, "aged" and "fresh" scenarios produce identical ECC event counts
in the report (zero).

Suggested additions to `metrics_t`:

```c
uint32_t ecc_mid_events;    /* NAND_ECC_1_TO_3_BITS_CORRECTED occurrences */
uint32_t ecc_high_events;   /* NAND_ECC_4_TO_6_BITS_CORRECTED occurrences */
uint32_t ecc_fail_events;   /* NAND_ECC_NOT_CORRECTED occurrences          */
uint32_t ecc_hotspot_block; /* block index with the most ECC events        */
```

---

### 10. `scenario_config_t` and JSON parser don't expose the new pre-prog ECC fields

When the three `ecc_prog_*_erase_threshold` fields are added to
`nand_fault_sim_config_t`, they also need to be added to `scenario_config_t` in
`app_config.h` and parsed in `app_config.c`.

Scenarios using `"preset"` get the new fields for free through
`nand_fault_sim_config_preset()`. But inline custom scenarios (no `"preset"` key)
will silently get zeros for the new fields unless the parser is updated.

Suggested JSON fields (following the existing naming style):

```json
{
  "name": "wear_profile_aged",
  "ecc_prog_mid_erase_threshold": 300,
  "ecc_prog_high_erase_threshold": 600,
  "ecc_prog_fail_erase_threshold": 900,
  "ecc_prog_noise_prob": 0.05
}
```

---

### 11. No way to start from a pre-worn flash state

Every run starts from blank flash. A `LIGHTLY_USED` scenario sets `max_erase_cycles`
to limit future erases but starts with all erase counts at zero. This means:
- Erase-count-based ECC (item 1) never fires in the first run, only after blocks
  have been erased enough times during the test itself.
- To exercise page relief from the very first write, you need blocks that are
  already worn.

A `pre_warm_erase_cycles` field would make the sim pre-populate `erase_count[]`
for all blocks before the test starts:

```c
uint32_t pre_warm_erase_cycles; /*!< Pre-populate all erase_count[] to this value at init (0 = off) */
```

Combined with a matching JSON field, this would let you say "simulate a flash that
has already consumed 80% of its erase budget" without needing to actually run 8000
erase cycles as warmup.

---

### 12. No read-intensive workload for read-disturb testing

The three existing workloads (sequential, random, mixed 70/30) are all write-heavy.
Read-disturb ECC (the `ecc_mid/high/fail_threshold` model) only fires when pages
are read many times. There is no way to exercise the data-refresh / scrubbing path
from the ftl_eval app.

A `"read_loop"` workload type — write all logical pages once, then read each page
N times — would close this gap. It would specifically target the
`on_page_read_ecc` callback → `ecc_high_events` metric path.

---

### 13. Access pattern skew not modelled

Both `"sequential"` and `"random"` workloads have uniform LBA access distribution.
Real embedded workloads are skewed: a small set of "hot" LBAs (config sectors,
journal head) receives the majority of writes. This is the primary driver of
uneven wear and high Gini coefficients in practice.

A `"zipf"` workload type with a configurable skew exponent would stress
wear-leveling more realistically and produce more meaningful WAF vs Gini tradeoffs
in the sweep output.

---

### 14. No remount / data-integrity check after power-loss scenario

After a `POWER_LOSS` crash scenario, the app tears down the device and exits.
It never:
1. Remounts the flash image from the mmap file
2. Verifies that data written before the crash is intact
3. Checks that the FTL returns to a consistent state

The orphaned-pages metadata replay (commit `50bb217`) specifically handles
post-crash remount. The ftl_eval app would be the correct place to exercise this
end-to-end: crash → remount → read-back → report `recovery_status` and
`data_loss_pages` in the result.

---

## Priority summary

| # | Area | Effort | Impact |
|---|---|---|---|
| 1 DONE | Fix `nand_get_ecc_status()`, add erase-count ECC fields | Small | **Unblocks page relief entirely** |
| 4 DONE | Simulate ECC on `nand_copy()` destination | Small | Completes the relief path |
| 8 DONE | Add page relief metrics to report | Small | Makes relief activity observable |
| 9 DONE | Wire `on_page_read_ecc` in ftl_eval | Small | Makes ECC events visible in report |
| 10 DONE | Expose new fields in JSON parser | Small | Required for custom scenario relief configs |
| 11 DONE | `pre_warm_erase_cycles` config field | Medium | Enables wear-profile testing from write zero |
| 3 DONE | Document / expose `ecc_data_refresh_threshold` | Trivial | Prevents subtle misconfiguration |
| 2 DONE | Separate ECC model sections in header | Trivial | Maintainability |
| 5 DONE | `copy_ecc_fail_prob` for ECC branch coverage | Small | Test coverage completeness |
| 6 DONE | Add physical rationale comments to presets | Trivial | Readability |
| 12 DONE | `read_loop` workload | Medium | Read-disturb path coverage |
| 14 DONE | Remount / recovery check after power-loss | Large | End-to-end reliability validation |
| 13 DONE | Zipf workload | Large | Realistic wear-leveling stress |
