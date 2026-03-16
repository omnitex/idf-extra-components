# Advanced Flash Tracking - Implementation Tasks

## Overview

This document breaks down the 8 implementation phases from `proposal.md` into concrete, actionable tasks. Each task includes:

- **ID**: Unique identifier (e.g., P1.T1)
- **Description**: What needs to be done
- **Complexity**: Low (1-2 hours), Medium (3-6 hours), High (1-2 days)
- **Dependencies**: Which tasks must be completed first
- **Acceptance Criteria**: How to verify the task is complete

## Phase 1: Core Infrastructure

**Goal**: Foundation for advanced tracking system - data structures, interfaces, initialization

### P1.T1: Define Core Data Structures
- **Complexity**: Low
- **Dependencies**: None
- **Files**: Create `include/nand_emul_advanced.h`
- **Description**: 
  - Define `byte_delta_metadata_t`, `page_metadata_t`, `block_metadata_t`
  - Define `nand_wear_stats_t` for aggregate statistics
  - Define `nand_operation_context_t` for failure models
  - Add explicit padding for cross-platform compatibility
  - Add static assertions for struct sizes
- **Acceptance Criteria**:
  - All structs compile without warnings
  - Static assertions pass (header size = 64 bytes, etc.)
  - Doxygen comments for all fields

### P1.T2: Define Backend Interface
- **Complexity**: Low
- **Dependencies**: P1.T1
- **Files**: `include/nand_emul_advanced.h`
- **Description**:
  - Define `nand_metadata_backend_ops_t` vtable structure
  - Add lifecycle methods: `init()`, `deinit()`
  - Add operation callbacks: `on_block_erase()`, `on_page_program()`, `on_byte_write_range()`
  - Add query methods: `get_block_info()`, `get_page_info()`, `get_byte_deltas()`
  - Add iteration methods: `iterate_blocks()`, `iterate_pages()`
  - Add snapshot methods: `save_snapshot()`, `load_snapshot()`
  - Add export method: `export_json()`
- **Acceptance Criteria**:
  - Interface compiles with function pointer syntax
  - All methods documented with parameter descriptions

### P1.T3: Define Failure Model Interface
- **Complexity**: Low
- **Dependencies**: P1.T1
- **Files**: `include/nand_emul_advanced.h`
- **Description**:
  - Define `nand_failure_model_ops_t` vtable structure
  - Add lifecycle methods: `init()`, `deinit()`
  - Add decision methods: `should_fail_read()`, `should_fail_write()`, `should_fail_erase()`
  - Add corruption method: `corrupt_read_data()`
  - Add health check: `is_block_bad()`
- **Acceptance Criteria**:
  - Interface compiles
  - All methods documented

### P1.T4: Define Configuration Structures
- **Complexity**: Low
- **Dependencies**: P1.T2, P1.T3
- **Files**: `include/nand_emul_advanced.h`
- **Description**:
  - Define `nand_emul_advanced_config_t`
  - Include base config (`nand_file_mmap_emul_config_t`)
  - Add backend and failure model pointers
  - Add tracking flags (block/page/byte level)
  - Add optional timestamp function pointer
- **Acceptance Criteria**:
  - Config struct compiles
  - Default values documented

### P1.T5: Extend Internal Handle
- **Complexity**: Medium
- **Dependencies**: P1.T4
- **Files**: `src/nand_linux_mmap_emul.c` (private header section)
- **Description**:
  - Add `advanced` field to `nand_mmap_emul_handle_t` (struct pointer)
  - Define internal `nand_advanced_context_t` structure
  - Include backend handle, failure model handle, config flags
  - Add cached device geometry (total_blocks, pages_per_block, page_size)
  - Add timestamp function pointer and default counter
- **Acceptance Criteria**:
  - Handle compiles with new field
  - `advanced` field is NULL by default (backward compatibility)

### P1.T6: Implement `nand_emul_advanced_init()`
- **Complexity**: Medium
- **Dependencies**: P1.T5
- **Files**: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
- **Description**:
  - Create advanced init function with config parameter
  - Call base `nand_emul_init()` first
  - Allocate `advanced` context structure
  - Initialize metadata backend (call `backend_ops->init()`)
  - Initialize failure model if provided (call `failure_ops->init()`)
  - Cache device geometry
  - Set timestamp function (default or user-provided)
- **Acceptance Criteria**:
  - Function compiles and links
  - Returns `ESP_OK` on success
  - Returns `ESP_ERR_NO_MEM` if allocation fails
  - Backend is properly initialized

### P1.T7: Implement `nand_emul_advanced_deinit()`
- **Complexity**: Low
- **Dependencies**: P1.T6
- **Files**: `src/nand_linux_mmap_emul.c`
- **Description**:
  - Call backend `deinit()` if present
  - Call failure model `deinit()` if present
  - Free `advanced` context
  - Call base `nand_emul_deinit()`
- **Acceptance Criteria**:
  - No memory leaks (verify with valgrind)
  - Handles NULL pointers gracefully

### P1.T8: Implement No-op Backend
- **Complexity**: Low
- **Dependencies**: P1.T2
- **Files**: Create `src/backends/noop_backend.c`
- **Description**:
  - Implement all backend operations as no-ops
  - `init()` returns `ESP_OK`
  - All query methods return zeros/empty results
  - Iteration methods never call callback
- **Acceptance Criteria**:
  - Compiles and links
  - Can be used in `nand_emul_advanced_init()`
  - Unit test: init with no-op backend, verify no crash

### P1.T9: Implement No-op Failure Model
- **Complexity**: Low
- **Dependencies**: P1.T3
- **Files**: Create `src/failure_models/noop_failure_model.c`
- **Description**:
  - Implement all failure model operations as no-ops
  - All `should_fail_*()` return `false`
  - `corrupt_read_data()` does nothing
  - `is_block_bad()` returns `false`
- **Acceptance Criteria**:
  - Compiles and links
  - Can be used in `nand_emul_advanced_init()`

### P1.T10: Write Basic Unit Test
- **Complexity**: Low
- **Dependencies**: P1.T8, P1.T9
- **Files**: Create `host_test/test_advanced_init.c`
- **Description**:
  - Test: Init with no-op backend and failure model
  - Test: Verify handle is created
  - Test: Perform basic read/write operations
  - Test: Deinit and verify no memory leaks
- **Acceptance Criteria**:
  - All tests pass
  - No valgrind errors

---

## Phase 2: Sparse Hash Backend

**Goal**: Implement production-ready metadata storage backend with block and page tracking

### P2.T1: Implement Hash Table Core
- **Complexity**: High
- **Dependencies**: P1.T2
- **Files**: Create `src/backends/hash_table.c`, `src/backends/hash_table.h`
- **Description**:
  - Define `hash_table_t` structure (buckets, capacity, count, load_factor)
  - Define `hash_node_t` structure (key, next pointer, flexible array for data)
  - Implement `hash_table_create(capacity, entry_size, load_factor)`
  - Implement `hash_table_destroy()`
  - Implement hash function (Knuth multiplicative hash)
  - Implement `next_power_of_2()` utility
- **Acceptance Criteria**:
  - Unit test: Create table, verify initial state
  - Unit test: Destroy empty table, no leaks

### P2.T2: Implement Hash Table Insert/Lookup
- **Complexity**: Medium
- **Dependencies**: P2.T1
- **Files**: `src/backends/hash_table.c`
- **Description**:
  - Implement `hash_table_get(table, key)` - returns existing node or NULL
  - Implement `hash_table_get_or_insert(table, key)` - returns existing or creates new
  - Handle collision chaining (linked list)
  - Zero-initialize new node data
- **Acceptance Criteria**:
  - Unit test: Insert 100 entries, verify all can be retrieved
  - Unit test: Insert duplicate key, verify returns same node

### P2.T3: Implement Hash Table Rehashing
- **Complexity**: Medium
- **Dependencies**: P2.T2
- **Files**: `src/backends/hash_table.c`
- **Description**:
  - Implement `hash_table_rehash(table, new_capacity)`
  - Allocate new bucket array
  - Reinsert all nodes into new buckets
  - Free old bucket array
  - Trigger rehash when load factor exceeded in `get_or_insert()`
- **Acceptance Criteria**:
  - Unit test: Insert 1000 entries, verify automatic rehashing
  - Unit test: All entries retrievable after rehash

### P2.T4: Implement Hash Table Removal
- **Complexity**: Medium
- **Dependencies**: P2.T2
- **Files**: `src/backends/hash_table.c`
- **Description**:
  - Implement `hash_table_remove(table, key)`
  - Unlink node from collision chain
  - Free node memory
  - Decrement count
- **Acceptance Criteria**:
  - Unit test: Insert, remove, verify not found
  - Unit test: Remove from collision chain

### P2.T5: Implement Hash Table Iteration
- **Complexity**: Low
- **Dependencies**: P2.T2
- **Files**: `src/backends/hash_table.c`
- **Description**:
  - Implement `hash_table_iterate(table, callback, user_data)`
  - Visit all buckets
  - Follow collision chains
  - Call callback for each node
  - Stop if callback returns false
- **Acceptance Criteria**:
  - Unit test: Insert 50 entries, iterate, verify all visited

### P2.T6: Implement Sparse Hash Backend Structure
- **Complexity**: Low
- **Dependencies**: P2.T1
- **Files**: Create `src/backends/sparse_hash_backend.c`
- **Description**:
  - Define `sparse_hash_backend_t` structure
  - Include `block_table`, `page_table` pointers
  - Include configuration (track_byte_deltas, load_factor)
  - Include cached device geometry
  - Include cached statistics and dirty flag
- **Acceptance Criteria**:
  - Structure compiles

### P2.T7: Implement Sparse Hash Backend Init
- **Complexity**: Medium
- **Dependencies**: P2.T6, P2.T1
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_init()` function
  - Parse `sparse_hash_backend_config_t`
  - Allocate backend structure
  - Create block hash table (estimate 10% of total blocks)
  - Create page hash table (estimate 10% of total pages)
  - Cache configuration
- **Acceptance Criteria**:
  - Unit test: Init with valid config, verify success
  - Unit test: Init with NULL config, verify error

### P2.T8: Implement Sparse Hash Backend Deinit
- **Complexity**: Medium
- **Dependencies**: P2.T7
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_deinit()` function
  - Free all byte delta arrays in page table
  - Destroy page hash table
  - Destroy block hash table
  - Free backend structure
- **Acceptance Criteria**:
  - No memory leaks (valgrind)
  - Handles NULL backend gracefully

### P2.T9: Implement Block Erase Tracking
- **Complexity**: Medium
- **Dependencies**: P2.T8, P2.T4
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_on_block_erase()`
  - Get or insert block metadata node
  - Increment erase count
  - Update first/last erase timestamps
  - Remove all page metadata in block (pages invalidated)
  - Free byte deltas for removed pages
  - Set stats dirty flag
- **Acceptance Criteria**:
  - Unit test: Erase block, verify count incremented
  - Unit test: Erase block with pages, verify pages removed

### P2.T10: Implement Block Query
- **Complexity**: Low
- **Dependencies**: P2.T9
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_get_block_info()`
  - Lookup block in hash table
  - If not found, return zeros (block never erased)
  - If found, copy metadata to output
- **Acceptance Criteria**:
  - Unit test: Query never-erased block, verify zeros
  - Unit test: Erase block, query, verify count=1

### P2.T11: Implement Page Program Tracking
- **Complexity**: Medium
- **Dependencies**: P2.T10
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_on_page_program()`
  - Get or insert page metadata node
  - Increment program count
  - Update first/last program timestamps
  - Update parent block's total_page_programs
  - Set stats dirty flag
- **Acceptance Criteria**:
  - Unit test: Program page, verify count incremented
  - Unit test: Program page, verify block aggregate updated

### P2.T12: Implement Page Query
- **Complexity**: Low
- **Dependencies**: P2.T11
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_get_page_info()`
  - Lookup page in hash table
  - If not found, return zeros
  - If found, copy metadata to output
- **Acceptance Criteria**:
  - Unit test: Query never-programmed page, verify zeros
  - Unit test: Program page, query, verify count=1

### P2.T13: Implement Block Iteration
- **Complexity**: Low
- **Dependencies**: P2.T10, P2.T5
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_iterate_blocks()`
  - Use `hash_table_iterate()` on block table
  - Call user callback for each block
  - Stop if callback returns false
- **Acceptance Criteria**:
  - Unit test: Erase 10 blocks, iterate, verify all visited

### P2.T14: Implement Page Iteration
- **Complexity**: Low
- **Dependencies**: P2.T12, P2.T5
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_iterate_pages()`
  - Use `hash_table_iterate()` on page table
  - Call user callback for each page
  - Stop if callback returns false
- **Acceptance Criteria**:
  - Unit test: Program 20 pages, iterate, verify all visited

### P2.T15: Implement Statistics Calculation
- **Complexity**: Medium
- **Dependencies**: P2.T13, P2.T14
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_get_stats()`
  - Check if stats are dirty, recompute if needed
  - Iterate blocks: calculate min/max/avg erase counts
  - Iterate pages: calculate min/max program counts
  - Calculate `wear_leveling_variation = (max - min) / avg`
  - Cache results, clear dirty flag
- **Acceptance Criteria**:
  - Unit test: Erase blocks with varying counts, verify min/max/avg
  - Unit test: Verify `wear_leveling_variation` formula

### P2.T16: Write Comprehensive Backend Unit Tests
- **Complexity**: Medium
- **Dependencies**: P2.T15
- **Files**: Create `host_test/test_sparse_hash_backend.c`
- **Description**:
  - Test: Init/deinit lifecycle
  - Test: Block erase tracking (single, multiple, repeated)
  - Test: Page program tracking (single, multiple, repeated)
  - Test: Block query (never erased, erased once, erased multiple times)
  - Test: Page query (never programmed, programmed once, programmed multiple times)
  - Test: Iteration (blocks, pages, early termination)
  - Test: Statistics (min/max/avg, wear variation)
  - Test: Memory efficiency (verify sparse storage)
- **Acceptance Criteria**:
  - All tests pass
  - No memory leaks

---

## Phase 3: Integration with Core Operations

**Goal**: Hook metadata tracking and failure injection into read/write/erase operations

### P3.T1: Add Timestamp Helper
- **Complexity**: Low
- **Dependencies**: P1.T6
- **Files**: `src/nand_linux_mmap_emul.c`
- **Description**:
  - Implement `default_timestamp()` function (monotonic counter)
  - In `nand_emul_advanced_init()`, set timestamp function (user-provided or default)
- **Acceptance Criteria**:
  - Unit test: Verify counter increments on each call

### P3.T2: Modify `nand_emul_erase_block()` - Failure Check
- **Complexity**: Low
- **Dependencies**: P1.T6
- **Files**: `src/nand_linux_mmap_emul.c`
- **Description**:
  - Before erase operation, check if `advanced` is set
  - If failure model present, build `nand_operation_context_t`
  - Call `should_fail_erase()`
  - If true, log warning and return `ESP_ERR_FLASH_OP_FAIL`
- **Acceptance Criteria**:
  - Unit test: Configure threshold model (max 1 erase), verify 2nd erase fails

### P3.T3: Modify `nand_emul_erase_block()` - Metadata Tracking
- **Complexity**: Low
- **Dependencies**: P3.T2, P2.T9
- **Files**: `src/nand_linux_mmap_emul.c`
- **Description**:
  - After successful erase, check if `advanced` is set
  - If `track_block_level` enabled and metadata backend present, call `on_block_erase()`
  - Pass block number and timestamp
- **Acceptance Criteria**:
  - Unit test: Erase block with tracking enabled, verify metadata updated

### P3.T4: Modify `nand_emul_write()` - Failure Check
- **Complexity**: Medium
- **Dependencies**: P1.T6
- **Files**: `src/nand_linux_mmap_emul.c`
- **Description**:
  - Before write operation, check if `advanced` is set
  - If failure model present, build `nand_operation_context_t`
  - Query block and page metadata for context (optional)
  - Call `should_fail_write()`
  - If true, log warning and return `ESP_ERR_FLASH_OP_FAIL`
- **Acceptance Criteria**:
  - Unit test: Configure threshold model, verify write fails after limit

### P3.T5: Modify `nand_emul_write()` - Page Tracking
- **Complexity**: Medium
- **Dependencies**: P3.T4, P2.T11
- **Files**: `src/nand_linux_mmap_emul.c`
- **Description**:
  - After successful write, check if `advanced` is set
  - Calculate affected pages (first_page, last_page)
  - For each page:
    - If `track_page_level` enabled, call `on_page_program()`
- **Acceptance Criteria**:
  - Unit test: Write spanning single page, verify page count=1
  - Unit test: Write spanning 3 pages, verify all 3 pages tracked

### P3.T6: Modify `nand_emul_write()` - Byte Range Tracking
- **Complexity**: Medium
- **Dependencies**: P3.T5
- **Files**: `src/nand_linux_mmap_emul.c`
- **Description**:
  - After page tracking, if `track_byte_level` enabled
  - For each affected page, calculate byte range within page
  - Call `on_byte_write_range()` with page_num, byte_offset, length
  - Backend handles delta creation internally
- **Acceptance Criteria**:
  - Unit test: Write full page, then partial page, verify backend tracks correctly
  - Unit test: Write multi-page, verify byte ranges calculated correctly

### P3.T7: Modify `nand_emul_read()` - Failure Check
- **Complexity**: Low
- **Dependencies**: P1.T6
- **Files**: `src/nand_linux_mmap_emul.c`
- **Description**:
  - After read operation, check if `advanced` is set
  - If failure model present, build `nand_operation_context_t`
  - Call `should_fail_read()`
  - If true, log warning and return `ESP_ERR_FLASH_OP_FAIL`
- **Acceptance Criteria**:
  - Unit test: Configure failure model to fail reads, verify error

### P3.T8: Modify `nand_emul_read()` - Data Corruption
- **Complexity**: Low
- **Dependencies**: P3.T7
- **Files**: `src/nand_linux_mmap_emul.c`
- **Description**:
  - After read operation (before returning), check if `advanced` is set
  - If failure model present, call `corrupt_read_data(data, len)`
  - Model modifies data buffer in-place (bit flips)
- **Acceptance Criteria**:
  - Unit test: Configure probabilistic model, verify some reads have bit errors

### P3.T9: Write Integration Tests
- **Complexity**: Medium
- **Dependencies**: P3.T3, P3.T6, P3.T8
- **Files**: Create `host_test/test_integration.c`
- **Description**:
  - Test: Erase block, verify metadata updated
  - Test: Write single page, verify page metadata
  - Test: Write multi-page, verify all pages tracked
  - Test: Write partial page, then full page, verify byte deltas created
  - Test: Read with corruption model, verify bit errors injected
  - Test: Backward compatibility (init without advanced, verify no crash)
- **Acceptance Criteria**:
  - All tests pass
  - Existing NAND tests still pass (backward compatibility verified)

---

## Phase 4: Query API & Snapshots

**Goal**: Expose metadata to users via query functions and implement snapshot save/load

### P4.T1: Implement `nand_emul_get_block_wear()`
- **Complexity**: Low
- **Dependencies**: P2.T10
- **Files**: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
- **Description**:
  - Validate handle and block number
  - Check if advanced tracking enabled
  - Call backend `get_block_info()`
  - Return result
- **Acceptance Criteria**:
  - Unit test: Get block wear for never-erased block, verify zeros
  - Unit test: Erase block twice, get wear, verify count=2

### P4.T2: Implement `nand_emul_get_page_wear()`
- **Complexity**: Low
- **Dependencies**: P2.T12
- **Files**: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
- **Description**:
  - Validate handle and page number
  - Check if advanced tracking enabled
  - Call backend `get_page_info()`
  - Return result
- **Acceptance Criteria**:
  - Unit test: Get page wear for never-programmed page, verify zeros
  - Unit test: Program page 5 times, get wear, verify count=5

### P4.T3: Implement `nand_emul_get_byte_deltas()`
- **Complexity**: Low
- **Dependencies**: P2.T12
- **Files**: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
- **Description**:
  - Validate handle and page number
  - Check if byte-level tracking enabled
  - Call backend `get_byte_deltas()`
  - Return filtered delta array (caller must free)
- **Acceptance Criteria**:
  - Unit test: Get deltas for page with no outliers, verify empty
  - Unit test: Program page, then partial write, get deltas, verify outliers

### P4.T4: Implement `nand_emul_get_wear_stats()`
- **Complexity**: Low
- **Dependencies**: P2.T15
- **Files**: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
- **Description**:
  - Validate handle
  - Check if advanced tracking enabled
  - Call backend `get_stats()`
  - Return aggregate statistics
- **Acceptance Criteria**:
  - Unit test: Perform mixed operations, get stats, verify correctness

### P4.T5: Implement `nand_emul_iterate_worn_blocks()`
- **Complexity**: Low
- **Dependencies**: P2.T13
- **Files**: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
- **Description**:
  - Validate handle and callback
  - Check if advanced tracking enabled
  - Call backend `iterate_blocks()`
  - Pass through callback and user_data
- **Acceptance Criteria**:
  - Unit test: Erase 5 blocks, iterate, verify all visited

### P4.T6: Implement `nand_emul_mark_bad_block()`
- **Complexity**: Low
- **Dependencies**: P2.T10
- **Files**: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
- **Description**:
  - Validate handle and block number
  - Check if advanced tracking enabled
  - Call backend `set_bad_block(block_num, true)`
- **Acceptance Criteria**:
  - Unit test: Mark block bad, query, verify flag set

### P4.T7: Implement Snapshot Header Structure
- **Complexity**: Low
- **Dependencies**: None
- **Files**: `src/backends/snapshot_format.h`
- **Description**:
  - Define `snapshot_header_t` with `__attribute__((packed))`
  - Add static assertion: `sizeof(snapshot_header_t) == 64`
  - Define magic number: `0x4E414E44`
  - Define version: `0x01`
  - Define flags bitfield (block/page/byte tracking)
- **Acceptance Criteria**:
  - Header compiles
  - Static assertion passes

### P4.T8: Implement Snapshot Save (Sparse Hash Backend)
- **Complexity**: High
- **Dependencies**: P4.T7, P2.T15
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_save_snapshot()`
  - Write header with placeholders
  - Iterate blocks, write block metadata section
  - Iterate pages, write page metadata section
  - Accumulate byte deltas, write byte delta section
  - Calculate CRC32 of header
  - Rewrite header with correct offsets and checksum
- **Acceptance Criteria**:
  - Unit test: Save empty snapshot, verify header correct
  - Unit test: Erase blocks, program pages, save snapshot, verify file size

### P4.T9: Implement Snapshot Load (Sparse Hash Backend)
- **Complexity**: High
- **Dependencies**: P4.T8
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_load_snapshot()`
  - Read header, validate magic and CRC32
  - Clear existing metadata (reset hash tables)
  - Read block metadata section, populate block table
  - Read byte deltas into memory buffer
  - Read page metadata section, reconstruct byte delta pointers
  - Set stats dirty flag
- **Acceptance Criteria**:
  - Unit test: Save then load, verify metadata identical
  - Unit test: Load corrupted snapshot, verify error

### P4.T10: Implement `nand_emul_save_snapshot()`
- **Complexity**: Low
- **Dependencies**: P4.T8
- **Files**: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
- **Description**:
  - Validate handle and filename
  - Check if advanced tracking enabled
  - Get current timestamp
  - Call backend `save_snapshot(filename, timestamp)`
- **Acceptance Criteria**:
  - Unit test: Save snapshot, verify file created

### P4.T11: Implement `nand_emul_load_snapshot()`
- **Complexity**: Low
- **Dependencies**: P4.T9
- **Files**: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
- **Description**:
  - Validate handle and filename
  - Check if advanced tracking enabled
  - Call backend `load_snapshot(filename)`
  - Verify success
- **Acceptance Criteria**:
  - Unit test: Save, deinit, init, load, verify metadata restored

### P4.T12: Implement JSON Export (Sparse Hash Backend)
- **Complexity**: Medium
- **Dependencies**: P2.T15
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_export_json()`
  - Write JSON header with device info
  - Iterate blocks, write block metadata as JSON objects
  - Iterate pages, write page metadata as JSON objects
  - Include byte deltas as nested arrays
  - Write aggregate statistics
- **Acceptance Criteria**:
  - Unit test: Export to JSON, verify valid JSON (use jq to parse)
  - Unit test: Verify structure matches specification

### P4.T13: Implement `nand_emul_export_json()`
- **Complexity**: Low
- **Dependencies**: P4.T12
- **Files**: `src/nand_linux_mmap_emul.c`, `include/nand_emul_advanced.h`
- **Description**:
  - Validate handle and filename
  - Check if advanced tracking enabled
  - Call backend `export_json(filename)`
- **Acceptance Criteria**:
  - Unit test: Export JSON, parse with external tool

### P4.T14: Write Query API Unit Tests
- **Complexity**: Medium
- **Dependencies**: P4.T6, P4.T11, P4.T13
- **Files**: `host_test/test_query_api.c`
- **Description**:
  - Test all query functions (block, page, byte deltas, stats)
  - Test iteration (blocks, pages)
  - Test bad block marking
  - Test snapshot save/load roundtrip
  - Test JSON export structure
- **Acceptance Criteria**:
  - All tests pass

---

## Phase 5: Built-in Failure Models

**Goal**: Implement threshold and probabilistic failure models for realistic testing

### P5.T1: Implement Threshold Model Structure
- **Complexity**: Low
- **Dependencies**: P1.T3
- **Files**: Create `src/failure_models/threshold_failure_model.c`
- **Description**:
  - Define `threshold_failure_config_t`
  - Define internal state structure (config copy)
  - Declare `nand_threshold_failure_model` ops variable
- **Acceptance Criteria**:
  - Structure compiles

### P5.T2: Implement Threshold Model Init/Deinit
- **Complexity**: Low
- **Dependencies**: P5.T1
- **Files**: `src/failure_models/threshold_failure_model.c`
- **Description**:
  - Implement `threshold_init()` - allocate state, copy config
  - Implement `threshold_deinit()` - free state
- **Acceptance Criteria**:
  - Unit test: Init/deinit, verify no leaks

### P5.T3: Implement Threshold Failure Logic
- **Complexity**: Medium
- **Dependencies**: P5.T2
- **Files**: `src/failure_models/threshold_failure_model.c`
- **Description**:
  - Implement `threshold_should_fail_erase()` - check block erase count vs max
  - Implement `threshold_should_fail_write()` - check page program count vs max
  - Implement `threshold_should_fail_read()` - always false (no read failure)
  - Implement `threshold_is_block_bad()` - check erase count exceeds max
- **Acceptance Criteria**:
  - Unit test: Configure max 10 erases, erase 11 times, verify 11th fails
  - Unit test: Configure max 100 programs, program 101 times, verify 101st fails

### P5.T4: Write Threshold Model Unit Tests
- **Complexity**: Low
- **Dependencies**: P5.T3
- **Files**: `host_test/test_threshold_failure_model.c`
- **Description**:
  - Test: Erase threshold enforcement
  - Test: Write threshold enforcement
  - Test: Bad block detection
- **Acceptance Criteria**:
  - All tests pass

### P5.T5: Implement Probabilistic Model Structure
- **Complexity**: Low
- **Dependencies**: P1.T3
- **Files**: Create `src/failure_models/probabilistic_failure_model.c`
- **Description**:
  - Define `probabilistic_failure_config_t` (rated_cycles, shape, BER, seed)
  - Define internal state (config + PRNG state)
  - Declare `nand_probabilistic_failure_model` ops variable
- **Acceptance Criteria**:
  - Structure compiles

### P5.T6: Implement Probabilistic Model Init/Deinit
- **Complexity**: Low
- **Dependencies**: P5.T5
- **Files**: `src/failure_models/probabilistic_failure_model.c`
- **Description**:
  - Implement `probabilistic_init()` - allocate state, copy config, seed PRNG
  - Implement `probabilistic_deinit()` - free state
- **Acceptance Criteria**:
  - Unit test: Init/deinit, verify no leaks

### P5.T7: Implement Weibull Distribution
- **Complexity**: Medium
- **Dependencies**: P5.T6
- **Files**: `src/failure_models/probabilistic_failure_model.c`
- **Description**:
  - Implement `weibull_failure_probability(erase_count, rated_cycles, shape)`
  - Formula: `P = 1 - exp(-((erase_count / rated_cycles) ^ shape))`
  - Use standard math library (`math.h`)
- **Acceptance Criteria**:
  - Unit test: Verify P(0) ≈ 0
  - Unit test: Verify P(rated_cycles) ≈ 0.632 (1 - 1/e)
  - Unit test: Verify P → 1 as erase_count → ∞

### P5.T8: Implement Probabilistic Failure Logic
- **Complexity**: Medium
- **Dependencies**: P5.T7
- **Files**: `src/failure_models/probabilistic_failure_model.c`
- **Description**:
  - Implement `probabilistic_should_fail_erase()` - use Weibull + PRNG
  - Implement `probabilistic_should_fail_write()` - similar logic for page programs
  - Implement `probabilistic_should_fail_read()` - always false
  - Implement `probabilistic_is_block_bad()` - check if failure probability > 0.9
- **Acceptance Criteria**:
  - Unit test: Fixed seed, verify reproducible failures
  - Unit test: Erase 100K cycles, verify failure rate increases

### P5.T9: Implement Bit Flip Injection
- **Complexity**: Medium
- **Dependencies**: P5.T8
- **Files**: `src/failure_models/probabilistic_failure_model.c`
- **Description**:
  - Implement `probabilistic_corrupt_read_data(data, len)`
  - Calculate BER based on wear (increases with erase count)
  - For each byte, flip bits with probability = BER
  - Use PRNG for bit selection
- **Acceptance Criteria**:
  - Unit test: Fresh block, verify BER ≈ base_bit_error_rate
  - Unit test: Worn block (50K erases), verify BER > base
  - Unit test: Fixed seed, verify reproducible bit flips

### P5.T10: Write Probabilistic Model Unit Tests
- **Complexity**: Medium
- **Dependencies**: P5.T9
- **Files**: `host_test/test_probabilistic_failure_model.c`
- **Description**:
  - Test: Weibull distribution formula
  - Test: Failure probability increases with wear
  - Test: Reproducibility with fixed seed
  - Test: Bit error rate increases with wear
  - Test: Bit flip injection corrupts data
- **Acceptance Criteria**:
  - All tests pass

---

## Phase 6: Byte-Level Delta Tracking & Optimization

**Goal**: Add byte-level delta tracking to sparse hash backend, optimize memory usage

### P6.T1: Implement Byte Delta Array Growth
- **Complexity**: Low
- **Dependencies**: P2.T12
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Add `byte_delta_capacity` field to `page_metadata_t` tracking
  - Implement dynamic array growth (start with 8, double each time)
  - Use `realloc()` to grow array
- **Acceptance Criteria**:
  - Unit test: Add 100 byte deltas, verify array grows correctly

### P6.T2: Implement Byte Write Range Tracking
- **Complexity**: Medium
- **Dependencies**: P6.T1
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `sparse_hash_on_byte_write_range()`
  - For each byte in range, call `update_byte_delta()`
  - `update_byte_delta()` searches existing deltas by offset
  - If found, increment delta and update timestamp
  - If not found, create new delta entry (delta starts at 1)
- **Acceptance Criteria**:
  - Unit test: Write byte range, verify deltas created
  - Unit test: Write same range twice, verify deltas incremented

### P6.T3: Implement Zero-Delta Filtering
- **Complexity**: Medium
- **Dependencies**: P6.T2
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Modify `sparse_hash_get_byte_deltas()` to filter zero-deltas
  - Iterate through byte_deltas array
  - Count non-zero deltas
  - Allocate filtered array (caller must free)
  - Copy only non-zero deltas to output
- **Acceptance Criteria**:
  - Unit test: Program page, write full page again, get deltas, verify empty (all zeros)
  - Unit test: Program page, partial write, get deltas, verify only outliers

### P6.T4: Implement Delta Memory Limit
- **Complexity**: Low
- **Dependencies**: P6.T2
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Define `MAX_BYTE_DELTAS_PER_PAGE` constant (512 = 12.5% of 4KB page)
  - In `update_byte_delta()`, check if count >= MAX before creating new delta
  - Log warning if limit reached
- **Acceptance Criteria**:
  - Unit test: Write 1000 different bytes, verify capped at 512 deltas

### P6.T5: Implement Memory Usage Query
- **Complexity**: Low
- **Dependencies**: P6.T3
- **Files**: `src/backends/sparse_hash_backend.c`, `include/nand_emul_advanced.h`
- **Description**:
  - Define `memory_usage_t` structure
  - Implement `sparse_hash_get_memory_usage()`
  - Calculate block metadata bytes
  - Calculate page metadata bytes
  - Calculate byte delta bytes (iterate pages, sum capacities)
  - Calculate hash table overhead
  - Return total
- **Acceptance Criteria**:
  - Unit test: Perform operations, get memory usage, verify reasonable

### P6.T6: Optimize Delta Storage
- **Complexity**: Medium
- **Dependencies**: P6.T3
- **Files**: `src/backends/sparse_hash_backend.c`
- **Description**:
  - Implement `compact_byte_deltas()` function
  - Remove zero-delta entries from internal array
  - Shrink array with `realloc()`
  - Call during snapshot save to reduce file size
- **Acceptance Criteria**:
  - Unit test: Create deltas, erase some, compact, verify size reduced

### P6.T7: Write Byte Delta Unit Tests
- **Complexity**: Medium
- **Dependencies**: P6.T6
- **Files**: `host_test/test_byte_deltas.c`
- **Description**:
  - Test: Full page write, no deltas created
  - Test: Partial page write, deltas created for outliers
  - Test: Repeated partial writes, deltas accumulate
  - Test: Zero-delta filtering
  - Test: Delta memory limit enforcement
  - Test: Memory usage calculation
  - Test: Delta compaction
- **Acceptance Criteria**:
  - All tests pass

---

## Phase 7: Wear Lifetime Simulation Example

**Goal**: Create example simulation demonstrating 10K cycle simulation with snapshots

### P7.T1: Create Simulation Workload Generator
- **Complexity**: Medium
- **Dependencies**: P4.T11
- **Files**: Create `examples/wear_simulation/workload_generator.c`
- **Description**:
  - Implement `simulate_random_writes(device, count)` - random page writes
  - Implement `simulate_sequential_writes(device, count)` - sequential page writes
  - Implement `simulate_wear_leveling(device, count)` - evenly distributed erases
  - Use PRNG for reproducibility
- **Acceptance Criteria**:
  - Unit test: Verify write patterns match expectations

### P7.T2: Create Snapshot Manager
- **Complexity**: Low
- **Dependencies**: P4.T10, P4.T11
- **Files**: Create `examples/wear_simulation/snapshot_manager.c`
- **Description**:
  - Implement `snapshot_save_periodic(device, cycle, interval, base_dir)`
  - Generate filename: `wear_{cycle:05d}.bin`
  - Check if cycle % interval == 0
  - Call `nand_emul_save_snapshot()`
- **Acceptance Criteria**:
  - Unit test: Verify snapshots saved at correct intervals

### P7.T3: Create Wear Analysis Script
- **Complexity**: Medium
- **Dependencies**: P4.T13
- **Files**: Create `examples/wear_simulation/analyze_wear.py`
- **Description**:
  - Python script to load JSON exports
  - Calculate wear leveling metrics (min/max/avg erases, variation)
  - Generate plots (matplotlib): erase distribution histogram, hotspot heatmap
  - Output summary statistics
- **Acceptance Criteria**:
  - Script runs without errors
  - Generates PDF plots

### P7.T4: Create Main Simulation Program
- **Complexity**: Medium
- **Dependencies**: P7.T1, P7.T2, P5.T10
- **Files**: Create `examples/wear_simulation/main.c`
- **Description**:
  - Initialize 32MB flash with advanced config
  - Enable probabilistic failure model (rated_cycles = 100K)
  - Run 10,000 cycles (1000 ops per cycle = 10M operations)
  - Save snapshot every 100 cycles (100 total snapshots)
  - At end, export final state to JSON
  - Print simulation time and memory usage
- **Acceptance Criteria**:
  - Program compiles and runs
  - Completes 10K cycles in <5 minutes (single-core)

### P7.T5: Create Simulation Makefile
- **Complexity**: Low
- **Dependencies**: P7.T4
- **Files**: Create `examples/wear_simulation/Makefile`
- **Description**:
  - Build simulation program
  - Link against nand_emul library
  - Add targets: `make run` (run simulation), `make analyze` (run Python script)
- **Acceptance Criteria**:
  - `make all` builds successfully
  - `make run` executes simulation

### P7.T6: Document Simulation Workflow
- **Complexity**: Low
- **Dependencies**: P7.T5
- **Files**: Create `examples/wear_simulation/README.md`
- **Description**:
  - Explain simulation goals
  - List commands to build and run
  - Describe output files (snapshots, JSON)
  - Explain how to use analysis script
  - Show example plots
- **Acceptance Criteria**:
  - README is clear and complete

### P7.T7: Benchmark Performance
- **Complexity**: Low
- **Dependencies**: P7.T4
- **Files**: `examples/wear_simulation/benchmark.c`
- **Description**:
  - Measure time for 10K cycles with full tracking
  - Measure memory usage (peak RSS)
  - Compare with/without byte-level tracking
  - Output performance report
- **Acceptance Criteria**:
  - 10K cycles complete in <5 minutes
  - Memory usage <10MB for 32MB flash

### P7.T8: Test Different Flash Sizes
- **Complexity**: Low
- **Dependencies**: P7.T7
- **Files**: `examples/wear_simulation/` (modify main.c)
- **Description**:
  - Test with 32MB, 64MB, 128MB flash sizes
  - Verify memory scaling (linear with flash size)
  - Verify time scaling (linear with operations)
- **Acceptance Criteria**:
  - All sizes complete successfully
  - Memory usage stays <5% of flash size

---

## Phase 8: Testing & Documentation

**Goal**: Comprehensive testing, documentation, and polishing for release

### P8.T1: Write Wear Leveling Validation Test
- **Complexity**: Medium
- **Dependencies**: P4.T4
- **Files**: Create `host_test/test_wear_leveling.c`
- **Description**:
  - Write random blocks 10,000 times
  - Get wear statistics
  - Verify `max_block_erases - min_block_erases < threshold` (e.g., 100)
  - Verify `wear_leveling_variation < 0.1` (10% variation)
- **Acceptance Criteria**:
  - Test passes with proper wear leveling
  - Test fails with intentionally skewed writes

### P8.T2: Write Failure Injection Integration Test
- **Complexity**: Medium
- **Dependencies**: P5.T10
- **Files**: Create `host_test/test_failure_injection.c`
- **Description**:
  - Configure threshold model: max 10 erases per block
  - Erase same block 11 times
  - Verify 11th erase fails with `ESP_ERR_FLASH_OP_FAIL`
  - Verify block marked as bad
- **Acceptance Criteria**:
  - Test passes

### P8.T3: Write Memory Efficiency Test
- **Complexity**: Low
- **Dependencies**: P6.T5
- **Files**: Create `host_test/test_memory_efficiency.c`
- **Description**:
  - Initialize 32MB flash
  - Perform 1000 random operations
  - Get memory usage
  - Verify total < 5% of flash size (1.6MB)
- **Acceptance Criteria**:
  - Test passes

### P8.T4: Write Snapshot Roundtrip Test
- **Complexity**: Medium
- **Dependencies**: P4.T11
- **Files**: Create `host_test/test_snapshot_roundtrip.c`
- **Description**:
  - Perform various operations (erases, writes)
  - Save snapshot to file
  - Deinit device
  - Init new device
  - Load snapshot
  - Query metadata, verify matches original
- **Acceptance Criteria**:
  - All metadata restored correctly

### P8.T5: Write Snapshot Corruption Test
- **Complexity**: Low
- **Dependencies**: P4.T11
- **Files**: Create `host_test/test_snapshot_corruption.c`
- **Description**:
  - Save valid snapshot
  - Corrupt file (flip bits in header)
  - Attempt to load
  - Verify returns `ESP_ERR_INVALID_CRC`
- **Acceptance Criteria**:
  - Corruption detected

### P8.T6: Write Lifetime Simulation Test
- **Complexity**: High
- **Dependencies**: P7.T4
- **Files**: Create `host_test/test_lifetime_simulation.c`
- **Description**:
  - Run 10K cycles with probabilistic model
  - Save 100 snapshots
  - Load snapshots at cycles 0, 5000, 9999
  - Verify wear progression (erase counts increase)
  - Verify failure probability increases
- **Acceptance Criteria**:
  - Test completes in reasonable time (<5 min)
  - Wear progression observed

### P8.T7: Measure Code Coverage
- **Complexity**: Low
- **Dependencies**: All test tasks
- **Files**: Add to Makefile
- **Description**:
  - Add gcov/lcov flags to test build
  - Run all tests
  - Generate coverage report
  - Verify >80% code coverage
- **Acceptance Criteria**:
  - Coverage report generated
  - Target coverage achieved

### P8.T8: Write API Documentation
- **Complexity**: Medium
- **Dependencies**: All API implementation tasks
- **Files**: All header files
- **Description**:
  - Add Doxygen comments for all public functions
  - Document parameters, return values, error codes
  - Add usage examples in comments
  - Document backend and failure model interfaces
- **Acceptance Criteria**:
  - Doxygen builds without warnings
  - All public APIs documented

### P8.T9: Write Usage Examples
- **Complexity**: Medium
- **Dependencies**: P8.T8
- **Files**: Create `examples/basic_tracking/`, `examples/failure_injection/`
- **Description**:
  - **Basic Tracking**: Init with sparse hash backend, perform operations, query metadata
  - **Failure Injection**: Init with threshold model, trigger failures, handle errors
  - Each example includes Makefile and README
- **Acceptance Criteria**:
  - Examples compile and run
  - READMEs explain usage

### P8.T10: Create Architecture Diagrams
- **Complexity**: Low
- **Dependencies**: None
- **Files**: Create `docs/architecture.md`
- **Description**:
  - System component diagram (ASCII art or Graphviz)
  - Data flow diagrams (operation handlers → backend → metadata)
  - Snapshot file format diagram
  - Include in main README
- **Acceptance Criteria**:
  - Diagrams clear and accurate

### P8.T11: Update Main README
- **Complexity**: Medium
- **Dependencies**: P8.T10
- **Files**: `README.md`
- **Description**:
  - Add "Advanced Flash Tracking" section
  - Explain use cases and features
  - Link to examples and documentation
  - Show quick start code snippet
  - Include architecture diagram
  - Explain snapshot workflow
- **Acceptance Criteria**:
  - README is clear and comprehensive

### P8.T12: Run Full Regression Suite
- **Complexity**: Low
- **Dependencies**: All test tasks
- **Files**: N/A
- **Description**:
  - Run all unit tests
  - Run all integration tests
  - Run example programs
  - Verify no memory leaks (valgrind)
  - Verify no warnings (compile with -Wall -Wextra)
- **Acceptance Criteria**:
  - All tests pass
  - No leaks, no warnings

---

## Summary Statistics

- **Total Tasks**: 122
- **Phases**: 8
- **Estimated Effort**: 
  - Low complexity (1-2h): 42 tasks → ~60 hours
  - Medium complexity (3-6h): 54 tasks → ~240 hours
  - High complexity (1-2d): 26 tasks → ~320 hours
  - **Total**: ~620 hours (~4 months at full-time)

## Critical Path

The critical path through the implementation:

1. P1.T1 → P1.T2 → P1.T4 → P1.T5 → P1.T6 (Core infrastructure)
2. P2.T1 → P2.T2 → P2.T3 (Hash table foundation)
3. P2.T9 → P2.T11 (Block/page tracking)
4. P3.T3 → P3.T5 → P3.T6 (Integration)
5. P6.T2 → P6.T3 (Byte delta tracking)
6. P4.T8 → P4.T9 (Snapshots)
7. P7.T4 (Simulation example)
8. P8.T12 (Final validation)

**Estimated Critical Path Duration**: ~3 months

## Next Steps

1. Review this task breakdown with stakeholders
2. Prioritize phases based on MVP requirements
3. Assign tasks to developers
4. Set up CI/CD pipeline for automated testing
5. Begin Phase 1 implementation
