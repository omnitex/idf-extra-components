## 1. BBAL Header and Core Implementation

- [ ] 1.1 Create `include/dhara_bbal.h` with `dhara_bbal_t` struct definition (`logical_nand` as first field, `phys_nand`, `logical_to_phys`, `num_logical`, `num_bad`)
- [ ] 1.2 Add `dhara_bbal_init` and `dhara_bbal_deinit` declarations to `include/dhara_bbal.h` with full doc-comments
- [ ] 1.3 Create `src/dhara_bbal.c` with `dhara_bbal_init`: malloc, OOB scan loop, realloc, and `logical_nand` geometry setup
- [ ] 1.4 Implement `dhara_bbal_deinit` in `src/dhara_bbal.c`
- [ ] 1.5 Implement `bbal_from_nand` and `phys_block` / `phys_page` inline helpers in `src/dhara_bbal.c`
- [ ] 1.6 Implement all 7 `dhara_nand_*` callback wrappers in `src/dhara_bbal.c`: `is_bad`, `mark_bad`, `erase`, `prog`, `is_free`, `read`, `copy`

## 2. Migration Utility

- [ ] 2.1 Create `migration/dhara_migration.h` with `dhara_migrate` declaration and full doc-comment
- [ ] 2.2 Create `migration/dhara_migration.c` with `dhara_migrate` implementation: capacity check, sector loop, `find`/`read`/`write`/`trim` dispatch, progress callback, final `sync`

## 3. mmap Emulator: preserve_contents Flag

- [ ] 3.1 Add `bool preserve_contents` field to `nand_file_mmap_emul_config_t` in `include/nand_linux_mmap_emul.h`
- [ ] 3.2 Update `nand_emul_mmap_init` in `src/nand_linux_mmap_emul.c` to skip `ftruncate` and `memset(0xFF)` when `preserve_contents == true`

## 4. Build System

- [ ] 4.1 Add `src/dhara_bbal.c` to `SRCS` in `CMakeLists.txt`
- [ ] 4.2 Add `migration/dhara_migration.c` to `SRCS` in `CMakeLists.txt`
- [ ] 4.3 Add `migration/` to `INCLUDE_DIRS` (or equivalent) so `dhara_migration.h` is discoverable

## 5. Host Tests

- [ ] 5.1 Create `host_test/test_dhara_bbal.cpp` (or extend existing host test file) with configurable geometry `#define`s at the top
- [ ] 5.2 Implement `TEST_CASE("generate_bbal_source_image")`: fresh emulator init, BBAL init, map init + resume, write deterministic sector patterns, sync, deinit (file kept at `/tmp/dhara-bbal-source.bin`)
- [ ] 5.3 Implement `TEST_CASE("bbal_migrate_from_image")`: open source with `preserve_contents=true`, create dst emulator, inject bad blocks (blocks 3, 7, 15), BBAL init dst, map init + clear + sync dst, call `dhara_migrate`, verify all sectors match expected pattern, cleanup
- [ ] 5.4 Verify both test cases build and pass under `idf.py build` / `cmake --build` for the host_test target

## 6. Validation

- [ ] 6.1 Run `openspec validate dhara-bbal` and resolve any formatting errors
- [ ] 6.2 Confirm `openspec status dhara-bbal` shows all artifacts complete
