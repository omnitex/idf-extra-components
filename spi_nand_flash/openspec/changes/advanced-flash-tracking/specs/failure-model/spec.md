# Failure Model Capability

## ADDED Requirements

### Requirement: Pluggable failure model interface
The system SHALL define an abstract interface `nand_failure_model_ops_t` that allows injection of custom failure behaviors for testing.

#### Scenario: Initialize custom failure model
- **WHEN** developer calls `nand_emul_advanced_init()` with custom `failure_model_ops`
- **THEN** emulator SHALL call the model's `init()` function with provided config
- **AND** SHALL store the model handle for subsequent operations

#### Scenario: No failure model configured
- **WHEN** `nand_emul_advanced_init()` is called with `failure_model = NULL`
- **THEN** emulator SHALL operate without failure injection
- **AND** all operations SHALL succeed unless actual errors occur

### Requirement: Read failure injection
The system SHALL invoke failure model before read operations to determine if the operation should fail.

#### Scenario: Simulate read failure
- **WHEN** failure model's `should_fail_read()` returns true
- **THEN** `nand_emul_read()` SHALL return `ESP_ERR_FLASH_OP_FAIL`
- **AND** SHALL NOT copy data to destination buffer

#### Scenario: Normal read with failure model
- **WHEN** failure model's `should_fail_read()` returns false
- **THEN** `nand_emul_read()` SHALL proceed normally
- **AND** SHALL copy data to destination buffer

### Requirement: Write failure injection
The system SHALL invoke failure model before write operations to determine if the operation should fail.

#### Scenario: Simulate write failure
- **WHEN** failure model's `should_fail_write()` returns true
- **THEN** `nand_emul_write()` SHALL return `ESP_ERR_FLASH_OP_FAIL`
- **AND** SHALL NOT modify flash memory contents

#### Scenario: Write failure after partial program
- **WHEN** failure model's `should_fail_write()` returns true
- **THEN** flash state SHALL remain consistent (either fully written or unchanged)
- **AND** metadata tracking SHALL NOT record the failed write

### Requirement: Erase failure injection
The system SHALL invoke failure model before erase operations to determine if the operation should fail.

#### Scenario: Simulate erase failure
- **WHEN** failure model's `should_fail_erase()` returns true
- **THEN** `nand_emul_erase_block()` SHALL return `ESP_ERR_FLASH_OP_FAIL`
- **AND** SHALL NOT erase the block contents

#### Scenario: Failed erase metadata handling
- **WHEN** erase operation fails due to failure model
- **THEN** metadata backend SHALL NOT increment block erase count
- **AND** SHALL NOT update block timestamps

### Requirement: Data corruption injection
The system SHALL allow failure model to corrupt read data to simulate bit flips and retention errors.

#### Scenario: Inject bit errors on read
- **WHEN** failure model's `corrupt_read_data()` is called after successful read
- **THEN** model SHALL modify data buffer in-place to simulate bit flips
- **AND** `nand_emul_read()` SHALL return corrupted data to caller

#### Scenario: No corruption
- **WHEN** failure model's `corrupt_read_data()` chooses not to corrupt data
- **THEN** data buffer SHALL remain unchanged
- **AND** caller SHALL receive original flash contents

### Requirement: Operation context
The system SHALL provide comprehensive context to failure model for decision making.

#### Scenario: Context includes metadata
- **WHEN** failure model callback is invoked
- **THEN** `nand_operation_context_t` SHALL include current block/page metadata
- **AND** SHALL include device geometry (total blocks, page size)
- **AND** SHALL include operation timestamp

#### Scenario: Context without metadata
- **WHEN** failure model is configured but metadata backend is not
- **THEN** context SHALL have NULL metadata pointers
- **AND** failure model SHALL still function with available information

### Requirement: No-op failure model
The system SHALL provide a built-in no-op failure model that never injects failures.

#### Scenario: No-op model never fails
- **WHEN** emulator is configured with `nand_no_failure_model`
- **THEN** all `should_fail_*()` callbacks SHALL return false
- **AND** `corrupt_read_data()` SHALL NOT modify data

#### Scenario: Zero overhead
- **WHEN** using no-op failure model
- **THEN** performance overhead SHALL be less than 1% compared to no failure model

### Requirement: Threshold-based failure model
The system SHALL provide a built-in threshold failure model that fails operations after configured cycle limits.

#### Scenario: Block erase limit exceeded
- **WHEN** threshold model is configured with `max_block_erases = 100`
- **AND** a block has been erased 100 times
- **THEN** next erase of that block SHALL fail
- **AND** `should_fail_erase()` SHALL return true

#### Scenario: Page program limit exceeded
- **WHEN** threshold model is configured with `max_page_programs = 1000`
- **AND** a page has been programmed 1000 times
- **THEN** next write to that page SHALL fail

#### Scenario: Within limits
- **WHEN** block erase count is below `max_block_erases`
- **THEN** `should_fail_erase()` SHALL return false
- **AND** operation SHALL proceed normally

### Requirement: Probabilistic failure model
The system SHALL provide a built-in probabilistic failure model using Weibull distribution for realistic wear simulation.

#### Scenario: Fresh flash low failure rate
- **WHEN** probabilistic model is configured with `rated_cycles = 100000`
- **AND** a block has been erased only 100 times (0.1% of rated)
- **THEN** failure probability SHALL be less than 0.01%

#### Scenario: Worn flash high failure rate
- **WHEN** a block has been erased 100000 times (100% of rated cycles)
- **THEN** failure probability SHALL be approximately 50%

#### Scenario: Reproducible failures
- **WHEN** probabilistic model is initialized with same `random_seed`
- **THEN** sequence of failure decisions SHALL be identical across runs
- **AND** tests using fixed seed SHALL be deterministic

### Requirement: Bad block detection
The system SHALL allow failure model to mark blocks as bad based on metadata analysis.

#### Scenario: Model marks block bad
- **WHEN** failure model's `is_block_bad()` returns true for a block
- **THEN** metadata backend SHALL reflect bad block status
- **AND** subsequent operations on that block SHALL honor bad block state

#### Scenario: Bad block operations fail
- **WHEN** a block is marked bad by failure model
- **AND** operation is attempted on that block
- **THEN** operation SHALL fail with `ESP_ERR_FLASH_BAD_BLOCK` if supported

### Requirement: Model lifecycle
The system SHALL properly initialize and clean up failure model resources.

#### Scenario: Model initialization with config
- **WHEN** failure model's `init()` is called with configuration structure
- **THEN** model SHALL allocate necessary resources (RNG state, lookup tables)
- **AND** SHALL return handle for subsequent operations

#### Scenario: Model cleanup
- **WHEN** emulator calls failure model's `deinit()`
- **THEN** model SHALL free all allocated resources
- **AND** SHALL NOT leak memory

### Requirement: Error handling
The system SHALL gracefully handle failure model errors during initialization.

#### Scenario: Model init fails
- **WHEN** failure model's `init()` returns error code
- **THEN** `nand_emul_advanced_init()` SHALL fail with same error code
- **AND** SHALL NOT proceed with emulator initialization

#### Scenario: Model callback crashes
- **WHEN** failure model callback encounters unexpected error
- **THEN** system SHALL document that models must not crash
- **AND** SHALL recommend defensive programming in custom models
