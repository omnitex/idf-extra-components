#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Havlik <omnitex.git@gmail.com>
#
# SPDX-License-Identifier: Apache-2.0
"""NAND perf result inventory — config checker & gap finder.

Scans perf_results/*.json, groups by unique build configuration,
and reports which parameter combinations have (or lack) measurements.

Usage:
    python perf_inventory.py                    # full inventory
    python perf_inventory.py --io-mode SIO      # filter by SPI mode
    python perf_inventory.py --gaps-only        # only show missing combos
    python perf_inventory.py --json             # machine-readable output
    python perf_inventory.py --summary-perf     # include mean kB/s in table
"""

import argparse
import json
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from itertools import product
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

PARAM_BOOL = ["nand_page_register_cache", "dhara_map_path_cache"]
PARAM_INT = ["dhara_meta_cache_slots", "dhara_radix_depth"]
PARAM_EXTRA_BOOL = ["dhara_prog_page_relief"]


@dataclass(frozen=True)
class ConfigKey:
    """Hashable config identity (the dimensions we compare across)."""

    page_reg_cache: bool
    map_path_cache: bool
    meta_cache_slots: int
    radix_depth: int
    prog_relief: bool
    io_mode: str
    verify_write: bool = False

    def short_label(self) -> str:
        """Human-readable one-line summary."""
        parts = [
            f"reg={'Y' if self.page_reg_cache else 'N'}",
            f"map={'Y' if self.map_path_cache else 'N'}",
            f"meta={self.meta_cache_slots}",
            f"radix={self.radix_depth}",
            f"relief={'Y' if self.prog_relief else 'N'}",
            self.io_mode,
        ]
        if self.verify_write:
            parts.append("verify")
        return " ".join(parts)


@dataclass
class RunInfo:
    """One JSON result file."""

    filename: str
    schema: str
    config_key: ConfigKey
    raw_build_config: dict[str, Any]
    raw_spi: dict[str, Any]
    perf: dict[str, dict[str, float]] = field(default_factory=dict)
    has_cache_stats: bool = False


def _extract_config_key(bc: dict, spi: dict) -> ConfigKey:
    return ConfigKey(
        page_reg_cache=bc.get("nand_page_register_cache", False),
        map_path_cache=bc.get("dhara_map_path_cache", False),
        meta_cache_slots=bc.get("dhara_meta_cache_slots", 0),
        radix_depth=bc.get("dhara_radix_depth", 0),
        prog_relief=bc.get("dhara_prog_page_relief", False),
        io_mode=spi.get("io_mode", "SIO"),
        verify_write=bc.get("nand_verify_write", False),
    )


def _extract_perf(results: list[dict]) -> dict[str, dict[str, float]]:
    """Extract {benchmark_name: {write: mean_kbps, read: mean_kbps}}."""
    perf: dict[str, dict[str, float]] = {}
    for r in results:
        name = r["name"]
        perf[name] = {
            "write": r.get("write", {}).get("mean_kbps", 0.0),
            "read": r.get("read", {}).get("mean_kbps", 0.0),
        }
    return perf


# ---------------------------------------------------------------------------
# Loader
# ---------------------------------------------------------------------------

def load_results(directory: Path) -> list[RunInfo]:
    """Load all run_*.json files from directory."""
    runs: list[RunInfo] = []
    for fp in sorted(directory.glob("run_*.json")):
        try:
            data = json.loads(fp.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            print(f"  WARN: skipping {fp.name}: {exc}", file=sys.stderr)
            continue

        bc = data.get("build_config", {})
        spi = data.get("spi", {})
        results = data.get("results", [])
        schema = data.get("schema", "unknown")

        key = _extract_config_key(bc, spi)
        perf = _extract_perf(results)

        runs.append(
            RunInfo(
                filename=fp.name,
                schema=schema,
                config_key=key,
                raw_build_config=bc,
                raw_spi=spi,
                perf=perf,
                has_cache_stats=(schema == "esp_nand_perf_v2"),
            )
        )
    return runs


# ---------------------------------------------------------------------------
# Grouping
# ---------------------------------------------------------------------------

def group_by_config(runs: list[RunInfo]) -> dict[ConfigKey, list[RunInfo]]:
    groups: dict[ConfigKey, list[RunInfo]] = defaultdict(list)
    for r in runs:
        groups[r.config_key].append(r)
    return dict(groups)


# ---------------------------------------------------------------------------
# Gap analysis
# ---------------------------------------------------------------------------

def find_gaps(
    existing: set[ConfigKey],
    io_mode_filter: str | None = None,
    sweep_all: bool = False,
) -> list[ConfigKey]:
    """Find parameter combinations not yet measured.

    By default (sweep_all=False), only sweeps the 'interesting' space:
      page_reg_cache=True, meta_cache_slots in {0,4,8}, map/relief/radix vary.
    With sweep_all=True, enumerates all combos including page_reg_cache=False.
    """
    observed = set()
    for k in existing:
        observed.add(
            ConfigKey(
                page_reg_cache=k.page_reg_cache,
                map_path_cache=k.map_path_cache,
                meta_cache_slots=k.meta_cache_slots,
                radix_depth=k.radix_depth,
                prog_relief=k.prog_relief,
                io_mode=k.io_mode,
                verify_write=False,
            )
        )

    io_modes = [io_mode_filter] if io_mode_filter else ["SIO", "DIO"]
    page_reg_vals = [True, False] if sweep_all else [True, False]
    map_path_vals = [True, False]
    meta_vals = [0, 4, 8]
    radix_vals = [17, 32]
    relief_vals = [True, False]

    missing: list[ConfigKey] = []
    for io, prc, mpc, meta, radix, relief in product(
        io_modes, page_reg_vals, map_path_vals, meta_vals, radix_vals, relief_vals
    ):
        if not sweep_all and not prc and (mpc or meta > 0 or relief):
            continue
        key = ConfigKey(
            page_reg_cache=prc,
            map_path_cache=mpc,
            meta_cache_slots=meta,
            radix_depth=radix,
            prog_relief=relief,
            io_mode=io,
        )
        if key not in observed:
            missing.append(key)

    return missing


# ---------------------------------------------------------------------------
# Terminal output
# ---------------------------------------------------------------------------

def _cell(text: str, width: int, align: str = "center") -> str:
    """Pad/truncate a cell to width."""
    if len(text) > width:
        text = text[: width - 1] + "…"
    pad = width - len(text)
    if align == "center":
        left = pad // 2
        right = pad - left
        return " " * left + text + " " * right
    elif align == "right":
        return " " * pad + text
    return text + " " * pad


COL_WIDTHS = {
    "#": 3,
    "reg_cache": 10,
    "map_cache": 10,
    "meta_slots": 10,
    "radix": 7,
    "relief": 8,
    "io": 4,
    "verify": 7,
    "schema": 5,
    "runs": 5,
}


def _print_header():
    cols = list(COL_WIDTHS.keys())
    header = "│"
    for col_name in cols:
        w = COL_WIDTHS[col_name]
        header += f" {_cell(col_name, w)} │"
    print(header)


def _print_row(idx: int, key: ConfigKey, n_runs: int, schema: str):
    cols = [
        str(idx),
        "✓" if key.page_reg_cache else "✗",
        "✓" if key.map_path_cache else "✗",
        str(key.meta_cache_slots),
        str(key.radix_depth),
        "✓" if key.prog_relief else "✗",
        key.io_mode,
        "✓" if key.verify_write else "✗",
        schema.replace("esp_nand_perf_", ""),
        str(n_runs),
    ]
    row = "│"
    for col_name, val in zip(COL_WIDTHS.keys(), cols):
        w = COL_WIDTHS[col_name]
        row += f" {_cell(val, w)} │"
    print(row)


def print_inventory(
    groups: dict[ConfigKey, list[RunInfo]],
    runs: list[RunInfo],
    show_perf: bool = False,
):
    """Print the full config inventory table."""
    total_files = len(runs)
    unique_configs = len(groups)

    print()
    print(
        f"  NAND Perf Config Inventory  "
        f"({total_files} files, {unique_configs} unique configs)"
    )
    print()

    # Header
    top_border = "╭" + "┬".join("─" * (w + 2) for w in COL_WIDTHS.values()) + "╮"
    mid_border = "├" + "┼".join("─" * (w + 2) for w in COL_WIDTHS.values()) + "┤"
    bot_border = "╰" + "┴".join("─" * (w + 2) for w in COL_WIDTHS.values()) + "╯"

    print(top_border)
    _print_header()
    print(mid_border)

    # Sort configs: group by io_mode, then by increasing optimization level
    def sort_key(item: tuple[ConfigKey, list[RunInfo]]) -> tuple:
        k = item[0]
        return (
            k.io_mode,
            not k.page_reg_cache,
            not k.map_path_cache,
            k.meta_cache_slots,
            -k.radix_depth,  # 32 before 17 (larger first)
            not k.prog_relief,
            k.verify_write,
        )

    for idx, (key, group) in enumerate(
        sorted(groups.items(), key=sort_key), start=1
    ):
        # Pick the latest schema version among runs
        schemas = {r.schema for r in group}
        if "esp_nand_perf_v2" in schemas:
            schema = "v2"
        else:
            schema = "v1"
        _print_row(idx, key, len(group), schema)
    print(bot_border)

    # Perf summary if requested
    if show_perf:
        print()
        print("  Throughput summary (mean kB/s, averaged across duplicate runs):")
        print()
        bench_names = ["Sequential", "Random", "Zipf"]
        directions = ["write", "read"]
        # Collect all benchmark names present
        all_benches: set[str] = set()
        for group_runs in groups.values():
            for r in group_runs:
                all_benches.update(r.perf.keys())
        bench_names_ordered = [b for b in bench_names if b in all_benches]

        header_parts = [f"{'Config':<50}"]
        for bench in bench_names_ordered:
            for d in directions:
                header_parts.append(f"{bench[:3]} {d[:1].upper()} (kB/s)")
        header_line = "  ".join(header_parts)
        print(f"  {header_line}")
        print(f"  {'─' * len(header_line)}")

        for idx, (key, group) in enumerate(
            sorted(groups.items(), key=sort_key), start=1
        ):
            label = f"#{idx:<2} {key.short_label()}"
            parts = [f"{label:<50}"]
            for bench in bench_names_ordered:
                for d in directions:
                    vals = [r.perf.get(bench, {}).get(d, 0.0) for r in group]
                    vals = [v for v in vals if v > 0]
                    if vals:
                        avg = sum(vals) / len(vals)
                        parts.append(f"{avg:>12.0f}")
                    else:
                        parts.append(f"{'—':>12}")
            print(f"  {'  '.join(parts)}")


def print_gaps(
    missing: list[ConfigKey],
    io_mode_filter: str | None = None,
):
    """Print missing combos."""
    if not missing:
        print("\n  No gaps — full coverage! All combos measured.\n")
        return

    # Group by io_mode
    by_io: dict[str, list[ConfigKey]] = defaultdict(list)
    for k in missing:
        by_io[k.io_mode].append(k)

    for io, keys in sorted(by_io.items()):
        print(f"\n  Missing combos ({io}):")
        print(f"  {'reg_cache':>10}  {'map_cache':>10}  {'meta_slots':>10}  {'radix':>7}  {'relief':>8}")
        print(f"  {'─'*10}  {'─'*10}  {'─'*10}  {'─'*7}  {'─'*8}")
        for k in sorted(keys, key=lambda x: (not x.page_reg_cache, not x.map_path_cache, x.meta_cache_slots, -x.radix_depth, not x.prog_relief)):
            print(
                f"  {'✓' if k.page_reg_cache else '✗':>10}  "
                f"{'✓' if k.map_path_cache else '✗':>10}  "
                f"{k.meta_cache_slots:>10}  "
                f"{k.radix_depth:>7}  "
                f"{'✓' if k.prog_relief else '✗':>8}"
            )
    print()


def print_json_output(
    groups: dict[ConfigKey, list[RunInfo]],
    missing: list[ConfigKey],
):
    """Machine-readable JSON output."""
    output: dict[str, Any] = {
        "total_files": sum(len(g) for g in groups.values()),
        "unique_configs": len(groups),
        "configs": [],
        "missing": [],
    }
    for key, group in sorted(groups.items(), key=lambda x: x[0].short_label()):
        runs_data = []
        for r in group:
            runs_data.append(
                {
                    "filename": r.filename,
                    "schema": r.schema,
                    "perf": r.perf,
                }
            )
        output["configs"].append(
            {
                "config": {
                    "page_reg_cache": key.page_reg_cache,
                    "map_path_cache": key.map_path_cache,
                    "meta_cache_slots": key.meta_cache_slots,
                    "radix_depth": key.radix_depth,
                    "prog_relief": key.prog_relief,
                    "io_mode": key.io_mode,
                    "verify_write": key.verify_write,
                },
                "num_runs": len(group),
                "runs": runs_data,
            }
        )
    for k in missing:
        output["missing"].append(
            {
                "page_reg_cache": k.page_reg_cache,
                "map_path_cache": k.map_path_cache,
                "meta_cache_slots": k.meta_cache_slots,
                "radix_depth": k.radix_depth,
                "prog_relief": k.prog_relief,
                "io_mode": k.io_mode,
            }
        )
    print(json.dumps(output, indent=2))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="NAND perf result inventory — config checker & gap finder",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=Path(__file__).parent / "perf_results",
        help="Directory containing run_*.json files (default: perf_results/)",
    )
    parser.add_argument(
        "--io-mode",
        choices=["SIO", "DIO"],
        default=None,
        help="Filter inventory to a specific SPI IO mode",
    )
    parser.add_argument(
        "--gaps-only",
        action="store_true",
        help="Only show missing parameter combinations",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        dest="json_output",
        help="Output machine-readable JSON",
    )
    parser.add_argument(
        "--summary-perf",
        action="store_true",
        help="Include throughput summary in the table",
    )
    parser.add_argument(
        "--sweep-all",
        action="store_true",
        help="Show ALL missing combos (default: only practical ones with page_reg_cache=True, or False+no-other-caches)",
    )
    args = parser.parse_args()

    if not args.results_dir.is_dir():
        print(f"Error: {args.results_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    runs = load_results(args.results_dir)
    if not runs:
        print(f"No run_*.json files found in {args.results_dir}", file=sys.stderr)
        sys.exit(1)

    # Filter by io_mode if requested
    if args.io_mode:
        runs = [r for r in runs if r.config_key.io_mode == args.io_mode]

    groups = group_by_config(runs)
    missing = find_gaps(set(groups.keys()), io_mode_filter=args.io_mode, sweep_all=args.sweep_all)

    if args.json_output:
        print_json_output(groups, missing)
        return

    if not args.gaps_only:
        print_inventory(groups, runs, show_perf=args.summary_perf)

    print_gaps(missing, io_mode_filter=args.io_mode)

    # Summary stats
    sio_count = sum(1 for r in runs if r.config_key.io_mode == "SIO")
    dio_count = sum(1 for r in runs if r.config_key.io_mode == "DIO")
    v1_count = sum(1 for r in runs if r.schema == "esp_nand_perf_v1")
    v2_count = sum(1 for r in runs if r.schema == "esp_nand_perf_v2")
    print(f"  SIO runs: {sio_count}  |  DIO runs: {dio_count}")
    print(f"  v1 schema: {v1_count}  |  v2 schema: {v2_count}")
    print(f"  Total gaps (SIO+DIO): {len(missing)}")
    print()


if __name__ == "__main__":
    main()
