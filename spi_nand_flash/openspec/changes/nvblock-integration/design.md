# Design: nvblock Integration

## Context

The `spi_nand_flash` component currently provides SPI NAND flash support with Dhara as the sole wear leveling/FTL implementation. The component is structured with a clear separation of concerns:

- **Public API** (`include/spi_nand_flash.h`): User-facing interface for flash operations
- **Core Logic** (`src/nand.c`): Chip detection, initialization, device management
- **Hardware Abstraction Layer** (`src/nand_impl.c` + `priv_include/nand_impl.h`): Low-level SPI NAND operations (read page, write page, erase block, check/mark bad blocks)
- **Wear Leveling Glue** (`src/dhara_glue.c`): Implements `spi_nand_ops` interface to bridge HAL to Dhara library

**Current State:**
- Single implementation: Dhara wear leveling (default, always compiled)
- Internal abstraction exists (`spi_nand_ops` interface in `priv_include/nand.h`)
- HAL is wear-leveling-agnostic (can support multiple FTL implementations)
- Component dependency: `espressif/dhara` via `idf_component.yml`

**Constraints:**
- **Zero API breakage**: Public API (`spi_nand_flash.h`) must remain 100% unchanged
- **Compile-time selection only**: No runtime switching between implementations
- **Backward compatibility**: Dhara must remain default, existing projects unaffected
- **ESP-IDF conventions**: Follow component structure, Kconfig patterns, naming conventions
- **Component manager limitation**: `idf_component.yml` evaluated before Kconfig, both dependencies must be present

**Stakeholders:**
- Existing ESP-IDF users (must not break)
- Resource-constrained applications (benefit from nvblock's smaller footprint)
- Applications requiring specific wear leveling characteristics (nvblock's configurable block grouping)

## Goals / Non-Goals

**Goals:**
- Provide nvblock as alternative wear leveling implementation with feature parity to Dhara
- Enable compile-time selection via Kconfig (`idf.py menuconfig`)
- Maintain identical public API behavior regardless of implementation choice
- Ensure comprehensive test coverage for nvblock path (functional, wear leveling, bad blocks, power-loss)
- Document trade-offs, configuration guidelines, and migration requirements

**Non-Goals:**
- Runtime switching between Dhara and nvblock (would add complexity, increase binary size)
- Automatic migration tool between implementations (Phase 1 - document manual process only)
- Performance optimization beyond parity (>90% of Dhara performance acceptable)
- Supporting both implementations simultaneously in single binary
- Changes to existing Dhara integration or public API

## Decisions

### 1. Parallel Implementation Structure (nvblock_glue.c)

**Decision:** Create `src/nvblock_glue.c` as parallel implementation to `dhara_glue.c`, both implementing `spi_nand_ops` interface.

**Rationale:**
- **Proven pattern**: `dhara_glue.c` already demonstrates successful abstraction via `spi_nand_ops`
- **Isolation**: Changes contained to new file, zero risk to existing Dhara code
- **Maintainability**: Clear 1:1 correspondence between implementations
- **Code reuse**: Both share HAL (`nand_impl.h` functions), duplicating only glue logic

**Alternatives Considered:**
- ❌ **Unified glue with runtime dispatch**: Increases binary size, adds complexity, violates compile-time selection goal
- ❌ **Modify dhara_glue.c with ifdefs**: Creates maintenance burden, harder to review, pollutes existing code

**Implementation Details:**
```c
// src/nvblock_glue.c structure (parallel to dhara_glue.c)

#include "nvblock/nvblock.h"
#include "nand.h"
#include "nand_impl.h"

typedef struct {
    spi_nand_flash_device_t *dev;  // Parent device handle
    nvb_t nvb;                      // nvblock instance
    uint8_t *meta_buf;              // Metadata buffer (runtime-sized)
    // ... state tracking
} nvblock_context_t;

// Callback implementations for nvblock (nvb_config.cfg.*)
static int nvb_read_cb(void *ctx, uint32_t group, uint32_t page, void *buf, uint32_t len);
static int nvb_write_cb(void *ctx, uint32_t group, uint32_t page, const void *buf, uint32_t len);
static int nvb_erase_cb(void *ctx, uint32_t group);
static int nvb_isbad_cb(void *ctx, uint32_t group);
static int nvb_markbad_cb(void *ctx, uint32_t group);
static int nvb_move_cb(void *ctx, uint32_t from_grp, uint32_t from_pg, 
                       uint32_t to_grp, uint32_t to_pg, uint32_t len);

// spi_nand_ops interface implementation
static esp_err_t nvblock_init(spi_nand_flash_device_t *handle);
static esp_err_t nvblock_read(spi_nand_flash_device_t *handle, uint32_t addr, 
                              void *dst, uint32_t size);
static esp_err_t nvblock_write(spi_nand_flash_device_t *handle, uint32_t addr, 
                               const void *src, uint32_t size);
static esp_err_t nvblock_erase(spi_nand_flash_device_t *handle, uint32_t addr, 
                               uint32_t size);
static esp_err_t nvblock_sync(spi_nand_flash_device_t *handle);
static esp_err_t nvblock_get_capacity(spi_nand_flash_device_t *handle, uint32_t *capacity);

const spi_nand_ops nvblock_ops = {
    .init = nvblock_init,
    .read = nvblock_read,
    .write = nvblock_write,
    .erase = nvblock_erase,
    .sync = nvblock_sync,
    .get_capacity = nvblock_get_capacity,
};
```

### 2. nvblock Configuration Strategy

**Decision:** Calculate nvblock configuration at runtime during initialization based on NAND chip parameters.

**Rationale:**
- **Chip diversity**: NAND parameters vary (page size: 2048-4096, pages/block: 64-128, block count: 512-4096)
- **No hardcoding**: Runtime calculation ensures correctness across all supported chips
- **Optimal spares**: Formula `spgcnt = max(1, total_groups/25)` gives ~4% spare (balanced wear leveling vs capacity)

**Configuration Mapping:**
```c
// In nvblock_init():
nvb_config_t cfg = {
    .bsize = handle->chip.page_size,              // e.g., 2048 bytes
    .bpg = 1 << handle->chip.log2_ppb,           // e.g., 64 pages/block
    .gcnt = handle->chip.num_blocks,             // e.g., 1024 blocks
    .spgcnt = MAX(1, handle->chip.num_blocks/25), // ~4% spare groups
    .cfg = {
        .read = nvb_read_cb,
        .write = nvb_write_cb,
        .erase = nvb_erase_cb,
        .isbad = nvb_isbad_cb,
        .markbad = nvb_markbad_cb,
        .move = nvb_move_cb,  // Optimized copy using nand_copy()
        .ctx = nvblock_ctx,
    }
};

// Metadata buffer sizing (runtime calculation)
size_t meta_size = 48 + (cfg.bpg * 2);  // Header + 2 bytes per page
nvblock_ctx->meta_buf = malloc(meta_size);
```

**Alternatives Considered:**
- ❌ **Kconfig compile-time config**: Inflexible, requires rebuild for different chips
- ❌ **Fixed 4KB buffer**: Wasteful for small page sizes, insufficient for large blocks

### 3. Conditional Compilation via Kconfig

**Decision:** Use Kconfig choice menu for exclusive selection, with CMakeLists.txt conditional source inclusion.

**Rationale:**
- **ESP-IDF standard**: Choice menu (`choice ... endchoice`) is idiomatic for mutually exclusive options
- **Build-time optimization**: Only one implementation compiled, minimal binary size impact
- **Clear defaults**: Dhara default preserves backward compatibility
- **Validation**: Kconfig enforces mutual exclusion, prevents misconfiguration

**Kconfig Structure:**
```kconfig
menu "SPI NAND Flash Wear Leveling"

    choice SPI_NAND_FLASH_WEAR_LEVELING
        prompt "Wear Leveling Implementation"
        default SPI_NAND_FLASH_WL_DHARA
        help
            Select the wear leveling algorithm for SPI NAND flash.
            
            - Dhara: Mature, well-tested, recommended for most applications
            - nvblock: Smaller footprint, configurable block grouping, simpler architecture
            
            Note: Changing this option requires chip erase (incompatible data formats).

        config SPI_NAND_FLASH_WL_DHARA
            bool "Dhara"
            help
                Use Dhara wear leveling (default, recommended).

        config SPI_NAND_FLASH_WL_NVBLOCK
            bool "nvblock"
            help
                Use nvblock wear leveling (smaller footprint, experimental).

    endchoice

endmenu
```

**CMakeLists.txt Conditional Compilation:**
```cmake
set(srcs "src/nand.c" "src/nand_impl.c")

# Conditionally add wear leveling glue
if(CONFIG_SPI_NAND_FLASH_WL_DHARA)
    list(APPEND srcs "src/dhara_glue.c")
elseif(CONFIG_SPI_NAND_FLASH_WL_NVBLOCK)
    list(APPEND srcs "src/nvblock_glue.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES driver esp_timer
                       PRIV_REQUIRES dhara nvblock)  # Both always present
```

**Alternatives Considered:**
- ❌ **Runtime selection via API parameter**: Violates API stability, increases complexity and binary size
- ❌ **Separate components (spi_nand_dhara, spi_nand_nvblock)**: Duplicates HAL code, complicates maintenance

### 4. Component Dependency Management

**Decision:** Include both `dhara` and `nvblock` in `idf_component.yml` as `PRIV_REQUIRES`, rely on linker dead code elimination.

**Rationale:**
- **Component manager limitation**: `idf_component.yml` evaluated before Kconfig, cannot conditionally include dependencies
- **Minimal overhead**: Unused library not linked if no symbols referenced (linker eliminates)
- **Precedent**: Common pattern in ESP-IDF (e.g., mbedtls with multiple crypto backends)
- **Simplicity**: Avoids complex manifest templating or multi-stage builds

**idf_component.yml:**
```yaml
dependencies:
  espressif/dhara: "^1.0.0"
  laczen/nvblock: "^1.0.0"  # (hypothetical, actual path TBD)
```

**Alternatives Considered:**
- ❌ **Conditional manifest generation**: Not supported by ESP-IDF component manager
- ❌ **Git submodules with Kconfig**: More complex, harder to version, breaks component registry workflow

### 5. Error Mapping Strategy

**Decision:** Map nvblock error codes to ESP-IDF `esp_err_t` codes with consistent semantics across both implementations.

**Rationale:**
- **User expectation**: Same API should return same errors for same conditions
- **Debuggability**: Consistent error codes simplify troubleshooting
- **Existing pattern**: `dhara_glue.c` already maps Dhara errors to `esp_err_t`

**Mapping Table:**
```c
static esp_err_t nvb_err_to_esp_err(int nvb_err) {
    switch (nvb_err) {
        case 0:              return ESP_OK;
        case -EINVAL:        return ESP_ERR_INVALID_ARG;
        case -ENOMEM:        return ESP_ERR_NO_MEM;
        case -EIO:           return ESP_FAIL;           // Hardware I/O error
        case -ENOSPC:        return ESP_ERR_NO_MEM;     // No space (out of blocks)
        case -EBADMSG:       return ESP_ERR_INVALID_CRC; // Data corruption
        default:             return ESP_FAIL;
    }
}
```

**Alternatives Considered:**
- ❌ **Direct nvblock error pass-through**: Breaks abstraction, exposes implementation details
- ❌ **Different error codes per implementation**: Confusing for users, complicates testing

### 6. Testing Strategy

**Decision:** Create parallel test suite mirroring existing Dhara tests, plus nvblock-specific wear leveling verification.

**Rationale:**
- **Parity validation**: Same test cases ensure feature parity
- **Regression prevention**: Existing Dhara tests continue to pass
- **Implementation-specific tests**: nvblock wear leveling characteristics may differ (acceptable within specs)
- **CI integration**: Both implementations tested on every commit

**Test Structure:**
```
test/
├── test_spi_nand_dhara.c       # Existing Dhara tests (unchanged)
├── test_spi_nand_nvblock.c     # New nvblock tests (parallel structure)
├── test_spi_nand_common.c      # Shared test utilities
└── test_wear_leveling.c        # Implementation-agnostic wear tests
```

**Test Categories:**
1. **Functional tests**: Basic read/write/erase (both implementations, identical assertions)
2. **Wear leveling tests**: Block distribution analysis (allow implementation-specific patterns)
3. **Bad block handling**: Graceful degradation, remapping verification
4. **Power-loss simulation**: Sync guarantees, data integrity after interruption
5. **Performance benchmarks**: Ensure nvblock within 90% of Dhara throughput

**Alternatives Considered:**
- ❌ **Single unified test suite with runtime selection**: Requires both in binary (violates compile-time selection)
- ❌ **No nvblock-specific tests**: Risks missing implementation-specific edge cases

## Risks / Trade-offs

### 1. Component Manager Dependency Overhead
**Risk:** Both dhara and nvblock dependencies downloaded even when only one used.

**Mitigation:**
- Both libraries are small (<100KB each)
- Linker dead code elimination prevents unused code in binary
- Document in README for user awareness
- Future: Request ESP-IDF component manager support for conditional dependencies

**Trade-off:** Minor build-time overhead (download/extract) vs significant implementation complexity.

### 2. Migration Between Implementations
**Risk:** Switching wear leveling requires chip erase (data loss). Users may not understand this.

**Mitigation:**
- Prominent Kconfig help text warning about data loss
- Document migration procedure in README with step-by-step instructions
- Consider runtime check on first boot (magic number mismatch) with clear error message
- Future (Phase 2): Migration tool to copy data between formats

**Trade-off:** User inconvenience vs implementation simplicity (no dual-format support).

### 3. nvblock Maturity vs Dhara
**Risk:** nvblock less mature, may have undiscovered bugs or edge cases.

**Mitigation:**
- Keep Dhara as default (conservative choice for production)
- Label nvblock as "experimental" in initial release (Kconfig help text)
- Comprehensive test suite including stress testing, power-loss simulation
- Gradual rollout: Internal testing → beta users → general availability
- Monitor issue reports, ready to patch or revert if critical bugs found

**Trade-off:** Innovation/choice vs stability risk.

### 4. Maintenance Burden
**Risk:** Two implementations = double maintenance effort for future features/fixes.

**Mitigation:**
- Strong abstraction (`spi_nand_ops` interface) minimizes shared code changes
- Parallel test structure ensures both implementations validated
- Clear ownership: nvblock tests must pass before merging changes
- Document internal architecture for future maintainers

**Trade-off:** Maintenance cost vs user flexibility/choice.

### 5. Performance Variability
**Risk:** nvblock performance may vary significantly from Dhara in unexpected ways.

**Mitigation:**
- Establish performance baseline: Dhara throughput on reference hardware
- Define acceptance criteria: nvblock >90% of Dhara (read/write/erase)
- Benchmark suite in CI, track performance regression
- Profile and optimize nvblock callbacks if below threshold
- Document known performance characteristics (e.g., nvblock GC patterns)

**Trade-off:** Performance predictability vs algorithm diversity.

### 6. Sparse Documentation for nvblock
**Risk:** nvblock library may lack comprehensive documentation, making integration harder.

**Mitigation:**
- Thorough code review of nvblock source (`lib/include/nvblock/nvblock.h`)
- Reference implementation analysis (if available)
- Document findings in `docs/specs/nvblock-integration.md`
- Engage with nvblock maintainer (Laczen) for clarifications
- Unit test assumptions to verify behavior

**Trade-off:** Integration effort vs long-term benefits.

## Migration Plan

### Phase 1: Implementation (This Change)
1. **Create nvblock component** (separate repository/PR):
   - Port nvblock to ESP-IDF component structure
   - Add CMakeLists.txt, idf_component.yml
   - Publish to component registry (or use git dependency)

2. **Implement nvblock_glue.c**:
   - Create parallel structure to dhara_glue.c
   - Implement all `spi_nand_ops` methods
   - Add nvblock callback implementations (read, write, erase, isbad, markbad, move)

3. **Update build system**:
   - Add Kconfig choice menu
   - Update CMakeLists.txt for conditional compilation
   - Add nvblock to idf_component.yml dependencies

4. **Testing**:
   - Port existing Dhara tests to nvblock equivalents
   - Add nvblock-specific wear leveling tests
   - Validate both implementations pass full suite

5. **Documentation**:
   - Update README with wear leveling selection guide
   - Add migration procedure (manual chip erase)
   - Document configuration trade-offs

### Phase 2: Validation & Rollout
1. **Internal testing** (1-2 weeks):
   - Run on reference hardware (multiple NAND chips)
   - Stress testing, long-running wear tests
   - Power-loss simulation

2. **Beta release**:
   - Mark nvblock as "experimental" in Kconfig
   - Solicit feedback from early adopters
   - Monitor issue reports

3. **General availability**:
   - Remove "experimental" label if stable
   - Promote in ESP-IDF release notes

### Rollback Strategy
- **Before merge**: Revert branch, abandon PR
- **After merge, before release**: Revert commit (clean rollback, no API impact)
- **After release**: 
  - Deprecate nvblock option via Kconfig warning
  - Fix critical bugs if feasible
  - Remove in next major version if unfixable

### Testing Rollout Plan
```bash
# Step 1: Compile with Dhara (existing default)
idf.py menuconfig  # Verify Dhara selected
idf.py build
idf.py flash test  # All existing tests pass

# Step 2: Compile with nvblock
idf.py menuconfig  # Select nvblock
idf.py fullclean build  # Fresh build
idf.py flash test  # nvblock tests pass

# Step 3: Migration test (manual)
idf.py erase-flash  # Simulate chip erase
idf.py flash  # Flash nvblock build
# Verify no data from Dhara era, clean slate
```

## Open Questions

### 1. nvblock Component Location
**Question:** Should nvblock be:
- (A) Separate GitHub repo + ESP component registry
- (B) Forked into espressif/nvblock with ESP-IDF adaptations
- (C) Git submodule in this repository

**Recommendation:** (A) Separate repo + registry (cleanest, follows dhara pattern)

**Decision Needed By:** Before implementation starts

---

### 2. Performance Acceptance Criteria
**Question:** What is acceptable performance degradation for nvblock vs Dhara?
- Current spec: >90% throughput acceptable
- Should we set stricter thresholds for specific operations (e.g., sequential write)?

**Recommendation:** Start with 90% overall, measure reality, adjust if needed

**Decision Needed By:** Before performance testing phase

---

### 3. Runtime Format Detection
**Question:** Should we add runtime detection to warn users about format mismatch (Dhara data with nvblock build)?

**Implementation:** Check magic number/header on mount, return ESP_ERR_INVALID_STATE with clear message

**Trade-off:** +100 bytes code, better UX vs simplicity

**Recommendation:** Implement if time permits (low priority, nice-to-have)

**Decision Needed By:** During implementation (can defer to Phase 2)

---

### 4. Default Spare Group Percentage
**Question:** Is 4% spare groups (`spgcnt = gcnt/25`) optimal for all use cases?

**Context:** Dhara uses fixed overhead; nvblock allows tuning
- More spares: Better wear leveling, less capacity
- Fewer spares: More capacity, potential wear hotspots

**Recommendation:** Start with 4%, add Kconfig option later if users request tuning

**Decision Needed By:** Before nvblock_init() implementation
