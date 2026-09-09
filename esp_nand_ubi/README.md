# esp_nand_ubi

A UBI-like Block Device Layer (BDL) middleware for SPI NAND flash. It wraps a raw
NAND flash BDL, hides bad blocks, provides logical erase blocks (LEBs), and enables
position-independent factory images: the filesystem above only ever sees LEB numbers,
never physical PEB numbers.

## Why this component

`esp_partition` was designed for NOR flash — fixed physical offsets, no bad blocks —
and cannot sit below a filesystem on raw NAND without breaking position-independence
at the partition-table level. This component replaces that role for NAND. UBI volumes
are the partitioning scheme for NAND.

## Stack

```
SPI NAND hardware
    |
nand_flash_get_blockdev(&spi_cfg, &nand_bdl)        [spi_nand_flash]
    |
nand_ubi_attach(nand_bdl, &cfg, &ubi_dev)           [esp_nand_ubi — scan once]
nand_ubi_open_volume(ubi_dev, vol_id, &vol_bdl)     [one BDL per created volume]
    |   geometry: disk_size = leb_count x LEB_SIZE, erase_size = LEB_SIZE
    |   bad blocks invisible; no physical addresses above this point
LittleFS / FatFS via vol_bdl
```

> The handle passed to `nand_ubi_attach()` must be the **raw** flash BDL from
> `nand_flash_get_blockdev()`, not the Dhara wear-leveling BDL from
> `spi_nand_flash_wl_get_blockdev()`. Stacking UBI on top of Dhara produces a
> double FTL with incompatible geometry contracts and double write amplification.

## Requirements

- ESP-IDF >= 6.0
- `CONFIG_NAND_FLASH_ENABLE_BDL` enabled in the `spi_nand_flash` component
- `CONFIG_ESP_NAND_UBI_ENABLE` enabled in this component

## Usage

Single-volume common case (whole NAND is one filesystem — `nand_ubi_get_blockdev()`
auto-creates a volume spanning full available capacity the first time it's called on
a device with no volumes yet, so this is a one-shot call on a factory-blank chip):

```c
#include "esp_nand_ubi.h"
#include "esp_nand_blockdev.h"

esp_blockdev_handle_t nand_bdl = NULL;
ESP_ERROR_CHECK(nand_flash_get_blockdev(&spi_cfg, &nand_bdl));

esp_blockdev_handle_t vol_bdl = NULL;
nand_ubi_config_t cfg = NAND_UBI_CONFIG_DEFAULT();
ESP_ERROR_CHECK(nand_ubi_get_blockdev(nand_bdl, &cfg, &vol_bdl));

/* mount a filesystem on vol_bdl ... */

vol_bdl->ops->release(vol_bdl);   /* also detaches the UBI device */
nand_bdl->ops->release(nand_bdl);
```

Multi-volume (explicit device lifecycle, one or more named volumes):

```c
nand_ubi_device_t *ubi_dev = NULL;
ESP_ERROR_CHECK(nand_ubi_attach(nand_bdl, &cfg, &ubi_dev));

/* Only needed once per volume, ever: the volume table (vtbl) persists on flash,
 * so a later nand_ubi_attach() on the same chip already sees it — no need to
 * re-create volumes on every boot. */
uint32_t app_vol_id = 0, data_vol_id = 0;
ESP_ERROR_CHECK(nand_ubi_create_volume(ubi_dev, "app", UBI_VID_DYNAMIC, 64, &app_vol_id));
ESP_ERROR_CHECK(nand_ubi_create_volume(ubi_dev, "data", UBI_VID_DYNAMIC, 200, &data_vol_id));

esp_blockdev_handle_t app_vol_bdl = NULL;
ESP_ERROR_CHECK(nand_ubi_open_volume(ubi_dev, app_vol_id, &app_vol_bdl));

/* ... use app_vol_bdl ... */

app_vol_bdl->ops->release(app_vol_bdl);
ESP_ERROR_CHECK(nand_ubi_detach(ubi_dev));
nand_bdl->ops->release(nand_bdl);
```

## On-flash format

Two 64-byte headers per physical erase block (PEB), both at the start of the block,
byte-compatible with Linux UBI EC/VID headers:

```
PEB offset 0            EC header  (magic "UBI#")  image_seq, ec, offsets, CRC
vid_hdr_offset          VID header (magic "UBI!")  vol_id, lnum, sqnum, CRC
data_offset             LEB data   (LEB_SIZE = PEB_SIZE - data_offset)
```

PEBs 0 and 1 are always reserved for two mirrored copies of the volume table
(`vol_id = 0x7FFFEFFF`, Linux UBI's `UBI_LAYOUT_VOLUME_ID`), holding one
172-byte `ubi_vtbl_record` per created volume (name, type, LEB count, CRC) —
also byte-compatible with Linux UBI's layout-volume format. Every other PEB's
VID header carries the real `vol_id` it belongs to; `nand_ubi_attach()` parses
the volume table first, then routes each subsequent PEB into the correct
volume's slice of the shared LEB->PEB table.

With `data_offset = 2 x page_size`, images built with `ubinize` from `mtd-utils`
are compatible with this layer. The `esp_ubinize.py` host tool (Phase 3) is a simpler
alternative for users without `mtd-utils`.

### Read contract for unmapped LEBs

A LEB that has never been written (no PEB mapping yet -- e.g. every LEB of a
freshly-created volume, or a LEB right after `erase()`) reads back as if it were a
blank, erased NAND block: the read fills the destination buffer with `0xFF` and
returns `ESP_OK`. It does **not** return `ESP_ERR_NOT_FOUND`. This matches what a
filesystem expects when it probes a block before ever writing to it (erased flash
== all `0xFF`), and is required for `LittleFS`/`FatFS` to mount cleanly on a
brand-new volume: without this convention, the very first superblock read on an
empty volume looked like a hard I/O fault instead of "nothing here yet", which
made `format_if_mount_failed` fail before it ever got a chance to write anything.

## Examples

| Example | Description | Hardware |
|---------|-------------|----------|
| `examples/nand_ubi_example` | Attach + erase + write/read-back verification + logical-erase-returns-0xFF-filled-data, directly on physical SPI NAND flash. Does not mount a filesystem. | Physical ESP32 + external SPI NAND chip |
| `examples/nand_ubi_metadata_dump` | Read-only scan of every PEB’s EC/VID headers; progressive table of MAPPED/FREE/CORRUPT/BAD/IO_ERR plus end-of-scan summary. Does not call `nand_ubi_attach()` or write flash. | Physical ESP32 + external SPI NAND chip |
| `examples/littlefs_on_ubi` (Phase 2) | Mounts `joltwallet/littlefs` directly on `nand_ubi_get_blockdev()`'s volume BDL — no Dhara, no adapter/shim needed (`esp_littlefs` already speaks `esp_blockdev_t`). Write/close/remount/read-back round-trip. | Physical ESP32 + external SPI NAND chip |

See each example's `README.md` for wiring and expected console output.

## Known limitations

- **No mid-session bad-block eviction signal to LittleFS**: `joltwallet/littlefs`'s BDL adapter
  (`littlefs_bdl.c`) maps every `ESP_ERR_*` to generic `LFS_ERR_IO`, never `LFS_ERR_CORRUPT`.
  LittleFS only evicts/reallocates a block on `LFS_ERR_CORRUPT`. UBI's attach-time
  `IS_BAD_BLOCK` scan hides factory-bad blocks fine, but a PEB that fails *during* a mounted
  session (write/ECC failure after attach) will surface as a plain IO error, not trigger
  littlefs's block-eviction path. Fixing this needs either a local patch to `littlefs_bdl.c`,
  an upstream `joltwallet/esp_littlefs` change, or UBI itself silently remapping the LEB on
  write failure before returning to the caller. Not yet scheduled into a phase — see
  `docs/plans/2026-07-09-esp-nand-ubi-mvp.md` Phase 2.

- **Erase counter is tracked but not yet acted on**: every erase through a volume's
  `erase()` op persists an honest, incrementing `ec` in that PEB's on-flash EC header
  (survives detach/reattach), but `nand_ubi_eba_find_free_peb()` is still a plain
  first-fit linear scan — nothing yet *prefers* a low-EC PEB, so wear is not actually
  spread by this layer. That allocator change is Task 12 (background WL). PEBs 0 and 1
  (the volume-table mirrors, rewritten via a dedicated code path separate from the
  per-volume erase/allocate path) are excluded from this tracking entirely — their EC
  always reads 0.

## Status

Phase 1 (minimum viable layer): attach scan, EBA table, per-volume read/write/erase,
passive bad-block hiding. Phase 2 (LittleFS-on-UBI hardware PoC): example in progress.
Multi-volume support (`nand_ubi_create_volume()`, on-flash volume table) is implemented
and host-tested. Real wear-leveling and fastmap are planned for later phases; see
`docs/plans/2026-07-09-esp-nand-ubi-mvp.md` for the full phase breakdown.
