## Why

Dhara's journal and radix-tree map store raw physical block/page numbers in metadata. Because bad blocks occupy different physical locations on every chip, the logical address space varies between devices, making it impossible to migrate logical content from one chip to another without rewriting every sector. A Bad Block Abstraction Layer (BBAL) inserted between the raw NAND HAL and Dhara presents a contiguous, bad-block-free block address space to Dhara, enabling transparent content migration between any two chips with the same total block count.

## What Changes

- **NEW** `dhara_bbal_t` context struct and `dhara_bbal_init` / `dhara_bbal_deinit` API in `include/dhara_bbal.h` and `src/dhara_bbal.c`
- **NEW** 7 `dhara_nand_*` callback implementations in `src/dhara_bbal.c` that translate logical block/page addresses to physical via a remapping table
- **NEW** `dhara_migrate()` utility in `migration/dhara_migration.h` and `migration/dhara_migration.c` for transferring logical sector content between two Dhara maps
- **MODIFIED** `nand_file_mmap_emul_config_t` — adds `preserve_contents` flag to suppress 0xFF initialization so an existing flash image file can be opened without being wiped
- **NEW** Host test cases: image generator and migration-with-bad-blocks verifier
- `CMakeLists.txt` updated to include the two new source files

## Capabilities

### New Capabilities
- `bbal`: Bad Block Abstraction Layer — translates logical block addresses to physical, presents a contiguous bad-block-free space to Dhara, rebuilt from OOB markers on every init
- `dhara-migration`: Logical sector migration utility — copies all mapped sectors from a source Dhara map to a destination Dhara map, agnostic of each device's bad block layout
- `mmap-emulator-preserve`: `preserve_contents` flag on the mmap emulator — allows host tests to re-open an existing flash image without zeroing it
- `bbal-host-tests`: Image-based host test suite — generates a known-good source image and verifies correct migration to a bad-block-injected destination image

### Modified Capabilities
<!-- No existing spec-level capabilities are being changed -->

## Impact

- **New files**: `include/dhara_bbal.h`, `src/dhara_bbal.c`, `migration/dhara_migration.h`, `migration/dhara_migration.c`
- **Modified files**: `include/nand_linux_mmap_emul.h`, `src/nand_linux_mmap_emul.c`, `CMakeLists.txt`
- **New host test files**: `host_test/test_dhara_bbal_image_gen.cpp`, `host_test/test_dhara_bbal_migrate.cpp`
- **Dependencies**: Dhara upstream submodule (`dhara/`) is read-only; no modifications
- **RAM**: Adds a heap allocation of `num_logical_blocks × 4` bytes per BBAL instance (≈ 4 KiB for a 128 MiB chip)
- **Init latency**: One OOB read per physical block at startup (O(N), ≈ 50–200 ms for 128 MiB)
