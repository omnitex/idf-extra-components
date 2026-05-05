#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""
NAND Flash Performance Benchmark Runner
========================================
Resets the ESP32, captures serial output until the app finishes, then
extracts and saves the embedded JSON performance report.

Usage
-----
    # Auto-detect port, save to perf_results/
    python3 run_perf.py

    # Explicit port
    python3 run_perf.py --port /dev/cu.usbmodem14301

    # Skip flash (device already has the firmware)
    python3 run_perf.py --no-flash

    # Keep raw log alongside the JSON
    python3 run_perf.py --keep-log

Dependencies: pyserial (already present in the IDF Python env)
"""

import argparse
import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports

# ── constants ────────────────────────────────────────────────────────────────

BAUD = 115_200
APP_DONE_MARKER = b'Returned from app_main()'
JSON_BEGIN = b'<<<NAND_PERF_JSON_BEGIN>>>'
JSON_END = b'<<<NAND_PERF_JSON_END>>>'
JSON_SCHEMA = 'esp_nand_perf_v1'

PERF_RESULTS_DIR = Path(__file__).parent / 'perf_results'

# macOS ports to skip (same list as esp-idf-monitor)
_FILTERED_PORT_SUFFIXES = ('Bluetooth-Incoming-Port', 'wlan-debug', 'cu.debug-console')

# USB JTAG/serial PID — needs a different RTS/DTR reset sequence
_USB_JTAG_PID = 0x1001


# ── port detection ────────────────────────────────────────────────────────────

def _detect_port() -> str:
    ports = [
        p for p in list_ports.comports()
        if not p.device.endswith(_FILTERED_PORT_SUFFIXES)
    ]
    if not ports:
        sys.exit('ERROR: No serial ports detected. Connect the device and retry.')
    port = ports[-1].device
    print(f'[run_perf] Auto-detected port: {port}')
    return port


def _port_pid(port_device: str):
    for p in list_ports.comports():
        if p.device == port_device:
            return p.pid
    return None


# ── chip reset via RTS/DTR ────────────────────────────────────────────────────

def _reset_chip(ser: serial.Serial) -> None:
    pid = _port_pid(ser.port)
    if pid == _USB_JTAG_PID:
        # USB JTAG/serial reset sequence (same as esp-idf-monitor)
        ser.setRTS(True)
        ser.setDTR(True)
        time.sleep(0.1)
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)
        ser.setDTR(True)
        ser.setRTS(False)
        time.sleep(0.1)
        ser.setDTR(True)
        ser.setRTS(True)
    else:
        # Classic EN/IO0 reset sequence
        ser.setDTR(True)   # IO0 = HIGH
        ser.setRTS(False)  # EN  = LOW  → chip in reset
        time.sleep(0.1)
        ser.setDTR(False)  # IO0 = LOW
        ser.setRTS(True)   # EN  = HIGH → chip out of reset
        time.sleep(0.05)
        ser.setDTR(True)   # IO0 = HIGH, done


# ── flash step ────────────────────────────────────────────────────────────────

def _flash(port: str) -> None:
    print(f'[run_perf] Flashing via idf.py -p {port} flash ...')
    result = subprocess.run(
        ['idf.py', '-p', port, 'flash'],
        cwd=Path(__file__).parent,
    )
    if result.returncode != 0:
        sys.exit(f'ERROR: idf.py flash failed (exit {result.returncode})')
    print('[run_perf] Flash complete.')


# ── serial capture ────────────────────────────────────────────────────────────

def _capture(port: str) -> tuple[list[bytes], bool]:
    """
    Open the serial port, reset the chip, then read lines until APP_DONE_MARKER
    or EOF / KeyboardInterrupt.  Returns (lines, json_found).
    """
    print(f'[run_perf] Opening {port} @ {BAUD} baud ...')
    ser = serial.Serial(port, BAUD, timeout=1)
    ser.flushInput()

    print('[run_perf] Resetting chip ...')
    _reset_chip(ser)

    lines: list[bytes] = []
    json_found = False

    print('[run_perf] Capturing output  (Ctrl-C to abort)\n')
    try:
        while True:
            raw = ser.readline()  # blocks up to timeout=1 s
            if not raw:
                continue

            lines.append(raw)

            sys.stdout.write(raw.decode('utf-8', errors='replace'))
            sys.stdout.flush()

            if JSON_END in raw:
                json_found = True

            if APP_DONE_MARKER in raw:
                print('\n[run_perf] App finished — stopping capture.')
                break

    except KeyboardInterrupt:
        print('\n[run_perf] Interrupted by user.')
    finally:
        ser.close()

    return lines, json_found


# ── JSON extraction ───────────────────────────────────────────────────────────

def _extract_json(lines: list[bytes]) -> dict:
    joined = b''.join(lines)
    if JSON_BEGIN not in joined or JSON_END not in joined:
        sys.exit('ERROR: JSON markers not found in captured output.')

    payload_bytes = joined.rsplit(JSON_BEGIN, 1)[1].split(JSON_END, 1)[0].strip()
    payload_str = payload_bytes.decode('utf-8', errors='replace')

    report = json.loads(payload_str)
    if report.get('schema') != JSON_SCHEMA:
        print(f"WARNING: unexpected schema '{report.get('schema')}' (expected '{JSON_SCHEMA}')")
    return report


# ── save outputs ─────────────────────────────────────────────────────────────

def _save(report: dict, lines: list[bytes], keep_log: bool, timestamp: str) -> Path:
    PERF_RESULTS_DIR.mkdir(exist_ok=True)

    json_path = PERF_RESULTS_DIR / f'run_{timestamp}.json'
    json_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'[run_perf] JSON  → {json_path}')

    if keep_log:
        log_path = PERF_RESULTS_DIR / f'run_{timestamp}.log'
        log_path.write_bytes(b''.join(lines))
        print(f'[run_perf] Log   → {log_path}')

    return json_path


# ── entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description='Flash, capture, and extract NAND perf benchmark results.'
    )
    parser.add_argument('--port', '-p', help='Serial port (auto-detected if omitted)')
    parser.add_argument('--no-flash', action='store_true',
                        help='Skip idf.py flash (device already has current firmware)')
    parser.add_argument('--keep-log', action='store_true',
                        help='Save raw serial log alongside the JSON')
    args = parser.parse_args()

    port = args.port or _detect_port()
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')

    if not args.no_flash:
        _flash(port)

    lines, json_found = _capture(port)

    if not json_found:
        sys.exit('ERROR: Capture ended before JSON markers were seen.')

    report = _extract_json(lines)
    json_path = _save(report, lines, args.keep_log, timestamp)

    chip = report.get('meta', {}).get('chip', '?')
    spi = report.get('spi', {})
    print(
        f'\n[run_perf] Done — {chip}  '
        f'{spi.get("io_mode","?")}  '
        f'{spi.get("clock_khz","?")} kHz'
    )
    for r in report.get('results', []):
        name = r.get('name', '?')
        w = r.get('write', {}).get('mean_kbps', 0)
        rd = r.get('read', {}).get('mean_kbps', 0)
        print(f'  {name:12s}  write {w:6.0f} kB/s   read {rd:6.0f} kB/s')
    print(f'\n  → {json_path.relative_to(Path(__file__).parent)}')


if __name__ == '__main__':
    main()
