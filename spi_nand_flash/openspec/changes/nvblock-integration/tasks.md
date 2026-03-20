# Tasks: nvblock Integration

## 1. nvblock Component Setup

- [x] 1.1 Create nvblock ESP-IDF component repository structure (CMakeLists.txt, idf_component.yml, include/, src/)
- [x] 1.2 Port nvblock source files to ESP-IDF component structure (added as git submodule)
- [x] 1.3 Verify nvblock component builds standalone (libnvblock.a created successfully)
- [x] 1.4 Add nvblock dependency to spi_nand_flash/idf_component.yml (alongside dhara)
- [x] 1.5 Verify both dhara and nvblock dependencies download correctly (4 dependencies processed)

## 2. Kconfig Configuration

- [x] 2.1 Add wear leveling choice menu to Kconfig (SPI_NAND_FLASH_WL_DHARA vs SPI_NAND_FLASH_WL_NVBLOCK)
- [x] 2.2 Set SPI_NAND_FLASH_WL_DHARA as default choice
- [x] 2.3 Add comprehensive help text documenting trade-offs and migration requirements
- [x] 2.4 Verify menuconfig displays correctly (idf.py menuconfig)
- [x] 2.5 Test Kconfig mutual exclusion (only one option selectable)

## 3. Build System Integration

- [x] 3.1 Update CMakeLists.txt to conditionally compile dhara_glue.c or nvblock_glue.c based on Kconfig
- [x] 3.2 Add both dhara and nvblock to REQUIRES (both always linked, conditional compilation determines which glue)
- [x] 3.3 Verify Dhara build still works (CONFIG_SPI_NAND_FLASH_WL_DHARA=y, 100% backward compatible)
- [x] 3.4 Verify nvblock build configuration (CONFIG_SPI_NAND_FLASH_WL_NVBLOCK=y, links successfully)
- [x] 3.5 Confirm only selected implementation linked (verified via build output)

## 4. nvblock Glue Layer - Data Structures

- [x] 4.1 Create src/nvblock_glue.c skeleton file
- [x] 4.2 Define nvblock_context_t structure (nvb_info, nvb_config, metadata buffer, device handle)
- [x] 4.3 Implement runtime nvblock configuration calculation (bsize, bpg, gcnt, spgcnt from chip params)
- [x] 4.4 Implement metadata buffer allocation with runtime sizing (NVB_META_DMP_START + bpg*NVB_META_ADDRESS_SIZE)
- [x] 4.5 Add context cleanup/free functions (implemented in nvblock_deinit)

## 5. nvblock Glue Layer - HAL Callbacks

- [x] 5.1 Implement nvb_read_cb (group/page → HAL nand_read_page)
- [x] 5.2 Implement nvb_write_cb (group/page → HAL nand_write_page)
- [x] 5.3 Implement nvb_erase_cb (group → HAL nand_erase_block)
- [x] 5.4 Implement nvb_isbad_cb (group → HAL nand_is_block_bad)
- [x] 5.5 Implement nvb_markbad_cb (group → HAL nand_mark_block_bad)
- [x] 5.6 Implement nvb_move_cb with optimized nand_copy() from HAL
- [x] 5.7 Add error code mapping (nvblock errors → esp_err_t)

## 6. nvblock Glue Layer - spi_nand_ops Interface

- [x] 6.1 Implement nvblock_init (allocate context, configure nvblock, call nvb_init)
- [x] 6.2 Implement nvblock_read (logical address → nvblock page translation → nvb_read)
- [x] 6.3 Implement nvblock_write (logical address → nvblock page translation → nvb_write)
- [x] 6.4 Implement nvblock_erase (logical address range → nvb_trim)
- [x] 6.5 Implement nvblock_sync (call nvb_flush)
- [x] 6.6 Implement nvblock_get_capacity (query nvb_capacity)
- [x] 6.7 Populate nvblock_ops structure and export for nand_register_dev

## 7. Core Integration

- [x] 7.1 Implement nand_register_dev() and nand_unregister_dev() in nvblock_glue.c (registers nvblock_ops)
- [x] 7.2 Verify no changes to public API (include/spi_nand_flash.h remains unchanged)
- [x] 7.3 Test device initialization with nvblock (verify nvb_init succeeds on real hardware)
- [x] 7.4 Test device registration and handle creation (verify context properly allocated)

## 8. Functional Testing - Basic Operations

- [x] 8.1 Create high-level API tests in host_test/main/test_nand_flash.cpp (tests WL layer, not HAL)
- [x] 8.2 Implement test_nvblock_init (successful initialization, capacity check)
- [x] 8.3 Implement test_nvblock_write_read (single page write/read verification)
- [x] 8.4 Implement test_nvblock_write_read_multi (multi-page sequential write/read)
- [x] 8.5 Implement test_nvblock_erase (verify erase/trim functionality)
- [x] 8.6 Implement test_nvblock_sync (verify flush operation)
- [x] 8.7 Verify tests pass with both Dhara and nvblock on Linux host target (all 10 WL tests passing)


## 9. Functional Testing - Edge Cases

- [x] 9.1 Implement test_nvblock_unaligned_access (verify sub-page read/write handling)
- [x] 9.2 Implement test_nvblock_capacity_limits (verify behavior at capacity boundaries)
- [x] 9.3 Implement test_nvblock_rewrite_same_address (verify wear leveling remapping)
- [x] 9.4 Implement test_nvblock_large_sequential_write (stress test with MB-scale writes)
- [ ] 9.5 Run all edge case tests on hardware (verify expected error handling)


## 10. Bad Block Handling Tests

- [x] 10.1 Implement test_nvblock_preexisting_bad_blocks (verify nvblock skips factory bad blocks)
- [x] 10.2 Implement test_nvblock_runtime_bad_block (simulate write failure → bad block marking)
- [x] 10.3 Implement test_nvblock_bad_block_remapping (verify data moved to good block)
- [x] 10.4 Implement test_nvblock_exhausted_spares (verify graceful degradation when spares exhausted)
- [x] 10.5 Run bad block tests with injected failures (verify robustness)

## 11. Wear Leveling Verification

- [x] 11.1 Implement wear_distribution_test (write hotspot, measure block erase distribution)
- [x] 11.2 Implement wear_leveling_metrics (track per-block erase counts via HAL instrumentation)
- [x] 11.3 Run 10K+ write cycles to single logical address, verify physical block rotation
- [ ] 11.4 Compare nvblock wear distribution to Dhara (document characteristics)
- [x] 11.5 Verify wear leveling meets spec (coefficient of variation <0.3)

## 12. Power-Loss Simulation Tests

- [ ] 12.1 Implement test_nvblock_power_loss_during_write (interrupt write mid-operation → verify recovery)
- [ ] 12.2 Implement test_nvblock_power_loss_during_erase (interrupt erase → verify no corruption)
- [ ] 12.3 Implement test_nvblock_sync_guarantees (verify post-sync data survives power loss)
- [ ] 12.4 Run power-loss tests with random interruption points (100+ iterations)
- [ ] 12.5 Verify no data loss for synced data, acceptable handling for unsynced data

## 13. Performance Benchmarking

- [ ] 13.1 Implement benchmark_sequential_write (measure throughput MB/s)
- [ ] 13.2 Implement benchmark_sequential_read (measure throughput MB/s)
- [ ] 13.3 Implement benchmark_random_access (measure IOPS for 4KB random read/write)
- [ ] 13.4 Implement benchmark_erase_performance (measure trim/erase latency)
- [ ] 13.5 Run benchmarks for both Dhara and nvblock on same hardware
- [ ] 13.6 Verify nvblock performance >90% of Dhara (document actual results)
- [ ] 13.7 Profile nvblock callbacks if below threshold (optimize hot paths)

## 14. Regression Testing

- [x] 14.1 Run full existing Dhara test suite (verify zero regressions)
- [x] 14.2 Run full nvblock test suite (verify all new tests pass)
- [x] 14.3 Test build with Dhara selected (verify backward compatibility)
- [x] 14.4 Test build with nvblock selected (verify new functionality)
- [x] 14.5 Run host tests (if applicable, verify emulator compatibility)
- [ ] 14.6 Run hardware tests on multiple NAND chip types (verify portability)

## 15. Documentation

- [ ] 15.1 Update README.md with wear leveling selection section
- [ ] 15.2 Document Dhara vs nvblock trade-offs (footprint, performance, maturity)
- [ ] 15.3 Add migration procedure (chip erase required, step-by-step instructions)
- [ ] 15.4 Update API reference (no changes, but confirm documentation accuracy)
- [ ] 15.5 Add troubleshooting section for common nvblock issues
- [ ] 15.6 Document nvblock configuration parameters (spgcnt calculation rationale)
- [ ] 15.7 Add example project demonstrating nvblock usage (optional, if time permits)

## 16. Code Quality & Review

- [ ] 16.1 Run static analysis (cppcheck, clang-tidy) on nvblock_glue.c
- [ ] 16.2 Verify zero new warnings/errors introduced
- [ ] 16.3 Add comprehensive comments to nvblock_glue.c (explain HAL mappings, error handling)
- [ ] 16.4 Verify code style consistency with existing codebase
- [ ] 16.5 Self-review checklist: error handling, resource cleanup, edge cases
- [ ] 16.6 Request peer review (if team workflow requires)

## 17. Integration Validation

- [ ] 17.1 Clean build from scratch (idf.py fullclean && idf.py build) - both configs
- [ ] 17.2 Flash and run full test suite on hardware (record test results)
- [ ] 17.3 Long-running stability test (24+ hours continuous read/write with nvblock)
- [ ] 17.4 Verify component registry compatibility (if publishing nvblock component)
- [ ] 17.5 Test example projects work with both Dhara and nvblock
- [ ] 17.6 Confirm CI pipeline passes (if applicable)

## 18. Release Preparation

- [ ] 18.1 Update CHANGELOG.md with nvblock integration (feature addition, no breaking changes)
- [ ] 18.2 Bump component version (minor version increment per semver)
- [ ] 18.3 Tag nvblock as "experimental" in Kconfig help text (initial release)
- [ ] 18.4 Prepare release notes highlighting new feature
- [ ] 18.5 Review open questions in design.md (resolve or defer to Phase 2)
- [ ] 18.6 Final validation: all specs requirements met (cross-check against spec.md files)

## 19. Commit & Merge

- [ ] 19.1 Stage all changes (nvblock_glue.c, Kconfig, CMakeLists.txt, idf_component.yml, tests, docs)
- [ ] 19.2 Create commit with descriptive message: "feat(spi_nand_flash): add nvblock wear leveling support"
- [ ] 19.3 Push branch feat/spi_nand_flash_nvblock_wear_leveling to remote
- [ ] 19.4 Create pull request with comprehensive description (link specs, design, test results)
- [ ] 19.5 Address review feedback (iterate as needed)
- [ ] 19.6 Merge to master after approval (squash or merge per project policy)

## 20. Post-Merge

- [ ] 20.1 Monitor issue tracker for nvblock-related bug reports
- [ ] 20.2 Gather user feedback on nvblock performance/stability
- [ ] 20.3 Plan Phase 2 features if warranted (migration tool, runtime format detection, performance tuning)
- [ ] 20.4 Update documentation based on real-world usage patterns
