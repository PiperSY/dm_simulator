#!/usr/bin/env python3

"""Analyze dm_simulator matrix outputs and generate report-ready plots."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


INSTALL_MESSAGE = (
    "Matplotlib is required for Phase F plotting. Install analysis "
    "dependencies with: python3 -m pip install -r requirements-analysis.txt"
)

PREFERRED_POLICY_ORDER = (
    "always_remote",
    "lru",
    "hotness_only",
    "hotness_only_cumulative",
    "global_hottest_replication",
    "contention_aware",
    "contention_aware_v1",
    "contention_aware_smoothed",
    "contention_aware_reuse_gated",
    "contention_aware_hysteresis",
)

GROUP_COLUMNS = (
    "preset",
    "seed",
    "node_count",
    "epoch_count",
    "requests_per_node_per_epoch",
    "hot_set_mode",
    "hot_set_churn_fraction",
    "cross_node_overlap",
    "object_count",
    "hot_set_size",
    "hot_access_probability",
    "object_size_mode",
    "object_size_bytes",
    "object_size_small_bytes",
    "object_size_large_bytes",
    "large_object_probability",
    "cache_capacity_hotset_multiplier",
    "cache_capacity_bytes",
    "memory_bandwidth_level",
    "memory_bandwidth_bytes_per_time",
    "memory_base_latency_level",
    "memory_base_latency",
    "link_latency_level",
    "one_way_link_latency",
)

COMPARISON_COLUMNS = (
    *GROUP_COLUMNS,
    "policy",
    "baseline_policy",
    "baseline_available",
    "classification",
    "mean_latency",
    "baseline_mean_latency",
    "mean_latency_delta",
    "mean_latency_delta_pct",
    "p99_latency",
    "baseline_p99_latency",
    "p99_latency_delta",
    "p99_latency_delta_pct",
    "local_hit_rate",
    "baseline_local_hit_rate",
    "local_hit_rate_delta",
    "average_memory_wait",
    "baseline_average_memory_wait",
    "average_memory_wait_delta",
    "average_memory_wait_delta_pct",
    "total_remote_accesses",
    "baseline_total_remote_accesses",
    "total_remote_accesses_delta",
    "total_remote_accesses_delta_pct",
    "estimated_avoided_remote_accesses",
    "estimated_avoided_queue_wait",
    "estimated_avoided_remote_service_time",
    "estimated_avoided_contention_cost",
    "admission_yield",
    "reuse_after_admit_rate",
    "stale_telemetry_rate",
    "average_top_object_overlap",
    "eviction_regret_count",
    "remote_eviction_regret_count",
    "jain_inverse_latency_fairness",
    "baseline_jain_inverse_latency_fairness",
    "jain_inverse_latency_fairness_delta",
    "per_node_mean_latency_spread",
    "per_node_p99_latency_max",
    "source_aggregate",
    "experiment_name",
    "output_dir",
)

POLICY_SUMMARY_COLUMNS = (
    "policy",
    "run_count",
    "comparable_run_count",
    "win_count",
    "tie_count",
    "loss_count",
    "avg_mean_latency",
    "avg_p99_latency",
    "avg_local_hit_rate",
    "avg_average_memory_wait",
    "avg_total_remote_accesses",
    "avg_mean_latency_delta_pct",
    "best_mean_latency_delta_pct",
    "worst_mean_latency_delta_pct",
    "avg_p99_latency_delta_pct",
    "avg_local_hit_rate_delta",
    "avg_average_memory_wait_delta_pct",
    "avg_total_remote_accesses_delta_pct",
    "avg_admission_yield",
    "avg_reuse_after_admit_rate",
    "avg_stale_telemetry_rate",
    "avg_average_top_object_overlap",
    "avg_estimated_avoided_remote_accesses",
    "avg_estimated_avoided_contention_cost",
    "avg_eviction_regret_count",
    "avg_jain_inverse_latency_fairness",
    "avg_jain_inverse_latency_fairness_delta",
)

CONDITION_COLUMNS = (
    "dimension",
    "value",
    "policy",
    "run_count",
    "win_count",
    "tie_count",
    "loss_count",
    "avg_mean_latency",
    "avg_mean_latency_delta_pct",
    "avg_p99_latency_delta_pct",
    "avg_local_hit_rate",
    "avg_average_memory_wait",
    "avg_stale_telemetry_rate",
    "avg_estimated_avoided_contention_cost",
)

CONDITION_DIMENSIONS = (
    "hot_set_churn_fraction",
    "requests_per_node_per_epoch",
    "epoch_count",
    "cross_node_overlap",
    "object_count",
    "hot_set_size",
    "hot_access_probability",
    "cache_capacity_hotset_multiplier",
    "memory_bandwidth_level",
    "node_count",
)


def parse_args() -> argparse.Namespace:
    """Parse report-generation options."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--aggregate",
        action="append",
        type=Path,
        required=True,
        help="Path to an aggregate_summary.csv file. May be repeated.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Directory for report.md, derived CSVs, and plots/.",
    )
    parser.add_argument(
        "--baseline",
        default="lru",
        help="Policy used as the baseline for delta calculations.",
    )
    parser.add_argument(
        "--tie-threshold",
        type=float,
        default=0.02,
        help="Absolute latency delta ratio counted as a tie. Default: 0.02.",
    )
    parser.add_argument(
        "--formats",
        default="svg",
        help="Comma-separated plot formats. Default: svg. Example: svg,png.",
    )
    parser.add_argument(
        "--title",
        default="DM Simulator Results Analysis",
        help="Title used in the Markdown report and plot headings.",
    )
    return parser.parse_args()


def load_matplotlib() -> Any:
    """Import Matplotlib lazily so dependency errors can be explained clearly."""

    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as error:
        raise RuntimeError(INSTALL_MESSAGE) from error

    try:
        plt.style.use("seaborn-v0_8-whitegrid")
    except OSError:
        pass
    return plt


def parse_formats(value: str) -> list[str]:
    """Parse and validate requested plot output formats."""

    formats = [item.strip().lower() for item in value.split(",") if item.strip()]
    if not formats:
        raise SystemExit("At least one plot format is required")
    invalid = [item for item in formats if item not in {"svg", "png", "pdf"}]
    if invalid:
        raise SystemExit(
            "Invalid plot format(s): "
            + ", ".join(invalid)
            + ". Expected svg, png, or pdf"
        )
    return formats


def read_aggregate_rows(paths: list[Path]) -> list[dict[str, str]]:
    """Read successful matrix rows from one or more aggregate CSV files."""

    rows: list[dict[str, str]] = []
    for path in paths:
        if not path.exists():
            raise SystemExit(f"Aggregate file does not exist: {path}")
        with path.open("r", encoding="utf-8", newline="") as input_file:
            for row in csv.DictReader(input_file):
                if row.get("status", "success") != "success":
                    continue
                row["_source_aggregate"] = str(path)
                rows.append(row)
    if not rows:
        raise SystemExit("No successful rows found in aggregate input")
    return rows


def float_or_none(value: Any) -> float | None:
    """Parse a numeric CSV value, treating blanks and invalid values as missing."""

    if value is None:
        return None
    text = str(value).strip()
    if not text:
        return None
    try:
        parsed = float(text)
    except ValueError:
        return None
    if not math.isfinite(parsed):
        return None
    return parsed


def metric(row: dict[str, Any], column: str) -> float | None:
    """Fetch a numeric metric from a row."""

    return float_or_none(row.get(column))


def ratio_delta(value: float | None, baseline: float | None) -> float | None:
    """Return (value - baseline) / baseline with safe missing/zero handling."""

    if value is None or baseline is None or baseline == 0.0:
        return None
    return (value - baseline) / baseline


def absolute_delta(value: float | None,
                   baseline: float | None) -> float | None:
    """Return value - baseline when both values are available."""

    if value is None or baseline is None:
        return None
    return value - baseline


def classify_delta(delta_pct: float | None, tie_threshold: float) -> str:
    """Classify latency delta versus baseline; lower latency is better."""

    if delta_pct is None:
        return "no_baseline"
    if delta_pct < -tie_threshold:
        return "win"
    if delta_pct > tie_threshold:
        return "loss"
    return "tie"


def group_key(row: dict[str, str]) -> tuple[str, ...]:
    """Build the policy-independent experiment key for baseline matching."""

    return tuple(row.get(column, "") for column in GROUP_COLUMNS)


def sum_optional(lhs: float | None, rhs: float | None) -> float | None:
    """Sum optional values while preserving missingness when both are absent."""

    if lhs is None and rhs is None:
        return None
    return (lhs or 0.0) + (rhs or 0.0)


def format_value(value: Any) -> str:
    """Format values consistently for CSV output."""

    if value is None:
        return ""
    if isinstance(value, float):
        if not math.isfinite(value):
            return ""
        return f"{value:.6f}"
    return str(value)


def write_csv(path: Path,
              fieldnames: tuple[str, ...],
              rows: list[dict[str, Any]]) -> None:
    """Write rows with stable field ordering."""

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {column: format_value(row.get(column)) for column in fieldnames}
            )


def build_policy_comparison(rows: list[dict[str, str]],
                            baseline_policy: str,
                            tie_threshold: float) -> list[dict[str, Any]]:
    """Join every run against its matched baseline and compute deltas."""

    baseline_by_group: dict[tuple[str, ...], dict[str, str]] = {}
    for row in rows:
        if row.get("policy") == baseline_policy:
            baseline_by_group[group_key(row)] = row

    comparison_rows: list[dict[str, Any]] = []
    for row in rows:
        baseline = baseline_by_group.get(group_key(row))
        result: dict[str, Any] = {
            column: row.get(column, "") for column in GROUP_COLUMNS
        }
        result.update(
            {
                "policy": row.get("policy", ""),
                "baseline_policy": baseline_policy,
                "baseline_available": baseline is not None,
                "source_aggregate": row.get("_source_aggregate", ""),
                "experiment_name": row.get("experiment_name", ""),
                "output_dir": row.get("output_dir", ""),
            }
        )

        mean_latency = metric(row, "mean_latency")
        p99_latency = metric(row, "p99_latency")
        hit_rate = metric(row, "local_hit_rate")
        memory_wait = metric(row, "average_memory_wait")
        remote_accesses = metric(row, "total_remote_accesses")
        fairness = metric(row, "jain_inverse_latency_fairness")
        avoided_queue = metric(row, "estimated_avoided_queue_wait")
        avoided_service = metric(row, "estimated_avoided_remote_service_time")

        baseline_mean = metric(baseline, "mean_latency") if baseline else None
        baseline_p99 = metric(baseline, "p99_latency") if baseline else None
        baseline_hit = metric(baseline, "local_hit_rate") if baseline else None
        baseline_wait = (
            metric(baseline, "average_memory_wait") if baseline else None
        )
        baseline_remote = (
            metric(baseline, "total_remote_accesses") if baseline else None
        )
        baseline_fairness = (
            metric(baseline, "jain_inverse_latency_fairness")
            if baseline
            else None
        )

        mean_delta_pct = ratio_delta(mean_latency, baseline_mean)
        result.update(
            {
                "classification": classify_delta(
                    mean_delta_pct,
                    tie_threshold,
                ),
                "mean_latency": mean_latency,
                "baseline_mean_latency": baseline_mean,
                "mean_latency_delta": absolute_delta(mean_latency,
                                                     baseline_mean),
                "mean_latency_delta_pct": mean_delta_pct,
                "p99_latency": p99_latency,
                "baseline_p99_latency": baseline_p99,
                "p99_latency_delta": absolute_delta(p99_latency, baseline_p99),
                "p99_latency_delta_pct": ratio_delta(p99_latency, baseline_p99),
                "local_hit_rate": hit_rate,
                "baseline_local_hit_rate": baseline_hit,
                "local_hit_rate_delta": absolute_delta(hit_rate, baseline_hit),
                "average_memory_wait": memory_wait,
                "baseline_average_memory_wait": baseline_wait,
                "average_memory_wait_delta": absolute_delta(memory_wait,
                                                            baseline_wait),
                "average_memory_wait_delta_pct": ratio_delta(memory_wait,
                                                             baseline_wait),
                "total_remote_accesses": remote_accesses,
                "baseline_total_remote_accesses": baseline_remote,
                "total_remote_accesses_delta": absolute_delta(remote_accesses,
                                                              baseline_remote),
                "total_remote_accesses_delta_pct": ratio_delta(
                    remote_accesses,
                    baseline_remote,
                ),
                "estimated_avoided_remote_accesses": metric(
                    row,
                    "estimated_avoided_remote_accesses",
                ),
                "estimated_avoided_queue_wait": avoided_queue,
                "estimated_avoided_remote_service_time": avoided_service,
                "estimated_avoided_contention_cost": sum_optional(
                    avoided_queue,
                    avoided_service,
                ),
                "admission_yield": metric(row, "admission_yield"),
                "reuse_after_admit_rate": metric(row, "reuse_after_admit_rate"),
                "stale_telemetry_rate": metric(row, "stale_telemetry_rate"),
                "average_top_object_overlap": metric(
                    row,
                    "average_top_object_overlap",
                ),
                "eviction_regret_count": metric(row, "eviction_regret_count"),
                "remote_eviction_regret_count": metric(
                    row,
                    "remote_eviction_regret_count",
                ),
                "jain_inverse_latency_fairness": fairness,
                "baseline_jain_inverse_latency_fairness": baseline_fairness,
                "jain_inverse_latency_fairness_delta": absolute_delta(
                    fairness,
                    baseline_fairness,
                ),
                "per_node_mean_latency_spread": metric(
                    row,
                    "per_node_mean_latency_spread",
                ),
                "per_node_p99_latency_max": metric(row,
                                                   "per_node_p99_latency_max"),
            }
        )
        comparison_rows.append(result)
    return comparison_rows


def numeric_values(rows: list[dict[str, Any]], column: str) -> list[float]:
    """Extract available numeric values from rows."""

    values = []
    for row in rows:
        value = float_or_none(row.get(column))
        if value is not None:
            values.append(value)
    return values


def average(rows: list[dict[str, Any]], column: str) -> float | None:
    """Average a numeric column, ignoring missing values."""

    values = numeric_values(rows, column)
    if not values:
        return None
    return sum(values) / len(values)


def count_class(rows: list[dict[str, Any]], name: str) -> int:
    """Count rows with the requested win/tie/loss classification."""

    return sum(1 for row in rows if row.get("classification") == name)


def ordered_policies(rows: list[dict[str, Any]]) -> list[str]:
    """Return policies in a stable, human-friendly order."""

    present = {row.get("policy", "") for row in rows if row.get("policy")}
    ordered = [policy for policy in PREFERRED_POLICY_ORDER if policy in present]
    ordered.extend(sorted(present.difference(ordered)))
    return ordered


def build_policy_summary(
    comparison_rows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Collapse comparison rows into one summary row per policy."""

    by_policy: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in comparison_rows:
        by_policy[row["policy"]].append(row)

    summaries = []
    for policy in ordered_policies(comparison_rows):
        rows = by_policy[policy]
        comparable = [
            row for row in rows if row.get("classification") != "no_baseline"
        ]
        mean_deltas = numeric_values(comparable, "mean_latency_delta_pct")
        summaries.append(
            {
                "policy": policy,
                "run_count": len(rows),
                "comparable_run_count": len(comparable),
                "win_count": count_class(comparable, "win"),
                "tie_count": count_class(comparable, "tie"),
                "loss_count": count_class(comparable, "loss"),
                "avg_mean_latency": average(rows, "mean_latency"),
                "avg_p99_latency": average(rows, "p99_latency"),
                "avg_local_hit_rate": average(rows, "local_hit_rate"),
                "avg_average_memory_wait": average(rows,
                                                   "average_memory_wait"),
                "avg_total_remote_accesses": average(rows,
                                                     "total_remote_accesses"),
                "avg_mean_latency_delta_pct": average(
                    comparable,
                    "mean_latency_delta_pct",
                ),
                "best_mean_latency_delta_pct": (
                    min(mean_deltas) if mean_deltas else None
                ),
                "worst_mean_latency_delta_pct": (
                    max(mean_deltas) if mean_deltas else None
                ),
                "avg_p99_latency_delta_pct": average(
                    comparable,
                    "p99_latency_delta_pct",
                ),
                "avg_local_hit_rate_delta": average(
                    comparable,
                    "local_hit_rate_delta",
                ),
                "avg_average_memory_wait_delta_pct": average(
                    comparable,
                    "average_memory_wait_delta_pct",
                ),
                "avg_total_remote_accesses_delta_pct": average(
                    comparable,
                    "total_remote_accesses_delta_pct",
                ),
                "avg_admission_yield": average(rows, "admission_yield"),
                "avg_reuse_after_admit_rate": average(
                    rows,
                    "reuse_after_admit_rate",
                ),
                "avg_stale_telemetry_rate": average(rows,
                                                    "stale_telemetry_rate"),
                "avg_average_top_object_overlap": average(
                    rows,
                    "average_top_object_overlap",
                ),
                "avg_estimated_avoided_remote_accesses": average(
                    rows,
                    "estimated_avoided_remote_accesses",
                ),
                "avg_estimated_avoided_contention_cost": average(
                    rows,
                    "estimated_avoided_contention_cost",
                ),
                "avg_eviction_regret_count": average(rows,
                                                     "eviction_regret_count"),
                "avg_jain_inverse_latency_fairness": average(
                    rows,
                    "jain_inverse_latency_fairness",
                ),
                "avg_jain_inverse_latency_fairness_delta": average(
                    comparable,
                    "jain_inverse_latency_fairness_delta",
                ),
            }
        )
    return summaries


def build_condition_summary(
    comparison_rows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Summarize policy behavior across each major experiment dimension."""

    summaries = []
    for dimension in CONDITION_DIMENSIONS:
        grouped: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
        for row in comparison_rows:
            grouped[(row.get(dimension, ""), row.get("policy", ""))].append(row)
        for (value, policy), rows in sorted(grouped.items()):
            summaries.append(
                {
                    "dimension": dimension,
                    "value": value,
                    "policy": policy,
                    "run_count": len(rows),
                    "win_count": count_class(rows, "win"),
                    "tie_count": count_class(rows, "tie"),
                    "loss_count": count_class(rows, "loss"),
                    "avg_mean_latency": average(rows, "mean_latency"),
                    "avg_mean_latency_delta_pct": average(
                        rows,
                        "mean_latency_delta_pct",
                    ),
                    "avg_p99_latency_delta_pct": average(
                        rows,
                        "p99_latency_delta_pct",
                    ),
                    "avg_local_hit_rate": average(rows, "local_hit_rate"),
                    "avg_average_memory_wait": average(
                        rows,
                        "average_memory_wait",
                    ),
                    "avg_stale_telemetry_rate": average(
                        rows,
                        "stale_telemetry_rate",
                    ),
                    "avg_estimated_avoided_contention_cost": average(
                        rows,
                        "estimated_avoided_contention_cost",
                    ),
                }
            )
    return summaries


def value_for_plot(row: dict[str, Any], column: str) -> float | None:
    """Fetch a plot value, accepting already-numeric derived rows."""

    return float_or_none(row.get(column))


def plot_placeholder(plt: Any, title: str, message: str) -> Any:
    """Create a simple placeholder plot when a view has no usable data."""

    fig, ax = plt.subplots(figsize=(9, 5))
    ax.axis("off")
    ax.set_title(title)
    ax.text(0.5, 0.5, message, ha="center", va="center", wrap=True)
    return fig


def save_figure(fig: Any,
                plots_dir: Path,
                name: str,
                formats: list[str]) -> list[Path]:
    """Save a Matplotlib figure in all requested formats."""

    saved = []
    plots_dir.mkdir(parents=True, exist_ok=True)
    for fmt in formats:
        path = plots_dir / f"{name}.{fmt}"
        fig.savefig(path, bbox_inches="tight", dpi=180)
        saved.append(path)
    return saved


def bar_plot(plt: Any,
             labels: list[str],
             series: list[tuple[str, list[float | None]]],
             title: str,
             ylabel: str) -> Any:
    """Create a grouped bar chart without requiring NumPy."""

    if not labels or not series:
        return plot_placeholder(plt, title, "No data available for this plot.")

    fig, ax = plt.subplots(figsize=(max(9, len(labels) * 1.2), 5.5))
    group_width = 0.78
    bar_width = group_width / max(1, len(series))
    x_positions = list(range(len(labels)))

    for index, (name, values) in enumerate(series):
        offset = -group_width / 2 + bar_width / 2 + index * bar_width
        heights = [value if value is not None else 0.0 for value in values]
        ax.bar(
            [x + offset for x in x_positions],
            heights,
            width=bar_width,
            label=name,
        )

    ax.set_title(title)
    ax.set_ylabel(ylabel)
    ax.set_xticks(x_positions)
    ax.set_xticklabels(labels, rotation=30, ha="right")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    return fig


def policy_summary_lookup(
    policy_summary: list[dict[str, Any]],
) -> dict[str, dict[str, Any]]:
    """Index summary rows by policy name."""

    return {row["policy"]: row for row in policy_summary}


def make_policy_latency_plot(plt: Any,
                             policy_summary: list[dict[str, Any]]) -> Any:
    """Plot mean and P99 latency by policy."""

    policies = ordered_policies(policy_summary)
    lookup = policy_summary_lookup(policy_summary)
    return bar_plot(
        plt,
        policies,
        [
            ("Mean latency",
             [value_for_plot(lookup[p], "avg_mean_latency") for p in policies]),
            ("P99 latency",
             [value_for_plot(lookup[p], "avg_p99_latency") for p in policies]),
        ],
        "Policy latency overview",
        "Latency (simulation time)",
    )


def make_cache_memory_plot(plt: Any,
                           policy_summary: list[dict[str, Any]]) -> Any:
    """Plot cache and memory-pressure metrics in a compact 2x2 view."""

    policies = ordered_policies(policy_summary)
    if not policies:
        return plot_placeholder(
            plt,
            "Cache and memory overview",
            "No data available for this plot.",
        )

    lookup = policy_summary_lookup(policy_summary)
    fig, axes = plt.subplots(2, 2, figsize=(14, 9))
    plots = [
        ("avg_local_hit_rate", "Local hit rate", "Hit rate"),
        ("avg_average_memory_wait", "Average memory wait", "Wait time"),
        ("avg_total_remote_accesses", "Remote accesses", "Accesses"),
        ("avg_estimated_avoided_contention_cost",
         "Estimated contention relief",
         "Avoided wait + service time"),
    ]
    for ax, (column, title, ylabel) in zip(axes.flat, plots):
        values = [
            value_for_plot(lookup[policy], column) or 0.0
            for policy in policies
        ]
        ax.bar(range(len(policies)), values)
        ax.set_title(title)
        ax.set_ylabel(ylabel)
        ax.set_xticks(range(len(policies)))
        ax.set_xticklabels(policies, rotation=30, ha="right")
        ax.grid(axis="y", alpha=0.3)
    fig.suptitle("Policy cache and memory-pressure overview")
    fig.tight_layout()
    return fig


def make_latency_delta_plot(plt: Any,
                            policy_summary: list[dict[str, Any]]) -> Any:
    """Plot latency deltas versus the selected baseline."""

    policies = ordered_policies(policy_summary)
    lookup = policy_summary_lookup(policy_summary)
    return bar_plot(
        plt,
        policies,
        [
            (
                "Mean latency delta",
                [
                    100.0 * value_for_plot(p_lookup, "avg_mean_latency_delta_pct")
                    if value_for_plot(p_lookup, "avg_mean_latency_delta_pct")
                    is not None
                    else None
                    for p_lookup in [lookup[p] for p in policies]
                ],
            ),
            (
                "P99 latency delta",
                [
                    100.0 * value_for_plot(p_lookup, "avg_p99_latency_delta_pct")
                    if value_for_plot(p_lookup, "avg_p99_latency_delta_pct")
                    is not None
                    else None
                    for p_lookup in [lookup[p] for p in policies]
                ],
            ),
        ],
        "Latency delta versus baseline",
        "Delta (%)",
    )


def aggregate_for_line(rows: list[dict[str, Any]],
                       x_column: str,
                       y_column: str,
                       numeric_x: bool = True) -> dict[str, list[tuple[Any, float]]]:
    """Average a y metric by policy and x value for sweep plots."""

    grouped: dict[tuple[str, Any], list[float]] = defaultdict(list)
    for row in rows:
        x_raw = row.get(x_column)
        y_value = value_for_plot(row, y_column)
        if x_raw in (None, "") or y_value is None:
            continue
        x_value: Any = float_or_none(x_raw) if numeric_x else str(x_raw)
        if x_value is None:
            continue
        grouped[(row.get("policy", ""), x_value)].append(y_value)

    by_policy: dict[str, list[tuple[Any, float]]] = defaultdict(list)
    for (policy, x_value), values in grouped.items():
        by_policy[policy].append((x_value, sum(values) / len(values)))
    for values in by_policy.values():
        values.sort(key=lambda item: item[0])
    return by_policy


def line_sweep_plot(plt: Any,
                    rows: list[dict[str, Any]],
                    x_column: str,
                    y_specs: list[tuple[str, str, float]],
                    title: str,
                    x_label: str,
                    numeric_x: bool = True) -> Any:
    """Create one or more line charts for a matrix sweep dimension."""

    fig, axes = plt.subplots(
        len(y_specs),
        1,
        figsize=(10, max(4, 3.6 * len(y_specs))),
        squeeze=False,
    )
    any_data = False
    for ax, (y_column, y_label, scale) in zip(axes.flat, y_specs):
        by_policy = aggregate_for_line(rows, x_column, y_column, numeric_x)
        for policy in ordered_policies(rows):
            points = by_policy.get(policy, [])
            if not points:
                continue
            any_data = True
            xs = [point[0] for point in points]
            ys = [point[1] * scale for point in points]
            ax.plot(xs, ys, marker="o", label=policy)
        ax.axhline(0.0, color="black", linewidth=0.8, alpha=0.4)
        ax.set_ylabel(y_label)
        ax.grid(alpha=0.3)
        if not numeric_x:
            ax.tick_params(axis="x", rotation=20)
        ax.legend(fontsize="small")
    axes.flat[-1].set_xlabel(x_label)
    fig.suptitle(title)
    fig.tight_layout()
    if not any_data:
        return plot_placeholder(plt, title, "No data available for this plot.")
    return fig


def make_viability_scatter(plt: Any,
                           comparison_rows: list[dict[str, Any]]) -> Any:
    """Scatter policy viability signals against latency improvement."""

    rows = [
        row for row in comparison_rows
        if "contention_aware" in str(row.get("policy", ""))
    ]
    usable = [
        row for row in rows
        if value_for_plot(row, "stale_telemetry_rate") is not None
        and value_for_plot(row, "mean_latency_delta_pct") is not None
    ]
    if not usable:
        return plot_placeholder(
            plt,
            "Contention-aware viability scatter",
            "No contention-aware viability rows with stale-telemetry data.",
        )

    fig, ax = plt.subplots(figsize=(9, 6))
    for policy in ordered_policies(usable):
        policy_rows = [row for row in usable if row.get("policy") == policy]
        xs = [value_for_plot(row, "stale_telemetry_rate") or 0.0
              for row in policy_rows]
        ys = [
            -100.0 * (value_for_plot(row, "mean_latency_delta_pct") or 0.0)
            for row in policy_rows
        ]
        sizes = [
            40.0 + 30.0 * (value_for_plot(row, "admission_yield") or 0.0)
            for row in policy_rows
        ]
        ax.scatter(xs, ys, s=sizes, alpha=0.75, label=policy)
    ax.axhline(0.0, color="black", linewidth=0.8, alpha=0.4)
    ax.set_title("Contention-aware viability signals")
    ax.set_xlabel("Stale telemetry rate")
    ax.set_ylabel("Mean latency improvement vs baseline (%)")
    ax.legend(fontsize="small")
    ax.grid(alpha=0.3)
    return fig


def make_fairness_plot(plt: Any,
                       policy_summary: list[dict[str, Any]]) -> Any:
    """Plot fairness and node imbalance by policy."""

    policies = ordered_policies(policy_summary)
    lookup = policy_summary_lookup(policy_summary)
    return bar_plot(
        plt,
        policies,
        [
            (
                "Jain inverse-latency fairness",
                [
                    value_for_plot(lookup[p],
                                   "avg_jain_inverse_latency_fairness")
                    for p in policies
                ],
            ),
            (
                "Mean latency delta (%)",
                [
                    100.0 * value_for_plot(lookup[p],
                                           "avg_mean_latency_delta_pct")
                    if value_for_plot(lookup[p],
                                      "avg_mean_latency_delta_pct") is not None
                    else None
                    for p in policies
                ],
            ),
        ],
        "Fairness and latency tradeoff",
        "Metric value",
    )


def generate_plots(plt: Any,
                   output_dir: Path,
                   formats: list[str],
                   comparison_rows: list[dict[str, Any]],
                   policy_summary: list[dict[str, Any]]) -> list[Path]:
    """Generate the standard Phase F plot set."""

    plots_dir = output_dir / "plots"
    plot_specs = [
        ("policy_latency_overview",
         make_policy_latency_plot(plt, policy_summary)),
        ("policy_cache_memory_overview",
         make_cache_memory_plot(plt, policy_summary)),
        ("latency_delta_vs_baseline",
         make_latency_delta_plot(plt, policy_summary)),
        (
            "churn_sweep",
            line_sweep_plot(
                plt,
                comparison_rows,
                "hot_set_churn_fraction",
                [
                    ("mean_latency_delta_pct", "Mean latency delta (%)", 100.0),
                    ("stale_telemetry_rate", "Stale telemetry rate", 1.0),
                ],
                "Temporal stability sweep",
                "Hot-set churn fraction",
            ),
        ),
        (
            "epoch_length_sweep",
            line_sweep_plot(
                plt,
                comparison_rows,
                "requests_per_node_per_epoch",
                [("mean_latency_delta_pct", "Mean latency delta (%)", 100.0)],
                "Epoch length sweep",
                "Requests per node per epoch",
            ),
        ),
        (
            "cache_pressure_sweep",
            line_sweep_plot(
                plt,
                comparison_rows,
                "cache_capacity_hotset_multiplier",
                [
                    ("mean_latency_delta_pct", "Mean latency delta (%)", 100.0),
                    ("local_hit_rate", "Local hit rate", 1.0),
                ],
                "Cache pressure sweep",
                "Cache capacity / hot-set footprint",
            ),
        ),
        (
            "memory_bottleneck_sweep",
            line_sweep_plot(
                plt,
                comparison_rows,
                "memory_bandwidth_level",
                [
                    ("average_memory_wait", "Average memory wait", 1.0),
                    ("mean_latency_delta_pct", "Mean latency delta (%)", 100.0),
                ],
                "Memory bottleneck sweep",
                "Memory bandwidth level",
                numeric_x=False,
            ),
        ),
        ("viability_scatter", make_viability_scatter(plt, comparison_rows)),
        ("fairness_overview", make_fairness_plot(plt, policy_summary)),
    ]

    saved_paths: list[Path] = []
    for name, fig in plot_specs:
        saved_paths.extend(save_figure(fig, plots_dir, name, formats))
        plt.close(fig)
    return saved_paths


def markdown_table(rows: list[dict[str, Any]],
                   columns: list[tuple[str, str]],
                   limit: int | None = None) -> str:
    """Render a small Markdown table from derived rows."""

    selected = rows if limit is None else rows[:limit]
    if not selected:
        return "_No rows available._\n"

    header = "| " + " | ".join(title for title, _ in columns) + " |"
    divider = "| " + " | ".join("---" for _ in columns) + " |"
    lines = [header, divider]
    for row in selected:
        values = []
        for _, key in columns:
            value = row.get(key)
            if isinstance(value, float):
                if "pct" in key or "rate" in key or "fairness" in key:
                    values.append(f"{value:.3f}")
                else:
                    values.append(f"{value:.2f}")
            else:
                values.append(str(value) if value is not None else "")
        lines.append("| " + " | ".join(values) + " |")
    return "\n".join(lines) + "\n"


def percent(value: float | None) -> str:
    """Format a ratio as a signed percentage."""

    if value is None:
        return "n/a"
    return f"{100.0 * value:+.1f}%"


def build_observations(
    comparison_rows: list[dict[str, Any]],
    policy_summary: list[dict[str, Any]],
    baseline: str,
) -> list[str]:
    """Generate lightweight interpretation notes from aggregate metrics."""

    observations: list[str] = []
    non_baseline = [
        row for row in policy_summary
        if row.get("policy") != baseline
        and value_for_plot(row, "avg_mean_latency_delta_pct") is not None
    ]
    if non_baseline:
        best = min(
            non_baseline,
            key=lambda row: value_for_plot(row, "avg_mean_latency_delta_pct")
            or float("inf"),
        )
        observations.append(
            f"Best average mean-latency delta versus `{baseline}` is "
            f"{percent(value_for_plot(best, 'avg_mean_latency_delta_pct'))} "
            f"from `{best['policy']}`."
        )

    contention_rows = [
        row for row in comparison_rows
        if "contention_aware" in str(row.get("policy", ""))
    ]
    if contention_rows:
        low_stale = [
            row for row in contention_rows
            if (value_for_plot(row, "stale_telemetry_rate") or 1.0) <= 0.4
        ]
        high_stale = [
            row for row in contention_rows
            if (value_for_plot(row, "stale_telemetry_rate") or 0.0) >= 0.7
        ]
        low_stale_wins = count_class(low_stale, "win")
        high_stale_losses = count_class(high_stale, "loss")
        observations.append(
            "Contention-aware rows with low stale telemetry produced "
            f"{low_stale_wins}/{len(low_stale)} wins; high-stale rows "
            f"produced {high_stale_losses}/{len(high_stale)} losses."
        )

        similar_hit_lower_wait = [
            row for row in contention_rows
            if abs(value_for_plot(row, "local_hit_rate_delta") or 0.0) <= 0.02
            and (value_for_plot(row, "average_memory_wait_delta_pct") or 0.0)
            < -0.02
        ]
        if similar_hit_lower_wait:
            observations.append(
                f"{len(similar_hit_lower_wait)} contention-aware rows had "
                "similar hit rate to baseline but lower memory wait, which is "
                "a useful signal even when hit rate alone looks neutral."
            )

    if not observations:
        observations.append(
            "No strong automated interpretation was available; inspect the "
            "plots and derived CSVs for policy-specific behavior."
        )
    return observations


def write_report(path: Path,
                 title: str,
                 aggregate_paths: list[Path],
                 baseline: str,
                 tie_threshold: float,
                 plot_paths: list[Path],
                 policy_summary: list[dict[str, Any]],
                 comparison_rows: list[dict[str, Any]]) -> None:
    """Write the Phase F Markdown report."""

    rel_plots = [
        plot.relative_to(path.parent)
        for plot in plot_paths
        if plot.suffix == ".svg"
    ]
    observations = build_observations(comparison_rows, policy_summary, baseline)
    sorted_summary = sorted(
        policy_summary,
        key=lambda row: (
            value_for_plot(row, "avg_mean_latency_delta_pct")
            if value_for_plot(row, "avg_mean_latency_delta_pct") is not None
            else float("inf")
        ),
    )

    lines = [
        f"# {title}",
        "",
        "## Inputs",
        "",
        *[f"- `{aggregate}`" for aggregate in aggregate_paths],
        "",
        "## Baseline And Classification",
        "",
        f"- Baseline policy: `{baseline}`",
        f"- Tie threshold: +/-{100.0 * tie_threshold:.1f}% mean-latency delta",
        "- Lower latency, lower memory wait, and fewer remote accesses are better.",
        "- Higher cache hit rate, estimated contention relief, and fairness are better.",
        "",
        "## Key Observations",
        "",
        *[f"- {observation}" for observation in observations],
        "",
        "## Policy Summary",
        "",
        markdown_table(
            sorted_summary,
            [
                ("Policy", "policy"),
                ("Runs", "run_count"),
                ("Wins", "win_count"),
                ("Ties", "tie_count"),
                ("Losses", "loss_count"),
                ("Avg Mean Delta", "avg_mean_latency_delta_pct"),
                ("Avg P99 Delta", "avg_p99_latency_delta_pct"),
                ("Avg Hit Rate", "avg_local_hit_rate"),
                ("Avg Stale Telemetry", "avg_stale_telemetry_rate"),
                ("Avg Fairness", "avg_jain_inverse_latency_fairness"),
            ],
        ),
        "",
        "## Plots",
        "",
    ]
    for plot in rel_plots:
        lines.extend([f"### {plot.stem.replace('_', ' ').title()}", ""])
        lines.extend([f"![{plot.stem}]({plot})", ""])

    lines.extend(
        [
            "## Interpretation Guidance",
            "",
            "Contention-aware placement should be expected to help when shared "
            "remote-memory pressure is persistent, hot objects are stable or "
            "drift gradually, remote memory is a real bottleneck, cache "
            "capacity is constrained but useful, and previous-epoch telemetry "
            "is predictive enough to act on.",
            "",
            "It should be expected to hurt or tie when hot sets fully shift "
            "every epoch, epochs are too short, remote memory is lightly "
            "loaded, object sizes/service costs are uniform, or local reuse is "
            "too sparse for private-cache placement to pay off.",
            "",
            "A policy can still be interesting when hit rate is similar to LRU "
            "but memory wait or remote accesses fall, because the research "
            "question is about reducing shared bottleneck pressure, not only "
            "maximizing local hits.",
            "",
            "## Generated Files",
            "",
            "- `policy_comparison.csv`",
            "- `policy_summary.csv`",
            "- `condition_summary.csv`",
            "- `plots/`",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    """Generate derived CSVs, plots, and a Markdown report."""

    args = parse_args()
    if args.tie_threshold < 0.0 or not math.isfinite(args.tie_threshold):
        raise SystemExit("--tie-threshold must be a nonnegative finite number")
    formats = parse_formats(args.formats)
    output_dir = (
        args.output_dir
        if args.output_dir is not None
        else args.aggregate[0].parent / "analysis"
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    try:
        plt = load_matplotlib()
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 2

    aggregate_rows = read_aggregate_rows(args.aggregate)
    comparison_rows = build_policy_comparison(
        aggregate_rows,
        args.baseline,
        args.tie_threshold,
    )
    policy_summary = build_policy_summary(comparison_rows)
    condition_summary = build_condition_summary(comparison_rows)

    write_csv(output_dir / "policy_comparison.csv",
              COMPARISON_COLUMNS,
              comparison_rows)
    write_csv(output_dir / "policy_summary.csv",
              POLICY_SUMMARY_COLUMNS,
              policy_summary)
    write_csv(output_dir / "condition_summary.csv",
              CONDITION_COLUMNS,
              condition_summary)
    plot_paths = generate_plots(
        plt,
        output_dir,
        formats,
        comparison_rows,
        policy_summary,
    )
    write_report(
        output_dir / "report.md",
        args.title,
        args.aggregate,
        args.baseline,
        args.tie_threshold,
        plot_paths,
        policy_summary,
        comparison_rows,
    )
    print(f"Wrote analysis report: {output_dir / 'report.md'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
