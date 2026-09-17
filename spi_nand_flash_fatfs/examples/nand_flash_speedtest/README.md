| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- | -------- |

# SPI NAND Flash — Cache Layer Speedtest

Benchmark for the four independently togglable read-path caches added on
this branch:

- **`CONFIG_NAND_FLASH_PAGE_REGISTER_CACHE`** (driver level, `spi_nand_flash/src/nand_impl.c`):
  skips the `READ PAGE ADDRESS` command when the requested page is already
  loaded in the NAND chip's internal register.
- **`CONFIG_NAND_DHARA_FTL_MAP_PATH_CACHE`** (FTL level, `dhara/dhara/dhara/map.c`):
  skips radix-tree levels shared with the previous `trace_path()` lookup.
- **`CONFIG_NAND_DHARA_FTL_MAP_EXACT_REPEAT_CACHE`** (FTL level, depends on
  the path cache above): fast-paths an exact repeat of the same sector
  lookup, skipping the metadata read and walk entirely.
- **`CONFIG_NAND_DHARA_JOURNAL_META_CACHE_SLOTS`** (journal level, below the
  FTL map, `dhara/dhara/dhara/journal.c`): 0-8 slots, each caching a whole
  checkpoint page in DRAM to accelerate `dhara_journal_read_meta()`.

All four are off by default (0 slots for the last one); enable any
combination in menuconfig and compare the printed timings for the same
workload to measure each layer's contribution.

Canonical source: [`main/spi_nand_flash_example_main.c`](main/spi_nand_flash_example_main.c).

## Workload

Two write/read workloads run back to back, since they stress the caches
differently and a single shape would be misleading for one side or the
other:

1. **Small files** (`SMALL_FILE_COUNT` x `SMALL_FILE_SIZE`, default 64 x
   4 KiB): write throughput here is dominated by per-file FAT/journal
   overhead (directory entry + close-time sync paid once per file, and each
   file is smaller than the FAT allocation unit), not raw NAND bandwidth.
   This workload's write number is mostly overhead noise; its point is the
   read phases below — ascending sector numbers across many small files
   share long common radix-tree prefixes, exercising the dhara map's
   prefix cache.
2. **Large files** (`LARGE_FILE_COUNT` x `LARGE_FILE_SIZE`, default 4 x
   256 KiB, written/read in `LARGE_CHUNK_SIZE` chunks matching the FAT
   allocation unit): per-file overhead is amortized over much more data, so
   this is a fairer comparison point for write throughput.

For each workload:

- **Sequential write**: baseline for that workload's write throughput.
- **Cold sequential read**: read the same files back in order. Exercises the
  `trace_path()` prefix-cache path (ascending sector numbers share long
  common radix-tree prefixes).
- **Warm repeat read**: read the exact same files again immediately after.
  Exercises the page-register cache and the `trace_path()` exact-repeat fast
  path (same sector looked up twice while the journal is unchanged).

The example prints elapsed time and throughput for each phase of each
workload, plus the warm/cold speedup ratio.

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

Toggle the caches under test in menuconfig:

```
idf.py menuconfig
-> Component config -> SPI NAND Flash configuration
   -> NAND_FLASH_PAGE_REGISTER_CACHE
   -> NAND_DHARA_FTL_MAP_PATH_CACHE
   -> NAND_DHARA_FTL_MAP_EXACT_REPEAT_CACHE  (only visible once the path
      cache above is enabled)
   -> NAND_DHARA_JOURNAL_META_CACHE_SLOTS    (0-8, 0 = disabled)
```

Keep **`CONFIG_NAND_FLASH_ENABLE_BDL` disabled** — this example uses
`spi_nand_flash_init_device()`, which is not available when BDL is enabled.

The chip is reformatted on every run (`format_if_mount_failed = true` and a
fresh mount), so timings are reproducible across flashes.

## How to Use Example

Build the project and flash it to the board, then run monitor tool to view
serial output:

```bash
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

Run it once per cache configuration (all off, each layer on individually,
all on) to compare.

## Example Output

```
I (315) speedtest: --- Cache layers under test ---
I (315) speedtest: driver page-register cache : off
I (315) speedtest: dhara map path cache       : off
I (315) speedtest: dhara map exact-repeat     : off
I (315) speedtest: dhara journal meta cache   : off
I (315) speedtest: --------------------------------
I (355) speedtest: FAT FS: 117024 kB total, 117024 kB free
I (355) speedtest: === small files workload (64 x 4096 bytes) ===
I (610) speedtest: sequential write          2500000 us  (25.6 files/s, 102.4 kB/s)
I (870) speedtest: cold sequential read        260000 us  (246.2 files/s, 984.6 kB/s)
I (1010) speedtest: warm repeat read            140000 us  (457.1 files/s, 1828.6 kB/s)
I (1010) speedtest: warm/cold speedup: 1.86x
I (1010) speedtest: === large files workload (4 x 262144 bytes) ===
I (3420) speedtest: sequential write          2410000 us  (1.7 files/s, 435.0 kB/s)
I (5830) speedtest: cold sequential read       2380000 us  (1.7 files/s, 440.3 kB/s)
I (8200) speedtest: warm repeat read           1290000 us  (3.1 files/s, 812.4 kB/s)
I (8200) speedtest: warm/cold speedup: 1.85x
I (8200) speedtest: Benchmark done
```

Numbers above are illustrative; actual timings depend on the chip, SPI clock,
and which cache options are enabled. Note the small-files write number is
dominated by per-file overhead (see [Workload](#workload)) — use the
large-files write number to judge raw sequential write throughput.
