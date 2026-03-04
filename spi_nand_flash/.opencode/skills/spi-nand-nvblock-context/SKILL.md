---
name: spi-nand-nvblock-context
description: How to work on the spi_nand_flash nvblock integration project - OpenSpec workflows and development practices
license: MIT
metadata:
  author: OpenCode AI
  version: "2.0"
  created: 2026-03-04
  updated: 2026-03-04
---

# SPI NAND Flash + nvblock Integration - How To Guide

This skill provides everything you need to know to work on the nvblock integration project using OpenSpec workflows.

## How to Check Current Progress

**ALWAYS start a session by checking current state:**

```bash
# View dashboard with overall progress
openspec view

# Check specific change status
openspec status --change "nvblock-integration"

# List all changes
openspec list
```

This shows you:
- Current task completion percentage
- Which sections are done
- What to work on next

## How to Implement Tasks with OpenSpec

### ⭐ Recommended: Use `/opsx-apply` Command

**This is the PRIMARY way to work on this project:**

```
/opsx-apply nvblock-integration
```

**What it does:**
- Loads all context (proposal, specs, design, tasks)
- Shows current progress
- Guides you through pending tasks
- **Automatically marks tasks complete** in tasks.md as you work
- Updates OpenSpec tracking correctly

**Why use it:**
- Prevents forgetting to update tasks.md (which breaks OpenSpec dashboard)
- Ensures progress tracking stays accurate
- Provides full context automatically
- Handles the OpenSpec workflow for you

### Alternative: Manual Task Tracking

If you work without `/opsx-apply`, **you MUST manually update tasks.md**:

1. Edit `openspec/changes/nvblock-integration/tasks.md`
2. Change `- [ ]` to `- [x]` for completed tasks
3. Commit tasks.md with your implementation commits
4. Verify with `openspec view` that progress updated

**Common mistake:** Implementing features but forgetting to mark tasks complete → OpenSpec shows wrong progress percentage!

## How to Work on This Project

### Starting a Work Session

1. **Check current state** (see above)
2. **Use the recommended approach:**
   ```
   /opsx-apply nvblock-integration
   ```
3. **Or manually review pending tasks:**
   ```bash
   # View which tasks are pending
   cat openspec/changes/nvblock-integration/tasks.md | grep "^\- \[ \]"
   ```

### During Implementation

**If using `/opsx-apply`:**
- Let it guide you through tasks
- It will mark tasks complete automatically

**If working manually:**
- Implement the code
- Update tasks.md to mark `[x]` complete
- Commit both code AND tasks.md together
- Verify: `openspec view` shows correct progress

### Commit Message Format

Include task numbers in commits:

```bash
git commit -m "feat(nvblock): implement HAL callbacks (tasks 5.1-5.7)

- Implement nvb_read_cb bridging to nand_read_page
- Implement nvb_write_cb bridging to nand_write_page
..."
```

### Building and Testing

**Test both configurations after changes:**

```bash
# Build with Dhara (default - backward compatibility check)
cd test_app
idf.py build

# Build with nvblock
# Edit test_app/sdkconfig: CONFIG_SPI_NAND_FLASH_WL_NVBLOCK=y
idf.py fullclean build
```

**Both must build successfully!**

## Project Architecture

### What We're Building

Add **nvblock** as an alternative wear leveling implementation alongside **Dhara**:
- Compile-time selection via Kconfig
- 100% backward compatible (no public API changes)
- Parallel implementation (`nvblock_glue.c` alongside `dhara_glue.c`)
- Both implement the same `spi_nand_ops` interface

### Architecture Diagram

```
┌─────────────────────────────────────────┐
│  Public API (spi_nand_flash.h)         │  ← NO CHANGES ALLOWED
└─────────────────────────────────────────┘
                   │
┌─────────────────────────────────────────┐
│  Core (nand.c)                          │
│  Registers ops based on Kconfig         │
└─────────────────────────────────────────┘
                   │
        ┌──────────┴──────────┐
        ▼                     ▼
┌──────────────┐      ┌──────────────┐
│ dhara_glue.c │      │nvblock_glue.c│  ← Implementation here
│ (existing)   │      │  (new)       │
└──────────────┘      └──────────────┘
        │                     │
        └──────────┬──────────┘
                   ▼
        ┌─────────────────────┐
        │  HAL (nand_impl.h)  │
        │  - nand_read_page   │
        │  - nand_write_page  │
        │  - nand_erase_block │
        │  - nand_is_bad      │
        │  - nand_mark_bad    │
        └─────────────────────┘
```

### Key Interfaces

**spi_nand_ops** (what nvblock_glue.c must implement):
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
    esp_err_t (*copy_sector)(spi_nand_flash_device_t *handle, uint32_t src, uint32_t dst);
    esp_err_t (*get_capacity)(spi_nand_flash_device_t *handle, uint32_t *sectors);
} spi_nand_ops;
```

**nvblock HAL callbacks** (bridge to HAL):
```c
int nvb_read_cb(void *ctx, uint32_t group, uint32_t page, void *buf, uint32_t len);
int nvb_prog_cb(void *ctx, uint32_t group, uint32_t page, const void *buf, uint32_t len);
int nvb_move_cb(void *ctx, uint32_t from_grp, uint32_t from_pg, uint32_t to_grp, uint32_t to_pg, uint32_t len);
bool nvb_is_bad_cb(const void *ctx, uint32_t group);
void nvb_mark_bad_cb(const void *ctx, uint32_t group);
```

**Terminology mapping:**
- nvblock "group" = NAND block
- nvblock "page" = page within block
- nvblock "virtual block" = NAND page (configured via `bsize`)

## Implementation Guidelines

### Runtime Configuration

nvblock config is calculated from chip parameters in `nvblock_init()`:

```c
uint32_t bsize = handle->chip.page_size;           // e.g., 2048 bytes
uint32_t bpg = 1 << handle->chip.log2_ppb;         // e.g., 64 pages/block
uint32_t gcnt = handle->chip.num_blocks;           // e.g., 1024 blocks
uint32_t spgcnt = (gcnt * gc_factor) / 100;        // spare groups (min 2)
```

**Metadata buffer size:**
```c
size_t meta_size = NVB_META_DMP_START + (bpg * NVB_META_ADDRESS_SIZE);
// For 64 pages/block: 48 + (64 * 2) = 176 bytes
```

### HAL Function Reference

Available in `nvblock_glue.c` via `#include "nand_impl.h"`:

```c
esp_err_t nand_read_page(device, block, page, buffer);
esp_err_t nand_write_page(device, block, page, buffer);
esp_err_t nand_erase_block(device, block);
bool nand_is_block_bad(device, block);
esp_err_t nand_mark_block_bad(device, block);
esp_err_t nand_copy(device, src_block, src_page, dst_block, dst_page, count);
```

**Use dhara_glue.c as reference** for how to:
- Structure the glue layer
- Implement callbacks
- Handle errors
- Manage context

### Critical Constraints

⚠️ **MUST NOT:**
- Change public API in `include/spi_nand_flash.h`
- Break backward compatibility with Dhara
- Modify `dhara_glue.c` or existing Dhara integration
- Support runtime switching (compile-time only)

✅ **MUST:**
- Keep Dhara as default (backward compatible)
- Maintain identical public API behavior
- Support all features (bad blocks, ECC, trim, sync)
- Test both configurations (Dhara and nvblock)

## OpenSpec Commands Reference

### Daily Workflow Commands

```bash
# Check what needs to be done
openspec view

# Start working (recommended)
/opsx-apply nvblock-integration

# When stuck or need to think through problems
/opsx-explore

# Before finishing work
/opsx-verify nvblock-integration
```

### Other Available Commands

```bash
# Create new change
/opsx-new <change-name>

# Continue creating artifacts
/opsx-continue <change-name>

# Fast-forward create all artifacts
/opsx-ff <change-name>

# Sync specs without archiving
/opsx-sync <change-name>

# Archive completed change
/opsx-archive <change-name>
```

## File Locations

```
spi_nand_flash/
├── openspec/changes/nvblock-integration/
│   ├── proposal.md          # Why this change
│   ├── design.md            # Technical decisions
│   ├── tasks.md             # Implementation checklist ⭐ UPDATE THIS!
│   └── specs/               # Detailed capability specs
├── src/
│   ├── nvblock_glue.c       # Your implementation here
│   ├── dhara_glue.c         # Reference implementation
│   └── nand.c               # Core (calls nand_register_dev)
├── priv_include/
│   ├── nand.h               # spi_nand_ops interface definition
│   └── nand_impl.h          # HAL function declarations
└── Kconfig                  # Wear leveling selection menu

nvblock/ (sibling directory)
├── nvblock/                 # Git submodule
│   └── lib/
│       ├── include/nvblock/nvblock.h   # nvblock API
│       └── src/nvblock.c               # nvblock implementation
```

## Troubleshooting

### "OpenSpec shows wrong progress percentage"

**Cause:** tasks.md not updated when implementing features

**Fix:**
```bash
# Either use /opsx-apply which handles this automatically, or:
# Manually edit openspec/changes/nvblock-integration/tasks.md
# Change - [ ] to - [x] for completed tasks
# Commit tasks.md
# Verify: openspec view
```

### "Which tasks should I do next?"

```bash
# Check OpenSpec
openspec view

# Or use guided implementation
/opsx-apply nvblock-integration
```

### "Build fails with nvblock configuration"

1. Check Kconfig: `CONFIG_SPI_NAND_FLASH_WL_NVBLOCK=y`
2. Clean build: `idf.py fullclean build`
3. Verify only nvblock_glue.c compiles (not dhara_glue.c)
4. Check for linker errors (usually missing function implementations)

### "How to verify changes are correct?"

```bash
# Build both configurations
# 1. Dhara (default) - must still work (backward compat)
idf.py build

# 2. nvblock - must compile and link
# (edit sdkconfig to enable nvblock)
idf.py fullclean build

# Run verification
/opsx-verify nvblock-integration
```

## ESP-IDF Component Structure

Standard ESP-IDF component layout:

```
component/
├── include/           # Public API headers
├── priv_include/      # Private headers (component-internal)
├── src/               # Implementation (.c files)
├── Kconfig            # Configuration options
├── CMakeLists.txt     # Build configuration
└── idf_component.yml  # Component dependencies
```

## Resources

- **nvblock repo**: https://github.com/Laczen/nvblock (874-line implementation)
- **Dhara reference**: `src/dhara_glue.c` (existing implementation to mirror)
- **OpenSpec change**: `openspec/changes/nvblock-integration/`
- **Design decisions**: `openspec/changes/nvblock-integration/design.md`
- **Task checklist**: `openspec/changes/nvblock-integration/tasks.md`

## Quick Reference

**Always start here:**
```bash
openspec view                      # See current progress
/opsx-apply nvblock-integration    # Start working (recommended!)
```

**Reference implementation:**
- Look at `src/dhara_glue.c` for patterns
- Check `design.md` for technical decisions
- Read nvblock API in `nvblock/nvblock/lib/include/nvblock/nvblock.h`

**Don't forget:**
- Update tasks.md (or use `/opsx-apply` which does it for you)
- Test both Dhara and nvblock builds
- Commit with task numbers in message
- Check `openspec view` shows correct progress
