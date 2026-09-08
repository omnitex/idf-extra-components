| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- | -------- |

# LittleFS on UBI Example

This example mounts [`joltwallet/littlefs`](https://components.espressif.com/components/joltwallet/littlefs)
directly on the `esp_nand_ubi` volume block device (`nand_ubi_get_blockdev()`),
running on physical SPI NAND flash hardware. It writes a file, closes it,
reopens it, and reads the contents back to prove the round-trip through
UBI's LEB->PEB translation.

## Stack

```
SPI NAND hardware
    |
nand_flash_get_blockdev()      raw flash BDL          [spi_nand_flash]
    |
nand_ubi_get_blockdev()        UBI volume BDL         [esp_nand_ubi]
    |
esp_vfs_littlefs_register()    POSIX file API         [joltwallet/littlefs]
```

No Dhara wear-leveling BDL anywhere in this chain, and no filesystem
adapter/shim was written for this example: `joltwallet/littlefs >= 1.21.0`
already speaks `esp_blockdev_t` natively via `esp_vfs_littlefs_conf_t.blockdev`.

## Design decisions

- **`CONFIG_LITTLEFS_BLOCK_CYCLES=-1`** (disables LittleFS's own metadata
  wear-leveling): `esp_nand_ubi` has no wear-leveling implemented yet (Phase 1
  is passive/lowest-EC-free-PEB allocation only). With nothing yet for
  LittleFS's block-cycling to double up against, disabling it now is simply
  correct — not a placeholder. Revisit once `esp_nand_ubi`'s Phase 4
  background WL task lands (see `docs/plans/2026-07-09-esp-nand-ubi-mvp.md`).
- **`CONFIG_LITTLEFS_CACHE_SIZE=4096`**: `joltwallet/littlefs` requires a
  per-file cache size >= the NAND page size, or mount fails with
  `No valid cache_size <= ... for block=...`. 4096 bytes covers every SPI NAND
  part `spi_nand_flash` currently supports.

## Known limitation: no mid-session bad-block eviction signal

`joltwallet/littlefs`'s BDL adapter (`littlefs_bdl.c`) maps every `ESP_ERR_*`
return to generic `LFS_ERR_IO`, never `LFS_ERR_CORRUPT`. LittleFS only
evicts/reallocates a block on `LFS_ERR_CORRUPT`. `esp_nand_ubi`'s attach-time
`IS_BAD_BLOCK` scan hides factory-bad blocks fine, but a PEB that fails
*during* a mounted session (a write/ECC failure discovered after attach) will
surface to LittleFS as a plain IO error, not trigger its block-eviction path.
This is a known gap, not fixed by this example — see
`esp_nand_ubi/README.md` and `docs/plans/2026-07-09-esp-nand-ubi-mvp.md` Phase 2.

## Hardware Required

* Any ESP board from the supported targets list above
* An external SPI NAND Flash chip connected to the following pins:
  * For ESP32 (SPI3):
    - MOSI - SPI3_IOMUX_PIN_NUM_MOSI (23)
    - MISO - SPI3_IOMUX_PIN_NUM_MISO (19)
    - CLK  - SPI3_IOMUX_PIN_NUM_CLK (18)
    - CS   - SPI3_IOMUX_PIN_NUM_CS (5)
    - WP   - SPI3_IOMUX_PIN_NUM_WP (22)
    - HD   - SPI3_IOMUX_PIN_NUM_HD (21)
  * For other ESP chips (SPI2):
    - MOSI - SPI2_IOMUX_PIN_NUM_MOSI (13)
    - MISO - SPI2_IOMUX_PIN_NUM_MISO (12)
    - CLK  - SPI2_IOMUX_PIN_NUM_CLK (14)
    - CS   - SPI2_IOMUX_PIN_NUM_CS (15)
    - WP   - SPI2_IOMUX_PIN_NUM_WP (2)
    - HD   - SPI2_IOMUX_PIN_NUM_HD (4)

## Configuration

`sdkconfig.defaults` already enables what this example needs:

```
CONFIG_NAND_FLASH_ENABLE_BDL=y
CONFIG_ESP_NAND_UBI_ENABLE=y
CONFIG_LITTLEFS_CACHE_SIZE=4096
CONFIG_LITTLEFS_BLOCK_CYCLES=-1
```

`CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED` (menuconfig -> "LittleFS on UBI Example
Configuration") is on by default so the first run on a factory-blank chip
formats automatically.

Requires ESP-IDF >= 6.0 (`esp_blockdev` and LittleFS blockdev support) and
`joltwallet/littlefs >= 1.21.0`.

## How to Use Example

```bash
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

## Example Output

The example:
1. Initializes the SPI bus and raw NAND flash block device (`nand_flash_get_blockdev()`)
2. Attaches the UBI volume (`nand_ubi_get_blockdev()`)
3. Mounts LittleFS on the UBI volume BDL (`esp_vfs_littlefs_register()`, formatting on first run)
4. Writes `hello.txt`, closes it
5. Reopens `hello.txt` and reads the contents back
6. Unmounts LittleFS (releases the UBI volume and detaches the UBI device) and releases the raw NAND BDL

```
I (315) main_task: Calling app_main()
I (315) example: DMA CHANNEL: 3
I (325) example: Raw NAND: page_size=2048 peb_size=131072 disk_size=134217728
I (325) example: Attaching NAND UBI volume 0 (raw BDL, not Dhara WL)
I (365) example: UBI volume ready: leb_size=126976 disk_size=129511424
I (365) example: Mounting LittleFS on the UBI volume BDL
I (665) esp_littlefs: Initializing LittleFS
I (6685) example: LittleFS: 126484 kB total, 8 kB used
I (6685) example: Opening file
I (6715) example: File written
I (6715) example: Reading file back
I (6715) example: Read from file: 'Written using ESP-IDF v6.0-... over esp_nand_ubi'
I (6715) example: LittleFS: 126484 kB total, 8 kB used
I (6725) example: LittleFS-on-UBI example finished successfully
I (6735) main_task: Returned from app_main()
```

(Exact log timestamps, `leb_size`, `disk_size`, and reported LittleFS capacity
depend on the connected chip's geometry and factory bad-block count.)
