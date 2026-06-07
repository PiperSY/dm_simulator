#!/usr/bin/env python3

"""Analyze dm_simulator matrix outputs and generate report-ready plots."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import defaultdict
from dataclasses import dataclass
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
    "hotness_only_windowed",
    "global_hottest_replication",
    "contention_aware",
    "contention_aware_v1",
    "contention_aware_smoothed",
    "contention_aware_smoothed_reuse_gated",
    "contention_aware_smoothed_reuse_gated_hysteresis",
    "contention_aware_size_value",
    "contention_aware_reuse_gated",
    "contention_aware_hysteresis",
)

GROUP_COLUMNS = (
    "preset",
    "seed",
    "node_count",
    "epoch_count",
    "requests_per_node_per_epoch",
    "workload_issue_mode",
    "burst_size",
    "burst_interval",
    "intra_burst_gap",
    "node_phase_jitter",
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
    "memory_channel_count",
    "hot_object_channel_count",
    "memory_bandwidth_level",
    "memory_bandwidth_bytes_per_time",
    "memory_base_latency_level",
    "memory_base_latency",
    "link_latency_level",
    "one_way_link_latency",
)

COMPARISON_COLUMNS = (
    *GROUP_COLUMNS,
    # Weight profile columns are reported but intentionally excluded from
    # GROUP_COLUMNS so all 24 profiled runs can share one matched LRU baseline.
    "contention_weight_profile",
    "contention_weight_name",
    "contention_weight_value",
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
    "max_memory_wait",
    "peak_memory_queue_depth",
    "peak_memory_channel_queue_depth",
    "max_channel_total_queue_wait",
    "channel_queue_imbalance",
    "total_remote_accesses",
    "baseline_total_remote_accesses",
    "total_remote_accesses_delta",
    "total_remote_accesses_delta_pct",
    "total_queue_wait",
    "total_remote_service_time",
    "max_observed_queue_depth",
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

FINAL_POLICY_TABLE_COLUMNS = (
    "policy",
    "seed_count",
    "comparable_run_count",
    "win_rate",
    "mean_latency_improvement_pct",
    "seed_std_latency_improvement_pct",
    "mean_p99_improvement_pct",
    "seed_std_p99_improvement_pct",
    "mean_memory_wait_reduction_pct",
    "seed_std_memory_wait_reduction_pct",
    "mean_local_hit_rate",
    "seed_std_local_hit_rate",
)

FINAL_REPORT_PRESETS = {
    "eval_final_contention_scaling",
    "eval_final_node_bandwidth",
    "eval_final_policy_iteration",
}

FINAL_REPORT_POLICY_PRESETS = {
    "eval_final_policy_iteration",
    # Accept the exploratory preset as a fallback so older pilot results can be
    # rendered with the paper-oriented report before final reruns finish.
    "eval_policy_iteration",
}

FINAL_REPORT_POLICY_ORDER = (
    "lru",
    "hotness_only_windowed",
    "contention_aware_smoothed",
    "contention_aware_smoothed_reuse_gated",
    "contention_aware_smoothed_reuse_gated_hysteresis",
    "contention_aware_size_value",
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

BEST_RUN_BASELINE_POLICIES = {
    "always_remote",
    "lru",
    "global_hottest_replication",
}

BEST_RUN_COLUMNS = (
    "target_policy",
    "latency_improvement_pct",
    "mean_latency_delta_pct",
    "p99_latency_delta_pct",
    "local_hit_rate",
    "local_hit_rate_delta",
    "average_memory_wait_delta_pct",
    "total_remote_accesses_delta_pct",
    "admission_yield",
    "reuse_after_admit_rate",
    "stale_telemetry_rate",
    "average_top_object_overlap",
    "best_experiment_name",
    "best_output_dir",
    "preset",
    "seed",
    "node_count",
    "epoch_count",
    "requests_per_node_per_epoch",
    "hot_set_churn_fraction",
    "cache_capacity_hotset_multiplier",
    "cache_capacity_bytes",
    "hot_set_size",
    "hot_access_probability",
    "cross_node_overlap",
    "object_count",
    "object_size_mode",
    "object_size_bytes",
    "object_size_small_bytes",
    "object_size_large_bytes",
    "large_object_probability",
    "memory_channel_count",
    "hot_object_channel_count",
    "memory_bandwidth_level",
    "memory_bandwidth_bytes_per_time",
    "memory_base_latency_level",
    "memory_base_latency",
    "link_latency_level",
    "one_way_link_latency",
)

BEST_RUN_CONTEXT_COLUMNS = (
    "target_policy",
    "comparison_policy",
    "is_target_policy",
    "latency_improvement_pct",
    "mean_latency_delta_pct",
    "p99_latency_delta_pct",
    "local_hit_rate",
    "local_hit_rate_delta",
    "average_memory_wait",
    "average_memory_wait_delta_pct",
    "total_remote_accesses",
    "total_remote_accesses_delta_pct",
    "admission_yield",
    "reuse_after_admit_rate",
    "stale_telemetry_rate",
    "average_top_object_overlap",
    "experiment_name",
    "output_dir",
    *GROUP_COLUMNS,
)

CONDITION_DIMENSIONS = (
    "hot_set_churn_fraction",
    "requests_per_node_per_epoch",
    "epoch_count",
    "cross_node_overlap",
    "object_count",
    "hot_set_size",
    "hot_access_probability",
    "large_object_probability",
    "workload_issue_mode",
    "burst_size",
    "burst_interval",
    "intra_burst_gap",
    "node_phase_jitter",
    "cache_capacity_hotset_multiplier",
    # These are condition/report dimensions only. They are not baseline-match
    # keys because the baseline run has blank weight metadata by design.
    "contention_weight_profile",
    "contention_weight_name",
    "contention_weight_value",
    "memory_channel_count",
    "hot_object_channel_count",
    "memory_bandwidth_level",
    "node_count",
)

REPORT_DIMENSIONS = (
    *CONDITION_DIMENSIONS,
    "memory_base_latency_level",
    "link_latency_level",
    "object_size_mode",
)

REPORT_MODES = (
    "auto",
    "generic",
    "contention_calibration",
    "policy_viability",
    "interaction",
    "parameter_demo",
    "weight_sensitivity",
    "final_report",
)

CATEGORICAL_ORDER = {
    "memory_bandwidth_level": ("mild", "moderate", "severe"),
    "memory_base_latency_level": ("low", "medium", "high"),
    "link_latency_level": ("low", "medium", "high"),
    "cross_node_overlap": ("low", "medium", "high"),
    "workload_issue_mode": ("completion_driven", "scheduled_bursty"),
    # Keep weight facets in the same order as the policy scoring formula so the
    # sensitivity report reads from local evidence through remote-cost signals.
    "contention_weight_name": (
        "local_hotness_weight",
        "remote_access_weight",
        "distinct_requester_weight",
        "queue_wait_weight",
        "remote_service_time_weight",
        "size_penalty_weight",
    ),
}


@dataclass(frozen=True)
class PlotOutput:
    """A saved plot plus the report section where it should be displayed."""

    path: Path
    group: str


@dataclass(frozen=True)
class ObjectConcentrationSummary:
    """Top-object concentration summary for one simulation run."""

    top_distinct_requesters: float
    top_remote_access_share: float


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
    parser.add_argument(
        "--report-mode",
        choices=REPORT_MODES,
        default="auto",
        help=(
            "Plot bundle to generate. 'auto' detects from aggregate presets; "
            "mixed or unknown presets fall back to generic."
        ),
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


def read_contention_rows(output_dir: str) -> list[dict[str, str]]:
    """Read object-contention rows for one run if the file exists."""

    if not output_dir:
        return []
    path = Path(output_dir) / "contention_by_object.csv"
    if not path.exists():
        return []
    with path.open("r", encoding="utf-8", newline="") as input_file:
        return list(csv.DictReader(input_file))


def distinct_values(rows: list[dict[str, Any]], column: str) -> list[str]:
    """Return non-empty distinct values for a column in stable sorted order."""

    values = {
        str(row.get(column, "")).strip()
        for row in rows
        if str(row.get(column, "")).strip()
    }
    return sorted(values, key=lambda value: sort_value(column, value))


def detect_report_mode(rows: list[dict[str, str]], requested: str) -> str:
    """Resolve auto report mode from the aggregate preset column."""

    if requested != "auto":
        return requested
    presets = distinct_values(rows, "preset")
    if presets and any(preset in FINAL_REPORT_PRESETS for preset in presets):
        allowed = FINAL_REPORT_PRESETS | {"eval_bursty_calibration"}
        if all(preset in allowed for preset in presets):
            return "final_report"
    if len(presets) != 1:
        return "generic"
    preset = presets[0]
    if (preset == "eval_contention_calibration" or
            preset == "eval_bursty_calibration" or
            preset.startswith("eval_channel_")):
        return "contention_calibration"
    if preset in {"eval_policy_viability", "eval_policy_iteration"}:
        return "policy_viability"
    if preset.startswith("eval_interactions_"):
        return "interaction"
    if preset.startswith("eval_knob_"):
        return "parameter_demo"
    if preset == "eval_contention_weight_sensitivity":
        return "weight_sensitivity"
    return "generic"


def dimension_summary(rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """Describe which known experiment dimensions were fixed or swept."""

    summary: dict[str, dict[str, Any]] = {}
    for column in REPORT_DIMENSIONS:
        values = distinct_values(rows, column)
        if not values:
            continue
        summary[column] = {
            "values": values,
            "swept": len(values) > 1,
        }
    return summary


def swept_dimensions(summary: dict[str, dict[str, Any]]) -> set[str]:
    """Return the dimensions that vary across the aggregate input."""

    return {
        column for column, info in summary.items() if bool(info.get("swept"))
    }


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


def sort_value(column: str, value: Any) -> tuple[int, Any]:
    """Sort numeric and known categorical values in presentation order."""

    text = str(value)
    order = CATEGORICAL_ORDER.get(column)
    if order and text in order:
        return (0, order.index(text))
    numeric = float_or_none(text)
    if numeric is not None:
        return (1, numeric)
    return (2, text)


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
                "contention_weight_profile": row.get(
                    "contention_weight_profile",
                    "",
                ),
                "contention_weight_name": row.get("contention_weight_name", ""),
                "contention_weight_value": row.get(
                    "contention_weight_value",
                    "",
                ),
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
        max_memory_wait = metric(row, "max_memory_wait")
        peak_queue_depth = metric(row, "peak_memory_queue_depth")
        peak_channel_queue_depth = metric(row,
                                          "peak_memory_channel_queue_depth")
        max_channel_queue_wait = metric(row, "max_channel_total_queue_wait")
        channel_imbalance = metric(row, "channel_queue_imbalance")
        remote_accesses = metric(row, "total_remote_accesses")
        total_queue_wait = metric(row, "total_queue_wait")
        total_service_time = metric(row, "total_remote_service_time")
        max_observed_queue_depth = metric(row, "max_observed_queue_depth")
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
                "max_memory_wait": max_memory_wait,
                "peak_memory_queue_depth": peak_queue_depth,
                "peak_memory_channel_queue_depth": peak_channel_queue_depth,
                "max_channel_total_queue_wait": max_channel_queue_wait,
                "channel_queue_imbalance": channel_imbalance,
                "total_remote_accesses": remote_accesses,
                "baseline_total_remote_accesses": baseline_remote,
                "total_remote_accesses_delta": absolute_delta(remote_accesses,
                                                              baseline_remote),
                "total_remote_accesses_delta_pct": ratio_delta(
                    remote_accesses,
                    baseline_remote,
                ),
                "total_queue_wait": total_queue_wait,
                "total_remote_service_time": total_service_time,
                "max_observed_queue_depth": max_observed_queue_depth,
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


def sample_standard_deviation(values: list[float]) -> float | None:
    """Return sample standard deviation, or no value for one seed."""

    if len(values) < 2:
        return None
    mean = sum(values) / len(values)
    variance = sum((value - mean) ** 2 for value in values) / (
        len(values) - 1
    )
    return math.sqrt(variance)


def rows_for_preferred_presets(
    rows: list[dict[str, Any]],
    preferred: set[str],
    fallback: set[str],
) -> list[dict[str, Any]]:
    """Use final-study rows when present, otherwise accept pilot-study rows."""

    selected = [
        row for row in rows if str(row.get("preset", "")) in preferred
    ]
    if selected:
        return selected
    return [row for row in rows if str(row.get("preset", "")) in fallback]


def build_final_policy_table(
    comparison_rows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Summarize final policy results with variation across seed means."""

    rows = rows_for_preferred_presets(
        comparison_rows,
        {"eval_final_policy_iteration"},
        {"eval_policy_iteration"},
    )
    by_policy: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        policy = str(row.get("policy", ""))
        if policy in FINAL_REPORT_POLICY_ORDER:
            by_policy[policy].append(row)

    table: list[dict[str, Any]] = []
    metric_specs = (
        (
            "mean_latency_delta_pct",
            "mean_latency_improvement_pct",
            "seed_std_latency_improvement_pct",
            -100.0,
        ),
        (
            "p99_latency_delta_pct",
            "mean_p99_improvement_pct",
            "seed_std_p99_improvement_pct",
            -100.0,
        ),
        (
            "average_memory_wait_delta_pct",
            "mean_memory_wait_reduction_pct",
            "seed_std_memory_wait_reduction_pct",
            -100.0,
        ),
        (
            "local_hit_rate",
            "mean_local_hit_rate",
            "seed_std_local_hit_rate",
            1.0,
        ),
    )
    for policy in FINAL_REPORT_POLICY_ORDER:
        policy_rows = by_policy.get(policy, [])
        if not policy_rows:
            continue
        by_seed: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for row in policy_rows:
            by_seed[str(row.get("seed", ""))].append(row)

        result: dict[str, Any] = {
            "policy": policy,
            "seed_count": len(by_seed),
            "comparable_run_count": sum(
                row.get("classification") != "no_baseline"
                for row in policy_rows
            ),
        }
        comparable = [
            row for row in policy_rows
            if row.get("classification") != "no_baseline"
        ]
        result["win_rate"] = (
            count_class(comparable, "win") / len(comparable)
            if comparable
            else None
        )
        for source, mean_column, std_column, scale in metric_specs:
            seed_means = []
            for seed_rows in by_seed.values():
                seed_mean = average(seed_rows, source)
                if seed_mean is not None:
                    seed_means.append(scale * seed_mean)
            result[mean_column] = (
                sum(seed_means) / len(seed_means) if seed_means else None
            )
            result[std_column] = sample_standard_deviation(seed_means)
        table.append(result)
    return table


def count_class(rows: list[dict[str, Any]], name: str) -> int:
    """Count rows with the requested win/tie/loss classification."""

    return sum(1 for row in rows if row.get("classification") == name)


def ordered_policies(rows: list[dict[str, Any]]) -> list[str]:
    """Return policies in a stable, human-friendly order."""

    present = {row.get("policy", "") for row in rows if row.get("policy")}
    ordered = [policy for policy in PREFERRED_POLICY_ORDER if policy in present]
    ordered.extend(sorted(present.difference(ordered)))
    return ordered


def policy_order_key(policy: str) -> tuple[int, Any]:
    """Sort a single policy label with the same order used in plots."""

    if policy in PREFERRED_POLICY_ORDER:
        return (0, PREFERRED_POLICY_ORDER.index(policy))
    return (1, policy)


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


def latency_improvement_pct(row: dict[str, Any]) -> float | None:
    """Return positive-is-better latency improvement versus the baseline."""

    delta = value_for_plot(row, "mean_latency_delta_pct")
    if delta is None:
        return None
    return -100.0 * delta


def is_best_run_policy(row: dict[str, Any]) -> bool:
    """Return whether a policy should be considered for best-run examples."""

    policy = str(row.get("policy", ""))
    if policy in BEST_RUN_BASELINE_POLICIES:
        return False
    return value_for_plot(row, "mean_latency_delta_pct") is not None


def best_run_sort_key(row: dict[str, Any]) -> tuple[float, float, float, str]:
    """Deterministically rank rows for a policy's best example condition."""

    mean_delta = value_for_plot(row, "mean_latency_delta_pct")
    p99_delta = value_for_plot(row, "p99_latency_delta_pct")
    hit_rate = value_for_plot(row, "local_hit_rate")
    return (
        mean_delta if mean_delta is not None else float("inf"),
        p99_delta if p99_delta is not None else float("inf"),
        -(hit_rate if hit_rate is not None else -1.0),
        str(row.get("experiment_name", "")),
    )


def best_run_summary_row(row: dict[str, Any]) -> dict[str, Any]:
    """Build the compact audit row for one policy's best condition."""

    summary = {
        column: row.get(column, "")
        for column in BEST_RUN_COLUMNS
        if column in GROUP_COLUMNS
    }
    summary.update(
        {
            "target_policy": row.get("policy", ""),
            "latency_improvement_pct": latency_improvement_pct(row),
            "mean_latency_delta_pct": value_for_plot(
                row,
                "mean_latency_delta_pct",
            ),
            "p99_latency_delta_pct": value_for_plot(
                row,
                "p99_latency_delta_pct",
            ),
            "local_hit_rate": value_for_plot(row, "local_hit_rate"),
            "local_hit_rate_delta": value_for_plot(
                row,
                "local_hit_rate_delta",
            ),
            "average_memory_wait_delta_pct": value_for_plot(
                row,
                "average_memory_wait_delta_pct",
            ),
            "total_remote_accesses_delta_pct": value_for_plot(
                row,
                "total_remote_accesses_delta_pct",
            ),
            "admission_yield": value_for_plot(row, "admission_yield"),
            "reuse_after_admit_rate": value_for_plot(
                row,
                "reuse_after_admit_rate",
            ),
            "stale_telemetry_rate": value_for_plot(
                row,
                "stale_telemetry_rate",
            ),
            "average_top_object_overlap": value_for_plot(
                row,
                "average_top_object_overlap",
            ),
            "best_experiment_name": row.get("experiment_name", ""),
            "best_output_dir": row.get("output_dir", ""),
        }
    )
    return summary


def best_run_context_row(target_policy: str,
                         row: dict[str, Any]) -> dict[str, Any]:
    """Build one comparison-policy row for a target policy's best condition."""

    context = {
        column: row.get(column, "")
        for column in BEST_RUN_CONTEXT_COLUMNS
        if column in GROUP_COLUMNS
    }
    comparison_policy = str(row.get("policy", ""))
    context.update(
        {
            "target_policy": target_policy,
            "comparison_policy": comparison_policy,
            "is_target_policy": comparison_policy == target_policy,
            "latency_improvement_pct": latency_improvement_pct(row),
            "mean_latency_delta_pct": value_for_plot(
                row,
                "mean_latency_delta_pct",
            ),
            "p99_latency_delta_pct": value_for_plot(
                row,
                "p99_latency_delta_pct",
            ),
            "local_hit_rate": value_for_plot(row, "local_hit_rate"),
            "local_hit_rate_delta": value_for_plot(
                row,
                "local_hit_rate_delta",
            ),
            "average_memory_wait": value_for_plot(
                row,
                "average_memory_wait",
            ),
            "average_memory_wait_delta_pct": value_for_plot(
                row,
                "average_memory_wait_delta_pct",
            ),
            "total_remote_accesses": value_for_plot(
                row,
                "total_remote_accesses",
            ),
            "total_remote_accesses_delta_pct": value_for_plot(
                row,
                "total_remote_accesses_delta_pct",
            ),
            "admission_yield": value_for_plot(row, "admission_yield"),
            "reuse_after_admit_rate": value_for_plot(
                row,
                "reuse_after_admit_rate",
            ),
            "stale_telemetry_rate": value_for_plot(
                row,
                "stale_telemetry_rate",
            ),
            "average_top_object_overlap": value_for_plot(
                row,
                "average_top_object_overlap",
            ),
            "experiment_name": row.get("experiment_name", ""),
            "output_dir": row.get("output_dir", ""),
        }
    )
    return context


def build_best_run_examples(
    comparison_rows: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Find each non-baseline policy's best row and matched peer context."""

    by_policy: dict[str, list[dict[str, Any]]] = defaultdict(list)
    by_group: dict[tuple[str, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in comparison_rows:
        by_group[group_key(row)].append(row)
        if is_best_run_policy(row):
            by_policy[str(row.get("policy", ""))].append(row)

    best_rows: list[dict[str, Any]] = []
    for policy in ordered_policies(
        [{"policy": policy} for policy in by_policy.keys()]
    ):
        policy_rows = by_policy.get(policy, [])
        if policy_rows:
            best_rows.append(min(policy_rows, key=best_run_sort_key))

    summary_rows: list[dict[str, Any]] = []
    context_rows: list[dict[str, Any]] = []
    for best_row in best_rows:
        target_policy = str(best_row.get("policy", ""))
        summary_rows.append(best_run_summary_row(best_row))
        # Best-run examples are high-water marks, not averages: show every
        # practical cache policy at the exact same matched matrix condition.
        peer_rows = [
            row for row in by_group[group_key(best_row)]
            if is_best_run_policy(row)
        ]
        for row in sorted(
            peer_rows,
            key=lambda item: policy_order_key(str(item.get("policy", ""))),
        ):
            context_rows.append(best_run_context_row(target_policy, row))

    return summary_rows, context_rows


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
        values.sort(key=lambda item: sort_value(x_column, item[0]))
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


def faceted_line_sweep_plot(
    plt: Any,
    rows: list[dict[str, Any]],
    facet_column: str,
    x_column: str,
    y_specs: list[tuple[str, str, float]],
    title: str,
    facet_label: str,
    x_label: str,
    numeric_x: bool = True,
) -> Any:
    """Create line charts that condition a sweep on one matrix dimension."""

    facet_values = distinct_values(rows, facet_column)
    policies = ordered_policies(rows)
    if not facet_values or not policies:
        return plot_placeholder(plt, title, "No data available for this plot.")

    fig, axes = plt.subplots(
        len(y_specs),
        len(facet_values),
        figsize=(max(10, 3.8 * len(facet_values)),
                 max(4.5, 3.4 * len(y_specs))),
        squeeze=False,
    )
    any_data = False
    legend_handles = []
    legend_labels = []
    for facet_index, facet_value in enumerate(facet_values):
        facet_rows = [
            row for row in rows
            if str(row.get(facet_column, "")) == str(facet_value)
        ]
        for metric_index, (y_column, y_label, scale) in enumerate(y_specs):
            ax = axes[metric_index][facet_index]
            by_policy = aggregate_for_line(
                facet_rows,
                x_column,
                y_column,
                numeric_x,
            )
            for policy in policies:
                points = by_policy.get(policy, [])
                if not points:
                    continue
                any_data = True
                xs = [point[0] for point in points]
                ys = [point[1] * scale for point in points]
                line, = ax.plot(xs, ys, marker="o", label=policy)
                if policy not in legend_labels:
                    legend_handles.append(line)
                    legend_labels.append(policy)
            ax.axhline(0.0, color="black", linewidth=0.8, alpha=0.35)
            ax.grid(alpha=0.3)
            if metric_index == 0:
                ax.set_title(f"{facet_label}: {facet_value}")
            if facet_index == 0:
                ax.set_ylabel(y_label)
            if metric_index == len(y_specs) - 1:
                ax.set_xlabel(x_label)
            if not numeric_x:
                ax.tick_params(axis="x", rotation=20)

    fig.suptitle(title)
    if legend_handles:
        fig.legend(
            legend_handles,
            legend_labels,
            loc="lower center",
            ncol=min(4, len(legend_labels)),
            fontsize="small",
        )
    fig.tight_layout(rect=[0.0, 0.08, 1.0, 0.94])
    if not any_data:
        plt.close(fig)
        return plot_placeholder(plt, title, "No data available for this plot.")
    return fig


def average_by_key(rows: list[dict[str, Any]],
                   key_columns: tuple[str, ...],
                   metric_column: str) -> dict[tuple[str, ...], float]:
    """Average a metric for each tuple of dimension values."""

    grouped: dict[tuple[str, ...], list[float]] = defaultdict(list)
    for row in rows:
        value = value_for_plot(row, metric_column)
        if value is None:
            continue
        key = tuple(str(row.get(column, "")) for column in key_columns)
        if any(not part for part in key):
            continue
        grouped[key].append(value)
    return {
        key: sum(values) / len(values)
        for key, values in grouped.items()
        if values
    }


def heatmap_color_scale(metric_column: str,
                        values: list[float]) -> tuple[float, float, str]:
    """Return shared color limits and colormap for a heatmap figure."""

    finite_values = [value for value in values if math.isfinite(value)]
    if not finite_values:
        return (0.0, 1.0, "viridis")

    if metric_column.endswith("_delta_pct") or "delta" in metric_column:
        max_abs = max(abs(min(finite_values)), abs(max(finite_values)))
        if max_abs == 0.0:
            max_abs = 1.0
        return (-max_abs, max_abs, "coolwarm")

    minimum = min(finite_values)
    maximum = max(finite_values)
    if minimum == maximum:
        padding = abs(minimum) * 0.05 or 1.0
        minimum -= padding
        maximum += padding
    return (minimum, maximum, "viridis")


def heatmap_by_policy(plt: Any,
                      rows: list[dict[str, Any]],
                      x_column: str,
                      y_column: str,
                      metric_column: str,
                      title: str,
                      x_label: str,
                      y_label: str,
                      metric_label: str,
                      scale: float = 1.0) -> Any:
    """Create one heatmap per policy for a two-dimensional matrix sweep."""

    xs = distinct_values(rows, x_column)
    ys = distinct_values(rows, y_column)
    policies = ordered_policies(rows)
    if not xs or not ys or not policies:
        return plot_placeholder(plt, title, "No data available for this plot.")

    averaged = average_by_key(rows, ("policy", y_column, x_column),
                              metric_column)
    matrices: dict[str, list[list[float]]] = {}
    plotted_values: list[float] = []
    any_data = False
    for policy in policies:
        matrix: list[list[float]] = []
        for y_value in ys:
            row_values = []
            for x_value in xs:
                value = averaged.get((policy, y_value, x_value))
                if value is None:
                    row_values.append(math.nan)
                    continue
                any_data = True
                scaled_value = value * scale
                plotted_values.append(scaled_value)
                row_values.append(scaled_value)
            matrix.append(row_values)
        matrices[policy] = matrix

    if not any_data:
        return plot_placeholder(plt, title, "No data available for this plot.")

    vmin, vmax, cmap = heatmap_color_scale(metric_column, plotted_values)
    fig, axes = plt.subplots(
        1,
        len(policies),
        figsize=(max(9, 5.1 * len(policies)), 5.8),
        constrained_layout=True,
        squeeze=False,
    )
    images = []
    for policy_index, (ax, policy) in enumerate(zip(axes.flat, policies)):
        matrix = matrices[policy]
        image = ax.imshow(matrix, aspect="auto", vmin=vmin, vmax=vmax,
                          cmap=cmap)
        images.append(image)
        ax.set_title(policy)
        ax.set_xticks(range(len(xs)))
        ax.set_xticklabels(xs, rotation=30, ha="right")
        ax.set_yticks(range(len(ys)))
        ax.set_xlabel(x_label)
        if policy_index == 0:
            ax.set_yticklabels(ys)
            ax.set_ylabel(y_label, labelpad=12)
        else:
            ax.set_yticklabels([])
            ax.tick_params(axis="y", length=0)
        for y_index, row_values in enumerate(matrix):
            for x_index, value in enumerate(row_values):
                if math.isfinite(value):
                    ax.text(
                        x_index,
                        y_index,
                        f"{value:.1f}",
                        ha="center",
                        va="center",
                            fontsize="x-small",
                    )
    fig.suptitle(title)
    if images:
        fig.colorbar(images[0], ax=list(axes.flat), shrink=0.8,
                     label=metric_label)
    return fig


def policy_scope_label(rows: list[dict[str, Any]]) -> str:
    """Describe whether a plot is policy-specific or averaged over policies."""

    policies = ordered_policies(rows)
    if not policies:
        return "Policy scope: no policy rows"
    if len(policies) == 1:
        return f"Policy scope: {policies[0]}"
    return "Policy scope: averaged across " + ", ".join(policies)


def policy_averaged_panel_heatmap(
    plt: Any,
    rows: list[dict[str, Any]],
    panel_specs: list[tuple[str, str, str, str, str, str]],
    title: str,
) -> Any:
    """Create side-by-side heatmaps averaged over all non-axis dimensions."""

    if not rows or not panel_specs:
        return plot_placeholder(plt, title, "No data available for this plot.")

    fig, axes = plt.subplots(
        1,
        len(panel_specs),
        figsize=(max(7, 6.4 * len(panel_specs)), 5.6),
        squeeze=False,
    )
    any_data = False
    for ax, (x_column,
             y_column,
             metric_column,
             panel_title,
             x_label,
             y_label) in zip(axes.flat, panel_specs):
        xs = distinct_values(rows, x_column)
        ys = distinct_values(rows, y_column)
        values_by_cell = average_by_key(rows,
                                        (y_column, x_column),
                                        metric_column)
        matrix: list[list[float]] = []
        for y_value in ys:
            row_values = []
            for x_value in xs:
                value = values_by_cell.get((y_value, x_value))
                if value is None:
                    row_values.append(math.nan)
                else:
                    any_data = True
                    row_values.append(value)
            matrix.append(row_values)

        image = ax.imshow(matrix, aspect="auto")
        ax.set_title(panel_title)
        ax.set_xticks(range(len(xs)))
        ax.set_xticklabels(xs, rotation=30, ha="right")
        ax.set_yticks(range(len(ys)))
        ax.set_yticklabels(ys)
        ax.set_xlabel(x_label)
        ax.set_ylabel(y_label)
        for y_index, row_values in enumerate(matrix):
            for x_index, value in enumerate(row_values):
                if math.isfinite(value):
                    ax.text(x_index,
                            y_index,
                            f"{value:.1f}",
                            ha="center",
                            va="center",
                            fontsize="small")
        fig.colorbar(image,
                     ax=ax,
                     shrink=0.82,
                     label=metric_column.replace("_", " ").title())

    fig.suptitle(title)
    fig.text(0.5, 0.01, policy_scope_label(rows), ha="center", fontsize="small")
    if not any_data:
        plt.close(fig)
        return plot_placeholder(plt, title, "No data available for this plot.")
    fig.tight_layout(rect=[0.0, 0.04, 1.0, 0.94])
    return fig


def calibration_memory_pressure_heatmap(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Plot global memory pressure by node count and bandwidth."""

    return policy_averaged_panel_heatmap(
        plt,
        comparison_rows,
        [
            (
                "node_count",
                "memory_bandwidth_level",
                "average_memory_wait",
                "Average memory wait",
                "Compute node count",
                "Memory bandwidth level",
            ),
            (
                "node_count",
                "memory_bandwidth_level",
                "total_queue_wait",
                "Total queue wait",
                "Compute node count",
                "Memory bandwidth level",
            ),
        ],
        "Memory pressure by node count and bandwidth",
    )


def calibration_hotness_memory_pressure_heatmap(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Plot memory wait against hotness, cache capacity, and bandwidth."""

    return policy_averaged_panel_heatmap(
        plt,
        comparison_rows,
        [
            (
                "hot_access_probability",
                "cache_capacity_hotset_multiplier",
                "average_memory_wait",
                "Hotness by cache capacity",
                "Hot-access probability",
                "Cache capacity / hot-set footprint",
            ),
            (
                "hot_access_probability",
                "memory_bandwidth_level",
                "average_memory_wait",
                "Hotness by memory bandwidth",
                "Hot-access probability",
                "Memory bandwidth level",
            ),
        ],
        "Memory wait by hotness, cache capacity, and bandwidth",
    )


def calibration_channel_count_pressure_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Plot how memory-channel parallelism changes bottleneck pressure."""

    return line_sweep_plot(
        plt,
        comparison_rows,
        "memory_channel_count",
        [
            ("average_memory_wait", "Average memory wait", 1.0),
            ("total_queue_wait", "Total queue wait", 1.0),
            ("peak_memory_channel_queue_depth",
             "Peak channel queue depth",
             1.0),
            ("channel_queue_imbalance", "Channel queue imbalance", 1.0),
        ],
        "Memory pressure as channel parallelism changes",
        "Memory channel count",
    )


def calibration_issue_mode_pressure_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Compare response-paced issue with planned burst arrivals."""

    # Issue mode changes arrival timing, not the object stream. These pressure
    # panels show whether the timing change actually creates sharper queues.
    return line_sweep_plot(
        plt,
        comparison_rows,
        "workload_issue_mode",
        [
            ("average_memory_wait", "Average memory wait", 1.0),
            ("total_queue_wait", "Total queue wait", 1.0),
            ("peak_memory_channel_queue_depth",
             "Peak channel queue depth",
             1.0),
            ("p99_latency", "P99 latency", 1.0),
        ],
        "Memory pressure by workload issue mode",
        "Workload issue mode",
        numeric_x=False,
    )


def calibration_channel_hotspot_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Plot how concentrating hot objects onto fewer channels changes pressure."""

    return line_sweep_plot(
        plt,
        comparison_rows,
        "hot_object_channel_count",
        [
            ("average_memory_wait", "Average memory wait", 1.0),
            ("peak_memory_channel_queue_depth",
             "Peak channel queue depth",
             1.0),
            ("max_channel_total_queue_wait",
             "Max channel total queue wait",
             1.0),
            ("channel_queue_imbalance", "Channel queue imbalance", 1.0),
        ],
        "Channel-local hotspot pressure",
        "Hot-object channel count (0 = unrestricted)",
    )


def calibration_channel_pressure_heatmap(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Show channel pressure across channel-count and hotspot dimensions."""

    # Channel plots distinguish global-looking memory pressure from localized
    # resource pressure by averaging over policy and non-axis dimensions.
    return policy_averaged_panel_heatmap(
        plt,
        comparison_rows,
        [
            (
                "memory_channel_count",
                "hot_object_channel_count",
                "average_memory_wait",
                "Average memory wait",
                "Memory channel count",
                "Hot-object channel count",
            ),
            (
                "memory_channel_count",
                "hot_object_channel_count",
                "peak_memory_channel_queue_depth",
                "Peak channel queue depth",
                "Memory channel count",
                "Hot-object channel count",
            ),
        ],
        "Channel-local memory pressure",
    )


def summarize_object_concentration(
    contention_rows: list[dict[str, str]],
    top_k: int = 5,
) -> ObjectConcentrationSummary | None:
    """Summarize requester diversity and remote-access concentration."""

    objects = []
    total_remote_accesses = 0.0
    for row in contention_rows:
        remote_accesses = float_or_none(row.get("remote_accesses"))
        distinct_requesters = float_or_none(row.get("distinct_requesters"))
        if remote_accesses is None or remote_accesses <= 0.0:
            continue
        if distinct_requesters is None:
            continue
        total_remote_accesses += remote_accesses
        objects.append((remote_accesses, distinct_requesters))

    if not objects or total_remote_accesses <= 0.0:
        return None

    objects.sort(key=lambda item: item[0], reverse=True)
    top_objects = objects[:top_k]
    top_remote_accesses = sum(item[0] for item in top_objects)
    top_distinct_requesters = (
        sum(item[1] for item in top_objects) / len(top_objects)
    )
    return ObjectConcentrationSummary(
        top_distinct_requesters=top_distinct_requesters,
        top_remote_access_share=top_remote_accesses / total_remote_accesses,
    )


def object_concentration_by_run(
    comparison_rows: list[dict[str, Any]],
    top_k: int = 5,
) -> dict[str, ObjectConcentrationSummary]:
    """Index top-object concentration summaries by experiment name."""

    summaries: dict[str, ObjectConcentrationSummary] = {}
    for row in comparison_rows:
        experiment_name = str(row.get("experiment_name", ""))
        if not experiment_name:
            continue
        summary = summarize_object_concentration(
            read_contention_rows(str(row.get("output_dir", ""))),
            top_k,
        )
        if summary is not None:
            summaries[experiment_name] = summary
    return summaries


def concentration_metric_by_cell(
    comparison_rows: list[dict[str, Any]],
    concentration: dict[str, ObjectConcentrationSummary],
    metric_name: str,
) -> dict[tuple[str, str], float]:
    """Average one concentration metric by overlap and hot probability."""

    grouped: dict[tuple[str, str], list[float]] = defaultdict(list)
    for row in comparison_rows:
        summary = concentration.get(str(row.get("experiment_name", "")))
        if summary is None:
            continue
        key = (
            str(row.get("cross_node_overlap", "")),
            str(row.get("hot_access_probability", "")),
        )
        if any(not part for part in key):
            continue
        grouped[key].append(float(getattr(summary, metric_name)))
    return {
        key: sum(values) / len(values)
        for key, values in grouped.items()
        if values
    }


def calibration_overlap_hotness_concentration_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Plot object-level concentration across overlap and hotness settings."""

    concentration = object_concentration_by_run(comparison_rows, top_k=5)
    if not concentration:
        return plot_placeholder(
            plt,
            "Object concentration by overlap and hotness",
            "No contention_by_object.csv files were available.",
        )

    hot_probabilities = distinct_values(comparison_rows,
                                        "hot_access_probability")
    overlaps = distinct_values(comparison_rows, "cross_node_overlap")
    if not hot_probabilities or not overlaps:
        return plot_placeholder(
            plt,
            "Object concentration by overlap and hotness",
            "No overlap/hot-access matrix values were available.",
        )

    metric_specs = [
        (
            "top_distinct_requesters",
            "Top-5 average distinct requesters",
            "Distinct requesters",
        ),
        (
            "top_remote_access_share",
            "Top-5 remote access share",
            "Share of remote accesses",
        ),
    ]
    fig, axes = plt.subplots(1, 2, figsize=(14, 5.5), squeeze=False)
    policies = ordered_policies(comparison_rows)
    if len(policies) == 1:
        policy_scope = f"Policy scope: {policies[0]}"
    else:
        policy_scope = "Policy scope: averaged across " + ", ".join(policies)
    any_data = False
    for ax, (metric_name, title, color_label) in zip(axes.flat, metric_specs):
        values_by_cell = concentration_metric_by_cell(comparison_rows,
                                                      concentration,
                                                      metric_name)
        matrix: list[list[float]] = []
        for overlap in overlaps:
            row_values = []
            for hot_probability in hot_probabilities:
                value = values_by_cell.get((overlap, hot_probability))
                if value is None:
                    row_values.append(math.nan)
                else:
                    any_data = True
                    row_values.append(value)
            matrix.append(row_values)
        image = ax.imshow(matrix, aspect="auto")
        ax.set_title(title)
        ax.set_xticks(range(len(hot_probabilities)))
        ax.set_xticklabels(hot_probabilities, rotation=30, ha="right")
        ax.set_yticks(range(len(overlaps)))
        ax.set_yticklabels(overlaps)
        ax.set_xlabel("Hot-access probability")
        ax.set_ylabel("Cross-node overlap")
        for y_index, row_values in enumerate(matrix):
            for x_index, value in enumerate(row_values):
                if math.isfinite(value):
                    ax.text(x_index,
                            y_index,
                            f"{value:.2f}",
                            ha="center",
                            va="center",
                            fontsize="small")
        fig.colorbar(image, ax=ax, shrink=0.82, label=color_label)

    fig.suptitle("Object-level concentration by overlap and hotness")
    fig.text(0.5, 0.01, policy_scope, ha="center", fontsize="small")
    if not any_data:
        plt.close(fig)
        return plot_placeholder(
            plt,
            "Object concentration by overlap and hotness",
            "No object-concentration data was available.",
        )
    fig.tight_layout(rect=[0.0, 0.04, 1.0, 0.94])
    return fig


def calibration_node_count_pressure_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Plot how queue pressure scales with nodes and bandwidth."""

    metrics = [
        ("average_memory_wait", "Average memory wait", 1.0),
        ("peak_memory_queue_depth", "Peak queue depth", 1.0),
        ("total_queue_wait", "Total queue wait", 1.0),
    ]
    fig, axes = plt.subplots(len(metrics), 1, figsize=(11, 10), squeeze=False)
    any_data = False
    for ax, (metric_column, ylabel, scale) in zip(axes.flat, metrics):
        grouped: dict[tuple[str, str, str], list[float]] = defaultdict(list)
        for row in comparison_rows:
            value = value_for_plot(row, metric_column)
            if value is None:
                continue
            key = (
                str(row.get("policy", "")),
                str(row.get("memory_bandwidth_level", "")),
                str(row.get("node_count", "")),
            )
            if any(not part for part in key):
                continue
            grouped[key].append(value)
        for policy in ordered_policies(comparison_rows):
            bandwidths = distinct_values(
                [
                    row for row in comparison_rows
                    if row.get("policy") == policy
                ],
                "memory_bandwidth_level",
            )
            for bandwidth in bandwidths:
                points = []
                for node_count in distinct_values(comparison_rows,
                                                  "node_count"):
                    values = grouped.get((policy, bandwidth, node_count), [])
                    if values:
                        points.append(
                            (float_or_none(node_count) or 0.0,
                             scale * sum(values) / len(values))
                        )
                if points:
                    any_data = True
                    points.sort(key=lambda item: item[0])
                    ax.plot(
                        [point[0] for point in points],
                        [point[1] for point in points],
                        marker="o",
                        label=f"{policy}/{bandwidth}",
                    )
        ax.set_ylabel(ylabel)
        ax.grid(alpha=0.3)
        ax.legend(fontsize="x-small", ncol=2)
    axes.flat[-1].set_xlabel("Compute node count")
    fig.suptitle("Contention scaling by node count and bandwidth")
    fig.tight_layout()
    if not any_data:
        plt.close(fig)
        return plot_placeholder(
            plt,
            "Contention scaling by node count and bandwidth",
            "No node-count pressure data available.",
        )
    return fig


def calibration_policy_pressure_reduction_plot(
    plt: Any,
    policy_summary: list[dict[str, Any]],
) -> Any:
    """Compare how policies reduce remote pressure in calibration runs."""

    policies = ordered_policies(policy_summary)
    lookup = policy_summary_lookup(policy_summary)
    return bar_plot(
        plt,
        policies,
        [
            (
                "Remote accesses",
                [
                    value_for_plot(lookup[p], "avg_total_remote_accesses")
                    for p in policies
                ],
            ),
            (
                "Average memory wait",
                [
                    value_for_plot(lookup[p], "avg_average_memory_wait")
                    for p in policies
                ],
            ),
            (
                "Mean latency delta (%)",
                [
                    100.0 * value_for_plot(
                        lookup[p],
                        "avg_mean_latency_delta_pct",
                    )
                    if value_for_plot(lookup[p],
                                      "avg_mean_latency_delta_pct") is not None
                    else None
                    for p in policies
                ],
            ),
        ],
        "Policy pressure reduction during contention calibration",
        "Metric value",
    )


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
    ax.set_xlabel("Hot-set-sized stale telemetry rate")
    ax.set_ylabel("Mean latency improvement vs baseline (%)")
    ax.legend(fontsize="small")
    ax.grid(alpha=0.3)
    return fig


def best_run_targets(context_rows: list[dict[str, Any]]) -> list[str]:
    """Return target policies represented in best-run context rows."""

    targets = {
        str(row.get("target_policy", ""))
        for row in context_rows
        if str(row.get("target_policy", ""))
    }
    return sorted(targets, key=policy_order_key)


def best_run_condition_label(row: dict[str, Any]) -> str:
    """Format the most important sweep values for a best-run panel title."""

    parts = []
    for column, label in (
        ("hot_set_churn_fraction", "churn"),
        ("cache_capacity_hotset_multiplier", "cachex"),
        ("requests_per_node_per_epoch", "rpe"),
        ("large_object_probability", "large-p"),
    ):
        value = str(row.get(column, "")).strip()
        if value:
            parts.append(f"{label}={value}")
    return ", ".join(parts)


def sorted_best_context_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Order peer-policy rows consistently inside best-run example panels."""

    return sorted(
        rows,
        key=lambda row: policy_order_key(str(row.get("comparison_policy", ""))),
    )


def make_best_run_latency_examples_plot(
    plt: Any,
    context_rows: list[dict[str, Any]],
) -> Any:
    """Plot peer policies at each target policy's best latency condition."""

    targets = best_run_targets(context_rows)
    if not targets:
        return plot_placeholder(
            plt,
            "Best-run latency examples",
            "No non-baseline best-run examples were available.",
        )

    columns = min(3, len(targets))
    rows = math.ceil(len(targets) / columns)
    fig, axes = plt.subplots(
        rows,
        columns,
        figsize=(max(10, 4.8 * columns), max(4.5, 4.1 * rows)),
        squeeze=False,
    )
    any_data = False
    for index, target_policy in enumerate(targets):
        ax = axes.flat[index]
        target_rows = sorted_best_context_rows([
            row for row in context_rows
            if row.get("target_policy") == target_policy
        ])
        policies = [str(row.get("comparison_policy", "")) for row in target_rows]
        values = [value_for_plot(row, "latency_improvement_pct")
                  for row in target_rows]
        if any(value is not None for value in values):
            any_data = True
        heights = [value if value is not None else 0.0 for value in values]
        colors = [
            "tab:orange" if row.get("is_target_policy") is True else "tab:blue"
            for row in target_rows
        ]
        ax.bar(range(len(policies)), heights, color=colors)
        ax.axhline(0.0, color="black", linewidth=0.8, alpha=0.45)
        label = best_run_condition_label(target_rows[0]) if target_rows else ""
        ax.set_title(f"{target_policy}\n{label}")
        ax.set_ylabel("Mean latency improvement vs baseline (%)")
        ax.set_xticks(range(len(policies)))
        ax.set_xticklabels(policies, rotation=30, ha="right")
        ax.grid(axis="y", alpha=0.3)

    for index in range(len(targets), rows * columns):
        axes.flat[index].axis("off")
    fig.suptitle("Best individual run for each non-baseline policy")
    fig.tight_layout(rect=[0.0, 0.0, 1.0, 0.95])
    if not any_data:
        plt.close(fig)
        return plot_placeholder(
            plt,
            "Best-run latency examples",
            "No latency-improvement values were available.",
        )
    return fig


def make_best_run_pressure_examples_plot(
    plt: Any,
    context_rows: list[dict[str, Any]],
) -> Any:
    """Plot cache and remote-pressure context for each best-run condition."""

    targets = best_run_targets(context_rows)
    if not targets:
        return plot_placeholder(
            plt,
            "Best-run pressure examples",
            "No non-baseline best-run examples were available.",
        )

    metrics = [
        ("local_hit_rate", "Local hit rate", 1.0),
        (
            "average_memory_wait_delta_pct",
            "Memory-wait reduction vs baseline (%)",
            -100.0,
        ),
        (
            "total_remote_accesses_delta_pct",
            "Remote-access reduction vs baseline (%)",
            -100.0,
        ),
    ]
    fig, axes = plt.subplots(
        len(metrics),
        len(targets),
        figsize=(max(10, 4.4 * len(targets)), 10.5),
        squeeze=False,
    )
    any_data = False
    for column_index, target_policy in enumerate(targets):
        target_rows = sorted_best_context_rows([
            row for row in context_rows
            if row.get("target_policy") == target_policy
        ])
        policies = [str(row.get("comparison_policy", "")) for row in target_rows]
        colors = [
            "tab:orange" if row.get("is_target_policy") is True else "tab:blue"
            for row in target_rows
        ]
        for row_index, (metric_column, label, scale) in enumerate(metrics):
            ax = axes[row_index][column_index]
            values = [
                value_for_plot(row, metric_column)
                for row in target_rows
            ]
            if any(value is not None for value in values):
                any_data = True
            heights = [
                scale * value if value is not None else 0.0
                for value in values
            ]
            ax.bar(range(len(policies)), heights, color=colors)
            ax.axhline(0.0, color="black", linewidth=0.8, alpha=0.4)
            if row_index == 0:
                condition = (
                    best_run_condition_label(target_rows[0])
                    if target_rows
                    else ""
                )
                ax.set_title(f"{target_policy}\n{condition}")
            if column_index == 0:
                ax.set_ylabel(label)
            ax.set_xticks(range(len(policies)))
            ax.set_xticklabels(policies, rotation=30, ha="right")
            ax.grid(axis="y", alpha=0.3)

    fig.suptitle("Cache and remote-pressure context at best-run conditions")
    fig.tight_layout(rect=[0.0, 0.0, 1.0, 0.96])
    if not any_data:
        plt.close(fig)
        return plot_placeholder(
            plt,
            "Best-run pressure examples",
            "No pressure-context values were available.",
        )
    return fig


def viability_admission_quality_plot(
    plt: Any,
    policy_summary: list[dict[str, Any]],
) -> Any:
    """Plot policy diagnostics that explain placement quality."""

    policies = ordered_policies(policy_summary)
    if not policies:
        return plot_placeholder(
            plt,
            "Admission quality and telemetry diagnostics",
            "No policy diagnostics available.",
        )

    lookup = policy_summary_lookup(policy_summary)
    fig, axes = plt.subplots(2, 2, figsize=(14, 9))
    plots = [
        ("avg_admission_yield", "Admission yield", "Future hits / placement"),
        ("avg_reuse_after_admit_rate", "Reuse-after-admit rate", "Rate"),
        ("avg_stale_telemetry_rate", "Hot-set-sized stale telemetry", "Rate"),
        ("avg_eviction_regret_count", "Eviction regret count", "Count"),
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
    fig.suptitle("Admission quality and telemetry diagnostics")
    fig.tight_layout()
    return fig


def viability_admission_quality_by_churn_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Show whether placement quality decays as hot sets churn."""

    return line_sweep_plot(
        plt,
        comparison_rows,
        "hot_set_churn_fraction",
        [
            ("admission_yield", "Admission yield", 1.0),
            ("reuse_after_admit_rate", "Reuse-after-admit rate", 1.0),
            ("estimated_avoided_contention_cost",
             "Estimated contention relief",
             1.0),
            ("eviction_regret_count", "Eviction regret count", 1.0),
        ],
        "Admission quality by hot-set churn",
        "Hot-set churn fraction",
    )


def viability_remote_pressure_by_churn_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Show whether policies reduce remote-memory pressure under churn."""

    return line_sweep_plot(
        plt,
        comparison_rows,
        "hot_set_churn_fraction",
        [
            ("total_remote_accesses_delta_pct",
             "Remote-access delta vs baseline (%)",
             100.0),
            ("average_memory_wait_delta_pct",
             "Average memory-wait delta vs baseline (%)",
             100.0),
            ("estimated_avoided_contention_cost",
             "Estimated contention relief",
             1.0),
        ],
        "Remote pressure by hot-set churn",
        "Hot-set churn fraction",
    )


def viability_fairness_by_churn_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Show whether policy choices create node-level imbalance under churn."""

    return line_sweep_plot(
        plt,
        comparison_rows,
        "hot_set_churn_fraction",
        [
            ("jain_inverse_latency_fairness",
             "Jain inverse-latency fairness",
             1.0),
            ("per_node_mean_latency_spread",
             "Per-node mean-latency spread",
             1.0),
        ],
        "Fairness by hot-set churn",
        "Hot-set churn fraction",
    )


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


FINAL_POLICY_COLORS = {
    "always_remote": "#6f6f6f",
    "lru": "#1f1f1f",
    "hotness_only_windowed": "#d17a00",
    "contention_aware_smoothed": "#1f77b4",
    "contention_aware_smoothed_reuse_gated": "#2ca02c",
    "contention_aware_smoothed_reuse_gated_hysteresis": "#c43c39",
    "contention_aware_size_value": "#7a5195",
}

FINAL_POLICY_LABELS = {
    "always_remote": "Always remote",
    "lru": "LRU",
    "hotness_only_windowed": "Windowed hotness",
    "contention_aware_smoothed": "Smoothed CA",
    "contention_aware_smoothed_reuse_gated": "Smoothed + confirmation",
    "contention_aware_smoothed_reuse_gated_hysteresis":
        "Smoothed + confirmation + hysteresis",
    "contention_aware_size_value": "Smoothed + cost density",
}


def final_policy_label(policy: str) -> str:
    """Return compact labels suitable for paper figure legends."""

    return FINAL_POLICY_LABELS.get(policy, policy)


def final_study_rows(
    rows: list[dict[str, Any]],
    final_preset: str,
    fallback_preset: str,
) -> list[dict[str, Any]]:
    """Select final-study rows while allowing exploratory-result previews."""

    return rows_for_preferred_presets(
        rows,
        {final_preset},
        {fallback_preset},
    )


def final_metric_value(
    row: dict[str, Any],
    column: str,
    fallback: str | None = None,
) -> float | None:
    """Read a final-figure metric with an optional legacy-column fallback."""

    value = value_for_plot(row, column)
    if value is None and fallback is not None:
        return value_for_plot(row, fallback)
    return value


def final_grouped_points(
    rows: list[dict[str, Any]],
    dimensions: tuple[str, ...],
    metric_column: str,
    scale: float = 1.0,
    fallback_metric: str | None = None,
) -> dict[tuple[str, ...], float]:
    """Average a displayed metric over all dimensions not named by the plot."""

    grouped: dict[tuple[str, ...], list[float]] = defaultdict(list)
    for row in rows:
        value = final_metric_value(row, metric_column, fallback_metric)
        key = tuple(str(row.get(column, "")) for column in dimensions)
        if value is None or any(not part for part in key):
            continue
        grouped[key].append(scale * value)
    return {
        key: sum(values) / len(values)
        for key, values in grouped.items()
        if values
    }


def final_simulator_contention_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Create the paper view that validates queue scaling and burst pressure."""

    calibration_rows = final_study_rows(
        comparison_rows,
        "eval_final_contention_scaling",
        "eval_contention_calibration",
    )
    burst_rows = [
        row for row in comparison_rows
        if str(row.get("preset", "")) == "eval_bursty_calibration"
    ]
    if not calibration_rows:
        return plot_placeholder(
            plt,
            "Simulator contention calibration",
            "No final contention-scaling rows were supplied.",
        )

    fig, axes = plt.subplots(
        2,
        2,
        figsize=(13.5, 8.2),
        constrained_layout=True,
    )
    scale_specs = (
        (
            "average_memory_wait",
            None,
            "Average memory wait",
        ),
        (
            "peak_memory_channel_queue_depth",
            "peak_memory_queue_depth",
            "Peak channel queue depth",
        ),
    )
    bandwidth_styles = {
        "mild": "-",
        "moderate": "--",
        "severe": ":",
    }
    policies = [
        policy for policy in ("always_remote", "lru")
        if any(row.get("policy") == policy for row in calibration_rows)
    ]
    for ax, (metric_column, fallback, ylabel) in zip(
        axes[0],
        scale_specs,
    ):
        grouped = final_grouped_points(
            calibration_rows,
            ("policy", "memory_bandwidth_level", "node_count"),
            metric_column,
            fallback_metric=fallback,
        )
        for policy in policies:
            for bandwidth in distinct_values(
                calibration_rows,
                "memory_bandwidth_level",
            ):
                points = []
                for node_count in distinct_values(
                    calibration_rows,
                    "node_count",
                ):
                    value = grouped.get((policy, bandwidth, node_count))
                    numeric_node = float_or_none(node_count)
                    if value is not None and numeric_node is not None:
                        points.append((numeric_node, value))
                if not points:
                    continue
                points.sort()
                ax.plot(
                    [point[0] for point in points],
                    [point[1] for point in points],
                    marker="o",
                    color=FINAL_POLICY_COLORS.get(policy),
                    linestyle=bandwidth_styles.get(bandwidth, "-"),
                    label=f"{final_policy_label(policy)}, {bandwidth}",
                )
        ax.set_xlabel("Compute node count")
        ax.set_ylabel(ylabel)
        ax.grid(alpha=0.25)

    axes[0][0].legend(fontsize="x-small", ncol=2)
    burst_specs = (
        ("average_memory_wait", "Average memory wait"),
        ("p99_latency", "P99 request latency"),
    )
    for ax, (metric_column, ylabel) in zip(axes[1], burst_specs):
        if not burst_rows:
            ax.axis("off")
            ax.text(
                0.5,
                0.5,
                "Add eval_bursty_calibration to show\n"
                "completion-driven vs scheduled-bursty pressure.",
                ha="center",
                va="center",
            )
            continue
        modes = distinct_values(burst_rows, "workload_issue_mode")
        grouped = final_grouped_points(
            burst_rows,
            ("policy", "workload_issue_mode"),
            metric_column,
        )
        bar_width = 0.36
        for policy_index, policy in enumerate(
            [
                item for item in ("always_remote", "lru")
                if any(row.get("policy") == item for row in burst_rows)
            ]
        ):
            values = [
                grouped.get((policy, mode), 0.0) for mode in modes
            ]
            offset = (policy_index - 0.5) * bar_width
            ax.bar(
                [index + offset for index in range(len(modes))],
                values,
                width=bar_width,
                color=FINAL_POLICY_COLORS.get(policy),
                label=final_policy_label(policy),
            )
        ax.set_xticks(range(len(modes)))
        ax.set_xticklabels(
            [mode.replace("_", " ") for mode in modes],
            rotation=12,
        )
        ax.set_ylabel(ylabel)
        ax.grid(axis="y", alpha=0.25)
    if burst_rows:
        axes[1][0].legend(fontsize="small")
    fig.suptitle(
        "Simulator contention scales with demand and overlapping arrivals",
        fontsize=15,
    )
    return fig


def final_pressure_policy_scaling_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Plot policy gains as node pressure rises at bandwidth endpoints."""

    rows = final_study_rows(
        comparison_rows,
        "eval_final_node_bandwidth",
        "eval_interactions_node_bandwidth",
    )
    rows = [
        row for row in rows
        if str(row.get("policy", "")) in FINAL_REPORT_POLICY_ORDER
        and str(row.get("policy", "")) != "lru"
    ]
    if not rows:
        return plot_placeholder(
            plt,
            "Policy benefit under memory pressure",
            "No final node-bandwidth policy rows were supplied.",
        )
    available_bandwidths = distinct_values(rows, "memory_bandwidth_level")
    bandwidths = [
        value for value in ("mild", "severe")
        if value in available_bandwidths
    ] or available_bandwidths
    metrics = (
        (
            "mean_latency_delta_pct",
            "Mean latency improvement vs LRU (%)",
        ),
        (
            "average_memory_wait_delta_pct",
            "Memory-wait reduction vs LRU (%)",
        ),
    )
    fig, axes = plt.subplots(
        len(metrics),
        len(bandwidths),
        figsize=(6.2 * len(bandwidths), 7.6),
        squeeze=False,
        sharex="col",
    )
    for column_index, bandwidth in enumerate(bandwidths):
        bandwidth_rows = [
            row for row in rows
            if str(row.get("memory_bandwidth_level", "")) == bandwidth
        ]
        for metric_index, (metric_column, ylabel) in enumerate(metrics):
            ax = axes[metric_index][column_index]
            grouped = final_grouped_points(
                bandwidth_rows,
                ("policy", "node_count"),
                metric_column,
                scale=-100.0,
            )
            for policy in FINAL_REPORT_POLICY_ORDER:
                if policy == "lru":
                    continue
                points = []
                for node_count in distinct_values(
                    bandwidth_rows,
                    "node_count",
                ):
                    value = grouped.get((policy, node_count))
                    numeric_node = float_or_none(node_count)
                    if value is not None and numeric_node is not None:
                        points.append((numeric_node, value))
                if points:
                    points.sort()
                    ax.plot(
                        [point[0] for point in points],
                        [point[1] for point in points],
                        marker="o",
                        color=FINAL_POLICY_COLORS.get(policy),
                        label=final_policy_label(policy),
                    )
            ax.axhline(0.0, color="black", linewidth=0.8, alpha=0.45)
            ax.grid(alpha=0.25)
            if metric_index == 0:
                ax.set_title(f"{bandwidth.title()} memory bandwidth")
            if column_index == 0:
                ax.set_ylabel(ylabel)
            if metric_index == len(metrics) - 1:
                ax.set_xlabel("Compute node count")
    handles, labels = axes[0][0].get_legend_handles_labels()
    if handles:
        fig.legend(
            handles,
            labels,
            loc="center right",
            bbox_to_anchor=(0.995, 0.5),
            ncol=1,
            fontsize="small",
        )
    fig.suptitle(
        "Contention-aware benefit as shared-memory pressure increases",
        fontsize=15,
    )
    fig.tight_layout(rect=[0.0, 0.0, 0.78, 0.94])
    return fig


def final_policy_operating_region_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Plot cache-pressure response without averaging over churn or RPE."""

    rows = final_study_rows(
        comparison_rows,
        "eval_final_policy_iteration",
        "eval_policy_iteration",
    )
    rows = [
        row for row in rows
        if str(row.get("policy", "")) in FINAL_REPORT_POLICY_ORDER
        and str(row.get("policy", "")) != "lru"
    ]
    churn_values = distinct_values(rows, "hot_set_churn_fraction")
    rpe_values = distinct_values(rows, "requests_per_node_per_epoch")
    if not rows or not churn_values or not rpe_values:
        return plot_placeholder(
            plt,
            "Latest-policy operating region",
            "No final policy-iteration rows were supplied.",
        )

    fig, axes = plt.subplots(
        len(rpe_values),
        len(churn_values),
        figsize=(4.8 * len(churn_values), 3.8 * len(rpe_values)),
        squeeze=False,
        sharex=True,
        sharey=True,
    )
    for row_index, rpe in enumerate(rpe_values):
        for column_index, churn in enumerate(churn_values):
            ax = axes[row_index][column_index]
            facet_rows = [
                row for row in rows
                if str(row.get("requests_per_node_per_epoch", "")) == rpe
                and str(row.get("hot_set_churn_fraction", "")) == churn
            ]
            grouped = final_grouped_points(
                facet_rows,
                ("policy", "cache_capacity_hotset_multiplier"),
                "mean_latency_delta_pct",
                scale=-100.0,
            )
            for policy in FINAL_REPORT_POLICY_ORDER:
                if policy == "lru":
                    continue
                points = []
                for cache_multiplier in distinct_values(
                    facet_rows,
                    "cache_capacity_hotset_multiplier",
                ):
                    value = grouped.get((policy, cache_multiplier))
                    numeric_cache = float_or_none(cache_multiplier)
                    if value is not None and numeric_cache is not None:
                        points.append((numeric_cache, value))
                if points:
                    points.sort()
                    ax.plot(
                        [point[0] for point in points],
                        [point[1] for point in points],
                        marker="o",
                        color=FINAL_POLICY_COLORS.get(policy),
                        label=final_policy_label(policy),
                    )
            ax.axhline(0.0, color="black", linewidth=0.8, alpha=0.45)
            ax.grid(alpha=0.25)
            if row_index == 0:
                ax.set_title(f"Churn = {churn}")
            if column_index == 0:
                ax.set_ylabel(
                    f"RPE = {rpe}\nLatency improvement vs LRU (%)"
                )
            if row_index == len(rpe_values) - 1:
                ax.set_xlabel("Cache capacity / hot-set footprint")
    handles, labels = axes[0][0].get_legend_handles_labels()
    if handles:
        fig.legend(
            handles,
            labels,
            loc="center right",
            bbox_to_anchor=(0.995, 0.5),
            ncol=1,
            fontsize="small",
        )
    fig.suptitle(
        "Latest-policy operating region across churn, reuse, and cache pressure",
        fontsize=15,
    )
    fig.tight_layout(rect=[0.0, 0.0, 0.76, 0.94])
    return fig


def final_policy_mechanism_plot(
    plt: Any,
    comparison_rows: list[dict[str, Any]],
) -> Any:
    """Explain policy gains through pressure reduction and placement quality."""

    rows = final_study_rows(
        comparison_rows,
        "eval_final_policy_iteration",
        "eval_policy_iteration",
    )
    mechanism_policies = {
        "contention_aware_smoothed",
        "contention_aware_smoothed_reuse_gated",
        "contention_aware_smoothed_reuse_gated_hysteresis",
        "contention_aware_size_value",
    }
    rows = [
        row for row in rows
        if str(row.get("policy", "")) in mechanism_policies
    ]
    if not rows:
        return plot_placeholder(
            plt,
            "Contention-aware policy mechanisms",
            "No latest contention-aware policy rows were supplied.",
        )
    metrics = (
        (
            "total_remote_accesses_delta_pct",
            "Remote-access reduction vs LRU (%)",
            -100.0,
        ),
        (
            "average_memory_wait_delta_pct",
            "Memory-wait reduction vs LRU (%)",
            -100.0,
        ),
        (
            "admission_yield",
            "Admission yield (future hits / placement)",
            1.0,
        ),
        ("reuse_after_admit_rate", "Reuse after admission (%)", 100.0),
    )
    fig, axes = plt.subplots(
        2,
        2,
        figsize=(13, 8),
        sharex=True,
    )
    for ax, (metric_column, ylabel, scale) in zip(axes.flat, metrics):
        grouped = final_grouped_points(
            rows,
            ("policy", "hot_set_churn_fraction"),
            metric_column,
            scale=scale,
        )
        for policy in FINAL_REPORT_POLICY_ORDER:
            if policy not in mechanism_policies:
                continue
            points = []
            for churn in distinct_values(rows, "hot_set_churn_fraction"):
                value = grouped.get((policy, churn))
                numeric_churn = float_or_none(churn)
                if value is not None and numeric_churn is not None:
                    points.append((numeric_churn, value))
            if points:
                points.sort()
                ax.plot(
                    [point[0] for point in points],
                    [point[1] for point in points],
                    marker="o",
                    color=FINAL_POLICY_COLORS.get(policy),
                    label=final_policy_label(policy),
                )
        if "reduction" in ylabel.lower():
            ax.axhline(0.0, color="black", linewidth=0.8, alpha=0.45)
        ax.set_ylabel(ylabel)
        ax.set_xlabel("Hot-set churn fraction")
        ax.grid(alpha=0.25)
    handles, labels = axes[0][0].get_legend_handles_labels()
    if handles:
        fig.legend(
            handles,
            labels,
            loc="center right",
            bbox_to_anchor=(0.995, 0.5),
            ncol=1,
            fontsize="small",
        )
    fig.suptitle(
        "Why the latest contention-aware variants behave differently",
        fontsize=15,
    )
    fig.tight_layout(rect=[0.0, 0.0, 0.77, 0.94])
    return fig


def generate_final_report_plots(
    plt: Any,
    plots_dir: Path,
    formats: list[str],
    comparison_rows: list[dict[str, Any]],
) -> list[PlotOutput]:
    """Generate only the four figures intended for the final report."""

    return save_plot_specs(
        plt,
        plots_dir,
        formats,
        "Final Report Figures",
        [
            (
                "final_simulator_contention",
                final_simulator_contention_plot(plt, comparison_rows),
            ),
            (
                "final_pressure_policy_scaling",
                final_pressure_policy_scaling_plot(plt, comparison_rows),
            ),
            (
                "final_policy_operating_region",
                final_policy_operating_region_plot(plt, comparison_rows),
            ),
            (
                "final_policy_mechanisms",
                final_policy_mechanism_plot(plt, comparison_rows),
            ),
        ],
    )


def save_plot_specs(plt: Any,
                    plots_dir: Path,
                    formats: list[str],
                    group: str,
                    plot_specs: list[tuple[str, Any]]) -> list[PlotOutput]:
    """Save a plot group and preserve section metadata for the report."""

    outputs: list[PlotOutput] = []
    for name, fig in plot_specs:
        for path in save_figure(fig, plots_dir, name, formats):
            outputs.append(PlotOutput(path=path, group=group))
        plt.close(fig)
    return outputs


def generate_common_plots(plt: Any,
                          plots_dir: Path,
                          formats: list[str],
                          comparison_rows: list[dict[str, Any]],
                          policy_summary: list[dict[str, Any]]) -> list[PlotOutput]:
    """Generate plots that are useful for every report mode."""

    return save_plot_specs(
        plt,
        plots_dir,
        formats,
        "Global Average Views",
        [
            ("policy_latency_overview",
             make_policy_latency_plot(plt, policy_summary)),
            ("policy_cache_memory_overview",
             make_cache_memory_plot(plt, policy_summary)),
            ("latency_delta_vs_baseline",
             make_latency_delta_plot(plt, policy_summary)),
            ("fairness_overview", make_fairness_plot(plt, policy_summary)),
        ],
    )


def generate_generic_plots(plt: Any,
                           plots_dir: Path,
                           formats: list[str],
                           comparison_rows: list[dict[str, Any]],
                           swept: set[str]) -> list[PlotOutput]:
    """Generate dimension-aware generic sweep plots."""

    plot_specs: list[tuple[str, Any]] = []
    if "hot_set_churn_fraction" in swept:
        plot_specs.append(
            (
                "churn_sweep",
                line_sweep_plot(
                    plt,
                    comparison_rows,
                    "hot_set_churn_fraction",
                    [
                        ("mean_latency_delta_pct",
                         "Mean latency delta (%)",
                         100.0),
                        ("stale_telemetry_rate",
                         "Hot-set-sized stale telemetry",
                         1.0),
                    ],
                    "Temporal stability sweep",
                    "Hot-set churn fraction",
                ),
            )
        )
    if "requests_per_node_per_epoch" in swept:
        plot_specs.append(
            (
                "epoch_length_sweep",
                line_sweep_plot(
                    plt,
                    comparison_rows,
                    "requests_per_node_per_epoch",
                    [("mean_latency_delta_pct",
                      "Mean latency delta (%)",
                      100.0)],
                    "Epoch length sweep",
                    "Requests per node per epoch",
                ),
            )
        )
    if "cache_capacity_hotset_multiplier" in swept:
        plot_specs.append(
            (
                "cache_pressure_sweep",
                line_sweep_plot(
                    plt,
                    comparison_rows,
                    "cache_capacity_hotset_multiplier",
                    [
                        ("mean_latency_delta_pct",
                         "Mean latency delta (%)",
                         100.0),
                        ("local_hit_rate", "Local hit rate", 1.0),
                    ],
                    "Cache pressure sweep",
                    "Cache capacity / hot-set footprint",
                ),
            )
        )
    if "memory_bandwidth_level" in swept:
        plot_specs.append(
            (
                "memory_bottleneck_sweep",
                line_sweep_plot(
                    plt,
                    comparison_rows,
                    "memory_bandwidth_level",
                    [
                        ("average_memory_wait", "Average memory wait", 1.0),
                        ("mean_latency_delta_pct",
                         "Mean latency delta (%)",
                         100.0),
                    ],
                    "Memory bottleneck sweep",
                    "Memory bandwidth level",
                    numeric_x=False,
                ),
            )
        )
    plot_specs.append(("viability_scatter",
                       make_viability_scatter(plt, comparison_rows)))
    return save_plot_specs(plt, plots_dir, formats, "Generic Sweeps", plot_specs)


def generate_contention_calibration_plots(
    plt: Any,
    plots_dir: Path,
    formats: list[str],
    comparison_rows: list[dict[str, Any]],
    policy_summary: list[dict[str, Any]],
    swept: set[str],
) -> list[PlotOutput]:
    """Generate plots for validating simulator contention behavior."""

    plot_specs: list[tuple[str, Any]] = []
    if "node_count" in swept:
        plot_specs.append(
            ("calibration_node_count_pressure",
             calibration_node_count_pressure_plot(plt, comparison_rows))
        )
    if {"node_count", "memory_bandwidth_level"}.issubset(swept):
        plot_specs.append(
            (
                "calibration_memory_pressure_heatmap",
                calibration_memory_pressure_heatmap(plt, comparison_rows),
            )
        )
    if "hot_access_probability" in swept:
        plot_specs.append(
            (
                "calibration_hotness_memory_pressure_heatmap",
                calibration_hotness_memory_pressure_heatmap(
                    plt,
                    comparison_rows,
                ),
            )
        )
    if "memory_bandwidth_level" in swept:
        plot_specs.append(
            (
                "calibration_bandwidth_pressure",
                line_sweep_plot(
                    plt,
                    comparison_rows,
                    "memory_bandwidth_level",
                    [
                        ("average_memory_wait", "Average memory wait", 1.0),
                        ("total_queue_wait", "Total queue wait", 1.0),
                        ("total_remote_service_time",
                         "Total remote service time",
                         1.0),
                    ],
                    "Memory bandwidth pressure",
                    "Memory bandwidth level",
                    numeric_x=False,
                ),
            )
        )
    if "memory_channel_count" in swept:
        plot_specs.append(
            (
                "calibration_channel_count_pressure",
                calibration_channel_count_pressure_plot(
                    plt,
                    comparison_rows,
                ),
            )
        )
    if "workload_issue_mode" in swept:
        plot_specs.append(
            (
                "calibration_issue_mode_pressure",
                calibration_issue_mode_pressure_plot(
                    plt,
                    comparison_rows,
                ),
            )
        )
    if "hot_object_channel_count" in swept:
        plot_specs.append(
            (
                "calibration_channel_hotspot_pressure",
                calibration_channel_hotspot_plot(
                    plt,
                    comparison_rows,
                ),
            )
        )
    if {"memory_channel_count", "hot_object_channel_count"}.issubset(swept):
        plot_specs.append(
            (
                "calibration_channel_pressure_heatmap",
                calibration_channel_pressure_heatmap(
                    plt,
                    comparison_rows,
                ),
            )
        )
    if {"cross_node_overlap", "hot_access_probability"}.issubset(swept):
        plot_specs.append(
            (
                "calibration_overlap_hotness_heatmap",
                heatmap_by_policy(
                    plt,
                    comparison_rows,
                    "hot_access_probability",
                    "cross_node_overlap",
                    "average_memory_wait",
                    "Overlap and hotness concentration pressure",
                    "Hot-access probability",
                    "Cross-node overlap",
                    "Average memory wait",
                ),
            )
        )
        plot_specs.append(
            (
                "calibration_overlap_hotness_concentration",
                calibration_overlap_hotness_concentration_plot(
                    plt,
                    comparison_rows,
                ),
            )
        )
    if len({row.get("policy", "") for row in comparison_rows}) > 1:
        plot_specs.append(
            ("calibration_policy_pressure_reduction",
             calibration_policy_pressure_reduction_plot(plt, policy_summary))
        )
    return save_plot_specs(
        plt,
        plots_dir,
        formats,
        "Contention Calibration",
        plot_specs,
    )


def generate_policy_viability_plots(
    plt: Any,
    plots_dir: Path,
    formats: list[str],
    comparison_rows: list[dict[str, Any]],
    policy_summary: list[dict[str, Any]],
    swept: set[str],
    best_run_context: list[dict[str, Any]],
) -> list[PlotOutput]:
    """Generate plots for contention-aware policy viability studies."""

    plot_specs: list[tuple[str, Any]] = []
    conditioned_specs: list[tuple[str, Any]] = []
    best_run_specs: list[tuple[str, Any]] = []
    if {"hot_set_churn_fraction",
            "requests_per_node_per_epoch"}.issubset(swept):
        rows = [
            row for row in comparison_rows
            if "contention_aware" in str(row.get("policy", ""))
        ]
        plot_specs.append(
            (
                "viability_churn_epoch_heatmap",
                heatmap_by_policy(
                    plt,
                    rows,
                    "requests_per_node_per_epoch",
                    "hot_set_churn_fraction",
                    "mean_latency_delta_pct",
                    "Contention-aware latency delta by churn and epoch length",
                    "Requests per node per epoch",
                    "Hot-set churn fraction",
                    "Mean latency delta vs baseline (%)",
                    scale=100.0,
                ),
            )
        )
    if "cache_capacity_hotset_multiplier" in swept:
        plot_specs.append(
            (
                "viability_cache_pressure",
                line_sweep_plot(
                    plt,
                    comparison_rows,
                    "cache_capacity_hotset_multiplier",
                    [
                        ("mean_latency_delta_pct",
                         "Mean latency delta (%)",
                         100.0),
                        ("local_hit_rate", "Local hit rate", 1.0),
                    ],
                    "Policy viability across cache pressure",
                    "Cache capacity / hot-set footprint",
                ),
            )
        )
        if distinct_values(comparison_rows, "hot_set_churn_fraction"):
            conditioned_specs.append(
                (
                    "viability_cache_pressure_by_churn",
                    faceted_line_sweep_plot(
                        plt,
                        comparison_rows,
                        "hot_set_churn_fraction",
                        "cache_capacity_hotset_multiplier",
                        [
                            ("mean_latency_delta_pct",
                             "Mean latency delta (%)",
                             100.0),
                            ("local_hit_rate", "Local hit rate", 1.0),
                        ],
                        "Policy viability across cache pressure by churn",
                        "Hot-set churn",
                        "Cache capacity / hot-set footprint",
                    ),
                )
            )
    if "hot_set_churn_fraction" in swept:
        conditioned_specs.extend(
            [
                (
                    "viability_admission_quality_by_churn",
                    viability_admission_quality_by_churn_plot(
                        plt,
                        comparison_rows,
                    ),
                ),
                (
                    "viability_remote_pressure_by_churn",
                    viability_remote_pressure_by_churn_plot(
                        plt,
                        comparison_rows,
                    ),
                ),
                (
                    "viability_fairness_by_churn",
                    viability_fairness_by_churn_plot(
                        plt,
                        comparison_rows,
                    ),
                ),
            ]
        )
    plot_specs.extend(
        [
            ("viability_scatter",
             make_viability_scatter(plt, comparison_rows)),
            ("viability_admission_quality",
             viability_admission_quality_plot(plt, policy_summary)),
        ]
    )
    best_run_specs.extend(
        [
            (
                "viability_best_run_latency_examples",
                make_best_run_latency_examples_plot(plt, best_run_context),
            ),
            (
                "viability_best_run_pressure_examples",
                make_best_run_pressure_examples_plot(plt, best_run_context),
            ),
        ]
    )
    outputs = save_plot_specs(
        plt,
        plots_dir,
        formats,
        "Policy Viability Global Views",
        plot_specs,
    )
    outputs.extend(
        save_plot_specs(
            plt,
            plots_dir,
            formats,
            "Churn-Conditioned Policy Viability",
            conditioned_specs,
        )
    )
    outputs.extend(
        save_plot_specs(
            plt,
            plots_dir,
            formats,
            "Best Run Examples",
            best_run_specs,
        )
    )
    return outputs


def interaction_axes_for_preset(preset: str) -> tuple[str, str] | None:
    """Return the intended heatmap axes for a focused interaction preset."""

    if preset == "eval_interactions_churn_epoch":
        return ("requests_per_node_per_epoch", "hot_set_churn_fraction")
    if preset == "eval_interactions_cache_hotset":
        return ("cache_capacity_hotset_multiplier", "hot_set_size")
    if preset == "eval_interactions_node_bandwidth":
        return ("node_count", "memory_bandwidth_level")
    return None


def generate_interaction_plots(
    plt: Any,
    plots_dir: Path,
    formats: list[str],
    comparison_rows: list[dict[str, Any]],
    swept: set[str],
) -> list[PlotOutput]:
    """Generate focused two-dimensional interaction heatmaps."""

    presets = distinct_values(comparison_rows, "preset")
    axes = interaction_axes_for_preset(presets[0]) if len(presets) == 1 else None
    if axes is None or not set(axes).issubset(swept):
        return generate_generic_plots(plt, plots_dir, formats,
                                      comparison_rows, swept)
    x_column, y_column = axes
    plot_specs = [
        (
            "interaction_mean_latency_delta_heatmap",
            heatmap_by_policy(
                plt,
                comparison_rows,
                x_column,
                y_column,
                "mean_latency_delta_pct",
                "Interaction: mean latency delta",
                x_column.replace("_", " ").title(),
                y_column.replace("_", " ").title(),
                "Mean latency delta vs baseline (%)",
                scale=100.0,
            ),
        ),
        (
            "interaction_memory_wait_heatmap",
            heatmap_by_policy(
                plt,
                comparison_rows,
                x_column,
                y_column,
                "average_memory_wait",
                "Interaction: average memory wait",
                x_column.replace("_", " ").title(),
                y_column.replace("_", " ").title(),
                "Average memory wait",
            ),
        ),
        (
            "interaction_hit_rate_heatmap",
            heatmap_by_policy(
                plt,
                comparison_rows,
                x_column,
                y_column,
                "local_hit_rate",
                "Interaction: local hit rate",
                x_column.replace("_", " ").title(),
                y_column.replace("_", " ").title(),
                "Local hit rate",
            ),
        ),
    ]
    return save_plot_specs(plt, plots_dir, formats, "Interaction Studies",
                           plot_specs)


def parameter_demo_axis_for_preset(preset: str) -> tuple[str, str] | None:
    """Return the single intended sweep axis for an isolated knob preset."""

    # Phase 4B presets are deliberately one-knob demonstrations. Keeping this
    # mapping explicit prevents a report from silently choosing a misleading
    # x-axis when fixed background dimensions are later adjusted.
    if preset == "eval_knob_cache_pressure":
        return ("cache_capacity_hotset_multiplier",
                "Cache capacity / hot-set footprint")
    if preset == "eval_knob_epoch_reuse":
        return ("requests_per_node_per_epoch",
                "Requests per node per epoch")
    if preset == "eval_knob_object_size_mix":
        return ("large_object_probability", "Large-object probability")
    if preset == "eval_knob_hot_concentration":
        return ("hot_access_probability", "Hot-access probability")
    return None


def generate_parameter_demo_plots(
    plt: Any,
    plots_dir: Path,
    formats: list[str],
    comparison_rows: list[dict[str, Any]],
    swept: set[str],
) -> list[PlotOutput]:
    """Generate compact one-knob plots for presentation-oriented studies."""

    presets = distinct_values(comparison_rows, "preset")
    axis = parameter_demo_axis_for_preset(presets[0]) if len(presets) == 1 else None
    if axis is None or axis[0] not in swept:
        return generate_generic_plots(plt, plots_dir, formats,
                                      comparison_rows, swept)

    x_column, x_label = axis
    # Each plot averages over all non-axis dimensions. That is intentional for
    # Phase 4B: these studies are explanatory slices, not exhaustive rankings.
    plot_specs = [
        (
            "parameter_demo_latency_hit_rate",
            line_sweep_plot(
                plt,
                comparison_rows,
                x_column,
                [
                    ("mean_latency_delta_pct",
                     "Mean latency delta (%)",
                     100.0),
                    ("local_hit_rate", "Local hit rate", 1.0),
                ],
                "Isolated knob effect on latency and cache hits",
                x_label,
            ),
        ),
        (
            "parameter_demo_memory_pressure",
            line_sweep_plot(
                plt,
                comparison_rows,
                x_column,
                [
                    ("average_memory_wait", "Average memory wait", 1.0),
                    ("total_remote_accesses", "Total remote accesses", 1.0),
                ],
                "Isolated knob effect on remote-memory pressure",
                x_label,
            ),
        ),
        (
            "parameter_demo_admission_quality",
            line_sweep_plot(
                plt,
                comparison_rows,
                x_column,
                [
                    ("admission_yield", "Admission yield", 1.0),
                    ("reuse_after_admit_rate",
                     "Reuse-after-admit rate",
                     1.0),
                ],
                "Isolated knob effect on admission quality",
                x_label,
            ),
        ),
        (
            "parameter_demo_tail_latency",
            line_sweep_plot(
                plt,
                comparison_rows,
                x_column,
                [
                    ("p99_latency_delta_pct",
                     "P99 latency delta (%)",
                     100.0),
                ],
                "Isolated knob effect on tail latency",
                x_label,
            ),
        ),
    ]
    return save_plot_specs(
        plt,
        plots_dir,
        formats,
        "Isolated Parameter Demonstrations",
        plot_specs,
    )


def generate_weight_sensitivity_plots(
    plt: Any,
    plots_dir: Path,
    formats: list[str],
    comparison_rows: list[dict[str, Any]],
) -> list[PlotOutput]:
    """Generate one-at-a-time contention weight sensitivity plots."""

    # LRU has blank weight metadata and exists only as the shared baseline.
    # Filtering keeps the visual story focused on how each contention-aware
    # score component responds as its weight is varied.
    rows = [
        row for row in comparison_rows
        if str(row.get("contention_weight_name", "")).strip()
    ]
    plot_specs = [
        (
            "weight_sensitivity_latency",
            faceted_line_sweep_plot(
                plt,
                rows,
                "contention_weight_name",
                "contention_weight_value",
                [
                    ("mean_latency_delta_pct",
                     "Mean latency delta (%)",
                     100.0),
                    ("p99_latency_delta_pct",
                     "P99 latency delta (%)",
                     100.0),
                ],
                "Contention-aware latency sensitivity by score weight",
                "Weight",
                "Weight value",
            ),
        ),
        (
            "weight_sensitivity_memory_hit_rate",
            faceted_line_sweep_plot(
                plt,
                rows,
                "contention_weight_name",
                "contention_weight_value",
                [
                    ("local_hit_rate", "Local hit rate", 1.0),
                    ("average_memory_wait", "Average memory wait", 1.0),
                ],
                "Contention-aware cache and memory sensitivity by score weight",
                "Weight",
                "Weight value",
            ),
        ),
        (
            "weight_sensitivity_admission_regret",
            faceted_line_sweep_plot(
                plt,
                rows,
                "contention_weight_name",
                "contention_weight_value",
                [
                    ("admission_yield", "Admission yield", 1.0),
                    ("eviction_regret_count", "Eviction regret", 1.0),
                ],
                "Contention-aware admission sensitivity by score weight",
                "Weight",
                "Weight value",
            ),
        ),
    ]
    return save_plot_specs(
        plt,
        plots_dir,
        formats,
        "Contention Weight Sensitivity",
        plot_specs,
    )


def generate_plots(plt: Any,
                   output_dir: Path,
                   formats: list[str],
                   comparison_rows: list[dict[str, Any]],
                   policy_summary: list[dict[str, Any]],
                   report_mode: str,
                   dimensions: dict[str, dict[str, Any]],
                   best_run_context: list[dict[str, Any]]) -> list[PlotOutput]:
    """Generate common and mode-specific plot bundles."""

    plots_dir = output_dir / "plots"
    swept = swept_dimensions(dimensions)
    if report_mode == "final_report":
        # The paper bundle is intentionally compact. Exploratory common plots
        # remain available through the existing mode-specific reports.
        return generate_final_report_plots(
            plt,
            plots_dir,
            formats,
            comparison_rows,
        )
    outputs = generate_common_plots(
        plt,
        plots_dir,
        formats,
        comparison_rows,
        policy_summary,
    )
    if report_mode == "contention_calibration":
        outputs.extend(
            generate_contention_calibration_plots(
                plt,
                plots_dir,
                formats,
                comparison_rows,
                policy_summary,
                swept,
            )
        )
    elif report_mode == "policy_viability":
        outputs.extend(
            generate_policy_viability_plots(
                plt,
                plots_dir,
                formats,
                comparison_rows,
                policy_summary,
                swept,
                best_run_context,
            )
        )
    elif report_mode == "interaction":
        outputs.extend(
            generate_interaction_plots(
                plt,
                plots_dir,
                formats,
                comparison_rows,
                swept,
            )
        )
    elif report_mode == "parameter_demo":
        outputs.extend(
            generate_parameter_demo_plots(
                plt,
                plots_dir,
                formats,
                comparison_rows,
                swept,
            )
        )
    elif report_mode == "weight_sensitivity":
        outputs.extend(
            generate_weight_sensitivity_plots(
                plt,
                plots_dir,
                formats,
                comparison_rows,
            )
        )
    else:
        outputs.extend(
            generate_generic_plots(plt, plots_dir, formats,
                                   comparison_rows, swept)
        )
    return outputs


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


def average_for_dimension_value(rows: list[dict[str, Any]],
                                dimension: str,
                                value: str,
                                metric_column: str) -> float | None:
    """Average a metric for rows matching one dimension value."""

    return average(
        [row for row in rows if str(row.get(dimension, "")) == value],
        metric_column,
    )


def format_dimension_values(values: list[str], limit: int = 8) -> str:
    """Format a compact list of dimension values for Markdown."""

    if len(values) <= limit:
        return ", ".join(f"`{value}`" for value in values)
    shown = ", ".join(f"`{value}`" for value in values[:limit])
    return f"{shown}, ... ({len(values)} values)"


def build_observations(
    comparison_rows: list[dict[str, Any]],
    policy_summary: list[dict[str, Any]],
    baseline: str,
    report_mode: str,
    dimensions: dict[str, dict[str, Any]],
) -> list[str]:
    """Generate lightweight interpretation notes from aggregate metrics."""

    observations: list[str] = []
    swept = swept_dimensions(dimensions)
    if report_mode == "final_report":
        seeds = distinct_values(comparison_rows, "seed")
        observations.append(
            f"Final-study estimates include {len(seeds)} matched workload "
            f"seed{'s' if len(seeds) != 1 else ''}."
        )
        calibration_rows = final_study_rows(
            comparison_rows,
            "eval_final_contention_scaling",
            "eval_contention_calibration",
        )
        node_values = distinct_values(calibration_rows, "node_count")
        if len(node_values) >= 2:
            low_wait = average_for_dimension_value(
                calibration_rows,
                "node_count",
                node_values[0],
                "average_memory_wait",
            )
            high_wait = average_for_dimension_value(
                calibration_rows,
                "node_count",
                node_values[-1],
                "average_memory_wait",
            )
            if low_wait is not None and high_wait is not None:
                observations.append(
                    "Average memory wait changed from "
                    f"{low_wait:.2f} at {node_values[0]} nodes to "
                    f"{high_wait:.2f} at {node_values[-1]} nodes."
                )
        final_table = build_final_policy_table(comparison_rows)
        candidates = [
            row for row in final_table
            if row.get("policy") != "lru"
            and value_for_plot(row, "mean_latency_improvement_pct") is not None
        ]
        if candidates:
            best = max(
                candidates,
                key=lambda row: value_for_plot(
                    row,
                    "mean_latency_improvement_pct",
                ) or float("-inf"),
            )
            observations.append(
                f"`{best['policy']}` had the largest condition-averaged "
                "latency improvement in the final policy study "
                f"({best['mean_latency_improvement_pct']:.1f}%)."
            )
        return observations

    if report_mode == "contention_calibration":
        concentration = object_concentration_by_run(comparison_rows, top_k=5)
        if concentration:
            low_rows = [
                row for row in comparison_rows
                if row.get("cross_node_overlap") == "low"
                and str(row.get("experiment_name", "")) in concentration
            ]
            high_rows = [
                row for row in comparison_rows
                if row.get("cross_node_overlap") == "high"
                and str(row.get("experiment_name", "")) in concentration
            ]
            low_requesters = average(
                [
                    {
                        "value": concentration[
                            str(row.get("experiment_name", ""))
                        ].top_distinct_requesters
                    }
                    for row in low_rows
                ],
                "value",
            )
            high_requesters = average(
                [
                    {
                        "value": concentration[
                            str(row.get("experiment_name", ""))
                        ].top_distinct_requesters
                    }
                    for row in high_rows
                ],
                "value",
            )
            if low_requesters is not None and high_requesters is not None:
                direction = (
                    "increased" if high_requesters > low_requesters
                    else "did not increase"
                )
                observations.append(
                    "Top-object requester diversity "
                    f"{direction} from low overlap ({low_requesters:.2f}) "
                    f"to high overlap ({high_requesters:.2f})."
                )
        if "node_count" in swept:
            node_values = distinct_values(comparison_rows, "node_count")
            low_node = node_values[0]
            high_node = node_values[-1]
            low_wait = average_for_dimension_value(
                comparison_rows,
                "node_count",
                low_node,
                "average_memory_wait",
            )
            high_wait = average_for_dimension_value(
                comparison_rows,
                "node_count",
                high_node,
                "average_memory_wait",
            )
            if low_wait is not None and high_wait is not None:
                direction = "increased" if high_wait > low_wait else "did not increase"
                observations.append(
                    f"Average memory wait {direction} from node count "
                    f"`{low_node}` ({low_wait:.2f}) to `{high_node}` "
                    f"({high_wait:.2f})."
                )
        if "memory_bandwidth_level" in swept:
            mild_wait = average_for_dimension_value(
                comparison_rows,
                "memory_bandwidth_level",
                "mild",
                "average_memory_wait",
            )
            severe_wait = average_for_dimension_value(
                comparison_rows,
                "memory_bandwidth_level",
                "severe",
                "average_memory_wait",
            )
            if mild_wait is not None and severe_wait is not None:
                direction = "higher" if severe_wait > mild_wait else "not higher"
                observations.append(
                    f"Severe bandwidth produced {direction} average memory "
                    f"wait than mild bandwidth ({severe_wait:.2f} vs "
                    f"{mild_wait:.2f})."
                )

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
            "Contention-aware rows with low hot-set-sized stale telemetry "
            f"produced {low_stale_wins}/{len(low_stale)} wins; high-stale "
            f"rows produced {high_stale_losses}/{len(high_stale)} losses."
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
                 plot_outputs: list[PlotOutput],
                 policy_summary: list[dict[str, Any]],
                 comparison_rows: list[dict[str, Any]],
                 report_mode: str,
                 dimensions: dict[str, dict[str, Any]],
                 best_run_summary: list[dict[str, Any]],
                 final_policy_table: list[dict[str, Any]]) -> None:
    """Write the Phase F Markdown report."""

    rel_plots_by_group: dict[str, list[Path]] = defaultdict(list)
    for output in plot_outputs:
        if output.path.suffix == ".svg":
            rel_plots_by_group[output.group].append(
                output.path.relative_to(path.parent)
            )
    observations = build_observations(
        comparison_rows,
        policy_summary,
        baseline,
        report_mode,
        dimensions,
    )
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
        f"- Report mode: `{report_mode}`",
        f"- Baseline policy: `{baseline}`",
        f"- Tie threshold: +/-{100.0 * tie_threshold:.1f}% mean-latency delta",
        "- Lower latency, lower memory wait, and fewer remote accesses are better.",
        "- Higher cache hit rate, estimated contention relief, and fairness are better.",
        "",
        "## Experiment Dimensions",
        "",
        "Swept dimensions:",
        "",
    ]
    swept_lines = [
        f"- `{column}`: {format_dimension_values(info['values'])}"
        for column, info in dimensions.items()
        if info.get("swept")
    ]
    fixed_lines = [
        f"- `{column}`: {format_dimension_values(info['values'])}"
        for column, info in dimensions.items()
        if not info.get("swept")
    ]
    lines.extend(swept_lines or ["- _No known swept dimensions detected._"])
    lines.extend(
        [
            "",
            "Fixed dimensions:",
            "",
        ]
    )
    lines.extend(fixed_lines or ["- _No known fixed dimensions detected._"])
    lines.extend(
        [
            "",
            "## Key Observations",
            "",
            *[f"- {observation}" for observation in observations],
            "",
        ]
    )
    if report_mode != "final_report":
        lines.extend(
            [
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
                    ("Avg Hot-Set Stale", "avg_stale_telemetry_rate"),
                    ("Avg Fairness", "avg_jain_inverse_latency_fairness"),
                ],
            ),
                "",
            ]
        )
    if report_mode == "policy_viability":
        lines.extend(
            [
                "## Best Run Examples",
                "",
                "These rows show each non-baseline policy's best individual "
                f"mean-latency improvement versus `{baseline}`. They are "
                "high-water examples from the matrix, not global averages.",
                "",
                markdown_table(
                    best_run_summary,
                    [
                        ("Target Policy", "target_policy"),
                        ("Improvement", "latency_improvement_pct"),
                        ("Churn", "hot_set_churn_fraction"),
                        ("Cache X", "cache_capacity_hotset_multiplier"),
                        ("RPE", "requests_per_node_per_epoch"),
                        ("Hot Set", "hot_set_size"),
                        ("Object Mode", "object_size_mode"),
                        ("Large Obj P", "large_object_probability"),
                        ("Channels", "memory_channel_count"),
                        ("Hot Channels", "hot_object_channel_count"),
                    ],
                ),
                "",
            ]
        )
    if report_mode == "final_report":
        lines.extend(
            [
                "## Final Policy Table",
                "",
                "Improvements and reductions are positive-is-better. "
                "Variation is the sample standard deviation of per-seed means "
                "after averaging matched conditions within each seed.",
                "",
                markdown_table(
                    final_policy_table,
                    [
                        ("Policy", "policy"),
                        ("Seeds", "seed_count"),
                        ("Runs", "comparable_run_count"),
                        ("Win Rate", "win_rate"),
                        ("Mean Latency Improve %", "mean_latency_improvement_pct"),
                        ("Seed SD", "seed_std_latency_improvement_pct"),
                        ("P99 Improve %", "mean_p99_improvement_pct"),
                        ("Memory-Wait Reduce %",
                         "mean_memory_wait_reduction_pct"),
                        ("Hit Rate", "mean_local_hit_rate"),
                    ],
                ),
                "",
            ]
        )
    lines.extend(
        [
            "## Plots",
            "",
        ]
    )
    for group, rel_plots in rel_plots_by_group.items():
        lines.extend([f"### {group}", ""])
        for plot in rel_plots:
            lines.extend([f"#### {plot.stem.replace('_', ' ').title()}", ""])
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
        ]
    )
    if report_mode == "policy_viability":
        lines.extend(
            [
                "For policy-viability runs, global-average plots summarize all "
                "regimes together. Churn-conditioned plots should be used to "
                "separate stable, predictive telemetry from high-churn regimes "
                "where hot-set-sized stale telemetry is expected to hurt "
                "contention-aware policies.",
                "",
                "Best-run example plots intentionally zoom in on each "
                "non-baseline policy's strongest individual condition. They "
                "are useful for explaining high points in the scatter plot, "
                "but should be paired with global and churn-conditioned views "
                "before making broad policy claims.",
                "",
            ]
        )
    if report_mode == "parameter_demo":
        lines.extend(
            [
                "For isolated parameter demonstrations, each report is designed "
                "to vary one primary knob while holding the rest of the regime "
                "fixed. These plots are best used to explain mechanism and "
                "scaling behavior; they should not replace the broader policy "
                "viability matrix as a complete ranking of policies.",
                "",
            ]
        )
    if report_mode == "weight_sensitivity":
        lines.extend(
            [
                "For contention-weight sensitivity runs, the plots vary one "
                "score weight at a time around the default smoothed policy. Use "
                "these results to identify which score components drive "
                "admission quality, latency, memory wait, and eviction regret; "
                "do not treat the sweep as proof that a tuned policy wins in "
                "all regimes.",
                "",
            ]
        )
    if report_mode == "final_report":
        lines.extend(
            [
                "The four final-report figures answer distinct questions: "
                "whether the simulator creates pressure, whether policy benefit "
                "grows with pressure, where the latest policies operate well, "
                "and which placement mechanisms explain their behavior.",
                "",
                "Do not treat best individual cells as independent evidence. "
                "Use the paired condition deltas and variation across workload "
                "seeds when stating quantitative conclusions.",
                "",
            ]
        )
    lines.extend(
        [
            "## Generated Files",
            "",
            "- `policy_comparison.csv`",
            "- `policy_summary.csv`",
            "- `condition_summary.csv`",
            *(
                [
                    "- `viability_best_runs.csv`",
                    "- `viability_best_run_policy_context.csv`",
                ]
                if report_mode == "policy_viability"
                else []
            ),
            *(
                ["- `final_policy_table.csv`"]
                if report_mode == "final_report"
                else []
            ),
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
    report_mode = detect_report_mode(aggregate_rows, args.report_mode)
    dimensions = dimension_summary(aggregate_rows)
    comparison_rows = build_policy_comparison(
        aggregate_rows,
        args.baseline,
        args.tie_threshold,
    )
    policy_summary = build_policy_summary(comparison_rows)
    condition_summary = build_condition_summary(comparison_rows)
    best_run_summary: list[dict[str, Any]] = []
    best_run_context: list[dict[str, Any]] = []
    if report_mode == "policy_viability":
        best_run_summary, best_run_context = build_best_run_examples(
            comparison_rows,
        )
    final_policy_table: list[dict[str, Any]] = []
    if report_mode == "final_report":
        final_policy_table = build_final_policy_table(comparison_rows)

    write_csv(output_dir / "policy_comparison.csv",
              COMPARISON_COLUMNS,
              comparison_rows)
    write_csv(output_dir / "policy_summary.csv",
              POLICY_SUMMARY_COLUMNS,
              policy_summary)
    write_csv(output_dir / "condition_summary.csv",
              CONDITION_COLUMNS,
              condition_summary)
    if report_mode == "policy_viability":
        write_csv(output_dir / "viability_best_runs.csv",
                  BEST_RUN_COLUMNS,
                  best_run_summary)
        write_csv(output_dir / "viability_best_run_policy_context.csv",
                  BEST_RUN_CONTEXT_COLUMNS,
                  best_run_context)
    if report_mode == "final_report":
        write_csv(
            output_dir / "final_policy_table.csv",
            FINAL_POLICY_TABLE_COLUMNS,
            final_policy_table,
        )
    plot_paths = generate_plots(
        plt,
        output_dir,
        formats,
        comparison_rows,
        policy_summary,
        report_mode,
        dimensions,
        best_run_context,
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
        report_mode,
        dimensions,
        best_run_summary,
        final_policy_table,
    )
    print(f"Wrote analysis report: {output_dir / 'report.md'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
