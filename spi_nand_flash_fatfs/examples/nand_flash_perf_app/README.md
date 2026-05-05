| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- | -------- |

# SPI NAND Flash Performance Benchmark

Measures SPI NAND Flash read/write throughput and per-page latency through the
Dhara FTL wear-leveling layer.

## What it measures

- **Sequential benchmark**: Write all logical pages in order, then read them back.
- **Random benchmark**: Write/read pages in shuffled order (stresses Dhara journal).

Each benchmark runs multiple passes and reports:
- Per-pass throughput (kB/s)
- Min / Max / Mean / StdDev throughput across passes
- Per-page latency: min, max, mean, p95 (µs)
- Latency histogram (6 buckets: <500 µs → ≥10 ms)

## How to use

```cmake
idf.py -p PORT flash monitor
```

The app performs a full chip erase, an untimed warmup pass, then the two
benchmarks. Expect the full run to take several minutes on a 1 Gbit device.

## Example output

```
I (400)  perf_app: Flash: 2048 pages x 2048 bytes = 4096 KB total
I (400)  perf_app: Erasing chip...
I (2100) perf_app: Warmup pass (not timed)...
I (8200) perf_app: Running sequential benchmark...

[PERF] Sequential WRITE (2048 pages x 3 passes):
  Pass 1:  893 kB/s   Pass 2:  901 kB/s   Pass 3:  888 kB/s
  Mean: 894 kB/s  Min: 888  Max: 901  StdDev: 5.4 kB/s
  Latency (us): min=820  mean=2290  p95=4100  max=9830
  Histogram: <500us:0  500-1ms:312  1-2ms:3210  2-5ms:2610  5-10ms:512  >=10ms:0

[PERF] Sequential READ (2048 pages x 3 passes):
  Pass 1: 3310 kB/s   Pass 2: 3298 kB/s   Pass 3: 3321 kB/s
  Mean: 3310 kB/s  Min: 3298  Max: 3321  StdDev: 9.7 kB/s
  Latency (us): min=480  mean=618  p95=720  max=1100
  Histogram: <500us:91  500-1ms:5970  1-2ms:83  2-5ms:0  5-10ms:0  >=10ms:0
...
```
