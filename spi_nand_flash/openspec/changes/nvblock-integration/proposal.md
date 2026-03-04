# Proposal: nvblock Integration

## Why

The spi_nand_flash component currently only supports Dhara for wear leveling. Adding nvblock as an alternative provides users with choice based on their specific needs: nvblock offers smaller footprint, configurable block sizes, and simpler architecture suitable for resource-constrained systems. This gives ESP-IDF users flexibility in selecting the wear leveling algorithm that best fits their application requirements.

## What Changes

- Add nvblock library as an alternative wear leveling implementation (compile-time selectable)
- Create `nvblock_glue.c` implementing the existing `spi_nand_ops` interface for nvblock
- Add Kconfig menu option to choose between Dhara (default) and nvblock wear leveling
- Add nvblock component dependency (conditional, similar to dhara)
- Update CMakeLists.txt for conditional compilation based on Kconfig selection
- Comprehensive testing for nvblock path (host tests, hardware tests, wear leveling verification)
- Documentation updates: README, architecture diagrams, migration guide

**Key Constraint**: Maintain 100% backward compatibility - no changes to public API (`spi_nand_flash.h`)

## Capabilities

### New Capabilities

- `nvblock-wear-leveling`: Implement nvblock as alternative FTL/wear leveling layer with full feature parity to Dhara (read, write, trim, sync, bad block handling, capacity queries)
- `wear-leveling-selection`: Allow compile-time selection of wear leveling implementation (Dhara vs nvblock) via Kconfig
- `nvblock-testing`: Comprehensive test suite for nvblock including wear leveling verification, power-loss simulation, and bad block handling

### Modified Capabilities

<!-- No existing capabilities being modified - this is purely additive -->

## Impact

**Affected Code:**
- `CMakeLists.txt`: Add conditional compilation for dhara_glue.c vs nvblock_glue.c
- `Kconfig`: Add wear leveling selection menu
- `idf_component.yml`: Add nvblock component dependency
- `src/`: New file `nvblock_glue.c` (parallel to `dhara_glue.c`)
- `README.md`: Add documentation on wear leveling options

**Affected APIs:**
- None (public API in `include/spi_nand_flash.h` remains unchanged)
- Internal `spi_nand_ops` interface used by both implementations (no changes to structure)

**Dependencies:**
- New dependency: `nvblock` ESP-IDF component (to be created separately)
- Existing dependency: `dhara` (remains unchanged, conditionally used)

**Affected Systems:**
- Build system: Conditional compilation based on Kconfig
- Component manager: Conditional dependency resolution
- Test infrastructure: New test cases for nvblock path
- Examples: Can optionally demonstrate nvblock usage

**User Impact:**
- **Existing users**: Zero impact (Dhara remains default, no code changes needed)
- **New users**: Can choose nvblock via menuconfig
- **Migration**: Switching wear leveling requires chip erase (incompatible data formats)
