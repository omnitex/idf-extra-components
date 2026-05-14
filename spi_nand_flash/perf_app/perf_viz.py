#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Havlik <omnitex.git@gmail.com>
#
# SPDX-License-Identifier: Apache-2.0
# pyright: reportMissingImports=false
"""Create thesis-ready benchmark figures from perf_results/run_*.json.

Reuses perf_inventory.py for config discovery and deduplication.
Saves PDF and PNG outputs for LaTeX and quick inspection.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Union

import matplotlib

matplotlib.use("Agg")

import matplotlib.colors as mcolors
import matplotlib.pyplot as plt
import numpy as np

from perf_inventory import ConfigKey, RunInfo, group_by_config, load_results

THESIS_TEXTWIDTH_IN = 5.984
BENCHMARKS = ["Sequential", "Random", "Zipf"]
DIRECTIONS = ["write", "read"]
BAR_COLORS = {"write": "#4C78A8", "read": "#F58518"}
STEP_COLORS = ["#DCEAF7", "#B8D8F0", "#91C4E8", "#64A9D9", "#356D9E"]
SCATTER_MARKERS = {False: "s", True: "o"}
FIGURE_GROUPS = {
    "1": ["1"],
    "1b": ["1b"],
    "1split": ["1a", "1c", "1d", "1e"],
    "2": ["2a", "2b", "2c"],
    "3": ["3a", "3b", "3c"],
    "4": ["4a", "4b", "4c"],
    "5": ["5a", "5b", "5c"],
    "6": ["6"],
    "7": ["7"],
}
FIGURE_FILES = {
    "1": "fig01_throughput_heatmap",
    "1a": "fig01a_throughput_heatmap_rlfN",
    "1c": "fig01c_throughput_heatmap_rlfY",
    "1b": "fig01b_latency_heatmap",
    "1d": "fig01d_latency_heatmap_rlfN",
    "1e": "fig01e_latency_heatmap_rlfY",
    "2a": "fig02a_sequential_throughput",
    "2b": "fig02b_random_throughput",
    "2c": "fig02c_zipf_throughput",
    "3a": "fig03a_sequential_waterfall",
    "3b": "fig03b_random_waterfall",
    "3c": "fig03c_zipf_waterfall",
    "4a": "fig04a_sequential_latency",
    "4b": "fig04b_random_latency",
    "4c": "fig04c_zipf_latency",
    "5a": "fig05a_sequential_histogram",
    "5b": "fig05b_random_histogram",
    "5c": "fig05c_zipf_histogram",
    "6": "fig06_cache_hit_rates",
    "7": "fig07_throughput_vs_latency",
}
WATERFALL_STEPS = [
    (
        "Baseline",
        lambda c: not c.page_reg_cache
        and not c.map_path_cache
        and c.meta_cache_slots == 0
        and not c.prog_relief,
    ),
    (
        "+Page Register Cache",
        lambda c: c.page_reg_cache
        and not c.map_path_cache
        and c.meta_cache_slots == 0
        and not c.prog_relief,
    ),
    (
        "+Map Path Cache",
        lambda c: c.page_reg_cache
        and c.map_path_cache
        and c.meta_cache_slots == 0
        and not c.prog_relief,
    ),
    (
        "+Meta Cache",
        lambda c: c.page_reg_cache
        and c.map_path_cache
        and c.meta_cache_slots in {4, 8}
        and not c.prog_relief,
    ),
    (
        "+Prog Relief",
        lambda c: c.page_reg_cache
        and c.map_path_cache
        and c.meta_cache_slots == 4
        and c.prog_relief,
    ),
]


@dataclass
class CacheStats:
    l1_total: float = 0.0
    l1_hits: float = 0.0
    l2_hits: float = 0.0
    l2_misses: float = 0.0
    l3_calls: float = 0.0
    l3_hits: float = 0.0
    l3_levels_skipped: float = 0.0


@dataclass
class OperationStats:
    mean_kbps: float
    min_kbps: float
    max_kbps: float
    stddev_kbps: float
    latency_min: float
    latency_mean: float
    latency_stddev: float
    latency_p95: float
    latency_max: float
    histogram_labels: list[str]
    histogram_counts: list[float]
    cache: CacheStats


@dataclass
class AggregatedConfig:
    index: int
    key: ConfigKey
    label: str
    code: str
    filenames: list[str]
    schemas: set[str]
    benchmarks: dict[str, dict[str, OperationStats]]

    def cache_totals(self) -> CacheStats:
        total = CacheStats()
        for ops in self.benchmarks.values():
            for stats in ops.values():
                total.l1_total += stats.cache.l1_total
                total.l1_hits += stats.cache.l1_hits
                total.l2_hits += stats.cache.l2_hits
                total.l2_misses += stats.cache.l2_misses
                total.l3_calls += stats.cache.l3_calls
                total.l3_hits += stats.cache.l3_hits
                total.l3_levels_skipped += stats.cache.l3_levels_skipped
        return total


def thesis_figsize(fraction=1.0, aspect: Union[str, float] = "golden"):
    width = THESIS_TEXTWIDTH_IN * fraction
    if aspect == "golden":
        ratio = 0.618
    elif aspect == "4:3":
        ratio = 0.75
    elif aspect == "wide":
        ratio = 0.5
    else:
        ratio = float(aspect)
    return width, width * ratio


def configure_matplotlib(use_tex: bool):
    plt.rcParams.update(
        {
            "text.usetex": use_tex,
            "font.family": "serif",
            "font.serif": ["Computer Modern Roman"] if use_tex else ["Computer Modern Roman", "DejaVu Serif"],
            "font.size": 9,
            "axes.labelsize": 10,
            "axes.titlesize": 11,
            "xtick.labelsize": 8,
            "ytick.labelsize": 8,
            "legend.fontsize": 8,
            "savefig.dpi": 300,
            "figure.constrained_layout.use": True,
            "savefig.bbox": "tight",
            "savefig.pad_inches": 0.04,
        }
    )


def sort_config_key(item: tuple[ConfigKey, list[RunInfo]]):
    key = item[0]
    return (
        key.io_mode,
        not key.page_reg_cache,
        not key.map_path_cache,
        key.meta_cache_slots,
        -key.radix_depth,
        not key.prog_relief,
        key.verify_write,
    )


def config_label(key: ConfigKey) -> str:
    return (
        f"PRc={'Y' if key.page_reg_cache else 'N'} "
        f"MPc={'Y' if key.map_path_cache else 'N'} "
        f"Met={key.meta_cache_slots} "
        f"Rdx={key.radix_depth} "
        f"Rlf={'Y' if key.prog_relief else 'N'}"
    )


def safe_ratio(numerator: float, denominator: float) -> float:
    return numerator / denominator if denominator else 0.0


def format_pct(value: float) -> str:
    return f"{value * 100:.1f}%"


def mean_of(values: list[float]) -> float:
    present = [v for v in values if v is not None]
    return sum(present) / len(present) if present else 0.0


def load_payloads(results_dir: Path, runs: list[RunInfo]) -> dict[str, dict]:
    payloads = {}
    for run in runs:
        payloads[run.filename] = json.loads((results_dir / run.filename).read_text(encoding="utf-8"))
    return payloads


def average_operation(entries: list[dict]) -> OperationStats:
    first = entries[0]
    hist_labels = [bucket.get("label", "") for bucket in first.get("histogram", [])]
    hist_len = len(hist_labels)

    def get_latency(entry: dict, field: str) -> float:
        return float(entry.get("latency_us", {}).get(field, 0.0))

    cache_entries = [entry.get("cache_stats", {}) for entry in entries]
    histogram_counts = []
    for idx in range(hist_len):
        histogram_counts.append(
            mean_of(
                [
                    float(entry.get("histogram", [])[idx].get("count", 0.0))
                    for entry in entries
                    if idx < len(entry.get("histogram", []))
                ]
            )
        )

    cache = CacheStats(
        l1_total=mean_of([float(c.get("l1_total", 0.0)) for c in cache_entries]),
        l1_hits=mean_of([float(c.get("l1_hits", 0.0)) for c in cache_entries]),
        l2_hits=mean_of([float(c.get("l2_hits", 0.0)) for c in cache_entries]),
        l2_misses=mean_of([float(c.get("l2_misses", 0.0)) for c in cache_entries]),
        l3_calls=mean_of([float(c.get("l3_calls", 0.0)) for c in cache_entries]),
        l3_hits=mean_of([float(c.get("l3_hits", 0.0)) for c in cache_entries]),
        l3_levels_skipped=mean_of([float(c.get("l3_levels_skipped", 0.0)) for c in cache_entries]),
    )
    return OperationStats(
        mean_kbps=mean_of([float(entry.get("mean_kbps", 0.0)) for entry in entries]),
        min_kbps=mean_of([float(entry.get("min_kbps", 0.0)) for entry in entries]),
        max_kbps=mean_of([float(entry.get("max_kbps", 0.0)) for entry in entries]),
        stddev_kbps=mean_of([float(entry.get("stddev_kbps", 0.0)) for entry in entries]),
        latency_min=mean_of([get_latency(entry, "min") for entry in entries]),
        latency_mean=mean_of([get_latency(entry, "mean") for entry in entries]),
        latency_stddev=mean_of([get_latency(entry, "stddev") for entry in entries]),
        latency_p95=mean_of([get_latency(entry, "p95") for entry in entries]),
        latency_max=mean_of([get_latency(entry, "max") for entry in entries]),
        histogram_labels=hist_labels,
        histogram_counts=histogram_counts,
        cache=cache,
    )


def build_aggregates(results_dir: Path, runs: list[RunInfo]) -> list[AggregatedConfig]:
    payloads = load_payloads(results_dir, runs)
    groups = group_by_config(runs)
    aggregates = []
    for index, (key, group_runs) in enumerate(sorted(groups.items(), key=sort_config_key), start=1):
        by_benchmark = {}
        for benchmark in BENCHMARKS:
            ops = {}
            for direction in DIRECTIONS:
                entries = []
                for run in group_runs:
                    payload = payloads[run.filename]
                    result = next(
                        (item for item in payload.get("results", []) if item.get("name") == benchmark),
                        None,
                    )
                    if not result or direction not in result:
                        continue
                    op_entry = dict(result[direction])
                    op_entry["cache_stats"] = result.get(f"cache_stats_{direction}", {})
                    entries.append(op_entry)
                if entries:
                    ops[direction] = average_operation(entries)
            if ops:
                by_benchmark[benchmark] = ops
        aggregates.append(
            AggregatedConfig(
                index=index,
                key=key,
                label=config_label(key),
                code=f"#{index}",
                filenames=[run.filename for run in group_runs],
                schemas={run.schema for run in group_runs},
                benchmarks=by_benchmark,
            )
        )
    return aggregates


def ensure_output_dir(path: Path):
    path.mkdir(parents=True, exist_ok=True)


def save_figure(fig, output_dir: Path, base_name: str) -> list[Path]:
    outputs = []
    for suffix in (".pdf", ".png"):
        out_path = output_dir / f"{base_name}{suffix}"
        fig.savefig(out_path)
        outputs.append(out_path)
    plt.close(fig)
    return outputs


def add_value_labels(ax, bars, values, fmt="{:.0f}", rotation=90, fontsize=7):
    ymax = max(values) if values else 0.0
    pad = ymax * 0.015 if ymax else 0.5
    for bar, value in zip(bars, values):
        ax.annotate(
            fmt.format(value),
            (bar.get_x() + bar.get_width() / 2, bar.get_height()),
            xytext=(0, 2 + pad),
            textcoords="offset points",
            ha="center",
            va="bottom",
            rotation=rotation,
            fontsize=fontsize,
        )


def config_note(configs: list[AggregatedConfig], columns: int = 2) -> str:
    items = [f"{cfg.code} {cfg.label}" for cfg in configs]
    if not items:
        return ""
    rows = math.ceil(len(items) / columns)
    blocks = [items[idx * rows : (idx + 1) * rows] for idx in range(columns)]
    width = max(len(item) for item in items) + 3
    lines = []
    for row in range(rows):
        pieces = []
        for block in blocks:
            if row < len(block):
                pieces.append(block[row].ljust(width))
        lines.append("".join(pieces).rstrip())
    return "\n".join(lines)


def add_config_note(fig, configs: list[AggregatedConfig], columns: int = 2):
    note = config_note(configs, columns=columns)
    if note:
        fig.text(0.01, 0.01, note, ha="left", va="bottom", fontsize=6, family="monospace")


HEATMAP_COLUMNS = [
    ("Sequential", "write", "Seq W"),
    ("Sequential", "read", "Seq R"),
    ("Random", "write", "Rnd W"),
    ("Random", "read", "Rnd R"),
    ("Zipf", "write", "Zipf W"),
    ("Zipf", "read", "Zipf R"),
]


def _filter_by_relief(configs: list[AggregatedConfig], relief: bool | None) -> list[AggregatedConfig]:
    if relief is None:
        return configs
    return [cfg for cfg in configs if cfg.key.prog_relief == relief]


def render_heatmap(configs: list[AggregatedConfig], output_dir: Path, relief: bool | None = None, file_token: str = "1"):
    filtered = _filter_by_relief(configs, relief)
    matrix = np.array(
        [
            [cfg.benchmarks.get(bench, {}).get(direction, OperationStats(0, 0, 0, 0, 0, 0, 0, 0, 0, [], [], CacheStats())).mean_kbps for bench, direction, _ in HEATMAP_COLUMNS]
            for cfg in filtered
        ],
        dtype=float,
    )
    flat = matrix[np.isfinite(matrix)]
    median = float(np.median(flat)) if flat.size else 0.0
    if flat.size and np.min(flat) != np.max(flat):
        norm = mcolors.TwoSlopeNorm(vmin=float(np.min(flat)), vcenter=median, vmax=float(np.max(flat)))
    else:
        norm = None
    width, _ = thesis_figsize()
    height = max(2.4, 0.34 * len(filtered) + 1.2)
    fig, ax = plt.subplots(figsize=(width, height), layout="constrained")
    im = ax.imshow(matrix, aspect="auto", cmap="RdBu_r", norm=norm)
    ax.set_xticks(range(len(HEATMAP_COLUMNS)), [label for _, _, label in HEATMAP_COLUMNS])
    ax.set_yticks(range(len(filtered)), [f"{cfg.code} {cfg.label}" for cfg in filtered])
    title = "Mean throughput overview across benchmark classes"
    if relief is not None:
        title += f" — Prog Relief {'Y' if relief else 'N'}"
    ax.set_title(title)
    cbar = fig.colorbar(im, ax=ax, shrink=0.9)
    cbar.set_label("Throughput (kB/s)")
    for row in range(matrix.shape[0]):
        for col in range(matrix.shape[1]):
            ax.text(col, row, f"{matrix[row, col]:.0f}", ha="center", va="center", fontsize=7)
    return save_figure(fig, output_dir, FIGURE_FILES[file_token])


def render_latency_heatmap(configs: list[AggregatedConfig], output_dir: Path, relief: bool | None = None, file_token: str = "1b"):
    filtered = _filter_by_relief(configs, relief)
    matrix = np.array(
        [
            [cfg.benchmarks.get(bench, {}).get(direction, OperationStats(0, 0, 0, 0, 0, 0, 0, 0, 0, [], [], CacheStats())).latency_p95 for bench, direction, _ in HEATMAP_COLUMNS]
            for cfg in filtered
        ],
        dtype=float,
    )
    flat = matrix[np.isfinite(matrix)]
    median = float(np.median(flat)) if flat.size else 0.0
    if flat.size and np.min(flat) != np.max(flat):
        norm = mcolors.TwoSlopeNorm(vmin=float(np.min(flat)), vcenter=median, vmax=float(np.max(flat)))
    else:
        norm = None
    width, _ = thesis_figsize()
    height = max(2.4, 0.34 * len(filtered) + 1.2)
    fig, ax = plt.subplots(figsize=(width, height), layout="constrained")
    im = ax.imshow(matrix, aspect="auto", cmap="RdYlGn_r", norm=norm)
    ax.set_xticks(range(len(HEATMAP_COLUMNS)), [label for _, _, label in HEATMAP_COLUMNS])
    ax.set_yticks(range(len(filtered)), [f"{cfg.code} {cfg.label}" for cfg in filtered])
    title = "p95 latency overview across benchmark classes"
    if relief is not None:
        title += f" — Prog Relief {'Y' if relief else 'N'}"
    ax.set_title(title)
    cbar = fig.colorbar(im, ax=ax, shrink=0.9)
    cbar.set_label("Latency p95 (µs)")
    for row in range(matrix.shape[0]):
        for col in range(matrix.shape[1]):
            val = matrix[row, col]
            brightness = (val - flat.min()) / (flat.max() - flat.min()) if flat.size and flat.max() != flat.min() else 0.5
            text_color = "white" if brightness > 0.55 else "black"
            ax.text(col, row, f"{val:.0f}", ha="center", va="center", fontsize=7, color=text_color)
    return save_figure(fig, output_dir, FIGURE_FILES[file_token])


def render_throughput_bars(configs: list[AggregatedConfig], benchmark: str, token: str, output_dir: Path):
    ordered = sorted(configs, key=lambda cfg: cfg.benchmarks[benchmark]["write"].mean_kbps)
    labels = [f"{cfg.code}\n{cfg.label}" for cfg in ordered]
    x = np.arange(len(ordered))
    width, height = thesis_figsize(aspect=max(0.72, 0.18 * len(ordered)))
    fig, ax = plt.subplots(figsize=(width, height), layout="constrained")
    write_vals = [cfg.benchmarks[benchmark]["write"].mean_kbps for cfg in ordered]
    read_vals = [cfg.benchmarks[benchmark]["read"].mean_kbps for cfg in ordered]
    bar_width = 0.38
    write_bars = ax.bar(x - bar_width / 2, write_vals, width=bar_width, color=BAR_COLORS["write"], label="Write")
    read_bars = ax.bar(x + bar_width / 2, read_vals, width=bar_width, color=BAR_COLORS["read"], label="Read")
    add_value_labels(ax, write_bars, write_vals)
    add_value_labels(ax, read_bars, read_vals)
    ax.set_title(f"{benchmark} throughput by configuration")
    ax.set_ylabel("Throughput (kB/s)")
    ax.set_xticks(x, labels, rotation=45, ha="right")
    ax.grid(axis="y", alpha=0.25)
    ax.legend(ncols=2)
    return save_figure(fig, output_dir, FIGURE_FILES[token])


def select_waterfall_configs(configs: list[AggregatedConfig]) -> tuple[list[tuple[str, AggregatedConfig]], list[str]]:
    selected = []
    missing = []
    for label, predicate in WATERFALL_STEPS:
        candidates = [cfg for cfg in configs if predicate(cfg.key)]
        if not candidates:
            missing.append(label)
            continue
        selected.append((label, sorted(candidates, key=lambda cfg: sort_config_key((cfg.key, [])))[0]))
    return selected, missing


def render_waterfall(configs: list[AggregatedConfig], benchmark: str, token: str, output_dir: Path):
    selected, missing = select_waterfall_configs(configs)
    n_steps = max(len(selected), 1)
    step_height = 0.55
    panel_height = n_steps * step_height + 0.8
    total_height = panel_height * 2 + 0.6
    width, _ = thesis_figsize()
    fig, axes = plt.subplots(2, 1, figsize=(width, total_height), layout="constrained")
    for ax, direction in zip(axes, DIRECTIONS):
        names = []
        values = []
        codes = []
        for step_name, cfg in selected:
            if benchmark in cfg.benchmarks and direction in cfg.benchmarks[benchmark]:
                names.append(step_name)
                values.append(cfg.benchmarks[benchmark][direction].mean_kbps)
                codes.append(cfg.code)
        if not values:
            ax.text(0.5, 0.5, "No matching data", ha="center", va="center")
            ax.set_axis_off()
            continue
        y = np.arange(len(values) - 1, -1, -1)
        max_val = max(values) if values else 1.0
        label_x = max_val * 1.02
        prev_value = None
        for idx, value in enumerate(values):
            color = STEP_COLORS[min(idx, len(STEP_COLORS) - 1)]
            ax.barh(y[idx], value, height=0.6, color=color, edgecolor="white", linewidth=0.5)
            bar_end = value
            ax.annotate(
                f"{value:.0f} kB/s",
                (bar_end, y[idx]),
                xytext=(6, 0),
                textcoords="offset points",
                va="center",
                fontsize=8,
            )
            if prev_value is not None:
                delta = value - prev_value
                pct = safe_ratio(delta, prev_value)
                sign = "+" if delta >= 0 else ""
                ax.annotate(
                    f"({sign}{pct:.1%})",
                    (bar_end, y[idx]),
                    xytext=(6, -10),
                    textcoords="offset points",
                    va="center",
                    fontsize=7,
                    color="#555555",
                )
            prev_value = value
        ax.set_yticks(y, [f"{name}  {code}" for name, code in zip(names, codes)])
        ax.set_xlim(0, max_val * 1.35)
        ax.set_xlabel("Throughput (kB/s)")
        ax.grid(axis="x", alpha=0.25)
        ax.set_title(f"{direction.capitalize()}")
    fig.suptitle(f"{benchmark} — additive throughput progression")
    if missing:
        fig.text(0.5, 0.01, f"Missing steps: {', '.join(missing)}", ha="center", fontsize=7, color="#888888")
    return save_figure(fig, output_dir, FIGURE_FILES[token])


def render_latency_bars(configs: list[AggregatedConfig], benchmark: str, token: str, output_dir: Path):
    ordered = sorted(configs, key=lambda cfg: cfg.benchmarks[benchmark]["write"].latency_p95)
    x = np.arange(len(ordered))
    labels = [f"{cfg.code}\n{cfg.label}" for cfg in ordered]
    width, height = thesis_figsize(aspect=max(0.85, 0.2 * len(ordered)))
    fig, axes = plt.subplots(2, 1, figsize=(width, height), sharex=True, layout="constrained")
    all_p95 = []
    for direction in DIRECTIONS:
        all_p95.extend([cfg.benchmarks[benchmark][direction].latency_p95 for cfg in ordered])
    min_positive = min(v for v in all_p95 if v > 0)
    use_log = max(all_p95) / min_positive > 10 if min_positive else False
    for ax, direction in zip(axes, DIRECTIONS):
        stats = [cfg.benchmarks[benchmark][direction] for cfg in ordered]
        p95 = [stat.latency_p95 for stat in stats]
        lower = [max(0.0, stat.latency_p95 - stat.latency_min) for stat in stats]
        upper = [max(0.0, stat.latency_max - stat.latency_p95) for stat in stats]
        bars = ax.bar(x, p95, color=BAR_COLORS[direction], yerr=np.vstack([lower, upper]), capsize=3)
        add_value_labels(ax, bars, p95, fmt="{:.0f}", rotation=90)
        ax.set_ylabel(f"{direction.capitalize()}\np95 (µs)")
        ax.grid(axis="y", alpha=0.25)
        if use_log:
            ax.set_yscale("log")
    axes[0].set_title(f"{benchmark} p95 latency with min/max whiskers")
    axes[-1].set_xticks(x, labels, rotation=45, ha="right")
    return save_figure(fig, output_dir, FIGURE_FILES[token])


def normalized_histogram(stats: OperationStats) -> list[float]:
    total = sum(stats.histogram_counts)
    return [safe_ratio(count, total) * 100.0 for count in stats.histogram_counts]


def render_histograms(configs: list[AggregatedConfig], benchmark: str, token: str, output_dir: Path):
    width, height = thesis_figsize(aspect=1.02)
    fig, axes = plt.subplots(2, 1, figsize=(width, height), sharex=True, layout="constrained")
    palette = list(plt.get_cmap("tab20").colors)
    for ax, direction in zip(axes, DIRECTIONS):
        labels = None
        for idx, cfg in enumerate(configs):
            stats = cfg.benchmarks[benchmark][direction]
            labels = stats.histogram_labels
            values = normalized_histogram(stats)
            x = np.arange(len(values))
            ax.fill_between(x, values, step="mid", alpha=0.18, color=palette[idx % len(palette)])
            ax.plot(x, values, marker="o", linewidth=1.1, markersize=2.5, color=palette[idx % len(palette)], label=cfg.code)
        if labels:
            ax.set_xticks(range(len(labels)), labels, rotation=25, ha="right")
        ax.set_ylabel(f"{direction.capitalize()}\nshare (%)")
        ax.grid(axis="y", alpha=0.25)
    axes[0].set_title(f"{benchmark} latency distribution by configuration")
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncols=min(5, len(configs)), title="Config")
    add_config_note(fig, configs, columns=2)
    return save_figure(fig, output_dir, FIGURE_FILES[token])


def render_cache_hit_rates(configs: list[AggregatedConfig], output_dir: Path):
    width, height = thesis_figsize(aspect=1.08)
    fig, axes = plt.subplots(3, 1, figsize=(width, height), layout="constrained")
    panels = [
        ("L1", lambda c: c.l1_total > 0, lambda c: safe_ratio(c.l1_hits, c.l1_total), None),
        ("L2", lambda c: (c.l2_hits + c.l2_misses) > 0, lambda c: safe_ratio(c.l2_hits, c.l2_hits + c.l2_misses), None),
        (
            "L3",
            lambda c: c.l3_calls > 0,
            lambda c: safe_ratio(c.l3_hits, c.l3_calls),
            lambda c: safe_ratio(c.l3_levels_skipped, c.l3_calls),
        ),
    ]
    for ax, (title, include, rate_fn, extra_fn) in zip(axes, panels):
        chosen = [(cfg, cfg.cache_totals()) for cfg in configs]
        chosen = [(cfg, cache) for cfg, cache in chosen if include(cache)]
        if not chosen:
            ax.text(0.5, 0.5, f"No {title} data", ha="center", va="center")
            ax.set_axis_off()
            continue
        x = np.arange(len(chosen))
        rates = [rate_fn(cache) for _, cache in chosen]
        bars = ax.bar(x, rates, color="#54A24B")
        ax.set_ylim(0.0, 1.08)
        ax.set_ylabel(f"{title} hit rate")
        ax.set_xticks(x, [cfg.code for cfg, _ in chosen])
        ax.grid(axis="y", alpha=0.25)
        for bar, rate, (cfg, cache) in zip(bars, rates, chosen):
            text = format_pct(rate)
            if extra_fn is not None:
                text += f"\nskip={extra_fn(cache):.1f}"
            ax.annotate(text, (bar.get_x() + bar.get_width() / 2, bar.get_height()), xytext=(0, 2), textcoords="offset points", ha="center", va="bottom", fontsize=7)
        ax.set_title(title)
    fig.suptitle("Average cache hit rates across benchmark runs")
    add_config_note(fig, configs, columns=2)
    return save_figure(fig, output_dir, FIGURE_FILES["6"])


def pareto_frontier(points: list[tuple[float, float]]):
    frontier = []
    best_latency = math.inf
    for throughput, latency in sorted(points, key=lambda item: item[0], reverse=True):
        if latency < best_latency:
            frontier.append((throughput, latency))
            best_latency = latency
    return sorted(frontier, key=lambda item: item[0])


def render_tradeoff_scatter(configs: list[AggregatedConfig], output_dir: Path):
    width, height = thesis_figsize(aspect=1.05)
    fig, axes = plt.subplots(3, 1, figsize=(width, height), layout="constrained")
    meta_values = sorted({cfg.key.meta_cache_slots for cfg in configs})
    cmap = plt.get_cmap("viridis")
    meta_colors = {meta: cmap(idx / max(1, len(meta_values) - 1)) for idx, meta in enumerate(meta_values)}
    for ax, benchmark in zip(axes, BENCHMARKS):
        points = []
        for cfg in configs:
            stats = cfg.benchmarks.get(benchmark, {}).get("write")
            if not stats:
                continue
            ax.scatter(
                stats.mean_kbps,
                stats.latency_p95,
                s=48,
                marker=SCATTER_MARKERS[cfg.key.map_path_cache],
                color=meta_colors[cfg.key.meta_cache_slots],
                edgecolors="#222222",
                linewidths=0.4,
            )
            ax.annotate(cfg.code, (stats.mean_kbps, stats.latency_p95), xytext=(4, 2), textcoords="offset points", fontsize=7)
            points.append((stats.mean_kbps, stats.latency_p95))
        frontier = pareto_frontier(points)
        if len(frontier) >= 2:
            ax.plot([p[0] for p in frontier], [p[1] for p in frontier], linestyle="--", color="#444444", linewidth=1)
        ax.set_title(benchmark)
        ax.set_xlabel("Write throughput (kB/s)")
        ax.set_ylabel("Write p95 latency (µs)")
        ax.grid(alpha=0.25)
    handles = []
    for meta in meta_values:
        handles.append(plt.Line2D([0], [0], marker="o", linestyle="", color=meta_colors[meta], label=f"Met={meta}"))
    handles.extend(
        [
            plt.Line2D([0], [0], marker=SCATTER_MARKERS[False], linestyle="", color="#666666", label="MPc=N"),
            plt.Line2D([0], [0], marker=SCATTER_MARKERS[True], linestyle="", color="#666666", label="MPc=Y"),
        ]
    )
    fig.legend(handles=handles, loc="upper center", ncols=min(5, len(handles)))
    fig.suptitle("Throughput versus latency trade-off")
    add_config_note(fig, configs, columns=2)
    return save_figure(fig, output_dir, FIGURE_FILES["7"])


def list_configs(configs: list[AggregatedConfig]):
    print(f"Found {len(configs)} unique configs")
    for cfg in configs:
        schemas = ",".join(sorted(s.replace("esp_nand_perf_", "") for s in cfg.schemas))
        print(f"{cfg.code:<4} {cfg.label}  runs={len(cfg.filenames)}  schema={schemas}")


def expand_figures(tokens: list[str] | None) -> list[str]:
    if not tokens:
        return list(FIGURE_FILES.keys())
    expanded = []
    for token in tokens:
        if token in FIGURE_GROUPS:
            expanded.extend(FIGURE_GROUPS[token])
        elif token in FIGURE_FILES:
            expanded.append(token)
        else:
            raise ValueError(f"Unknown figure selector: {token}")
    seen = set()
    ordered = []
    for token in expanded:
        if token not in seen:
            ordered.append(token)
            seen.add(token)
    return ordered


def generate_figures(selected: list[str], configs: list[AggregatedConfig], output_dir: Path) -> list[Path]:
    generated = []
    benchmark_token_map = {"2a": "Sequential", "2b": "Random", "2c": "Zipf", "3a": "Sequential", "3b": "Random", "3c": "Zipf", "4a": "Sequential", "4b": "Random", "4c": "Zipf", "5a": "Sequential", "5b": "Random", "5c": "Zipf"}
    for token in selected:
        if token == "1":
            generated.extend(render_heatmap(configs, output_dir))
        elif token == "1a":
            generated.extend(render_heatmap(configs, output_dir, relief=False, file_token="1a"))
        elif token == "1c":
            generated.extend(render_heatmap(configs, output_dir, relief=True, file_token="1c"))
        elif token == "1b":
            generated.extend(render_latency_heatmap(configs, output_dir))
        elif token == "1d":
            generated.extend(render_latency_heatmap(configs, output_dir, relief=False, file_token="1d"))
        elif token == "1e":
            generated.extend(render_latency_heatmap(configs, output_dir, relief=True, file_token="1e"))
        elif token.startswith("2"):
            generated.extend(render_throughput_bars(configs, benchmark_token_map[token], token, output_dir))
        elif token.startswith("3"):
            generated.extend(render_waterfall(configs, benchmark_token_map[token], token, output_dir))
        elif token.startswith("4"):
            generated.extend(render_latency_bars(configs, benchmark_token_map[token], token, output_dir))
        elif token.startswith("5"):
            generated.extend(render_histograms(configs, benchmark_token_map[token], token, output_dir))
        elif token == "6":
            generated.extend(render_cache_hit_rates(configs, output_dir))
        elif token == "7":
            generated.extend(render_tradeoff_scatter(configs, output_dir))
    return generated


def parse_args():
    parser = argparse.ArgumentParser(description="Create publication-quality SPI NAND benchmark figures")
    script_dir = Path(__file__).resolve().parent
    parser.add_argument("--results-dir", type=Path, default=script_dir / "perf_results")
    parser.add_argument("--output-dir", type=Path, default=script_dir / "perf_results" / "figures")
    parser.add_argument("--io-mode", choices=["SIO", "DIO"], default="SIO")
    parser.add_argument("--figures", nargs="*", default=None)
    parser.add_argument("--no-tex", action="store_true")
    parser.add_argument("--list", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    configure_matplotlib(use_tex=not args.no_tex)

    if not args.results_dir.is_dir():
        print(f"Error: {args.results_dir} is not a directory", file=sys.stderr)
        return 1

    runs = load_results(args.results_dir)
    if args.io_mode:
        runs = [run for run in runs if run.config_key.io_mode == args.io_mode]
    if not runs:
        print(f"No run_*.json files found for io_mode={args.io_mode}", file=sys.stderr)
        return 1

    configs = build_aggregates(args.results_dir, runs)
    if args.list:
        list_configs(configs)
        return 0

    try:
        selected = expand_figures(args.figures)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    ensure_output_dir(args.output_dir)
    generated = generate_figures(selected, configs, args.output_dir)
    print(f"Generated {len(generated) // 2} figure bases for io_mode={args.io_mode} in {args.output_dir}")
    for path in generated:
        print(f"  - {path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
