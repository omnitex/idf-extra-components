| Supported Targets | Linux |
| ----------------- | ----- |

# ftl_eval — FTL Evaluation App

`ftl_eval` is an ESP-IDF **Linux-target** application that benchmarks Flash Translation Layer (FTL) implementations against simulated NAND fault scenarios. It reads a sweep configuration from JSON, runs every combination of scenario × FTL config, and writes a structured JSON report for offline analysis.

## Quick start

```bash
# From the spi_nand_flash/ftl_eval directory, with ESP-IDF loaded:
idf.py build
./build/ftl_eval.elf --config sweep.json
```

The results are written to `report.json` (or whatever `"output"` is set to in your sweep file).

---

## Building

Requires ESP-IDF 6.0+ (Linux target).

```bash
# Load ESP-IDF environment first (interactive shell)
get_idf

cd spi_nand_flash/ftl_eval
idf.py build
```

The build is controlled by `sdkconfig.defaults`, which sets:

```
CONFIG_IDF_TARGET="linux"
CONFIG_NAND_FLASH_FAULT_SIM=y
```

No `set-target` or `menuconfig` step is needed for a normal run.

---

## CLI usage

```
ftl_eval.elf --config <sweep.json> [--output <report.json>]
```

| Flag | Required | Description |
|------|----------|-------------|
| `--config <path>` | Yes | Path to sweep configuration JSON |
| `--output <path>` | No | Overrides the `"output"` field in the sweep file |

---

## Sweep configuration (`sweep.json`)

The sweep file drives a full matrix of **scenarios × FTL configs**, all using the same NAND geometry and workload.

```json
{
  "name": "gc_factor_sweep",
  "output": "report.json",
  "nand": {
    "num_blocks": 1024,
    "pages_per_block": 64,
    "page_size": 2048
  },
  "scenarios": [
    { "name": "fresh",  "preset": "FRESH" },
    { "name": "aged",   "preset": "AGED"  },
    {
      "name": "custom_wearout",
      "max_erase_cycles": 500,
      "prog_fail_prob": 0.001,
      "erase_fail_prob": 0.0005
    }
  ],
  "ftl_configs": [
    { "name": "dhara_gc3", "ftl": "dhara", "gc_factor": 3 },
    { "name": "dhara_gc5", "ftl": "dhara", "gc_factor": 5 },
    { "name": "dhara_gc8", "ftl": "dhara", "gc_factor": 8 }
  ],
  "workload": {
    "type": "sequential",
    "total_writes": 1000000,
    "write_size_bytes": 4096
  }
}
```

### `nand` — geometry (fixed per sweep)

| Field | Description |
|-------|-------------|
| `num_blocks` | Number of erase blocks |
| `pages_per_block` | Pages per erase block (must be power of two) |
| `page_size` | Page size in bytes |

### `scenarios` — NAND fault conditions

Each scenario configures `nand_fault_sim` before the run. Use a **named preset** or set individual fault fields directly.

**Named presets** (`"preset"` field):

| Preset | Bad blocks | Max erase cycles | Notes |
|--------|-----------|-----------------|-------|
| `FRESH` | 0 | unlimited | No faults; clean baseline |
| `LIGHTLY_USED` | 2 | 10 000 | Low wear |
| `AGED` | 10 | 1 000 | Blocks near half erase budget |
| `FAILING` | 20 | 200 | Elevated failure probabilities |
| `POWER_LOSS` | 0 | unlimited | Torn writes, simulated power cuts |

**Custom fields** (when `"preset"` is omitted or you want to override specific values):

| Field | Type | Description |
|-------|------|-------------|
| `max_erase_cycles` | uint | Block wears out after N erases (0 = unlimited) |
| `max_prog_cycles` | uint | Page wears out after N programs (0 = unlimited) |
| `grave_page_threshold` | uint | prog\_count > N → ECC-uncorrectable on read (0 = off) |
| `prog_fail_prob` | float | Probability [0, 1] that a page program fails |
| `erase_fail_prob` | float | Probability [0, 1] that a block erase fails |
| `read_fail_prob` | float | Probability [0, 1] that a page read fails |
| `copy_fail_prob` | float | Probability [0, 1] that a copy operation fails |
| `crash_after_ops_min/max` | uint | Crash fires after op count in [min, max] |

### `ftl_configs` — FTL parameters

| Field | Description |
|-------|-------------|
| `ftl` | FTL implementation name. Currently: `"dhara"` |
| `gc_factor` | Dhara garbage-collection aggressiveness (higher = more GC overhead, better wear leveling) |

### `workload` — access pattern

| Field | Description |
|-------|-------------|
| `type` | `"sequential"`, `"random"`, or `"mixed"` (70 % writes / 30 % reads) |
| `total_writes` | Total write operations to issue |
| `write_size_bytes` | Size of each write in bytes |

---

## Output — `report.json`

One result entry is produced for every scenario × FTL config combination.

```json
{
  "sweep": "gc_factor_sweep",
  "timestamp": "2026-04-25T14:32:00Z",
  "nand": { "num_blocks": 1024, "pages_per_block": 64, "page_size": 2048 },
  "results": [
    {
      "scenario": "aged",
      "ftl_config": "dhara_gc5",
      "status": "completed",
      "metrics": {
        "writes_attempted": 1000000,
        "writes_succeeded": 998431,
        "write_amplification_factor": 2.34,
        "total_erases": 45231,
        "total_prog_ops": 2340000,
        "bad_blocks_initial": 10,
        "bad_blocks_final": 12,
        "mean_erase_count": 44.2,
        "max_erase_count": 891,
        "min_erase_count": 2,
        "erase_count_stddev": 12.4,
        "erase_count_p50": 43,
        "erase_count_p90": 71,
        "erase_count_p99": 134,
        "erase_count_gini": 0.18,
        "blocks_worn_out": 2,
        "first_worn_out_at_write": 734120,
        "ftl_errors": 3
      }
    }
  ]
}
```

### Key metrics explained

| Metric | What it tells you |
|--------|------------------|
| `write_amplification_factor` | Physical writes ÷ logical writes. The primary FTL quality metric. 1.0 = perfect, higher = more overhead. |
| `writes_succeeded` / `writes_attempted` | How many logical writes completed without error. |
| `total_erases` | Total block-erase operations issued by the FTL. |
| `total_prog_ops` | Total page-program operations (numerator for WAF). |
| `bad_blocks_initial` / `bad_blocks_final` | Bad-block count at start vs. end of the run. |
| `erase_count_p50/p90/p99` | Percentiles of the per-block erase distribution. A P99/P50 ratio close to 1.0 means very uniform wear. |
| `erase_count_gini` | Gini coefficient of erase distribution. 0.0 = perfectly uniform, 1.0 = all wear on one block. |
| `blocks_worn_out` | Blocks that hit `max_erase_cycles` during the run. |
| `first_worn_out_at_write` | Logical write index when the first block wore out — an FTL **lifetime** indicator. |
| `ftl_errors` | Non-OK return codes from FTL read/write/sync calls. |

### Quick analysis with `jq`

```bash
# WAF for every result, sorted ascending
jq '[.results[] | {scenario, ftl_config, waf: .metrics.write_amplification_factor}]
    | sort_by(.waf)' report.json

# Compare Gini coefficients across gc_factor values
jq '.results[] | select(.scenario == "aged") | {ftl_config, gini: .metrics.erase_count_gini}' report.json

# Which configs wore out blocks?
jq '.results[] | select(.metrics.blocks_worn_out > 0)
    | {scenario, ftl_config, worn: .metrics.blocks_worn_out, first_at: .metrics.first_worn_out_at_write}' report.json
```

### Typical interpretation

- **Compare WAF across `gc_factor` values** to find the sweet spot between write overhead and wear uniformity.
- **Low Gini (< 0.2) + low WAF** is the goal. A high Gini with low WAF means wear leveling is doing little work.
- **`first_worn_out_at_write`** gives a rough lifetime estimate: scale it to your real write rate to get years.
- **`ftl_errors > 0` with `status: "completed"`** means the FTL encountered errors but kept running — check whether the workload completed despite them.

---

## Extension points

| What to add | How |
|-------------|-----|
| New FTL (e.g. LittleFS) | Implement `ftl_ops_t` in `main/ftl/`, register name in `main/ftl/ftl.c` |
| New workload pattern | Implement `workload_ops_t` in `main/workload/`, register name in `main/workload/workload.c` |
| New report format (CSV, HTML) | Implement `reporter_t` in `main/reporter/`, swap in `main/main.c` |
| New fault scenario | Add a preset to `nand_fault_sim_config_preset()` or use inline custom JSON fields |
