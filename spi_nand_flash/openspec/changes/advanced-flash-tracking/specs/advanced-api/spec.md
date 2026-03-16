# Advanced API Capability

## ADDED Requirements

### Requirement: Advanced initialization function
The system SHALL provide `nand_emul_advanced_init()` function that accepts configuration with metadata backend and failure model.

#### Scenario: Initialize with full advanced config
- **WHEN** developer calls `nand_emul_advanced_init()` with metadata backend and failure model
- **THEN** function SHALL initialize base emulation using `base_config`
- **AND** SHALL initialize metadata backend with provided config
- **AND** SHALL initialize failure model with provided config
- **AND** SHALL return `ESP_OK` on success

#### Scenario: Initialize with metadata only
- **WHEN** developer provides metadata backend but `failure_model = NULL`
- **THEN** function SHALL initialize metadata tracking without failure injection
- **AND** operations SHALL track wear but not fail artificially

#### Scenario: Advanced init backward compatible
- **WHEN** `nand_emul_advanced_init()` is used with minimal config
- **THEN** functionality SHALL match `nand_emul_init()` behavior when no advanced features enabled

### Requirement: Configuration structure
The system SHALL define `nand_emul_advanced_config_t` structure that extends base configuration.

#### Scenario: Reuse base config fields
- **WHEN** `nand_emul_advanced_config_t.base_config` is populated
- **THEN** those settings SHALL be used for underlying file mmap emulation
- **AND** SHALL support all existing `nand_file_mmap_emul_config_t` fields

#### Scenario: Configure tracking granularity with byte deltas
- **WHEN** config specifies `track_block_level = true, track_page_level = true, track_byte_level = true`
- **THEN** emulator SHALL track blocks, pages, and byte-level deltas
- **AND** SHALL use delta encoding for byte tracking (not dense per-byte metadata)
- **AND** SHALL pass granularity settings to metadata backend

#### Scenario: track_byte_level is the master control for byte tracking
- **WHEN** `nand_emul_advanced_config_t.track_byte_level = false`
- **THEN** the emulator SHALL NOT call `on_byte_write_range()` on the backend regardless of the backend's own `track_byte_deltas` configuration flag
- **AND** no byte-level deltas SHALL be created
- **WHEN** `track_byte_level = true`
- **THEN** the emulator SHALL call `on_byte_write_range()` for every write; the backend then decides whether to create delta entries based on its own `track_byte_deltas` flag
- **AND** if the backend's `track_byte_deltas = false`, the backend SHALL return `ESP_ERR_NOT_SUPPORTED` from `on_byte_write_range()` and the emulator SHALL silently skip byte-level tracking

### Requirement: Custom timestamp source
The system SHALL allow developers to provide custom timestamp function for metadata.

#### Scenario: Provide custom timestamp
- **WHEN** config includes `get_timestamp` function pointer
- **THEN** emulator SHALL call that function for all metadata timestamps
- **AND** SHALL NOT use default monotonic counter

#### Scenario: Default timestamp behavior
- **WHEN** `get_timestamp = NULL` in config
- **THEN** emulator SHALL use internal monotonic counter starting at 1
- **AND** SHALL increment counter on each operation

### Requirement: Advanced deinitialization
The system SHALL extend `nand_emul_deinit()` to clean up advanced tracking resources.

#### Scenario: Deinit with advanced tracking
- **WHEN** emulator initialized with `nand_emul_advanced_init()` is deinitialized
- **THEN** `nand_emul_deinit()` SHALL call failure model's `deinit()`
- **AND** SHALL call metadata backend's `deinit()`
- **AND** SHALL free advanced tracking structures

#### Scenario: Deinit without advanced tracking
- **WHEN** emulator initialized with basic `nand_emul_init()` is deinitialized
- **THEN** `nand_emul_deinit()` SHALL behave as before with no additional cleanup

### Requirement: Header file organization
The system SHALL define advanced interfaces in new header `nand_emul_advanced.h`.

#### Scenario: Include advanced header
- **WHEN** developer includes `nand_emul_advanced.h`
- **THEN** SHALL have access to `nand_emul_advanced_init()` and related types
- **AND** SHALL automatically include base `nand_linux_mmap_emul.h`

#### Scenario: Backward compatibility
- **WHEN** existing code includes only `nand_linux_mmap_emul.h`
- **THEN** code SHALL compile without changes
- **AND** SHALL not expose advanced types unless explicitly included

### Requirement: Configuration validation
The system SHALL validate advanced configuration before initialization.

#### Scenario: Invalid backend ops
- **WHEN** config provides metadata backend with NULL required function pointers
- **THEN** `nand_emul_advanced_init()` SHALL return `ESP_ERR_INVALID_ARG`
- **AND** SHALL log descriptive error message

#### Scenario: Incompatible tracking settings
- **WHEN** `track_byte_level = true` but backend does not support byte delta tracking
- **THEN** initialization SHALL succeed with warning
- **AND** byte delta tracking SHALL be silently disabled
- **AND** page-level tracking SHALL continue to function

### Requirement: Device geometry caching
The system SHALL cache device geometry in advanced tracking structure for efficient access.

#### Scenario: Store geometry on init
- **WHEN** `nand_emul_advanced_init()` completes successfully
- **THEN** advanced structure SHALL contain cached `total_blocks`, `pages_per_block`, `page_size`
- **AND** these values SHALL be used for context generation without recalculation

#### Scenario: Geometry mismatch detection
- **WHEN** device geometry changes after initialization (invalid scenario)
- **THEN** system SHALL document that geometry must remain constant
- **AND** behavior is undefined if geometry changes

### Requirement: Minimal overhead for disabled features
The system SHALL impose minimal performance overhead when advanced features are not used.

#### Scenario: Legacy init path unchanged
- **WHEN** emulator is initialized with `nand_emul_init()` (not advanced)
- **THEN** `emul_handle->advanced` SHALL be NULL
- **AND** operation handlers SHALL skip all advanced tracking code paths

#### Scenario: Performance measurement
- **WHEN** comparing `nand_emul_init()` vs `nand_emul_advanced_init()` with no-op backend/model
- **THEN** performance difference SHALL be less than 2%

### Requirement: Error propagation
The system SHALL propagate errors from backends and models to callers with clear codes.

#### Scenario: Backend error during operation
- **WHEN** metadata backend callback returns error during write or erase operation
- **THEN** the operation SHALL log a warning with backend name and function name
- **AND** SHALL continue and return the flash operation result (tracking failure is non-fatal)
- **AND** SHALL NOT return the backend error to the caller

#### Scenario: Model init error
- **WHEN** failure model's `init()` returns `ESP_ERR_NO_MEM`
- **THEN** `nand_emul_advanced_init()` SHALL return `ESP_ERR_NO_MEM`
- **AND** SHALL not attempt to initialize metadata backend

### Requirement: Snapshot lifecycle management
The system SHALL support snapshot save/load operations for wear lifetime simulation.

#### Scenario: Save snapshot during simulation
- **WHEN** developer calls `nand_emul_save_snapshot()` after operations
- **THEN** function SHALL delegate to metadata backend's `save_snapshot()` callback
- **AND** SHALL include current timestamp in snapshot
- **AND** SHALL return `ESP_OK` on success

#### Scenario: Load snapshot to restore state
- **WHEN** developer calls `nand_emul_load_snapshot()` with valid snapshot file
- **THEN** function SHALL delegate to metadata backend's `load_snapshot()` callback
- **AND** SHALL restore all block, page, and byte delta metadata
- **AND** SHALL return `ESP_OK` on success

#### Scenario: Snapshot without advanced tracking
- **WHEN** emulator initialized with `nand_emul_init()` (not advanced)
- **AND** developer calls `nand_emul_save_snapshot()`
- **THEN** function SHALL return `ESP_ERR_NOT_SUPPORTED`

#### Scenario: Backend does not support snapshots
- **WHEN** metadata backend has NULL `save_snapshot` or `load_snapshot` callbacks
- **THEN** `nand_emul_save_snapshot()` SHALL return `ESP_ERR_NOT_SUPPORTED`
- **AND** SHALL log descriptive error message

### Requirement: JSON export support
The system SHALL provide JSON export for analysis and visualization.

#### Scenario: Export metadata to JSON
- **WHEN** developer calls `nand_emul_export_json()` with filename
- **THEN** function SHALL delegate to metadata backend's `export_json()` callback
- **AND** JSON SHALL include device geometry, metadata, and statistics
- **AND** SHALL return `ESP_OK` on success

#### Scenario: JSON export without advanced tracking
- **WHEN** emulator initialized without advanced tracking
- **AND** developer calls `nand_emul_export_json()`
- **THEN** function SHALL return `ESP_ERR_NOT_SUPPORTED`

### Requirement: Query API pointer lifetime
The system SHALL document that pointers returned by `nand_emul_get_byte_deltas()` and by `get_page_info()` (e.g. `page_metadata_t.byte_deltas`) are owned by the backend and valid only until the next metadata-modifying operation or deinit; callers SHALL NOT free them.

#### Scenario: Byte deltas pointer use
- **WHEN** developer calls `nand_emul_get_byte_deltas()` and receives `out_deltas` and `out_count`
- **THEN** the pointer SHALL remain valid until the next write, erase, snapshot load, or deinit
- **AND** caller SHALL NOT free the pointer; caller MAY copy the data for longer use
