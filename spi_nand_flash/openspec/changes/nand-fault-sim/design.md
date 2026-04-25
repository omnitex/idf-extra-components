## Context

The `spi_nand_flash` component ships a Linux mmap emulator (`nand_linux_mmap_emul.c`) used by `nand_impl_linux.c` for host tests. This emulator models a perfect NAND device. Real NAND devices fail in well-understood ways: factory bad blocks, erase/program wear-out, retention loss, and read disturb. The existing Catch2 host-test suite (`host_test/main/`) cannot verify FTL robustness against these failure modes.

## Goals / Non-Goals

**Goals:**
- Provide a configurable fault-injection layer that implements the `nand_impl` interface and intercepts the eight NAND operations (`nand_is_bad`, `nand_mark_bad`, `nand_erase_block`, `nand_prog`, `nand_is_free`, `nand_read`, `nand_copy`, `nand_get_ecc_status`).
- Keep the fault simulator in the `host_test` app directory; zero changes to production `src/`.
- Enable Catch2 tests to exercise Dhara (and future FTLs) against realistic failure scenarios. The `FTLInterface` abstraction makes the test suite FTL-agnostic; Dhara is the first concrete implementation.
- Keep the implementation Apache-2.0 licensed.

**Non-Goals:**
- Emulating timing (access delays, bus cycles).
- Supporting non-Linux (embedded) targets.
- Modifying the existing `nand_impl_linux.c` or `nand_linux_mmap_emul.c` files.

## Decisions

### D1: `nand_fault_sim.c` implements the `nand_impl` interface; substituted at link time

The `nand_impl` interface — the eight global functions (`nand_is_bad`, `nand_mark_bad`, `nand_erase_block`, `nand_prog`, `nand_is_free`, `nand_read`, `nand_copy`, `nand_get_ecc_status`) — is the stable seam between flash hardware semantics and any FTL layer. `dhara_glue.c` calls these functions directly. Any future FTL that follows the component's pattern would do the same. This seam is therefore the correct and FTL-agnostic interception point.

`nand_fault_sim.c` provides its own definitions of these eight functions. The fault-sim test build includes `nand_fault_sim.c` and excludes `nand_impl_linux.c` from the source list. Non-faulted paths delegate to `nand_emul_*` (Layer 3, `nand_linux_mmap_emul.c`) unchanged. The `nand_linux_mmap_emul.c` file is never modified.

```
┌─────────────────────────────────────────────────────┐
│  FTL (Dhara, or any future FTL)                    │
│  calls: nand_read / nand_prog / nand_erase_block   │
│         nand_is_bad / nand_mark_bad / nand_is_free │
│         nand_copy / nand_get_ecc_status             │
└────────────────────┬────────────────────────────────┘
                     │
        ┌────────────▼────────────┐
        │   nand_impl interface   │  ← link-time substitution
        ├─────────────────────────┤
        │ normal build:           │     fault-sim build:
        │  nand_impl_linux.c      │      nand_fault_sim.c
        └────────────┬────────────┘
                     │ delegates (non-faulted paths)
        ┌────────────▼────────────┐
        │  nand_linux_mmap_emul.c │  ← never modified
        │  (nand_emul_read/write/ │
        │   erase_block)          │
        └─────────────────────────┘
```

*Alternatives considered:*
- Patching `nand_impl_linux.c` with `#ifdef` guards — rejected; pollutes production code.
- Intercepting at the `nand_emul_*` level (Layer 3) — rejected; `nand_is_bad` / `nand_mark_bad` / `nand_is_free` carry OOB-byte semantics at the `nand_impl` layer and cannot be intercepted meaningfully below it.
- Intercepting at the `spi_nand_ops` vtable (Layer 1, `dhara_glue.c`) — rejected; that vtable operates at sector granularity and does not expose `nand_is_bad`, `nand_prog`, or `nand_is_free`.

### D2: C implementation with a thin C++ abstraction for FTL-agnostic test integration
The simulator itself is plain C (matching the existing codebase style). A lightweight `ftl_interface.hpp` C++ abstract base class wraps any FTL for Catch2 parameterized tests — `DharaFTL` is the first concrete subclass. Adding a second FTL requires only a new subclass of `FTLInterface`; the robustness tests in `test_ftl_robustness.cpp` are parameterized over `FTLInterface*` and need no changes. This avoids introducing C++ into the component's `src/` tree.

### D3: No data-level bit-flip injection; all error signalling via callbacks
The Linux mmap emulator has no ECC engine. Injecting actual bit flips into data buffers would be indistinguishable from a bad data path and could not be corrected by the FTL. Instead, all soft-error and retention-failure signalling is done exclusively through the `on_page_read_ecc` callback with the appropriate `nand_ecc_status_t` value. Data bytes in the mmap backing are never corrupted by the fault simulator.

This applies to:
- Read-disturb simulation (threshold-based ECC status escalation)
- Grave page / retention failure (`NAND_ECC_NOT_CORRECTED` after `grave_page_threshold` exceeded)

*Alternatives considered:* Actual bit-flip injection into the data buffer — rejected because the Linux target has no ECC hardware to correct them; the FTL would see unrecoverable corruption with no way to distinguish it from a bad data path.

### D4: New files inside `host_test/main/`; `nand_impl_linux.c` excluded from fault-sim build
The following files are added to `host_test/main/`: `nand_fault_sim.c`, `nand_fault_sim.h`, `ftl_interface.hpp`, `dhara_ftl.cpp`, `test_fault_sim.cpp`, and `test_ftl_robustness.cpp`.

The existing `CMakeLists.txt` is updated so that when the fault-sim sources are present, `nand_impl_linux.c` is **removed** from the component's source list and `nand_fault_sim.c` is added in its place. Both files export the same eight `nand_impl` symbol names; only one may be linked at a time. `nand_linux_mmap_emul.c` remains in the build unchanged — `nand_fault_sim.c` calls `nand_emul_*` directly for non-faulted paths.

No new CMake targets are needed; the substitution is conditional within the existing `idf_component_register` call.

## Risks / Trade-offs

- [Risk] Fault simulator state must be reset between test cases → Mitigation: expose `nand_fault_sim_reset()` and call it in Catch2 `SECTION` setup.
- [Risk] Erase-count overflow for very long wear tests → Mitigation: use `uint32_t`; document max value; assert in debug builds.
- [Risk] Power-loss remount tests require the mmap file to persist across sim instances → Mitigation: use `keep_dump = true` on first instance; second instance opens the same file path.
- [Risk] ECC threshold simulation assumes `on_page_read_ecc` is wired by the FTL layer (e.g. `dhara_glue.c`); if the callback is NULL (e.g. in unit tests not using any FTL), ECC injection silently does nothing → Mitigation: document this; unit tests for the sim itself directly inspect read counts rather than relying on the callback.
- [Risk] Bad-block state persistence across remount — `nand_fault_sim.c` implements `nand_mark_bad` identically to `nand_impl_linux.c` (writes OOB marker bytes into the mmap backing). This means bad-block markings survive remount via the mmap file just as they would in the normal build, with no separate in-memory set needed.

### D5: Power-loss crash uses seeded PRNG for both crash point and torn offset
A single `crash_seed` value seeds both the crash-point selection (within `[crash_after_ops_min, crash_after_ops_max]`) and the torn-write byte/page offset, so a test that fails can be reproduced exactly by replaying the same seed. Three crash modes are available: range, per-op probability, and deterministic (range with min==max). All share the same PRNG mechanism.

*Alternatives considered:* Separate seeds for crash point and torn offset — rejected as unnecessary complexity; a single seed is sufficient for reproducibility.

### D6: ECC status and grave pages use the existing callback; per-op failures use a separate PRNG
The fault sim maintains per-page read counts and prog counts. ECC status escalation (read disturb) and grave-page uncorrectable errors both invoke `handle->on_page_read_ecc(page, status, ctx)` — data bytes are never modified. If the callback is NULL the signal is silently dropped.

Per-operation probabilistic failures (`read_fail_prob`, `prog_fail_prob`, `erase_fail_prob`, `copy_fail_prob`) use a separate `op_fail_seed`-seeded PRNG and fire before any emulator delegation — a failed op does not touch the mmap backing. This separates transient fault simulation (per-op probability) from wear/retention simulation (threshold-based ECC callbacks).

*Alternatives considered:* Combining per-op failure and ECC simulation under one PRNG — rejected because they serve different purposes and sharing a seed would make it harder to reason about which failures were triggered by which mechanism.

### D7: Fault scenario presets are plain const structs
`nand_fault_sim_config_preset()` returns a `nand_fault_sim_config_t` by value, which the caller can modify before passing to `nand_fault_sim_init()`. No inheritance or vtable needed.

### D8: `nand_impl` interface is the FTL-agnostic extension point
All FTLs that follow this component's architecture call the same eight `nand_impl` functions. Intercepting at this layer means the fault simulator works identically with Dhara today and with any future FTL (e.g. a custom wear-leveler, SPIFFS-like layer) without modification. Adding a new FTL to the test suite requires only:
1. A new `FTLInterface` subclass (e.g. `MyFtlFTL : public FTLInterface`) in `host_test/main/`.
2. Instantiating it with the same `nand_fault_sim_t*` handle.

No changes to `nand_fault_sim.c`, `nand_fault_sim.h`, or any test file are needed.

## Open Questions

- Should `nand_fault_sim` support multi-plane or multi-LUN emulation? (Deferred — not required for current Dhara tests.)
- Should wear statistics be exposed as a struct-of-arrays or array-of-structs? (AoS chosen for cache locality on per-block queries.)
