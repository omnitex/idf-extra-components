## ADDED Requirements

### Requirement: Abstract FTL interface
The host-test infrastructure SHALL provide an abstract C++ class `FTLInterface` with virtual methods covering the FTL operations (mount, read, write, erase, unmount) so robustness tests are decoupled from a specific FTL implementation.

#### Scenario: Dhara backed FTL satisfies interface
- **WHEN** a `DharaFTL` subclass implements `FTLInterface`
- **THEN** Catch2 test cases parameterized on `FTLInterface*` run against Dhara without modification

### Requirement: Catch2 robustness test suite
The host-test app SHALL include a Catch2 test suite that exercises the FTL via `FTLInterface` against `nand_fault_sim` for the following scenarios: normal read/write round-trip, write across factory bad blocks, write until block wear-out, detection of read data loss from grave pages, and survival of random bit flips within ECC capability.

#### Scenario: Normal round-trip passes
- **WHEN** no faults are configured and data is written then read back
- **THEN** read data matches written data

#### Scenario: FTL handles factory bad blocks
- **WHEN** factory bad blocks are injected and data is written
- **THEN** the FTL successfully stores and retrieves data by routing around bad blocks

#### Scenario: FTL survives wear-out of some blocks
- **WHEN** a subset of blocks are driven past `max_erase_cycles`
- **THEN** the FTL continues to operate on remaining good blocks without data loss

#### Scenario: FTL detects or recovers from grave page corruption
- **WHEN** pages become grave and return corrupted reads
- **THEN** the FTL either corrects via ECC or reports an error rather than silently returning wrong data

### Requirement: Host-test build restricted to Linux target
The Catch2 host-test app SHALL only build when `IDF_TARGET` is `linux`, enforced in `CMakeLists.txt`.

#### Scenario: Build fails gracefully on non-Linux target
- **WHEN** the host-test app is configured for an ESP32 target
- **THEN** CMake skips the app with an informative message rather than producing a build error
