# esp_littlefs: report the cache size as `st_blksize`

Candidate upstream fix for [`joltwallet/esp_littlefs`](https://github.com/joltwallet/esp_littlefs).
Not applied anywhere in this repository; kept here so it is not lost.

## Problem

`esp_littlefs` fills `struct stat.st_blksize` with the LittleFS block size
(`cfg.block_size`) in both `vfs_littlefs_fstat()` and `vfs_littlefs_stat()`.

The C library sizes the buffer of every `FILE*` from `st_blksize` (ESP-IDF's
`esp_libc` test `test_file.c` checks exactly this). On a filesystem with small
blocks, such as LittleFS on a 4 KiB NOR partition, this goes unnoticed. On a
volume with large erase blocks it does not: on `esp_nand_ubi` the LittleFS block
is a whole LEB (126976 bytes on a 2 KiB-page, 128 KiB-block NAND), so every
`fopen()` allocates a ~124 KiB stdio buffer until `fclose()`. Calling `setvbuf()`
after `fopen()` does not avoid it, because the buffer already exists by then.

FatFS treats the same field as a heap/speed setting: it reports
`CONFIG_FATFS_VFS_FSTAT_BLKSIZE`, which defaults to 0 and then falls back to the
libc default (128 B). The ESP-IDF speed guide describes it the same way.

## Fix

Report `cfg.cache_size` instead. That is the unit LittleFS already reads and
programs in, so it is a sensible "preferred I/O size", and it is small (4 KiB in
the `littlefs_on_ubi` example).

An alternative for upstream is a Kconfig option like FatFS's, so users can pick
the stdio buffer size; `cache_size` would be a reasonable default for it.

## Measured

ESP32-S3, GigaDevice 4 Gbit SPI NAND (2 KiB pages, 128 KiB blocks), no PSRAM,
`examples/littlefs_on_ubi`, LittleFS on an `esp_nand_ubi` volume. Heap drop
during one `fopen()` ... `fclose()`:

| `esp_littlefs` | Heap used by one open `FILE*` |
|---|---|
| 1.22.3 as released | ~134 KiB |
| with this patch | ~12 KiB (includes a redundant `setvbuf(f, NULL, _IOFBF, 4096)` that build still made) |
| `open()`/`read()`/`write()`, no `FILE*` | ~4 KiB (LittleFS file cache only) |

The whole stack (raw NAND driver, UBI, LittleFS mounted, one file open) peaks at
about 32 KiB above baseline with `open()`/`read()`/`write()`.

## Applying

Made against `joltwallet/littlefs` 1.22.3. From the component root:

```bash
patch -p1 < 0001-fstat-report-cache-size-as-st_blksize.patch
```

To try it in an example without touching `managed_components/`, copy the
component to a directory named `joltwallet__littlefs`, apply the patch there, and
point the dependency at it in `main/idf_component.yml`:

```yaml
  joltwallet/littlefs:
    version: '>=1.21.0'
    override_path: '/path/to/joltwallet__littlefs'
```

The directory name matters: with `override_path` the component is named after
its directory, and the example requires `joltwallet__littlefs`.
