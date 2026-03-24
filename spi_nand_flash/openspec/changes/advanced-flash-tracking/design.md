# Advanced Flash Tracking - Detailed Implementation Design

## Table of Contents

1. [Overview](#overview)
2. [Data Structure Layouts](#data-structure-layouts)
3. [Sparse Hash Backend Implementation](#sparse-hash-backend-implementation)
4. [Binary Snapshot Format](#binary-snapshot-format)
5. [Integration with Core Operations](#integration-with-core-operations)
6. [Memory Management Strategy](#memory-management-strategy)
7. [Performance Considerations](#performance-considerations)
8. [Error Handling](#error-handling)

## Overview

This document provides the detailed implementation design for the advanced flash tracking system. It translates the high-level architecture from `proposal.md` into concrete data structures, algorithms, and implementation strategies.

### Design Goals

- **Memory efficiency**: <5% overhead for 32-128MB flash sizes
- **Performance**: <15% operation overhead with full tracking enabled
- **Correctness**: Accurate metadata even with complex write patterns
- **Simplicity**: Conservative tracking approach, backend handles optimization
- **Read stress visibility**: Per-page read counts (and optional read-disturb modeling) without storing per-byte read history

## Data Structure Layouts

### Core Metadata Types (Memory Layout)

```c
// Page-level metadata (no per-byte tracking; wear is block + page granularity)
typedef struct {
    uint32_t page_num;              // Page number (for iteration/hashing)
    uint32_t program_count;         // Programs in current erase cycle (reset to 0 on block erase)
    uint32_t program_count_total;   // Folded from program_count on each block erase (lifetime)
    uint32_t read_count;            // Reads in current erase cycle (reset to 0 on block erase)
    uint32_t read_count_total;      // Folded from read_count on each block erase (lifetime)
    uint64_t first_program_timestamp; // First program in current erase cycle
    uint64_t last_program_timestamp;  // Last program in current erase cycle
    uint32_t _reserved;             // Padding for stable alignment / future use
} page_metadata_t;

// Block-level metadata (44 bytes)
typedef struct {
    uint32_t block_num;                   // Block number (for iteration/hashing)
    uint32_t erase_count;                 // Number of block erases
    uint64_t first_erase_timestamp;
    uint64_t last_erase_timestamp;
    uint32_t total_page_programs;         // Sum of page programs in this block for current erase cycle (reset on erase)
    uint32_t total_page_programs_total;   // Lifetime cumulative sum across all erase cycles (never reset)
    uint8_t  is_bad_block;                // Boolean: simulated bad block
    uint8_t  _padding[7];                 // Align to 44 bytes
} block_metadata_t;
```

**Rationale**:
- Explicit padding ensures consistent struct sizes across platforms
- `page_num` and `block_num` stored in metadata for hash table iteration
- Pointers are platform-sized (8 bytes on 64-bit systems)
- **Read counters** mirror program counters: each successful read increments `read_count`; on block erase, `read_count_total += read_count` then `read_count = 0`. **Lifetime reads** for a page are `read_count_total + read_count` (same pattern as `program_count_total + program_count`). This gives failure models a monotonic wear signal for read-disturb and retention stress without dense per-byte read tracking

### Hash Table Structure

```c
// Hash table node (intrusive structure)
typedef struct hash_node {
    uint32_t key;              // block_num or page_num
    struct hash_node *next;    // Chaining for collision resolution
    uint8_t data[];           // Flexible array: block_metadata_t or page_metadata_t
} hash_node_t;

// Hash table
typedef struct {
    hash_node_t **buckets;     // Array of bucket pointers
    size_t capacity;           // Number of buckets (power of 2)
    size_t count;              // Number of stored entries
    float load_factor;         // Rehash threshold (default 0.75)
    size_t entry_size;         // sizeof(block_metadata_t) or sizeof(page_metadata_t)
} hash_table_t;
```

**Hash Function** (simple multiplicative hash):

```c
static inline size_t hash_u32(uint32_t key, size_t capacity) {
    // Knuth's multiplicative hash
    const uint32_t knuth = 2654435761U;
    return (key * knuth) & (capacity - 1);  // capacity is power of 2
}
```

### Sparse Hash Backend Handle

```c
typedef struct {
    hash_table_t *block_table;     // block_num → block_metadata_t
    hash_table_t *page_table;      // page_num → page_metadata_t
    
    // Configuration
    float load_factor;
    
    // Device geometry (cached for calculations)
    uint32_t total_blocks;
    uint32_t pages_per_block;
    uint32_t page_size;
    
    // Statistics cache (updated lazily)
    nand_wear_stats_t cached_stats;
    bool stats_dirty;              // Recompute on next get_stats call

    bool enable_histogram_query;   // If false, get_histograms op is NULL at registration time
} sparse_hash_backend_t;
```

### Histogram computation (`get_histograms`)

When `enable_histogram_query` is true, the sparse backend registers a non-NULL `get_histograms` that:

1. **Zeroes** all `count[]` arrays in `out->block_erase_count` and `out->page_lifetime_programs`.
2. **Validates** each sub-histogram: `n_bins` in `[2, NAND_WEAR_HIST_MAX_BINS]`, `bin_width > 0`; on violation return `ESP_ERR_INVALID_ARG`.
3. **Iterates** the block hash table: for each entry, increment the bin for `erase_count` using the uniform rule in `proposal.md` §1a.
4. **Iterates** the page hash table: for each page with `program_count_total + program_count > 0`, increment the bin for that lifetime program count.

Histograms are **not** folded into `cached_stats`; each query performs a full metadata sweep (same order of work as recomputing min/max if those were not cached). If future work caches min/max erase/program on each mutation, histograms can either stay on-demand or reuse a shared “full scan” helper.

### Write amplification accounting

`logical_write_bytes_recorded` lives in the **mmap emulator handle** (`advanced->logical_write_bytes_recorded`), not in the sparse backend: logical traffic is a test-harness concept. `nand_emul_record_logical_write()` increments this field; `nand_emul_get_wear_stats()` copies it into `nand_wear_stats_t.logical_write_bytes_recorded` and sets `write_amplification` from `total_bytes_written` (from backend `get_stats`) and that counter. The backend continues to own `total_bytes_written` / physical byte aggregates as today.

## Sparse Hash Backend Implementation

### Initialization

```c
esp_err_t sparse_hash_init(void **backend_handle, const void *config) {
    const sparse_hash_backend_config_t *cfg = config;
    
    sparse_hash_backend_t *backend = calloc(1, sizeof(*backend));
    if (!backend) return ESP_ERR_NO_MEM;
    
    // Initialize block table (estimate: 10% of blocks will be written)
    size_t initial_blocks = cfg->initial_capacity ? 
        cfg->initial_capacity : (total_blocks / 10);
    backend->block_table = hash_table_create(
        next_power_of_2(initial_blocks), 
        sizeof(block_metadata_t),
        cfg->load_factor ? cfg->load_factor : 0.75f
    );
    
    // Initialize page table (estimate: similar to blocks × pages_per_block)
    size_t initial_pages = initial_blocks * pages_per_block;
    backend->page_table = hash_table_create(
        next_power_of_2(initial_pages),
        sizeof(page_metadata_t),
        cfg->load_factor ? cfg->load_factor : 0.75f
    );
    
    backend->stats_dirty = true;
    
    *backend_handle = backend;
    return ESP_OK;
}
```

### Hash Table Operations

```c
// Insert or get existing entry
static hash_node_t* hash_table_get_or_insert(hash_table_t *table, uint32_t key) {
    size_t bucket_idx = hash_u32(key, table->capacity);
    hash_node_t *node = table->buckets[bucket_idx];
    
    // Search chain for existing entry
    while (node) {
        if (node->key == key) {
            return node;  // Found
        }
        node = node->next;
    }
    
    // Not found, create new node
    node = calloc(1, sizeof(hash_node_t) + table->entry_size);
    if (!node) return NULL;
    
    node->key = key;
    node->next = table->buckets[bucket_idx];
    table->buckets[bucket_idx] = node;
    table->count++;
    
    // Check if rehash needed
    if ((float)table->count / table->capacity > table->load_factor) {
        hash_table_rehash(table, table->capacity * 2);
    }
    
    return node;
}

// Rehash to larger table
static void hash_table_rehash(hash_table_t *table, size_t new_capacity) {
    hash_node_t **old_buckets = table->buckets;
    size_t old_capacity = table->capacity;
    
    // Allocate new bucket array
    table->buckets = calloc(new_capacity, sizeof(hash_node_t*));
    if (!table->buckets) {
        table->buckets = old_buckets;  // Allocation failed, keep old
        return;
    }
    
    table->capacity = new_capacity;
    table->count = 0;
    
    // Reinsert all nodes
    for (size_t i = 0; i < old_capacity; i++) {
        hash_node_t *node = old_buckets[i];
        while (node) {
            hash_node_t *next = node->next;
            size_t new_bucket = hash_u32(node->key, new_capacity);
            node->next = table->buckets[new_bucket];
            table->buckets[new_bucket] = node;
            table->count++;
            node = next;
        }
    }
    
    free(old_buckets);
}
```

### Block Operations

```c
esp_err_t sparse_hash_on_block_erase(void *backend_handle, 
                                      uint32_t block_num, 
                                      uint64_t timestamp) {
    sparse_hash_backend_t *backend = backend_handle;
    
    hash_node_t *node = hash_table_get_or_insert(backend->block_table, block_num);
    if (!node) return ESP_ERR_NO_MEM;
    
    block_metadata_t *meta = (block_metadata_t*)node->data;
    
    // Initialize on first erase
    if (meta->erase_count == 0) {
        meta->block_num = block_num;
        meta->first_erase_timestamp = timestamp;
    }
    
    meta->erase_count++;
    meta->last_erase_timestamp = timestamp;
    
    // Reset all page metadata in this block (pages invalidated by erase)
    uint32_t first_page = block_num * backend->pages_per_block;
    uint32_t last_page = first_page + backend->pages_per_block;
    
    for (uint32_t page_num = first_page; page_num < last_page; page_num++) {
        hash_node_t *page_node = hash_table_get(backend->page_table, page_num);
        if (page_node) {
            page_metadata_t *page_meta = (page_metadata_t*)page_node->data;
            
            // Accumulate into lifetime total before resetting
            page_meta->program_count_total += page_meta->program_count;
            page_meta->read_count_total += page_meta->read_count;
            
            // Reset per-cycle counters (preserve lifetime totals and page_num)
            page_meta->program_count = 0;
            page_meta->read_count = 0;
            page_meta->first_program_timestamp = 0;
            page_meta->last_program_timestamp = 0;
            
            // Note: keep the hash table entry; program_count_total preserves history
        }
    }
    
    // Accumulate block-level aggregate into lifetime total, then reset per-cycle
    meta->total_page_programs_total += meta->total_page_programs;
    meta->total_page_programs = 0;
    
    backend->stats_dirty = true;
    return ESP_OK;
}

esp_err_t sparse_hash_get_block_info(void *backend_handle, 
                                      uint32_t block_num,
                                      block_metadata_t *out) {
    sparse_hash_backend_t *backend = backend_handle;
    
    hash_node_t *node = hash_table_get(backend->block_table, block_num);
    if (!node) {
        // Block never erased, return zeros
        memset(out, 0, sizeof(*out));
        out->block_num = block_num;
        return ESP_OK;
    }
    
    memcpy(out, node->data, sizeof(*out));
    return ESP_OK;
}
```

### Page Operations

```c
esp_err_t sparse_hash_on_page_program(void *backend_handle,
                                       uint32_t page_num,
                                       uint64_t timestamp) {
    sparse_hash_backend_t *backend = backend_handle;
    
    hash_node_t *node = hash_table_get_or_insert(backend->page_table, page_num);
    if (!node) return ESP_ERR_NO_MEM;
    
    page_metadata_t *meta = (page_metadata_t*)node->data;
    
    // Initialize on first program in this erase cycle
    if (meta->program_count == 0) {
        meta->page_num = page_num;
        meta->first_program_timestamp = timestamp;
    }
    
    meta->program_count++;
    meta->last_program_timestamp = timestamp;
    
    // Update parent block's total page programs
    uint32_t block_num = page_num / backend->pages_per_block;
    hash_node_t *block_node = hash_table_get_or_insert(backend->block_table, block_num);
    if (block_node) {
        block_metadata_t *block_meta = (block_metadata_t*)block_node->data;
        block_meta->total_page_programs++;        // per-cycle counter
        block_meta->total_page_programs_total++;  // lifetime counter
        if (block_meta->erase_count == 0) {
            // Block written before first erase (init case)
            block_meta->block_num = block_num;
        }
    }
    
    backend->stats_dirty = true;
    return ESP_OK;
}
```

### Page read tracking

Each host read that successfully copies flash data into the caller buffer SHALL increment read counters for every page that overlaps the read range. This matches how write tracking iterates affected pages.

```c
esp_err_t sparse_hash_on_page_read(void *backend_handle,
                                    uint32_t page_num,
                                    uint64_t timestamp) {
    (void)timestamp;  // Reserved for future (e.g. last_read_timestamp)
    sparse_hash_backend_t *backend = backend_handle;

    hash_node_t *node = hash_table_get_or_insert(backend->page_table, page_num);
    if (!node) {
        return ESP_ERR_NO_MEM;
    }

    page_metadata_t *meta = (page_metadata_t *)node->data;
    meta->page_num = page_num;

    meta->read_count++;
    backend->stats_dirty = true;
    return ESP_OK;
}
```

**Semantics**:
- `read_count` counts successful reads that included this page in the operation range (one increment per page per `nand_emul_read`, not per byte).
- Reads to pages that have never been programmed still create or update a sparse page entry when read tracking is enabled, so “read-heavy cold” pages appear in metadata and snapshots.
- Failure models that need **neighbor** read-disturb (stress on adjacent pages when page *P* is read) can use `get_page_info(P ± 1)` inside `corrupt_read_data()`; this design does not duplicate neighbor aggregates in `page_metadata_t`.

### Read-disturb modeling (failure layer)

Physical read disturb is dominated by repeated reads of **other** pages in the same block (and word-line neighbors in real arrays). This metadata layer exposes **per-page read counts** so models can:

1. **Same-page simplification**: increase `corrupt_read_data` bit-flip probability with `read_count_total + read_count` for the page being read (cheap, conservative stress test).
2. **Neighbor-aware model**: on read of page `P`, fetch metadata for `P−1`, `P+1` (and optionally same-block pages) and scale corruption from their accumulated read counts.

The built-in probabilistic model SHOULD combine existing Weibull wear (erase/program) with an optional read-disturb term driven by these counters (see `proposal.md`).

## Binary Snapshot Format

### File Layout

```
┌─────────────────────────────────────────────────────────────┐
│                     HEADER (64 bytes)                        │
├─────────────────────────────────────────────────────────────┤
│              BLOCK METADATA SECTION (variable)               │
│  [block_0_data][block_5_data][block_17_data]...             │
├─────────────────────────────────────────────────────────────┤
│              PAGE METADATA SECTION (variable)                │
│  [page_0_data][page_1_data][page_128_data]...               │
└─────────────────────────────────────────────────────────────┘
```

### Snapshot Header

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;                // 0x4E414E44 ("NAND")
    uint8_t  version;              // 0x01
    uint8_t  flags;                // Bit 0: block tracking, Bit 1: page
    uint16_t reserved;
    uint64_t timestamp;
    uint32_t total_blocks;
    uint32_t pages_per_block;
    uint32_t page_size;
    uint32_t block_metadata_count;
    uint32_t page_metadata_count;
    uint64_t block_metadata_offset; // Offset in file
    uint64_t page_metadata_offset;
    uint32_t reserved2[2];         // Reserved for future format extensions
    uint32_t checksum;             // CRC32 of header (bytes 0..59, excluding this field)
} snapshot_header_t;

_Static_assert(sizeof(snapshot_header_t) == 64, "Header must be 64 bytes");
```

### Snapshot Save Algorithm

```c
esp_err_t sparse_hash_save_snapshot(void *backend_handle, 
                                     const char *filename,
                                     uint64_t timestamp) {
    sparse_hash_backend_t *backend = backend_handle;
    
    FILE *fp = fopen(filename, "wb");
    if (!fp) return ESP_ERR_NOT_FOUND;
    
    // 1. Write placeholder header (update later with offsets)
    snapshot_header_t header = {
        .magic = 0x4E414E44,
        .version = 1,
        .flags = 0x03,  // Block + page tracking
        .timestamp = timestamp,
        .total_blocks = backend->total_blocks,
        .pages_per_block = backend->pages_per_block,
        .page_size = backend->page_size,
        .block_metadata_count = backend->block_table->count,
        .page_metadata_count = backend->page_table->count,
        .reserved2 = {0, 0},
    };
    
    fseek(fp, sizeof(header), SEEK_SET);
    
    // 2. Write block metadata section
    header.block_metadata_offset = ftell(fp);
    
    for (size_t i = 0; i < backend->block_table->capacity; i++) {
        hash_node_t *node = backend->block_table->buckets[i];
        while (node) {
            block_metadata_t *meta = (block_metadata_t*)node->data;
            
            // Write serialized block metadata (40 bytes)
            fwrite(meta, sizeof(block_metadata_t), 1, fp);
            
            node = node->next;
        }
    }
    
    // 3. Write page metadata section
    header.page_metadata_offset = ftell(fp);
    
    for (size_t i = 0; i < backend->page_table->capacity; i++) {
        hash_node_t *node = backend->page_table->buckets[i];
        while (node) {
            page_metadata_t *meta = (page_metadata_t*)node->data;
            
            struct {
                uint32_t page_num;
                uint32_t program_count;
                uint32_t read_count;
                uint32_t read_count_total;
                uint64_t first_program_ts;
                uint64_t last_program_ts;
                uint32_t program_count_total;
                uint32_t _reserved_pad;
            } __attribute__((packed)) page_record = {
                .page_num = meta->page_num,
                .program_count = meta->program_count,
                .read_count = meta->read_count,
                .read_count_total = meta->read_count_total,
                .first_program_ts = meta->first_program_timestamp,
                .last_program_ts = meta->last_program_timestamp,
                .program_count_total = meta->program_count_total,
                ._reserved_pad = 0,
            };
            
            fwrite(&page_record, sizeof(page_record), 1, fp);
            
            node = node->next;
        }
    }
    
    // 4. Calculate CRC32 checksum of header (excluding checksum field)
    header.checksum = crc32(0, (uint8_t*)&header, 
                            offsetof(snapshot_header_t, checksum));
    
    // 5. Write final header
    fseek(fp, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, fp);
    
    fclose(fp);
    return ESP_OK;
}
```

### Snapshot Load Algorithm

```c
esp_err_t sparse_hash_load_snapshot(void *backend_handle, 
                                     const char *filename) {
    sparse_hash_backend_t *backend = backend_handle;
    
    FILE *fp = fopen(filename, "rb");
    if (!fp) return ESP_ERR_NOT_FOUND;
    
    // 1. Read and validate header
    snapshot_header_t header;
    fread(&header, sizeof(header), 1, fp);
    
    if (header.magic != 0x4E414E44) {
        fclose(fp);
        return ESP_ERR_INVALID_ARG;
    }
    
    uint32_t computed_crc = crc32(0, (uint8_t*)&header, 
                                   offsetof(snapshot_header_t, checksum));
    if (computed_crc != header.checksum) {
        fclose(fp);
        return ESP_ERR_INVALID_CRC;
    }
    
    // 2. Clear existing metadata
    hash_table_clear(backend->block_table);
    hash_table_clear(backend->page_table);
    
    // 3. Load block metadata
    fseek(fp, header.block_metadata_offset, SEEK_SET);
    
    for (uint32_t i = 0; i < header.block_metadata_count; i++) {
        block_metadata_t block_data;
        fread(&block_data, sizeof(block_metadata_t), 1, fp);
        
        hash_node_t *node = hash_table_get_or_insert(
            backend->block_table, block_data.block_num);
        memcpy(node->data, &block_data, sizeof(block_data));
    }
    
    // 4. Load page metadata
    fseek(fp, header.page_metadata_offset, SEEK_SET);
    
    for (uint32_t i = 0; i < header.page_metadata_count; i++) {
        struct {
            uint32_t page_num;
            uint32_t program_count;
            uint32_t read_count;
            uint32_t read_count_total;
            uint64_t first_program_ts;
            uint64_t last_program_ts;
            uint32_t program_count_total;
            uint32_t _reserved_pad;
        } __attribute__((packed)) page_record;
        
        fread(&page_record, sizeof(page_record), 1, fp);
        
        hash_node_t *node = hash_table_get_or_insert(
            backend->page_table, page_record.page_num);
        page_metadata_t *meta = (page_metadata_t*)node->data;
        
        meta->page_num = page_record.page_num;
        meta->program_count = page_record.program_count;
        meta->read_count = page_record.read_count;
        meta->read_count_total = page_record.read_count_total;
        meta->program_count_total = page_record.program_count_total;
        meta->first_program_timestamp = page_record.first_program_ts;
        meta->last_program_timestamp = page_record.last_program_ts;
    }
    
    fclose(fp);
    
    backend->stats_dirty = true;
    return ESP_OK;
}
```

## Integration with Core Operations

### Timestamp Generation

```c
// Default timestamp: monotonic counter
static uint64_t default_timestamp(void) {
    static uint64_t counter = 0;
    return counter++;
}

// In nand_emul_advanced_init():
if (cfg->get_timestamp) {
    emul->advanced->get_timestamp = cfg->get_timestamp;
} else {
    emul->advanced->get_timestamp = default_timestamp;
}
```

### Write Handler Integration

```c
esp_err_t nand_emul_write(spi_nand_flash_device_t *handle, 
                          size_t offset, 
                          const void *data, 
                          size_t len) {
    nand_mmap_emul_handle_t *emul = handle->emul_handle;
    
    // Validation
    if (!emul->mem_file_buf) return ESP_ERR_INVALID_STATE;
    if (offset + len > emul->file_mmap_ctrl.flash_file_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Advanced: Failure injection (before write)
    if (emul->advanced && emul->advanced->failure_ops) {
        uint64_t timestamp = emul->advanced->get_timestamp();  // captured once for this operation
        
        // Build context for failure decision
        nand_operation_context_t ctx = {
            .block_num = offset / handle->chip.block_size,
            .page_num = offset / handle->chip.page_size,
            .byte_offset = offset % handle->chip.page_size,
            .operation_size = len,
            .timestamp = timestamp,
            .total_blocks = emul->advanced->total_blocks,
            .pages_per_block = emul->advanced->pages_per_block,
            .page_size = emul->advanced->page_size
        };
        
        // Query metadata for context (optional, may be NULL)
        if (emul->advanced->metadata_ops) {
            block_metadata_t block_meta;
            page_metadata_t page_meta;
            
            emul->advanced->metadata_ops->get_block_info(
                emul->advanced->metadata_handle, ctx.block_num, &block_meta);
            emul->advanced->metadata_ops->get_page_info(
                emul->advanced->metadata_handle, ctx.page_num, &page_meta);
            
            ctx.block_meta = &block_meta;
            ctx.page_meta = &page_meta;
        }
        
        if (emul->advanced->failure_ops->should_fail_write(
                emul->advanced->failure_handle, &ctx)) {
            ESP_LOGW(TAG, "Simulated write failure at offset 0x%zx", offset);
            return ESP_ERR_FLASH_OP_FAIL;
        }
    }
    
    // Perform write
    memcpy(emul->mem_file_buf + offset, data, len);
    
    // Advanced: Metadata tracking (after successful write)
    // Reuse timestamp from ctx if failure ops were configured; otherwise capture now
    if (emul->advanced && emul->advanced->metadata_ops) {
        uint64_t timestamp = emul->advanced->failure_ops 
                             ? ctx.timestamp   // reuse — same logical operation
                             : emul->advanced->get_timestamp();
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

### Read Handler Integration (metadata + data corruption)

**Ordering contract** (align with `specs/integration/spec.md`): `should_fail_read()` runs **before** copying flash data to the caller buffer; only if it returns false does the core perform `memcpy`. After a successful read, the core updates read metadata (if enabled), rebuilds `nand_operation_context_t` with fresh `page_meta` / `block_meta`, then calls `corrupt_read_data()` so read-disturb models can use counts that include the current access.

```c
esp_err_t nand_emul_read(spi_nand_flash_device_t *handle,
                         size_t offset,
                         void *data,
                         size_t len) {
    nand_mmap_emul_handle_t *emul = handle->emul_handle;
    
    // Validation
    if (!emul->mem_file_buf) return ESP_ERR_INVALID_STATE;
    if (offset + len > emul->file_mmap_ctrl.flash_file_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t timestamp = (emul->advanced && emul->advanced->get_timestamp)
                             ? emul->advanced->get_timestamp()
                             : 0;

    /* Failure model: fail before touching caller buffer (integration spec) */
    if (emul->advanced && emul->advanced->failure_ops) {
        nand_operation_context_t ctx = { /* block_num, page_num, offsets, timestamp, geometry */ };
        /* Optionally fill ctx.block_meta / ctx.page_meta via get_*_info (pre-read counts) */
        if (emul->advanced->failure_ops->should_fail_read(
                emul->advanced->failure_handle, &ctx)) {
            ESP_LOGW(TAG, "Simulated read failure at offset 0x%zx", offset);
            return ESP_ERR_FLASH_OP_FAIL;
        }
    }

    memcpy(data, emul->mem_file_buf + offset, len);

    /* Metadata: one increment per overlapping page (after successful memcpy) */
    if (emul->advanced && emul->advanced->metadata_ops && emul->advanced->track_page_level) {
        uint32_t page_size = handle->chip.page_size;
        uint32_t first_page = offset / page_size;
        uint32_t last_page = (offset + len - 1) / page_size;
        for (uint32_t page_num = first_page; page_num <= last_page; page_num++) {
            if (emul->advanced->metadata_ops->on_page_read) {
                emul->advanced->metadata_ops->on_page_read(
                    emul->advanced->metadata_handle, page_num, timestamp);
            }
        }
    }

    if (emul->advanced && emul->advanced->failure_ops) {
        nand_operation_context_t ctx = { /* same geometry; refresh metadata */ };
        if (emul->advanced->metadata_ops) {
            emul->advanced->metadata_ops->get_block_info(
                emul->advanced->metadata_handle, ctx.block_num, &block_meta);
            emul->advanced->metadata_ops->get_page_info(
                emul->advanced->metadata_handle, ctx.page_num, &page_meta);
            ctx.block_meta = &block_meta;
            ctx.page_meta = &page_meta;
        }
        emul->advanced->failure_ops->corrupt_read_data(
            emul->advanced->failure_handle, &ctx, data, len);
    }

#ifdef CONFIG_NAND_ENABLE_STATS
    emul->stats.read_ops++;
    emul->stats.read_bytes += len;
#endif

    return ESP_OK;
}
```

**Note**: If `on_page_read` is NULL (minimal backend), read counting is skipped; failure models that depend on read counts SHOULD use a backend that implements `on_page_read`.

## Memory Management Strategy

### Memory Pools (Future Optimization)

For high-performance scenarios, consider using memory pools:

```c
// Pool for page metadata (fixed size sizeof(page_metadata_t)); illustrative only
typedef struct {
    page_metadata_t **blocks;   // ring of recycled pointers, or NULL to always calloc
    size_t n_free;
} page_metadata_pool_t;

page_metadata_t* pool_alloc_page_metadata(page_metadata_pool_t *pool) {
    (void)pool;
    return calloc(1, sizeof(page_metadata_t));
}

void pool_free_page_metadata(page_metadata_pool_t *pool, page_metadata_t *meta) {
    (void)pool;
    free(meta);
}
```

### Memory Budget Tracking

```c
typedef struct {
    size_t block_metadata_bytes;
    size_t page_metadata_bytes;
    size_t hash_table_overhead_bytes;
    size_t total_bytes;
} memory_usage_t;

esp_err_t sparse_hash_get_memory_usage(void *backend_handle, 
                                        memory_usage_t *out) {
    sparse_hash_backend_t *backend = backend_handle;
    
    out->block_metadata_bytes = backend->block_table->count * 
        (sizeof(hash_node_t) + sizeof(block_metadata_t));
    
    out->page_metadata_bytes = backend->page_table->count * 
        (sizeof(hash_node_t) + sizeof(page_metadata_t));
    
    out->hash_table_overhead_bytes = 
        (backend->block_table->capacity + backend->page_table->capacity) * 
        sizeof(hash_node_t*);
    
    out->total_bytes = out->block_metadata_bytes + 
                       out->page_metadata_bytes + 
                       out->hash_table_overhead_bytes;
    
    return ESP_OK;
}
```

## Performance Considerations

### Operation Overhead Analysis

**Baseline (no tracking)**:
- Read: memcpy (~10 GB/s)
- Write: memcpy (~10 GB/s)
- Erase: memset (~20 GB/s)

**With full tracking**:
- Read: +2-3 μs (failure check + context build)
- Write: +3-8 μs (page update + hash lookup)
- Erase: +50-300 μs (iterate pages in block, fold counters)

**Overhead**: ~5-10% for write-heavy workloads

### Hash Table Performance

- **Lookup**: O(1) average, O(n) worst case (collision chain)
- **Insert**: O(1) amortized (with rehashing)
- **Iteration**: O(capacity + count) - must visit all buckets

**Optimization**: Keep load factor at 0.75 to minimize collisions

## Error Handling

### Failure Recovery Strategies

1. **Metadata allocation failure**:
   - Log warning
   - Continue without tracking this operation
   - Don't fail the flash operation itself

2. **Snapshot corruption**:
   - Detect via CRC32 mismatch
   - Return `ESP_ERR_INVALID_CRC`
   - Leave existing metadata intact (don't clear)

3. **Hash table rehash failure**:
   - Keep old table
   - Log warning
   - Accept higher load factor temporarily

### Error Codes

```c
#define ESP_ERR_INVALID_CRC      (ESP_ERR_FLASH_BASE + 0x50)
#define ESP_ERR_SNAPSHOT_CORRUPT (ESP_ERR_FLASH_BASE + 0x51)
#define ESP_ERR_METADATA_FULL    (ESP_ERR_FLASH_BASE + 0x52)
```

### Logging Strategy

```c
#define TAG "nand_advanced"

// Info: Normal operations
ESP_LOGI(TAG, "Advanced init: %u blocks, %u pages",
         total_blocks, total_pages);

// Warning: Non-fatal issues
ESP_LOGW(TAG, "Metadata allocation failed for page %u", page_num);

// Error: Fatal issues
ESP_LOGE(TAG, "Snapshot corrupted: CRC mismatch (expected=0x%08x, got=0x%08x)",
         expected_crc, actual_crc);
```

---

## Summary

This design provides:

1. **Concrete data structures** with explicit sizes and padding
2. **Hash table implementation** with collision handling and rehashing
3. **Binary snapshot format** with CRC32 integrity checking
4. **Integration hooks** in read/write/erase handlers
5. **Memory management** with usage tracking
6. **Performance analysis** with overhead estimates
7. **Error handling** with graceful degradation

Next step: Create `tasks.md` to break down implementation into concrete work items.
