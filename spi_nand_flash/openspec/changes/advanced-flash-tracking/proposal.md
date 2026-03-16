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
// Byte-level metadata (delta-encoded, sparse storage)
// Stores deltas from page-level aggregate for bytes with different write patterns
typedef struct {
    uint16_t byte_offset;          // Offset within page (0-4095)
    int16_t  write_count_delta;    // Delta from page program_count (-32K to +32K)
    uint64_t last_write_timestamp; // Last write timestamp for this specific byte
} byte_delta_metadata_t;

// Page-level metadata
typedef struct {
    uint32_t program_count;         // Number of programs in the CURRENT erase cycle (reset to 0 on block erase)
    uint32_t program_count_total;   // Lifetime total programs (never reset; accumulates across all erase cycles)
    uint64_t first_program_timestamp; 
    uint64_t last_program_timestamp;
    uint32_t page_num;              // Page number (for iteration)
    uint16_t byte_delta_count;      // Number of bytes with non-zero deltas in the current erase cycle
    byte_delta_metadata_t *byte_deltas; // Sparse array of byte-level deltas (NULL if none; reset on block erase)
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
    uint32_t min_page_programs;      // Based on program_count_total (lifetime)
    uint32_t max_page_programs;      // Based on program_count_total (lifetime)
    uint32_t blocks_never_erased;
    uint32_t pages_never_written;
    double   wear_leveling_variation;  // (max - min) / avg erase counts (lower is better)
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
    const byte_delta_metadata_t *byte_deltas;  // NULL if no deltas for this page
    uint16_t byte_delta_count;                  // Number of deltas available
    
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
    
    // Byte delta operations (optional, may return ESP_ERR_NOT_SUPPORTED)
    // Called for every write operation when byte_level tracking is enabled
    // Backend optimizes away zero-deltas (bytes written same count as page)
    esp_err_t (*on_byte_write_range)(void *backend_handle, uint32_t page_num, 
                                     uint16_t byte_offset, size_t len, uint64_t timestamp);
    esp_err_t (*get_byte_deltas)(void *backend_handle, uint32_t page_num, 
                                 byte_delta_metadata_t **out_deltas, uint16_t *out_count);
    
    // Query/iteration
    esp_err_t (*iterate_blocks)(void *backend_handle, 
                                bool (*callback)(uint32_t block_num, block_metadata_t *meta, void *user_data),
                                void *user_data);
    esp_err_t (*iterate_pages)(void *backend_handle,
                               bool (*callback)(uint32_t page_num, page_metadata_t *meta, void *user_data),
                               void *user_data);
    
    // Aggregate statistics
    esp_err_t (*get_stats)(void *backend_handle, nand_wear_stats_t *out);
    
    // Binary snapshot (optimized for wear lifetime simulation)
    esp_err_t (*save_snapshot)(void *backend_handle, const char *filename, uint64_t timestamp);
    esp_err_t (*load_snapshot)(void *backend_handle, const char *filename);
    
    // Export to analysis formats (JSON for graphing/plotting)
    esp_err_t (*export_json)(void *backend_handle, const char *filename);
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
    bool track_byte_level;    // Default: true (uses delta encoding for efficiency)
    
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

// Export wear map to JSON for analysis (human-readable)
esp_err_t nand_emul_export_json(spi_nand_flash_device_t *handle,
                                const char *filename);

// Binary snapshot management (for wear lifetime simulation)
esp_err_t nand_emul_save_snapshot(spi_nand_flash_device_t *handle,
                                  const char *filename);
esp_err_t nand_emul_load_snapshot(spi_nand_flash_device_t *handle,
                                  const char *filename);

// Get byte-level deltas for a specific page
// out_deltas: backend-owned; valid until next metadata-modifying call or deinit; caller must not free
esp_err_t nand_emul_get_byte_deltas(spi_nand_flash_device_t *handle,
                                    uint32_t page_num,
                                    byte_delta_metadata_t **out_deltas,
                                    uint16_t *out_count);
```

For blocks/pages that have never been erased or written, `nand_emul_get_block_wear()` and `nand_emul_get_page_wear()` SHALL return `ESP_OK` and SHALL fill `out` with a zeroed metadata structure (no error).

### 5. Built-in Implementations

#### Default Sparse Hash Backend

```c
// Configuration for sparse hash backend
typedef struct {
    size_t initial_capacity;  // Initial hash table size (0 = auto)
    float  load_factor;       // Rehash threshold (default 0.75)
    bool   track_byte_deltas; // Enable byte-level delta tracking
} sparse_hash_backend_config_t;

// Registration
extern const nand_metadata_backend_ops_t nand_sparse_hash_backend;
```

**Design**: 

- Two primary hash tables: `block_num → block_metadata_t`, `page_num → page_metadata_t`
- Byte-level: Delta encoding from page aggregate (only track outlier bytes)
  - Page programmed 47 times → most bytes written 47 times (implicit)
  - Byte at offset 0x10 written 53 times → store delta: `{offset: 0x10, delta: +6}`
- Memory usage: 
  - Block metadata: O(written_blocks) ≈ 64 bytes/block
  - Page metadata: O(written_pages) ≈ 32 bytes/page
  - Byte deltas: O(outlier_bytes) ≈ 12 bytes/outlier
  - **32MB flash**: ~512KB (blocks) + ~512KB (pages) + ~50KB (deltas) ≈ **1.1MB** (3% of flash size)
  - **128MB flash**: ~2MB (blocks) + ~2MB (pages) + ~200KB (deltas) ≈ **4.2MB** (3% of flash size)

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

### 6. Binary Snapshot Format

For wear lifetime simulation, the system supports efficient binary snapshots:

```
SNAPSHOT FILE FORMAT (Version 1)
════════════════════════════════════════════════════

Header (64 bytes, aligned):
┌────────────────────────────────────────────────┐
│ Magic: 0x4E414E44 ("NAND")         4 bytes     │
│ Version: 0x01                      1 byte      │
│ Flags: [byte|page|block tracking]  1 byte      │
│ Reserved                           2 bytes     │
│ Timestamp: uint64_t                8 bytes     │
│ Total blocks: uint32_t             4 bytes     │
│ Pages per block: uint32_t          4 bytes     │
│ Page size: uint32_t                4 bytes     │
│ Block metadata count: uint32_t     4 bytes     │
│ Page metadata count: uint32_t      4 bytes     │
│ Byte delta count: uint32_t         4 bytes     │
│ Block metadata offset: uint64_t    8 bytes     │
│ Page metadata offset: uint64_t     8 bytes     │
│ Byte delta offset: uint64_t        8 bytes     │
│ Checksum (CRC32): uint32_t    4 bytes     │  ← CRC32 of entire file
└────────────────────────────────────────────────┘

Block Metadata Section (variable):
  For each written block (sparse):
  - block_num: uint32_t
  - erase_count: uint32_t
  - first_erase_ts: uint64_t
  - last_erase_ts: uint64_t
  - total_page_programs: uint32_t
  - is_bad_block: uint8_t
  - (padding): 3 bytes
  TOTAL: 40 bytes per block

Page Metadata Section (variable):
  For each written page (sparse):
  - page_num: uint32_t
  - program_count: uint32_t       (current erase cycle)
  - program_count_total: uint32_t (lifetime cumulative)
  - (padding): 4 bytes
  - first_program_ts: uint64_t
  - last_program_ts: uint64_t
  - byte_delta_count: uint16_t
  - byte_delta_offset: uint32_t (relative offset in byte delta section)
  - (padding): 2 bytes
  TOTAL: 40 bytes per page

Byte Delta Section (variable):
  For each outlier byte (compressed):
  - page_num: uint32_t
  - byte_offset: uint16_t
  - write_count_delta: int16_t
  - last_write_ts: uint64_t
  TOTAL: 16 bytes per delta
```

**Snapshot integrity and versioning**:
- **Checksum**: CRC32 is computed over the **header only** (the first 60 bytes, excluding the 4-byte checksum field at the end). On load, the backend verifies it and returns an error if mismatch.
- **Version**: Only version 1 is supported. Load SHALL return `ESP_ERR_NOT_SUPPORTED` for any other version and SHALL NOT change backend state.

**Snapshot Size Estimates**:

- 32MB flash, 10% blocks written, 1% byte outliers: ~500KB per snapshot
- 128MB flash, 10% blocks written, 1% byte outliers: ~2MB per snapshot
- 100 snapshots for lifetime simulation: 50MB - 200MB total

**Snapshot Scope**:

- Snapshots contain **metadata only** (erase counts, timestamps, byte deltas)
- Snapshots do **NOT** contain flash data (actual memory contents)
- Use case: Analyze wear patterns at different points in simulation
- Note: To checkpoint full simulation state including flash data, separately save the mmap file

**Performance**:

- Save snapshot: ~5-10ms (single-core)
- Load snapshot: ~10-20ms (single-core)

### 7. Wear Lifetime Simulation

The snapshot system enables realistic lifetime simulation:

```c
// Example: Simulate 10,000 erase cycles with periodic snapshots
nand_emul_advanced_config_t cfg = {
    .base_config = {
        .flash_file_name = "/tmp/sim_nand.bin",
        .flash_file_size = 32 * 1024 * 1024,  // 32MB
        .keep_dump = false
    },
    .metadata_backend = &nand_sparse_hash_backend,
    .metadata_backend_config = &(sparse_hash_backend_config_t){
        .track_byte_deltas = true
    },
    .failure_model = &nand_probabilistic_failure_model,
    .failure_model_config = &(probabilistic_failure_config_t){
        .rated_cycles = 100000,
        .wear_out_shape = 2.0,
        .base_bit_error_rate = 1e-8,
        .random_seed = 12345  // Reproducible
    },
    .track_block_level = true,
    .track_page_level = true,
    .track_byte_level = true
};

nand_emul_advanced_init(&dev, &cfg);

// Simulate wear leveling workload
for (int cycle = 0; cycle < 10000; cycle++) {
    // Perform random writes/erases (1000 ops per cycle)
    simulate_workload(&dev, 1000);
    
    // Save snapshot every 100 cycles
    if (cycle % 100 == 0) {
        char snapshot[64];
        snprintf(snapshot, sizeof(snapshot), "wear_%05d.bin", cycle);
        nand_emul_save_snapshot(&dev, snapshot);
    }
}

// Analysis: Load specific snapshot and export to JSON
nand_emul_load_snapshot(&dev, "wear_05000.bin");
nand_emul_export_json(&dev, "wear_5000.json");
```

**Simulation Performance** (single-core, 32MB flash):

- 10,000 cycles × 1000 ops = 10M operations
- Estimated time: 2-5 minutes with full tracking
- 100 snapshots: +1-2 seconds overhead
- **Total: ~3-6 minutes for full simulation**

For larger flash (128MB), scale accordingly (~10-20 minutes).

### 8. Integration Points

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

**Example: `nand_emul_write()` with byte delta tracking**

```c
esp_err_t nand_emul_write(spi_nand_flash_device_t *handle, size_t offset, 
                          const void *data, size_t len)
{
    nand_mmap_emul_handle_t *emul = handle->emul_handle;
    
    // Existing validation
    if (emul->mem_file_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Advanced: Check failure model before operation
    if (emul->advanced && emul->advanced->failure_ops) {
        // Build context and check for failure
        // ... (similar to erase example)
    }
    
    // Perform write
    void *dst_addr = emul->mem_file_buf + offset;
    memcpy(dst_addr, data, len);
    
    // Advanced: Track metadata after successful operation
    if (emul->advanced && emul->advanced->metadata_ops) {
        uint64_t timestamp = emul->advanced->get_timestamp();
        uint32_t page_size = handle->chip.page_size;
        
        // Calculate affected pages
        uint32_t first_page = offset / page_size;
        uint32_t last_page = (offset + len - 1) / page_size;
        
        for (uint32_t page_num = first_page; page_num <= last_page; page_num++) {
            // Track page program
            if (emul->advanced->track_page_level) {
                emul->advanced->metadata_ops->on_page_program(
                    emul->advanced->metadata_handle, page_num, timestamp);
            }
            
            // Track byte-level deltas (conservative approach)
            if (emul->advanced->track_byte_level) {
                size_t page_start = page_num * page_size;
                size_t write_start = (offset > page_start) ? offset : page_start;
                size_t write_end = ((offset + len) < (page_start + page_size)) 
                                   ? (offset + len) : (page_start + page_size);
                
                uint16_t byte_offset_in_page = write_start - page_start;
                size_t write_len = write_end - write_start;
                
                // Record byte range write (backend will create deltas if needed)
                emul->advanced->metadata_ops->on_byte_write_range(
                    emul->advanced->metadata_handle, 
                    page_num, byte_offset_in_page, write_len, timestamp);
            }
        }
    }
    
    // Existing stats
#ifdef CONFIG_NAND_ENABLE_STATS
    emul->stats.write_ops++;
    emul->stats.write_bytes += len;
#endif
    
    return ESP_OK;
}
```

**Backend optimization**: The sparse hash backend compares byte write counts against page program count. If all bytes in a page have the same count as the page, no deltas are stored (zero-delta optimization).

## Implementation Plan

### Phase 1: Core Infrastructure (Week 1)

- Define all data structures and interface types in new header `nand_emul_advanced.h`
- Extend `nand_mmap_emul_handle_t` with optional `advanced` pointer
- Implement `nand_emul_advanced_init()` and `nand_emul_advanced_deinit()`
- Add no-op metadata backend and failure model (identity implementations)
- Write basic unit test: init with advanced config, verify no crash

### Phase 2: Sparse Hash Backend (Week 1-2)

- Implement hash table data structure (khash or custom)
- Implement `sparse_hash_backend_config_t` and all ops callbacks
- Add block-level tracking: `on_block_erase()`, `get_block_info()`
- Add page-level tracking: `on_page_program()`, `get_page_info()`
- Implement `iterate_blocks()` and `get_stats()`
- Unit tests: verify correct counts, iteration, memory efficiency

### Phase 3: Integration with Core Operations (Week 2)

- Modify `nand_emul_erase_block()` to call metadata backend and failure model
- Modify `nand_emul_write()` to track page programs and byte-level deltas
- Detect partial page programs and record byte deltas automatically
- Modify `nand_emul_read()` to call failure model for corruption injection
- Add timestamp generation (monotonic counter by default)
- Integration tests: run existing NAND tests, verify backward compatibility

### Phase 4: Query API & Snapshots (Week 2-3)

- Implement `nand_emul_get_block_wear()`
- Implement `nand_emul_get_page_wear()`
- Implement `nand_emul_get_byte_deltas()`
- Implement `nand_emul_get_wear_stats()`
- Implement `nand_emul_iterate_worn_blocks()`
- Implement `nand_emul_mark_bad_block()`
- Implement binary snapshot save/load functions
- Implement JSON export for analysis
- Unit tests for all query and snapshot functions

### Phase 5: Built-in Failure Models (Week 3)

- Implement threshold failure model with configurable limits
- Implement probabilistic failure model (Weibull distribution)
- Add bit flip injection in `corrupt_read_data()`
- Unit tests: verify failure probabilities match configuration

### Phase 6: Wear Lifetime Simulation (Week 3-4)

- Add byte-level delta tracking to sparse hash backend
- Optimize delta encoding for memory efficiency (target: <5% overhead)
- Implement snapshot compression/optimization
- Create wear simulation example: 10K cycles with periodic snapshots
- Performance tests: measure operation overhead with full tracking
- Benchmark: simulate 32MB flash lifetime in <5 minutes
- Validate: memory usage stays under 10MB for 32MB flash simulation

### Phase 7: Testing & Documentation (Week 4)

- Comprehensive unit tests: 80%+ code coverage
- Wear leveling validation test: write to random blocks, verify even distribution
- Failure injection test: configure threshold model, verify operations fail after limit
- Memory efficiency test: verify sparse storage with deltas uses <5% of flash size
- Snapshot roundtrip test: save, load, verify metadata integrity
- Lifetime simulation test: 10K cycles, 100 snapshots, analyze wear progression
- API documentation with Doxygen comments
- Usage examples: basic tracking, wear analysis, failure injection, simulation
- Add section to README with architecture diagrams and snapshot workflow

### Phase 8: Optional Extensions (Future)

- SQLite-based metadata backend (persistent, queryable)
- Real-time wear visualization (web dashboard)
- Machine learning-based failure prediction
- Multi-die support (parallel die metadata)

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
- **Memory Efficiency with Byte Deltas**:
  ```c
  // Write workload with 10% partial page programs, verify delta encoding efficiency
  // Dense byte tracking would need: 32MB × 16 bytes/byte = 512MB
  // Delta encoding should use: <10MB for typical workload
  size_t initial_heap = esp_get_free_heap_size();

  // Simulate workload: 1000 full page writes + 100 partial page writes
  for (int i = 0; i < 1000; i++) {
      write_full_page(handle, random_page());
  }
  for (int i = 0; i < 100; i++) {
      write_partial_page(handle, random_page(), 64);  // Only OOB area
  }

  size_t used_heap = initial_heap - esp_get_free_heap_size();
  TEST_ASSERT_LESS_THAN(10 * 1024 * 1024, used_heap);  // <10MB

  // Verify deltas were tracked
  byte_delta_metadata_t *deltas;
  uint16_t count;
  nand_emul_get_byte_deltas(handle, partial_page_num, &deltas, &count);
  TEST_ASSERT_GREATER_THAN(0, count);  // Partial writes created deltas
  ```
- **Snapshot Integrity**:
  ```c
  // Perform operations, save snapshot, load, verify consistency
  perform_random_operations(handle, 10000);
  nand_wear_stats_t before;
  nand_emul_get_wear_stats(handle, &before);

  nand_emul_save_snapshot(handle, "test_snapshot.bin");

  // Reset emulator and load snapshot
  nand_emul_deinit(handle);
  nand_emul_advanced_init(handle, &cfg);
  nand_emul_load_snapshot(handle, "test_snapshot.bin");

  nand_wear_stats_t after;
  nand_emul_get_wear_stats(handle, &after);

  // Verify stats match exactly
  TEST_ASSERT_EQUAL(before.total_blocks, after.total_blocks);
  TEST_ASSERT_EQUAL(before.max_block_erases, after.max_block_erases);
  // ... verify all fields
  ```

### Performance Benchmarks

- Measure overhead of tracking at block/page/byte granularity
- Target: <5% performance degradation for block+page tracking, <15% for byte delta tracking
- Snapshot performance: <10ms save, <20ms load for 32MB flash
- Lifetime simulation: <5 minutes for 10K cycles on 32MB flash (single-core)

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

### Opt-in to Advanced Features with Byte Tracking

```c
// New code can use advanced features with byte-level delta tracking
spi_nand_flash_device_t dev;
nand_emul_advanced_config_t adv_cfg = {
    .base_config = {
        .flash_file_name = "/tmp/nand.bin",
        .flash_file_size = 32 * 1024 * 1024,  // 32MB for simulation
        .keep_dump = false
    },
    .metadata_backend = &nand_sparse_hash_backend,
    .metadata_backend_config = &(sparse_hash_backend_config_t){
        .track_byte_deltas = true  // Enable delta encoding
    },
    .failure_model = &nand_probabilistic_failure_model,
    .failure_model_config = &(probabilistic_failure_config_t){
        .rated_cycles = 100000,
        .wear_out_shape = 2.0,
        .base_bit_error_rate = 1e-8,
        .random_seed = 12345
    },
    .track_block_level = true,
    .track_page_level = true,
    .track_byte_level = true  // Track byte deltas automatically
};
nand_emul_advanced_init(&dev, &adv_cfg);

// Run wear simulation, take snapshots
for (int cycle = 0; cycle < 10000; cycle++) {
    simulate_workload(&dev, 100);
    if (cycle % 100 == 0) {
        char snapshot[64];
        snprintf(snapshot, sizeof(snapshot), "sim_%05d.bin", cycle);
        nand_emul_save_snapshot(&dev, snapshot);
    }
}

// Export final state to JSON for analysis
nand_emul_export_json(&dev, "wear_analysis.json");

// Query wear statistics
nand_wear_stats_t stats;
nand_emul_get_wear_stats(&dev, &stats);
printf("Wear variation: %.2f\n", stats.wear_leveling_variation);
printf("Max block erases: %u\n", stats.max_block_erases);
```

## Success Criteria

1. **Backward Compatibility**: All existing host tests pass without modification
2. **Memory Efficiency**: Delta-encoded byte tracking uses <5% of flash size for typical workloads (vs. 1600% for dense)
3. **Performance**: <5% overhead for block+page tracking, <15% for byte delta tracking
4. **Snapshot Performance**: <10ms save, <20ms load for 32MB flash
5. **Simulation Performance**: 10K cycle simulation completes in <5 minutes (32MB, single-core)
6. **Code Coverage**: >80% unit test coverage for new code
7. **Documentation**: Complete API docs and usage examples including snapshot workflow
8. **Validation**: At least one realistic wear leveling validation test using metadata and snapshots

## Non-Goals

- **Target Device Implementation**: This is host-test only; not for ESP32 target
- **Real Hardware Simulation**: Not simulating electrical characteristics (voltage, timing)
- **Multi-threading**: Single-threaded access model (matches current emulation)
- **Infinite History**: Metadata tracks current state + timestamps, not full operation log

## Resolved Design Decisions

1. **Byte-level tracking approach**: Delta encoding from page-level aggregate
  - **Decision**: Store only deltas for bytes that differ from page program count
  - **Rationale**: 95% memory savings vs. dense tracking; most bytes in a page are written together
  - **Memory**: ~3-5MB for 32MB flash vs. 512MB for dense tracking
2. **Persistence format**: Binary snapshots + JSON export
  - **Decision**: Binary for snapshots (fast, compact), JSON for analysis (human-readable)
  - **Rationale**: Snapshots enable time-series wear simulation; JSON enables plotting/graphing
  - **Use case**: Save snapshots every 100 cycles during 10K cycle simulation
  - **Scope**: Snapshots contain metadata only (not flash data); use for wear pattern analysis
3. **Wear metric naming**: `wear_leveling_variation` (not "efficiency")
  - **Decision**: `(max - min) / avg` where lower is better
  - **Rationale**: Clearer semantics (variation vs. efficiency); matches intuition
4. **Simulation scale**: Target 10,000 cycles for validation (10% of lifetime)
  - **Decision**: Optimize for 10K cycles in <5 minutes; support 100K cycles overnight
  - **Rationale**: 10K cycles sufficient to validate wear leveling; 100K available if needed
5. **Partial page write detection**: Always track deltas when byte-level enabled (conservative)
  - **Decision**: Every write operation creates byte deltas; backend optimizes away zero-deltas
  - **Rationale**: Simple implementation, never misses partial writes, backend handles optimization
  - **Implementation**: Write handler records all byte ranges; backend compares against page program count

### Resolved Edge Cases

6. **`wear_leveling_variation` when no blocks erased (avg = 0)**
  - **Question**: Formula `(max - min) / avg` divides by zero when no block has been erased.
  - **Decision**: When `avg_block_erases == 0` (or no block has been erased), `wear_leveling_variation` SHALL be 0.0. Pristine flash has no variation.
  - **Rationale**: Matches "Statistics for pristine flash"; avoids undefined behavior; 0.0 is the correct semantic (no spread).

7. **`is_block_bad` (failure model) vs `nand_emul_mark_bad_block` (explicit mark)**
  - **Question**: How do the two sources of "bad block" combine?
  - **Decision**: A block is considered bad if **either** (a) it was explicitly marked via `nand_emul_mark_bad_block` / backend `set_bad_block`, or (b) the failure model's `is_block_bad()` returns true. When the failure model returns true, the core SHALL call the backend's `set_bad_block(block_num, true)` so metadata reflects the status. The core operation handler SHALL then immediately return `ESP_ERR_FLASH_BAD_BLOCK` and SHALL NOT modify flash contents or update other metadata. Explicit mark and model-driven mark are equivalent once set.
  - **Rationale**: Single source of truth in metadata; both paths update the same flag; bad block enforcement is in the core layer (unconditional), not delegated to the failure model.

8. **Lifetime of `page_metadata_t.byte_deltas` and `get_byte_deltas` pointer**
  - **Question**: How long is the pointer returned in `get_page_info()` / `get_byte_deltas()` valid?
  - **Decision**: The pointer is **owned by the backend**. It is valid until the next call to any metadata backend operation or emulator API that may modify metadata (e.g. write, erase, snapshot load), or until `nand_emul_deinit()`. Callers SHALL NOT free it; they may copy or use the data within that scope only.
  - **Rationale**: Allows backend to use internal storage; avoids forcing a copy on every query.

9. **Snapshot checksum (CRC32) scope**
  - **Question**: CRC32 covers header only or entire file?
  - **Decision**: CRC32 SHALL cover **the snapshot header only** (the first 60 bytes, excluding the 4-byte checksum field). On load, the backend SHALL verify the checksum over the same range and SHALL return an error if it does not match.
  - **Rationale**: Header-only CRC is computable at header-write time without buffering the entire file. The header contains offsets and counts that describe the payload, so a corrupt payload (changed counts or offsets) is detectable via the header checksum.

10. **Snapshot format versioning**
  - **Question**: How to handle unknown or future snapshot versions?
  - **Decision**: The loader SHALL accept only version 1. For any other version field value, `load_snapshot()` SHALL return `ESP_ERR_NOT_SUPPORTED` and SHALL NOT modify backend state. Future versions may define backward compatibility in later specs.
  - **Rationale**: Prevents misinterpreting unknown formats; allows format evolution with explicit support.

11. **`corrupt_read_data` buffer contract**
  - **Question**: May the failure model corrupt only a subset of bytes? May it change length?
  - **Decision**: `data` is the read buffer; `len` is the number of bytes read (unchanged by the model). The model MAY flip bits (or otherwise corrupt) any subset of bytes in the range `[0, len)`. The model SHALL NOT write outside `[0, len)` and SHALL NOT assume it can change `len` or reallocate the buffer.
  - **Rationale**: Simulates bit errors in place; keeps API simple and safe.

12. **Page program history across erase cycles**
  - **Question**: Should `program_count` accumulate across all erase cycles, or only track the current cycle?
  - **Decision**: `page_metadata_t` SHALL have two counters:
    - `program_count` — number of programs **in the current erase cycle** (reset to 0 when the block is erased)
    - `program_count_total` — **lifetime cumulative** programs across all cycles (never reset)
  - Byte deltas (`byte_deltas`, `byte_delta_count`) track the current erase cycle only (freed and reset to NULL/0 on block erase).
  - `first_program_timestamp` and `last_program_timestamp` track the **current cycle**; lifetime-first and lifetime-last can be derived from block data if needed.
  - **Rationale**: `program_count` (per-cycle) is what makes delta encoding semantically correct — `write_count_delta` means "this byte was written N more times than the page in this cycle." `program_count_total` adds the historical view requested without breaking the delta math.

## Open Questions (For Future Consideration)

1. **ECC simulation**: Should we track ECC errors separately from bit flips?
  - Current: Bit flips injected by failure model are returned directly
  - Future: Could add ECC correction layer with `ecc_corrected_errors` counter
2. **Address range filtering**: Should byte tracking support address ranges (e.g., only OOB)?
  - Current: Delta encoding makes full-page tracking efficient enough
  - Future: Could add `track_address_range(start, end)` if needed for optimization
3. **Integration with existing `CONFIG_NAND_ENABLE_STATS`**: Merge or keep separate?
  - Decision: Keep separate; advanced tracking is much richer than simple counters

## References

- Existing code: `src/nand_linux_mmap_emul.c`, `include/nand_linux_mmap_emul.h`
- NAND flash reliability: "Understanding Flash: The Building Blocks" (Micron TN-29-42)
- Wear leveling algorithms: "Design Tradeoffs for SSD Performance" (USENIX 2008)
- Similar implementations: QEMU NAND emulation, FlashSim

## Changelog

- **2026-03-16**: Initial proposal
- **2026-03-16**: Updated with delta-encoded byte tracking, binary snapshot format, and wear simulation design

