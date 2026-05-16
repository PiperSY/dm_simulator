#!/usr/bin/env python3

"""Generate and run matched dm_simulator experiment matrices."""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


POLICIES = (
    "always_remote",
    "lru",
    "hotness_only",
    "global_hottest_replication",
    "contention_aware",
)

AGGREGATE_COLUMNS = (
    "status",
    "error",
    "preset",
    "experiment_name",
    "policy",
    "seed",
    "node_count",
    "epoch_count",
    "requests_per_node_per_epoch",
    "hot_set_mode",
    "hot_set_churn_label",
    "hot_set_churn_fraction",
    "cross_node_overlap",
    "object_size_mode",
    "object_size_bytes",
    "object_size_small_bytes",
    "object_size_large_bytes",
    "large_object_probability",
    "cache_capacity_bytes",
    "memory_bandwidth_bytes_per_time",
    "completed_requests",
    "mean_latency",
    "median_latency",
    "p95_latency",
    "p99_latency",
    "local_hit_rate",
    "local_hits",
    "local_misses",
    "average_memory_wait",
    "max_memory_wait",
    "peak_memory_queue_depth",
    "total_remote_accesses",
    "total_queue_wait",
    "total_remote_service_time",
    "max_observed_queue_depth",
    "per_node_mean_latency_min",
    "per_node_mean_latency_max",
    "per_node_mean_latency_spread",
    "per_node_p99_latency_max",
    "per_node_hit_rate_min",
    "per_node_hit_rate_max",
    "policy_admitted",
    "policy_rejected",
    "config_path",
    "output_dir",
)


@dataclass(frozen=True)
class MatrixPreset:
    name: str
    memory_node_id: int
    memory_base_latency: int
    memory_bandwidth_bytes_per_time: int
    one_way_link_latency: int
    cache_capacity_bytes: int
    cache_hit_latency: int
    compute_node_ids: tuple[int, ...]
    object_count: int
    object_size_bytes: int
    object_size_small_bytes: int
    object_size_large_bytes: int
    large_object_probability: float
    requests_per_node_per_epoch_values: tuple[int, ...]
    epoch_count: int
    hot_set_size: int
    hot_access_probability: float
    hot_set_mode: str
    hot_set_churn_fractions: tuple[float, ...]
    cross_node_overlaps: tuple[str, ...]
    object_size_modes: tuple[str, ...]


@dataclass(frozen=True)
class MatrixRun:
    preset: MatrixPreset
    policy: str
    seed: int
    requests_per_node_per_epoch: int
    hot_set_churn_fraction: float
    cross_node_overlap: str
    object_size_mode: str
    run_name: str
    config_path: Path
    output_dir: Path


PRESETS = {
    "quick": MatrixPreset(
        name="quick",
        memory_node_id=99,
        memory_base_latency=25,
        memory_bandwidth_bytes_per_time=16,
        one_way_link_latency=6,
        cache_capacity_bytes=256,
        cache_hit_latency=1,
        compute_node_ids=(1, 2, 3, 4),
        object_count=128,
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probability=0.1,
        requests_per_node_per_epoch_values=(16,),
        epoch_count=3,
        hot_set_size=8,
        hot_access_probability=0.8,
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(1.0,),
        cross_node_overlaps=("medium",),
        object_size_modes=("fixed",),
    ),
    "phase8": MatrixPreset(
        name="phase8",
        memory_node_id=99,
        memory_base_latency=25,
        memory_bandwidth_bytes_per_time=16,
        one_way_link_latency=6,
        cache_capacity_bytes=256,
        cache_hit_latency=1,
        compute_node_ids=(1, 2, 3, 4, 5, 6, 7, 8),
        object_count=128,
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probability=0.1,
        requests_per_node_per_epoch_values=(512,),
        epoch_count=10,
        hot_set_size=8,
        hot_access_probability=0.8,
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(1.0,),
        cross_node_overlaps=("medium",),
        object_size_modes=("fixed",),
    ),
    "phase_b": MatrixPreset(
        name="phase_b",
        memory_node_id=99,
        memory_base_latency=25,
        memory_bandwidth_bytes_per_time=16,
        one_way_link_latency=6,
        cache_capacity_bytes=512,
        cache_hit_latency=1,
        compute_node_ids=(1, 2, 3, 4, 5, 6, 7, 8),
        object_count=256,
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probability=0.2,
        requests_per_node_per_epoch_values=(16, 64, 256, 1024),
        epoch_count=8,
        hot_set_size=8,
        hot_access_probability=0.8,
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.0, 0.25, 0.5, 0.75, 1.0),
        cross_node_overlaps=("low", "medium", "high"),
        object_size_modes=("fixed", "bimodal"),
    ),
}


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--preset",
        choices=sorted(PRESETS),
        default="quick",
        help="Built-in matrix preset to run.",
    )
    parser.add_argument(
        "--binary",
        type=Path,
        default=repo_root / "build" / "dm_simulator",
        help="Path to the dm_simulator binary.",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=None,
        help="Directory for generated configs, run outputs, and aggregate CSV.",
    )
    parser.add_argument(
        "--seeds",
        default="8888",
        help="Comma-separated workload seeds.",
    )
    parser.add_argument(
        "--policies",
        default=",".join(POLICIES),
        help="Comma-separated policy list.",
    )
    parser.add_argument(
        "--churn-fractions",
        default=None,
        help="Comma-separated hot-set churn fractions overriding the preset.",
    )
    parser.add_argument(
        "--epoch-lengths",
        default=None,
        help="Comma-separated requests-per-node-per-epoch values.",
    )
    parser.add_argument(
        "--overlaps",
        default=None,
        help="Comma-separated overlap values: low, medium, high.",
    )
    parser.add_argument(
        "--object-size-modes",
        default=None,
        help="Comma-separated object size modes: fixed, bimodal.",
    )
    parser.add_argument(
        "--expect-runs",
        type=int,
        default=None,
        help="Dry-run assertion for the expected generated run count.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Generate configs and aggregate schema without running simulator.",
    )
    parser.add_argument(
        "--keep-going",
        action="store_true",
        help="Continue remaining runs after a simulator failure.",
    )
    return parser.parse_args()


def parse_csv_list(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def parse_seeds(value: str) -> list[int]:
    seeds = []
    for item in parse_csv_list(value):
        try:
            seeds.append(int(item))
        except ValueError as error:
            raise SystemExit(f"Invalid seed '{item}': expected integer") from error
    if not seeds:
        raise SystemExit("At least one seed is required")
    return seeds


def parse_policies(value: str) -> list[str]:
    policies = parse_csv_list(value)
    if not policies:
        raise SystemExit("At least one policy is required")

    invalid = [policy for policy in policies if policy not in POLICIES]
    if invalid:
        raise SystemExit(
            "Invalid policy value(s): "
            + ", ".join(invalid)
            + ". Expected one of: "
            + ", ".join(POLICIES)
        )
    return policies


def parse_positive_ints(value: str) -> list[int]:
    values = []
    for item in parse_csv_list(value):
        try:
            parsed = int(item)
        except ValueError as error:
            raise SystemExit(f"Invalid integer '{item}'") from error
        if parsed <= 0:
            raise SystemExit(f"Invalid positive integer '{item}'")
        values.append(parsed)
    if not values:
        raise SystemExit("At least one integer value is required")
    return values


def parse_probabilities(value: str, label: str) -> list[float]:
    values = []
    for item in parse_csv_list(value):
        try:
            parsed = float(item)
        except ValueError as error:
            raise SystemExit(f"Invalid {label} '{item}'") from error
        if not math.isfinite(parsed) or parsed < 0.0 or parsed > 1.0:
            raise SystemExit(f"Invalid {label} '{item}': expected [0, 1]")
        values.append(parsed)
    if not values:
        raise SystemExit(f"At least one {label} value is required")
    return values


def parse_overlaps(value: str) -> list[str]:
    overlaps = parse_csv_list(value)
    invalid = [overlap for overlap in overlaps if overlap not in {"low", "medium", "high"}]
    if invalid:
        raise SystemExit(
            "Invalid overlap value(s): "
            + ", ".join(invalid)
            + ". Expected low, medium, or high"
        )
    if not overlaps:
        raise SystemExit("At least one overlap value is required")
    return overlaps


def parse_object_size_modes(value: str) -> list[str]:
    modes = parse_csv_list(value)
    invalid = [mode for mode in modes if mode not in {"fixed", "bimodal"}]
    if invalid:
        raise SystemExit(
            "Invalid object size mode(s): "
            + ", ".join(invalid)
            + ". Expected fixed or bimodal"
        )
    if not modes:
        raise SystemExit("At least one object size mode is required")
    return modes


def slug_float(value: float) -> str:
    return f"{value:g}".replace(".", "p")


def hot_set_churn_label(hot_set_mode: str, churn_fraction: float) -> str:
    if hot_set_mode == "static" or churn_fraction == 0.0:
        return "none"
    if churn_fraction == 1.0:
        return "full"
    return "partial"


def run_slug(preset: MatrixPreset,
             policy: str,
             seed: int,
             epoch_length: int,
             churn_fraction: float,
             overlap: str,
             object_size_mode: str) -> str:
    return (
        f"{preset.name}"
        f"__policy-{policy}"
        f"__seed-{seed}"
        f"__nodes-{len(preset.compute_node_ids)}"
        f"__epochs-{preset.epoch_count}"
        f"__rpe-{epoch_length}"
        f"__mode-{preset.hot_set_mode}"
        f"__churn-{slug_float(churn_fraction)}"
        f"__overlap-{overlap}"
        f"__size-{object_size_mode}"
        f"__cache-{preset.cache_capacity_bytes}"
        f"__bw-{preset.memory_bandwidth_bytes_per_time}"
    )


def build_runs(
    preset: MatrixPreset,
    policies: list[str],
    seeds: list[int],
    epoch_lengths: list[int],
    churn_fractions: list[float],
    overlaps: list[str],
    object_size_modes: list[str],
    results_dir: Path,
) -> list[MatrixRun]:
    config_dir = results_dir / "generated_configs"
    output_root = results_dir / "runs"
    runs = []
    for seed in seeds:
        for policy in policies:
            for epoch_length in epoch_lengths:
                for churn_fraction in churn_fractions:
                    for overlap in overlaps:
                        for object_size_mode in object_size_modes:
                            name = run_slug(preset,
                                            policy,
                                            seed,
                                            epoch_length,
                                            churn_fraction,
                                            overlap,
                                            object_size_mode)
                            runs.append(
                                MatrixRun(
                                    preset=preset,
                                    policy=policy,
                                    seed=seed,
                                    requests_per_node_per_epoch=epoch_length,
                                    hot_set_churn_fraction=churn_fraction,
                                    cross_node_overlap=overlap,
                                    object_size_mode=object_size_mode,
                                    run_name=name,
                                    config_path=config_dir / f"{name}.yaml",
                                    output_dir=output_root / name,
                                )
                            )
    return runs


def yaml_string(value: Any) -> str:
    escaped = str(value).replace("'", "''")
    return f"'{escaped}'"


def yaml_bool(value: bool) -> str:
    return "true" if value else "false"


def write_yaml_config(run: MatrixRun) -> None:
    preset = run.preset
    run.config_path.parent.mkdir(parents=True, exist_ok=True)
    node_ids = ", ".join(str(node_id) for node_id in preset.compute_node_ids)

    lines = [
        "experiment:",
        f"  name: {yaml_string(run.run_name)}",
        f"  output_dir: {yaml_string(run.output_dir)}",
        "",
        "memory:",
        f"  node_id: {preset.memory_node_id}",
        f"  base_latency: {preset.memory_base_latency}",
        (
            "  bandwidth_bytes_per_time: "
            f"{preset.memory_bandwidth_bytes_per_time}"
        ),
        "",
        "link:",
        f"  one_way_latency: {preset.one_way_link_latency}",
        "",
        "local_cache:",
        f"  capacity_bytes: {preset.cache_capacity_bytes}",
        f"  hit_latency: {preset.cache_hit_latency}",
        f"  policy: {run.policy}",
    ]

    if run.policy == "hotness_only":
        lines.extend(
            [
                "  hotness:",
                "    min_admit_count: 2",
                f"    reset_on_epoch_change: {yaml_bool(True)}",
            ]
        )
    elif run.policy == "contention_aware":
        lines.extend(
            [
                "  contention:",
                "    local_hotness_weight: 1.0",
                "    remote_access_weight: 1.0",
                "    distinct_requester_weight: 1.5",
                "    queue_wait_weight: 2.0",
                "    remote_service_time_weight: 1.0",
                "    size_penalty_weight: 0.5",
                "    min_admit_score: 1.0",
                "    local_hotness_threshold: 2",
                f"    reset_on_epoch_change: {yaml_bool(True)}",
            ]
        )

    lines.extend(
        [
            "",
            "workload:",
            f"  seed: {run.seed}",
            f"  compute_node_ids: [{node_ids}]",
            f"  object_count: {preset.object_count}",
            f"  object_size_bytes: {preset.object_size_bytes}",
            f"  hot_set_churn_fraction: {run.hot_set_churn_fraction:g}",
            f"  object_size_mode: {run.object_size_mode}",
            f"  object_size_small_bytes: {preset.object_size_small_bytes}",
            f"  object_size_large_bytes: {preset.object_size_large_bytes}",
            f"  large_object_probability: {preset.large_object_probability:g}",
            (
                "  requests_per_node_per_epoch: "
                f"{run.requests_per_node_per_epoch}"
            ),
            f"  epoch_count: {preset.epoch_count}",
            f"  hot_set_size: {preset.hot_set_size}",
            f"  hot_access_probability: {preset.hot_access_probability}",
            f"  hot_set_mode: {preset.hot_set_mode}",
            f"  cross_node_overlap: {run.cross_node_overlap}",
            "",
        ]
    )

    run.config_path.write_text("\n".join(lines), encoding="utf-8")


def base_row(run: MatrixRun, status: str, error: str = "") -> dict[str, Any]:
    preset = run.preset
    return {
        "status": status,
        "error": error,
        "preset": preset.name,
        "experiment_name": run.run_name,
        "policy": run.policy,
        "seed": run.seed,
        "node_count": len(preset.compute_node_ids),
        "epoch_count": preset.epoch_count,
        "requests_per_node_per_epoch": run.requests_per_node_per_epoch,
        "hot_set_mode": preset.hot_set_mode,
        "hot_set_churn_label": hot_set_churn_label(
            preset.hot_set_mode,
            run.hot_set_churn_fraction,
        ),
        "hot_set_churn_fraction": run.hot_set_churn_fraction,
        "cross_node_overlap": run.cross_node_overlap,
        "object_size_mode": run.object_size_mode,
        "object_size_bytes": preset.object_size_bytes,
        "object_size_small_bytes": preset.object_size_small_bytes,
        "object_size_large_bytes": preset.object_size_large_bytes,
        "large_object_probability": preset.large_object_probability,
        "cache_capacity_bytes": preset.cache_capacity_bytes,
        "memory_bandwidth_bytes_per_time": (
            preset.memory_bandwidth_bytes_per_time
        ),
        "config_path": run.config_path,
        "output_dir": run.output_dir,
    }


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as input_file:
        return json.load(input_file)


def number_or_blank(value: Any) -> Any:
    return "" if value is None else value


def spread(values: list[float]) -> Any:
    if not values:
        return ""
    return max(values) - min(values)


def aggregate_contention(output_dir: Path) -> dict[str, Any]:
    path = output_dir / "contention_by_object.csv"
    totals = {
        "total_remote_accesses": 0,
        "total_queue_wait": 0,
        "total_remote_service_time": 0,
        "max_observed_queue_depth": 0,
    }
    if not path.exists():
        return {key: "" for key in totals}

    with path.open("r", encoding="utf-8", newline="") as input_file:
        for row in csv.DictReader(input_file):
            totals["total_remote_accesses"] += int(row["remote_accesses"])
            totals["total_queue_wait"] += int(row["total_queue_wait"])
            totals["total_remote_service_time"] += int(
                row["total_remote_service_time"]
            )
            totals["max_observed_queue_depth"] = max(
                totals["max_observed_queue_depth"],
                int(row["max_observed_queue_depth"]),
            )
    return totals


def summarize_run(run: MatrixRun, status: str, error: str = "") -> dict[str, Any]:
    row = base_row(run, status, error)
    if status != "success":
        return row

    summary = read_json(run.output_dir / "summary.json")
    latency = summary.get("latency", {})
    cache = summary.get("cache", {})
    memory = summary.get("memory", {})
    policy = summary.get("policy", {})
    per_node = summary.get("per_node", [])
    per_node_mean_latencies = [
        float(node["mean_latency"]) for node in per_node if "mean_latency" in node
    ]
    per_node_p99_latencies = [
        float(node["p99_latency"]) for node in per_node if "p99_latency" in node
    ]
    per_node_hit_rates = [
        float(node["local_cache_hit_rate"])
        for node in per_node
        if "local_cache_hit_rate" in node
    ]

    row.update(
        {
            "completed_requests": summary.get("completed_requests", ""),
            "mean_latency": number_or_blank(latency.get("mean")),
            "median_latency": number_or_blank(latency.get("median")),
            "p95_latency": number_or_blank(latency.get("p95")),
            "p99_latency": number_or_blank(latency.get("p99")),
            "local_hit_rate": number_or_blank(cache.get("local_hit_rate")),
            "local_hits": number_or_blank(cache.get("local_hits")),
            "local_misses": number_or_blank(cache.get("local_misses")),
            "average_memory_wait": number_or_blank(memory.get("average_wait")),
            "max_memory_wait": number_or_blank(memory.get("max_wait")),
            "peak_memory_queue_depth": number_or_blank(
                memory.get("peak_queue_depth")
            ),
            "per_node_mean_latency_min": (
                min(per_node_mean_latencies) if per_node_mean_latencies else ""
            ),
            "per_node_mean_latency_max": (
                max(per_node_mean_latencies) if per_node_mean_latencies else ""
            ),
            "per_node_mean_latency_spread": spread(per_node_mean_latencies),
            "per_node_p99_latency_max": (
                max(per_node_p99_latencies) if per_node_p99_latencies else ""
            ),
            "per_node_hit_rate_min": (
                min(per_node_hit_rates) if per_node_hit_rates else ""
            ),
            "per_node_hit_rate_max": (
                max(per_node_hit_rates) if per_node_hit_rates else ""
            ),
            "policy_admitted": number_or_blank(policy.get("admitted")),
            "policy_rejected": number_or_blank(policy.get("rejected")),
        }
    )
    row.update(aggregate_contention(run.output_dir))
    return row


def write_aggregate_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=AGGREGATE_COLUMNS)
        writer.writeheader()
        for row in rows:
            writer.writerow({column: row.get(column, "") for column in AGGREGATE_COLUMNS})


def run_simulator(binary: Path, run: MatrixRun) -> None:
    command = [
        str(binary),
        "--config",
        str(run.config_path),
        "--output-dir",
        str(run.output_dir),
    ]
    print(f"Running {run.run_name}", flush=True)
    subprocess.run(command, check=True)


def verify_dry_run(
    runs: list[MatrixRun],
    rows: list[dict[str, Any]],
    selected_policies: list[str],
    aggregate_path: Path,
) -> None:
    run_names = [run.run_name for run in runs]
    if len(run_names) != len(set(run_names)):
        raise RuntimeError("Dry run generated duplicate run names")

    generated_policies = {row["policy"] for row in rows}
    if generated_policies != set(selected_policies):
        raise RuntimeError(
            "Dry run policy coverage mismatch: "
            f"expected {selected_policies}, got {sorted(generated_policies)}"
        )

    missing_configs = [run.config_path for run in runs if not run.config_path.exists()]
    if missing_configs:
        raise RuntimeError(
            "Dry run did not generate config files: "
            + ", ".join(str(path) for path in missing_configs)
        )

    for run in runs:
        config_text = run.config_path.read_text(encoding="utf-8")
        required_fields = (
            "hot_set_churn_fraction:",
            "object_size_mode:",
            "object_size_small_bytes:",
            "object_size_large_bytes:",
            "large_object_probability:",
        )
        for field in required_fields:
            if field not in config_text:
                raise RuntimeError(
                    f"Dry run config {run.config_path} is missing {field}"
                )

    with aggregate_path.open("r", encoding="utf-8", newline="") as input_file:
        reader = csv.reader(input_file)
        header = next(reader)
        body_rows = list(reader)

    if tuple(header) != AGGREGATE_COLUMNS:
        raise RuntimeError("Dry run aggregate header does not match schema")
    if len(body_rows) != len(runs):
        raise RuntimeError("Dry run aggregate row count does not match runs")


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    args = parse_args()
    preset = PRESETS[args.preset]
    seeds = parse_seeds(args.seeds)
    policies = parse_policies(args.policies)
    epoch_lengths = (
        parse_positive_ints(args.epoch_lengths)
        if args.epoch_lengths is not None
        else list(preset.requests_per_node_per_epoch_values)
    )
    churn_fractions = (
        parse_probabilities(args.churn_fractions, "churn fraction")
        if args.churn_fractions is not None
        else list(preset.hot_set_churn_fractions)
    )
    overlaps = (
        parse_overlaps(args.overlaps)
        if args.overlaps is not None
        else list(preset.cross_node_overlaps)
    )
    object_size_modes = (
        parse_object_size_modes(args.object_size_modes)
        if args.object_size_modes is not None
        else list(preset.object_size_modes)
    )
    results_dir = (
        args.results_dir
        if args.results_dir is not None
        else repo_root / "results" / f"matrix_{preset.name}"
    )
    results_dir.mkdir(parents=True, exist_ok=True)

    runs = build_runs(preset,
                      policies,
                      seeds,
                      epoch_lengths,
                      churn_fractions,
                      overlaps,
                      object_size_modes,
                      results_dir)
    if args.expect_runs is not None and len(runs) != args.expect_runs:
        raise SystemExit(
            f"Expected {args.expect_runs} generated runs, got {len(runs)}"
        )
    rows: list[dict[str, Any]] = []
    for run in runs:
        write_yaml_config(run)
        if args.dry_run:
            rows.append(summarize_run(run, "dry_run"))
            continue

        try:
            run_simulator(args.binary, run)
        except subprocess.CalledProcessError as error:
            rows.append(summarize_run(run, "failed", str(error)))
            if not args.keep_going:
                aggregate_path = results_dir / "aggregate_summary.csv"
                write_aggregate_csv(aggregate_path, rows)
                raise
        else:
            rows.append(summarize_run(run, "success"))

    aggregate_path = results_dir / "aggregate_summary.csv"
    write_aggregate_csv(aggregate_path, rows)

    if args.dry_run:
        verify_dry_run(runs, rows, policies, aggregate_path)
        print(f"Dry run generated {len(runs)} configs", flush=True)
    else:
        print(f"Wrote aggregate summary: {aggregate_path}", flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
