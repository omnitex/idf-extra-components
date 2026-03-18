## ADDED Requirements

### Requirement: preserve_contents flag suppresses initialization
`nand_file_mmap_emul_config_t` SHALL include a `preserve_contents` boolean field. When `preserve_contents = true`, `nand_emul_mmap_init` SHALL skip both the `ftruncate` call and the `memset` 0xFF initialization, leaving existing file data intact so a valid Dhara journal can be resumed without modification.

#### Scenario: Existing image preserved on open
- **WHEN** `nand_emul_mmap_init` is called with `preserve_contents = true` on an existing file with valid Dhara journal data
- **THEN** the file data is unchanged after init, and `dhara_map_resume` can be called successfully on the resulting device handle

#### Scenario: File still opened read-write when preserving
- **WHEN** `nand_emul_mmap_init` is called with `preserve_contents = true`
- **THEN** the file is opened with `O_RDWR` (mmap write access is still required), but no data is written by init itself

#### Scenario: Default behavior unchanged when preserve_contents is false
- **WHEN** `nand_emul_mmap_init` is called with `preserve_contents = false` (or the field is zero-initialized)
- **THEN** the existing behavior is preserved: `ftruncate` is called to set size and `memset` initializes data to 0xFF

### Requirement: preserve_contents field is backward-compatible
The `preserve_contents` field SHALL default to `false` (zero value) so that existing callers that zero-initialize `nand_file_mmap_emul_config_t` are unaffected.

#### Scenario: Zero-initialized config behaves as before
- **WHEN** an existing caller zero-initializes `nand_file_mmap_emul_config_t` and does not set `preserve_contents`
- **THEN** the emulator behavior is identical to before this change
