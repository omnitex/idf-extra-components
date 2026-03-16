# Integration Capability

## ADDED Requirements

### Requirement: Backward compatible initialization
The system SHALL maintain full backward compatibility with existing `nand_emul_init()` API.

#### Scenario: Existing code unchanged
- **WHEN** existing code uses `nand_emul_init()` function
- **THEN** code SHALL compile without modifications
- **AND** SHALL execute with identical behavior to previous version
- **AND** SHALL NOT require linking against new advanced tracking code

#### Scenario: Existing tests pass
- **WHEN** existing host test suite is executed against new version
- **THEN** all tests SHALL pass without changes
- **AND** test execution time SHALL not increase more than 2%

### Requirement: Integrate metadata tracking with erase operations
The system SHALL call metadata backend during `nand_emul_erase_block()` when advanced tracking is enabled.

#### Scenario: Track block erase
- **WHEN** `nand_emul_erase_block()` successfully erases a block
- **AND** advanced tracking is enabled with metadata backend
- **THEN** function SHALL call backend's `on_block_erase()` with block number and timestamp
- **AND** SHALL continue even if backend callback returns error (log warning)

#### Scenario: Erase without tracking
- **WHEN** `nand_emul_erase_block()` is called on emulator without advanced tracking
- **THEN** function SHALL skip metadata backend calls
- **AND** SHALL execute at same speed as before

### Requirement: Integrate metadata tracking with write operations
The system SHALL call metadata backend during `nand_emul_write()` when advanced tracking is enabled.

#### Scenario: Track page program
- **WHEN** `nand_emul_write()` writes data to a page
- **AND** page-level tracking is enabled
- **THEN** function SHALL call backend's `on_page_program()` for affected pages
- **AND** SHALL pass page numbers and timestamp

#### Scenario: Track byte write ranges (conservative approach)
- **WHEN** `nand_emul_write()` writes any data
- **AND** byte-level tracking is enabled
- **THEN** function SHALL call backend's `on_byte_write_range()` for all affected byte ranges
- **AND** SHALL pass page_num, byte_offset, length for each page touched
- **AND** backend SHALL determine if deltas are needed (comparing against page program count)

#### Scenario: Backend optimizes zero-deltas
- **WHEN** backend receives `on_byte_write_range()` for entire page
- **AND** all bytes in page have same write count as page program count
- **THEN** backend SHALL NOT store deltas (zero-delta optimization)
- **AND** SHALL only store page-level metadata

#### Scenario: Backend creates deltas for outliers
- **WHEN** backend receives `on_byte_write_range()` for partial page (e.g., 64 bytes)
- **AND** page has been programmed multiple times
- **THEN** backend SHALL create deltas for bytes with different write counts
- **AND** SHALL store delta: actual_write_count - page_program_count

#### Scenario: Track multi-page write
- **WHEN** write operation spans multiple pages
- **THEN** metadata tracking SHALL record program operation for each affected page
- **AND** SHALL call `on_byte_write_range()` with appropriate byte range for each page

### Requirement: Integrate failure model with erase operations
The system SHALL check failure model before executing `nand_emul_erase_block()`.

#### Scenario: Failure model rejects erase
- **WHEN** failure model's `should_fail_erase()` returns true
- **THEN** `nand_emul_erase_block()` SHALL return `ESP_ERR_FLASH_OP_FAIL` before erasing
- **AND** SHALL NOT modify flash contents
- **AND** SHALL NOT update metadata

#### Scenario: Failure model allows erase
- **WHEN** failure model's `should_fail_erase()` returns false
- **THEN** erase SHALL proceed normally
- **AND** metadata SHALL be updated after successful erase

### Requirement: Integrate failure model with write operations
The system SHALL check failure model before executing `nand_emul_write()`.

#### Scenario: Failure model rejects write
- **WHEN** failure model's `should_fail_write()` returns true
- **THEN** `nand_emul_write()` SHALL return `ESP_ERR_FLASH_OP_FAIL` before writing
- **AND** SHALL NOT modify flash contents

#### Scenario: Simulate program disturb
- **WHEN** failure model allows write but corrupts data
- **THEN** write SHALL succeed with corrupted data written to flash
- **AND** metadata SHALL reflect successful write

### Requirement: Integrate failure model with read operations
The system SHALL check failure model during `nand_emul_read()` to inject failures and corruption.

#### Scenario: Failure model rejects read
- **WHEN** failure model's `should_fail_read()` returns true
- **THEN** `nand_emul_read()` SHALL return `ESP_ERR_FLASH_OP_FAIL`
- **AND** SHALL NOT copy data to destination buffer

#### Scenario: Inject bit errors on read
- **WHEN** failure model's `should_fail_read()` returns false
- **AND** `corrupt_read_data()` is called after read
- **THEN** failure model MAY flip bits in read buffer
- **AND** caller SHALL receive potentially corrupted data

#### Scenario: Read without failure model
- **WHEN** no failure model is configured
- **THEN** `nand_emul_read()` SHALL always succeed (unless actual error)
- **AND** SHALL return uncorrupted data

### Requirement: Operation context construction
The system SHALL build comprehensive `nand_operation_context_t` before calling failure model.

#### Scenario: Context for erase
- **WHEN** `nand_emul_erase_block()` prepares to check failure model
- **THEN** SHALL construct context with block number, timestamp, device geometry
- **AND** SHALL query metadata backend for current block metadata if available

#### Scenario: Context for write
- **WHEN** `nand_emul_write()` prepares to check failure model
- **THEN** SHALL construct context with block/page numbers, byte offset, operation size
- **AND** SHALL include current page and block metadata

#### Scenario: Context without metadata
- **WHEN** metadata backend is not configured
- **THEN** context SHALL have NULL metadata pointers
- **AND** failure model SHALL still receive operation details

#### Scenario: Single timestamp per operation
- **WHEN** an operation handler (erase, write, read) begins
- **THEN** SHALL call `get_timestamp()` exactly once per operation
- **AND** SHALL reuse that single timestamp for both the failure model context and any subsequent metadata backend calls within the same operation
- **AND** SHALL NOT call `get_timestamp()` a second time for the metadata update, ensuring the failure model and metadata backend see the same logical operation timestamp

#### Scenario: Core updates metadata when failure model marks block bad
- **WHEN** failure model's `is_block_bad()` returns true for a block (e.g. before erase or write)
- **THEN** core SHALL call metadata backend's `set_bad_block(block_num, true)` so metadata reflects the bad block
- **AND** the core handler SHALL immediately return `ESP_ERR_FLASH_BAD_BLOCK`
- **AND** SHALL NOT modify flash contents or update other metadata

### Requirement: Maintain existing statistics
The system SHALL continue to update `CONFIG_NAND_ENABLE_STATS` counters when enabled.

#### Scenario: Stats and advanced tracking coexist
- **WHEN** both `CONFIG_NAND_ENABLE_STATS` and advanced tracking are enabled
- **THEN** both statistics systems SHALL function correctly
- **AND** simple counters SHALL increment as before

#### Scenario: Stats without advanced tracking
- **WHEN** `CONFIG_NAND_ENABLE_STATS` is enabled but advanced tracking is not
- **THEN** statistics SHALL work exactly as before

### Requirement: Error handling consistency
The system SHALL maintain consistent error handling when adding advanced features.

#### Scenario: Backend error does not crash
- **WHEN** metadata backend callback returns unexpected error
- **THEN** operation SHALL log warning and continue
- **OR** SHALL fail gracefully with appropriate error code

#### Scenario: Model error does not crash
- **WHEN** failure model callback returns unexpected value
- **THEN** operation SHALL handle defensively
- **AND** SHALL NOT crash or corrupt memory

### Requirement: Performance overhead limits
The system SHALL limit performance impact of advanced tracking to acceptable levels.

#### Scenario: Block+page tracking overhead
- **WHEN** emulator is configured with block and page level tracking
- **THEN** operation performance SHALL degrade by less than 5% compared to no tracking

#### Scenario: Byte-level delta tracking overhead
- **WHEN** byte-level delta tracking is enabled
- **THEN** write performance degradation SHALL be less than 15%
- **AND** overhead SHALL be proportional to number of partial page programs (not total bytes)
- **AND** full page programs SHALL have minimal overhead

#### Scenario: No-op model overhead
- **WHEN** using no-op failure model
- **THEN** overhead SHALL be less than 1% (mostly function pointer calls)

### Requirement: Memory management integration
The system SHALL properly manage memory for advanced tracking structures.

#### Scenario: Allocate advanced structure on init
- **WHEN** `nand_emul_advanced_init()` is called
- **THEN** SHALL allocate `nand_mmap_emul_handle_t.advanced` structure
- **AND** SHALL initialize all pointers to NULL

#### Scenario: Free advanced structure on deinit
- **WHEN** `nand_emul_deinit()` is called on advanced emulator
- **THEN** SHALL call backend and model deinit functions
- **AND** SHALL free advanced structure
- **AND** SHALL set pointer to NULL

#### Scenario: No memory leak
- **WHEN** emulator is repeatedly initialized and deinitialized
- **THEN** memory usage SHALL remain constant
- **AND** valgrind SHALL report no leaks
