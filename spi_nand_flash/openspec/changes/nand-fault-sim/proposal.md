## Why

Host-side unit tests for the FTL layer (`spi_nand_flash`) currently use a simple Linux mmap emulator that faithfully models a perfect NAND device. Real NAND devices exhibit factory bad blocks, wear-induced failures, and random bit flips; without fault injection the test suite cannot verify FTL robustness under these conditions.

## What Changes

- Add `nand_fault_sim` — a Linux-only fault-injection wrapper around the existing `nand_linux_mmap_emul` layer that injects configurable NAND failure modes.
- Implement the same `nand_is_bad / mark_bad / erase_block / prog / is_free / read` interface as `nand_impl_linux.c` so it is a drop-in hardware-layer replacement in host tests.
- Add per-block wear counters and programmable thresholds for: factory bad blocks, weak blocks (erase wear), weak pages (write wear), grave pages (read data loss), and random bit-flip injection.
- Add an abstract `FTLInterface` C++ class and a Catch2-based host-test suite that exercises the FTL against the fault simulator for robustness scenarios.

## Capabilities

### New Capabilities
- `nand-fault-sim`: NAND fault-injection simulator for Linux host tests — wraps the mmap emulator and injects factory bad blocks, wear faults, bit flips, and read disturbance with per-block statistics.
- `ftl-host-tests`: Catch2 host-test suite using an abstract `FTLInterface` C++ class to run robustness scenarios against the fault simulator.

### Modified Capabilities
<!-- None — existing emulator and Dhara integration are unchanged at the spec level. -->

## Impact

- **New files**: `test_apps/host_tests/nand_fault_sim.[ch]` (or similar), `test_apps/host_tests/ftl_interface.hpp`, `test_apps/host_tests/test_ftl_robustness.cpp`
- **Build system**: new `CMakeLists.txt` entries for the host-test app; target restricted to `linux`
- **No production-code changes**: fault sim is test-only and gated by `CONFIG_IDF_TARGET_LINUX`
- **Dependencies**: Catch2 (already used in the component's host tests), no new external deps
- **License**: Apache-2.0; concepts derived from open knowledge of NAND failure modes, not from GPL `nandsim.c`
