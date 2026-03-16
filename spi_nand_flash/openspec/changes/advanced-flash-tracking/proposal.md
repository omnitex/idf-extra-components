# Advanced Flash Tracking - Change Proposal

## Overview

Extend the Linux host test NAND flash memory emulation system (`nand_linux_mmap_emul.c`) to support comprehensive byte-level tracking, hierarchical metadata organization, and pluggable reliability modeling for advanced testing scenarios.

## Motivation

### Current Limitations

The existing `nand_linux_mmap_emul.c` provides basic NAND flash emulation with optional operation-level statistics (`CONFIG_NAND_ENABLE_STATS`), but lacks:

1. **Granular tracking**: Cannot track per-block, per-page, or per-byte wear patterns
2. **History**: No timestamp or state transition history for forensic analysis
3. **Failure modeling**: No ability to simulate realistic NAND failures (bit flips, block wear-out, program disturb)
4. **Wear analysis**: Cannot identify hotspots or validate wear leveling algorithms
5. **Memory efficiency**: Any tracking would require dense arrays (wasteful for sparse write patterns)

### Use Cases Enabled

1. **Wear Leveling Validation**: Track actual write/erase distribution to verify wear leveling algorithms distribute operations evenly across blocks
2. **Filesystem Robustness Testing**: Inject realistic failure models (probabilistic bit errors, worn-out blocks) to test recovery mechanisms
3. **Performance Analysis**: Identify write hotspots and amplification patterns for optimization
4. **Regression Testing**: Capture and compare wear patterns across code changes
5. **Documentation**: Generate visual heatmaps and statistics for technical presentations

## Proposed Solution

### Architecture Principles

1. **Backward Compatibility**: Existing `nand_emul_init()` API continues to work unchanged; advanced features are opt-in via new `nand_emul_advanced_init()`
2. **Open/Closed Principle**: Abstract interfaces for metadata storage backends and failure models enable extensibility without modifying core code
3. **Memory Efficiency**: Default sparse storage backend only allocates metadata for written pages/bytes
4. **Hierarchical Organization**: Three-level metadata structure (block → page → byte) matches NAND flash architecture
5. **Composability**: Metadata tracking and failure injection are independent; can use either or both

### System Components

```
┌─────────────────────────────────────────────────────────────┐
│                    Application / Tests                      │
└─────────────────┬───────────────────────────────────────────┘
                  │
                  │ nand_emul_* API calls
                  ↓
┌─────────────────────────────────────────────────────────────┐
│              nand_linux_mmap_emul.c (Core)                  │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  Operation Handlers: read/write/erase_block          │   │
│  └────────┬─────────────────────────────────────┬───────┘   │
│           │                                     │            │
│           ↓                                     ↓            │
│  ┌────────────────────┐              ┌──────────────────┐   │
│  │  Metadata Backend  │              │  Failure Model   │   │
│  │  (pluggable)       │              │  (pluggable)     │   │
│  └────────────────────┘              └──────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## Detailed Design

### 1. Data Structures

#### Core Metadata Types

```c
// Byte-level metadata (allocated sparsely)
typedef struct {
    uint32_t write_count;          // Number of times this byte was written
    uint64_t last_write_timestamp; // Monotonic timestamp of last write
    uint8_t  state_history_flags;  // Bit flags for state transitions (optional)
} byte_metadata_t;

// Page-level metadata
typedef struct {
    uint32_t program_count;         // Number of times page was programmed
    uint64_t first_program_timestamp; 
    uint64_t last_program_timestamp;
    uint32_t page_num;              // Page number (for iteration)
    bool     has_byte_metadata;     // Whether byte-level tracking is active
} page_metadata_t;

// Block-level metadata
typedef struct {
    uint32_t erase_count;           // Number of block erases
    uint64_t first_erase_timestamp;
    uint64_t last_erase_timestamp;
    uint32_t total_page_programs;   // Aggregate across all pages in block
    uint32_t block_num;             // Block number
    bool     is_bad_block;          // Simulated bad block flag
} block_metadata_t;

// Aggregate statistics
typedef struct {
    uint32_t total_blocks;
    uint32_t total_pages;
    uint64_t total_bytes_written;
    uint32_t min_block_erases;
    uint32_t max_block_erases;
    uint32_t avg_block_erases;
    uint32_t min_page_programs;
    uint32_t max_page_programs;
    uint32_t blocks_never_erased;
    uint32_t pages_never_written;
    double   wear_leveling_efficiency; // Ratio of max/min erase counts
} nand_wear_stats_t;
```

#### Operation Context (for failure models)

```c
// Context passed to failure model for decision making
typedef struct {
    uint32_t block_num;
    uint32_t page_num;
    size_t   byte_offset;
    size_t   operation_size;
    uint64_t timestamp;
    
    // Metadata at operation time
    const block_metadata_t *block_meta;
    const page_metadata_t  *page_meta;
    const byte_metadata_t  *byte_meta;  // NULL if not byte-tracked
    
    // Flash device parameters
    uint32_t total_blocks;
    uint32_t pages_per_block;
    uint32_t page_size;
} nand_operation_context_t;
```

### 2. Abstract Interfaces

#### Metadata Storage Backend

```c
typedef struct nand_metadata_backend_ops {
    // Backend lifecycle
    esp_err_t (*init)(void **backend_handle, const void *config);
    esp_err_t (*deinit)(void *backend_handle);
    
    // Block operations
    esp_err_t (*on_block_erase)(void *backend_handle, uint32_t block_num, uint64_t timestamp);
    esp_err_t (*get_block_info)(void *backend_handle, uint32_t block_num, block_metadata_t *out);
    esp_err_t (*set_bad_block)(void *backend_handle, uint32_t block_num, bool is_bad);
    
    // Page operations
    esp_err_t (*on_page_program)(void *backend_handle, uint32_t page_num, uint64_t timestamp);
    esp_err_t (*get_page_info)(void *backend_handle, uint32_t page_num, page_metadata_t *out);
    
    // Byte operations (optional, may return ESP_ERR_NOT_SUPPORTED)
    esp_err_t (*on_byte_write)(void *backend_handle, size_t addr, uint8_t old_val, 
                               uint8_t new_val, uint64_t timestamp);
    esp_err_t (*get_byte_info)(void *backend_handle, size_t addr, byte_metadata_t *out);
    
    // Query/iteration
    esp_err_t (*iterate_blocks)(void *backend_handle, 
                                bool (*callback)(uint32_t block_num, block_metadata_t *meta, void *user_data),
                                void *user_data);
    esp_err_t (*iterate_pages)(void *backend_handle,
                               bool (*callback)(uint32_t page_num, page_metadata_t *meta, void *user_data),
                               void *user_data);
    
    // Aggregate statistics
    esp_err_t (*get_stats)(void *backend_handle, nand_wear_stats_t *out);
    
    // Serialization (optional, for persistence)
    esp_err_t (*save_state)(void *backend_handle, const char *filename);
    esp_err_t (*load_state)(void *backend_handle, const char *filename);
} nand_metadata_backend_ops_t;
```

#### Failure Model Interface

```c
typedef struct nand_failure_model_ops {
    // Model lifecycle
    esp_err_t (*init)(void **model_handle, const void *config);
    esp_err_t (*deinit)(void *model_handle);
    
    // Failure decision points (return true to fail operation)
    bool (*should_fail_read)(void *model_handle, const nand_operation_context_t *ctx);
    bool (*should_fail_write)(void *model_handle, const nand_operation_context_t *ctx);
    bool (*should_fail_erase)(void *model_handle, const nand_operation_context_t *ctx);
    
    // Data corruption (modifies data buffer in-place)
    void (*corrupt_read_data)(void *model_handle, const nand_operation_context_t *ctx,
                              uint8_t *data, size_t len);
    
    // Block health assessment
    bool (*is_block_bad)(void *model_handle, uint32_t block_num, 
                         const block_metadata_t *meta);
} nand_failure_model_ops_t;
```

### 3. Configuration API

```c
// Advanced emulation configuration
typedef struct {
    // Base emulation config (reuses existing structure)
    nand_file_mmap_emul_config_t base_config;
    
    // Metadata backend
    const nand_metadata_backend_ops_t *metadata_backend;
    void *metadata_backend_config;  // Backend-specific config
    
    // Failure model (optional)
    const nand_failure_model_ops_t *failure_model;
    void *failure_model_config;     // Model-specific config
    
    // Tracking granularity
    bool track_block_level;   // Default: true
    bool track_page_level;    // Default: true
    bool track_byte_level;    // Default: false (memory intensive)
    
    // Timestamp source (NULL = use default monotonic counter)
    uint64_t (*get_timestamp)(void);
} nand_emul_advanced_config_t;

// Initialize with advanced features
esp_err_t nand_emul_advanced_init(spi_nand_flash_device_t *handle, 
                                   nand_emul_advanced_config_t *cfg);
```

### 4. Query API

```c
// Get metadata for specific block
esp_err_t nand_emul_get_block_wear(spi_nand_flash_device_t *handle, 
                                    uint32_t block_num, 
                                    block_metadata_t *out);

// Get metadata for specific page
esp_err_t nand_emul_get_page_wear(spi_nand_flash_device_t *handle,
                                   uint32_t page_num,
                                   page_metadata_t *out);

// Get aggregate statistics
esp_err_t nand_emul_get_wear_stats(spi_nand_flash_device_t *handle,
                                    nand_wear_stats_t *out);

// Iterate over all written blocks
esp_err_t nand_emul_iterate_worn_blocks(spi_nand_flash_device_t *handle,
                                        bool (*callback)(uint32_t block_num, 
                                                         block_metadata_t *meta,
                                                         void *user_data),
                                        void *user_data);

// Mark block as bad (simulated factory defect)
esp_err_t nand_emul_mark_bad_block(spi_nand_flash_device_t *handle,
                                    uint32_t block_num);

// Export wear map to file (JSON or CSV format)
esp_err_t nand_emul_export_wear_map(spi_nand_flash_device_t *handle,
                                     const char *filename,
                                     const char *format); // "json" or "csv"
```

### 5. Built-in Implementations

#### Default Sparse Hash Backend

```c
// Configuration for sparse hash backend
typedef struct {
    size_t initial_capacity;  // Initial hash table size (0 = auto)
    float  load_factor;       // Rehash threshold (default 0.75)
    bool   track_byte_level;  // Enable byte-level tracking
} sparse_hash_backend_config_t;

// Registration
extern const nand_metadata_backend_ops_t nand_sparse_hash_backend;
```

**Design**: 
- Three separate hash tables: `block_num → block_metadata_t`, `page_num → page_metadata_t`, `byte_addr → byte_metadata_t`
- Only allocate entries on first write/erase
- Memory usage: O(written_blocks + written_pages + written_bytes) instead of O(total_capacity)

#### Built-in Failure Models

**1. No-op Model** (default, zero overhead)
```c
extern const nand_failure_model_ops_t nand_no_failure_model;
```

**2. Threshold Model** (fail after N cycles)
```c
typedef struct {
    uint32_t max_block_erases;   // Default: 100,000
    uint32_t max_page_programs;  // Default: 100,000
    bool     fail_over_limit;    // true = fail operations, false = mark bad only
} threshold_failure_config_t;

extern const nand_failure_model_ops_t nand_threshold_failure_model;
```

**3. Probabilistic Model** (realistic wear-out curve)
```c
typedef struct {
    uint32_t rated_cycles;         // Cycles at 50% failure probability
    double   wear_out_shape;       // Weibull shape parameter (2.0 = bathtub curve)
    double   base_bit_error_rate;  // BER for fresh cells (e.g., 1e-8)
    uint32_t random_seed;          // For reproducible failures
} probabilistic_failure_config_t;

extern const nand_failure_model_ops_t nand_probabilistic_failure_model;
```

**Formula**: `P(fail) = 1 - exp(-((erase_count / rated_cycles) ^ wear_out_shape))`

### 6. Integration Points

#### Modified `nand_mmap_emul_handle_t`

```c
typedef struct {
    // Existing fields
    void *mem_file_buf;
    int mem_file_fd;
    nand_file_mmap_emul_config_t file_mmap_ctrl;
#ifdef CONFIG_NAND_ENABLE_STATS
    struct {
        size_t read_ops;
        size_t write_ops;
        size_t erase_ops;
        size_t read_bytes;
        size_t write_bytes;
    } stats;
#endif

    // New advanced tracking fields (only allocated if advanced_init used)
    struct {
        // Metadata backend
        const nand_metadata_backend_ops_t *metadata_ops;
        void *metadata_handle;
        
        // Failure model
        const nand_failure_model_ops_t *failure_ops;
        void *failure_handle;
        
        // Configuration
        bool track_block_level;
        bool track_page_level;
        bool track_byte_level;
        
        // Timestamp source
        uint64_t (*get_timestamp)(void);
        uint64_t operation_counter; // Default timestamp source
        
        // Cached device geometry
        uint32_t total_blocks;
        uint32_t pages_per_block;
        uint32_t page_size;
    } *advanced;  // NULL if not using advanced features
} nand_mmap_emul_handle_t;
```

#### Modified Operation Handlers

**Example: `nand_emul_erase_block()` with advanced tracking**

```c
esp_err_t nand_emul_erase_block(spi_nand_flash_device_t *handle, size_t offset)
{
    nand_mmap_emul_handle_t *emul = handle->emul_handle;
    
    // Existing validation
    if (emul->mem_file_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (offset + handle->chip.block_size > emul->file_mmap_ctrl.flash_file_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    uint32_t block_num = offset / handle->chip.block_size;
    
    // Advanced: Check failure model before operation
    if (emul->advanced && emul->advanced->failure_ops) {
        nand_operation_context_t ctx = {
            .block_num = block_num,
            .timestamp = emul->advanced->get_timestamp(),
            // ... fill other fields
        };
        
        if (emul->advanced->failure_ops->should_fail_erase(
                emul->advanced->failure_handle, &ctx)) {
            ESP_LOGW(TAG, "Simulated erase failure at block %u", block_num);
            return ESP_ERR_FLASH_OP_FAIL;
        }
    }
    
    // Perform erase
    void *dst_addr = emul->mem_file_buf + offset;
    memset(dst_addr, 0xFF, handle->chip.block_size);
    
    // Advanced: Update metadata after successful operation
    if (emul->advanced && emul->advanced->track_block_level 
        && emul->advanced->metadata_ops) {
        uint64_t timestamp = emul->advanced->get_timestamp();
        emul->advanced->metadata_ops->on_block_erase(
            emul->advanced->metadata_handle, block_num, timestamp);
    }
    
    // Existing stats
#ifdef CONFIG_NAND_ENABLE_STATS
    emul->stats.erase_ops++;
#endif
    
    return ESP_OK;
}
```

## Implementation Plan

### Phase 1: Core Infrastructure (Week 1)
- [ ] Define all data structures and interface types in new header `nand_emul_advanced.h`
- [ ] Extend `nand_mmap_emul_handle_t` with optional `advanced` pointer
- [ ] Implement `nand_emul_advanced_init()` and `nand_emul_advanced_deinit()`
- [ ] Add no-op metadata backend and failure model (identity implementations)
- [ ] Write basic unit test: init with advanced config, verify no crash

### Phase 2: Sparse Hash Backend (Week 1-2)
- [ ] Implement hash table data structure (khash or custom)
- [ ] Implement `sparse_hash_backend_config_t` and all ops callbacks
- [ ] Add block-level tracking: `on_block_erase()`, `get_block_info()`
- [ ] Add page-level tracking: `on_page_program()`, `get_page_info()`
- [ ] Implement `iterate_blocks()` and `get_stats()`
- [ ] Unit tests: verify correct counts, iteration, memory efficiency

### Phase 3: Integration with Core Operations (Week 2)
- [ ] Modify `nand_emul_erase_block()` to call metadata backend and failure model
- [ ] Modify `nand_emul_write()` to track page programs and optional byte writes
- [ ] Modify `nand_emul_read()` to call failure model for corruption injection
- [ ] Add timestamp generation (monotonic counter by default)
- [ ] Integration tests: run existing NAND tests, verify backward compatibility

### Phase 4: Query API (Week 2-3)
- [ ] Implement `nand_emul_get_block_wear()`
- [ ] Implement `nand_emul_get_page_wear()`
- [ ] Implement `nand_emul_get_wear_stats()`
- [ ] Implement `nand_emul_iterate_worn_blocks()`
- [ ] Implement `nand_emul_mark_bad_block()`
- [ ] Unit tests for all query functions

### Phase 5: Built-in Failure Models (Week 3)
- [ ] Implement threshold failure model with configurable limits
- [ ] Implement probabilistic failure model (Weibull distribution)
- [ ] Add bit flip injection in `corrupt_read_data()`
- [ ] Unit tests: verify failure probabilities match configuration

### Phase 6: Advanced Features (Week 3-4)
- [ ] Add byte-level tracking to sparse hash backend (optional, off by default)
- [ ] Implement `nand_emul_export_wear_map()` (JSON and CSV formats)
- [ ] Add `save_state()` / `load_state()` for metadata persistence
- [ ] Performance tests: measure overhead of tracking at different granularities

### Phase 7: Testing & Documentation (Week 4)
- [ ] Comprehensive unit tests: 80%+ code coverage
- [ ] Wear leveling validation test: write to random blocks, verify even distribution
- [ ] Failure injection test: configure threshold model, verify operations fail after limit
- [ ] Memory efficiency test: verify sparse storage uses <1% of dense alternative
- [ ] API documentation with Doxygen comments
- [ ] Usage examples: basic tracking, wear analysis, failure injection
- [ ] Add section to README with architecture diagrams

### Phase 8: Optional Extensions (Future)
- [ ] SQLite-based metadata backend (persistent, queryable)
- [ ] Real-time wear visualization (web dashboard)
- [ ] Machine learning-based failure prediction
- [ ] Multi-die support (parallel die metadata)

## Testing Strategy

### Unit Tests
- **Metadata Backend Tests**: Verify hash table correctness, collision handling, memory leaks
- **Failure Model Tests**: Validate probability distributions, reproducibility with fixed seeds
- **API Tests**: Cover all public functions, error paths, edge cases
- **Backward Compatibility**: Ensure existing tests pass without modification

### Integration Tests
- **Wear Leveling Validation**: 
  ```c
  // Write 10,000 operations, verify max_erases - min_erases < threshold
  for (int i = 0; i < 10000; i++) {
      write_random_block();
  }
  nand_wear_stats_t stats;
  nand_emul_get_wear_stats(handle, &stats);
  TEST_ASSERT(stats.max_block_erases - stats.min_block_erases < 100);
  ```

- **Failure Injection**:
  ```c
  // Configure threshold model: max 10 erases per block
  threshold_failure_config_t cfg = { .max_block_erases = 10 };
  // Erase same block 11 times, expect failure on 11th
  for (int i = 0; i < 11; i++) {
      esp_err_t ret = nand_emul_erase_block(handle, 0);
      if (i < 10) TEST_ASSERT_EQUAL(ESP_OK, ret);
      else TEST_ASSERT_EQUAL(ESP_ERR_FLASH_OP_FAIL, ret);
  }
  ```

- **Memory Efficiency**:
  ```c
  // Write to 1% of pages, verify metadata uses <10KB (not 128MB/256 per byte)
  size_t initial_heap = esp_get_free_heap_size();
  write_sparse_pattern(handle, 0.01); // 1% of pages
  size_t used_heap = initial_heap - esp_get_free_heap_size();
  TEST_ASSERT_LESS_THAN(10 * 1024, used_heap);
  ```

### Performance Benchmarks
- Measure overhead of tracking at block/page/byte granularity
- Target: <5% performance degradation for block+page tracking, <20% for byte tracking

## Migration Path

### Existing Code (No Changes Required)
```c
// This continues to work exactly as before
spi_nand_flash_device_t dev;
nand_file_mmap_emul_config_t cfg = {
    .flash_file_name = "/tmp/nand.bin",
    .flash_file_size = 128 * 1024 * 1024,
    .keep_dump = false
};
nand_emul_init(&dev, &cfg);
```

### Opt-in to Advanced Features
```c
// New code can use advanced features
spi_nand_flash_device_t dev;
nand_emul_advanced_config_t adv_cfg = {
    .base_config = {
        .flash_file_name = "/tmp/nand.bin",
        .flash_file_size = 128 * 1024 * 1024,
        .keep_dump = false
    },
    .metadata_backend = &nand_sparse_hash_backend,
    .metadata_backend_config = &(sparse_hash_backend_config_t){
        .track_byte_level = false // Block+page only for efficiency
    },
    .failure_model = &nand_threshold_failure_model,
    .failure_model_config = &(threshold_failure_config_t){
        .max_block_erases = 100000
    },
    .track_block_level = true,
    .track_page_level = true,
    .track_byte_level = false
};
nand_emul_advanced_init(&dev, &adv_cfg);

// Query wear statistics
nand_wear_stats_t stats;
nand_emul_get_wear_stats(&dev, &stats);
printf("Wear leveling efficiency: %.2f\n", stats.wear_leveling_efficiency);
```

## Success Criteria

1. **Backward Compatibility**: All existing host tests pass without modification
2. **Memory Efficiency**: Sparse backend uses <1% memory of dense alternative for typical workloads
3. **Performance**: <5% overhead for block+page tracking, <20% for byte tracking
4. **Code Coverage**: >80% unit test coverage for new code
5. **Documentation**: Complete API docs and usage examples
6. **Validation**: At least one realistic wear leveling validation test using metadata

## Non-Goals

- **Target Device Implementation**: This is host-test only; not for ESP32 target
- **Real Hardware Simulation**: Not simulating electrical characteristics (voltage, timing)
- **Multi-threading**: Single-threaded access model (matches current emulation)
- **Infinite History**: Metadata tracks current state + timestamps, not full operation log

## Open Questions

1. **Should byte-level tracking be opt-in per address range** (e.g., only track OOB area)?
   - Proposal: Add `track_address_range(start, end)` API for selective byte tracking
   
2. **Persistence format for metadata**: JSON (human-readable) vs. binary (compact)?
   - Proposal: Support both via backend interface
   
3. **Should we track ECC errors separately** from bit flips?
   - Proposal: Add `ecc_corrected_errors` counter to page metadata
   
4. **Integration with existing `CONFIG_NAND_ENABLE_STATS`**: Merge or keep separate?
   - Proposal: Keep separate; advanced tracking is much richer than simple counters

## References

- Existing code: `src/nand_linux_mmap_emul.c`, `include/nand_linux_mmap_emul.h`
- NAND flash reliability: "Understanding Flash: The Building Blocks" (Micron TN-29-42)
- Wear leveling algorithms: "Design Tradeoffs for SSD Performance" (USENIX 2008)
- Similar implementations: QEMU NAND emulation, FlashSim

## Changelog

- **2026-03-16**: Initial proposal
