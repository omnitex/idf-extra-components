#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Martin Havlik <omnitex.git@gmail.com>
#
# SPDX-License-Identifier: Apache-2.0
"""Create thesis-ready figures from ftl_eval report_*.json files.

Saves PDF and PNG outputs. Follows the same styling conventions as perf_viz.py.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import matplotlib

matplotlib.use("Agg")

import matplotlib.colors as mcolors
import matplotlib.pyplot as plt
import numpy as np

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

THESIS_TEXTWIDTH_IN = 5.984

GC_ORDER = [5, 10, 15, 20, 25]
GC_LABELS = [f"{v}%" for v in GC_ORDER]

WEAR_ORDER = ["fresh", "life_10pct", "life_25pct", "life_50pct", "life_75pct", "life_90pct"]
WEAR_LABELS = ["Fresh", "10%", "25%", "50%", "75%", "90%"]

NOISE_ORDER = ["no_noise", "noise_0.01", "noise_0.05", "noise_0.10", "noise_0.25"]
NOISE_LABELS = ["0", "0.01", "0.05", "0.10", "0.25"]

WORKLOAD_PANELS = [
    ("report_gc_vs_wear_sequential_monotonic.json", "Sequential"),
    ("report_gc_vs_wear_random_monotonic.json", "Random"),
    ("report_gc_vs_wear_monotonic.json", "Zipf (skew=1.0)"),
]

FIGURE_FILES = {
    "1": "fig_waf_heatmap",
    "2": "fig_noise_degradation",
}

# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------


@dataclass
class SweepResult:
    scenario: str
    ftl_config: str
    status: str
    metrics: dict[str, Any]


def parse_gc_percent(ftl_config: str) -> int:
    """Extract gc_overhead percent from config name like 'dhara_20pct'."""
    return int(ftl_config.split("_")[1].replace("pct", ""))


def load_report(path: Path) -> tuple[str, list[SweepResult]]:
    """Load a single report JSON, return (sweep_name, results)."""
    data = json.loads(path.read_text(encoding="utf-8"))
    results = []
    for entry in data.get("results", []):
        results.append(
            SweepResult(
                scenario=entry["scenario"],
                ftl_config=entry["ftl_config"],
                status=entry["status"],
                metrics=entry.get("metrics", {}),
            )
        )
    return data.get("sweep", ""), results


def build_matrix(
    results: list[SweepResult],
    row_key_fn,
    col_key_fn,
    metric: str,
    row_order: list,
    col_order: list,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Build value/status/writes_succeeded matrices from flat results.

    Returns:
        values: float matrix (rows × cols)
        failed: bool matrix
        success_pct: float matrix (writes_succeeded / writes_attempted × 100)
    """
    idx = {}
    for r in results:
        idx[(row_key_fn(r), col_key_fn(r))] = r

    n_rows = len(row_order)
    n_cols = len(col_order)
    values = np.full((n_rows, n_cols), np.nan)
    failed = np.zeros((n_rows, n_cols), dtype=bool)
    success_pct = np.full((n_rows, n_cols), np.nan)

    for i, row_key in enumerate(row_order):
        for j, col_key in enumerate(col_order):
            r = idx.get((row_key, col_key))
            if r is None:
                continue
            values[i, j] = r.metrics.get(metric, np.nan)
            failed[i, j] = r.status == "failed"
            attempted = r.metrics.get("writes_attempted", 0)
            succeeded = r.metrics.get("writes_succeeded", 0)
            if attempted > 0:
                success_pct[i, j] = succeeded / attempted * 100.0

    return values, failed, success_pct


# ---------------------------------------------------------------------------
# Styling
# ---------------------------------------------------------------------------


def thesis_figsize(fraction=1.0, aspect="golden"):
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


def save_figure(fig, output_dir: Path, base_name: str) -> list[Path]:
    outputs = []
    for suffix in (".pdf", ".png"):
        out_path = output_dir / f"{base_name}{suffix}"
        fig.savefig(out_path)
        outputs.append(out_path)
    plt.close(fig)
    return outputs


# ---------------------------------------------------------------------------
# Figure 1: WAF heatmap (3 panels)
# ---------------------------------------------------------------------------


def render_waf_heatmap(reports_dir: Path, output_dir: Path) -> list[Path]:
    n_panels = len(WORKLOAD_PANELS)
    row_height = 1.1
    total_height = n_panels * row_height + 0.4
    width, _ = thesis_figsize(fraction=1.0)

    fig, axes = plt.subplots(n_panels, 1, figsize=(width, total_height), layout="constrained")

    all_wafs = []
    all_failed = []
    for filename, _title in WORKLOAD_PANELS:
        path = reports_dir / filename
        if not path.exists():
            print(f"  WARN: {filename} not found, skipping", file=sys.stderr)
            continue
        _, results = load_report(path)
        vals, fail, _ = build_matrix(
            results,
            row_key_fn=lambda r: parse_gc_percent(r.ftl_config),
            col_key_fn=lambda r: r.scenario,
            metric="write_amplification_factor",
            row_order=GC_ORDER,
            col_order=WEAR_ORDER,
        )
        mask = ~fail
        if mask.any():
            all_wafs.extend(vals[mask].tolist())
        all_failed.append(fail)

    if not all_wafs:
        print("  ERROR: no WAF data found", file=sys.stderr)
        plt.close(fig)
        return []

    vmin = min(all_wafs)
    vmax = max(all_wafs)
    vcenter = (vmin + vmax) / 2.0

    if vmin != vmax:
        norm = mcolors.TwoSlopeNorm(vmin=vmin, vcenter=vcenter, vmax=vmax)
    else:
        norm = None

    for panel_idx, (filename, title) in enumerate(WORKLOAD_PANELS):
        ax = axes[panel_idx] if n_panels > 1 else axes
        path = reports_dir / filename
        if not path.exists():
            ax.set_axis_off()
            ax.text(0.5, 0.5, f"{filename}\nnot found", ha="center", va="center", transform=ax.transAxes)
            continue

        _, results = load_report(path)
        waf, failed, success_pct = build_matrix(
            results,
            row_key_fn=lambda r: parse_gc_percent(r.ftl_config),
            col_key_fn=lambda r: r.scenario,
            metric="write_amplification_factor",
            row_order=GC_ORDER,
            col_order=WEAR_ORDER,
        )

        im = ax.imshow(waf, aspect="auto", cmap="RdYlGn_r", norm=norm)

        # Annotations
        for i in range(waf.shape[0]):
            for j in range(waf.shape[1]):
                val = waf[i, j]
                if np.isnan(val):
                    continue
                if failed[i, j]:
                    sp = success_pct[i, j]
                    ax.text(j, i, f"{sp:.0f}%", ha="center", va="center", fontsize=7, color="#CC0000", fontweight="bold")
                else:
                    brightness = (val - vmin) / (vmax - vmin) if vmax != vmin else 0.5
                    text_color = "white" if brightness > 0.55 else "black"
                    ax.text(j, i, f"{val:.2f}", ha="center", va="center", fontsize=7, color=text_color)

        ax.set_xticks(range(len(WEAR_ORDER)), WEAR_LABELS)
        ax.set_yticks(range(len(GC_ORDER)), GC_LABELS)
        ax.set_title(title, fontsize=10)
        ax.set_xlabel("NAND wear level" if panel_idx == n_panels - 1 else "")
        if panel_idx == 0:
            ax.set_ylabel("GC overhead")

    fig.suptitle("Write amplification factor across GC overhead and NAND wear level", fontsize=11)
    cbar = fig.colorbar(im, ax=axes if n_panels > 1 else [axes], shrink=0.8, location="right")
    cbar.set_label("WAF")

    return save_figure(fig, output_dir, FIGURE_FILES["1"])


# ---------------------------------------------------------------------------
# Figure 2: Noise degradation (2 panels)
# ---------------------------------------------------------------------------

NOISE_GC_ORDER = [10, 15, 20, 25]
NOISE_GC_LABELS = [f"GC {v}%" for v in NOISE_GC_ORDER]
BAR_COLORS = {"GC 10%": "#e41a1c", "GC 15%": "#377eb8", "GC 20%": "#4daf4a", "GC 25%": "#984ea3"}


def render_noise_degradation(reports_dir: Path, output_dir: Path) -> list[Path]:
    path = reports_dir / "report_noise_vs_gc.json"
    if not path.exists():
        print("  ERROR: report_noise_vs_gc.json not found", file=sys.stderr)
        return []

    _, results = load_report(path)

    writes_ok, _, _ = build_matrix(
        results,
        row_key_fn=lambda r: parse_gc_percent(r.ftl_config),
        col_key_fn=lambda r: r.scenario,
        metric="writes_succeeded",
        row_order=NOISE_GC_ORDER,
        col_order=NOISE_ORDER,
    )
    ftl_errors, _, _ = build_matrix(
        results,
        row_key_fn=lambda r: parse_gc_percent(r.ftl_config),
        col_key_fn=lambda r: r.scenario,
        metric="ftl_errors",
        row_order=NOISE_GC_ORDER,
        col_order=NOISE_ORDER,
    )

    width, _ = thesis_figsize(fraction=1.0, aspect=1.2)
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(width, width * 0.55), layout="constrained", sharey=False)

    n_noise = len(NOISE_ORDER)
    n_gc = len(NOISE_GC_ORDER)
    x = np.arange(n_noise)
    bar_width = 0.18
    offsets = np.arange(n_gc) - (n_gc - 1) / 2.0

    # Left panel: writes succeeded
    for gi, gc_pct in enumerate(NOISE_GC_ORDER):
        label = NOISE_GC_LABELS[gi]
        vals = writes_ok[gi, :] / 1000.0
        bars = ax1.bar(x + offsets[gi] * bar_width, vals, width=bar_width, label=label, color=BAR_COLORS[label])
        for bar, v in zip(bars, vals):
            if v < 99.0:
                ax1.annotate(
                    f"{v:.1f}k",
                    (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                    xytext=(0, 3),
                    textcoords="offset points",
                    ha="center",
                    va="bottom",
                    fontsize=6,
                    rotation=90,
                )

    ax1.set_xticks(x, NOISE_LABELS)
    ax1.set_xlabel("ECC noise probability")
    ax1.set_ylabel("Writes succeeded (thousands)")
    ax1.set_title("Write throughput under ECC noise")
    ax1.set_ylim(0, 110)
    ax1.axhline(y=100, color="#AAAAAA", linewidth=0.5, linestyle="--")
    ax1.grid(axis="y", alpha=0.25)

    # Right panel: ftl_errors
    for gi, gc_pct in enumerate(NOISE_GC_ORDER):
        label = NOISE_GC_LABELS[gi]
        vals = ftl_errors[gi, :] / 1000.0
        bars = ax2.bar(x + offsets[gi] * bar_width, vals, width=bar_width, label=label, color=BAR_COLORS[label])
        for bar, v in zip(bars, vals):
            if v > 0.1:
                ax2.annotate(
                    f"{v:.1f}k",
                    (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                    xytext=(0, 3),
                    textcoords="offset points",
                    ha="center",
                    va="bottom",
                    fontsize=6,
                    rotation=90,
                )

    ax2.set_xticks(x, NOISE_LABELS)
    ax2.set_xlabel("ECC noise probability")
    ax2.set_ylabel("FTL errors (thousands)")
    ax2.set_title("FTL errors under ECC noise")
    ax2.grid(axis="y", alpha=0.25)

    fig.legend(
        [plt.Rectangle((0, 0), 1, 1, fc=BAR_COLORS[l]) for l in NOISE_GC_LABELS],
        NOISE_GC_LABELS,
        loc="outside lower center",
        ncols=n_gc,
        title="GC overhead",
    )
    fig.suptitle("FTL reliability under increasing ECC noise probability (pre-worn 60%)", fontsize=11)

    return save_figure(fig, output_dir, FIGURE_FILES["2"])


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def parse_args():
    parser = argparse.ArgumentParser(description="Create thesis-ready FTL evaluation figures")
    script_dir = Path(__file__).resolve().parent
    parser.add_argument("--reports-dir", type=Path, default=script_dir)
    parser.add_argument("--output-dir", type=Path, default=script_dir / "figures")
    parser.add_argument("--figures", nargs="*", default=None, help="Figure tokens to generate (1, 2). Default: all")
    parser.add_argument("--no-tex", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    configure_matplotlib(use_tex=not args.no_tex)

    if not args.reports_dir.is_dir():
        print(f"Error: {args.reports_dir} is not a directory", file=sys.stderr)
        return 1

    wanted = set(args.figures) if args.figures else set(FIGURE_FILES.keys())
    for token in wanted:
        if token not in FIGURE_FILES:
            print(f"Error: unknown figure token '{token}'", file=sys.stderr)
            return 2

    args.output_dir.mkdir(parents=True, exist_ok=True)
    generated = []

    if "1" in wanted:
        print("Generating Figure 1: WAF heatmap ...")
        generated.extend(render_waf_heatmap(args.reports_dir, args.output_dir))

    if "2" in wanted:
        print("Generating Figure 2: Noise degradation ...")
        generated.extend(render_noise_degradation(args.reports_dir, args.output_dir))

    print(f"Generated {len(generated) // 2} figure bases in {args.output_dir}")
    for path in generated:
        print(f"  - {path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
