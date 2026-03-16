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
  - Wear leveling efficiency ratio

#### Scenario: Empty flash statistics
- **WHEN** `nand_emul_get_wear_stats()` is called on fresh emulator
- **THEN** SHALL return zeroed statistics
- **AND** `blocks_never_erased` SHALL equal total block count

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
- **THEN** system behavior SHALL match failure model configuration
- **AND** operation MAY fail if failure model checks bad block flag

### Requirement: Export wear map
The system SHALL provide `nand_emul_export_wear_map()` function to export metadata to files.

#### Scenario: Export to JSON format
- **WHEN** developer calls `nand_emul_export_wear_map(handle, "wear.json", "json")`
- **THEN** function SHALL create JSON file with all block metadata
- **AND** JSON SHALL include block numbers, erase counts, timestamps

#### Scenario: Export to CSV format
- **WHEN** developer calls `nand_emul_export_wear_map(handle, "wear.csv", "csv")`
- **THEN** function SHALL create CSV file with columns: block_num, erase_count, first_erase_ts, last_erase_ts
- **AND** CSV SHALL have header row

#### Scenario: Invalid format
- **WHEN** `nand_emul_export_wear_map()` is called with unsupported format string
- **THEN** function SHALL return `ESP_ERR_INVALID_ARG`

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
