## Context

Dhara is an upstream NAND wear-levelling library that stores raw physical page numbers in its journal and radix-tree metadata. Each chip's bad blocks are at unique physical positions, so the same Dhara journal cannot be replayed on a different chip: every stored page pointer would refer to a different physical page after the remapping changes.

The `spi_nand_flash` component already wraps SPI NAND hardware with an ESP-IDF driver layer and exposes a `dhara_nand_t` descriptor and HAL callbacks. The BBAL sits between this HAL and Dhara, intercepting the 7 `dhara_nand_*` callbacks that Dhara resolves at link time.

The Dhara submodule is third-party upstream code and must not be modified.

## Goals / Non-Goals

**Goals:**
- Insert a transparent translation shim so Dhara sees a contiguous, bad-block-free block address space regardless of the chip's actual bad block layout
- Enable logical content migration between any two SPI NAND chips with the same total block count using `dhara_migrate()`
- Keep implementation entirely in new files; zero changes to the upstream `dhara/` submodule
- Support image-based host tests that validate migration correctness without real hardware

**Non-Goals:**
- Persistent Bad Block Table (BBT) stored in flash — the table is rebuilt from OOB markers at every boot
- Runtime shrinking of `num_logical` when `mark_bad` is called mid-session (Dhara's journal recovery handles this within a session)
- Multi-chip or interleaved NAND support
- Raw bit-copy migration between chips with different bad block layouts

## Decisions

### D1: Cast-based container_of via first-field layout
**Decision:** `logical_nand` is the first field of `dhara_bbal_t`, allowing any `dhara_nand_t *n` passed by Dhara to be cast directly to `dhara_bbal_t *` with `(dhara_bbal_t *)(uintptr_t)n`.

**Rationale:** Dhara's callbacks receive only a `dhara_nand_t *`. A first-field layout is guaranteed by C99 §6.7.2.1 to have the same address as the containing struct. This avoids the need for a global context pointer, a separate registration step, or modifying Dhara's callback signature.

**Alternatives considered:**
- Global `dhara_bbal_t *` singleton — rejected because it prevents multiple simultaneous BBAL instances (needed for migration tests with src + dst).
- `offsetof`-based container_of macro — functionally equivalent, but first-field layout is simpler and equally portable.

### D2: Linker-resolved free functions for the 7 callbacks
**Decision:** `dhara_bbal.c` provides the definitions of the 7 `dhara_nand_*` free functions. No explicit function pointer registration is needed.

**Rationale:** Dhara's design uses weak-or-undefined external symbols for these callbacks; the linker resolves them from whatever translation unit provides the definitions. The BBAL implementation is the sole provider in a BBAL-enabled build. The underlying SPI NAND HAL uses different function names (`spi_nand_read`, etc.) and lives in separate translation units, so there is no collision.

**Alternatives considered:**
- Function pointer table injected at `dhara_map_init` — requires patching upstream Dhara; out of scope.

### D3: Heap-allocated remapping table, resized with realloc after scan
**Decision:** `logical_to_phys[]` is `malloc`'d to `total_blocks * sizeof(dhara_block_t)` before the scan, then `realloc`'d to `num_logical * sizeof(dhara_block_t)` after.

**Rationale:** The good block count is unknown before scanning. A single allocation at max size followed by a shrinking realloc is simpler and avoids a two-pass scan. On any platform with a decent allocator, `realloc` to a smaller size is O(1) or simply updates the header. On platforms without realloc (bare-metal with no heap), this needs adaptation, but `spi_nand_flash` targets Linux host tests and ESP-IDF where standard malloc is available.

**Alternatives considered:**
- Two-pass scan (count, then allocate) — avoids the over-allocation but adds a second O(N) scan across flash, doubling init latency.
- Static array of max size — wastes RAM proportional to total blocks; unsuitable for large chips.

### D4: dhara_migrate iterates src capacity, trims unmapped sectors on dst
**Decision:** `dhara_migrate` iterates `0..capacity_src-1`. If `dhara_map_find` returns "not found" for a sector, `dhara_map_trim` is called on dst for that sector.

**Rationale:** An unmapped sector on src means no user data exists there. Trimming on dst ensures dst has no stale data from a prior use and is consistent with a fresh-format assumption.

**Alternatives considered:**
- Skip unmapped sectors entirely — faster but leaves dst potentially inconsistent if dst was not freshly formatted.

### D5: preserve_contents is a boolean field, not a separate open mode
**Decision:** Add `bool preserve_contents` to `nand_file_mmap_emul_config_t` rather than introducing a new function or open-mode enum.

**Rationale:** `nand_file_mmap_emul_config_t` already carries all other init parameters. A boolean field is backward-compatible (zero-initialized structs default to false), minimally invasive, and self-documenting at the call site.

## Risks / Trade-offs

| Risk | Mitigation |
|------|------------|
| New bad block at runtime makes `logical_to_phys` stale | `mark_bad` still marks the physical block in OOB; next `dhara_bbal_init` will exclude it. Dhara's journal recovery handles the block within the current session. |
| Raw bit-copy of flash to a different chip silently corrupts data | Documented as an explicit non-goal. `dhara_migrate()` is the only supported migration path. |
| `realloc` on bare-metal targets may not shrink | The over-allocation is at most `num_bad * 4` bytes. Callers on constrained targets can omit the `realloc` call. |
| Init scan latency (≈50–200 ms for 128 MiB) | Blocking but bounded; acceptable for mount-time init. No mitigation needed for current targets. |
| Destination `/tmp` space for host tests | Default geometry is 4 MiB; documented. Tests clean up on success. |

## Migration Plan

This is a purely additive change:
1. Merge new files (`dhara_bbal.h/c`, `dhara_migration.h/c`) and the `CMakeLists.txt` update.
2. Existing applications that do not call `dhara_bbal_init` are completely unaffected — the BBAL callbacks only replace the user-provided `dhara_nand_*` callbacks when linked into the same binary.
3. Applications migrating to BBAL replace their `dhara_map_init(&map, &phys_nand, ...)` call with `dhara_bbal_init(&bbal, &phys_nand)` followed by `dhara_map_init(&map, &bbal.logical_nand, ...)`.
4. Rollback: remove the two new source files from `CMakeLists.txt`.

## Open Questions

- **OQ1:** Should `dhara_bbal_init` accept a caller-provided buffer for `logical_to_phys[]` to support zero-heap targets? (Currently deferred; heap allocation is adequate for all current targets.)
- **OQ2:** Should `dhara_migrate` support a maximum-sector override to handle destination capacity < source capacity (partial migration)? (Currently returns error; deferred.)
