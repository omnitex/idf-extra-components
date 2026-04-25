## ADDED Requirements

### Requirement: Fault simulator implements NAND hardware interface
The `nand_fault_sim` module SHALL implement the same interface as `nand_impl_linux.c` (`nand_is_bad`, `mark_bad`, `erase_block`, `prog`, `is_free`, `read`) so it can replace the hardware layer in Linux host tests without changes to FTL code.

#### Scenario: Drop-in replacement for hardware layer
- **WHEN** a host test links `nand_fault_sim` instead of `nand_impl_linux`
- **THEN** the FTL compiles and operates without modification

### Requirement: Factory bad block injection
The fault simulator SHALL mark a configurable set of blocks as factory bad before any FTL operation, simulating real NAND factory defects.

#### Scenario: Factory bad blocks visible to FTL
- **WHEN** the simulator is initialized with a list of bad block indices
- **THEN** `nand_is_bad()` returns true for each listed block before any erase or program operations

### Requirement: Weak block wear-out (erase wear)
The fault simulator SHALL track erase counts per block and return an erase error once a block's erase count exceeds a configurable `max_erase_cycles` threshold.

#### Scenario: Block wears out after threshold
- **WHEN** a block's erase count reaches `max_erase_cycles`
- **THEN** the next `erase_block()` call on that block returns an error

#### Scenario: Blocks below threshold erase successfully
- **WHEN** a block's erase count is below `max_erase_cycles`
- **THEN** `erase_block()` succeeds normally

### Requirement: Weak page program wear (write wear)
The fault simulator SHALL track program counts per page and return a program error once a page's program count exceeds a configurable `max_prog_cycles` threshold.

#### Scenario: Page wears out after threshold
- **WHEN** a page's program count reaches `max_prog_cycles`
- **THEN** the next `prog()` call on that page returns an error

### Requirement: Grave page read data loss (retention failure)
The fault simulator SHALL simulate retention failure on pages that have been programmed more than a configurable `grave_page_threshold` times by reporting an uncorrectable ECC status via the `on_page_read_ecc` callback, without modifying the actual data bytes stored in the mmap backing. The Linux target has no ECC engine, so data corruption is signalled through the status callback path only, consistent with how real hardware reports uncorrectable errors.

#### Scenario: Grave page reports uncorrectable ECC
- **WHEN** a page's program count exceeds `grave_page_threshold` and `on_page_read_ecc` is non-NULL
- **THEN** `read()` invokes `on_page_read_ecc(page, NAND_ECC_NOT_CORRECTED, ctx)` and returns the unmodified data bytes

#### Scenario: No effect when callback is NULL
- **WHEN** `on_page_read_ecc` is NULL
- **THEN** `read()` returns data normally with no error indication

### Requirement: Per-operation probabilistic failure
The fault simulator SHALL support configurable per-operation failure probabilities that cause individual `read`, `prog`, `erase_block`, and `copy` calls to return an error with a seeded-random probability, independently of wear counts or the power-loss crash mechanism. This models transient hardware faults and intermittent bus errors.

Each operation has its own independent probability field (`read_fail_prob`, `prog_fail_prob`, `erase_fail_prob`, `copy_fail_prob`), all defaulting to 0.0 (disabled). A shared `op_fail_seed` seeds the PRNG for all four.

#### Scenario: Operation fails at configured rate
- **WHEN** `prog_fail_prob` is 0.1 and 1000 `prog()` calls are made
- **THEN** approximately 100 return an error (within a reasonable statistical margin)

#### Scenario: Failed operation does not modify flash state
- **WHEN** a probabilistic failure fires on `prog()`
- **THEN** the mmap backing is unchanged and the error is returned without writing any bytes

#### Scenario: All probabilities zero means no random failures
- **WHEN** all `*_fail_prob` fields are 0.0
- **THEN** no operation ever fails due to this mechanism

### Requirement: Per-block wear statistics
The fault simulator SHALL expose per-block erase counts and per-page program counts through a statistics API so tests can assert wear distribution.

#### Scenario: Query erase count for a block
- **WHEN** a block has been erased N times
- **THEN** `nand_fault_sim_get_erase_count(block)` returns N

#### Scenario: Query program count for a page
- **WHEN** a page has been programmed M times
- **THEN** `nand_fault_sim_get_prog_count(page)` returns M

### Requirement: Power-loss simulation — hard crash
The fault simulator SHALL support a configurable power-loss crash that freezes the device after a random number of NAND write operations (prog + erase counted together), with the random crash point derived from a seeded PRNG so tests are deterministic and reproducible.

Three mutually exclusive crash modes, all disabled when their fields are zero:
- **Range mode**: crash fires after a seeded-random op count in `[crash_after_ops_min, crash_after_ops_max]`.
- **Probability mode**: each prog/erase independently triggers the crash with probability `crash_probability`.
- **Deterministic mode**: degenerate case of range mode where `crash_after_ops_min == crash_after_ops_max`.

A single `crash_seed` field seeds both the crash-point selection and the torn-write offset.

#### Scenario: Device freezes after crash
- **WHEN** the crash condition fires
- **THEN** all subsequent calls to any simulator function return `ESP_ERR_INVALID_STATE`

#### Scenario: Reset allows remount
- **WHEN** `nand_fault_sim_reset()` is called after a crash
- **THEN** the op counter resets and the simulator accepts new operations (the mmap file retains its state)

#### Scenario: Deterministic replay with same seed
- **WHEN** two simulator instances are created with identical `crash_after_ops_min`, `crash_after_ops_max`, and `crash_seed`
- **THEN** the crash fires on the same operation number in both instances

### Requirement: Power-loss simulation — torn write
When a power-loss crash fires during an in-flight `prog()` operation, the fault simulator SHALL write only a seeded-random prefix of the page data and discard the remainder, simulating a partial write due to power loss.

#### Scenario: Torn prog leaves partial data
- **WHEN** the crash fires during `prog(page, data, len)`
- **THEN** the first `torn_offset` bytes (0 < `torn_offset` < `len`, seeded-random) are written to the mmap backing and the remainder remains 0xFF (erased state)

#### Scenario: Torn erase leaves block partially cleared
- **WHEN** the crash fires during `erase_block(block)`
- **THEN** a seeded-random prefix of pages within the block are cleared to 0xFF and the remaining pages retain their previous content

### Requirement: Read disturb / ECC status simulation
The fault simulator SHALL simulate read-disturb–induced soft ECC errors by reporting elevated `nand_ecc_status_t` values through the `on_page_read_ecc` callback after a page has been read a configurable number of times, without modifying the actual data bytes.

Three read-count thresholds correspond to the three ECC severity levels:
- `ecc_mid_threshold`: reads before reporting `NAND_ECC_1_TO_3_BITS_CORRECTED`
- `ecc_high_threshold`: reads before reporting `NAND_ECC_4_TO_6_BITS_CORRECTED`
- `ecc_fail_threshold`: reads before reporting `NAND_ECC_NOT_CORRECTED`

If `on_page_read_ecc` is NULL the callbacks are silently skipped; data is never corrupted.

#### Scenario: Mid-level ECC status reported after threshold reads
- **WHEN** a page has been read `ecc_mid_threshold` times
- **THEN** the next `read()` call invokes `on_page_read_ecc(page, NAND_ECC_1_TO_3_BITS_CORRECTED, ctx)` and returns the unmodified data

#### Scenario: Uncorrectable ECC status reported after fail threshold
- **WHEN** a page has been read `ecc_fail_threshold` times
- **THEN** `on_page_read_ecc(page, NAND_ECC_NOT_CORRECTED, ctx)` is invoked

#### Scenario: No ECC injection when thresholds are zero
- **WHEN** all three ECC thresholds are 0
- **THEN** `on_page_read_ecc` is never called by the fault simulator

### Requirement: Fault scenario presets
The fault simulator SHALL provide named preset configurations that initialize a complete `nand_fault_sim_config_t` representing common real-world device states, so test setup does not require manually specifying every fault parameter.

Presets:
- `NAND_SIM_SCENARIO_FRESH` — no faults, clean device baseline
- `NAND_SIM_SCENARIO_LIGHTLY_USED` — ~2% factory bad blocks, low erase counts
- `NAND_SIM_SCENARIO_AGED` — ~10% bad blocks, most blocks near half their erase budget, some weak blocks
- `NAND_SIM_SCENARIO_FAILING` — ~20% bad blocks, weak blocks and pages, elevated per-op failure probabilities, some grave pages
- `NAND_SIM_SCENARIO_POWER_LOSS` — crash range [50, 500] ops, torn writes enabled, no other faults

#### Scenario: Preset initializes valid config
- **WHEN** `nand_fault_sim_config_preset(NAND_SIM_SCENARIO_AGED)` is called
- **THEN** a fully populated `nand_fault_sim_config_t` is returned that passes `nand_fault_sim_init()` without error

#### Scenario: Preset config is overridable
- **WHEN** a preset config is modified after retrieval (e.g. changing `crash_seed`)
- **THEN** `nand_fault_sim_init()` accepts the modified config

### Requirement: Linux-only build
The `nand_fault_sim` module SHALL only be compiled and linked when the target is Linux (`CONFIG_IDF_TARGET_LINUX`), preventing inclusion in firmware builds.

#### Scenario: Not compiled for non-Linux targets
- **WHEN** building for an ESP32 target
- **THEN** `nand_fault_sim` source files are excluded from the build
