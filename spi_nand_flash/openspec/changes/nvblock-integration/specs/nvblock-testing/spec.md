# nvblock Testing Specification

## ADDED Requirements

### Requirement: Basic functional tests
The system SHALL include comprehensive functional tests for nvblock operations.

#### Scenario: Test single sector read/write
- **WHEN** test writes data pattern to sector
- **AND** test reads back from same sector
- **THEN** read data SHALL match written data exactly
- **AND** test SHALL pass on both Linux and hardware targets

#### Scenario: Test multiple sector operations
- **WHEN** test writes to 100 consecutive sectors
- **AND** test reads back all sectors
- **THEN** all data SHALL match
- **AND** test SHALL complete within reasonable time (< 10 seconds on hardware)

#### Scenario: Test sector boundary conditions
- **WHEN** test writes to sector 0 (first sector)
- **AND** test writes to last valid sector
- **THEN** both operations SHALL succeed
- **AND** attempting to write beyond capacity SHALL return error

### Requirement: Wear leveling verification tests
The system SHALL verify wear leveling distribution meets specifications.

#### Scenario: Repeated write to same sector distributes wear
- **WHEN** test writes same logical sector 1000 times
- **AND** test examines physical block erase counts (via diagnostic API)
- **THEN** maximum erase count difference SHALL be ≤ 1
- **AND** multiple physical blocks SHALL have been used

#### Scenario: Full capacity write distributes evenly
- **WHEN** test writes to all logical sectors
- **AND** test triggers multiple garbage collection cycles
- **THEN** all good blocks SHALL have similar erase counts
- **AND** erase count variance SHALL be within design limits

#### Scenario: Wear leveling comparison with Dhara
- **WHEN** same test workload runs with Dhara configuration
- **AND** same test workload runs with nvblock configuration
- **THEN** both SHALL show even wear distribution
- **AND** nvblock wear distribution SHALL be at least as good as Dhara

### Requirement: Bad block handling tests
The system SHALL verify proper bad block detection and handling.

#### Scenario: Pre-marked bad blocks excluded
- **WHEN** NAND chip has factory-marked bad blocks
- **AND** system initializes
- **THEN** bad blocks SHALL be detected via is_bad callback
- **AND** bad blocks SHALL NOT be included in available capacity
- **AND** bad blocks SHALL never be written

#### Scenario: Runtime bad block detection
- **WHEN** test simulates write failure to a block (Linux emulation)
- **THEN** system SHALL detect bad block condition
- **AND** system SHALL call mark_bad callback
- **AND** system SHALL retry operation on different block
- **AND** operation SHALL complete successfully

#### Scenario: Bad block persistence across reboot
- **WHEN** block is marked bad
- **AND** system reinitializes (simulated reboot)
- **THEN** previously marked bad block SHALL still be detected as bad
- **AND** block SHALL remain excluded from use

### Requirement: Power-loss simulation tests
The system SHALL verify data integrity after simulated power loss.

#### Scenario: Power loss during write (Linux only)
- **WHEN** test starts write operation
- **AND** test forcibly terminates process mid-operation (Linux)
- **AND** test reinitializes nvblock
- **THEN** nvblock SHALL recover to last valid state
- **AND** no data corruption SHALL occur
- **AND** previously completed writes SHALL be intact

#### Scenario: Power loss during garbage collection (Linux only)
- **WHEN** garbage collection is triggered
- **AND** test simulates power loss during GC
- **AND** test reinitializes
- **THEN** nvblock SHALL recover safely
- **AND** no valid data SHALL be lost

#### Scenario: Repeated power-loss cycles
- **WHEN** test performs 100 write operations with random power loss
- **AND** test verifies data integrity after each recovery
- **THEN** system SHALL maintain data consistency
- **AND** no corruption SHALL be detected

### Requirement: TRIM operation tests
The system SHALL verify correct TRIM/delete functionality.

#### Scenario: TRIM reclaims space
- **WHEN** test writes to fill available capacity
- **AND** test TRIMs 50% of sectors
- **AND** test writes new data to previously trimmed sectors
- **THEN** writes SHALL succeed
- **AND** capacity SHALL be restored

#### Scenario: TRIM improves garbage collection
- **WHEN** test performs writes without TRIM
- **AND** measures garbage collection overhead
- **THEN** test WITH TRIM SHALL have less GC overhead
- **AND** test WITH TRIM SHALL complete faster

#### Scenario: TRIM idempotency
- **WHEN** test TRIMs same sector multiple times
- **THEN** each TRIM SHALL succeed
- **AND** no error SHALL occur
- **AND** capacity SHALL reflect single TRIM

### Requirement: Copy sector tests
The system SHALL verify sector copy operations.

#### Scenario: Copy single sector
- **WHEN** test writes data to source sector
- **AND** test copies to destination sector
- **AND** test reads destination
- **THEN** destination data SHALL match source exactly
- **AND** source data SHALL remain unchanged

#### Scenario: Copy overwrites destination
- **WHEN** destination sector has existing data
- **AND** test performs copy operation
- **THEN** destination SHALL contain source data (overwritten)
- **AND** old destination data SHALL be lost

### Requirement: Sync operation tests
The system SHALL verify sync flushes all data to storage.

#### Scenario: Sync persists pending writes
- **WHEN** test performs multiple writes
- **AND** test calls sync
- **AND** test simulates power loss (Linux)
- **AND** test reinitializes
- **THEN** all writes before sync SHALL be present
- **AND** writes after sync may be lost (expected)

#### Scenario: Sync performance acceptable
- **WHEN** test measures sync operation time
- **THEN** sync SHALL complete within 100ms on typical hardware
- **AND** sync SHALL not block unreasonably

### Requirement: Capacity and limits tests
The system SHALL correctly report capacity and enforce limits.

#### Scenario: Capacity accounts for bad blocks
- **WHEN** chip has N bad blocks
- **AND** test queries capacity
- **THEN** reported capacity SHALL exclude bad blocks
- **AND** capacity SHALL be deterministic across reboots

#### Scenario: Capacity accounts for wear leveling overhead
- **WHEN** test queries capacity
- **THEN** reported capacity SHALL be less than raw chip capacity
- **AND** difference SHALL account for gc_factor configuration
- **AND** calculation SHALL match Dhara's approach

#### Scenario: Write beyond capacity fails gracefully
- **WHEN** test attempts write beyond reported capacity
- **THEN** system SHALL return ESP_ERR_NO_MEM (or equivalent)
- **AND** system SHALL not crash or corrupt data

### Requirement: ECC error handling tests
The system SHALL properly handle ECC errors.

#### Scenario: Correctable ECC error
- **WHEN** read encounters correctable ECC error (simulated)
- **THEN** system SHALL correct error transparently
- **AND** operation SHALL return ESP_OK
- **AND** correct data SHALL be returned

#### Scenario: Uncorrectable ECC error
- **WHEN** read encounters uncorrectable ECC error
- **THEN** system SHALL return error code
- **AND** system SHALL log ECC failure
- **AND** system SHALL not crash

### Requirement: Regression tests against Dhara
The system SHALL verify nvblock provides equivalent functionality to Dhara.

#### Scenario: Identical test suite passes with both
- **WHEN** existing test_spi_nand_flash.c runs with Dhara config
- **AND** same tests run with nvblock config
- **THEN** both SHALL pass all tests
- **AND** both SHALL produce equivalent results

#### Scenario: FATFS works with both implementations
- **WHEN** FATFS example runs with Dhara
- **AND** FATFS example runs with nvblock
- **THEN** both SHALL mount filesystem successfully
- **AND** both SHALL support file operations
- **AND** both SHALL maintain data integrity

### Requirement: Performance benchmarking tests
The system SHALL benchmark nvblock performance against Dhara.

#### Scenario: Sequential write performance
- **WHEN** test writes 1MB sequentially with Dhara
- **AND** test writes 1MB sequentially with nvblock
- **THEN** nvblock performance SHALL be within 20% of Dhara
- **AND** test SHALL report throughput for both

#### Scenario: Random write performance
- **WHEN** test performs 1000 random writes with Dhara
- **AND** test performs 1000 random writes with nvblock
- **THEN** nvblock performance SHALL be within acceptable limits
- **AND** test SHALL report latency distribution

#### Scenario: Read performance
- **WHEN** test performs sequential and random reads
- **THEN** both implementations SHALL have similar read performance
- **AND** difference SHALL be < 10% (reads are mostly pass-through)

### Requirement: Linux host test support
The system SHALL support all tests on Linux target using NAND emulation.

#### Scenario: Linux tests use emulated NAND
- **WHEN** CONFIG_IDF_TARGET_LINUX is defined
- **AND** tests run with nvblock selected
- **THEN** tests SHALL use nand_linux_mmap_emul for storage
- **AND** tests SHALL behave identically to hardware (except performance)

#### Scenario: Linux tests enable power-loss simulation
- **WHEN** running on Linux target
- **THEN** tests SHALL be able to simulate power loss
- **AND** tests SHALL verify recovery behavior
- **AND** hardware tests SHALL skip power-loss tests (not simulatable)

### Requirement: Hardware test coverage
The system SHALL verify nvblock on actual SPI NAND chips.

#### Scenario: Tests run on multiple chip vendors
- **WHEN** tests run on Winbond chip
- **AND** tests run on GigaDevice chip
- **AND** tests run on other supported vendors
- **THEN** all SHALL pass with nvblock
- **AND** vendor-specific quirks SHALL be handled correctly

#### Scenario: Real bad blocks handled
- **WHEN** tests run on chip with actual bad blocks
- **THEN** system SHALL detect and skip them
- **AND** tests SHALL pass despite bad blocks
- **AND** capacity SHALL reflect bad block exclusion

### Requirement: Test documentation and reports
The system SHALL provide clear test documentation and results.

#### Scenario: Test results clearly reported
- **WHEN** tests complete
- **THEN** console output SHALL show pass/fail for each test
- **AND** failures SHALL include diagnostic information
- **AND** performance metrics SHALL be logged

#### Scenario: Test coverage documented
- **WHEN** reviewing test code
- **THEN** each requirement SHALL map to at least one test
- **AND** test names SHALL clearly indicate what they verify
- **AND** comments SHALL explain complex test scenarios
