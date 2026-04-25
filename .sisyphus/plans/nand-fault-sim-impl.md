# nand-fault-sim: Implementation Plan

## TL;DR

> **Quick Summary**: Implement a NAND fault-injection simulator (`nand_fault_sim`) for Linux host tests in `spi_nand_flash`. The sim replaces `nand_impl_linux.c` at link time, wraps the mmap emulator, and injects configurable faults (wear-out, bad blocks, probabilistic failures, power-loss crash, ECC disturb).
>
> **Deliverables**:
> - `host_test/main/nand_fault_sim.h` + `nand_fault_sim.c` — fault sim C module (9 symbols replacing `nand_impl_linux.c`)
> - `host_test/main/ftl_interface.hpp` — abstract `FTLInterface` C++ class
> - `host_test/main/dhara_ftl.cpp` — `DharaFTL : public FTLInterface` concrete impl
> - `host_test/main/test_fault_sim.cpp` — 16 Catch2 unit tests for the fault sim
> - `host_test/main/test_ftl_robustness.cpp` — 10 Catch2 FTL robustness integration tests
> - Updated `host_test/main/CMakeLists.txt` with fault-sim build path
> - `spec.md` line 4 stale function list corrected (missing `nand_copy`, `nand_get_ecc_status`)
>
> **Estimated Effort**: Large
> **Parallel Execution**: YES — 3 waves
> **Critical Path**: Task 1 (spec fix) → Task 2 (header) → Task 3 (impl) → Task 10 (unit tests) → Task 13 (build update) → Task 14 (verify)

---

## Context

### Original Request
Implement `nand_fault_sim` as specified in OpenSpec change `nand-fault-sim` at `spi_nand_flash/openspec/changes/nand-fault-sim/`. All 37 sub-tasks from `tasks.md` must be completed.

### Interview Summary
**Key Decisions**:
- Apache-2.0 only — no GPL code
- Linux-target host tests only; zero changes to production `src/`
- All error signalling via `on_page_read_ecc` callback; no actual data bit-flip injection
- Plain C for fault sim (`nand_fault_sim.c`); C++ thin wrappers for Catch2 (`ftl_interface.hpp`, `dhara_ftl.cpp`)
- `FTLInterface` abstraction from day 1 for FTL-agnostic test suite
- Two separate PRNGs: `op_fail_seed` (per-op failures) and `crash_seed` (power-loss + torn write offset)

**Research Findings**:
- `nand_impl_linux.c` exports 9 symbols: `nand_init_device` + 8 operational functions. `nand_fault_sim.c` must provide ALL 9 or the linker will pull in `nand_impl_linux.o` for the missing ones — defeating the replacement strategy.
- `nand_linux_mmap_emul.c` public API: `nand_emul_init`, `nand_emul_deinit`, `nand_emul_read`, `nand_emul_write`, `nand_emul_erase_block`, `nand_emul_clear_stats`
- `on_page_read_ecc` registered at `dhara_glue.c:306-307`; NULL in `nand_impl_linux.c`
- `nand_ecc_status_t`: `NAND_ECC_OK=0`, `NAND_ECC_1_TO_3_BITS_CORRECTED=1`, `NAND_ECC_NOT_CORRECTED=2`, `NAND_ECC_4_TO_6_BITS_CORRECTED=3`, `NAND_ECC_7_8_BITS_CORRECTED=5`
- CMakeLists in `host_test/main/` uses `WHOLE_ARCHIVE` + `Catch2WithMain`

### Metis Review
**Identified Gaps** (addressed in this plan):
- `nand_init_device` must be in `nand_fault_sim.c` (not just the 8 operational functions) — it delegates to `nand_emul_init` with the same `detect_chip` logic
- `spec.md` line 4 stale function list must be corrected before committing implementation tasks
- CMake link exclusion of `nand_impl_linux.c` needs explicit guardrail: use CMake `EXCLUDE` or separate build condition, not hope for symbol clash resolution
- `nand_fault_sim_reset()` semantics: must reset op counter + crash PRNG state but NOT touch mmap file contents

---

## Work Objectives

### Core Objective
Implement all 37 tasks from `tasks.md` to produce a working, tested `nand_fault_sim` module that passes the full Catch2 test suite under `idf.py --target linux build` and all pre-existing tests continue to pass.

### Concrete Deliverables
- `spi_nand_flash/openspec/changes/nand-fault-sim/specs/nand-fault-sim/spec.md` — line 4 corrected
- `spi_nand_flash/host_test/main/nand_fault_sim.h`
- `spi_nand_flash/host_test/main/nand_fault_sim.c`
- `spi_nand_flash/host_test/main/ftl_interface.hpp`
- `spi_nand_flash/host_test/main/dhara_ftl.cpp`
- `spi_nand_flash/host_test/main/test_fault_sim.cpp`
- `spi_nand_flash/host_test/main/test_ftl_robustness.cpp`
- `spi_nand_flash/host_test/main/CMakeLists.txt` — updated

### Definition of Done
- [ ] `zsh -i -c 'get_idf && cd spi_nand_flash/host_test && idf.py build'` → zero errors, zero warnings
- [ ] `./build/nand_flash_host_test.elf` → exit 0, "All tests passed" (pre-existing tests)
- [ ] `./build/nand_flash_host_test.elf "[fault_sim]"` → exit 0
- [ ] `./build/nand_flash_host_test.elf "[ftl_robustness]"` → exit 0

### Must Have
- `nand_fault_sim.c` provides ALL 9 symbols from `nand_impl_linux.c` (including `nand_init_device`)
- `nand_impl_linux.c` excluded from the fault-sim build path in CMakeLists.txt via explicit CMake condition (not linker symbol clash resolution)
- All fault sim source files have Apache-2.0 SPDX header with year 2026
- `nand_fault_sim_reset()` resets counters + PRNG state but leaves mmap file contents intact
- Two separate `rand_r()` PRNG states: one for op failures (`op_fail_seed`), one for crash + torn write (`crash_seed`)

### Must NOT Have (Guardrails)
- No GPL-licensed code, no Linux kernel `nandsim.c` logic
- No changes to any file under `spi_nand_flash/src/` (production code untouched)
- No data-level bit-flip injection — ECC errors signalled ONLY via `on_page_read_ecc` callback
- No `nand_impl_linux.c` symbols in the fault-sim build (must be excluded via CMake, not worked around)
- No AI slop: no excessive JSDoc-style comments, no over-abstraction, no `data`/`result`/`temp` variable names
- No single `.c` file with 1000+ lines — split logically if needed

---

## Verification Strategy

### Test Decision
- **Infrastructure exists**: YES (Catch2WithMain, `idf.py --target linux`)
- **Automated tests**: Tests-after (unit tests written as part of the task, verified by running the binary)
- **Framework**: Catch2 via `Catch2WithMain` CMake target
- **TDD**: Not enforced — implementation and test file may be written together

### QA Policy
Every task MUST include agent-executed QA scenarios.
Evidence saved to `.sisyphus/evidence/task-{N}-{scenario-slug}.{ext}`.

- **Build verification**: Bash (`idf.py build`)
- **Test verification**: Bash (`.elf` binary with Catch2 tag filter)
- **File content verification**: Bash (`grep` / `nm` for symbol presence)

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (Start Immediately — spec fix + header + FTL interface):
├── Task 1: Fix spec.md stale function list [quick]
├── Task 2: nand_fault_sim.h — full public API header [quick]
└── Task 3: ftl_interface.hpp — abstract FTLInterface C++ class [quick]

Wave 2 (After Wave 1 — implementation files, MAX PARALLEL):
├── Task 4: nand_fault_sim.c — init/deinit/reset + all 9 symbols (depends: 2) [unspecified-high]
└── Task 5: dhara_ftl.cpp — DharaFTL : FTLInterface (depends: 3) [unspecified-high]

Wave 3 (After Wave 2 — tests + build, MAX PARALLEL):
├── Task 6: test_fault_sim.cpp — 16 unit tests (depends: 4) [unspecified-high]
├── Task 7: test_ftl_robustness.cpp — 10 integration tests (depends: 4, 5) [unspecified-high]
└── Task 8: CMakeLists.txt update — fault-sim build path (depends: 4, 5, 6, 7) [quick]

Wave FINAL (After ALL — 3 parallel reviews):
├── Task F1: Build verification + pre-existing test pass (oracle)
├── Task F2: New test suite passes [fault_sim] + [ftl_robustness] (unspecified-high)
└── Task F3: Spec compliance audit — symbol list, Apache-2.0 headers, no src/ changes (deep)
→ Present results → Get explicit user okay
```

### Agent Dispatch Summary
- **Wave 1**: 3 × `quick`
- **Wave 2**: 2 × `unspecified-high`
- **Wave 3**: 2 × `unspecified-high` + 1 × `quick`
- **FINAL**: 1 × `oracle`, 1 × `unspecified-high`, 1 × `deep`

---

## TODOs

- [ ] 1. Fix `spec.md` stale function list

  **What to do**:
  - Edit `spi_nand_flash/openspec/changes/nand-fault-sim/specs/nand-fault-sim/spec.md` line 4
  - Replace `('nand_is_bad', 'mark_bad', 'erase_block', 'prog', 'is_free', 'read')` with the complete, correctly-named list: `nand_is_bad`, `nand_mark_bad`, `nand_erase_block`, `nand_prog`, `nand_is_free`, `nand_read`, `nand_copy`, `nand_get_ecc_status`
  - Commit: `fix(spi_nand_flash): correct spec.md interface function list`

  **Must NOT do**:
  - Do NOT change any other line in spec.md
  - Do NOT touch any file under `src/`

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 2, 3)
  - **Blocks**: nothing (spec-only fix)
  - **Blocked By**: None

  **References**:
  - `spi_nand_flash/openspec/changes/nand-fault-sim/specs/nand-fault-sim/spec.md:4` — the line to fix
  - `spi_nand_flash/src/nand_impl_linux.c:57,107,122,138,182,196,209,221,235` — ground-truth function signatures

  **Acceptance Criteria**:
  - [ ] `grep "nand_copy\|nand_get_ecc_status" spi_nand_flash/openspec/changes/nand-fault-sim/specs/nand-fault-sim/spec.md` → matches line 4

  **QA Scenarios**:
  ```
  Scenario: Corrected function list in spec.md
    Tool: Bash (grep)
    Steps:
      1. grep -n "nand_copy" spi_nand_flash/openspec/changes/nand-fault-sim/specs/nand-fault-sim/spec.md
    Expected Result: line 4 contains "nand_copy" and "nand_get_ecc_status"
    Evidence: .sisyphus/evidence/task-1-spec-fix.txt
  ```

  **Commit**: YES
  - Message: `fix(spi_nand_flash): correct spec.md interface function list`
  - Files: `spi_nand_flash/openspec/changes/nand-fault-sim/specs/nand-fault-sim/spec.md`

- [ ] 2. Create `host_test/main/nand_fault_sim.h`

  **What to do**:
  - Create `spi_nand_flash/host_test/main/nand_fault_sim.h` with Apache-2.0 SPDX header (year 2026)
  - Define `nand_sim_scenario_t` enum: `NAND_SIM_SCENARIO_FRESH`, `NAND_SIM_SCENARIO_LIGHTLY_USED`, `NAND_SIM_SCENARIO_AGED`, `NAND_SIM_SCENARIO_FAILING`, `NAND_SIM_SCENARIO_POWER_LOSS`
  - Define `nand_fault_sim_config_t` struct with ALL fields: `bad_blocks[]` + `bad_block_count`; `max_erase_cycles`, `max_prog_cycles`, `grave_page_threshold`; `read_fail_prob`, `prog_fail_prob`, `erase_fail_prob`, `copy_fail_prob`, `op_fail_seed`; `crash_after_ops_min`, `crash_after_ops_max`, `crash_probability`, `crash_seed`; `ecc_mid_threshold`, `ecc_high_threshold`, `ecc_fail_threshold`; `emul_conf` pointer (`nand_file_mmap_emul_config_t *`)
  - Declare lifecycle API: `nand_fault_sim_init(config, handle*)`, `nand_fault_sim_deinit(handle)`, `nand_fault_sim_reset(handle)`
  - Declare stats API: `nand_fault_sim_get_erase_count(handle, block)`, `nand_fault_sim_get_prog_count(handle, page)`, `nand_fault_sim_get_read_count(handle, page)`
  - Declare preset API: `nand_fault_sim_config_t nand_fault_sim_config_preset(nand_sim_scenario_t scenario)`
  - The 9 `nand_impl` symbols (`nand_init_device`, `nand_is_bad`, `nand_mark_bad`, `nand_erase_block`, `nand_prog`, `nand_is_free`, `nand_read`, `nand_copy`, `nand_get_ecc_status`) are NOT declared here — they are defined in `nand_fault_sim.c` and declared in `nand_private/nand_impl_wrap.h` (existing header)

  **Must NOT do**:
  - Do NOT redeclare the 9 nand_impl symbols — they already have declarations in `include/nand_private/nand_impl_wrap.h`
  - Do NOT use `int` for probabilities — use `float` (range [0.0, 1.0])

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 3)
  - **Blocks**: Task 4
  - **Blocked By**: None

  **References**:
  - `spi_nand_flash/openspec/changes/nand-fault-sim/tasks.md` — task 1.1 for full field list
  - `spi_nand_flash/include/nand_linux_mmap_emul.h` — `nand_file_mmap_emul_config_t` type
  - `spi_nand_flash/include/nand_private/nand_impl_wrap.h` — existing nand_impl function declarations (DO NOT redeclare)
  - `spi_nand_flash/src/nand_impl_linux.c:1-10` — reference Apache-2.0 SPDX header format

  **Acceptance Criteria**:
  - [ ] File compiles without errors when included from a C file
  - [ ] All fields from tasks.md 1.1 present in struct

  **QA Scenarios**:
  ```
  Scenario: Header compiles cleanly
    Tool: Bash
    Steps:
      1. zsh -i -c 'get_idf && cd spi_nand_flash/host_test && idf.py build' (after Task 8 CMake update)
    Expected Result: zero compile errors referencing nand_fault_sim.h
    Evidence: .sisyphus/evidence/task-2-header-compile.txt
  ```

  **Commit**: NO (group with Task 4)

- [ ] 3. Create `host_test/main/ftl_interface.hpp`

  **What to do**:
  - Create `spi_nand_flash/host_test/main/ftl_interface.hpp` with Apache-2.0 SPDX header (year 2026)
  - Define pure-virtual `FTLInterface` class:
    ```cpp
    class FTLInterface {
    public:
        virtual ~FTLInterface() = default;
        virtual esp_err_t mount() = 0;
        virtual void unmount() = 0;
        virtual esp_err_t read(uint32_t lba, uint8_t *buf, size_t size) = 0;
        virtual esp_err_t write(uint32_t lba, const uint8_t *buf, size_t size) = 0;
        virtual esp_err_t sync() = 0;
        virtual uint32_t num_sectors() const = 0;
        virtual uint32_t sector_size() const = 0;
    };
    ```
  - No implementation, no dependencies beyond standard C++ + ESP-IDF `esp_err.h`

  **Must NOT do**:
  - Do NOT include any `nand_fault_sim.h` in this file — it is a pure abstraction
  - Do NOT add methods beyond the 7 listed

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 2)
  - **Blocks**: Task 5
  - **Blocked By**: None

  **References**:
  - `spi_nand_flash/openspec/changes/nand-fault-sim/specs/ftl-host-tests/spec.md` — FTLInterface spec
  - `spi_nand_flash/openspec/changes/nand-fault-sim/tasks.md` — task 2.1

  **Acceptance Criteria**:
  - [ ] File is valid C++ 11 header with no implementation
  - [ ] All 7 virtual methods present

  **QA Scenarios**:
  ```
  Scenario: Header is valid C++
    Tool: Bash (grep + compiler via build)
    Steps:
      1. grep -c "virtual" spi_nand_flash/host_test/main/ftl_interface.hpp
    Expected Result: stdout is "7" (7 virtual methods)
    Evidence: .sisyphus/evidence/task-3-ftl-interface.txt
  ```

  **Commit**: NO (group with Task 5)

- [ ] 4. Create `host_test/main/nand_fault_sim.c` — full implementation

  **What to do**:
  - Create `spi_nand_flash/host_test/main/nand_fault_sim.c` with Apache-2.0 SPDX header (year 2026)
  - Implement a private `nand_fault_sim_t` struct (opaque internal state): holds pointer to `spi_nand_flash_device_t` (the device opened by `nand_emul_init`), `uint32_t *erase_count` (per-block array), `uint32_t *prog_count` (per-page array), `uint32_t *read_count` (per-page array), `bool crashed`, `uint32_t op_count`, `unsigned int op_fail_rng` (seeded from `op_fail_seed`), `unsigned int crash_rng` (seeded from `crash_seed`), `uint32_t crash_point` (pre-computed for range mode), and a copy of the `nand_fault_sim_config_t`
  - Use a module-level static `s_sim` pointer so the 9 `nand_impl` symbols (which have fixed signatures matching `nand_impl_linux.c`) can access simulator state without changing their signatures
  - Implement `nand_fault_sim_init`: allocate `nand_fault_sim_t`, call `nand_init_device_impl` (the mmap-based init logic from `nand_impl_linux.c`, replicated or called via the emul layer), allocate per-block/per-page arrays, seed PRNGs, pre-compute crash_point for range mode, set `s_sim`
  - Implement `nand_fault_sim_deinit`: call `nand_emul_deinit`, free all arrays, free struct, null `s_sim`
  - Implement `nand_fault_sim_reset`: zero all count arrays, reset `crashed`/`op_count`, re-seed `op_fail_rng` from `op_fail_seed`, re-derive `crash_point`; do NOT touch mmap file contents
  - Implement `nand_fault_sim_get_erase_count(block)`, `nand_fault_sim_get_prog_count(page)`, `nand_fault_sim_get_read_count(page)`: return from arrays
  - Implement `nand_fault_sim_config_preset(scenario)`: return populated `nand_fault_sim_config_t` by value for each of 5 scenarios
  - Implement ALL 9 `nand_impl` symbols:
    - `nand_init_device`: replicate detect_chip logic from `nand_impl_linux.c:57-106` (calls `nand_emul_init`, sets `dev->chip.*` fields); this is the symbol pulled in by `nand_impl_wrap.c` at startup
    - `nand_is_bad`: check factory bad-block list first; then delegate to `nand_emul_read` to inspect OOB marker
    - `nand_mark_bad`: erase block via `nand_emul_erase_block`, write zero OOB marker via `nand_emul_write`
    - `nand_erase_block`: crashed guard → crash trigger check → `erase_fail_prob` roll → increment `erase_count[block]`, check vs `max_erase_cycles` → delegate
    - `nand_prog`: crashed guard → crash trigger → torn-write on crash (prefix via `nand_emul_write`) → `prog_fail_prob` roll → increment `prog_count[page]`, check vs `max_prog_cycles` → delegate
    - `nand_is_free`: crashed guard → delegate
    - `nand_read`: crashed guard → `read_fail_prob` roll → delegate → increment `read_count[page]` → grave-page check → ECC disturb check
    - `nand_copy`: crashed guard → `copy_fail_prob` roll → delegate to `nand_emul_read` + `nand_emul_write`
    - `nand_get_ecc_status`: delegate to emulator (no fault injection on status reads)
  - Shared helper `ns_check_crash(sim)`: increment `op_count`; range mode: compare to `crash_point`; probability mode: `rand_r(&crash_rng) / (float)RAND_MAX < crash_probability`; set `crashed = true`; return bool
  - ECC disturb helper: compare `read_count[page]` vs thresholds in descending order; invoke `handle->on_page_read_ecc(page, status, handle->on_page_read_ecc_ctx)` if non-NULL

  **Must NOT do**:
  - Do NOT modify `nand_impl_linux.c` or any file under `src/`
  - Do NOT use a non-seeded `rand()` — use `rand_r()` with the stored PRNG state for determinism
  - Do NOT corrupt actual data bytes in the mmap buffer for ECC simulation
  - Do NOT use global variables other than `static nand_fault_sim_t *s_sim`

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO (depends on Task 2)
  - **Parallel Group**: Wave 2 (with Task 5, after Wave 1)
  - **Blocks**: Tasks 6, 7, 8
  - **Blocked By**: Task 2

  **References**:
  - `spi_nand_flash/src/nand_impl_linux.c` — full file; replicate `detect_chip()` and `nand_init_device()` logic (lines 14-106) and all operational functions (lines 107-250+)
  - `spi_nand_flash/src/nand_linux_mmap_emul.c` — public API: `nand_emul_init`, `nand_emul_deinit`, `nand_emul_read`, `nand_emul_write`, `nand_emul_erase_block`
  - `spi_nand_flash/include/nand_linux_mmap_emul.h` — `nand_file_mmap_emul_config_t`, `nand_mmap_emul_handle_t`
  - `spi_nand_flash/include/nand_device_types.h` — `nand_ecc_status_t` enum values
  - `spi_nand_flash/include/nand.h` — `spi_nand_flash_device_t` fields including `on_page_read_ecc` callback and `on_page_read_ecc_ctx`
  - `spi_nand_flash/openspec/changes/nand-fault-sim/tasks.md` — tasks 1.2–1.15 for full behavioral spec
  - `spi_nand_flash/openspec/changes/nand-fault-sim/design.md` — design decisions D1–D7

  **Acceptance Criteria**:
  - [ ] File compiles without warnings
  - [ ] `nm` or `objdump` on the object file shows all 9 `nand_impl` symbols as defined (`T` type)
  - [ ] `nand_fault_sim_get_erase_count`, `nand_fault_sim_get_prog_count`, `nand_fault_sim_get_read_count` return 0 after `nand_fault_sim_reset()`

  **QA Scenarios**:
  ```
  Scenario: All 9 nand_impl symbols present
    Tool: Bash (nm)
    Steps:
      1. zsh -i -c 'get_idf && cd spi_nand_flash/host_test && idf.py build'
      2. nm build/esp-idf/spi_nand_flash/CMakeFiles/spi_nand_flash.dir/host_test/main/nand_fault_sim.c.obj | grep "T nand_"
    Expected Result: 9 lines including nand_init_device, nand_is_bad, nand_mark_bad, nand_erase_block, nand_prog, nand_is_free, nand_read, nand_copy, nand_get_ecc_status
    Evidence: .sisyphus/evidence/task-4-symbols.txt

  Scenario: nand_impl_linux.c symbols absent from build
    Tool: Bash (nm)
    Steps:
      1. nm build/esp-idf/spi_nand_flash/CMakeFiles/spi_nand_flash.dir/src/nand_impl_linux.c.obj 2>&1
    Expected Result: command fails (file does not exist) OR all 9 nand_impl symbols are absent from the final binary
    Evidence: .sisyphus/evidence/task-4-no-linux-impl.txt
  ```

  **Commit**: YES
  - Message: `feat(spi_nand_flash): add nand_fault_sim C module header and implementation`
  - Files: `spi_nand_flash/host_test/main/nand_fault_sim.h`, `spi_nand_flash/host_test/main/nand_fault_sim.c`
  - Pre-commit: `zsh -i -c 'get_idf && cd spi_nand_flash/host_test && idf.py build'` (after Task 8)

- [ ] 5. Create `host_test/main/dhara_ftl.cpp`

  **What to do**:
  - Create `spi_nand_flash/host_test/main/dhara_ftl.cpp` with Apache-2.0 SPDX header (year 2026)
  - Implement `DharaFTL : public FTLInterface`:
    - Constructor takes `nand_fault_sim_config_t *` + a temp file path (or `nand_file_mmap_emul_config_t *`)
    - `mount()`: call `nand_fault_sim_init(config, &m_sim_handle)` then `spi_nand_flash_init_device(&m_dev_config, &m_dev_handle)` — at link time `nand_init_device` resolves to `nand_fault_sim.c`'s version
    - `unmount()`: call `spi_nand_flash_deinit_device(m_dev_handle)` then `nand_fault_sim_deinit(m_sim_handle)`
    - `read(lba, buf, size)` → `spi_nand_flash_read_sector(m_dev_handle, buf, lba, size / sector_size_val)`
    - `write(lba, buf, size)` → `spi_nand_flash_write_sector(m_dev_handle, buf, lba, size / sector_size_val)`
    - `sync()` → `spi_nand_flash_sync(m_dev_handle)`
    - `num_sectors()` → `spi_nand_flash_get_sector_count(m_dev_handle)`
    - `sector_size()` → `spi_nand_flash_get_sector_size(m_dev_handle)`

  **Must NOT do**:
  - Do NOT pass the `nand_fault_sim_t*` explicitly into the FTL layer — fault injection happens transparently via the 9 linked symbols
  - Do NOT include any platform headers that break on Linux target

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (can run in parallel with Task 4)
  - **Parallel Group**: Wave 2 (with Task 4, after Wave 1)
  - **Blocks**: Tasks 7, 8
  - **Blocked By**: Task 3

  **References**:
  - `spi_nand_flash/host_test/main/ftl_interface.hpp` — base class (Task 3 output)
  - `spi_nand_flash/host_test/main/nand_fault_sim.h` — lifecycle API (Task 2 output)
  - `spi_nand_flash/include/spi_nand_flash.h` — `spi_nand_flash_init_device`, `spi_nand_flash_deinit_device`, `spi_nand_flash_read_sector`, `spi_nand_flash_write_sector`, `spi_nand_flash_sync`, `spi_nand_flash_get_sector_count`, `spi_nand_flash_get_sector_size`
  - `spi_nand_flash/host_test/main/test_nand_flash.cpp` — reference for how `spi_nand_flash_device_t` is initialized in existing host tests
  - `spi_nand_flash/openspec/changes/nand-fault-sim/tasks.md` — task 2.2

  **Acceptance Criteria**:
  - [ ] `DharaFTL` compiles without errors
  - [ ] `DharaFTL` can mount and unmount in a test without crashing

  **QA Scenarios**:
  ```
  Scenario: DharaFTL mount/unmount succeeds
    Tool: Bash (via test runner — validated by test_ftl_robustness.cpp Task 7, scenario 4.10 preset smoke test)
    Steps:
      1. ./build/nand_flash_host_test.elf "[ftl_robustness]" (after Tasks 7, 8)
    Expected Result: exit 0, "All tests passed"
    Evidence: .sisyphus/evidence/task-5-dhara-ftl.txt
  ```

  **Commit**: YES
  - Message: `feat(spi_nand_flash): add FTLInterface abstraction and DharaFTL implementation`
  - Files: `spi_nand_flash/host_test/main/ftl_interface.hpp`, `spi_nand_flash/host_test/main/dhara_ftl.cpp`

- [ ] 6. Create `host_test/main/test_fault_sim.cpp` — 16 unit tests

  **What to do**:
  - Create `spi_nand_flash/host_test/main/test_fault_sim.cpp` with Apache-2.0 SPDX header (year 2026)
  - All tests tagged `[fault_sim]`; each test creates a fresh temp mmap file, inits fault sim directly (bypassing FTL layer), and calls `nand_fault_sim_deinit` at end
  - Implement ALL 16 unit tests from `tasks.md` section 3, exactly as specified:
    - 3.2: factory bad blocks (`bad_blocks[]` set; `nand_is_bad` returns true for listed, false for others)
    - 3.3: `mark_bad` at runtime (OOB marker bytes 0x0000 in mmap; `reset()` does not clear marker)
    - 3.4: erase wear-out (`max_erase_cycles=3`; 3 erases succeed; 4th returns error; counter == 3)
    - 3.5: prog wear-out (`max_prog_cycles=2`; 2 progs succeed; 3rd errors; counter == 2)
    - 3.6: grave page ECC callback (`grave_page_threshold=1`; prog page twice; read; callback with `NAND_ECC_NOT_CORRECTED`; data bytes unchanged)
    - 3.7: `prog_fail_prob=1.0` → error, mmap unchanged; `prog_fail_prob=0.0` → success
    - 3.8: `read_fail_prob=1.0` → error; `=0.0` → returns correct data
    - 3.9: `erase_fail_prob=1.0` → error; block contents unchanged in mmap
    - 3.10: `nand_fault_sim_get_read_count` — read page 5 seven times; counter == 7
    - 3.11: deterministic crash — `crash_after_ops_min=crash_after_ops_max=5`; ops 1-4 succeed; op 5 returns fail; op 6 returns `ESP_ERR_INVALID_STATE`
    - 3.12: torn prog — crash at op 1; read mmap directly; prefix bytes match written data; suffix is 0xFF
    - 3.13: range mode — min=3, max=10, seed=7; crash fires between ops 3 and 10; re-init with same seed → same op count
    - 3.14: ECC disturb thresholds — `ecc_mid=3, ecc_high=6, ecc_fail=9`; read counts escalate callback status correctly; data bytes unchanged
    - 3.15: all thresholds=0 → `on_page_read_ecc` never called after 1000 reads
    - 3.16: `reset()` clears all counters; `crashed=false`; bad-block set empty in memory (but OOB in mmap unchanged)

  **Must NOT do**:
  - Do NOT go through the FTL (`spi_nand_flash_*`) layer — test `nand_fault_sim_*` API and nand_impl symbols directly
  - Do NOT write tests that require human inspection — all assertions are in-code

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (can run in parallel with Task 7)
  - **Parallel Group**: Wave 3 (with Tasks 7, 8, after Wave 2)
  - **Blocks**: Task 8
  - **Blocked By**: Task 4

  **References**:
  - `spi_nand_flash/host_test/main/test_ecc_relief.cpp` — reference Catch2 style, tag usage, tmp file pattern
  - `spi_nand_flash/host_test/main/nand_fault_sim.h` — full API
  - `spi_nand_flash/include/nand_device_types.h` — `nand_ecc_status_t` values
  - `spi_nand_flash/include/nand_linux_mmap_emul.h` — `nand_file_mmap_emul_config_t` for creating temp flash file
  - `spi_nand_flash/openspec/changes/nand-fault-sim/tasks.md` — tasks 3.1–3.16 (exact scenario specs)

  **Acceptance Criteria**:
  - [ ] `./build/nand_flash_host_test.elf "[fault_sim]"` → exit 0, all 16 test cases pass

  **QA Scenarios**:
  ```
  Scenario: All fault_sim unit tests pass
    Tool: Bash
    Steps:
      1. zsh -i -c 'get_idf && cd spi_nand_flash/host_test && idf.py build'
      2. ./build/nand_flash_host_test.elf "[fault_sim]" 2>&1 | tee .sisyphus/evidence/task-6-fault-sim-tests.txt
      3. echo "Exit: $?"
    Expected Result: exit 0, output contains "All tests passed" or "16 test cases passed"
    Evidence: .sisyphus/evidence/task-6-fault-sim-tests.txt
  ```

  **Commit**: YES (group with Task 7)
  - Message: `test(spi_nand_flash): add fault sim unit tests and FTL robustness tests`
  - Files: `spi_nand_flash/host_test/main/test_fault_sim.cpp`, `spi_nand_flash/host_test/main/test_ftl_robustness.cpp`

- [ ] 7. Create `host_test/main/test_ftl_robustness.cpp` — 10 integration tests

  **What to do**:
  - Create `spi_nand_flash/host_test/main/test_ftl_robustness.cpp` with Apache-2.0 SPDX header (year 2026)
  - All tests tagged `[ftl_robustness]`; each test creates a `DharaFTL` instance on a temp mmap file
  - Implement ALL 10 integration tests from `tasks.md` section 4:
    - 4.2: normal read/write round-trip — no faults; write incrementing byte pattern; read back; exact match
    - 4.3: 5% factory bad block injection; write+read full address space; FTL succeeds
    - 4.4: drive 20% of blocks past `max_erase_cycles`; FTL continues on remaining capacity
    - 4.5: low `grave_page_threshold`; hammer writes; FTL reports error or ECC rather than silent data corruption
    - 4.6: `prog_fail_prob=0.05`, `read_fail_prob=0.02`, fixed seed; 500 LBA write/read cycles; FTL retries without permanent data loss
    - 4.7: crash in range [50,150]; write data; observe crash; remount on same mmap file (`keep_dump=true`); assert flushed data intact
    - 4.8: seed sweep 0..19 with range [10,100]; all 20 seeds: write, crash, remount, assert FTL recovers
    - 4.9: `ecc_mid_threshold=10`; read same LBA 15 times; query `spi_nand_flash_get_ecc_relief_stats()`; assert relief map recorded the page
    - 4.10: five preset smoke tests — each preset: init, mount, write one sector, read back, unmount; no crash or assertion failure

  **Must NOT do**:
  - Do NOT hardcode block/page counts — query from `FTLInterface::num_sectors()` and `sector_size()`
  - Do NOT assume test order — each test must be fully independent (fresh mmap file)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES (can run in parallel with Task 6)
  - **Parallel Group**: Wave 3 (with Tasks 6, 8, after Wave 2)
  - **Blocks**: Task 8
  - **Blocked By**: Tasks 4, 5

  **References**:
  - `spi_nand_flash/host_test/main/test_nand_flash.cpp` — reference for full-device read/write test patterns and mmap config setup
  - `spi_nand_flash/host_test/main/test_ecc_relief.cpp` — `spi_nand_flash_get_ecc_relief_stats()` usage (for task 4.9)
  - `spi_nand_flash/host_test/main/dhara_ftl.cpp` — `DharaFTL` constructor usage (Task 5 output)
  - `spi_nand_flash/host_test/main/nand_fault_sim.h` — preset API, config fields
  - `spi_nand_flash/openspec/changes/nand-fault-sim/tasks.md` — tasks 4.1–4.10

  **Acceptance Criteria**:
  - [ ] `./build/nand_flash_host_test.elf "[ftl_robustness]"` → exit 0, all 10 test cases pass

  **QA Scenarios**:
  ```
  Scenario: All ftl_robustness tests pass
    Tool: Bash
    Steps:
      1. ./build/nand_flash_host_test.elf "[ftl_robustness]" 2>&1 | tee .sisyphus/evidence/task-7-ftl-robustness.txt
      2. echo "Exit: $?"
    Expected Result: exit 0, output contains "10 test cases passed"
    Evidence: .sisyphus/evidence/task-7-ftl-robustness.txt
  ```

  **Commit**: YES (group with Task 6 — see Task 6 commit message)

- [ ] 8. Update `host_test/main/CMakeLists.txt` for fault-sim build path

  **What to do**:
  - Add a new CMake condition (e.g. `if(CONFIG_NAND_FAULT_SIM)` or simply an unconditional `list(APPEND src ...)` guarded by `if(${target} STREQUAL "linux")`) to include `nand_fault_sim.c`, `dhara_ftl.cpp`, `test_fault_sim.cpp`, `test_ftl_robustness.cpp`
  - The critical requirement: `nand_impl_linux.c` is compiled as part of the `spi_nand_flash` component (not the `host_test` component). Both `nand_impl_linux.c` and `nand_fault_sim.c` define the same 9 symbols — they cannot both be linked. Strategy: the `host_test` CMakeLists must override the component's source list to exclude `nand_impl_linux.c` when fault-sim files are present. Use `set_property(TARGET ${COMPONENT_LIB} ...)` or `idf_build_set_property` to remove `nand_impl_linux.c` from the spi_nand_flash component sources, OR use the `idf_component_register(EXCLUDE_SRCS ...)` mechanism if available, OR link `nand_fault_sim.c` with stronger precedence
  - Simplest correct approach: add a `CONFIG_NAND_FAULT_SIM` Kconfig option (boolean, default n) in the component's `Kconfig`; gate `nand_impl_linux.c` on `!CONFIG_NAND_FAULT_SIM`; gate fault-sim files on `CONFIG_NAND_FAULT_SIM`; add `CONFIG_NAND_FAULT_SIM=y` to a new `sdkconfig.fault_sim` defaults file; document usage
  - If the Kconfig approach is too invasive, use the CMake `target_sources(REMOVE_ITEM ...)` approach on the spi_nand_flash component target
  - Verify there are no duplicate symbol linker errors

  **Must NOT do**:
  - Do NOT rely on linker symbol resolution order to "accidentally" pick the right one — make the exclusion explicit and deterministic
  - Do NOT remove fault-sim sources from the non-fault-sim build path

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 3 (after Tasks 4, 5, 6, 7)
  - **Blocks**: Final Verification
  - **Blocked By**: Tasks 4, 5, 6, 7

  **References**:
  - `spi_nand_flash/host_test/main/CMakeLists.txt` — current structure (3 source files + `Catch2WithMain`)
  - `spi_nand_flash/CMakeLists.txt` — how `nand_impl_linux.c` is currently gated on `${target} STREQUAL "linux"`
  - `spi_nand_flash/openspec/changes/nand-fault-sim/tasks.md` — task 5.1

  **Acceptance Criteria**:
  - [ ] `zsh -i -c 'get_idf && cd spi_nand_flash/host_test && idf.py build'` → zero errors, zero warnings
  - [ ] No duplicate symbol linker errors

  **QA Scenarios**:
  ```
  Scenario: Build succeeds with fault-sim sources included
    Tool: Bash
    Steps:
      1. zsh -i -c 'get_idf && cd spi_nand_flash/host_test && idf.py build' 2>&1 | tee .sisyphus/evidence/task-8-build.txt
      2. echo "Exit: $?"
    Expected Result: exit 0, no "multiple definition" linker errors
    Evidence: .sisyphus/evidence/task-8-build.txt

  Scenario: Pre-existing tests still pass after CMake change
    Tool: Bash
    Steps:
      1. ./build/nand_flash_host_test.elf 2>&1 | tee .sisyphus/evidence/task-8-preexisting.txt
    Expected Result: exit 0, "All tests passed"
    Evidence: .sisyphus/evidence/task-8-preexisting.txt
  ```

  **Commit**: YES
  - Message: `build(spi_nand_flash): update host_test CMakeLists for fault-sim build path`
  - Files: `spi_nand_flash/host_test/main/CMakeLists.txt`, optionally `spi_nand_flash/Kconfig` + `spi_nand_flash/host_test/sdkconfig.fault_sim`

---

## Final Verification Wave (MANDATORY — after ALL implementation tasks)

> 3 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.
> **Do NOT auto-proceed. Wait for user's explicit approval before marking work complete.**

- [ ] F1. **Build + Pre-existing Tests** — `oracle`
  Run `zsh -i -c 'get_idf && cd spi_nand_flash/host_test && idf.py build'` — assert zero errors, zero warnings. Run `./build/nand_flash_host_test.elf` — assert exit 0 and "All tests passed". Run `./build/nand_flash_host_test.elf "[ecc_relief]"` — assert pre-existing ECC relief tests still pass.
  Output: `Build [PASS/FAIL] | Pre-existing tests [PASS/FAIL] | VERDICT: APPROVE/REJECT`

- [ ] F2. **New Test Suite** — `unspecified-high`
  Run `./build/nand_flash_host_test.elf "[fault_sim]"` — assert all 16 unit tests pass. Run `./build/nand_flash_host_test.elf "[ftl_robustness]"` — assert all 10 integration tests pass. Capture full output as evidence.
  Output: `fault_sim [N/16 pass] | ftl_robustness [N/10 pass] | VERDICT: APPROVE/REJECT`

- [ ] F3. **Spec Compliance Audit** — `deep`
  Verify: (1) `nand_fault_sim.c` exports all 9 symbols via `nm build/esp-idf/spi_nand_flash/libspi_nand_flash.a` or similar. (2) All new files have Apache-2.0 SPDX header. (3) No file under `src/` was modified (`git diff --name-only HEAD~1` or similar). (4) `spec.md` line 4 now lists all 8 operational functions. (5) `nand_impl_linux.c` is excluded from fault-sim build (check CMakeLists).
  Output: `Symbols [9/9] | SPDX [PASS/FAIL] | src/ unchanged [PASS/FAIL] | spec fixed [PASS/FAIL] | VERDICT`

---

## Commit Strategy

All changes committed on branch `feat/nandsim` in logical groups:
- `fix(spi_nand_flash): correct spec.md interface function list`
- `feat(spi_nand_flash): add nand_fault_sim C module header and implementation`
- `feat(spi_nand_flash): add FTLInterface abstraction and DharaFTL implementation`
- `test(spi_nand_flash): add fault sim unit tests and FTL robustness tests`
- `build(spi_nand_flash): update host_test CMakeLists for fault-sim build path`

---

## Success Criteria

```bash
zsh -i -c 'get_idf && cd spi_nand_flash/host_test && idf.py build'
# Expected: exit 0, zero errors, zero warnings

./build/nand_flash_host_test.elf
# Expected: exit 0, "All tests passed"

./build/nand_flash_host_test.elf "[fault_sim]"
# Expected: exit 0, 16 assertions passed

./build/nand_flash_host_test.elf "[ftl_robustness]"
# Expected: exit 0, 10 test cases passed
```

### Final Checklist
- [ ] All 9 `nand_impl` symbols provided by `nand_fault_sim.c`
- [ ] `nand_impl_linux.c` excluded from fault-sim build via explicit CMake condition
- [ ] Apache-2.0 SPDX header on every new file
- [ ] No changes under `spi_nand_flash/src/`
- [ ] `spec.md` line 4 corrected
- [ ] All pre-existing Catch2 tests still pass
