# nvblock Wear Leveling Specification

## ADDED Requirements

### Requirement: nvblock initialization
The system SHALL initialize nvblock with proper configuration mapping from NAND chip parameters.

#### Scenario: Successful initialization with typical NAND chip
- **WHEN** `spi_nand_flash_init_device()` is called with nvblock selected via Kconfig
- **THEN** system SHALL create nvblock_priv_data structure
- **AND** system SHALL configure nvblock block size equal to NAND page size
- **AND** system SHALL configure blocks per group equal to pages per block
- **AND** system SHALL allocate metadata buffer based on chip geometry
- **AND** system SHALL call `nvb_init()` with configured parameters
- **AND** system SHALL return ESP_OK on success

#### Scenario: Initialization failure due to bad blocks
- **WHEN** nvblock initialization encounters excessive bad blocks
- **THEN** system SHALL return appropriate ESP error code
- **AND** system SHALL clean up allocated resources
- **AND** system SHALL log error with bad block information

### Requirement: Sector read operations
The system SHALL read sectors through nvblock with identical behavior to Dhara implementation.

#### Scenario: Read single sector successfully
- **WHEN** `spi_nand_flash_read_sector()` is called with valid sector_id
- **THEN** system SHALL map sector_id to nvblock block number
- **AND** system SHALL call `nvb_read()` with mapped block
- **AND** system SHALL copy data to user buffer
- **AND** system SHALL return ESP_OK

#### Scenario: Read with ECC error
- **WHEN** underlying NAND read encounters uncorrectable ECC error
- **THEN** system SHALL propagate error from nvblock
- **AND** system SHALL return ESP_ERR_FLASH_BASE + nvblock error code
- **AND** system SHALL log ECC error details

### Requirement: Sector write operations
The system SHALL write sectors through nvblock with wear leveling.

#### Scenario: Write single sector successfully
- **WHEN** `spi_nand_flash_write_sector()` is called with data
- **THEN** system SHALL map sector_id to nvblock block number
- **AND** system SHALL call `nvb_write()` with data
- **AND** nvblock SHALL select physical block with wear leveling
- **AND** system SHALL return ESP_OK on success

#### Scenario: Write triggers garbage collection
- **WHEN** write operation requires new physical block allocation
- **AND** nvblock determines garbage collection is needed
- **THEN** system SHALL perform garbage collection automatically
- **AND** system SHALL complete write operation
- **AND** system SHALL maintain data integrity throughout

#### Scenario: Write to bad block
- **WHEN** nvblock attempts write to physical block
- **AND** hardware indicates bad block (program failure)
- **THEN** nvblock SHALL mark block as bad
- **AND** nvblock SHALL retry write to different physical block
- **AND** system SHALL return ESP_OK if retry succeeds

### Requirement: Trim/delete operations
The system SHALL support sector trimming for wear leveling optimization.

#### Scenario: Trim single sector
- **WHEN** `spi_nand_flash_trim()` is called with sector_id
- **THEN** system SHALL call `nvb_write()` with NULL data
- **AND** nvblock SHALL mark sector as deleted
- **AND** system SHALL make space available for reuse
- **AND** system SHALL return ESP_OK

#### Scenario: Trim non-existent sector
- **WHEN** trim is called on already-trimmed sector
- **THEN** system SHALL handle gracefully without error
- **AND** system SHALL return ESP_OK

### Requirement: Sync operations
The system SHALL flush all cached data to physical NAND.

#### Scenario: Sync with pending writes
- **WHEN** `spi_nand_flash_sync()` is called
- **AND** nvblock has cached metadata changes
- **THEN** system SHALL call `nvb_ioctl(NVB_CMD_CTRL_SYNC)`
- **AND** nvblock SHALL flush all metadata to NAND
- **AND** system SHALL ensure data persistence
- **AND** system SHALL return ESP_OK

#### Scenario: Sync with no pending changes
- **WHEN** sync is called with no cached changes
- **THEN** system SHALL complete immediately
- **AND** system SHALL return ESP_OK

### Requirement: Capacity reporting
The system SHALL report accurate available sector count.

#### Scenario: Query capacity after initialization
- **WHEN** `spi_nand_flash_get_capacity()` is called
- **THEN** system SHALL call `nvb_ioctl(NVB_CMD_GET_BLK_COUNT)`
- **AND** system SHALL return nvblock's available block count
- **AND** returned capacity SHALL account for bad blocks
- **AND** returned capacity SHALL account for wear leveling overhead

#### Scenario: Capacity remains constant
- **WHEN** capacity is queried multiple times
- **THEN** system SHALL return same value (unless bad blocks detected)
- **AND** capacity SHALL NOT change due to normal write/erase operations

### Requirement: Copy sector operations
The system SHALL support efficient sector-to-sector copying.

#### Scenario: Copy one sector to another
- **WHEN** `spi_nand_flash_copy_sector()` is called with src and dst sector IDs
- **THEN** system SHALL read from source sector
- **AND** system SHALL write to destination sector
- **AND** system SHALL maintain data integrity
- **AND** system SHALL return ESP_OK

### Requirement: Bad block handling
The system SHALL detect, mark, and skip bad blocks transparently.

#### Scenario: Detect bad block during operation
- **WHEN** hardware operation fails on a block
- **AND** failure indicates bad block condition
- **THEN** system SHALL call nvblock's mark_bad callback
- **AND** callback SHALL call `nand_mark_bad()` from HAL
- **AND** nvblock SHALL never use that block again
- **AND** operation SHALL retry on good block

#### Scenario: Skip pre-existing bad blocks
- **WHEN** nvblock initializes
- **THEN** system SHALL query each block via is_bad callback
- **AND** system SHALL exclude bad blocks from available pool
- **AND** system SHALL adjust capacity accordingly

### Requirement: Wear leveling distribution
The system SHALL distribute erase operations evenly across blocks.

#### Scenario: Repeated writes to same logical sector
- **WHEN** same logical sector is written 1000+ times
- **THEN** nvblock SHALL rotate physical block assignments
- **AND** no physical block SHALL have erase count exceeding others by > 1
- **AND** system SHALL maintain wear leveling invariant

#### Scenario: Full chip write cycle
- **WHEN** all logical sectors are written
- **AND** garbage collection cycles occur
- **THEN** all good physical blocks SHALL have similar erase counts
- **AND** variance SHALL be within nvblock's design limits (max diff = 1)

### Requirement: Hardware abstraction callbacks
The system SHALL provide hardware operation callbacks to nvblock.

#### Scenario: nvblock read callback
- **WHEN** nvblock calls read callback
- **THEN** callback SHALL extract parent_handle from context
- **AND** callback SHALL call `nand_read()` with page parameters
- **AND** callback SHALL convert ESP errors to nvblock error codes
- **AND** callback SHALL return 0 on success, negative on failure

#### Scenario: nvblock prog callback
- **WHEN** nvblock calls prog callback
- **THEN** callback SHALL call `nand_prog()` from HAL
- **AND** callback SHALL handle bad block detection
- **AND** callback SHALL return -NVB_EFAULT on bad block
- **AND** callback SHALL return 0 on success

#### Scenario: nvblock move callback
- **WHEN** nvblock calls move callback for internal copy
- **THEN** callback SHALL call `nand_copy()` for optimization
- **OR** callback SHALL read source and prog destination if copy unavailable
- **AND** callback SHALL maintain data integrity

### Requirement: Error handling and conversion
The system SHALL properly convert between nvblock and ESP error codes.

#### Scenario: nvblock ENOSPC error
- **WHEN** nvblock returns -NVB_ENOSPC (no space)
- **THEN** system SHALL convert to ESP_ERR_NO_MEM
- **AND** system SHALL log error with context

#### Scenario: nvblock EFAULT error
- **WHEN** nvblock returns -NVB_EFAULT (bad block/hardware fault)
- **THEN** system SHALL convert to ESP_FAIL
- **AND** system SHALL include block number in log

#### Scenario: nvblock EINVAL error
- **WHEN** nvblock returns -NVB_EINVAL (invalid argument)
- **THEN** system SHALL convert to ESP_ERR_INVALID_ARG
- **AND** system SHALL log parameter details

### Requirement: Chip erase operations
The system SHALL support full chip erase bypassing nvblock.

#### Scenario: Erase entire chip
- **WHEN** `spi_nand_erase_chip()` is called
- **THEN** system SHALL call `nand_erase_chip()` directly (bypass nvblock)
- **AND** system SHALL erase all blocks
- **AND** nvblock metadata SHALL be invalidated
- **AND** system SHALL return ESP_OK

### Requirement: Block erase operations
The system SHALL support individual block erase for low-level access.

#### Scenario: Erase single block
- **WHEN** `spi_nand_flash_erase_block()` is called with block number
- **THEN** system SHALL call `nand_erase_block()` directly
- **AND** system SHALL NOT update nvblock metadata
- **AND** system SHALL return ESP_OK on success
