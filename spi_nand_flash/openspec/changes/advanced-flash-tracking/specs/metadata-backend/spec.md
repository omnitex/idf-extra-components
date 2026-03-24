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
The system SHALL maintain block-level and page-level metadata for wear and read-stress accounting.

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
- **AND** SHALL increment the page's `program_count` (current erase cycle)
- **AND** SHALL also increment the page's `program_count_total` (lifetime cumulative, never reset)
- **AND** SHALL increment the parent block's `total_page_programs` (current erase cycle)
- **AND** SHALL also increment the parent block's `total_page_programs_total` (lifetime cumulative, never reset)

#### Scenario: Multiple writes to same page
- **WHEN** multiple write operations target the same page without intermediate erase
- **THEN** backend SHALL count each write as a separate program operation in both `program_count` and `program_count_total`

#### Scenario: program_count resets on block erase
- **WHEN** a block is erased
- **THEN** backend SHALL reset `program_count` for all pages in that block to zero (per-cycle component)
- **AND** SHALL NOT reset `program_count_total` (it persists across erase cycles)
- **AND** SHALL retain the page metadata entry in the hash table to preserve the lifetime count
- **AND** SHALL reset the block's `total_page_programs` to zero (per-cycle aggregate)
- **AND** SHALL accumulate the pre-reset value into `total_page_programs_total` before resetting
- **AND** SHALL NOT reset `total_page_programs_total` (lifetime aggregate)

### Requirement: Page read tracking (read-disturb inputs)
The system SHALL count successful host read operations per page when page-level tracking is enabled, using the same per-cycle vs lifetime split as program counters.

#### Scenario: Record page read
- **WHEN** `nand_emul_read()` successfully copies data from flash for a byte range
- **AND** page-level tracking is enabled
- **AND** backend provides `on_page_read`
- **THEN** the emulator SHALL call `on_page_read()` once for each distinct page overlapped by the read range
- **AND** each call SHALL increment that page's `read_count` (current erase cycle)

#### Scenario: Multi-page read
- **WHEN** a single read spans multiple pages
- **THEN** backend SHALL receive one `on_page_read` per overlapped page
- **AND** each page's `read_count` SHALL increment independently

#### Scenario: read_count folds on block erase
- **WHEN** a block is erased
- **THEN** for each page in that block with stored metadata, backend SHALL execute `read_count_total += read_count` before resetting `read_count` to zero
- **AND** SHALL NOT reset `read_count_total` (lifetime folded component)

#### Scenario: Lifetime reads formula
- **WHEN** a consumer needs total reads for a page
- **THEN** lifetime reads SHALL be computed as `read_count_total + read_count`

#### Scenario: Read-only metadata entry
- **WHEN** a page is read but never written in the current design
- **THEN** backend MAY create a sparse page metadata entry solely to record read counts
- **AND** `program_count` MAY remain zero while `read_count` increases

### Requirement: Sparse storage implementation
The default sparse hash backend SHALL only allocate metadata for blocks and pages that have been touched (erased, programmed, or read when read-counting creates an entry).

#### Scenario: Unwritten blocks consume no metadata memory
- **WHEN** emulator is initialized with 1024 blocks
- **AND** only 10 blocks are erased or written
- **THEN** sparse backend SHALL allocate metadata for only 10 blocks
- **AND** SHALL NOT allocate 1024 block metadata structures

#### Scenario: Memory efficiency measurement
- **WHEN** workload writes to a sparse subset of flash (e.g. 10% of blocks)
- **THEN** metadata memory usage SHALL be less than 5% of emulated flash size for typical sparsity

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

### Requirement: Optional wear histograms
The metadata backend MAY expose `get_histograms()` to fill `nand_wear_histograms_t` on demand.

#### Scenario: Valid bin parameters
- **WHEN** `get_histograms()` is called with `out->block_erase_count.n_bins` and `out->page_lifetime_programs.n_bins` each in the range `[2, NAND_WEAR_HIST_MAX_BINS]`
- **AND** both `bin_width` fields are greater than zero
- **THEN** backend SHALL return `ESP_OK` and SHALL fill `count[]` with sample frequencies

#### Scenario: Invalid bin parameters
- **WHEN** `n_bins` or `bin_width` violates the constraints above
- **THEN** backend SHALL return `ESP_ERR_INVALID_ARG`
- **AND** implementation SHOULD either leave all `count[]` unchanged or zero them entirely before returning (no misleading partial frequencies)

#### Scenario: No-op backend without histograms
- **WHEN** backend does not support histograms
- **THEN** `get_histograms` function pointer MAY be NULL
- **AND** emulator `nand_emul_get_wear_histograms()` SHALL surface `ESP_ERR_NOT_SUPPORTED`

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
- **THEN** backend SHALL write binary file with header, block metadata, and page metadata
- **AND** file header SHALL include CRC32 checksum computed over **the header only** (excluding the checksum field itself) for integrity verification
- **AND** file SHALL contain metadata ONLY (not flash data contents)
- **AND** SHALL complete in less than 10ms for 32MB flash simulation

#### Scenario: Load snapshot restores metadata
- **WHEN** developer calls backend's `load_snapshot()` with snapshot filename
- **THEN** backend SHALL verify CRC32 over the header (excluding checksum field); on mismatch SHALL return error and SHALL NOT modify backend state
- **AND** SHALL restore all block and page metadata
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
- **AND** JSON SHALL include block metadata, page metadata, and aggregate statistics
- **AND** JSON format SHALL be suitable for import into plotting/graphing tools

#### Scenario: JSON completeness
- **WHEN** JSON export is generated
- **THEN** JSON SHALL include all tracked metadata (blocks, pages)
- **AND** SHALL include device geometry information
- **AND** SHALL include timestamp of export
