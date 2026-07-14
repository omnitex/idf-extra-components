## [2.0.0]

### Breaking Changes

- **NAND HAL API**: `dhara_nand_prog` and `dhara_nand_copy` each gained a mandatory `oob_lpn` (`dhara_sector_t`) parameter. Any implementation of the dhara NAND HAL must be updated:

  ```c
  /* Before */
  int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p,
                      const uint8_t *data, dhara_error_t *err);
  int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src,
                      dhara_page_t dst, dhara_error_t *err);

  /* After */
  int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p,
                      const uint8_t *data, dhara_sector_t oob_lpn,
                      dhara_error_t *err);
  int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src,
                      dhara_page_t dst, dhara_sector_t oob_lpn,
                      dhara_error_t *err);
  ```

- **NAND HAL API**: `dhara_nand_read_lpn` is now a required HAL function. Drivers that do not support OOB LPN storage must provide a stub:

  ```c
  int dhara_nand_read_lpn(const struct dhara_nand *n, dhara_page_t p,
                          dhara_sector_t *oob_lpn_out, dhara_error_t *err)
  {
      *oob_lpn_out = DHARA_OOB_LPN_NONE;
      return 0;
  }
  ```

  Drivers that provide the stub will compile and run correctly; orphan-page replay after power loss will be silently skipped (safe, no data loss — pages already on flash are still readable via the checkpoint).

### New Features

- **Orphan-page replay**: logical page numbers (LPNs) are now stored in each page's OOB area at program time. On remount after power loss, `dhara_map_resume` scans the orphan tail between the last checkpoint and the journal head, reads OOB LPNs, and reconstructs in-RAM map metadata for pages that were programmed but not yet checkpointed. Data written since the last `dhara_map_sync` is recovered instead of being silently lost.

- `dhara_journal_next_upage` is now a public function (declared in `journal.h`). It advances a page pointer to the next user page, skipping checkpoint pages and wrapping around the chip. Intended for use by replay or diagnostic code that needs to iterate user pages in journal order.

- `dhara_map_replay_orphans` is now a public function (declared in `map.h`). Called automatically by `dhara_map_resume`; exposed so callers that manage journal resume themselves can invoke it directly.

## [1.0.0]

### Versioning

This release starts a **separate semver line for this ESP-IDF component** (`dhara` in idf-extra-components), published via `idf_component.yml`. The FTL sources still come from upstream [dlbeer/dhara](https://github.com/dlbeer/dhara), vendored at a pinned baseline ([VENDORED_UPSTREAM.md](VENDORED_UPSTREAM.md)); upstream’s own tags are **not** the version consumers should pin for this component.

From **1.0.0** onward we intend to align this component’s **MAJOR.MINOR.PATCH** bumps with [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html) at this packaging boundary (vendored code, Espressif patches, build or public API surface exposed through this repo). Notable changes will be listed in this file per release.

### Packaging

- Replace the **git submodule** with an in-tree **vendored** snapshot under `dhara/dhara/` (ordinary tracked files; no submodule checkout required).
- Update **SBOM** and component metadata for the vendored tree (`sbom_dhara.yml`, `idf_component.yml`).

### Behavior

- **No intentional FTL behavior change** relative to the previous submodule layout at the same upstream baseline.
