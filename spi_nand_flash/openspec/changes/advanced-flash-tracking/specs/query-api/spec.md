# Query API Capability

## ADDED Requirements

### Requirement: Query block wear metadata
The system SHALL provide `nand_emul_get_block_wear()` function to retrieve metadata for a specific block.

#### Scenario: Query written block
- **WHEN** developer calls `nand_emul_get_block_wear(handle, block_num, &metadata)`
- **AND** block has been erased at least once
- **THEN** function SHALL return `ESP_OK`
- **AND** `metadata` SHALL contain erase count and timestamps

#### Scenario: Query pristine block
- **WHEN** `nand_emul_get_block_wear()` is called for block that has never been erased
- **THEN** function SHALL return `ESP_OK`
- **AND** `metadata` SHALL have zero erase count and timestamps

#### Scenario: Invalid block number
- **WHEN** `nand_emul_get_block_wear()` is called with block number >= total blocks
- **THEN** function SHALL return `ESP_ERR_INVALID_ARG`

### Requirement: Query page wear metadata
The system SHALL provide `nand_emul_get_page_wear()` function to retrieve metadata for a specific page.

#### Scenario: Query programmed page
- **WHEN** developer calls `nand_emul_get_page_wear(handle, page_num, &metadata)`
- **AND** page has been programmed
- **THEN** function SHALL return `ESP_OK`
- **AND** `metadata` SHALL contain program count and timestamps

#### Scenario: Embedded byte_deltas pointer lifetime
- **WHEN** `nand_emul_get_page_wear()` returns `ESP_OK` and byte-level tracking is enabled
- **THEN** the `byte_deltas` field in the returned `page_metadata_t` struct is a backend-owned pointer (same rule as `nand_emul_get_byte_deltas()`)
- **AND** the pointer SHALL remain valid only until the next write, erase, snapshot load, or `nand_emul_deinit()`
- **AND** caller SHALL NOT free the pointer; caller MAY copy the pointed-to data for longer use

#### Scenario: Query unwritten page
- **WHEN** page has never been written after last block erase
- **THEN** `nand_emul_get_page_wear()` SHALL return `ESP_OK`
- **AND** `metadata` SHALL have zero program count

### Requirement: Aggregate wear statistics
The system SHALL provide `nand_emul_get_wear_stats()` function to retrieve aggregate statistics.

#### Scenario: Get comprehensive stats
- **WHEN** developer calls `nand_emul_get_wear_stats(handle, &stats)`
- **THEN** function SHALL return statistics including:
  - Total blocks, pages, bytes written
  - Min/max/average block erase counts
  - Min/max page program counts
  - Count of blocks never erased
  - Count of pages never written
  - Wear leveling variation (lower is better)

#### Scenario: Empty flash statistics
- **WHEN** `nand_emul_get_wear_stats()` is called on fresh emulator
- **THEN** SHALL return zeroed statistics
- **AND** `blocks_never_erased` SHALL equal total block count
- **AND** `wear_leveling_variation` SHALL be 0.0

### Requirement: Iterate worn blocks
The system SHALL provide `nand_emul_iterate_worn_blocks()` function to iterate blocks with metadata.

#### Scenario: Iterate with callback
- **WHEN** developer calls `nand_emul_iterate_worn_blocks(handle, callback, user_data)`
- **THEN** function SHALL invoke callback for each block with non-zero wear
- **AND** SHALL pass block number, metadata pointer, and user_data to callback

#### Scenario: Stop iteration early
- **WHEN** callback function returns false
- **THEN** iteration SHALL stop immediately
- **AND** remaining blocks SHALL not be visited

#### Scenario: No worn blocks
- **WHEN** `nand_emul_iterate_worn_blocks()` is called on fresh emulator
- **THEN** callback SHALL never be invoked
- **AND** function SHALL return `ESP_OK`

### Requirement: Iterate worn pages
The system SHALL provide `nand_emul_iterate_worn_pages()` function to iterate pages with metadata.

#### Scenario: Iterate with callback
- **WHEN** developer calls `nand_emul_iterate_worn_pages(handle, callback, user_data)`
- **THEN** function SHALL invoke callback for each page that has non-zero program wear (`program_count_total + program_count > 0`) or non-zero read activity (`read_count_total + read_count > 0`)
- **AND** SHALL pass page number, metadata pointer, and user_data to callback

#### Scenario: Stop iteration early
- **WHEN** callback function returns false
- **THEN** iteration SHALL stop immediately
- **AND** remaining pages SHALL not be visited

#### Scenario: No worn pages
- **WHEN** `nand_emul_iterate_worn_pages()` is called on fresh emulator
- **THEN** callback SHALL never be invoked
- **AND** function SHALL return `ESP_OK`

#### Scenario: Without advanced tracking
- **WHEN** emulator was initialized with `nand_emul_init()` (not advanced)
- **AND** developer calls `nand_emul_iterate_worn_pages()`
- **THEN** function SHALL return `ESP_ERR_NOT_SUPPORTED`

### Requirement: Mark bad blocks
The system SHALL provide `nand_emul_mark_bad_block()` function to simulate factory bad blocks.

#### Scenario: Mark block as bad
- **WHEN** developer calls `nand_emul_mark_bad_block(handle, block_num)`
- **THEN** function SHALL set bad block flag in metadata
- **AND** SHALL return `ESP_OK`

#### Scenario: Query bad block status
- **WHEN** block is marked bad via `nand_emul_mark_bad_block()`
- **THEN** subsequent `nand_emul_get_block_wear()` SHALL return metadata with `is_bad_block = true`

#### Scenario: Operations on marked bad block
- **WHEN** block is marked bad
- **AND** developer attempts erase or write to that block
- **THEN** the core operation handler SHALL return `ESP_ERR_FLASH_BAD_BLOCK`
- **AND** SHALL NOT modify flash contents or update metadata

### Requirement: Export wear map
The system SHALL provide `nand_emul_export_json()` function to export metadata to JSON for analysis.

#### Scenario: Export to JSON format
- **WHEN** developer calls `nand_emul_export_json(handle, "wear.json")`
- **THEN** function SHALL create JSON file with all block metadata, page metadata, and byte deltas
- **AND** JSON SHALL include aggregate statistics
- **AND** JSON SHALL be human-readable and suitable for import into plotting tools

#### Scenario: JSON structure
- **WHEN** JSON export is created
- **THEN** JSON SHALL have top-level sections: "device", "blocks", "pages", "byte_deltas", "statistics"
- **AND** SHALL include timestamps for all tracked operations

### Requirement: Query without advanced init
The system SHALL handle query functions gracefully when advanced tracking is not enabled.

#### Scenario: Query on basic emulator
- **WHEN** emulator was initialized with `nand_emul_init()` (not advanced)
- **AND** developer calls `nand_emul_get_block_wear()`
- **THEN** function SHALL return `ESP_ERR_NOT_SUPPORTED`

#### Scenario: Clear error message
- **WHEN** query function returns `ESP_ERR_NOT_SUPPORTED`
- **THEN** log SHALL indicate that advanced tracking is not enabled
- **AND** SHALL suggest using `nand_emul_advanced_init()`

### Requirement: Thread-safe query operations
The system SHALL document thread safety guarantees for query operations.

#### Scenario: Query during operations
- **WHEN** documentation is reviewed
- **THEN** SHALL state that queries during concurrent operations require external synchronization
- **AND** SHALL note that metadata may be in intermediate state

#### Scenario: Query after operations
- **WHEN** all operations complete before query
- **THEN** queries SHALL return consistent, up-to-date metadata

### Requirement: Efficient metadata access
The system SHALL implement query functions with minimal overhead.

#### Scenario: Single block query
- **WHEN** `nand_emul_get_block_wear()` is called
- **THEN** function SHALL directly access block metadata without iteration
- **AND** SHALL complete in O(1) time for hash-based backend

#### Scenario: Statistics caching
- **WHEN** `nand_emul_get_wear_stats()` is called multiple times without intervening operations
- **THEN** backend MAY cache aggregate statistics for performance
- **AND** SHALL invalidate cache on any operation that changes metadata

### Requirement: Query byte-level deltas
The system SHALL provide `nand_emul_get_byte_deltas()` function to retrieve delta information for a specific page.

#### Scenario: Query page with byte deltas
- **WHEN** developer calls `nand_emul_get_byte_deltas(handle, page_num, &deltas, &count)`
- **AND** page has partial programs creating byte-level deltas
- **THEN** function SHALL return array of byte_delta_metadata_t structures
- **AND** count SHALL indicate number of bytes with non-zero deltas

#### Scenario: Query page without deltas
- **WHEN** page was only programmed as full page (no partial writes)
- **THEN** `nand_emul_get_byte_deltas()` SHALL return empty array
- **AND** count SHALL be zero

### Requirement: Binary snapshot operations
The system SHALL provide snapshot save/load functions for wear lifetime simulation.

#### Scenario: Save snapshot
- **WHEN** developer calls `nand_emul_save_snapshot(handle, "snapshot.bin")`
- **THEN** function SHALL save all metadata to binary file
- **AND** SHALL include checksum for integrity
- **AND** SHALL complete in less than 10ms for 32MB flash

#### Scenario: Load snapshot
- **WHEN** developer calls `nand_emul_load_snapshot(handle, "snapshot.bin")`
- **THEN** function SHALL restore metadata from binary file
- **AND** SHALL verify checksum before loading
- **AND** SHALL return error if checksum invalid
- **AND** SHALL complete in less than 20ms for 32MB flash

#### Scenario: Snapshot with operations
- **WHEN** snapshot is taken after 5000 operations
- **AND** loaded into fresh emulator
- **THEN** subsequent queries SHALL return same statistics as before save
- **AND** wear pattern SHALL be preserved exactly
