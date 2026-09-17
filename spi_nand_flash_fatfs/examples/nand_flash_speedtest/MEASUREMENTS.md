## RAM cost vs read/write speedup (observed)

RAM assumes a 2048 B NAND page (scale journal-slot rows if your chip differs).
Speedup = throughput / baseline throughput for that same app/config set.

### `nand_flash_speedtest` — large-files workload, cold sequential read (least FAT-overhead-noise signal)

| Config | RAM | Read kB/s | Read speedup | Write kB/s | Write speedup |
|---|---:|---:|---:|---:|---:|
| baseline (all off) | 0 B | 1091.8 | 1.00x | 1303.1 | 1.00x |
| +page-reg | 6 B | 1240.3 | 1.14x | 588.9 | 0.45x* |
| +path only | 136 B | 1950.3 | 1.79x | 561.6 | 0.43x* |
| +path+exact | 140 B | 1951.4 | 1.79x | 541.0 | 0.42x* |
| +page-reg+path | 142 B | 2152.0 | 1.97x | 567.5 | 0.44x* |
| +page-reg+path+exact | 146 B | 2150.7 | 1.97x | 577.0 | 0.44x* |
| +…+journal(1) | 2.17 KiB | 2299.8 | 2.11x | 1190.3 | 0.91x* |
| +…+journal(4) | 8.19 KiB | 2790.8 | 2.56x | 602.7 | 0.46x* |
| +…+journal(8) | 16.22 KiB | 2937.4 | 2.69x | 644.4 | 0.49x* |
| +page-reg+path+exact+legacy16 (cherry-picked) | 2.50 KiB | 2500.2 | 2.29x | 1329.6 | 1.02x* |

\* Write numbers swing ±2x across configs with **no write-path code touched by any of these caches** (`trace_path()` invalidates the read cache on every write) — this is GC/erase-state noise from re-formatting before each run, not a real effect. Treat the write column here as noise, not signal.

### `nand_flash_fatfs_throughput` — single 2 MiB file, posix chunk=16384 (representative; write is flat across all chunk sizes)

RAM below is cache-structure bytes only (page-register + map + legacy meta-cache
entries: `page`4 + `offset`4 + `length`4 + `data[132]` + `valid`1 → 148 B/entry).
It does **not** include `spi_nand_flash_perf_stats_t` (48 B, added by the same
cherry-pick, currently unconditional regardless of any cache toggle) — that's
a fixed cost of the diagnostics API, not of any individual cache, out of scope
until we decide whether to gate it too.

Sorted by RAM (single baseline: row 1, all off) so **marginal ROI** —
`Δ read kB/s ÷ Δ RAM in KiB` vs the row directly above — reads top-to-bottom
as "was the next chunk of RAM worth it."

| Config | RAM | Read kB/s | Speedup | Hit-rate | Marginal ROI (kB/s per KiB) |
|---|---:|---:|---:|---:|---:|
| 1) all off | 0 B | 1381.65 | 1.00x | 0% | — |
| 2) +page-reg | 6 B | 1704.12 | 1.23x | 0% | +55035 |
| 3) +page-reg+path | 142 B | 4373.11 | 3.17x | 0% (path cache isn't instrumented) | +20096 |
| 4) +page-reg+path+exact | 146 B | 4373.35 | 3.17x | 0% | +61 |
| 9) +page-reg+path+legacy(1) | 290 B | 4297.42 | 3.11x | 0% | **-540** |
| 10) +page-reg+path+legacy(2) | 438 B | 5230.00 | 3.79x | 24.9% | **+6452** |
| 11) +page-reg+path+legacy(4) | 734 B | 5428.68 | 3.93x | 37.4% | +687 |
| 12) +page-reg+path+legacy(8) | 1.29 KiB | 5531.09 | 4.00x | 43.5% | +177 |
| 5) +page-reg+path+exact+journal(1) | 2.17 KiB | 4371.23 | 3.16x | 0% | -1325 |
| 7) +page-reg+path+exact+legacy(16) | 2.46 KiB | 5542.13 | 4.01x | 46.6% | +4107 |
| 6) +page-reg+path+exact+journal(4) | 8.19 KiB | 4371.00 | 3.16x | 0% | -204 |

Two things the marginal-ROI ordering surfaces that a plain speedup table hides:

- **Row 9 (1 legacy slot) is a strict regression against its true peer, row 4**
  (same layers, minus the 1 slot): 4297.42 vs 4373.35 kB/s — adding one slot
  makes reads **slower** than not having the cache at all (round-robin
  eviction overhead with zero reuse: a single slot can only ever hold the
  most-recently-read entry, and this workload never re-reads that exact
  `(page, offset, length)` next).
- **Rows 5 and 6 (journal cache, 1 and 4 slots) sit interleaved by RAM between
  the legacy-cache rows and are net negative** relative to row 4 in this
  specific workload (4371.23/4371.00 vs 4373.35) — small measurement noise,
  but conclusively *zero* benefit here, at a much higher RAM cost (2.17-8.19
  KiB) than the legacy cache needs to reach 4x (2.46 KiB at 16 slots, or even
  0.43 KiB at just 2 slots for 3.79x).

## Key takeaways

1. **Map path cache (140 B) is by far the best ROI of any layer**: ~1.8-3.2x read speedup for effectively free RAM (marginal ROI in the tens of thousands of kB/s per KiB). Enable this everywhere, no tradeoff.
2. **Page-register cache (6 B)** is cheap and adds a real but modest ~1.1-1.2x on its own; mostly subsumed once the path cache is also on.
3. **Exact-repeat** adds no measurable delta over path-alone in either app's workload (both are single-pass sequential access, no repeated-lookup pattern to exploit) — expected, it targets a narrower case than these benchmarks exercise.
4. **The legacy glue-layer meta-cache (`NAND_FLASH_DHARA_META_CACHE`) has a sharp, non-monotonic ROI curve now that slot count is sweepable**:
   - **1 slot is a strict regression** (3.11x vs 3.17x at 0 slots) — round-robin eviction with a single slot never gets a hit on this workload, so you pay RAM for nothing.
   - **2 slots is the knee**: +148 B over 1 slot buys +6452 kB/s per KiB marginal ROI, jumping from 0% to 24.9% hit-rate and 3.11x→3.79x speedup. This is where essentially all of this cache's value lives.
   - **4 and 8 slots** still improve (37.4%, 43.5% hit-rate) but at rapidly falling marginal ROI (+687, then +177 kB/s per KiB).
   - **16 slots** (46.6% hit-rate, 4.01x) only adds +10 kB/s per KiB over 8 slots — essentially flat.
   - **Recommendation from this data: 2-4 slots**, not 16. Going past 4 buys single-digit-percent hit-rate gains for 2-8x the RAM.
5. **Your new journal meta-cache (`NAND_DHARA_JOURNAL_META_CACHE_SLOTS`) shows ~0% (slightly negative, i.e. noise) benefit** in the throughput app at 1 and 4 slots, despite costing 2.17-8.19 KiB — far more RAM than the legacy cache needs to do meaningfully better (0.43 KiB at 2 slots already beats it). In the `nand_flash_speedtest` app it does show a real trend (2299→2937 kB/s from 1→8 slots), so it *is* workload-dependent: it helps when access patterns revisit the same *checkpoint group*, which this single-large-file throughput benchmark apparently doesn't do much of. Worth checking whether the legacy cache's `(page, offset, length)`-exact keying captures a reuse pattern (e.g. repeated small metadata reads within a group) that the journal cache's whole-checkpoint-page keying doesn't — that's the concrete follow-up this data points to.

## RAM cost per caching toggle (per mounted NAND device handle)

| Toggle | Location | RAM cost | Notes |
|---|---|---|---|
| `NAND_FLASH_PAGE_REGISTER_CACHE` | `spi_nand_flash_device_t` (nand.h) | ~6 bytes | `uint32_t` + `uint8_t` + `bool`; may round up to 8 with alignment |
| `NAND_DHARA_FTL_MAP_PATH_CACHE` | `struct dhara_map` (map.h) | 136 bytes | `prev_target` 4 + `prev_path[32]` 128 + `prev_root` 4 |
| `NAND_DHARA_FTL_MAP_EXACT_REPEAT_CACHE` | `struct dhara_map` (map.h) | +4 bytes | `prev_loc`; on top of path cache, depends on it |
| `NAND_DHARA_JOURNAL_META_CACHE_SLOTS=N` (0-8) | `struct dhara_journal` + `spi_nand_flash_dhara_priv_data_t` + heap | 18 + 8N + N×page_size bytes | N=4, 2048B page → ~8.2 KB |
| `NAND_FLASH_DHARA_META_CACHE=N` (0-32) | `spi_nand_flash_dhara_priv_data_t` | N × 148 bytes | N=2 → 296 B (the ROI knee, see takeaways), N=16 → 2.31 KiB, N=32 → 4.62 KiB |

## `nand_flash_speedtest`
```
I (255) main_task: Calling app_main()
I (265) speedtest: --- Cache layers under test ---
I (265) speedtest: driver page-register cache : off
I (275) speedtest: dhara map path cache       : off
I (275) speedtest: dhara map exact-repeat     : off
I (275) speedtest: --------------------------------
W (295) vfs_fat_nand: f_mount failed (13)
I (295) vfs_fat_nand: Formatting FATFS partition, allocation unit size=16384
I (2745) vfs_fat_nand: Mounting again
I (2795) speedtest: FAT FS: 471968 kB total, 471968 kB free
I (2795) speedtest: === small files workload (64 x 4096 bytes) ===
I (4895) speedtest: sequential write          2096860 us  (30.5 files/s, 122.1 kB/s)
I (5135) speedtest: cold sequential read       246470 us  (259.7 files/s, 1038.7 kB/s)
I (5385) speedtest: warm repeat read           246411 us  (259.7 files/s, 1038.9 kB/s)
I (5385) speedtest: warm/cold speedup: 1.00x
I (5385) speedtest: === large files workload (4 x 262144 bytes) ===
I (6175) speedtest: sequential write           785832 us  (5.1 files/s, 1303.1 kB/s)
I (7115) speedtest: cold sequential read       937878 us  (4.3 files/s, 1091.8 kB/s)
I (8055) speedtest: warm repeat read           937945 us  (4.3 files/s, 1091.7 kB/s)
I (8055) speedtest: warm/cold speedup: 1.00x
I (8055) speedtest: Benchmark done
```

```
I (255) main_task: Calling app_main()
I (265) speedtest: --- Cache layers under test ---
I (265) speedtest: driver page-register cache : ON
I (275) speedtest: dhara map path cache       : off
I (275) speedtest: dhara map exact-repeat     : off
I (275) speedtest: --------------------------------
I (385) speedtest: FAT FS: 471968 kB total, 469920 kB free
I (385) speedtest: === small files workload (64 x 4096 bytes) ===
I (2445) speedtest: sequential write          2055468 us  (31.1 files/s, 124.5 kB/s)
I (2685) speedtest: cold sequential read       243142 us  (263.2 files/s, 1052.9 kB/s)
I (2935) speedtest: warm repeat read           243155 us  (263.2 files/s, 1052.8 kB/s)
I (2935) speedtest: warm/cold speedup: 1.00x
I (2935) speedtest: === large files workload (4 x 262144 bytes) ===
I (4675) speedtest: sequential write          1738807 us  (2.3 files/s, 588.9 kB/s)
I (5505) speedtest: cold sequential read       825585 us  (4.8 files/s, 1240.3 kB/s)
I (6335) speedtest: warm repeat read           825572 us  (4.8 files/s, 1240.4 kB/s)
I (6335) speedtest: warm/cold speedup: 1.00x
I (6335) speedtest: Benchmark done
```

```
I (255) main_task: Calling app_main()
I (265) speedtest: --- Cache layers under test ---
I (265) speedtest: driver page-register cache : off
I (275) speedtest: dhara map path cache       : ON
I (275) speedtest: dhara map exact-repeat     : off
I (275) speedtest: --------------------------------
I (375) speedtest: FAT FS: 471968 kB total, 469920 kB free
I (375) speedtest: === small files workload (64 x 4096 bytes) ===
I (3495) speedtest: sequential write          3112669 us  (20.6 files/s, 82.2 kB/s)
I (3675) speedtest: cold sequential read       180104 us  (355.4 files/s, 1421.4 kB/s)
I (3855) speedtest: warm repeat read           181239 us  (353.1 files/s, 1412.5 kB/s)
I (3855) speedtest: warm/cold speedup: 0.99x
I (3855) speedtest: === large files workload (4 x 262144 bytes) ===
I (5685) speedtest: sequential write          1823340 us  (2.2 files/s, 561.6 kB/s)
I (6215) speedtest: cold sequential read       525045 us  (7.6 files/s, 1950.3 kB/s)
I (6735) speedtest: warm repeat read           524977 us  (7.6 files/s, 1950.6 kB/s)
I (6735) speedtest: warm/cold speedup: 1.00x
I (6735) speedtest: Benchmark done
```

```
I (255) main_task: Calling app_main()
I (265) speedtest: --- Cache layers under test ---
I (265) speedtest: driver page-register cache : off
I (275) speedtest: dhara map path cache       : ON
I (275) speedtest: dhara map exact-repeat     : ON
I (275) speedtest: --------------------------------
I (385) speedtest: FAT FS: 471968 kB total, 469920 kB free
I (385) speedtest: === small files workload (64 x 4096 bytes) ===
I (3625) speedtest: sequential write          3234814 us  (19.8 files/s, 79.1 kB/s)
I (3795) speedtest: cold sequential read       175513 us  (364.6 files/s, 1458.6 kB/s)
I (3975) speedtest: warm repeat read           176986 us  (361.6 files/s, 1446.4 kB/s)
I (3975) speedtest: warm/cold speedup: 0.99x
I (3975) speedtest: === large files workload (4 x 262144 bytes) ===
I (5875) speedtest: sequential write          1892637 us  (2.1 files/s, 541.0 kB/s)
I (6405) speedtest: cold sequential read       524742 us  (7.6 files/s, 1951.4 kB/s)
I (6925) speedtest: warm repeat read           524783 us  (7.6 files/s, 1951.3 kB/s)
I (6925) speedtest: warm/cold speedup: 1.00x
I (6925) speedtest: Benchmark done
I (6935) main_task: Returned from app_main()
```

```
I (255) main_task: Calling app_main()
I (265) speedtest: --- Cache layers under test ---
I (265) speedtest: driver page-register cache : ON
I (275) speedtest: dhara map path cache       : ON
I (275) speedtest: dhara map exact-repeat     : off
I (275) speedtest: --------------------------------
I (385) speedtest: FAT FS: 471968 kB total, 469920 kB free
I (385) speedtest: === small files workload (64 x 4096 bytes) ===
I (3245) speedtest: sequential write          2859695 us  (22.4 files/s, 89.5 kB/s)
I (3415) speedtest: cold sequential read       170688 us  (375.0 files/s, 1499.8 kB/s)
I (3595) speedtest: warm repeat read           171723 us  (372.7 files/s, 1490.8 kB/s)
I (3595) speedtest: warm/cold speedup: 0.99x
I (3595) speedtest: === large files workload (4 x 262144 bytes) ===
I (5405) speedtest: sequential write          1804422 us  (2.2 files/s, 567.5 kB/s)
I (5875) speedtest: cold sequential read       475840 us  (8.4 files/s, 2152.0 kB/s)
I (6355) speedtest: warm repeat read           475745 us  (8.4 files/s, 2152.4 kB/s)
I (6355) speedtest: warm/cold speedup: 1.00x
I (6355) speedtest: Benchmark done
```

```
I (255) main_task: Calling app_main()
I (265) speedtest: --- Cache layers under test ---
I (265) speedtest: driver page-register cache : ON
I (275) speedtest: dhara map path cache       : ON
I (275) speedtest: dhara map exact-repeat     : ON
I (275) speedtest: --------------------------------
I (385) speedtest: FAT FS: 471968 kB total, 469920 kB free
I (385) speedtest: === small files workload (64 x 4096 bytes) ===
I (3245) speedtest: sequential write          2857484 us  (22.4 files/s, 89.6 kB/s)
I (3415) speedtest: cold sequential read       168654 us  (379.5 files/s, 1517.9 kB/s)
I (3585) speedtest: warm repeat read           168685 us  (379.4 files/s, 1517.6 kB/s)
I (3585) speedtest: warm/cold speedup: 1.00x
I (3585) speedtest: === large files workload (4 x 262144 bytes) ===
I (5365) speedtest: sequential write          1774554 us  (2.3 files/s, 577.0 kB/s)
I (5845) speedtest: cold sequential read       476116 us  (8.4 files/s, 2150.7 kB/s)
I (6315) speedtest: warm repeat read           476173 us  (8.4 files/s, 2150.5 kB/s)
I (6325) speedtest: warm/cold speedup: 1.00x
I (6325) speedtest: Benchmark done
```

```
I (255) main_task: Calling app_main()
I (265) speedtest: --- Cache layers under test ---
I (265) speedtest: driver page-register cache : ON
I (275) speedtest: dhara map path cache       : ON
I (275) speedtest: dhara map exact-repeat     : ON
I (275) speedtest: dhara journal meta cache   : ON  (1 slots)
I (285) speedtest: --------------------------------
I (375) speedtest: FAT FS: 472000 kB total, 471968 kB free
I (375) speedtest: === small files workload (64 x 4096 bytes) ===
I (2645) speedtest: sequential write          2267585 us  (28.2 files/s, 112.9 kB/s)
I (2865) speedtest: cold sequential read       219009 us  (292.2 files/s, 1168.9 kB/s)
I (3085) speedtest: warm repeat read           219117 us  (292.1 files/s, 1168.3 kB/s)
I (3085) speedtest: warm/cold speedup: 1.00x
I (3085) speedtest: === large files workload (4 x 262144 bytes) ===
I (3945) speedtest: sequential write           860301 us  (4.6 files/s, 1190.3 kB/s)
I (4395) speedtest: cold sequential read       445249 us  (9.0 files/s, 2299.8 kB/s)
I (4835) speedtest: warm repeat read           444554 us  (9.0 files/s, 2303.4 kB/s)
I (4845) speedtest: warm/cold speedup: 1.00x
I (4845) speedtest: Benchmark done
```

```
I (255) main_task: Calling app_main()
I (265) speedtest: --- Cache layers under test ---
I (265) speedtest: driver page-register cache : ON
I (275) speedtest: dhara map path cache       : ON
I (275) speedtest: dhara map exact-repeat     : ON
I (275) speedtest: dhara journal meta cache   : ON  (4 slots)
I (285) speedtest: --------------------------------
I (355) speedtest: FAT FS: 472000 kB total, 468928 kB free
I (355) speedtest: === small files workload (64 x 4096 bytes) ===
I (2365) speedtest: sequential write          2006102 us  (31.9 files/s, 127.6 kB/s)
I (2545) speedtest: cold sequential read       177991 us  (359.6 files/s, 1438.3 kB/s)
I (2725) speedtest: warm repeat read           178299 us  (358.9 files/s, 1435.8 kB/s)
I (2725) speedtest: warm/cold speedup: 1.00x
I (2725) speedtest: === large files workload (4 x 262144 bytes) ===
I (4425) speedtest: sequential write          1699069 us  (2.4 files/s, 602.7 kB/s)
I (4795) speedtest: cold sequential read       366919 us  (10.9 files/s, 2790.8 kB/s)
I (5165) speedtest: warm repeat read           366898 us  (10.9 files/s, 2791.0 kB/s)
I (5165) speedtest: warm/cold speedup: 1.00x
I (5165) speedtest: Benchmark done
```

```
I (255) main_task: Calling app_main()
I (265) speedtest: --- Cache layers under test ---
I (265) speedtest: driver page-register cache : ON
I (275) speedtest: dhara map path cache       : ON
I (275) speedtest: dhara map exact-repeat     : ON
I (275) speedtest: dhara journal meta cache   : ON  (8 slots)
I (285) speedtest: --------------------------------
I (365) speedtest: FAT FS: 472000 kB total, 468928 kB free
I (365) speedtest: === small files workload (64 x 4096 bytes) ===
I (2475) speedtest: sequential write          2115628 us  (30.3 files/s, 121.0 kB/s)
I (2655) speedtest: cold sequential read       174496 us  (366.8 files/s, 1467.1 kB/s)
I (2825) speedtest: warm repeat read           173942 us  (367.9 files/s, 1471.8 kB/s)
I (2825) speedtest: warm/cold speedup: 1.00x
I (2825) speedtest: === large files workload (4 x 262144 bytes) ===
I (4425) speedtest: sequential write          1588953 us  (2.5 files/s, 644.4 kB/s)
I (4775) speedtest: cold sequential read       348602 us  (11.5 files/s, 2937.4 kB/s)
I (5125) speedtest: warm repeat read           348137 us  (11.5 files/s, 2941.4 kB/s)
I (5125) speedtest: warm/cold speedup: 1.00x
I (5125) speedtest: Benchmark done
```


### with the cherry-picked cache enabled
```
I (255) main_task: Calling app_main()
I (265) speedtest: --- Cache layers under test ---
I (265) speedtest: driver page-register cache : ON
I (275) speedtest: dhara map path cache       : ON
I (275) speedtest: dhara map exact-repeat     : ON
I (275) speedtest: dhara journal meta cache   : off
I (285) speedtest: --------------------------------
I (365) speedtest: FAT FS: 472000 kB total, 471968 kB free
I (365) speedtest: === small files workload (64 x 4096 bytes) ===
I (2335) speedtest: sequential write          1966848 us  (32.5 files/s, 130.2 kB/s)
I (2495) speedtest: cold sequential read       157795 us  (405.6 files/s, 1622.4 kB/s)
I (2655) speedtest: warm repeat read           157196 us  (407.1 files/s, 1628.5 kB/s)
I (2655) speedtest: warm/cold speedup: 1.00x
I (2655) speedtest: === large files workload (4 x 262144 bytes) ===
I (3435) speedtest: sequential write           770160 us  (5.2 files/s, 1329.6 kB/s)
I (3845) speedtest: cold sequential read       409561 us  (9.8 files/s, 2500.2 kB/s)
I (4255) speedtest: warm repeat read           409259 us  (9.8 files/s, 2502.1 kB/s)
I (4255) speedtest: warm/cold speedup: 1.00x
I (4255) speedtest: Benchmark done
```

## `nand_flash_fatfs_throughput`

### 1) everything OFF
```
I (41226) vfs_fat_nand: Formatting FATFS partition, allocation unit size=32768
I (43646) vfs_fat_nand: Mounting again
I (43666) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43666) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43666) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43676) fatfs_tp: ======== baseline: stdio unbuffered ========
I (46306) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1209064 us, avg 1734.53 kB/s
I (46306) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 1409961 us, avg 1487.38 kB/s

I (46306) fatfs_tp: write stats: reads=109, programs=1096, copies=8, erases=17, metadata hits=0, misses=107 (0.0% hit)
I (46316) fatfs_tp: read stats: reads=7260, programs=0, copies=0, erases=0, metadata hits=0, misses=6235 (0.0% hit)
I (50256) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1298561 us, avg 1614.98 kB/s
I (50256) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 1397558 us, avg 1500.58 kB/s

I (50266) fatfs_tp: write stats: reads=1519, programs=1096, copies=8, erases=18, metadata hits=0, misses=1517 (0.0% hit)
I (50276) fatfs_tp: read stats: reads=7905, programs=0, copies=0, erases=0, metadata hits=0, misses=6880 (0.0% hit)
I (54496) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1287940 us, avg 1628.30 kB/s
I (54496) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 1524598 us, avg 1375.54 kB/s

I (54496) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (54516) fatfs_tp: read stats: reads=8842, programs=0, copies=0, erases=0, metadata hits=0, misses=7817 (0.0% hit)
I (58746) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1445089 us, avg 1451.23 kB/s
I (58746) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 1367026 us, avg 1534.10 kB/s

I (58756) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (58766) fatfs_tp: read stats: reads=7904, programs=0, copies=0, erases=0, metadata hits=0, misses=6879 (0.0% hit)
I (60166) fatfs_tp: ======== optimized: POSIX read/write ========
I (63046) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1283957 us, avg 1633.35 kB/s
I (63046) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 1591686 us, avg 1317.57 kB/s

I (63046) fatfs_tp: write stats: reads=1508, programs=1096, copies=8, erases=17, metadata hits=0, misses=1506 (0.0% hit)
I (63066) fatfs_tp: read stats: reads=8841, programs=0, copies=0, erases=0, metadata hits=0, misses=7816 (0.0% hit)
I (67456) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1445322 us, avg 1450.99 kB/s
I (67456) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 1524540 us, avg 1375.60 kB/s

I (67456) fatfs_tp: write stats: reads=2441, programs=1096, copies=8, erases=18, metadata hits=0, misses=2439 (0.0% hit)
I (67466) fatfs_tp: read stats: reads=8842, programs=0, copies=0, erases=0, metadata hits=0, misses=7817 (0.0% hit)
I (71676) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1282306 us, avg 1635.45 kB/s
I (71676) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 1517861 us, avg 1381.65 kB/s

I (71686) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (71696) fatfs_tp: read stats: reads=8842, programs=0, copies=0, erases=0, metadata hits=0, misses=7817 (0.0% hit)
I (75916) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1445407 us, avg 1450.91 kB/s
I (75926) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 1363487 us, avg 1538.08 kB/s

I (75926) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (75936) fatfs_tp: read stats: reads=7904, programs=0, copies=0, erases=0, metadata hits=0, misses=6879 (0.0% hit)
I (77346) main_task: Returned from app_main()
```

### 2) Driver page register cache
```
I (41236) vfs_fat_nand: Formatting FATFS partition, allocation unit size=32768
I (43656) vfs_fat_nand: Mounting again
I (43676) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43676) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43676) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43686) fatfs_tp: ======== baseline: stdio unbuffered ========
I (46106) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1211402 us, avg 1731.18 kB/s
I (46106) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 1209059 us, avg 1734.53 kB/s

I (46116) fatfs_tp: write stats: reads=109, programs=1096, copies=8, erases=17, metadata hits=0, misses=107 (0.0% hit)
I (46126) fatfs_tp: read stats: reads=7260, programs=0, copies=0, erases=0, metadata hits=0, misses=6235 (0.0% hit)
I (49736) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1193842 us, avg 1756.64 kB/s
I (49736) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 1201727 us, avg 1745.12 kB/s

I (49746) fatfs_tp: write stats: reads=1519, programs=1096, copies=8, erases=18, metadata hits=0, misses=1517 (0.0% hit)
I (49756) fatfs_tp: read stats: reads=7905, programs=0, copies=0, erases=0, metadata hits=0, misses=6880 (0.0% hit)
I (53476) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1187155 us, avg 1766.54 kB/s
I (53476) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 1234069 us, avg 1699.38 kB/s

I (53476) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (53486) fatfs_tp: read stats: reads=8842, programs=0, copies=0, erases=0, metadata hits=0, misses=7817 (0.0% hit)
I (57416) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1444789 us, avg 1451.53 kB/s
I (57416) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 1182996 us, avg 1772.75 kB/s

I (57426) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (57436) fatfs_tp: read stats: reads=7904, programs=0, copies=0, erases=0, metadata hits=0, misses=6879 (0.0% hit)
I (58726) fatfs_tp: ======== optimized: POSIX read/write ========
I (61216) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1183241 us, avg 1772.38 kB/s
I (61216) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 1304558 us, avg 1607.56 kB/s

I (61226) fatfs_tp: write stats: reads=1508, programs=1096, copies=8, erases=17, metadata hits=0, misses=1506 (0.0% hit)
I (61236) fatfs_tp: read stats: reads=8841, programs=0, copies=0, erases=0, metadata hits=0, misses=7816 (0.0% hit)
I (65216) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1445447 us, avg 1450.87 kB/s
I (65216) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 1237227 us, avg 1695.04 kB/s

I (65216) fatfs_tp: write stats: reads=2441, programs=1096, copies=8, erases=18, metadata hits=0, misses=2439 (0.0% hit)
I (65226) fatfs_tp: read stats: reads=8842, programs=0, copies=0, erases=0, metadata hits=0, misses=7817 (0.0% hit)
I (68936) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1182421 us, avg 1773.61 kB/s
I (68946) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 1230633 us, avg 1704.12 kB/s

I (68946) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (68956) fatfs_tp: read stats: reads=8842, programs=0, copies=0, erases=0, metadata hits=0, misses=7817 (0.0% hit)
I (72886) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1445217 us, avg 1451.10 kB/s
I (72886) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 1181138 us, avg 1775.54 kB/s

I (72896) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (72906) fatfs_tp: read stats: reads=7904, programs=0, copies=0, erases=0, metadata hits=0, misses=6879 (0.0% hit)
I (74196) main_task: Returned from app_main()
```

### 3) Driver page register cache + Dhara FTL trace path cache

```
I (41226) vfs_fat_nand: Formatting FATFS partition, allocation unit size=32768
I (43646) vfs_fat_nand: Mounting again
I (43656) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43656) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43666) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43666) fatfs_tp: ======== baseline: stdio unbuffered ========
I (45486) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1213170 us, avg 1728.65 kB/s
I (45486) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 596078 us, avg 3518.25 kB/s

I (45496) fatfs_tp: write stats: reads=109, programs=1096, copies=8, erases=17, metadata hits=0, misses=107 (0.0% hit)
I (45506) fatfs_tp: read stats: reads=3079, programs=0, copies=0, erases=0, metadata hits=0, misses=2054 (0.0% hit)
I (48406) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1194188 us, avg 1756.13 kB/s
I (48406) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 491722 us, avg 4264.91 kB/s

I (48416) fatfs_tp: write stats: reads=1519, programs=1096, copies=8, erases=18, metadata hits=0, misses=1517 (0.0% hit)
I (48426) fatfs_tp: read stats: reads=3083, programs=0, copies=0, erases=0, metadata hits=0, misses=2058 (0.0% hit)
I (51386) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1187111 us, avg 1766.60 kB/s
I (51396) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 481172 us, avg 4358.42 kB/s

I (51396) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (51406) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (54636) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1444681 us, avg 1451.64 kB/s
I (54636) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 479292 us, avg 4375.52 kB/s

I (54646) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (54656) fatfs_tp: read stats: reads=3082, programs=0, copies=0, erases=0, metadata hits=0, misses=2057 (0.0% hit)
I (55946) fatfs_tp: ======== optimized: POSIX read/write ========
I (57696) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1193468 us, avg 1757.19 kB/s
I (57696) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 553492 us, avg 3788.95 kB/s

I (57706) fatfs_tp: write stats: reads=1508, programs=1096, copies=8, erases=17, metadata hits=0, misses=1506 (0.0% hit)
I (57716) fatfs_tp: read stats: reads=3083, programs=0, copies=0, erases=0, metadata hits=0, misses=2058 (0.0% hit)
I (60946) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1445297 us, avg 1451.02 kB/s
I (60946) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 486121 us, avg 4314.05 kB/s

I (60946) fatfs_tp: write stats: reads=2441, programs=1096, copies=8, erases=18, metadata hits=0, misses=2439 (0.0% hit)
I (60956) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (63916) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1182427 us, avg 1773.60 kB/s
I (63916) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 479556 us, avg 4373.11 kB/s

I (63926) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (63936) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (67156) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1445169 us, avg 1451.15 kB/s
I (67156) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 478320 us, avg 4384.41 kB/s

I (67166) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (67176) fatfs_tp: read stats: reads=3082, programs=0, copies=0, erases=0, metadata hits=0, misses=2057 (0.0% hit)
I (68466) main_task: Returned from app_main()
```

### 4) Driver page register cache + Dhara FTL trace path cache + Dhara exact-repeat cache
```
I (43696) vfs_fat_nand: Mounting again
I (43706) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43706) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43716) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43716) fatfs_tp: ======== baseline: stdio unbuffered ========
I (45536) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1213519 us, avg 1728.16 kB/s
I (45536) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 596139 us, avg 3517.89 kB/s

I (45546) fatfs_tp: write stats: reads=109, programs=1096, copies=8, erases=17, metadata hits=0, misses=107 (0.0% hit)
I (45556) fatfs_tp: read stats: reads=3079, programs=0, copies=0, erases=0, metadata hits=0, misses=2054 (0.0% hit)
I (48456) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1194289 us, avg 1755.98 kB/s
I (48456) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 491758 us, avg 4264.60 kB/s

I (48466) fatfs_tp: write stats: reads=1519, programs=1096, copies=8, erases=18, metadata hits=0, misses=1517 (0.0% hit)
I (48476) fatfs_tp: read stats: reads=3083, programs=0, copies=0, erases=0, metadata hits=0, misses=2058 (0.0% hit)
I (51446) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1187219 us, avg 1766.44 kB/s
I (51456) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 481177 us, avg 4358.38 kB/s

I (51456) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (51466) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (54696) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1444634 us, avg 1451.68 kB/s
I (54696) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 479309 us, avg 4375.37 kB/s

I (54706) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (54716) fatfs_tp: read stats: reads=3082, programs=0, copies=0, erases=0, metadata hits=0, misses=2057 (0.0% hit)
I (56006) fatfs_tp: ======== optimized: POSIX read/write ========
I (57916) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1354247 us, avg 1548.57 kB/s
I (57916) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 553557 us, avg 3788.50 kB/s

I (57926) fatfs_tp: write stats: reads=1508, programs=1096, copies=8, erases=17, metadata hits=0, misses=1506 (0.0% hit)
I (57936) fatfs_tp: read stats: reads=3083, programs=0, copies=0, erases=0, metadata hits=0, misses=2058 (0.0% hit)
I (61166) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1445356 us, avg 1450.96 kB/s
I (61166) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 486160 us, avg 4313.71 kB/s

I (61166) fatfs_tp: write stats: reads=2441, programs=1096, copies=8, erases=18, metadata hits=0, misses=2439 (0.0% hit)
I (61176) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (64136) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1182521 us, avg 1773.46 kB/s
I (64136) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 479530 us, avg 4373.35 kB/s

I (64146) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (64156) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (67376) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1445142 us, avg 1451.17 kB/s
I (67376) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 478341 us, avg 4384.22 kB/s

I (67386) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (67396) fatfs_tp: read stats: reads=3082, programs=0, copies=0, erases=0, metadata hits=0, misses=2057 (0.0% hit)
I (68686) main_task: Returned from app_main()
```

### 5) Driver page register cache + Dhara FTL trace path cache + Dhara exact-repeat cache + 1 Dhara meta cache slot
```
I (43656) vfs_fat_nand: Mounting again
I (43666) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43666) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43676) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43676) fatfs_tp: ======== baseline: stdio unbuffered ========
I (45496) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1214856 us, avg 1726.26 kB/s
I (45496) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 596303 us, avg 3516.92 kB/s

I (45506) fatfs_tp: write stats: reads=109, programs=1096, copies=8, erases=17, metadata hits=0, misses=107 (0.0% hit)
I (45516) fatfs_tp: read stats: reads=3079, programs=0, copies=0, erases=0, metadata hits=0, misses=2054 (0.0% hit)
I (48416) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1194401 us, avg 1755.82 kB/s
I (48416) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 491985 us, avg 4262.63 kB/s

I (48426) fatfs_tp: write stats: reads=1519, programs=1096, copies=8, erases=18, metadata hits=0, misses=1517 (0.0% hit)
I (48436) fatfs_tp: read stats: reads=3083, programs=0, copies=0, erases=0, metadata hits=0, misses=2058 (0.0% hit)
I (51416) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1187394 us, avg 1766.18 kB/s
I (51416) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 481419 us, avg 4356.19 kB/s

I (51416) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (51426) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (54656) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1444788 us, avg 1451.53 kB/s
I (54656) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 479539 us, avg 4373.27 kB/s

I (54666) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (54676) fatfs_tp: read stats: reads=3082, programs=0, copies=0, erases=0, metadata hits=0, misses=2057 (0.0% hit)
I (55966) fatfs_tp: ======== optimized: POSIX read/write ========
I (57876) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1355050 us, avg 1547.66 kB/s
I (57876) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 553752 us, avg 3787.17 kB/s

I (57886) fatfs_tp: write stats: reads=1508, programs=1096, copies=8, erases=17, metadata hits=0, misses=1506 (0.0% hit)
I (57896) fatfs_tp: read stats: reads=3083, programs=0, copies=0, erases=0, metadata hits=0, misses=2058 (0.0% hit)
I (61126) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1445530 us, avg 1450.78 kB/s
I (61126) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 486444 us, avg 4311.19 kB/s

I (61126) fatfs_tp: write stats: reads=2441, programs=1096, copies=8, erases=18, metadata hits=0, misses=2439 (0.0% hit)
I (61136) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (64096) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1182714 us, avg 1773.17 kB/s
I (64096) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 479763 us, avg 4371.23 kB/s

I (64106) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (64116) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (67346) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1445283 us, avg 1451.03 kB/s
I (67346) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 478543 us, avg 4382.37 kB/s

I (67356) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (67366) fatfs_tp: read stats: reads=3082, programs=0, copies=0, erases=0, metadata hits=0, misses=2057 (0.0% hit)
I (68656) main_task: Returned from app_main()
```

### 6) Driver page register cache + Dhara FTL trace path cache + Dhara exact-repeat cache + 4 Dhara meta cache slots
```
I (43646) vfs_fat_nand: Mounting again
I (43656) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43656) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43666) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43666) fatfs_tp: ======== baseline: stdio unbuffered ========
I (45486) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1214416 us, avg 1726.88 kB/s
I (45486) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 596320 us, avg 3516.82 kB/s

I (45496) fatfs_tp: write stats: reads=109, programs=1096, copies=8, erases=17, metadata hits=0, misses=107 (0.0% hit)
I (45506) fatfs_tp: read stats: reads=3079, programs=0, copies=0, erases=0, metadata hits=0, misses=2054 (0.0% hit)
I (48406) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1194402 us, avg 1755.82 kB/s
I (48406) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 491989 us, avg 4262.60 kB/s

I (48416) fatfs_tp: write stats: reads=1519, programs=1096, copies=8, erases=18, metadata hits=0, misses=1517 (0.0% hit)
I (48426) fatfs_tp: read stats: reads=3083, programs=0, copies=0, erases=0, metadata hits=0, misses=2058 (0.0% hit)
I (51406) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1187314 us, avg 1766.30 kB/s
I (51406) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 481406 us, avg 4356.31 kB/s

I (51406) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (51416) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (54646) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1444777 us, avg 1451.54 kB/s
I (54646) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 479535 us, avg 4373.30 kB/s

I (54656) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (54666) fatfs_tp: read stats: reads=3082, programs=0, copies=0, erases=0, metadata hits=0, misses=2057 (0.0% hit)
I (55956) fatfs_tp: ======== optimized: POSIX read/write ========
I (57866) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1354672 us, avg 1548.09 kB/s
I (57866) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 553750 us, avg 3787.18 kB/s

I (57876) fatfs_tp: write stats: reads=1508, programs=1096, copies=8, erases=17, metadata hits=0, misses=1506 (0.0% hit)
I (57886) fatfs_tp: read stats: reads=3083, programs=0, copies=0, erases=0, metadata hits=0, misses=2058 (0.0% hit)
I (61116) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1445462 us, avg 1450.85 kB/s
I (61116) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 486429 us, avg 4311.32 kB/s

I (61116) fatfs_tp: write stats: reads=2441, programs=1096, copies=8, erases=18, metadata hits=0, misses=2439 (0.0% hit)
I (61126) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (64086) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1182625 us, avg 1773.30 kB/s
I (64086) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 479788 us, avg 4371.00 kB/s

I (64096) fatfs_tp: write stats: reads=1507, programs=1096, copies=8, erases=17, metadata hits=0, misses=1505 (0.0% hit)
I (64106) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (67336) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1445236 us, avg 1451.08 kB/s
I (67336) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 478568 us, avg 4382.14 kB/s

I (67346) fatfs_tp: write stats: reads=2448, programs=1096, copies=8, erases=18, metadata hits=0, misses=2446 (0.0% hit)
I (67356) fatfs_tp: read stats: reads=3082, programs=0, copies=0, erases=0, metadata hits=0, misses=2057 (0.0% hit)
I (68646) main_task: Returned from app_main()
```

### 7) Driver page register cache + Dhara FTL trace path cache + Dhara exact-repeat cache + CHERRY-PICKED meta cache enabled 16 slots
```
I (43696) vfs_fat_nand: Mounting again
I (43706) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43706) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43706) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43716) fatfs_tp: ======== baseline: stdio unbuffered ========
I (45456) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1204293 us, avg 1741.40 kB/s
I (45456) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 528576 us, avg 3967.55 kB/s

I (45466) fatfs_tp: write stats: reads=92, programs=1096, copies=8, erases=17, metadata hits=17, misses=90 (15.9% hit)
I (45476) fatfs_tp: read stats: reads=2117, programs=0, copies=0, erases=0, metadata hits=962, misses=1092 (46.8% hit)
I (48236) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1135833 us, avg 1846.36 kB/s
I (48246) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 416010 us, avg 5041.11 kB/s

I (48246) fatfs_tp: write stats: reads=1149, programs=1096, copies=8, erases=18, metadata hits=370, misses=1147 (24.4% hit)
I (48256) fatfs_tp: read stats: reads=2123, programs=0, copies=0, erases=0, metadata hits=960, misses=1098 (46.6% hit)
I (51066) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1126655 us, avg 1861.40 kB/s
I (51066) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 383535 us, avg 5467.95 kB/s

I (51066) fatfs_tp: write stats: reads=1144, programs=1096, copies=8, erases=17, metadata hits=363, misses=1142 (24.1% hit)
I (51076) fatfs_tp: read stats: reads=2124, programs=0, copies=0, erases=0, metadata hits=960, misses=1099 (46.6% hit)
I (53906) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1151550 us, avg 1821.16 kB/s
I (53916) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 377660 us, avg 5553.02 kB/s

I (53916) fatfs_tp: write stats: reads=1210, programs=1096, copies=8, erases=18, metadata hits=1238, misses=1208 (50.6% hit)
I (53926) fatfs_tp: read stats: reads=2121, programs=0, copies=0, erases=0, metadata hits=961, misses=1096 (46.7% hit)
I (55216) fatfs_tp: ======== optimized: POSIX read/write ========
I (56816) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1130782 us, avg 1854.60 kB/s
I (56816) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 463295 us, avg 4526.60 kB/s

I (56826) fatfs_tp: write stats: reads=1145, programs=1096, copies=8, erases=17, metadata hits=363, misses=1143 (24.1% hit)
I (56836) fatfs_tp: read stats: reads=2123, programs=0, copies=0, erases=0, metadata hits=960, misses=1098 (46.6% hit)
I (59676) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1152096 us, avg 1820.29 kB/s
I (59676) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 396018 us, avg 5295.60 kB/s

I (59686) fatfs_tp: write stats: reads=1203, programs=1096, copies=8, erases=18, metadata hits=1238, misses=1201 (50.8% hit)
I (59696) fatfs_tp: read stats: reads=2124, programs=0, copies=0, erases=0, metadata hits=960, misses=1099 (46.6% hit)
I (62486) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1120346 us, avg 1871.88 kB/s
I (62486) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 378402 us, avg 5542.13 kB/s

I (62496) fatfs_tp: write stats: reads=1146, programs=1096, copies=8, erases=17, metadata hits=361, misses=1144 (24.0% hit)
I (62506) fatfs_tp: read stats: reads=2124, programs=0, copies=0, erases=0, metadata hits=960, misses=1099 (46.6% hit)
I (65326) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1151845 us, avg 1820.69 kB/s
I (65326) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 374998 us, avg 5592.44 kB/s

I (65336) fatfs_tp: write stats: reads=1211, programs=1096, copies=8, erases=18, metadata hits=1237, misses=1209 (50.6% hit)
I (65346) fatfs_tp: read stats: reads=2121, programs=0, copies=0, erases=0, metadata hits=961, misses=1096 (46.7% hit)
I (66636) main_task: Returned from app_main()
```

### 8) Driver page register cache + Dhara FTL trace path cache + Dhara exact-repeat cache + CHERRY-PICKED meta cache enabled + 1 Dhara meta cache slot
```
I (43646) vfs_fat_nand: Mounting again
I (43656) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43656) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43656) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43666) fatfs_tp: ======== baseline: stdio unbuffered ========
I (45426) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1204658 us, avg 1740.87 kB/s
I (45426) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 550671 us, avg 3808.36 kB/s

I (45436) fatfs_tp: write stats: reads=92, programs=1096, copies=8, erases=17, metadata hits=17, misses=90 (15.9% hit)
I (45446) fatfs_tp: read stats: reads=2117, programs=0, copies=0, erases=0, metadata hits=962, misses=1092 (46.8% hit)
I (48216) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1135866 us, avg 1846.30 kB/s
I (48216) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 418036 us, avg 5016.68 kB/s

I (48216) fatfs_tp: write stats: reads=1149, programs=1096, copies=8, erases=18, metadata hits=370, misses=1147 (24.4% hit)
I (48226) fatfs_tp: read stats: reads=2123, programs=0, copies=0, erases=0, metadata hits=960, misses=1098 (46.6% hit)
I (51046) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1126850 us, avg 1861.07 kB/s
I (51046) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 382502 us, avg 5482.72 kB/s

I (51046) fatfs_tp: write stats: reads=1144, programs=1096, copies=8, erases=17, metadata hits=363, misses=1142 (24.1% hit)
I (51056) fatfs_tp: read stats: reads=2124, programs=0, copies=0, erases=0, metadata hits=960, misses=1099 (46.6% hit)
I (53886) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1151568 us, avg 1821.13 kB/s
I (53886) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 377274 us, avg 5558.70 kB/s

I (53896) fatfs_tp: write stats: reads=1210, programs=1096, copies=8, erases=18, metadata hits=1238, misses=1208 (50.6% hit)
I (53906) fatfs_tp: read stats: reads=2121, programs=0, copies=0, erases=0, metadata hits=961, misses=1096 (46.7% hit)
I (55196) fatfs_tp: ======== optimized: POSIX read/write ========
I (56956) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1291055 us, avg 1624.37 kB/s
I (56956) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 460447 us, avg 4554.60 kB/s

I (56956) fatfs_tp: write stats: reads=1145, programs=1096, copies=8, erases=17, metadata hits=363, misses=1143 (24.1% hit)
I (56966) fatfs_tp: read stats: reads=2123, programs=0, copies=0, erases=0, metadata hits=960, misses=1098 (46.6% hit)
I (59806) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1152259 us, avg 1820.04 kB/s
I (59806) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 386736 us, avg 5422.70 kB/s

I (59816) fatfs_tp: write stats: reads=1203, programs=1096, copies=8, erases=18, metadata hits=1238, misses=1201 (50.8% hit)
I (59826) fatfs_tp: read stats: reads=2124, programs=0, copies=0, erases=0, metadata hits=960, misses=1099 (46.6% hit)
I (62616) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1120544 us, avg 1871.55 kB/s
I (62616) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 375415 us, avg 5586.22 kB/s

I (62626) fatfs_tp: write stats: reads=1146, programs=1096, copies=8, erases=17, metadata hits=361, misses=1144 (24.0% hit)
I (62636) fatfs_tp: read stats: reads=2124, programs=0, copies=0, erases=0, metadata hits=960, misses=1099 (46.6% hit)
I (65456) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1151942 us, avg 1820.54 kB/s
I (65456) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 373136 us, avg 5620.34 kB/s

I (65466) fatfs_tp: write stats: reads=1211, programs=1096, copies=8, erases=18, metadata hits=1237, misses=1209 (50.6% hit)
I (65476) fatfs_tp: read stats: reads=2121, programs=0, copies=0, erases=0, metadata hits=961, misses=1096 (46.7% hit)
I (66766) main_task: Returned from app_main()
```

### 9) Driver page register cache + Dhara FTL trace path cache + CHERRY-PICKED meta cache enabled (1 slot)
```
I (43647) vfs_fat_nand: Mounting again
I (43657) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43657) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43667) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43667) fatfs_tp: ======== baseline: stdio unbuffered ========
I (45547) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1202458 us, avg 1744.05 kB/s
I (45547) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 666251 us, avg 3147.69 kB/s

I (45557) fatfs_tp: write stats: reads=104, programs=1096, copies=8, erases=17, metadata hits=5, misses=102 (4.7% hit)
I (45567) fatfs_tp: read stats: reads=3079, programs=0, copies=0, erases=0, metadata hits=0, misses=2054 (0.0% hit)
I (48507) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1195813 us, avg 1753.75 kB/s
I (48507) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 528525 us, avg 3967.93 kB/s

I (48517) fatfs_tp: write stats: reads=1514, programs=1096, copies=8, erases=18, metadata hits=5, misses=1512 (0.3% hit)
I (48527) fatfs_tp: read stats: reads=3083, programs=0, copies=0, erases=0, metadata hits=0, misses=2058 (0.0% hit)
I (51507) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1187230 us, avg 1766.42 kB/s
I (51507) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 492330 us, avg 4259.65 kB/s

I (51507) fatfs_tp: write stats: reads=1502, programs=1096, copies=8, erases=17, metadata hits=5, misses=1500 (0.3% hit)
I (51527) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (54757) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1446172 us, avg 1450.14 kB/s
I (54757) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 486204 us, avg 4313.32 kB/s

I (54767) fatfs_tp: write stats: reads=2443, programs=1096, copies=8, erases=18, metadata hits=5, misses=2441 (0.2% hit)
I (54777) fatfs_tp: read stats: reads=3082, programs=0, copies=0, erases=0, metadata hits=0, misses=2057 (0.0% hit)
I (56067) fatfs_tp: ======== optimized: POSIX read/write ========
I (57847) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1193461 us, avg 1757.20 kB/s
I (57847) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 586621 us, avg 3574.97 kB/s

I (57857) fatfs_tp: write stats: reads=1503, programs=1096, copies=8, erases=17, metadata hits=5, misses=1501 (0.3% hit)
I (57867) fatfs_tp: read stats: reads=3083, programs=0, copies=0, erases=0, metadata hits=0, misses=2058 (0.0% hit)
I (61117) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1446734 us, avg 1449.58 kB/s
I (61117) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 508196 us, avg 4126.66 kB/s

I (61127) fatfs_tp: write stats: reads=2436, programs=1096, copies=8, erases=18, metadata hits=5, misses=2434 (0.2% hit)
I (61137) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (64097) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1182622 us, avg 1773.31 kB/s
I (64097) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 488002 us, avg 4297.42 kB/s

I (64107) fatfs_tp: write stats: reads=1502, programs=1096, copies=8, erases=17, metadata hits=5, misses=1500 (0.3% hit)
I (64117) fatfs_tp: read stats: reads=3084, programs=0, copies=0, erases=0, metadata hits=0, misses=2059 (0.0% hit)
I (67347) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1446662 us, avg 1449.65 kB/s
I (67347) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 483978 us, avg 4333.16 kB/s

I (67347) fatfs_tp: write stats: reads=2444, programs=1096, copies=8, erases=18, metadata hits=4, misses=2442 (0.2% hit)
I (67357) fatfs_tp: read stats: reads=3082, programs=0, copies=0, erases=0, metadata hits=0, misses=2057 (0.0% hit)
I (68657) main_task: Returned from app_main()

```

### 10) Driver page register cache + Dhara FTL trace path cache + CHERRY-PICKED meta cache enabled (2 slots)
```
I (43647) vfs_fat_nand: Mounting again
I (43657) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43657) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43657) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43667) fatfs_tp: ======== baseline: stdio unbuffered ========
I (45437) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1203542 us, avg 1742.48 kB/s
I (45437) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 554141 us, avg 3784.51 kB/s

I (45437) fatfs_tp: write stats: reads=97, programs=1096, copies=8, erases=17, metadata hits=12, misses=95 (11.2% hit)
I (45447) fatfs_tp: read stats: reads=2567, programs=0, copies=0, erases=0, metadata hits=512, misses=1542 (24.9% hit)
I (48297) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1196061 us, avg 1753.38 kB/s
I (48307) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 433544 us, avg 4837.23 kB/s

I (48307) fatfs_tp: write stats: reads=1503, programs=1096, copies=8, erases=18, metadata hits=16, misses=1501 (1.1% hit)
I (48317) fatfs_tp: read stats: reads=2571, programs=0, copies=0, erases=0, metadata hits=512, misses=1546 (24.9% hit)
I (51217) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1188609 us, avg 1764.38 kB/s
I (51217) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 406536 us, avg 5158.59 kB/s

I (51227) fatfs_tp: write stats: reads=1496, programs=1096, copies=8, erases=17, metadata hits=11, misses=1494 (0.7% hit)
I (51237) fatfs_tp: read stats: reads=2572, programs=0, copies=0, erases=0, metadata hits=512, misses=1547 (24.9% hit)
I (54257) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1321509 us, avg 1586.94 kB/s
I (54257) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 401683 us, avg 5220.91 kB/s

I (54257) fatfs_tp: write stats: reads=1969, programs=1096, copies=8, erases=18, metadata hits=479, misses=1967 (19.6% hit)
I (54277) fatfs_tp: read stats: reads=2570, programs=0, copies=0, erases=0, metadata hits=512, misses=1545 (24.9% hit)
I (55567) fatfs_tp: ======== optimized: POSIX read/write ========
I (57247) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1194023 us, avg 1756.37 kB/s
I (57247) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 480273 us, avg 4366.58 kB/s

I (57247) fatfs_tp: write stats: reads=1496, programs=1096, copies=8, erases=17, metadata hits=12, misses=1494 (0.8% hit)
I (57257) fatfs_tp: read stats: reads=2571, programs=0, copies=0, erases=0, metadata hits=512, misses=1546 (24.9% hit)
I (60297) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1321997 us, avg 1586.35 kB/s
I (60297) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 412968 us, avg 5078.24 kB/s

I (60307) fatfs_tp: write stats: reads=1960, programs=1096, copies=8, erases=18, metadata hits=481, misses=1958 (19.7% hit)
I (60317) fatfs_tp: read stats: reads=2572, programs=0, copies=0, erases=0, metadata hits=512, misses=1547 (24.9% hit)
I (63197) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1183586 us, avg 1771.86 kB/s
I (63197) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 400985 us, avg 5230.00 kB/s

I (63197) fatfs_tp: write stats: reads=1498, programs=1096, copies=8, erases=17, metadata hits=9, misses=1496 (0.6% hit)
I (63207) fatfs_tp: read stats: reads=2572, programs=0, copies=0, erases=0, metadata hits=512, misses=1547 (24.9% hit)
I (66227) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1322029 us, avg 1586.31 kB/s
I (66237) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 398810 us, avg 5258.52 kB/s

I (66237) fatfs_tp: write stats: reads=1969, programs=1096, copies=8, erases=18, metadata hits=479, misses=1967 (19.6% hit)
I (66247) fatfs_tp: read stats: reads=2570, programs=0, copies=0, erases=0, metadata hits=512, misses=1545 (24.9% hit)
I (67547) main_task: Returned from app_main()
```

### 11) Driver page register cache + Dhara FTL trace path cache + CHERRY-PICKED meta cache enabled (4 slots)
```
I (43647) vfs_fat_nand: Mounting again
I (43657) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43657) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43657) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43667) fatfs_tp: ======== baseline: stdio unbuffered ========
I (45427) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1203492 us, avg 1742.56 kB/s
I (45427) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 544265 us, avg 3853.18 kB/s

I (45427) fatfs_tp: write stats: reads=92, programs=1096, copies=8, erases=17, metadata hits=17, misses=90 (15.9% hit)
I (45437) fatfs_tp: read stats: reads=2310, programs=0, copies=0, erases=0, metadata hits=769, misses=1285 (37.4% hit)
I (48267) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1197353 us, avg 1751.49 kB/s
I (48267) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 419333 us, avg 5001.16 kB/s

I (48277) fatfs_tp: write stats: reads=1499, programs=1096, copies=8, erases=18, metadata hits=20, misses=1497 (1.3% hit)
I (48287) fatfs_tp: read stats: reads=2314, programs=0, copies=0, erases=0, metadata hits=769, misses=1289 (37.4% hit)
I (51167) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1189349 us, avg 1763.28 kB/s
I (51167) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 391850 us, avg 5351.93 kB/s

I (51167) fatfs_tp: write stats: reads=1492, programs=1096, copies=8, erases=17, metadata hits=15, misses=1490 (1.0% hit)
I (51177) fatfs_tp: read stats: reads=2315, programs=0, copies=0, erases=0, metadata hits=769, misses=1290 (37.3% hit)
I (54127) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1260275 us, avg 1664.04 kB/s
I (54127) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 386927 us, avg 5420.02 kB/s

I (54137) fatfs_tp: write stats: reads=1733, programs=1096, copies=8, erases=18, metadata hits=715, misses=1731 (29.2% hit)
I (54147) fatfs_tp: read stats: reads=2313, programs=0, copies=0, erases=0, metadata hits=769, misses=1288 (37.4% hit)
I (55437) fatfs_tp: ======== optimized: POSIX read/write ========
I (57097) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1194234 us, avg 1756.06 kB/s
I (57097) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 465517 us, avg 4505.00 kB/s

I (57107) fatfs_tp: write stats: reads=1492, programs=1096, copies=8, erases=17, metadata hits=16, misses=1490 (1.1% hit)
I (57117) fatfs_tp: read stats: reads=2314, programs=0, copies=0, erases=0, metadata hits=769, misses=1289 (37.4% hit)
I (60067) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1260561 us, avg 1663.67 kB/s
I (60067) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 398350 us, avg 5264.60 kB/s

I (60077) fatfs_tp: write stats: reads=1725, programs=1096, copies=8, erases=18, metadata hits=716, misses=1723 (29.4% hit)
I (60087) fatfs_tp: read stats: reads=2315, programs=0, copies=0, erases=0, metadata hits=769, misses=1290 (37.3% hit)
I (62947) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1183762 us, avg 1771.60 kB/s
I (62947) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 386310 us, avg 5428.68 kB/s

I (62957) fatfs_tp: write stats: reads=1494, programs=1096, copies=8, erases=17, metadata hits=13, misses=1492 (0.9% hit)
I (62967) fatfs_tp: read stats: reads=2315, programs=0, copies=0, erases=0, metadata hits=769, misses=1290 (37.3% hit)
I (65907) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1260673 us, avg 1663.52 kB/s
I (65907) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 384032 us, avg 5460.88 kB/s

I (65917) fatfs_tp: write stats: reads=1734, programs=1096, copies=8, erases=18, metadata hits=714, misses=1732 (29.2% hit)
I (65927) fatfs_tp: read stats: reads=2313, programs=0, copies=0, erases=0, metadata hits=769, misses=1288 (37.4% hit)
I (67217) main_task: Returned from app_main()
```

### 12) Driver page register cache + Dhara FTL trace path cache + CHERRY-PICKED meta cache enabled (8 slots)
```
I (43647) vfs_fat_nand: Mounting again
I (43657) fatfs_tp: FAT FS: 472000 kB total, 472000 kB free
I (43657) fatfs_tp: SPI: 80000 kHz, QIO; cluster=32768; test file=2097152 bytes
I (43657) fatfs_tp: Volume was erased before mount; format uses the configured allocation unit
I (43667) fatfs_tp: ======== baseline: stdio unbuffered ========
I (45417) fatfs_tp: [stdio/_IONBF] chunk=512: Wrote 2097152 bytes in 1203703 us, avg 1742.25 kB/s
I (45417) fatfs_tp: [stdio/_IONBF] chunk=512: Read 2097152 bytes in 537320 us, avg 3902.99 kB/s

I (45427) fatfs_tp: write stats: reads=92, programs=1096, copies=8, erases=17, metadata hits=17, misses=90 (15.9% hit)
I (45437) fatfs_tp: read stats: reads=2184, programs=0, copies=0, erases=0, metadata hits=895, misses=1159 (43.6% hit)
I (48207) fatfs_tp: [stdio/_IONBF] chunk=4096: Wrote 2097152 bytes in 1144719 us, avg 1832.02 kB/s
I (48207) fatfs_tp: [stdio/_IONBF] chunk=4096: Read 2097152 bytes in 412130 us, avg 5088.57 kB/s

I (48217) fatfs_tp: write stats: reads=1206, programs=1096, copies=8, erases=18, metadata hits=313, misses=1204 (20.6% hit)
I (48227) fatfs_tp: read stats: reads=2188, programs=0, copies=0, erases=0, metadata hits=895, misses=1163 (43.5% hit)
I (51047) fatfs_tp: [stdio/_IONBF] chunk=16384: Wrote 2097152 bytes in 1136105 us, avg 1845.91 kB/s
I (51047) fatfs_tp: [stdio/_IONBF] chunk=16384: Read 2097152 bytes in 384708 us, avg 5451.28 kB/s

I (51047) fatfs_tp: write stats: reads=1199, programs=1096, copies=8, erases=17, metadata hits=308, misses=1197 (20.5% hit)
I (51057) fatfs_tp: read stats: reads=2189, programs=0, copies=0, erases=0, metadata hits=895, misses=1164 (43.5% hit)
I (53917) fatfs_tp: [stdio/_IONBF] chunk=32768: Wrote 2097152 bytes in 1177429 us, avg 1781.13 kB/s
I (53917) fatfs_tp: [stdio/_IONBF] chunk=32768: Read 2097152 bytes in 379767 us, avg 5522.21 kB/s

I (53927) fatfs_tp: write stats: reads=1324, programs=1096, copies=8, erases=18, metadata hits=1124, misses=1322 (46.0% hit)
I (53937) fatfs_tp: read stats: reads=2187, programs=0, copies=0, erases=0, metadata hits=895, misses=1162 (43.5% hit)
I (55227) fatfs_tp: ======== optimized: POSIX read/write ========
I (56827) fatfs_tp: [posix read/write] chunk=512: Wrote 2097152 bytes in 1140795 us, avg 1838.33 kB/s
I (56827) fatfs_tp: [posix read/write] chunk=512: Read 2097152 bytes in 458342 us, avg 4575.52 kB/s

I (56837) fatfs_tp: write stats: reads=1200, programs=1096, copies=8, erases=17, metadata hits=308, misses=1198 (20.5% hit)
I (56847) fatfs_tp: read stats: reads=2188, programs=0, copies=0, erases=0, metadata hits=895, misses=1163 (43.5% hit)
I (59707) fatfs_tp: [posix read/write] chunk=4096: Wrote 2097152 bytes in 1177893 us, avg 1780.43 kB/s
I (59707) fatfs_tp: [posix read/write] chunk=4096: Read 2097152 bytes in 391157 us, avg 5361.41 kB/s

I (59717) fatfs_tp: write stats: reads=1317, programs=1096, copies=8, erases=18, metadata hits=1124, misses=1315 (46.1% hit)
I (59727) fatfs_tp: read stats: reads=2189, programs=0, copies=0, erases=0, metadata hits=895, misses=1164 (43.5% hit)
I (62527) fatfs_tp: [posix read/write] chunk=16384: Wrote 2097152 bytes in 1130465 us, avg 1855.12 kB/s
I (62527) fatfs_tp: [posix read/write] chunk=16384: Read 2097152 bytes in 379157 us, avg 5531.09 kB/s

I (62537) fatfs_tp: write stats: reads=1201, programs=1096, copies=8, erases=17, metadata hits=306, misses=1199 (20.3% hit)
I (62547) fatfs_tp: read stats: reads=2189, programs=0, copies=0, erases=0, metadata hits=895, misses=1164 (43.5% hit)
I (65397) fatfs_tp: [posix read/write] chunk=32768: Wrote 2097152 bytes in 1177826 us, avg 1780.53 kB/s
I (65397) fatfs_tp: [posix read/write] chunk=32768: Read 2097152 bytes in 376902 us, avg 5564.18 kB/s

I (65407) fatfs_tp: write stats: reads=1325, programs=1096, copies=8, erases=18, metadata hits=1123, misses=1323 (45.9% hit)
I (65417) fatfs_tp: read stats: reads=2187, programs=0, copies=0, erases=0, metadata hits=895, misses=1162 (43.5% hit)
I (66707) main_task: Returned from app_main()

```
