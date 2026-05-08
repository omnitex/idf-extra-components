| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- | -------- |

# SPI NAND Flash Performance Benchmark

Measures SPI NAND Flash read/write throughput and per-page latency through the
Dhara FTL wear-leveling layer.

## What it measures

- **Sequential benchmark**: Write all logical pages in order, then read them back.
- **Random benchmark**: Write/read pages in shuffled order (stresses Dhara journal).
- **Zipf benchmark**: Write/read pages sampled from a Zipf distribution (skew=1.0 by default), modelling real-world hot-spot access patterns where a small fraction of pages receives the majority of writes.

Each benchmark runs multiple passes and reports:
- Per-pass throughput (kB/s)
- Min / Max / Mean / StdDev throughput across passes
- Per-page latency: min, max, mean, stddev, p95, max (µs)
- Latency histogram (**nine** buckets: `<500us` … `>=10ms`)

## How to use

### Automated capture with `run_perf.py` (recommended)

`run_perf.py` flashes the firmware, resets the chip, streams serial output to the
terminal, and — once the app prints `Returned from app_main()` — automatically
extracts and saves the JSON report to `perf_results/`.

```bash
# Flash + capture (auto-detect port)
python3 run_perf.py

# Explicit port
python3 run_perf.py --port /dev/cu.usbmodem14301

# Skip flash if firmware is already on device
python3 run_perf.py --no-flash

# Keep raw serial log alongside the JSON
python3 run_perf.py --keep-log
```

Each run produces a timestamped file:

```
perf_results/run_20260506_143022.json
perf_results/run_20260506_143022.log   # only with --keep-log
```

The script prints a brief summary on exit:

```
[run_perf] Done — esp32s3  SIO  40000 kHz
  Sequential    write    894 kB/s   read   3310 kB/s
  Random        write    761 kB/s   read   3298 kB/s

  → perf_results/run_20260506_143022.json
```

`perf_results/` is listed in `.gitignore` — result files are never committed.

### Manual capture

```bash
idf.py -p PORT flash monitor 2>&1 | tee nand_perf_run.log
```

The app performs a full chip erase, an untimed warmup pass, then the two
benchmarks. Expect the full run to take several minutes on a 1 Gbit device.

After the human-readable `[PERF]` logs and summary table, the firmware **always**
prints one UTF-8 JSON report on stdout (no log prefix):

- **`<<<NAND_PERF_JSON_BEGIN>>>`** — first marker line  
- Next line: single-line compact JSON, schema **`esp_nand_perf_v1`** (`schema` field)  
- **`<<<NAND_PERF_JSON_END>>>`** — last marker line  

If the UART log contains multiple runs (reset, reflashes), parsers should take the **last** complete block between the markers.

### Extract JSON from a raw log

Markers are delimiter-only; splitting is robust for nested `{}` inside the payload.

```python
import json
from pathlib import Path

LOG_PATH = Path("nand_perf_run.log")
BEGIN, END = "<<<NAND_PERF_JSON_BEGIN>>>", "<<<NAND_PERF_JSON_END>>>"

text = LOG_PATH.read_text(encoding="utf-8", errors="replace")
after_last_begin = text.rsplit(BEGIN, 1)[1]
payload = after_last_begin.split(END, 1)[0].strip()

report = json.loads(payload)
assert report.get("schema") == "esp_nand_perf_v1"

Path("nand_perf_report.json").write_text(
    json.dumps(report, indent=2, sort_keys=False) + "\n",
    encoding="utf-8",
)
```

Optional downstream: **`pandas`** `pd.json_normalize(report["results"])`, etc., keyed off `schema`.

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
