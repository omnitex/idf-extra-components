## [1.1.0]

### New Features

- **Path cache for sequential FTL map lookups** (`DHARA_MAP_PATH_CACHE`). When enabled (compile-time define), `trace_path()` caches the last successful read-only radix-tree traversal (136 bytes in `dhara_map`). Consecutive sector lookups that share a common prefix skip the shared levels entirely, saving up to 31 of 32 `dhara_journal_read_meta` calls for sequential sector access patterns. Disabled by default; can be overridden via `CONFIG_NAND_DHARA_FTL_MAP_PATH_CACHE` Kconfig in `spi_nand_flash`. Adds `prev_target`, `prev_path[]`, and `prev_root` fields to `struct dhara_map`, and moves `DHARA_RADIX_DEPTH` to the public `map.h` header.

## [1.0.0]

### Versioning

This release starts a **separate semver line for this ESP-IDF component** (`dhara` in idf-extra-components), published via `idf_component.yml`. The FTL sources still come from upstream [dlbeer/dhara](https://github.com/dlbeer/dhara), vendored at a pinned baseline ([VENDORED_UPSTREAM.md](VENDORED_UPSTREAM.md)); upstream’s own tags are **not** the version consumers should pin for this component.

From **1.0.0** onward we intend to align this component’s **MAJOR.MINOR.PATCH** bumps with [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html) at this packaging boundary (vendored code, Espressif patches, build or public API surface exposed through this repo). Notable changes will be listed in this file per release.

### Packaging

- Replace the **git submodule** with an in-tree **vendored** snapshot under `dhara/dhara/` (ordinary tracked files; no submodule checkout required).
- Update **SBOM** and component metadata for the vendored tree (`sbom_dhara.yml`, `idf_component.yml`).

### Behavior

- **No intentional FTL behavior change** relative to the previous submodule layout at the same upstream baseline.
