# ftl_eval & NAND Fault Simulator — Deep Dive

> **Purpose:** Current-state reference + jumping-off point for building new sweep configs  
> **Focus areas:** Write Amplification Factor (WAF) measurement, ECC page-relief skip behaviour  
> **Audience:** Internal / personal reference  
> **Status as of:** 2026-05-08

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Repository File Map](#2-repository-file-map)
3. [How ftl_eval Works](#3-how-ftl_eval-works)
   - [CLI & Build](#31-cli--build)
   - [Sweep Config Schema (JSON)](#32-sweep-config-schema-json)
   - [Execution Flow](#33-execution-flow)
   - [Workload Types](#34-workload-types)
   - [Output — report.json](#35-output--reportjson)
4. [nand_fault_sim — Implementation Deep Dive](#4-nand_fault_sim--implementation-deep-dive)
   - [Role in the Stack](#41-role-in-the-stack)
   - [Internal State](#42-internal-state)
   - [Feature Map: What Can Be Simulated](#43-feature-map-what-can-be-simulated)
   - [Full Config Reference (nand_fault_sim_config_t)](#44-full-config-reference-nand_fault_sim_config_t)
   - [Built-in Presets](#45-built-in-presets)
   - [ECC Models in Detail](#46-ecc-models-in-detail)
   - [Page-Relief (ECC Skip) Flow](#47-page-relief-ecc-skip-flow)
   - [Power-Loss / Crash Simulation](#48-power-loss--crash-simulation)
   - [Limitations & Known Gaps](#49-limitations--known-gaps)
5. [Metrics Reference](#5-metrics-reference)
   - [Write Amplification Factor](#51-write-amplification-factor)
   - [Full Metrics Schema](#52-full-metrics-schema)
6. [Existing JSON Sweep Configs](#6-existing-json-sweep-configs)
   - [sweep.json — GC Overhead Sweep](#61-sweepjson--gc-overhead-sweep)
   - [page_relief.json — Page Relief Threshold Sweep](#62-page-reliefjson--page-relief-threshold-sweep)
   - [ecc_noise_bump.json — ECC Noise Probability Sweep](#63-ecc_noise_bumpjson--ecc-noise-probability-sweep)
7. [Real Run Data from report.json](#7-real-run-data-from-reportjson)
8. [Pros and Cons Assessment](#8-pros-and-cons-assessment)
9. [Modelling Capabilities & What We Can Build Now](#9-modelling-capabilities--what-we-can-build-now)
   - [What is Modellable Today](#91-what-is-modellable-today)
   - [Gap Analysis (noted, not proposed)](#92-gap-analysis-noted-not-proposed)
10. [Jumping-Off Configs to Write Next](#10-jumping-off-configs-to-write-next)
    - [WAF Sweep Ideas](#101-waf-sweep-ideas)
    - [ECC Page-Relief Skip Sweep Ideas](#102-ecc-page-relief-skip-sweep-ideas)
11. [jq Cheatsheet for Report Analysis](#11-jq-cheatsheet-for-report-analysis)

---

## 1. Architecture Overview

```
ftl_eval.elf --config sweep.json
      │
      ▼
  app_config_parse()          ← JSON sweep → sweep_config_t
      │
      ▼
  run_suite()
  ├── for each scenario × ftl_config:
  │       run_single()
  │       ├── build_fault_config()    ← scenario_config_t → nand_fault_sim_config_t
  │       ├── nand_fault_sim_init()   ← allocates per-block/page counters, seeds PRNGs
  │       ├── nand_file_mmap_emul     ← file-backed NAND backing (Linux mmap)
  │       ├── spi_nand_flash_init_with_layers()   ← BDL + Dhara FTL
  │       ├── workload loop           ← sequential / random / mixed ops
  │       │       ├── esp_blockdev write/read
  │       │       └── metrics_record_*()
  │       ├── metrics_collect_*()     ← pull erase/prog counts from simulator
  │       └── (optional) run_recovery_check()
  │
  └── reporter_json  →  report.json

nand_fault_sim replaces nand_impl_linux.c at link time.
Non-faulted paths delegate to nand_emul_* (nand_linux_mmap_emul.c).
```

**Layer stack (Linux target):**

| Layer | File | Role |
|---|---|---|
| FTL (Dhara) | vendored | Maps logical sectors → physical pages, GC |
| Wear-leveling BDL | `nand_wl_bdl` | Bad-block management, wear stats |
| NAND Flash BDL | `nand_flash_bdl` | Physical read/write/erase |
| Fault Simulator | `nand_fault_sim.c` | Injects failures, tracks wear counts |
| Emulation | `nand_linux_mmap_emul.c` | File-backed NAND storage |

---

## 2. Repository File Map

### ftl_eval app

```
ftl_eval/
├── CMakeLists.txt                  Top-level CMake (Linux target, BDL=y, fault sim=y)
├── README.md                       User guide: build, CLI, sweep schema, output format
├── sdkconfig                       Generated sdkconfig for the build
│
├── sweep.json                      Default sweep: 3 scenarios × 4 GC overheads, random workload
├── page_relief.json                ECC write-wear threshold sweep (6 scenarios)
├── ecc_noise_bump.json             ECC noise probability sweep (4 scenarios)
│
├── report.json                     Example output for sweep.json run (real data)
├── report_page_relief.json         Example output for page_relief.json run
├── report_ecc_noise_bump.json      Example output for ecc_noise_bump.json run
│
└── main/
    ├── main.c                      Entry: CLI parsing, JSON load, run_suite, reporter
    │
    ├── config/
    │   ├── app_config.h            sweep_config_t, scenario_config_t, ftl_config_t structs
    │   └── app_config.c            cJSON → sweep_config_t parser (all sweep JSON fields)
    │
    ├── runner/
    │   ├── runner.h                run_single(), run_suite(), run_result_t
    │   └── runner.c                Core: build_fault_config, init stack, drive workload, metrics
    │
    ├── metrics/
    │   ├── metrics.h               metrics_t struct + all record/collect function prototypes
    │   └── metrics.c               WAF calculation, Gini coefficient, percentiles, ECC counters
    │
    ├── workload/
    │   ├── workload.h              workload_op_t, workload_ops_t interface, workload_create()
    │   ├── workload.c              Factory: maps type string → workload_ops_t
    │   ├── workload_sequential.c   Sequential sector sweep
    │   ├── workload_random.c       Uniform random sector selection
    │   ├── workload_mixed.c        70% writes / 30% reads, random sectors
    │   ├── workload_zipf.c         Zipf distribution helper for hot/cold workloads
    │   └── workload_read_loop.c    Read-heavy loop helper
    │
    └── reporter/
        ├── reporter.h              reporter_t interface (open/write_result/close)
        └── reporter_json.c         Writes the final report JSON via cJSON

test/
├── CMakeLists.txt                  Catch2-based test component
├── test_main.cpp                   Catch2 session main
├── test_fault_sim.cpp              Unit tests: op-fail probs, crash/torn-write, ECC escalation
├── test_ftl_robustness.cpp         FTL robustness under fault injection
├── test_page_relief.cpp            Page-relief callback and skip counting tests
├── test_app_config.cpp             JSON parser tests
├── dhara_ftl.cpp                   Dhara FTL adapter for tests
└── ftl_interface.hpp               Test harness FTL interface
```

### NAND Fault Simulator (component level)

```
include/
└── nand_fault_sim.h                Public API: config struct, presets, lifecycle, stats

src/
└── nand_fault_sim.c                Full implementation:
                                      - nand_impl shim (9 symbols replacing nand_impl_linux.c)
                                      - per-block erase_count[], per-page prog/read_count[]
                                      - probabilistic op-failure PRNG (rand_r, seeded)
                                      - crash / torn-write / torn-erase logic
                                      - ECC write-wear model (nand_get_ecc_status)
                                      - ECC read-disturb model (in nand_read)
                                      - factory bad-block list (in-memory)
                                      - preset configurations (5 scenarios)
```

### Supporting files

```
host_test/
├── CMakeLists.txt                  Host test project CMake
├── sdkconfig.defaults              Sets linux target, fault sim, BDL
├── main/
│   ├── test_app_main.cpp           Catch2 main for host tests
│   ├── test_nand_flash.cpp         Legacy path tests
│   ├── test_nand_flash_bdl.cpp     BDL path tests
│   └── test_nand_flash_ftl.cpp     FTL-level tests

docs/plans/
└── 2026-05-08-move-fault-sim-tests-out-of-host-test.md   Reorganisation plan

fault-sim-ftl-eval-improvements.md                        Design notes
```

---

## 3. How ftl_eval Works

### 3.1 CLI & Build

**Build (requires ESP-IDF 6.0+, Linux target):**
```bash
cd spi_nand_flash/ftl_eval
get_idf          # interactive shell alias
idf.py build
```

`sdkconfig.defaults` automatically enables:
- `CONFIG_IDF_TARGET="linux"`
- `CONFIG_NAND_FLASH_FAULT_SIM=y`
- `CONFIG_NAND_FLASH_ENABLE_BDL=y`
- `CONFIG_SPI_NAND_FLASH_WL_NVBLOCK=y`

**Run:**
```bash
./build/ftl_eval.elf --config sweep.json [--output report.json]
```

| Flag | Required | Description |
|------|----------|-------------|
| `--config <path>` | **Yes** | Sweep JSON to read |
| `--output <path>` | No | Overrides the `"output"` field in the sweep file |

### 3.2 Sweep Config Schema (JSON)

A sweep file defines one matrix: **scenarios × ftl_configs**, all sharing the same geometry and workload.

**Top-level fields:**

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Human label (appears in report) |
| `output` | string | Output JSON path (overridable via CLI) |
| `nand` | object | NAND geometry (fixed per sweep) |
| `scenarios` | array | One or more fault scenarios |
| `ftl_configs` | array | One or more FTL parameter sets |
| `workload` | object | Access pattern and volume |

**`nand` geometry:**

| Field | Type | Description |
|-------|------|-------------|
| `num_blocks` | uint | Number of erase blocks |
| `pages_per_block` | uint | Pages per block (power of two) |
| `page_size` | uint | Page size in bytes |

**`ftl_configs` fields:**

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Label for this FTL config |
| `ftl` | string | FTL implementation — currently only `"dhara"` |
| `gc_overhead_percent` | float | Max GC overhead as % of logical capacity. Lower → less WAF headroom, more variance |

**`workload` fields:**

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | `"sequential"`, `"random"`, or `"mixed"` |
| `total_writes` | uint | Total write operations to issue |
| `write_size_bytes` | uint | Size of each write in bytes |

**`scenarios` fields (full list — all optional except `name`):**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | string | — | Required label |
| `preset` | string | — | Named preset (`FRESH`, `LIGHTLY_USED`, `AGED`, `FAILING`, `POWER_LOSS`). Overrides all other fields |
| `factory_bad_block_count` | uint | 0 | Number of factory bad blocks (auto-assigned incrementally) |
| `max_erase_cycles` | uint | 0 | Block wears out after N erases (0 = unlimited) |
| `pre_warm_erase_cycles` | uint | 0 | Pre-populate all erase counts to N at init |
| `max_prog_cycles` | uint | 0 | Page wears out after N programs (0 = unlimited) |
| `grave_page_threshold` | uint | 0 | prog_count > N → ECC-uncorrectable on read |
| `read_fail_prob` | float | 0.0 | Probability per read of failure |
| `prog_fail_prob` | float | 0.0 | Probability per program of failure |
| `erase_fail_prob` | float | 0.0 | Probability per erase of failure |
| `copy_fail_prob` | float | 0.0 | Probability per copy of failure |
| `copy_ecc_fail_prob` | float | 0.0 | Like `copy_fail_prob` but sets ECC status to NOT_CORRECTED |
| `op_fail_seed` | uint | 0 | PRNG seed for per-op failures |
| `crash_after_ops_min` | uint | 0 | Crash fires after op count in [min, max] |
| `crash_after_ops_max` | uint | 0 | Upper bound; == min for deterministic single crash point |
| `crash_probability` | float | 0.0 | Per-op crash probability (mutually exclusive with range mode) |
| `crash_seed` | uint | 0 | Seed for crash point + torn-write offset |
| `ecc_data_refresh_threshold` | uint8 | 0 (→ 4) | ECC severity level at which page-relief fires |
| `ecc_mid_threshold` | uint | 0 | Reads before ECC_1_TO_3_BITS_CORRECTED (read-disturb) |
| `ecc_high_threshold` | uint | 0 | Reads before ECC_4_TO_6_BITS_CORRECTED (read-disturb) |
| `ecc_fail_threshold` | uint | 0 | Reads before ECC_NOT_CORRECTED (read-disturb) |
| `ecc_prog_mid_erase_threshold` | uint | 0 | Block erases before ECC_1_TO_3_BITS_CORRECTED (write-wear) |
| `ecc_prog_high_erase_threshold` | uint | 0 | Block erases before ECC_4_TO_6_BITS_CORRECTED (write-wear) |
| `ecc_prog_fail_erase_threshold` | uint | 0 | Block erases before ECC_NOT_CORRECTED (write-wear) |
| `ecc_prog_noise_prob` | float | 0.0 | Per-page probability of bumping ECC one level (noise) |
| `recovery_check` | bool | false | Re-mount after crash and verify data integrity |

### 3.3 Execution Flow

```
run_suite()
  for (scenario, ftl_config):
    run_single():
      1. build_fault_config()          map scenario JSON → nand_fault_sim_config_t
      2. nand_fault_sim_init()         allocate erase_count[num_blocks], prog/read_count[num_pages]
      3. nand_file_mmap_emul_init()    open/create backing file
      4. spi_nand_flash_init_with_layers()  mount BDL + Dhara FTL
      5. metrics_collect_bad_blocks(initial)
      6. workload loop:
           while (!workload.is_done):
             op = workload.next_op()
             esp_blockdev_write/read()       ← hits fault sim
             metrics_record_write/read()
             check nand_fault_sim_is_crashed()
      7. metrics_collect_erase_stats()   query nand_fault_sim_get_erase_count()
      8. metrics_collect_prog_stats()    query nand_fault_sim_get_prog_count()
      9. metrics_collect_bad_blocks(final)
     10. compute WAF = total_prog_ops / writes_succeeded
     11. (optional) run_recovery_check()
     12. nand_fault_sim_deinit()
     13. delete backing file
```

### 3.4 Workload Types

| Type | Behaviour | Best for |
|------|-----------|----------|
| `sequential` | Writes sectors 0 → N in order, wraps | Best-case WAF baseline |
| `random` | Uniform random sector selection | Realistic workload, drives GC |
| `mixed` | 70% writes / 30% reads, random | Read-write mix |

Each workload implements the `workload_ops_t` interface:
- `init(ctx, cJSON *config)` — parse workload params from the same JSON subtree
- `next_op(ctx, op)` — produce the next read/write op
- `is_done(ctx)` — signals completion
- `deinit(ctx)` — free resources

The workload config JSON is built from `sweep.json`'s `workload` object and forwarded as cJSON to the selected implementation.

### 3.5 Output — report.json

One result entry per scenario × ftl_config combination.

```json
{
  "sweep": "my_sweep_name",
  "timestamp": "2026-05-08T08:24:31Z",
  "nand": { "num_blocks": 128, "pages_per_block": 64, "page_size": 2048 },
  "results": [
    {
      "scenario": "aged",
      "ftl_config": "dhara_20pct",
      "status": "completed",
      "metrics": { ... }
    }
  ]
}
```

`status` values: `"completed"` | `"failed"` (FTL gave up / device ran out of space).

---

## 4. nand_fault_sim — Implementation Deep Dive

### 4.1 Role in the Stack

`nand_fault_sim.c` **replaces** `nand_impl_linux.c` at link time by providing definitions of all 9 `nand_impl` symbols (`nand_init_device`, `nand_read`, `nand_prog`, `nand_erase_block`, `nand_copy`, `nand_is_bad`, `nand_mark_bad`, `nand_is_free`, `nand_get_ecc_status`). Non-faulted operations delegate to `nand_emul_*` (the mmap backing file layer).

It is a **singleton** (`static nand_sim_state_t s_sim`) — not thread-safe by design (host tests are single-threaded).

### 4.2 Internal State

```c
typedef struct {
    nand_fault_sim_config_t cfg;       // copy of user config
    uint32_t  num_blocks;
    uint32_t  num_pages;
    uint32_t  pages_per_block;
    uint32_t *erase_count;             // [num_blocks]
    uint32_t *prog_count;              // [num_pages]
    uint32_t *read_count;              // [num_pages]
    bool      crashed;
    uint32_t  op_counter;
    uint32_t  crash_point;             // derived from [min,max] range at init
    unsigned int op_fail_state;        // rand_r PRNG state for op failures
    unsigned int crash_state;          // rand_r PRNG state for crash + torn offsets
    bool      initialized;
} nand_sim_state_t;
```

`pre_warm_erase_cycles > 0` copies that value into all `erase_count[]` at init, letting write-wear ECC thresholds fire immediately without running a burn-in loop.

### 4.3 Feature Map: What Can Be Simulated

| Feature | Mechanism | Config fields |
|---------|-----------|---------------|
| Factory bad blocks | In-memory list checked by `nand_is_bad` | `factory_bad_blocks`, `factory_bad_block_count` |
| Block erase wear-out | `erase_count[block]++ ≥ max_erase_cycles → fail` | `max_erase_cycles` |
| Page program wear-out | `prog_count[page]++ ≥ max_prog_cycles → fail` | `max_prog_cycles` |
| Retention / grave pages | `prog_count > grave_page_threshold → ECC_NOT_CORRECTED on read` | `grave_page_threshold` |
| Probabilistic read failure | `rand_r < read_fail_prob` | `read_fail_prob`, `op_fail_seed` |
| Probabilistic prog failure | `rand_r < prog_fail_prob` | `prog_fail_prob`, `op_fail_seed` |
| Probabilistic erase failure | `rand_r < erase_fail_prob` | `erase_fail_prob`, `op_fail_seed` |
| Probabilistic copy failure | `rand_r < copy_fail_prob` | `copy_fail_prob`, `op_fail_seed` |
| Copy failure with ECC status | Like above, but sets `ecc_corrected_bits_status = NOT_CORRECTED` | `copy_ecc_fail_prob` |
| Power-loss crash (range) | Deterministic crash after op count in [min, max] | `crash_after_ops_min/max`, `crash_seed` |
| Power-loss crash (probabilistic) | Per-op probability of crash | `crash_probability`, `crash_seed` |
| Torn erase | On crash in `nand_erase_block`: clears random prefix of pages | automatic when crash fires in erase |
| Torn write | On crash in `nand_prog`: writes random byte prefix only | automatic when crash fires in prog |
| ECC read-disturb | `read_count[page]` vs thresholds → `on_page_read_ecc` callback | `ecc_mid/high/fail_threshold` |
| ECC write-wear | `erase_count[block]` vs thresholds → sets `ecc_corrected_bits_status` | `ecc_prog_*_erase_threshold` |
| ECC noise jitter | Per-page probability of bumping ECC one level | `ecc_prog_noise_prob` |
| Pre-warmed flash | Pre-populate erase counts to simulate used device | `pre_warm_erase_cycles` |

### 4.4 Full Config Reference (nand_fault_sim_config_t)

See [`include/nand_fault_sim.h`](../include/nand_fault_sim.h) for full Doxygen. Summary:

**Factory bad blocks**
```c
const uint32_t *factory_bad_blocks;     // array of block indices (static, not freed)
uint32_t        factory_bad_block_count;
```

**Wear limits**
```c
uint32_t max_erase_cycles;              // 0 = unlimited
uint32_t pre_warm_erase_cycles;         // 0 = off; fills all erase_count[] at init
uint32_t max_prog_cycles;               // 0 = unlimited
uint32_t grave_page_threshold;          // 0 = off; prog_count > N → ECC fail on read
```

**Per-op probabilistic failures (PRNG seeded by op_fail_seed)**
```c
float read_fail_prob;    // [0.0, 1.0]
float prog_fail_prob;
float erase_fail_prob;
float copy_fail_prob;
float copy_ecc_fail_prob; // copy fail + ECC_NOT_CORRECTED status
unsigned int op_fail_seed;
```

**Power-loss crash**
```c
uint32_t crash_after_ops_min;  // 0 = disabled
uint32_t crash_after_ops_max;  // == min for single deterministic point
float    crash_probability;    // per-op; mutually exclusive with range mode
unsigned int crash_seed;       // seed for crash point + torn offsets
```

**ECC alignment**
```c
uint8_t ecc_data_refresh_threshold; // 0 → default 4 (NAND_ECC_4_TO_6_BITS_CORRECTED)
                                    // must match CONFIG_NAND_FLASH_PROG_PAGE_RELIEF_MIN_ECC
```

**ECC read-disturb (per-page read count thresholds)**
```c
uint32_t ecc_mid_threshold;   // reads before ECC_1_TO_3_BITS_CORRECTED
uint32_t ecc_high_threshold;  // reads before ECC_4_TO_6_BITS_CORRECTED
uint32_t ecc_fail_threshold;  // reads before ECC_NOT_CORRECTED
```

**ECC write-wear (per-block erase count thresholds)**
```c
uint32_t ecc_prog_mid_erase_threshold;  // erases before ECC_1_TO_3_BITS
uint32_t ecc_prog_high_erase_threshold; // erases before ECC_4_TO_6_BITS
uint32_t ecc_prog_fail_erase_threshold; // erases before ECC_NOT_CORRECTED
float    ecc_prog_noise_prob;           // per-page probability of +1 ECC level
```

**Callbacks**
```c
nand_fault_sim_ecc_cb_t on_page_read_ecc; // read-disturb events
void                   *ecc_cb_ctx;
nand_fault_sim_ecc_cb_t on_page_relief;   // write-wear events (pre-prog ECC check)
void                   *page_relief_cb_ctx;
```

### 4.5 Built-in Presets

| Preset | Bad blocks | max_erase_cycles | pre_warm | Notes |
|--------|-----------|-----------------|----------|-------|
| `FRESH` | 0 | 0 (unlimited) | 0 | Clean baseline |
| `LIGHTLY_USED` | 2 | 10 000 | — | Low wear |
| `AGED` | 10 | 1 000 | — | Near half erase budget; ecc_prog thresholds set; page relief fires |
| `FAILING` | 20 | 200 | — | Elevated `prog/erase_fail_prob`; many bad blocks |
| `POWER_LOSS` | 0 | 0 | — | `crash_after_ops_min=50`, `max=500`; torn writes |

`AGED` preset activates the write-wear ECC model (`ecc_prog_mid/high_erase_threshold` set). This is why `page_relief_skips` is non-zero for `aged` runs but zero for `fresh`.

### 4.6 ECC Models in Detail

The simulator has **two independent ECC models**:

#### Write-Wear ECC (the main page-relief driver)

**Trigger:** `nand_get_ecc_status()` — called by the driver before every `nand_prog()` and `nand_copy()` when `CONFIG_NAND_FLASH_PROG_PAGE_RELIEF` is enabled.

**Logic:**
```
erase_count[block] → compare against thresholds:
  ≥ ecc_prog_fail_erase_threshold → ECC_NOT_CORRECTED
  ≥ ecc_prog_high_erase_threshold → ECC_4_TO_6_BITS_CORRECTED
  ≥ ecc_prog_mid_erase_threshold  → ECC_1_TO_3_BITS_CORRECTED
  else                             → ECC_OK

+ noise bump: if rand_r < ecc_prog_noise_prob → status += 1 (capped at NOT_CORRECTED)
```

If the resulting status is ≥ `ecc_data_refresh_threshold`, the driver fires the `on_page_relief` callback (metrics: `page_relief_skips++`) and returns `ESP_ERR_SPI_NAND_PAGE_RELIEF` to the caller — **the program is skipped** (the page is not written). The wrapper layer (`nand_impl_wrap.c`) converts this to `ESP_OK` so the FTL sees a "success" that produced no physical write.

#### Read-Disturb ECC

**Trigger:** Inside `nand_read()`, after incrementing `read_count[page]`.

**Logic:**
```
read_count[page] vs thresholds → fire on_page_read_ecc(page, status, ctx)
grave page: prog_count[page] > grave_page_threshold → ECC_NOT_CORRECTED (always)
```

This does **not** skip the operation — it only fires the callback. The metrics module increments `ecc_mid_events`, `ecc_high_events`, or `ecc_fail_events`. The data in the backing file is unchanged; the simulator only signals severity.

### 4.7 Page-Relief (ECC Skip) Flow

```
FTL wants to write page P
    ↓
nand_prog() / nand_copy()
    ↓
nand_get_ecc_status(handle, page)          ← write-wear model
    ↓
erase_count[block] ≥ threshold?
    YES → set ecc_corrected_bits_status = HIGH/MID/FAIL
          fire on_page_relief() callback   → metrics.page_relief_skips++
          return ESP_ERR_SPI_NAND_PAGE_RELIEF
    NO  → proceed with actual write
    ↓
nand_impl_wrap.c intercepts ESP_ERR_SPI_NAND_PAGE_RELIEF
    → logs "page relief skip"
    → returns ESP_OK to BDL/FTL
```

**Result:** The FTL thinks the write succeeded. The physical page was **not** written. This reduces WAF (fewer physical progs) but means some logical → physical mappings were not committed. The `page_relief_first_at_write` metric records when the first skip occurred.

**Important:** Page relief requires:
1. `CONFIG_NAND_FLASH_PROG_PAGE_RELIEF=y` (set in sdkconfig.defaults)
2. `ecc_prog_high_erase_threshold` (or similar) configured above 0
3. `ecc_data_refresh_threshold` consistent with `CONFIG_NAND_FLASH_PROG_PAGE_RELIEF_MIN_ECC`

Without these, `page_relief_skips` will always be 0.

### 4.8 Power-Loss / Crash Simulation

**Range mode** (`crash_after_ops_min/max`): a crash point is derived deterministically from the PRNG at init. The crash fires on the first op where `op_counter >= crash_point`.

**Probabilistic mode** (`crash_probability`): per-op roll; first hit crashes.

**What "crash" does:**
- Sets `s_sim.crashed = true` — further ops return `ESP_FAIL` immediately
- In `nand_erase_block`: tears the erase by clearing a random prefix of pages (simulates incomplete erase)
- In `nand_prog`: tears the write by writing only a random byte prefix (simulates partial page program)

**Recovery check** (`recovery_check: true` in JSON): after crash detection, `run_recovery_check()` re-inits the sim with crash disabled, re-mounts the BDL/FTL, and reads back every written sector to verify data consistency.

### 4.9 Limitations & Known Gaps

| Gap | Impact | Notes |
|-----|--------|-------|
| Singleton (`s_sim`) — single device only | Cannot simulate multi-device configurations | Not relevant for current ESP NAND use case |
| Not thread-safe | Tests must be single-threaded | Documented; no plans to change |
| Read-disturb model does not modify data | ECC events are signalled but data is never corrupted | Realistic for most test scenarios; actual data corruption from read-disturb is extreme |
| ECC read-disturb does not trigger page relief | Only write-wear model feeds `on_page_relief`; read-disturb fires `on_page_read_ecc` only | TODO: may want scrubbing path to consume read-disturb events |
| No wear-leveling visibility per-block | Metrics report erase distribution but simulator doesn't expose hot-spot block list | Could add, but `nand_fault_sim_get_erase_count()` can be iterated |
| Only Dhara FTL supported in ftl_eval | `ftl_configs` accepts only `"dhara"` | Extensible via `workload_ops_t`-style interface |
| No read workload metrics | `reads_attempted/succeeded` tracked but not driven by any workload type by default | `mixed` workload issues reads; `sequential`/`random` are write-only |
| `total_erases` always 0 in report | Bug / known gap — erase count collected from sim, but `total_erases` in metrics is only incremented by direct erase calls visible to BDL, not from fault-sim counters | Check `metrics_collect_erase_stats` vs `metrics.total_erases` |
| No multi-plane or cache program simulation | Single-page prog model only | Acceptable for current chip portfolio |

---

## 5. Metrics Reference

### 5.1 Write Amplification Factor

```
WAF = total_prog_ops / writes_succeeded
```

- `total_prog_ops` = total physical page-program operations issued to the NAND (from `nand_fault_sim_get_prog_count()` summed across all pages)
- `writes_succeeded` = logical write ops that returned success to the workload

**Interpretation:**
- `WAF = 1.0` — every logical write maps to exactly 1 physical program (ideal, impossible with GC)
- `WAF = 1.25` — typical for fresh device with 25% GC overhead, sequential workload
- `WAF = 1.3–1.6` — typical for random workload depending on GC overhead %
- `WAF > 2.0` — high wear, many GC cycles, or large GC overhead configuration

**Effect of page-relief skips on WAF:**  
Relief skips **reduce** WAF (the physical write was suppressed). This is visible in `aged` scenario data: higher `page_relief_skips` correlates with slightly lower-than-expected WAF. The effect is a nuance to track when interpreting results.

### 5.2 Full Metrics Schema

```c
typedef struct {
    // I/O counts
    uint32_t reads_attempted;
    uint32_t reads_succeeded;
    uint32_t writes_attempted;
    uint32_t writes_succeeded;

    // Write amplification
    double   write_amplification_factor;  // total_prog_ops / writes_succeeded
    uint32_t total_erases;               // ⚠ see gap note above
    uint32_t total_prog_ops;             // sum of nand_fault_sim_get_prog_count()

    // Bad blocks
    uint32_t bad_blocks_initial;         // at start of run
    uint32_t bad_blocks_final;           // at end of run

    // Erase distribution (wear uniformity)
    double   mean_erase_count;
    uint32_t max_erase_count;
    uint32_t min_erase_count;
    double   erase_count_stddev;
    uint32_t erase_count_p50;
    uint32_t erase_count_p90;
    uint32_t erase_count_p99;
    double   erase_count_gini;           // 0=perfectly uniform, 1=all wear on one block

    // Lifetime
    uint32_t blocks_worn_out;            // blocks that hit max_erase_cycles
    uint32_t first_worn_out_at_write;    // logical write index of first wear-out

    // Errors
    uint32_t ftl_errors;                 // non-OK return codes from FTL

    // ECC read-disturb events (from on_page_read_ecc callback)
    uint32_t ecc_mid_events;             // ECC_1_TO_3_BITS_CORRECTED
    uint32_t ecc_high_events;            // ECC_4_TO_6_BITS_CORRECTED
    uint32_t ecc_fail_events;            // ECC_NOT_CORRECTED

    // Page-relief skip events (from on_page_relief callback)
    uint32_t page_relief_skips;          // programs skipped due to elevated ECC status
    uint32_t page_relief_first_at_write; // writes_attempted when first skip fired
} metrics_t;
```

---

## 6. Existing JSON Sweep Configs

### 6.1 sweep.json — GC Overhead Sweep

**Intent:** Measure WAF and wear distribution as a function of GC overhead (Dhara `gc_factor`).

```json
{
  "nand": { "num_blocks": 128, "pages_per_block": 64, "page_size": 2048 },
  "scenarios": [
    { "name": "fresh",   "preset": "FRESH"   },
    { "name": "aged",    "preset": "AGED"    },
    { "name": "failing", "preset": "FAILING" }
  ],
  "ftl_configs": [
    { "ftl": "dhara", "gc_overhead_percent": 25 },
    { "ftl": "dhara", "gc_overhead_percent": 20 },
    { "ftl": "dhara", "gc_overhead_percent": 15 },
    { "ftl": "dhara", "gc_overhead_percent": 10 }
  ],
  "workload": { "type": "random", "total_writes": 100000, "write_size_bytes": 2048 }
}
```

**What it shows:** How GC overhead % trades off against WAF and Gini. Lower `gc_overhead_percent` → higher WAF (more GC work per logical write) → more uniform wear.

**What it doesn't show:** ECC escalation, page-relief behaviour (fresh/failing presets either don't have ecc thresholds or they're not the focus).

---

### 6.2 page_relief.json — Page Relief Threshold Sweep

**Intent:** Find the `ecc_prog_high_erase_threshold` value at which page-relief starts firing, and measure the effect on WAF and skips.

```json
"scenarios": [
  { "name": "fresh_no_relief", "preset": "FRESH" },
  { "name": "relief_threshold_1", "pre_warm_erase_cycles": 50, "ecc_prog_high_erase_threshold": 1, "ecc_prog_noise_prob": 0.01 },
  { "name": "relief_threshold_2", "pre_warm_erase_cycles": 50, "ecc_prog_high_erase_threshold": 2, ... },
  ...up to threshold_5
]
```

**Key technique:** `pre_warm_erase_cycles: 50` pre-ages all blocks so the threshold fires from write 1. Noise prob `0.01` adds realistic page-to-page BER variation.

**What it shows:** Sensitivity of `page_relief_skips` to threshold value, and the threshold at which relief starts suppressing writes.

---

### 6.3 ecc_noise_bump.json — ECC Noise Probability Sweep

**Intent:** Measure how ECC noise probability (`ecc_prog_noise_prob`) affects write-wear ECC event frequency and WAF.

```json
"scenarios": [
  { "name": "baseline_no_noise", ..., "ecc_prog_noise_prob": 0.0  },
  { "name": "noise_low_prob",    ..., "ecc_prog_noise_prob": 0.01 },
  { "name": "noise_high_prob",   ..., "ecc_prog_noise_prob": 0.25 },
  { "name": "noise_above_high_threshold", "pre_warm_erase_cycles": 500, ..., "ecc_prog_noise_prob": 0.15 }
]
```

Sets explicit thresholds:
- `ecc_prog_mid_erase_threshold: 400`
- `ecc_prog_high_erase_threshold: 700`
- `ecc_prog_fail_erase_threshold: 1800`
- `max_erase_cycles: 2000`
- `ecc_data_refresh_threshold: 4`

**What it shows:** How noise probability amplifies ECC events when blocks are near or above the mid threshold.

---

## 7. Real Run Data from report.json

From the `gc_overhead_sweep` run (100k random writes, 128 blocks × 64 pages × 2048 bytes):

| scenario | gc_overhead | WAF | page_relief_skips | page_relief_first_at | ftl_errors | status |
|----------|------------|-----|-------------------|---------------------|-----------|--------|
| fresh | 25% | **1.250** | 0 | — | 0 | completed |
| fresh | 20% | **1.307** | 0 | — | 0 | completed |
| fresh | 15% | **1.417** | 0 | — | 0 | completed |
| fresh | 10% | **1.574** | 0 | — | 0 | completed |
| aged | 25% | **1.271** | 14 339 | write #4 153 | 6 211 | completed |
| aged | 20% | **1.334** | 18 810 | write #4 504 | 5 928 | completed |
| aged | 15% | **1.459** | 27 473 | write #4 906 | 5 792 | completed |
| aged | 10% | **1.642** | 40 534 | write #5 206 | 5 745 | completed |
| failing | 25% | **1.205** | 726 | write #20 | 97 744 | **failed** |
| failing | 20% | **1.104** | 241 | write #52 | 96 968 | completed |
| failing | 15% | **1.178** | 604 | write #53 | 97 509 | **failed** |
| failing | 10% | **1.131** | 403 | write #151 | 96 931 | **failed** |

**Notable observations:**
1. `fresh` — zero page-relief skips (AGED preset sets ECC thresholds; FRESH does not)
2. `aged` — page relief fires early (~write 4k–5k) and accumulates massively (14k–40k skips). The relief skip count scales with lower GC overhead % (more physical GC writes → more blocks hit thresholds faster)
3. `failing` — WAF is artificially low because most writes fail (`writes_succeeded` ~2.5–3k out of 100k attempted). Low WAF in a failed run is not a good thing.
4. Erase count stats are all 0 for `fresh` — Dhara only erases blocks during GC which only triggers when space is needed; for a short fresh run GC may not have fired.

---

## 8. Pros and Cons Assessment

### Pros

| Strength | Detail |
|----------|--------|
| **Fully deterministic** | Seeded PRNGs (`op_fail_seed`, `crash_seed`) → exact reproduction of any run |
| **No hardware required** | Runs on Linux host in <1s per scenario |
| **Rich fault modelling** | Covers bad blocks, wear-out, probabilistic failures, power-loss, ECC escalation |
| **Two orthogonal ECC models** | Write-wear (erase-count-driven) and read-disturb (read-count-driven) independently configurable |
| **Pre-warm support** | `pre_warm_erase_cycles` skips burn-in; can test a "worn" device from write 1 |
| **Noise injection** | `ecc_prog_noise_prob` adds realistic page-to-page BER variation |
| **Metrics are comprehensive** | WAF, Gini, percentiles, ECC events, relief skips, lifetime — all in one JSON |
| **JSON-driven sweeps** | No recompilation needed to test a new scenario; just edit the JSON |
| **Recovery verification** | `recovery_check: true` actually re-mounts and verifies data after crash |
| **Extensible** | New workloads, reporters, FTL backends via clean interfaces |

### Cons

| Weakness | Detail |
|----------|--------|
| **Singleton** | Only one simulated device per process; no multi-device scenarios |
| **`total_erases` metric is 0** | BDL erase count not plumbed into metrics correctly (known gap) |
| **Read-disturb doesn't feed page-relief** | Read ECC events fire callback but do not suppress writes / trigger scrubbing |
| **No temporal modelling** | All stress is uniform over time; cannot simulate bursty write patterns or idle-time data retention |
| **Write-wear threshold is block-granular** | Real NAND has page-granular retention variance within a block; simulator uses one erase count per block |
| **Workload types are basic** | Sequential, random, mixed — no Zipf-driven (despite file existing), no filesystem-style access patterns |
| **Only Dhara FTL** | `ftl_configs` has no other implementation to compare against |
| **No real chip register/OOB simulation** | OOB is used only for bad-block markers; no ECC byte simulation at storage level |
| **Linux only** | Cannot run on-target; results are emulation, not hardware characterisation |

---

## 9. Modelling Capabilities & What We Can Build Now

### 9.1 What is Modellable Today

#### Write Amplification

Can measure WAF as a function of:
- GC overhead % (`gc_overhead_percent` sweep)
- NAND age / bad-block count (preset or `max_erase_cycles` + `factory_bad_block_count`)
- Write pattern (sequential vs random vs mixed)
- Device lifetime (`pre_warm_erase_cycles` to start at any erase budget consumed)
- Page-relief skips' effect on WAF (skips reduce physical prog count → lower WAF)

#### ECC Page-Relief Skips

Can measure `page_relief_skips` as a function of:
- Write-wear threshold values (`ecc_prog_high/mid/fail_erase_threshold`)
- Pre-wear level (`pre_warm_erase_cycles`)
- ECC noise (`ecc_prog_noise_prob`)
- GC overhead (more GC → blocks cycle faster → thresholds hit sooner)
- `ecc_data_refresh_threshold` alignment with `CONFIG_NAND_FLASH_PROG_PAGE_RELIEF_MIN_ECC`

Can observe:
- When relief first fires (`page_relief_first_at_write`)
- Total relief volume per run
- Interaction between relief skips and FTL error rate

#### Power-Loss Robustness

Can measure data integrity after crash at configurable op count, with configurable torn-write semantics.

### 9.2 Gap Analysis (noted, not proposed)

| Gap | What we cannot measure today |
|-----|------------------------------|
| Read-disturb → scrubbing → WAF | Read-disturb events are counted but don't trigger scrubbing path; can't measure WAF cost of scrubbing |
| Hot-cold workloads | No Zipf workload wired up (`workload_zipf.c` exists but not registered) |
| Erase count distribution vs WAF | `total_erases` bug means erase distribution only comes from fault-sim counters; `mean_erase_count` is working |
| Multi-block failure patterns | Bad blocks allocated incrementally; cannot test clustered bad block patterns |
| Retention failures at rest | `grave_page_threshold` models it but no "time at rest" concept; must express as prog_count proxy |

---

## 10. Jumping-Off Configs to Write Next

### 10.1 WAF Sweep Ideas

#### A. WAF vs GC overhead across device lifetimes

**Goal:** Show WAF curves for fresh/lightly-used/aged/near-end-of-life.

```json
{
  "name": "waf_vs_gc_vs_lifetime",
  "nand": { "num_blocks": 128, "pages_per_block": 64, "page_size": 2048 },
  "scenarios": [
    { "name": "fresh",            "preset": "FRESH" },
    { "name": "25pct_life",       "pre_warm_erase_cycles": 250,  "max_erase_cycles": 1000 },
    { "name": "50pct_life",       "pre_warm_erase_cycles": 500,  "max_erase_cycles": 1000 },
    { "name": "75pct_life",       "pre_warm_erase_cycles": 750,  "max_erase_cycles": 1000 },
    { "name": "90pct_life",       "pre_warm_erase_cycles": 900,  "max_erase_cycles": 1000 }
  ],
  "ftl_configs": [
    { "name": "dhara_25pct", "ftl": "dhara", "gc_overhead_percent": 25 },
    { "name": "dhara_15pct", "ftl": "dhara", "gc_overhead_percent": 15 },
    { "name": "dhara_10pct", "ftl": "dhara", "gc_overhead_percent": 10 }
  ],
  "workload": { "type": "random", "total_writes": 100000, "write_size_bytes": 2048 }
}
```

**Expected insight:** WAF should rise as pre-warm increases (relief skips reduce usable write volume, GC must work harder).

#### B. WAF vs write size

**Goal:** Show how large vs small writes affect WAF.

```json
"ftl_configs": [
  { "name": "dhara_20pct_512",  "ftl": "dhara", "gc_overhead_percent": 20 },
  { "name": "dhara_20pct_2048", "ftl": "dhara", "gc_overhead_percent": 20 },
  { "name": "dhara_20pct_4096", "ftl": "dhara", "gc_overhead_percent": 20 }
],
"workload": { "type": "random", "total_writes": 100000, "write_size_bytes": 512 }
// run again with write_size_bytes: 2048, then 4096
```

*(Requires separate files or a CLI parameter for write size.)*

---

### 10.2 ECC Page-Relief Skip Sweep Ideas

#### C. Relief skip onset: threshold vs pre-warm level

**Goal:** Map `page_relief_first_at_write` as a function of how worn the device is and how tight the ECC threshold is.

```json
{
  "name": "relief_onset_matrix",
  "nand": { "num_blocks": 128, "pages_per_block": 64, "page_size": 2048 },
  "scenarios": [
    { "name": "warm300_thr50",  "pre_warm_erase_cycles": 300, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 50,  "ecc_prog_noise_prob": 0.02 },
    { "name": "warm300_thr100", "pre_warm_erase_cycles": 300, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 100, "ecc_prog_noise_prob": 0.02 },
    { "name": "warm300_thr200", "pre_warm_erase_cycles": 300, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 200, "ecc_prog_noise_prob": 0.02 },
    { "name": "warm500_thr50",  "pre_warm_erase_cycles": 500, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 50,  "ecc_prog_noise_prob": 0.02 },
    { "name": "warm500_thr100", "pre_warm_erase_cycles": 500, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 100, "ecc_prog_noise_prob": 0.02 },
    { "name": "warm700_thr50",  "pre_warm_erase_cycles": 700, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 50,  "ecc_prog_noise_prob": 0.02 },
    { "name": "warm700_thr100", "pre_warm_erase_cycles": 700, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 100, "ecc_prog_noise_prob": 0.02 }
  ],
  "ftl_configs": [
    { "name": "dhara_20pct", "ftl": "dhara", "gc_overhead_percent": 20 }
  ],
  "workload": { "type": "random", "total_writes": 50000, "write_size_bytes": 2048 }
}
```

**Expected insight:** `page_relief_first_at_write` should be lower (fires sooner) for higher pre-warm + lower threshold. Matrix shows the sensitivity surface.

#### D. Relief skips vs WAF: noise probability sweep at fixed wear level

**Goal:** Measure how noise `ecc_prog_noise_prob` affects skip volume and WAF at a realistic wear level (matching real chip behaviour where pages in a worn block have varying BER).

```json
{
  "name": "relief_noise_vs_waf",
  "nand": { "num_blocks": 128, "pages_per_block": 64, "page_size": 2048 },
  "scenarios": [
    { "name": "no_noise",    "pre_warm_erase_cycles": 600, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 400, "ecc_prog_noise_prob": 0.00 },
    { "name": "noise_0.01",  "pre_warm_erase_cycles": 600, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 400, "ecc_prog_noise_prob": 0.01 },
    { "name": "noise_0.05",  "pre_warm_erase_cycles": 600, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 400, "ecc_prog_noise_prob": 0.05 },
    { "name": "noise_0.10",  "pre_warm_erase_cycles": 600, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 400, "ecc_prog_noise_prob": 0.10 },
    { "name": "noise_0.25",  "pre_warm_erase_cycles": 600, "max_erase_cycles": 1000, "ecc_prog_high_erase_threshold": 400, "ecc_prog_noise_prob": 0.25 }
  ],
  "ftl_configs": [
    { "name": "dhara_20pct", "ftl": "dhara", "gc_overhead_percent": 20 }
  ],
  "workload": { "type": "random", "total_writes": 50000, "write_size_bytes": 2048 }
}
```

**Expected insight:** Higher noise → more pages bumped to HIGH threshold → more skips → lower WAF. At 0.0 noise, skips only fire for blocks above `ecc_prog_high_erase_threshold`; at 0.25, blocks slightly below also contribute.

#### E. Relief skips vs wear uniformity (Gini) — GC overhead interaction

**Goal:** Show that tighter GC overhead (more GC work) causes blocks to hit ECC thresholds more uniformly → earlier but more evenly distributed relief.

```json
{
  "name": "relief_vs_gini",
  "scenarios": [
    { "name": "worn_60pct", "pre_warm_erase_cycles": 600, "max_erase_cycles": 1000,
      "ecc_prog_high_erase_threshold": 400, "ecc_prog_noise_prob": 0.05 }
  ],
  "ftl_configs": [
    { "name": "dhara_25pct", "ftl": "dhara", "gc_overhead_percent": 25 },
    { "name": "dhara_15pct", "ftl": "dhara", "gc_overhead_percent": 15 },
    { "name": "dhara_10pct", "ftl": "dhara", "gc_overhead_percent": 10 },
    { "name": "dhara_5pct",  "ftl": "dhara", "gc_overhead_percent": 5  }
  ],
  "workload": { "type": "random", "total_writes": 100000, "write_size_bytes": 2048 }
}
```

**Read:** Compare `erase_count_gini` vs `page_relief_skips` vs `write_amplification_factor` across GC overhead values.

---

## 11. jq Cheatsheet for Report Analysis

```bash
# WAF sorted ascending
jq '[.results[] | {scenario, ftl_config, waf: .metrics.write_amplification_factor}]
    | sort_by(.waf)' report.json

# Page relief: where did first skip fire?
jq '.results[] | {scenario, ftl_config,
    skips: .metrics.page_relief_skips,
    first_at: .metrics.page_relief_first_at_write}' report.json

# ECC events summary
jq '.results[] | {scenario, ftl_config,
    mid:  .metrics.ecc_mid_events,
    high: .metrics.ecc_high_events,
    fail: .metrics.ecc_fail_events}' report.json

# Wear distribution — Gini + p99/p50 ratio
jq '.results[] | {scenario, ftl_config,
    gini: .metrics.erase_count_gini,
    p99: .metrics.erase_count_p99,
    p50: .metrics.erase_count_p50,
    hot_cold_ratio: (.metrics.erase_count_p99 / (if .metrics.erase_count_p50 == 0 then 1 else .metrics.erase_count_p50 end))}' report.json

# WAF vs relief skips (see correlation)
jq '[.results[] | {scenario, ftl_config,
    waf: .metrics.write_amplification_factor,
    relief: .metrics.page_relief_skips}]
    | sort_by(.waf)' report.json

# Only failed runs
jq '.results[] | select(.status == "failed") | {scenario, ftl_config,
    writes_succeeded: .metrics.writes_succeeded,
    ftl_errors: .metrics.ftl_errors}' report.json

# Cross-compare scenarios for a fixed ftl_config
jq '[.results[] | select(.ftl_config == "dhara_20pct")
    | {scenario, waf: .metrics.write_amplification_factor,
       relief: .metrics.page_relief_skips}]' report.json
```
