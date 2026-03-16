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

#### Scenario: Query never-erased block metadata
- **WHEN** developer calls backend's `get_block_info()` for a block that has never been erased
- **THEN** backend SHALL return zeroed metadata structure (erase_count 0, timestamps 0, is_bad_block false)

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
- **THEN** backend's `on_byte_write_range()` SHALL NOT be called during write operations

#### Scenario: Record byte write range (conservative approach)
- **WHEN** advanced emulator is initialized with `track_byte_level = true`
- **AND** any write operation occurs
- **THEN** backend SHALL receive `on_byte_write_range()` callback with page_num, byte_offset, length
- **AND** SHALL track per-byte write counts internally

#### Scenario: Backend optimizes zero-deltas away
- **WHEN** backend compares byte write counts against page program count
- **AND** all bytes have same count as page (no outliers)
- **THEN** backend SHALL NOT allocate delta structures
- **AND** SHALL rely on page-level program_count for those bytes

#### Scenario: Backend stores deltas for outliers
- **WHEN** specific bytes have different write count than page program count
- **THEN** backend SHALL store delta: `write_count_delta = byte_write_count - page_program_count`
- **AND** SHALL only allocate storage for outlier bytes
- **AND** `get_byte_deltas()` SHALL return only non-zero deltas

#### Scenario: Pointer lifetime for byte deltas and page metadata
- **WHEN** backend returns pointers in `get_page_info()` (e.g. `byte_deltas`) or `get_byte_deltas()`
- **THEN** those pointers SHALL be owned by the backend
- **AND** SHALL remain valid until the next call to any backend operation that may modify metadata (e.g. on_block_erase, on_page_program, on_byte_write_range, load_snapshot), or until backend deinit
- **AND** caller SHALL NOT free the pointers; caller MAY copy or use the data only within that scope

#### Scenario: Backend does not support byte tracking
- **WHEN** backend's `on_byte_write_range()` returns `ESP_ERR_NOT_SUPPORTED`
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

#### Scenario: Wear variation when no blocks erased (avg = 0)
- **WHEN** `get_stats()` is called and no block has been erased (avg_block_erases is 0)
- **THEN** SHALL set `wear_leveling_variation` to 0.0
- **AND** SHALL NOT divide by zero; 0.0 denotes no spread

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

#### Scenario: Save snapshot with metadata only
- **WHEN** developer calls backend's `save_snapshot()` with filename and timestamp
- **THEN** backend SHALL write binary file with header, block metadata, page metadata, and byte deltas
- **AND** file SHALL include CRC32 checksum computed over the **entire file** (header + all sections) for integrity verification
- **AND** file SHALL contain metadata ONLY (not flash data contents)
- **AND** SHALL complete in less than 10ms for 32MB flash simulation

#### Scenario: Load snapshot restores metadata
- **WHEN** developer calls backend's `load_snapshot()` with snapshot filename
- **THEN** backend SHALL verify CRC32 over the entire file; on mismatch SHALL return error and SHALL NOT modify backend state
- **AND** SHALL restore all block, page, and byte delta metadata
- **AND** SHALL NOT modify flash data contents (mmap file)
- **AND** subsequent queries SHALL return metadata from loaded snapshot
- **AND** SHALL complete in less than 20ms for 32MB flash

#### Scenario: Load snapshot rejects unknown version
- **WHEN** developer calls backend's `load_snapshot()` and file header specifies a version other than supported (e.g. version 1)
- **THEN** backend SHALL return `ESP_ERR_NOT_SUPPORTED`
- **AND** SHALL NOT modify backend state or load any data from the file

#### Scenario: Snapshot roundtrip integrity
- **WHEN** metadata state is saved, cleared, and loaded
- **THEN** all metadata SHALL match original state exactly
- **AND** aggregate statistics SHALL be identical
- **AND** flash data contents remain independent of snapshot

#### Scenario: Use case - wear pattern analysis
- **WHEN** simulation runs for 5000 cycles and saves snapshot
- **AND** later loads that snapshot into fresh emulator
- **THEN** metadata queries show wear state at cycle 5000
- **AND** flash data is fresh (all 0xFF)
- **AND** this is expected behavior for wear pattern analysis

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
