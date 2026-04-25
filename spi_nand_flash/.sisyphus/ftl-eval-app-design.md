# ftl_eval App Design
_Last updated: 2026-04-25_ _(reviewed 2026-04-25)_

## Goal

A dedicated ESP-IDF linux-target app (`idf.py set-target linux`) for evaluating FTL solutions
(primarily Dhara) using the `nand_fault_sim` + `nand_linux_mmap_emul` infrastructure built in
this branch. Produces structured JSON metrics for comparing multiple FTL configs and NAND
scenarios without being constrained by Catch2's test-runner model.

## Decisions

| Topic | Decision | Rationale |
|---|---|---|
| Build system | ESP-IDF linux target (`set-target linux`) | Stays in IDF ecosystem, reuses components directly |
| Location | `spi_nand_flash/ftl_eval/` | Sits next to `test_app`, `host_test` |
| Config input | JSON via cJSON | Already ships with IDF, zero extra deps, machine-writable for CI |
| Runner model | Multi-config: N scenarios × M ftl_configs, single comparative report | Enables sweep comparison in one run |
| Report output | JSON (cJSON serialize) | Most versatile for post-processing (Python, jq, CI dashboards) |
| Reporter arch | `reporter.h` interface, `reporter_json.c` first impl | Open/close: CSV/HTML reporters addable later |
| Workload arch | `workload_ops_t` interface, writes-only initially | Open/close: new workload types addable without touching runner |
| Workload types | sequential, random, mixed (writes only) | Covers main real-world access patterns |
| FTL abstraction | Fresh C `ftl_ops_t` interface, decoupled from host_test | Clean boundaries, no cross-app coupling |
| nand_fault_sim | Move into `spi_nand_flash` component (Kconfig-gated, linux only) | Reusable infrastructure, host_test + ftl_eval both benefit |
| nand_fault_sim filenames | Keep `nand_fault_sim.c` / `nand_fault_sim.h` on move | No rename — existing `#include` paths in host_test need only a path update |
| dhara_ftl.c in ftl_eval | Standalone C reimplementation of `ftl_ops_t`, no coupling to host_test's C++ | Clean boundary; host_test's `DharaFTL` C++ class stays in host_test |
| Per-block erase counts | `metrics.c` reads `nand_fault_sim_get_erase_count(block)` after run | API already exists; no shadow array needed in metrics layer |
| CLI entry point | `app_main()` with IDF argc/argv mechanism | Consistent with IDF Linux target convention |
| Output path override | `--output` CLI flag overrides `sweep.json` `"output"` field | Enables scripted sweeps without editing sweep.json |

## Data Flow

```
sweep.json (input)  →  ftl_eval app  →  report.json (output)
     ↑                                        ↑
  cJSON parse                          cJSON serialize
```

Runner iterates: `for each scenario × ftl_config → run workload → collect metrics → append result`

## CLI Interface

```
ftl_eval --config sweep.json [--output report.json]
```

- `--config <path>` — required; path to the sweep configuration JSON
- `--output <path>` — optional; overrides the `"output"` field inside `sweep.json`
- Entry point: `app_main()` using the IDF Linux target argc/argv mechanism

## Directory Structure

```
spi_nand_flash/ftl_eval/
├── CMakeLists.txt
├── sdkconfig.defaults              # CONFIG_NAND_FLASH_FAULT_SIM=y etc.
└── main/
    ├── CMakeLists.txt
    ├── main.c                      # entry: parse CLI args, load config, run runner, write report
    ├── config/
    │   ├── app_config.h            # structs: sweep_config_t, scenario_config_t, ftl_config_t
    │   └── app_config.c            # cJSON parse → structs
    ├── runner/
    │   ├── runner.h                # run_suite(), run_single()
    │   └── runner.c                # iterates scenario×ftl_config, drives workload, collects metrics
    ├── workload/
    │   ├── workload.h              # workload_ops_t interface + workload_op_t
    │   ├── workload.c              # dispatcher: "sequential"|"random"|"mixed" → impl
    │   ├── workload_sequential.c
    │   ├── workload_random.c
    │   └── workload_mixed.c
    ├── ftl/
    │   ├── ftl_ops.h               # ftl_ops_t interface
    │   ├── ftl.c                   # dispatcher: "dhara" → impl
    │   └── dhara_ftl.c             # ftl_ops_t impl for Dhara (independent of host_test)
    ├── metrics/
    │   ├── metrics.h               # metrics_t struct, collect/reset/snapshot
    │   └── metrics.c               # WAF calc, percentiles, Gini coefficient
    └── reporter/
        ├── reporter.h              # reporter_t interface (open/close)
        └── reporter_json.c         # first impl: JSON output via cJSON
```

### Component change: nand_fault_sim moves into spi_nand_flash component

```
spi_nand_flash/
├── include/
│   └── nand_fault_sim.h            # moved from host_test/main/
└── src/
    └── nand_fault_sim.c            # moved from host_test/main/
```

- Compiled only when `target == linux && CONFIG_NAND_FLASH_FAULT_SIM=y`
- `host_test` removes its local copies, depends on component instead
- `ftl_eval` gets it for free as a component dependency

## sweep.json Schema

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
    { "name": "fresh", "preset": "FRESH" },
    { "name": "aged",  "preset": "AGED"  },
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

Notes:
- NAND geometry fixed per sweep (not per scenario) — ensures comparability
- Single workload per sweep — same stress applied to all scenario × ftl_config pairs
- `scenarios` supports both named presets (`"preset"`) and fully custom fault configs
- Runner produces one result per scenario × ftl_config combination

### Scenario Presets

Named presets map to `nand_fault_sim_config_preset()` (from `nand_fault_sim.h`):

| Preset name | `nand_sim_scenario_t` | factory bad blocks | `max_erase_cycles` | `max_prog_cycles` | `grave_page_threshold` | Other |
|---|---|---|---|---|---|---|
| `"FRESH"` | `NAND_SIM_SCENARIO_FRESH` | 0 | 0 (unlimited) | 0 (unlimited) | 0 (disabled) | all zeros / no faults |
| `"LIGHTLY_USED"` | `NAND_SIM_SCENARIO_LIGHTLY_USED` | 2 | 10 000 | 0 (unlimited) | 0 (disabled) | — |
| `"AGED"` | `NAND_SIM_SCENARIO_AGED` | 10 | 1 000 | 5 000 | 3 000 | — |
| `"FAILING"` | `NAND_SIM_SCENARIO_FAILING` | 20 | 200 | 400 | 200 | `prog_fail_prob=0.02`, `erase_fail_prob=0.01` |
| `"POWER_LOSS"` | `NAND_SIM_SCENARIO_POWER_LOSS` | 0 | 0 | 0 | 0 | crash range [50, 500], torn writes |

Custom scenario fields (any `nand_fault_sim_config_t` member) may be set directly in JSON when `"preset"` is omitted.

## report.json Schema

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
        "reads_attempted": 500000,
        "reads_succeeded": 499987,
        "writes_attempted": 1000000,
        "writes_succeeded": 998431,
        "write_amplification_factor": 2.34,
        "total_erases": 45231,
        "total_prog_ops": 2340000,
        "bad_blocks_final": 12,
        "bad_blocks_initial": 3,
        "mean_erase_count": 44.2,
        "max_erase_count": 891,
        "min_erase_count": 2,
        "erase_count_stddev": 12.4,
        "erase_count_p50": 43.0,
        "erase_count_p90": 71.2,
        "erase_count_p99": 134.5,
        "erase_count_gini": 0.18,
        "blocks_worn_out": 2,
        "first_worn_out_at_write": 734120,
        "ftl_errors": 3
      }
    }
  ]
}
```

### Metrics explained

| Metric | Description |
|---|---|
| `write_amplification_factor` | total physical writes / logical writes requested. Core FTL quality metric. |
| `reads_attempted` / `reads_succeeded` | Logical read operations issued vs completed without error (non-OK `esp_err_t` = failure) |
| `writes_attempted` / `writes_succeeded` | Logical write operations issued vs completed without error |
| `total_erases` | Total block erase operations performed by FTL |
| `total_prog_ops` | Total page program operations |
| `bad_blocks_*` | Bad block count at start vs end of run |
| `mean/max/min/stddev erase_count` | Basic distribution of per-block erase counts |
| `erase_count_p50/p90/p99` | Percentiles — P99/P50 ratio close to 1.0 = good WL uniformity |
| `erase_count_gini` | 0.0 = perfectly uniform wear, 1.0 = all wear on one block |
| `blocks_worn_out` | Blocks that hit `max_erase_cycles` during the run |
| `first_worn_out_at_write` | Logical write index at which first block wore out — lifetime indicator |
| `ftl_errors` | Non-OK esp_err_t returns from FTL operations |

> **Implementation note — per-block erase counts**: `nand_fault_sim` already exposes
> `nand_fault_sim_get_erase_count(block)` (see `nand_fault_sim.h`). After the workload
> completes, `metrics.c` iterates all blocks, reads their erase counts via this API, and
> computes the distribution (mean, stddev, percentiles, Gini). No shadow array is needed
> in the metrics layer.

## Interfaces

### ftl_ops_t (ftl/ftl_ops.h)

```c
typedef struct {
    esp_err_t (*init)(void *ctx, spi_nand_flash_device_t *nand, const cJSON *ftl_config);
    esp_err_t (*write)(void *ctx, uint32_t sector, const uint8_t *data);
    esp_err_t (*read)(void *ctx, uint32_t sector, uint8_t *data);
    esp_err_t (*sync)(void *ctx);
    esp_err_t (*deinit)(void *ctx);
} ftl_ops_t;
```

### workload_ops_t (workload/workload.h)

```c
typedef struct {
    esp_err_t (*init)(void *ctx, const cJSON *config);
    esp_err_t (*next_op)(void *ctx, workload_op_t *op);
    bool      (*is_done)(void *ctx);
    esp_err_t (*deinit)(void *ctx);
} workload_ops_t;
```

### reporter_t (reporter/reporter.h)

```c
typedef struct {
    esp_err_t (*open)(void *ctx, const char *output_path);
    esp_err_t (*write_result)(void *ctx, const run_result_t *result);
    esp_err_t (*close)(void *ctx);
} reporter_t;
```

## Extension Points Summary

| What to extend | How |
|---|---|
| New FTL solution | Implement `ftl_ops_t`, register name in `ftl.c` dispatcher |
| New workload pattern | Implement `workload_ops_t`, register name in `workload.c` dispatcher |
| New report format | Implement `reporter_t`, swap in `main.c` |
| New fault scenario | Add preset to `nand_fault_sim_config_preset()` or use custom JSON fields |

## Implementation Order (suggested)

1. Move `nand_fault_sim` into `spi_nand_flash` component; update `host_test` to remove local copies
2. Scaffold `ftl_eval` project: CMakeLists, sdkconfig.defaults, empty main.c
3. Implement `config/` — cJSON parse → structs
4. Implement `ftl/` — `ftl_ops_t` + `dhara_ftl.c`
5. Implement `workload/` — sequential first, random + mixed after
6. Implement `metrics/` — counters, WAF, percentiles, Gini
7. Implement `runner/` — outer loop, wires everything together
8. Implement `reporter/reporter_json.c`
9. Wire up `main.c` — CLI args (`--config sweep.json`), run, write report
10. Add example `sweep.json` and verify end-to-end
