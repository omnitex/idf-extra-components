# Advanced Flash Tracking - Implementation Tasks

## Phase 1: Core Infrastructure

**Goal**: Foundation for advanced tracking system - data structures, interfaces, initialization

- [ ] P1.T1: Define Core Data Structures
  - Complexity: Low | Files: Create `include/nand_emul_advanced.h`
  - Define `byte_delta_metadata_t`, `page_metadata_t`, `block_metadata_t`
  - Define `nand_wear_stats_t` for aggregate statistics
  - Define `nand_operation_context_t` for failure models
  - Add explicit padding for cross-platform compatibility
  - Add static assertions for struct sizes
  - Acceptance: All structs compile without warnings; static assertions pass (header size = 64 bytes); Doxygen comments for all fields

- [ ] P1.T2: Define Backend Interface
  - Complexity: Low | Dependencies: P1.T1 | Files: `include/nand_emul_advanced.h`
  - Define `nand_metadata_backend_ops_t` vtable structure
  - Add lifecycle methods: `init()`, `deinit()`
  - Add operation callbacks: `on_block_erase()`, `on_page_program()`, `on_byte_write_range()`
  - Add query methods: `get_block_info()`, `get_page_info()`, `get_byte_deltas()`
  - Add iteration methods: `iterate_blocks()`, `iterate_pages()`
  - Add snapshot methods: `save_snapshot()`, `load_snapshot()`
  - Add export method: `export_json()`
  - Acceptance: Interface compiles with function pointer syntax; all methods documented with parameter descriptions

- [ ] P1.T3: Define Failure Model Interface
  - Complexity: Low | Dependencies: P1.T1 | Files: `include/nand_emul_advanced.h`
  - Define `nand_failure_model_ops_t` vtable structure
  - Add lifecycle methods: `init()`, `deinit()`
  - Add decision methods: `should_fail_read()`, `should_fail_write()`, `should_fail_erase()`
  - Add corruption method: `corrupt_read_data()`
  - Add health check: `is_block_bad()`
  - Acceptance: Interface compiles; all methods documented

- [ ] P1.T4: Define Configuration Structures
  - Complexity: Low | Dependencies: P1.T2, P1.T3 | Files: `include/nand_emul_advanced.h`
  - Define `nand_emul_advanced_config_t`
  - Include base config (`nand_file_mmap_emul_config_t`)
  - Add backend and failure model pointers
  - Add tracking flags (block/page/byte level)
  - Add optional timestamp function pointer
  - Acceptance: Config struct compiles; default values documented

- [ ] P1.T5: Extend Internal Handle
  - Complexity: Medium | Dependencies: P1.T4 | Files: `src/nand_linux_mmap_emul.c` (private header section)
  - Add `advanced` field to `nand_mmap_emul_handle_t` (struct pointer)
  - Define internal `nand_advanced_context_t` structure
  - Include backend handle, failure model handle, config flags
  - Add cached device geometry (total_blocks, pages_per_block, page_size)
  - Add timestamp function pointer and default counter
  - Acceptance: Handle compiles with new field; `advanced` field is NULL by default (backward compatibility)

- [ ] P1.T6: Implement `nand_emul_advanced_init()`
  - Complexity: Medium | Dependencies: P1.T5 | Files: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
  - Create advanced init function with config parameter
  - Call base `nand_emul_init()` first
  - Allocate `advanced` context structure
  - Initialize metadata backend (call `backend_ops->init()`)
  - Initialize failure model if provided (call `failure_ops->init()`)
  - Cache device geometry; set timestamp function (default or user-provided)
  - Acceptance: Returns `ESP_OK` on success; `ESP_ERR_NO_MEM` if allocation fails; backend properly initialized

- [ ] P1.T7: Implement `nand_emul_advanced_deinit()`
  - Complexity: Low | Dependencies: P1.T6 | Files: `src/nand_linux_mmap_emul.c`
  - Call backend `deinit()` if present
  - Call failure model `deinit()` if present
  - Free `advanced` context; call base `nand_emul_deinit()`
  - Acceptance: No memory leaks (verify with valgrind); handles NULL pointers gracefully

- [ ] P1.T8: Implement No-op Backend
  - Complexity: Low | Dependencies: P1.T2 | Files: Create `src/backends/noop_backend.c`
  - Implement all backend operations as no-ops
  - `init()` returns `ESP_OK`; all query methods return zeros/empty results
  - Iteration methods never call callback
  - Acceptance: Compiles and links; can be used in `nand_emul_advanced_init()`; unit test: init with no-op backend, verify no crash

- [ ] P1.T9: Implement No-op Failure Model
  - Complexity: Low | Dependencies: P1.T3 | Files: Create `src/failure_models/noop_failure_model.c`
  - Implement all failure model operations as no-ops
  - All `should_fail_*()` return `false`; `corrupt_read_data()` does nothing; `is_block_bad()` returns `false`
  - Acceptance: Compiles and links; can be used in `nand_emul_advanced_init()`

- [ ] P1.T10: Write Basic Unit Test
  - Complexity: Low | Dependencies: P1.T8, P1.T9 | Files: Create `host_test/test_advanced_init.c`
  - Test: Init with no-op backend and failure model
  - Test: Verify handle is created; perform basic read/write operations; deinit and verify no memory leaks
  - Acceptance: All tests pass; no valgrind errors

---

## Phase 2: Sparse Hash Backend

**Goal**: Implement production-ready metadata storage backend with block and page tracking

- [ ] P2.T1: Implement Hash Table Core
  - Complexity: High | Dependencies: P1.T2 | Files: Create `src/backends/hash_table.c`, `src/backends/hash_table.h`
  - Define `hash_table_t` structure (buckets, capacity, count, load_factor)
  - Define `hash_node_t` structure (key, next pointer, flexible array for data)
  - Implement `hash_table_create(capacity, entry_size, load_factor)`
  - Implement `hash_table_destroy()`; hash function (Knuth multiplicative hash); `next_power_of_2()` utility
  - Acceptance: Unit test: create table, verify initial state; destroy empty table, no leaks

- [ ] P2.T2: Implement Hash Table Insert/Lookup
  - Complexity: Medium | Dependencies: P2.T1 | Files: `src/backends/hash_table.c`
  - Implement `hash_table_get(table, key)` - returns existing node or NULL
  - Implement `hash_table_get_or_insert(table, key)` - returns existing or creates new
  - Handle collision chaining (linked list); zero-initialize new node data
  - Acceptance: Unit test: insert 100 entries, verify all can be retrieved; insert duplicate key, verify returns same node

- [ ] P2.T3: Implement Hash Table Rehashing
  - Complexity: Medium | Dependencies: P2.T2 | Files: `src/backends/hash_table.c`
  - Implement `hash_table_rehash(table, new_capacity)`
  - Allocate new bucket array; reinsert all nodes; free old bucket array
  - Trigger rehash when load factor exceeded in `get_or_insert()`
  - Acceptance: Unit test: insert 1000 entries, verify automatic rehashing; all entries retrievable after rehash

- [ ] P2.T4: Implement Hash Table Removal
  - Complexity: Medium | Dependencies: P2.T2 | Files: `src/backends/hash_table.c`
  - Implement `hash_table_remove(table, key)`
  - Unlink node from collision chain; free node memory; decrement count
  - Acceptance: Unit test: insert, remove, verify not found; remove from collision chain

- [ ] P2.T5: Implement Hash Table Iteration
  - Complexity: Low | Dependencies: P2.T2 | Files: `src/backends/hash_table.c`
  - Implement `hash_table_iterate(table, callback, user_data)`
  - Visit all buckets; follow collision chains; call callback for each node; stop if callback returns false
  - Acceptance: Unit test: insert 50 entries, iterate, verify all visited

- [ ] P2.T6: Implement Sparse Hash Backend Structure
  - Complexity: Low | Dependencies: P2.T1 | Files: Create `src/backends/sparse_hash_backend.c`
  - Define `sparse_hash_backend_t` structure
  - Include `block_table`, `page_table` pointers
  - Include configuration (track_byte_deltas, load_factor); cached device geometry; cached statistics and dirty flag
  - Acceptance: Structure compiles

- [ ] P2.T7: Implement Sparse Hash Backend Init
  - Complexity: Medium | Dependencies: P2.T6, P2.T1 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_init()` function; parse `sparse_hash_backend_config_t`
  - Allocate backend structure; create block hash table (estimate 10% of total blocks)
  - Create page hash table (estimate 10% of total pages); cache configuration
  - Acceptance: Unit test: init with valid config, verify success; init with NULL config, verify error

- [ ] P2.T8: Implement Sparse Hash Backend Deinit
  - Complexity: Medium | Dependencies: P2.T7 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_deinit()` function
  - Free all byte delta arrays in page table; destroy page hash table; destroy block hash table; free backend structure
  - Acceptance: No memory leaks (valgrind); handles NULL backend gracefully

- [ ] P2.T9: Implement Block Erase Tracking
  - Complexity: Medium | Dependencies: P2.T8, P2.T4 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_on_block_erase()`
  - Get or insert block metadata node; increment erase count; update first/last erase timestamps
  - Remove all page metadata in block (pages invalidated); free byte deltas for removed pages; set stats dirty flag
  - Acceptance: Unit test: erase block, verify count incremented; erase block with pages, verify pages removed

- [ ] P2.T10: Implement Block Query
  - Complexity: Low | Dependencies: P2.T9 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_get_block_info()`
  - Lookup block in hash table; if not found, return zeros (block never erased); if found, copy metadata to output
  - Acceptance: Unit test: query never-erased block, verify zeros; erase block, query, verify count=1

- [ ] P2.T11: Implement Page Program Tracking
  - Complexity: Medium | Dependencies: P2.T10 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_on_page_program()`
  - Get or insert page metadata node; increment program count; update first/last program timestamps
  - Update parent block's total_page_programs; set stats dirty flag
  - Acceptance: Unit test: program page, verify count incremented; verify block aggregate updated

- [ ] P2.T12: Implement Page Query
  - Complexity: Low | Dependencies: P2.T11 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_get_page_info()`
  - Lookup page in hash table; if not found, return zeros; if found, copy metadata to output
  - Acceptance: Unit test: query never-programmed page, verify zeros; program page, query, verify count=1

- [ ] P2.T13: Implement Block Iteration
  - Complexity: Low | Dependencies: P2.T10, P2.T5 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_iterate_blocks()`
  - Use `hash_table_iterate()` on block table; call user callback for each block; stop if callback returns false
  - Acceptance: Unit test: erase 10 blocks, iterate, verify all visited

- [ ] P2.T14: Implement Page Iteration
  - Complexity: Low | Dependencies: P2.T12, P2.T5 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_iterate_pages()`
  - Use `hash_table_iterate()` on page table; call user callback for each page; stop if callback returns false
  - Acceptance: Unit test: program 20 pages, iterate, verify all visited

- [ ] P2.T15: Implement Statistics Calculation
  - Complexity: Medium | Dependencies: P2.T13, P2.T14 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_get_stats()`
  - Check if stats are dirty, recompute if needed
  - Iterate blocks: calculate min/max/avg erase counts; iterate pages: calculate min/max program counts
  - Calculate `wear_leveling_variation = (max - min) / avg`; cache results, clear dirty flag
  - Acceptance: Unit test: erase blocks with varying counts, verify min/max/avg; verify wear_leveling_variation formula

- [ ] P2.T16: Write Comprehensive Backend Unit Tests
  - Complexity: Medium | Dependencies: P2.T15 | Files: Create `host_test/test_sparse_hash_backend.c`
  - Test: Init/deinit lifecycle; block erase tracking (single, multiple, repeated); page program tracking
  - Test: Block/page query; iteration (blocks, pages, early termination); statistics; memory efficiency (sparse storage)
  - Acceptance: All tests pass; no memory leaks

---

## Phase 3: Integration with Core Operations

**Goal**: Hook metadata tracking and failure injection into read/write/erase operations

- [ ] P3.T1: Add Timestamp Helper
  - Complexity: Low | Dependencies: P1.T6 | Files: `src/nand_linux_mmap_emul.c`
  - Implement `default_timestamp()` function (monotonic counter)
  - In `nand_emul_advanced_init()`, set timestamp function (user-provided or default)
  - Acceptance: Unit test: verify counter increments on each call

- [ ] P3.T2: Modify `nand_emul_erase_block()` - Failure Check
  - Complexity: Low | Dependencies: P1.T6 | Files: `src/nand_linux_mmap_emul.c`
  - Before erase operation, check if `advanced` is set
  - If failure model present, build `nand_operation_context_t`; call `should_fail_erase()`
  - If true, log warning and return `ESP_ERR_FLASH_OP_FAIL`
  - Acceptance: Unit test: configure threshold model (max 1 erase), verify 2nd erase fails

- [ ] P3.T3: Modify `nand_emul_erase_block()` - Metadata Tracking
  - Complexity: Low | Dependencies: P3.T2, P2.T9 | Files: `src/nand_linux_mmap_emul.c`
  - After successful erase, check if `advanced` is set
  - If `track_block_level` enabled and metadata backend present, call `on_block_erase()` with block number and timestamp
  - Acceptance: Unit test: erase block with tracking enabled, verify metadata updated

- [ ] P3.T4: Modify `nand_emul_write()` - Failure Check
  - Complexity: Medium | Dependencies: P1.T6 | Files: `src/nand_linux_mmap_emul.c`
  - Before write operation, check if `advanced` is set
  - If failure model present, build `nand_operation_context_t`; query block and page metadata for context (optional)
  - Call `should_fail_write()`; if true, log warning and return `ESP_ERR_FLASH_OP_FAIL`
  - Acceptance: Unit test: configure threshold model, verify write fails after limit

- [ ] P3.T5: Modify `nand_emul_write()` - Page Tracking
  - Complexity: Medium | Dependencies: P3.T4, P2.T11 | Files: `src/nand_linux_mmap_emul.c`
  - After successful write, check if `advanced` is set
  - Calculate affected pages (first_page, last_page)
  - For each page, if `track_page_level` enabled, call `on_page_program()`
  - Acceptance: Unit test: write spanning single page, verify page count=1; write spanning 3 pages, verify all 3 tracked

- [ ] P3.T6: Modify `nand_emul_write()` - Byte Range Tracking
  - Complexity: Medium | Dependencies: P3.T5 | Files: `src/nand_linux_mmap_emul.c`
  - After page tracking, if `track_byte_level` enabled
  - For each affected page, calculate byte range within page; call `on_byte_write_range()` with page_num, byte_offset, length
  - Acceptance: Unit test: write full page then partial, verify backend tracks correctly; multi-page, verify byte ranges calculated correctly

- [ ] P3.T7: Modify `nand_emul_read()` - Failure Check
  - Complexity: Low | Dependencies: P1.T6 | Files: `src/nand_linux_mmap_emul.c`
  - After read operation, check if `advanced` is set
  - If failure model present, build `nand_operation_context_t`; call `should_fail_read()`
  - If true, log warning and return `ESP_ERR_FLASH_OP_FAIL`
  - Acceptance: Unit test: configure failure model to fail reads, verify error

- [ ] P3.T8: Modify `nand_emul_read()` - Data Corruption
  - Complexity: Low | Dependencies: P3.T7 | Files: `src/nand_linux_mmap_emul.c`
  - After read (before returning), if failure model present, call `corrupt_read_data(data, len)`
  - Model modifies data buffer in-place (bit flips)
  - Acceptance: Unit test: configure probabilistic model, verify some reads have bit errors

- [ ] P3.T9: Write Integration Tests
  - Complexity: Medium | Dependencies: P3.T3, P3.T6, P3.T8 | Files: Create `host_test/test_integration.c`
  - Test: Erase block, verify metadata updated; write single/multi page, verify page metadata
  - Test: Write partial page then full page, verify byte deltas; read with corruption model, verify bit errors injected
  - Test: Backward compatibility (init without advanced, verify no crash)
  - Acceptance: All tests pass; existing NAND tests still pass

---

## Phase 4: Query API & Snapshots

**Goal**: Expose metadata to users via query functions and implement snapshot save/load

- [ ] P4.T1: Implement `nand_emul_get_block_wear()`
  - Complexity: Low | Dependencies: P2.T10 | Files: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
  - Validate handle and block number; check if advanced tracking enabled; call backend `get_block_info()`; return result
  - Acceptance: Unit test: get block wear for never-erased block, verify zeros; erase block twice, get wear, verify count=2

- [ ] P4.T2: Implement `nand_emul_get_page_wear()`
  - Complexity: Low | Dependencies: P2.T12 | Files: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
  - Validate handle and page number; check if advanced tracking enabled; call backend `get_page_info()`; return result
  - Acceptance: Unit test: get page wear for never-programmed page, verify zeros; program page 5 times, get wear, verify count=5

- [ ] P4.T3: Implement `nand_emul_get_byte_deltas()`
  - Complexity: Low | Dependencies: P2.T12 | Files: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
  - Validate handle and page number; check if byte-level tracking enabled
  - Call backend `get_byte_deltas()`; return filtered delta array (caller must free)
  - Acceptance: Unit test: get deltas for page with no outliers, verify empty; program page then partial write, get deltas, verify outliers

- [ ] P4.T4: Implement `nand_emul_get_wear_stats()`
  - Complexity: Low | Dependencies: P2.T15 | Files: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
  - Validate handle; check if advanced tracking enabled; call backend `get_stats()`; return aggregate statistics
  - Acceptance: Unit test: perform mixed operations, get stats, verify correctness

- [ ] P4.T5: Implement `nand_emul_iterate_worn_blocks()`
  - Complexity: Low | Dependencies: P2.T13 | Files: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
  - Validate handle and callback; check if advanced tracking enabled
  - Call backend `iterate_blocks()`; pass through callback and user_data
  - Acceptance: Unit test: erase 5 blocks, iterate, verify all visited

- [ ] P4.T6: Implement `nand_emul_mark_bad_block()`
  - Complexity: Low | Dependencies: P2.T10 | Files: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
  - Validate handle and block number; check if advanced tracking enabled; call backend `set_bad_block(block_num, true)`
  - Acceptance: Unit test: mark block bad, query, verify flag set

- [ ] P4.T7: Implement Snapshot Header Structure
  - Complexity: Low | Dependencies: None | Files: `src/backends/snapshot_format.h`
  - Define `snapshot_header_t` with `__attribute__((packed))`; static assertion: `sizeof(snapshot_header_t) == 64`
  - Define magic number: `0x4E414E44`; version: `0x01`; flags bitfield (block/page/byte tracking)
  - Acceptance: Header compiles; static assertion passes

- [ ] P4.T8: Implement Snapshot Save (Sparse Hash Backend)
  - Complexity: High | Dependencies: P4.T7, P2.T15 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_save_snapshot()`; write header with placeholders
  - Iterate blocks, write block metadata section; iterate pages, write page metadata section
  - Accumulate byte deltas, write byte delta section; calculate CRC32; rewrite header with correct offsets and checksum
  - Acceptance: Unit test: save empty snapshot, verify header correct; erase blocks, program pages, save snapshot, verify file size

- [ ] P4.T9: Implement Snapshot Load (Sparse Hash Backend)
  - Complexity: High | Dependencies: P4.T8 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_load_snapshot()`; read header, validate magic and CRC32
  - Clear existing metadata; read block metadata section; read byte deltas; read page metadata, reconstruct byte delta pointers
  - Set stats dirty flag
  - Acceptance: Unit test: save then load, verify metadata identical; load corrupted snapshot, verify error

- [ ] P4.T10: Implement `nand_emul_save_snapshot()`
  - Complexity: Low | Dependencies: P4.T8 | Files: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
  - Validate handle and filename; check if advanced tracking enabled; get current timestamp; call backend `save_snapshot()`
  - Acceptance: Unit test: save snapshot, verify file created

- [ ] P4.T11: Implement `nand_emul_load_snapshot()`
  - Complexity: Low | Dependencies: P4.T9 | Files: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
  - Validate handle and filename; check if advanced tracking enabled; call backend `load_snapshot()`; verify success
  - Acceptance: Unit test: save, deinit, init, load, verify metadata restored

- [ ] P4.T12: Implement JSON Export (Sparse Hash Backend)
  - Complexity: Medium | Dependencies: P2.T15 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_export_json()`; write JSON header with device info
  - Iterate blocks/pages, write as JSON objects; include byte deltas as nested arrays; write aggregate statistics
  - Acceptance: Unit test: export to JSON, verify valid JSON (use jq to parse); verify structure matches specification

- [ ] P4.T13: Implement `nand_emul_export_json()`
  - Complexity: Low | Dependencies: P4.T12 | Files: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
  - Validate handle and filename; check if advanced tracking enabled; call backend `export_json(filename)`
  - Acceptance: Unit test: export JSON, parse with external tool

- [ ] P4.T14: Write Query API Unit Tests
  - Complexity: Medium | Dependencies: P4.T6, P4.T11, P4.T13 | Files: `host_test/test_query_api.c`
  - Test all query functions (block, page, byte deltas, stats); test iteration; test bad block marking
  - Test snapshot save/load roundtrip; test JSON export structure
  - Acceptance: All tests pass

---

## Phase 5: Built-in Failure Models

**Goal**: Implement threshold and probabilistic failure models for realistic testing

- [ ] P5.T1: Implement Threshold Model Structure
  - Complexity: Low | Dependencies: P1.T3 | Files: Create `src/failure_models/threshold_failure_model.c`
  - Define `threshold_failure_config_t`; define internal state structure (config copy)
  - Declare `nand_threshold_failure_model` ops variable
  - Acceptance: Structure compiles

- [ ] P5.T2: Implement Threshold Model Init/Deinit
  - Complexity: Low | Dependencies: P5.T1 | Files: `src/failure_models/threshold_failure_model.c`
  - Implement `threshold_init()` - allocate state, copy config
  - Implement `threshold_deinit()` - free state
  - Acceptance: Unit test: init/deinit, verify no leaks

- [ ] P5.T3: Implement Threshold Failure Logic
  - Complexity: Medium | Dependencies: P5.T2 | Files: `src/failure_models/threshold_failure_model.c`
  - Implement `threshold_should_fail_erase()` - check block erase count vs max
  - Implement `threshold_should_fail_write()` - check page program count vs max
  - Implement `threshold_should_fail_read()` - always false
  - Implement `threshold_is_block_bad()` - check erase count exceeds max
  - Acceptance: Unit test: configure max 10 erases, erase 11 times, verify 11th fails; configure max 100 programs, verify 101st fails

- [ ] P5.T4: Write Threshold Model Unit Tests
  - Complexity: Low | Dependencies: P5.T3 | Files: `host_test/test_threshold_failure_model.c`
  - Test: Erase threshold enforcement; write threshold enforcement; bad block detection
  - Acceptance: All tests pass

- [ ] P5.T5: Implement Probabilistic Model Structure
  - Complexity: Low | Dependencies: P1.T3 | Files: Create `src/failure_models/probabilistic_failure_model.c`
  - Define `probabilistic_failure_config_t` (rated_cycles, shape, BER, seed)
  - Define internal state (config + PRNG state); declare `nand_probabilistic_failure_model` ops variable
  - Acceptance: Structure compiles

- [ ] P5.T6: Implement Probabilistic Model Init/Deinit
  - Complexity: Low | Dependencies: P5.T5 | Files: `src/failure_models/probabilistic_failure_model.c`
  - Implement `probabilistic_init()` - allocate state, copy config, seed PRNG
  - Implement `probabilistic_deinit()` - free state
  - Acceptance: Unit test: init/deinit, verify no leaks

- [ ] P5.T7: Implement Weibull Distribution
  - Complexity: Medium | Dependencies: P5.T6 | Files: `src/failure_models/probabilistic_failure_model.c`
  - Implement `weibull_failure_probability(erase_count, rated_cycles, shape)`
  - Formula: `P = 1 - exp(-((erase_count / rated_cycles) ^ shape))`; use standard `math.h`
  - Acceptance: Unit test: verify P(0) ≈ 0; P(rated_cycles) ≈ 0.632 (1 - 1/e); P → 1 as erase_count → ∞

- [ ] P5.T8: Implement Probabilistic Failure Logic
  - Complexity: Medium | Dependencies: P5.T7 | Files: `src/failure_models/probabilistic_failure_model.c`
  - Implement `probabilistic_should_fail_erase()` - use Weibull + PRNG
  - Implement `probabilistic_should_fail_write()` - similar logic for page programs
  - Implement `probabilistic_should_fail_read()` - always false
  - Implement `probabilistic_is_block_bad()` - check if failure probability > 0.9
  - Acceptance: Unit test: fixed seed, verify reproducible failures; erase 100K cycles, verify failure rate increases

- [ ] P5.T9: Implement Bit Flip Injection
  - Complexity: Medium | Dependencies: P5.T8 | Files: `src/failure_models/probabilistic_failure_model.c`
  - Implement `probabilistic_corrupt_read_data(data, len)`
  - Calculate BER based on wear (increases with erase count); for each byte, flip bits with probability = BER
  - Acceptance: Unit test: fresh block, verify BER ≈ base_bit_error_rate; worn block (50K erases), verify BER > base; fixed seed, verify reproducible bit flips

- [ ] P5.T10: Write Probabilistic Model Unit Tests
  - Complexity: Medium | Dependencies: P5.T9 | Files: `host_test/test_probabilistic_failure_model.c`
  - Test: Weibull distribution formula; failure probability increases with wear; reproducibility with fixed seed
  - Test: Bit error rate increases with wear; bit flip injection corrupts data
  - Acceptance: All tests pass

---

## Phase 6: Byte-Level Delta Tracking & Optimization

**Goal**: Add byte-level delta tracking to sparse hash backend, optimize memory usage

- [ ] P6.T1: Implement Byte Delta Array Growth
  - Complexity: Low | Dependencies: P2.T12 | Files: `src/backends/sparse_hash_backend.c`
  - Add `byte_delta_capacity` field to `page_metadata_t` tracking
  - Implement dynamic array growth (start with 8, double each time); use `realloc()`
  - Acceptance: Unit test: add 100 byte deltas, verify array grows correctly

- [ ] P6.T2: Implement Byte Write Range Tracking
  - Complexity: Medium | Dependencies: P6.T1 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `sparse_hash_on_byte_write_range()`; for each byte in range, call `update_byte_delta()`
  - `update_byte_delta()` searches existing deltas by offset; if found, increment; if not found, create new (delta starts at 1)
  - Acceptance: Unit test: write byte range, verify deltas created; write same range twice, verify deltas incremented

- [ ] P6.T3: Implement Zero-Delta Filtering
  - Complexity: Medium | Dependencies: P6.T2 | Files: `src/backends/sparse_hash_backend.c`
  - Modify `sparse_hash_get_byte_deltas()` to filter zero-deltas
  - Count non-zero deltas; allocate filtered array (caller must free); copy only non-zero deltas to output
  - Acceptance: Unit test: program page, write full page again, get deltas, verify empty; program page, partial write, get deltas, verify only outliers

- [ ] P6.T4: Implement Delta Memory Limit
  - Complexity: Low | Dependencies: P6.T2 | Files: `src/backends/sparse_hash_backend.c`
  - Define `MAX_BYTE_DELTAS_PER_PAGE` constant (512 = 12.5% of 4KB page)
  - In `update_byte_delta()`, check if count >= MAX before creating new delta; log warning if limit reached
  - Acceptance: Unit test: write 1000 different bytes, verify capped at 512 deltas

- [ ] P6.T5: Implement Memory Usage Query
  - Complexity: Low | Dependencies: P6.T3 | Files: `src/backends/sparse_hash_backend.c`, `include/nand_emul_advanced.h`
  - Define `memory_usage_t` structure; implement `sparse_hash_get_memory_usage()`
  - Calculate block/page metadata bytes; calculate byte delta bytes; calculate hash table overhead; return total
  - Acceptance: Unit test: perform operations, get memory usage, verify reasonable

- [ ] P6.T6: Optimize Delta Storage
  - Complexity: Medium | Dependencies: P6.T3 | Files: `src/backends/sparse_hash_backend.c`
  - Implement `compact_byte_deltas()` function; remove zero-delta entries from internal array
  - Shrink array with `realloc()`; call during snapshot save to reduce file size
  - Acceptance: Unit test: create deltas, erase some, compact, verify size reduced

- [ ] P6.T7: Write Byte Delta Unit Tests
  - Complexity: Medium | Dependencies: P6.T6 | Files: `host_test/test_byte_deltas.c`
  - Test: Full page write, no deltas; partial page write, deltas created; repeated partial writes, deltas accumulate
  - Test: Zero-delta filtering; delta memory limit; memory usage calculation; delta compaction
  - Acceptance: All tests pass

---

## Phase 7: Wear Lifetime Simulation Example

**Goal**: Create example simulation demonstrating 10K cycle simulation with snapshots

- [ ] P7.T1: Create Simulation Workload Generator
  - Complexity: Medium | Dependencies: P4.T11 | Files: Create `examples/wear_simulation/workload_generator.c`
  - Implement `simulate_random_writes(device, count)`, `simulate_sequential_writes(device, count)`, `simulate_wear_leveling(device, count)`
  - Use PRNG for reproducibility
  - Acceptance: Unit test: verify write patterns match expectations

- [ ] P7.T2: Create Snapshot Manager
  - Complexity: Low | Dependencies: P4.T10, P4.T11 | Files: Create `examples/wear_simulation/snapshot_manager.c`
  - Implement `snapshot_save_periodic(device, cycle, interval, base_dir)`
  - Generate filename: `wear_{cycle:05d}.bin`; check if cycle % interval == 0; call `nand_emul_save_snapshot()`
  - Acceptance: Unit test: verify snapshots saved at correct intervals

- [ ] P7.T3: Create Wear Analysis Script
  - Complexity: Medium | Dependencies: P4.T13 | Files: Create `examples/wear_simulation/analyze_wear.py`
  - Python script to load JSON exports; calculate wear leveling metrics; generate plots (matplotlib)
  - Acceptance: Script runs without errors; generates PDF plots

- [ ] P7.T4: Create Main Simulation Program
  - Complexity: Medium | Dependencies: P7.T1, P7.T2, P5.T10 | Files: Create `examples/wear_simulation/main.c`
  - Initialize 32MB flash with advanced config; enable probabilistic failure model (rated_cycles = 100K)
  - Run 10,000 cycles (1000 ops per cycle); save snapshot every 100 cycles; export final state to JSON
  - Acceptance: Program compiles and runs; completes 10K cycles in <5 minutes (single-core)

- [ ] P7.T5: Create Simulation Makefile
  - Complexity: Low | Dependencies: P7.T4 | Files: Create `examples/wear_simulation/Makefile`
  - Build simulation program; link against nand_emul library
  - Add targets: `make run` (run simulation), `make analyze` (run Python script)
  - Acceptance: `make all` builds successfully; `make run` executes simulation

- [ ] P7.T6: Document Simulation Workflow
  - Complexity: Low | Dependencies: P7.T5 | Files: Create `examples/wear_simulation/README.md`
  - Explain simulation goals; list commands to build and run; describe output files
  - Explain how to use analysis script; show example plots
  - Acceptance: README is clear and complete

- [ ] P7.T7: Benchmark Performance
  - Complexity: Low | Dependencies: P7.T4 | Files: `examples/wear_simulation/benchmark.c`
  - Measure time for 10K cycles with full tracking; measure memory usage (peak RSS)
  - Compare with/without byte-level tracking; output performance report
  - Acceptance: 10K cycles complete in <5 minutes; memory usage <10MB for 32MB flash

- [ ] P7.T8: Test Different Flash Sizes
  - Complexity: Low | Dependencies: P7.T7 | Files: `examples/wear_simulation/` (modify main.c)
  - Test with 32MB, 64MB, 128MB flash sizes; verify memory scaling (linear); verify time scaling (linear)
  - Acceptance: All sizes complete successfully; memory usage stays <5% of flash size

---

## Phase 8: Testing & Documentation

**Goal**: Comprehensive testing, documentation, and polishing for release

- [ ] P8.T1: Write Wear Leveling Validation Test
  - Complexity: Medium | Dependencies: P4.T4 | Files: Create `host_test/test_wear_leveling.c`
  - Write random blocks 10,000 times; get wear statistics
  - Verify `max_block_erases - min_block_erases < threshold` (e.g., 100); verify `wear_leveling_variation < 0.1`
  - Acceptance: Test passes with proper wear leveling; test fails with intentionally skewed writes

- [ ] P8.T2: Write Failure Injection Integration Test
  - Complexity: Medium | Dependencies: P5.T10 | Files: Create `host_test/test_failure_injection.c`
  - Configure threshold model: max 10 erases per block; erase same block 11 times
  - Verify 11th erase fails with `ESP_ERR_FLASH_OP_FAIL`; verify block marked as bad
  - Acceptance: Test passes

- [ ] P8.T3: Write Memory Efficiency Test
  - Complexity: Low | Dependencies: P6.T5 | Files: Create `host_test/test_memory_efficiency.c`
  - Initialize 32MB flash; perform 1000 random operations; get memory usage; verify total < 5% of flash size (1.6MB)
  - Acceptance: Test passes

- [ ] P8.T4: Write Snapshot Roundtrip Test
  - Complexity: Medium | Dependencies: P4.T11 | Files: Create `host_test/test_snapshot_roundtrip.c`
  - Perform various operations (erases, writes); save snapshot; deinit device; init new device; load snapshot
  - Query metadata, verify matches original
  - Acceptance: All metadata restored correctly

- [ ] P8.T5: Write Snapshot Corruption Test
  - Complexity: Low | Dependencies: P4.T11 | Files: Create `host_test/test_snapshot_corruption.c`
  - Save valid snapshot; corrupt file (flip bits in header); attempt to load; verify returns `ESP_ERR_INVALID_CRC`
  - Acceptance: Corruption detected

- [ ] P8.T6: Write Lifetime Simulation Test
  - Complexity: High | Dependencies: P7.T4 | Files: Create `host_test/test_lifetime_simulation.c`
  - Run 10K cycles with probabilistic model; save 100 snapshots; load snapshots at cycles 0, 5000, 9999
  - Verify wear progression (erase counts increase); verify failure probability increases
  - Acceptance: Test completes in reasonable time (<5 min); wear progression observed

- [ ] P8.T7: Measure Code Coverage
  - Complexity: Low | Dependencies: All test tasks | Files: Add to Makefile
  - Add gcov/lcov flags to test build; run all tests; generate coverage report; verify >80% code coverage
  - Acceptance: Coverage report generated; target coverage achieved

- [ ] P8.T8: Write API Documentation
  - Complexity: Medium | Dependencies: All API implementation tasks | Files: All header files
  - Add Doxygen comments for all public functions; document parameters, return values, error codes
  - Add usage examples in comments; document backend and failure model interfaces
  - Acceptance: Doxygen builds without warnings; all public APIs documented

- [ ] P8.T9: Write Usage Examples
  - Complexity: Medium | Dependencies: P8.T8 | Files: Create `examples/basic_tracking/`, `examples/failure_injection/`
  - Basic Tracking: init with sparse hash backend, perform operations, query metadata
  - Failure Injection: init with threshold model, trigger failures, handle errors; each example includes Makefile and README
  - Acceptance: Examples compile and run; READMEs explain usage

- [ ] P8.T10: Create Architecture Diagrams
  - Complexity: Low | Dependencies: None | Files: Create `docs/architecture.md`
  - System component diagram; data flow diagrams; snapshot file format diagram; include in main README
  - Acceptance: Diagrams clear and accurate

- [ ] P8.T11: Update Main README
  - Complexity: Medium | Dependencies: P8.T10 | Files: `README.md`
  - Add "Advanced Flash Tracking" section; explain use cases and features; link to examples and documentation
  - Show quick start code snippet; include architecture diagram; explain snapshot workflow
  - Acceptance: README is clear and comprehensive

- [ ] P8.T12: Run Full Regression Suite
  - Complexity: Low | Dependencies: All test tasks | Files: N/A
  - Run all unit tests; run all integration tests; run example programs
  - Verify no memory leaks (valgrind); verify no warnings (compile with -Wall -Wextra)
  - Acceptance: All tests pass; no leaks, no warnings

---

## Summary

- **Total Tasks**: 122 across 8 phases
- **Critical Path**: P1.T1 → P1.T2 → P1.T4 → P1.T5 → P1.T6 → P2.T1 → P2.T2 → P2.T3 → P2.T9 → P2.T11 → P3.T3 → P3.T5 → P3.T6 → P6.T2 → P6.T3 → P4.T8 → P4.T9 → P7.T4 → P8.T12
- **Estimated Effort**: ~620 hours (~4 months at full-time)
