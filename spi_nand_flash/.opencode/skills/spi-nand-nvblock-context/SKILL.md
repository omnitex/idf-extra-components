---
name: spi-nand-nvblock-context
description: Context for spi_nand_flash nvblock integration project - OpenSpec workflows, project structure, and implementation approach
license: MIT
metadata:
  author: OpenCode AI
  version: "1.1"
  created: 2026-03-04
  updated: 2026-03-04
  progress: "21/113 tasks (19%)"
  status: "active - implementing Section 5 HAL callbacks"
---

# SPI NAND Flash + nvblock Integration - Project Context

Complete context for working on the spi_nand_flash component nvblock integration using OpenSpec workflows.

## What is OpenSpec?

**OpenSpec** is an AI-native system for spec-driven development that uses a structured workflow to go from idea → specification → implementation.

**Core Concept**: Changes progress through artifact stages:
- `proposal.md` - Why/what/capabilities
- `specs/<capability>/spec.md` - Detailed requirements per capability
- `design.md` - Technical decisions and architecture
- `tasks.md` - Implementation checklist

**Current Schema**: `spec-driven` (proposal → specs → design → tasks)

## OpenSpec CLI Commands

```bash
# List all changes
openspec list

# Show change details
openspec show <change-name>

# Check artifact status
openspec status --change "<change-name>"

# Get instructions for creating artifacts
openspec instructions <artifact-id> --change "<change-name>" --json

# Get instructions for implementing tasks
openspec instructions apply --change "<change-name>" --json

# Archive completed change
openspec archive <change-name>
```

## Custom Slash Commands (/.opencode/command/)

Use these shortcuts for OpenSpec workflows:

### `/opsx-new [change-name]`
Start a new change - creates the directory structure and shows first artifact template

### `/opsx-continue [change-name]`
Create the next artifact in sequence (proposal → specs → design → tasks)

### `/opsx-ff [change-name]` 
Fast-forward - generate ALL artifacts at once without stepping through each

### `/opsx-apply [change-name]`
**Most important for implementation!** Implements tasks from tasks.md:
- Reads context files (proposal, specs, design, tasks)
- Works through pending tasks
- Marks tasks complete as you go
- Pauses on blockers or unclear requirements

### `/opsx-verify [change-name]`
Verify implementation matches specs before archiving:
- Checks task completion
- Validates spec coverage
- Checks requirement implementation

### `/opsx-archive [change-name]`
Archive completed change and update main specs

### `/opsx-explore`
Enter explore mode - thinking partner for ideas and requirements clarification

### `/opsx-sync [change-name]`
Sync delta specs from a change to main specs without archiving

## Project Structure

```
spi_nand_flash/
├── openspec/
│   └── changes/
│       └── nvblock-integration/          # Current active change
│           ├── .openspec.yaml           # Schema: spec-driven
│           ├── README.md                # Brief description
│           ├── proposal.md              # Why/what/capabilities
│           ├── comprehensive-spec.md    # Detailed 754-line spec (preserved)
│           ├── design.md                # Technical decisions
│           ├── tasks.md                 # 113 tasks (21 complete = 19%)
│           ├── REVIEW.md                # Verification checklist
│           └── specs/                   # Granular capability specs
│               ├── nvblock-testing/
│               ├── nvblock-wear-leveling/
│               └── wear-leveling-selection/
├── src/
│   ├── nand.c                          # Core device management
│   ├── nand_impl.c                     # HAL - low-level SPI NAND ops
│   ├── dhara_glue.c                    # Existing Dhara integration
│   └── nvblock_glue.c                  # nvblock integration (200+ lines, partial)
├── include/
│   └── spi_nand_flash.h                # Public API (MUST NOT CHANGE)
├── priv_include/
│   ├── nand.h                          # Internal: spi_nand_ops interface
│   └── nand_impl.h                     # HAL function declarations
├── Kconfig                             # Wear leveling selection menu ✅
├── CMakeLists.txt                      # Conditional compilation ✅
└── .opencode/
    ├── command/                        # Slash commands (opsx-*)
    └── skills/                         # Project-specific skills

# nvblock component (sibling directory)
nvblock/
├── nvblock/                            # Git submodule (Laczen/nvblock)
│   └── lib/
│       ├── include/nvblock/nvblock.h   # API (874 lines)
│       └── src/nvblock.c               # Implementation
├── CMakeLists.txt                      # ESP-IDF wrapper
├── idf_component.yml                   # Component manifest
└── README.md
```

## nvblock Integration - Current State

### What We're Building

Add **nvblock** as an alternative wear leveling implementation alongside **Dhara**:
- **Compile-time selection** via Kconfig (menuconfig) ✅ DONE
- **100% backward compatible** - no public API changes ✅ VERIFIED
- **Parallel implementation** - `nvblock_glue.c` alongside `dhara_glue.c` ✅ IN PROGRESS
- Both implement the same `spi_nand_ops` interface 🚧 PARTIAL

### Why nvblock?

- **Smaller footprint** - 0x2a6b0 bytes vs Dhara's 0x2b9c0 (confirmed!)
- **Configurable block sizes** - Min 64 bytes vs Dhara's page-based approach
- **Simpler design** - Single-file library (874 lines)
- **Flexible configuration** - Better for resource-constrained systems
- **Apache 2.0 license** - Same as ESP-IDF

### Architecture Overview

```
┌─────────────────────────────────────────┐
│  Public API (spi_nand_flash.h)         │  ← NO CHANGES
└─────────────────────────────────────────┘
                   │
┌─────────────────────────────────────────┐
│  Core (nand.c)                          │
│  - Device detection & init              │
│  - Registers spi_nand_ops based on      │
│    Kconfig (dhara_ops vs nvblock_ops)   │
└─────────────────────────────────────────┘
                   │
        ┌──────────┴──────────┐
        ▼                     ▼
┌──────────────┐      ┌──────────────┐
│ dhara_glue.c │      │nvblock_glue.c│  ← NEW
│ (existing)   │      │ (skeleton)   │
└──────────────┘      └──────────────┘
        │                     │
        └──────────┬──────────┘
                   ▼
        ┌─────────────────────┐
        │  HAL (nand_impl.c)  │
        │  - nand_read_page   │
        │  - nand_write_page  │
        │  - nand_erase_block │
        │  - nand_is_bad      │
        │  - nand_mark_bad    │
        │  - nand_copy        │
        └─────────────────────┘
                   │
                   ▼
        ┌─────────────────────┐
        │  SPI Driver         │
        │  (ESP-IDF)          │
        └─────────────────────┘
```

### Key Design Decisions

**1. Parallel Implementation (nvblock_glue.c)**
- Creates `src/nvblock_glue.c` parallel to `dhara_glue.c`
- Both implement `spi_nand_ops` interface (defined in `priv_include/nand.h`)
- Isolation: changes contained to new file, zero risk to existing code

**2. Runtime Configuration Calculation**
- nvblock config calculated at init time from chip parameters
- `bsize = chip.page_size` (e.g., 2048 bytes)
- `bpg = 1 << chip.log2_ppb` (e.g., 64 pages/block)
- `gcnt = chip.num_blocks` (e.g., 1024 blocks)
- `spgcnt = (gcnt * gc_factor) / 100` with minimum of 2 spare groups
- Metadata buffer: `NVB_META_DMP_START (48) + (bpg * NVB_META_ADDRESS_SIZE)` bytes
- **IMPLEMENTED** in nvblock_init() (tasks 4.3-4.4)

**3. Kconfig Choice Menu**
```
choice SPI_NAND_FLASH_WL_IMPL
    ├── SPI_NAND_FLASH_WL_DHARA (default)
    └── SPI_NAND_FLASH_WL_NVBLOCK (experimental)
```

**4. CMakeLists.txt Conditional Compilation**
```cmake
if(CONFIG_SPI_NAND_FLASH_WL_DHARA)
    list(APPEND srcs "src/dhara_glue.c")
elseif(CONFIG_SPI_NAND_FLASH_WL_NVBLOCK)
    list(APPEND srcs "src/nvblock_glue.c")
endif()
```

### spi_nand_ops Interface

Both glue layers must implement (from `priv_include/nand.h`):

```c
typedef struct {
    esp_err_t (*init)(spi_nand_flash_device_t *handle);
    esp_err_t (*deinit)(spi_nand_flash_device_t *handle);
    esp_err_t (*read)(spi_nand_flash_device_t *handle, uint8_t *buffer, uint32_t sector_id);
    esp_err_t (*write)(spi_nand_flash_device_t *handle, const uint8_t *buffer, uint32_t sector_id);
    esp_err_t (*erase_chip)(spi_nand_flash_device_t *handle);
    esp_err_t (*erase_block)(spi_nand_flash_device_t *handle, uint32_t block);
    esp_err_t (*trim)(spi_nand_flash_device_t *handle, uint32_t sector_id);
    esp_err_t (*sync)(spi_nand_flash_device_t *handle);
    esp_err_t (*copy_sector)(spi_nand_flash_device_t *handle, uint32_t src_sec, uint32_t dst_sec);
    esp_err_t (*get_capacity)(spi_nand_flash_device_t *handle, uint32_t *number_of_sectors);
} spi_nand_ops;
```

### nvblock HAL Callbacks

nvblock requires these callbacks (bridge to HAL):

```c
// nvblock callback types (from nvblock API)
int nvb_read_cb(void *ctx, uint32_t group, uint32_t page, void *buf, uint32_t len);
int nvb_write_cb(void *ctx, uint32_t group, uint32_t page, const void *buf, uint32_t len);
int nvb_erase_cb(void *ctx, uint32_t group);
int nvb_isbad_cb(void *ctx, uint32_t group);
int nvb_markbad_cb(void *ctx, uint32_t group);
int nvb_move_cb(void *ctx, uint32_t from_grp, uint32_t from_pg, 
                uint32_t to_grp, uint32_t to_pg, uint32_t len);
```

**Mapping**: nvblock "group" = NAND block, nvblock "page" = page within block

### Implementation Progress

**Completed: 21/113 tasks (19%)**

✅ **Section 1: nvblock Component Setup (5/5)** - COMPLETE
- 1.1: Created nvblock ESP-IDF component directory structure
- 1.2: Added nvblock as git submodule from https://github.com/Laczen/nvblock
- 1.3: Verified nvblock component builds (libnvblock.a)
- 1.4: Added dependencies to spi_nand_flash and test_app idf_component.yml
- 1.5: Verified both dhara and nvblock dependencies resolve (4 deps processed)

✅ **Section 2: Kconfig Configuration (5/5)** - COMPLETE
- 2.1-2.3: Created wear leveling choice menu with help text
- 2.4: Verified menuconfig displays correctly
- 2.5: Tested Kconfig mutual exclusion

✅ **Section 3: Build System Integration (5/5)** - COMPLETE
- 3.1: Updated CMakeLists.txt for conditional compilation
- 3.3: Verified Dhara build (100% backward compatible)
- 3.4: Verified nvblock build configuration
- 3.5: Confirmed only selected implementation compiled
- 3.2: Both components in REQUIRES list

✅ **Section 4: nvblock Glue Layer - Data Structures (5/5)** - COMPLETE
- 4.1: Created src/nvblock_glue.c skeleton (now 200+ lines)
- 4.2: Defined nvblock_context_t with nvb_info and nvb_config
- 4.3: Implemented runtime configuration calculation (bsize, bpg, gcnt, spgcnt)
- 4.4: Allocated metadata buffer with runtime sizing
- 4.5: Context cleanup in nvblock_deinit()

✅ **Section 7: Core Integration (1/4)** - PARTIAL
- 7.1: Implemented nand_register_dev() and nand_unregister_dev()

🚧 **Next Steps**:
- **Section 5: nvblock HAL Callbacks (0/7)** - Implement 6 callbacks to bridge nvblock to nand_impl.h
  - 5.1: nvb_read_cb
  - 5.2: nvb_write_cb (prog)
  - 5.3: nvb_erase_cb
  - 5.4: nvb_isbad_cb
  - 5.5: nvb_markbad_cb
  - 5.6: nvb_move_cb
  - 5.7: Wire up callbacks to nvblock config
- **Section 6: spi_nand_ops Interface (0/7)** - Implement nvblock API integration in each ops
- **Sections 8-20: Testing and Documentation** - Comprehensive testing and docs

### Build Status

Both configurations build successfully:
- **Dhara (default)**: ✅ 0x2b9c0 bytes - 100% backward compatible
- **nvblock**: ✅ 0x2a6b0 bytes - slightly smaller footprint
- Expected warnings for unused callbacks (addressed in Section 5)

### Recent Commits

1. `e2b57af` - feat(nvblock): add nvblock ESP-IDF component wrapper (tasks 1.1-1.5)
2. `89b568a` - feat(spi_nand_flash): implement nand_register_dev() for nvblock (task 7.1)
3. `f605ebb` - feat(spi_nand_flash): implement nvblock data structures and initialization (tasks 4.3-4.5)

### Critical Constraints

⚠️ **MUST NOT**:
- Change public API in `include/spi_nand_flash.h`
- Break backward compatibility with existing Dhara users
- Support runtime switching (compile-time only)
- Modify `dhara_glue.c` or existing Dhara integration

✅ **MUST**:
- Keep Dhara as default (backward compatible)
- Maintain identical public API behavior
- Support all existing features (bad blocks, ECC, trim, sync)
- Comprehensive testing before marking experimental → stable

## Working with OpenSpec

### Starting a Session

1. **Check current changes**:
   ```bash
   openspec list
   ```

2. **Load change context** (for nvblock-integration):
   ```bash
   openspec show nvblock-integration
   openspec status --change "nvblock-integration"
   ```

3. **Start implementing** (recommended approach):
   ```
   /opsx-apply nvblock-integration
   ```
   This automatically:
   - Reads all context files (proposal, specs, design, tasks)
   - Shows current progress
   - Guides through pending tasks
   - Marks tasks complete as you go

### When to Use Each Command

**Creating new changes**:
- `/opsx-new <name>` - Step-by-step artifact creation
- `/opsx-ff <name>` - Generate all artifacts at once

**Working on existing changes**:
- `/opsx-continue` - Create next artifact (if incomplete)
- `/opsx-apply` - **Implement tasks** (main work loop)
- `/opsx-explore` - Brainstorm/clarify before implementation

**Finishing up**:
- `/opsx-verify` - Check completeness/correctness before archiving
- `/opsx-archive` - Finalize and archive completed change

### Task Workflow

Tasks are in `openspec/changes/<name>/tasks.md`:

```markdown
## Section Name

- [ ] Task 1 description
- [x] Task 2 description (completed)
- [ ] Task 3 description
```

**When implementing**:
1. Work through tasks sequentially
2. Change `- [ ]` to `- [x]` when complete
3. Commit changes with task numbers in commit message
4. If blocked, document blocker and move to tasks that aren't blocked

## Useful References

**ESP-IDF Component Structure**:
- Public headers: `include/`
- Private headers: `priv_include/`
- Implementation: `src/`
- Kconfig options: `Kconfig`
- Build config: `CMakeLists.txt`
- Dependencies: `idf_component.yml`

**HAL Functions** (available in nvblock_glue.c via `nand_impl.h`):
- `nand_read_page(dev, block, page, buffer)` - Read page data
- `nand_write_page(dev, block, page, buffer)` - Write page data
- `nand_erase_block(dev, block)` - Erase entire block
- `nand_is_block_bad(dev, block)` - Check bad block status
- `nand_mark_block_bad(dev, block)` - Mark block as bad
- `nand_copy(dev, src_block, src_page, dst_block, dst_page, count)` - Optimized copy

**Supported NAND Chips**:
- Winbond W25N series
- Gigadevice GD5F series
- Alliance AS5F series
- Micron MT29F series
- Zetta ZD35Q1GC
- XTX XT26G08D

## Quick Tips

1. **Always use `/opsx-apply`** for implementation - it handles all the context loading and progress tracking

2. **Check OpenSpec status regularly**:
   ```bash
   openspec status --change "nvblock-integration"
   ```

3. **Commit often** with task numbers:
   ```
   feat(nvblock): implement HAL callbacks (tasks 5.1-5.7)
   ```

4. **Read the design.md** - contains all technical decisions and rationale

5. **When blocked** - use `/opsx-explore` to think through problems before coding

6. **Before archiving** - always run `/opsx-verify` to catch incomplete work

## Troubleshooting

**"Can't build nvblock_glue.c"**
→ Fixed! nvblock component now exists and builds successfully

**"Which tasks should I do next?"**
→ Section 5: nvblock HAL Callbacks (tasks 5.1-5.7) - implement the 6 callbacks
→ Use `/opsx-apply nvblock-integration` for guided implementation

**"How do I know if implementation is correct?"**
→ Build both configurations: Dhara (default) and nvblock
→ Run `/opsx-verify nvblock-integration` when sections are complete
→ Eventually: run test_app with real hardware

**"Can I modify the workflow?"**
→ Yes! OpenSpec is fluid - update artifacts if you discover better approaches

**"What's the metadata buffer size calculation?"**
→ `NVB_META_DMP_START (48) + (bpg * NVB_META_ADDRESS_SIZE (2))`
→ For 64 pages/block: 48 + (64 * 2) = 176 bytes
→ See nvblock_glue.c:152 for implementation

## Resources

- **nvblock repo**: https://github.com/Laczen/nvblock
- **Dhara library**: https://github.com/dlbeer/dhara
- **OpenSpec docs**: Check `.opencode/command/` for detailed command workflows
- **Change artifacts**: `openspec/changes/nvblock-integration/`
