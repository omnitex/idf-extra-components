| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- | -------- |

# NAND UBI Metadata Dump

Read-only debug tool that scans every physical erase block (PEB) on an external
SPI NAND flash chip, decodes Linux-compatible UBI EC/VID headers, and prints a
progressive one-line-per-interesting-PEB table plus an end-of-scan summary.

This example does **not** call `nand_ubi_attach()`, and it never erases or writes
flash. Use it to inspect what is already on the chip (for example after running
[`nand_ubi_example`](../nand_ubi_example/)).

## What is printed

1. Chip geometry and a "scanning N PEBs" line
2. A table of MAPPED / VTBL / CKPT / FREE / CORRUPT / BAD / IO_ERR PEBs with parsed
   fields (`EC`, `image_seq`, `vol`, `lnum`, `sqnum`, `copy`, notes). Internal volumes
   are printed by name: `UBI_LAYOUT_VOL_ID` for the two volume-table copies (VTBL),
   `UBI_CKPT_VOL_ID` for attach checkpoints (CKPT).
3. A summary: status counts, and when applicable `image_seq`, EC min/max, max
   `sqnum` / `lnum`
4. The volume table: for each copy, its PEB, record count, reserved LEBs and whether
   its records and table CRC check out; the volumes of the first good copy (slot, type,
   LEB count, name).
5. The newest attach checkpoint, if any (see `docs/fastmap-design.md` in the component):
   its header fields, the pool and previous pool lists, PEB state counts and LEB map
   counts. Pool PEBs written since the checkpoint are marked `*`, pool PEBs in any other
   state than free or data `!`. It then runs the checks `nand_ubi_attach()` makes before
   using the checkpoint (payload CRC, size, PEB number, PEB count, `image_seq`, LEB count
   against the volume table, mapped PEBs are in use) and reports whether the next attach
   would use the checkpoint or do a full scan.

PEBs whose page 0 is erased are counted as EMPTY. PEBs whose page 0 holds data
without the `UBI#` magic (for example a previous Dhara or FatFS image) are counted
as NON_UBI. Neither is listed in the table. A read-write `nand_ubi_attach()` erases
NON_UBI and CORRUPT PEBs, and the summary warns when there are any.

## Hardware Required

Same as [`nand_ubi_example`](../nand_ubi_example/README.md):

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

`sdkconfig.defaults` enables:

```
CONFIG_NAND_FLASH_ENABLE_BDL=y
CONFIG_ESP_NAND_UBI_ENABLE=y
```

Requires ESP-IDF >= 6.0.

## How to Use Example

```bash
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

Tip: run [`nand_ubi_example`](../nand_ubi_example/) first if the chip is blank,
then re-flash this dump example to see MAPPED / FREE rows.

## Example Output

On a factory-blank chip the table is omitted and the summary shows mostly EMPTY:

```
I (325) ubi_dump: Raw NAND: page_size=2048 peb_size=131072 peb_count=1024 disk_size=134217728
I (325) ubi_dump: Scanning 1024 PEBs for UBI EC/VID metadata (read-only)
I (365) ubi_dump: (no interesting PEBs — table omitted)
I (365) ubi_dump: === UBI metadata dump summary ===
I (365) ubi_dump: geometry: page_size=2048 peb_size=131072 peb_count=1024
I (375) ubi_dump: counts: EMPTY=1024 NON_UBI=0 FREE=0 MAPPED=0 CORRUPT=0 BAD=0 IO_ERR=0
I (375) ubi_dump: NAND UBI metadata dump finished successfully
```

After `nand_ubi_example` has written a few LEBs, expect MAPPED rows similar to:

```
PEB   STATUS   EC       image_seq   vol                lnum  sqnum      copy  notes
----  -------  -------  ----------  -----------------  ----  ---------  ----  -----
0     VTBL     1        0x........  UBI_LAYOUT_VOL_ID  0     1          0     volume table copy 0
1     VTBL     1        0x........  UBI_LAYOUT_VOL_ID  1     2          0     volume table copy 1
2     MAPPED   1        0x........  0                  0     3          0
...
I (....) ubi_dump: === UBI metadata dump summary ===
I (....) ubi_dump: counts: EMPTY=... FREE=... MAPPED=... ...
I (....) ubi_dump: NAND UBI metadata dump finished successfully
```

(Exact PEB numbers, `image_seq`, and counts depend on the chip and prior use.)
