# NAND Flash translation layer for small MCUs

This component provides an ESP-IDF wrapper around the [Dhara library](https://github.com/dlbeer/dhara),
a NAND Flash Translation Layer for small MCUs.

The sources under `dhara/dhara/` are **vendored** from the upstream repository at a recorded baseline
commit. Espressif may apply patches to this tree. See [VENDORED_UPSTREAM.md](VENDORED_UPSTREAM.md)
for the baseline commit SHA and the procedure for refreshing against upstream.

For library background and API documentation, refer to the upstream documentation:
https://github.com/dlbeer/dhara/blob/master/README

## Configuration

### `CONFIG_DHARA_RADIX_DEPTH` (default: 32)

Controls the number of bits used to address logical sectors in Dhara's
radix-tree map. This directly determines the size of every metadata record
stored in checkpoint pages:

    DHARA_META_SIZE = 4 + DHARA_RADIX_DEPTH × 4 bytes

**Why reduce it?** The default of 32 supports devices with up to 2³² sectors
(~4 TiB), far beyond any real embedded NAND. For a typical 128 MiB NAND with
2 KiB pages, only 15 bits are needed. Using 15 instead of 32 halves the
checkpoint page overhead and halves the worst-case lookup I/O.

**How to compute the right value** — use the `DHARA_RADIX_DEPTH_FOR()` macro
from `dhara/map.h`:

```c
// max_sectors = num_blocks × pages_per_block / 2
// depth = ceil(log2(max_sectors))
// CONFIG_DHARA_RADIX_DEPTH = DHARA_RADIX_DEPTH_FOR(num_blocks, log2_ppb)
```

| Device size | Page size | Pages per block | Number of blocks | log2(ppb) | Depth |
|-------------|-----------|------------|----------|-------|----|
| 1 Gb (128 MiB) | 2048 B | 64 ppb | 1024 | 6 | 15 |
| 2 Gb (256 MiB) | 2048 B | 64 ppb | 2048 | 6 | 16 |
| 4 Gb (512 MiB) | 2048 B | 64 ppb | 4096 | 6 | 17 |

> ⚠️ **Warning:** Changing this setting after formatting is a **breaking on-flash format change**.
> You must erase and reformat the NAND. Dhara will return an error on `resume()` if the stored
> depth doesn't match the compiled depth.

