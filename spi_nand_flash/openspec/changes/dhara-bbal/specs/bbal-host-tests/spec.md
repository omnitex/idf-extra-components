## ADDED Requirements

### Requirement: Image generator test case
The host test suite SHALL include a `TEST_CASE("generate_bbal_source_image")` test that creates a fresh mmap-emulated NAND device at `/tmp/dhara-bbal-source.bin`, initializes BBAL and Dhara map over it, writes a deterministic pattern to every sector (sector index `s` repeated to fill the page), syncs, and leaves the file on disk with `keep_dump = true`.

#### Scenario: Source image file is created
- **WHEN** `generate_bbal_source_image` runs to completion
- **THEN** `/tmp/dhara-bbal-source.bin` exists on disk and is non-empty

#### Scenario: Deterministic sector pattern written
- **WHEN** the image generator writes sector `s`
- **THEN** every byte in the page buffer for sector `s` encodes the sector index `s` (e.g. repeated byte value `s & 0xFF` or similar deterministic pattern)

#### Scenario: Image survives deinit
- **WHEN** `spi_nand_flash_deinit_device` is called after sync
- **THEN** the mmap is flushed to disk and the file remains at `/tmp/dhara-bbal-source.bin`

### Requirement: Migration test case with bad block injection
The host test suite SHALL include a `TEST_CASE("bbal_migrate_from_image")` test that opens the source image from `/tmp/dhara-bbal-source.bin` with `preserve_contents = true`, creates a fresh destination device with bad blocks injected at fixed positions (at minimum blocks 3, 7, and 15), migrates using `dhara_migrate`, and verifies every sector's content matches the expected deterministic pattern.

#### Scenario: Migration succeeds with bad blocks in destination
- **WHEN** `bbal_migrate_from_image` runs against a source image and a destination with injected bad blocks
- **THEN** `dhara_migrate` returns 0 and every sector readable from `dst_map` matches the expected pattern

#### Scenario: Destination file cleaned up on success
- **WHEN** the migration test passes
- **THEN** the destination temp file is removed (`keep_dump = false`)

#### Scenario: Source image not modified
- **WHEN** the migration test completes
- **THEN** `/tmp/dhara-bbal-source.bin` still exists and its content is unchanged

### Requirement: Configurable test geometry
All geometry parameters (page size, log2_ppb, num_blocks, gc_ratio) SHALL be `#define`-configurable at the top of the test file to allow adjustment without recompiling the full test suite.

#### Scenario: Default geometry is valid
- **WHEN** the test is compiled with default defines
- **THEN** the computed image size (num_blocks × pages_per_block × (page_size + OOB)) does not exceed available /tmp space (default ≤ 4 MiB)
