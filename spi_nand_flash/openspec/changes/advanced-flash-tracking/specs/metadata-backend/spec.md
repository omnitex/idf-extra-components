# Metadata Backend Capability

## ADDED Requirements

### Requirement: Pluggable metadata storage interface
The system SHALL define an abstract interface `nand_metadata_backend_ops_t` that allows different storage implementations for tracking NAND flash metadata.

#### Scenario: Initialize custom backend
- **WHEN** developer calls `nand_emul_advanced_init()` with custom `metadata_backend_ops`
- **THEN** the emulator SHALL call the backend's `init()` function with provided config
- **AND** SHALL store the backend handle for subsequent operations

#### Scenario: Backend initialization failure
- **WHEN** a metadata backend's `init()` function returns an error code
- **THEN** `nand_emul_advanced_init()` SHALL propagate the error
- **AND** SHALL NOT initialize the emulator

### Requirement: Hierarchical metadata organization
The system SHALL maintain three levels of metadata: block-level, page-level, and byte-level, matching NAND flash architecture.

#### Scenario: Query block metadata
- **WHEN** developer calls backend's `get_block_info()` for a block that has been erased
- **THEN** backend SHALL return metadata including erase count and timestamps

#### Scenario: Query unwritten page metadata
- **WHEN** developer calls backend's `get_page_info()` for a page that has never been written
- **THEN** backend SHALL return zeroed metadata structure

### Requirement: Block-level tracking
The system SHALL track erase operations at block granularity including erase count and timestamps.

#### Scenario: Record block erase
- **WHEN** `nand_emul_erase_block()` successfully erases a block
- **THEN** backend SHALL receive `on_block_erase()` callback with block number and timestamp
- **AND** SHALL increment the block's erase count
- **AND** SHALL update the block's last erase timestamp

#### Scenario: First block erase
- **WHEN** a block is erased for the first time
- **THEN** backend SHALL record both first and last erase timestamps as equal

### Requirement: Page-level tracking
The system SHALL track program operations at page granularity including program count and timestamps.

#### Scenario: Record page program
- **WHEN** `nand_emul_write()` writes data to a page for the first time after erase
- **THEN** backend SHALL receive `on_page_program()` callback with page number and timestamp
- **AND** SHALL increment the page's program count

#### Scenario: Multiple writes to same page
- **WHEN** multiple write operations target the same page without intermediate erase
- **THEN** backend SHALL count each write as a separate program operation

### Requirement: Byte-level tracking (optional)
The system SHALL support optional byte-level delta tracking for partial page programs when enabled in configuration.

#### Scenario: Byte-level tracking disabled
- **WHEN** advanced emulator is initialized with `track_byte_level = false`
- **THEN** backend's `on_byte_delta_write()` SHALL NOT be called during write operations

#### Scenario: Delta tracking for partial page program
- **WHEN** advanced emulator is initialized with `track_byte_level = true`
- **AND** a page is programmed partially (e.g., only first 64 bytes)
- **THEN** backend SHALL receive `on_byte_delta_write()` callback with page_num, byte_offset, length
- **AND** SHALL increment write count delta for affected byte range
- **AND** SHALL track delta from page-level program count (page written N times, byte written N+K times → delta = +K)

#### Scenario: Full page program with byte tracking
- **WHEN** entire page is programmed (all bytes written)
- **AND** byte-level tracking is enabled
- **THEN** backend SHALL increment page program_count
- **AND** SHALL NOT create byte deltas (all bytes written same number of times as page)

#### Scenario: Backend does not support byte tracking
- **WHEN** backend's `on_byte_delta_write()` returns `ESP_ERR_NOT_SUPPORTED`
- **THEN** emulator SHALL continue operation without byte-level tracking
- **AND** SHALL log a warning message

### Requirement: Sparse storage implementation
The default sparse hash backend SHALL only allocate metadata for blocks/pages that have been written or erased, and SHALL use delta encoding for byte-level tracking.

#### Scenario: Unwritten blocks consume no metadata memory
- **WHEN** emulator is initialized with 1024 blocks
- **AND** only 10 blocks are erased or written
- **THEN** sparse backend SHALL allocate metadata for only 10 blocks
- **AND** SHALL NOT allocate 1024 block metadata structures

#### Scenario: Delta encoding efficiency
- **WHEN** workload has 10% partial page programs (e.g., OOB area rewrites)
- **THEN** byte deltas SHALL only be stored for bytes with different write counts
- **AND** most bytes SHALL inherit page-level program count implicitly

#### Scenario: Memory efficiency measurement
- **WHEN** workload writes to 10% of total flash capacity with 1% byte outliers
- **THEN** metadata memory usage SHALL be less than 5% of flash size
- **AND** SHALL be approximately 95% less than dense byte tracking would require

### Requirement: Aggregate statistics
The system SHALL compute aggregate wear statistics across all blocks including min/max/average erase counts and wear variation.

#### Scenario: Calculate wear leveling variation
- **WHEN** developer calls backend's `get_stats()` after mixed operations
- **THEN** backend SHALL return `wear_leveling_variation` calculated as `(max_block_erases - min_block_erases) / avg_block_erases`
- **AND** lower values SHALL indicate better wear leveling (0.0 = perfect, >1.0 = poor)

#### Scenario: Statistics for pristine flash
- **WHEN** `get_stats()` is called on newly initialized flash with no operations
- **THEN** SHALL return all counts as zero
- **AND** SHALL set `blocks_never_erased` equal to total block count
- **AND** SHALL set `wear_leveling_variation` to 0.0 (no variation)

### Requirement: Block iteration
The system SHALL allow iteration over all blocks that have been erased or written.

#### Scenario: Iterate written blocks
- **WHEN** developer calls `iterate_blocks()` with callback function
- **THEN** backend SHALL invoke callback for each block that has non-zero metadata
- **AND** SHALL pass block number and metadata pointer to callback
- **AND** SHALL respect callback return value to continue (true) or stop (false)

#### Scenario: Empty flash iteration
- **WHEN** `iterate_blocks()` is called on fresh emulator with no operations
- **THEN** backend SHALL NOT invoke the callback function

### Requirement: Bad block management
The system SHALL support marking blocks as bad and tracking bad block status in metadata.

#### Scenario: Mark block as bad
- **WHEN** developer calls backend's `set_bad_block(block_num, true)`
- **THEN** backend SHALL set the block's `is_bad_block` flag to true
- **AND** subsequent `get_block_info()` SHALL return metadata with `is_bad_block = true`

#### Scenario: Operations on bad blocks
- **WHEN** a block is marked as bad
- **THEN** subsequent metadata tracking SHALL still function normally
- **AND** bad block status SHALL be visible in statistics and iteration

### Requirement: Backend lifecycle
The system SHALL properly initialize and clean up metadata backend resources.

#### Scenario: Clean shutdown
- **WHEN** developer calls `nand_emul_deinit()` on emulator with advanced tracking
- **THEN** emulator SHALL call backend's `deinit()` function
- **AND** SHALL free all backend-allocated memory
- **AND** SHALL NOT leak memory or file descriptors

#### Scenario: Multiple init/deinit cycles
- **WHEN** developer repeatedly initializes and deinitializes emulator
- **THEN** each cycle SHALL properly clean up previous backend state
- **AND** SHALL NOT accumulate memory leaks

### Requirement: Thread safety (not required)
The metadata backend interface SHALL document that it is designed for single-threaded access matching the emulator's threading model.

#### Scenario: Concurrent access behavior
- **WHEN** documentation is reviewed
- **THEN** it SHALL clearly state that concurrent access is not supported
- **AND** SHALL note that callers must provide external synchronization if needed

### Requirement: Binary snapshot support
The system SHALL support saving and loading binary snapshots of metadata for wear simulation.

#### Scenario: Save snapshot
- **WHEN** developer calls backend's `save_snapshot()` with filename and timestamp
- **THEN** backend SHALL write binary file with header, block metadata, page metadata, and byte deltas
- **AND** file SHALL include checksum for integrity verification
- **AND** SHALL complete in less than 10ms for 32MB flash simulation

#### Scenario: Load snapshot
- **WHEN** developer calls backend's `load_snapshot()` with snapshot filename
- **THEN** backend SHALL verify checksum
- **AND** SHALL restore all block, page, and byte delta metadata
- **AND** subsequent queries SHALL return metadata from loaded snapshot
- **AND** SHALL complete in less than 20ms for 32MB flash

#### Scenario: Snapshot roundtrip integrity
- **WHEN** metadata state is saved, cleared, and loaded
- **THEN** all metadata SHALL match original state exactly
- **AND** aggregate statistics SHALL be identical

### Requirement: JSON export for analysis
The system SHALL support exporting metadata to JSON format for external analysis tools.

#### Scenario: Export to JSON
- **WHEN** developer calls backend's `export_json()` with filename
- **THEN** backend SHALL create human-readable JSON file
- **AND** JSON SHALL include block metadata, page metadata, byte deltas, and aggregate statistics
- **AND** JSON format SHALL be suitable for import into plotting/graphing tools

#### Scenario: JSON completeness
- **WHEN** JSON export is generated
- **THEN** JSON SHALL include all tracked metadata (blocks, pages, deltas)
- **AND** SHALL include device geometry information
- **AND** SHALL include timestamp of export
