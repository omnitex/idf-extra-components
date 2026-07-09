| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- | -------- |

# NAND UBI Example

This example demonstrates the `esp_nand_ubi` block-device layer directly on a
physical SPI NAND flash chip: attach (scan + rebuild the logical-to-physical
erase block table), erase, write, read-back verification, and the
logical-erase-returns-`ESP_ERR_NOT_FOUND` contract.

## Scope: no filesystem is mounted

This example does **not** mount LittleFS or FatFS. There is currently no
FatFS/LittleFS adapter for the `esp_blockdev_t` interface that `esp_nand_ubi`
produces — see [`spi_nand_flash_fatfs/README.md`](../../../spi_nand_flash_fatfs/README.md)
("FatFs on top of the wear-leveling block device (`esp_blockdev_t`) is not
supported in this release"). The existing `spi_nand_flash_fatfs` examples only
support the legacy `spi_nand_flash_device_t` handle, which is a different,
incompatible init path from the one `esp_nand_ubi` requires
(`CONFIG_NAND_FLASH_ENABLE_BDL` + `nand_flash_get_blockdev()`).

Instead, this example exercises the UBI volume's block-device operations
(`read`/`write`/`erase`) directly — exactly the surface that Phase 1 of
`esp_nand_ubi` implements and host-tests.

There is also no separate "format" step. On a factory-blank chip,
`nand_ubi_get_blockdev()` already exposes the full usable capacity; the first
write to any logical erase block (LEB) lazily allocates a physical erase block
(PEB) and writes fresh EC/VID headers on the fly. Attaching and then writing
**is** the format operation.

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

`sdkconfig.defaults` already enables the two Kconfig options `esp_nand_ubi`
requires:

```
CONFIG_NAND_FLASH_ENABLE_BDL=y
CONFIG_ESP_NAND_UBI_ENABLE=y
```

Requires ESP-IDF >= 6.0 (the BDL Kconfig option does not exist on older IDF
versions).

> Keep whatever data is currently on the chip in mind: this example erases and
> overwrites the first `EXAMPLE_NUM_TEST_LEBS` (4) logical erase blocks every
> time it runs. It is safe to re-run repeatedly — the erase-before-write step
> is idempotent — but it is destructive to any UBI volume 0 data already
> occupying those LEBs.

## How to Use Example

Build the project and flash it to the board, then run monitor tool to view
serial output:

```bash
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to
build projects.

## Example Output

The example:
1. Initializes the SPI bus and raw NAND flash block device (`nand_flash_get_blockdev()`)
2. Attaches the UBI volume (`nand_ubi_get_blockdev()`) — scans existing headers if any, or starts from a blank chip
3. Erases the first 4 LEBs (idempotent no-op on a blank chip)
4. Writes a distinct one-page pattern to each of the 4 LEBs and reads it back to verify
5. Erases LEB 0 again and confirms a subsequent read returns `ESP_ERR_NOT_FOUND` (logical erase = unmapped, not stale/blank data)
6. Releases the UBI volume and the raw NAND block device, and tears down the SPI bus

Here is the example's console output on a factory-blank chip:
```
I (315) main_task: Calling app_main()
I (315) example: DMA CHANNEL: 3
I (325) example: Raw NAND: page_size=2048 peb_size=131072 disk_size=134217728
I (325) example: Attaching NAND UBI volume 0 (raw BDL, not Dhara WL)
I (365) example: UBI volume ready: leb_count=1020 leb_size=126976 bytes
I (385) example: LEB 0: write/read-back verified OK
I (405) example: LEB 1: write/read-back verified OK
I (425) example: LEB 2: write/read-back verified OK
I (445) example: LEB 3: write/read-back verified OK
I (465) example: Erasing LEB 0 again to demonstrate logical-erase semantics
I (475) example: LEB 0 read after erase correctly returned ESP_ERR_NOT_FOUND (unmapped)
I (475) example: NAND UBI example finished successfully
I (485) main_task: Returned from app_main()
```

(Exact log timestamps, `leb_count`, and `leb_size` values depend on the
connected chip's geometry and factory bad-block count.)
