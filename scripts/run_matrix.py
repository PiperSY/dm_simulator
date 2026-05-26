#!/usr/bin/env python3

"""Generate and run matched dm_simulator experiment matrices.

The script builds families of comparable YAML configs, runs each config through
the C++ simulator, and folds the resulting JSON/CSV outputs into one aggregate
CSV. Presets define the default experiment space, while CLI flags narrow or
override selected dimensions for quick probes.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


BASE_POLICIES = (
    "always_remote",
    "lru",
    "hotness_only",
    "global_hottest_replication",
    "contention_aware",
)

CONTENTION_VARIANT_POLICIES = (
    "contention_aware_v1",
    "contention_aware_smoothed",
    "contention_aware_reuse_gated",
    "contention_aware_hysteresis",
)

PHASE_E_DEFAULT_POLICIES = (
    "lru",
    "hotness_only",
    "hotness_only_cumulative",
    "global_hottest_replication",
    *CONTENTION_VARIANT_POLICIES,
)

POLICIES = (
    *BASE_POLICIES,
    "hotness_only_cumulative",
    *CONTENTION_VARIANT_POLICIES,
)

# Named bandwidth levels map to simulator units in write_yaml_config().
MEMORY_BANDWIDTH_BYTES_PER_TIME_BY_LEVEL = {
    "mild": 64,
    "moderate": 16,
    "severe": 4,
}

MEMORY_BASE_LATENCY_BY_LEVEL = {
    "low": 10,
    "medium": 25,
    "high": 75,
}

LINK_LATENCY_BY_LEVEL = {
    "low": 2,
    "medium": 6,
    "high": 20,
}

AGGREGATE_COLUMNS = (
    "status",
    "error",
    "preset",
    "experiment_name",
    "policy",
    "seed",
    "node_count",
    "compute_node_count",
    "epoch_count",
    "requests_per_node_per_epoch",
    "hot_set_mode",
    "hot_set_churn_label",
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
    "admission_yield",
    "reuse_after_admit_rate",
    "stale_telemetry_rate",
    "average_top_object_overlap",
    "estimated_avoided_remote_accesses",
    "estimated_avoided_queue_wait",
    "estimated_avoided_remote_service_time",
    "eviction_regret_count",
    "remote_eviction_regret_count",
    "jain_inverse_latency_fairness",
    "config_path",
    "output_dir",
)


@dataclass(frozen=True)
class MatrixPreset:
    """Default experiment dimensions used to generate a matrix.

    Attributes:
        name: Short preset identifier used in run names and default paths.
        memory_node_id: Node ID assigned to the single shared memory node.
        cache_hit_latency: Local-cache hit latency written into generated YAML.
        compute_node_ids: Maximum ordered pool of compute-node IDs available to
            this preset. A run with N nodes uses the first N IDs.
        compute_node_count_values: Default node-count sweep values.
        object_count_values: Object-universe size sweep values.
        object_size_bytes: Fixed object size and representative size used for
            cache-capacity calculations.
        object_size_small_bytes: Small-object size for bimodal workloads.
        object_size_large_bytes: Large-object size for bimodal workloads.
        large_object_probability: Probability that an object is large in
            bimodal mode.
        cache_capacity_hotset_multipliers: Cache capacities expressed as
            multiples of one hot set's representative byte size.
        memory_bandwidth_levels: Named bandwidth sweep values. Names map
            through MEMORY_BANDWIDTH_BYTES_PER_TIME_BY_LEVEL.
        memory_base_latency_levels: Named memory base-latency sweep values.
        link_latency_levels: Named one-way link-latency sweep values.
        requests_per_node_per_epoch_values: Epoch-length sweep, measured as
            requests issued by each compute node per epoch.
        epoch_count_values: Synthetic workload epoch-count sweep values.
        hot_set_size_values: Hot objects per node per epoch sweep values.
        hot_access_probabilities: Probability sweep for targeting a node's
            current hot set.
        hot_set_mode: Workload hot-set mode, usually "static" or
            "epoch_shift".
        hot_set_churn_fractions: Fraction of hot objects replaced at epoch
            boundaries when hot_set_mode is "epoch_shift".
        cross_node_overlaps: Default cross-node hot-set overlap sweep values:
            "low", "medium", or "high".
        object_size_modes: Object-size mode sweep values: "fixed" or
            "bimodal".
    """

    name: str
    memory_node_id: int
    cache_hit_latency: int
    compute_node_ids: tuple[int, ...]
    compute_node_count_values: tuple[int, ...]
    object_count_values: tuple[int, ...]
    object_size_bytes: int
    object_size_small_bytes: int
    object_size_large_bytes: int
    large_object_probability: float
    cache_capacity_hotset_multipliers: tuple[float, ...]
    memory_bandwidth_levels: tuple[str, ...]
    memory_base_latency_levels: tuple[str, ...]
    link_latency_levels: tuple[str, ...]
    requests_per_node_per_epoch_values: tuple[int, ...]
    epoch_count_values: tuple[int, ...]
    hot_set_size_values: tuple[int, ...]
    hot_access_probabilities: tuple[float, ...]
    hot_set_mode: str
    hot_set_churn_fractions: tuple[float, ...]
    cross_node_overlaps: tuple[str, ...]
    object_size_modes: tuple[str, ...]


@dataclass(frozen=True)
class MatrixRun:
    """One concrete generated experiment run from a preset's sweep space.

    Attributes:
        preset: The MatrixPreset this run was generated from.
        policy: Cache policy name written to local_cache.policy.
        seed: Synthetic workload RNG seed.
        compute_node_count: Number of compute nodes active in this run.
        object_count: Size of the synthetic object universe.
        epoch_count: Number of synthetic workload epochs.
        requests_per_node_per_epoch: Workload epoch length for each node.
        hot_set_size: Number of hot objects per node per epoch.
        hot_access_probability: Probability that a generated request targets
            that node's current hot set.
        hot_set_churn_fraction: Fraction of hot-set entries replaced per epoch.
        cross_node_overlap: Hot-set overlap level: "low", "medium", or "high".
        object_size_mode: Object-size generation mode: "fixed" or "bimodal".
        cache_capacity_hotset_multiplier: Cache size as a multiple of one hot
            set's representative byte footprint.
        memory_bandwidth_level: Named memory bandwidth setting.
        memory_base_latency_level: Named memory base-latency setting.
        link_latency_level: Named one-way link-latency setting.
        run_name: Stable run identifier used for file and directory names.
        config_path: Generated YAML config path for this run.
        output_dir: Directory where the simulator writes this run's outputs.
    """

    preset: MatrixPreset
    policy: str
    seed: int
    compute_node_count: int
    object_count: int
    epoch_count: int
    requests_per_node_per_epoch: int
    hot_set_size: int
    hot_access_probability: float
    hot_set_churn_fraction: float
    cross_node_overlap: str
    object_size_mode: str
    cache_capacity_hotset_multiplier: float
    memory_bandwidth_level: str
    memory_base_latency_level: str
    link_latency_level: str
    run_name: str
    config_path: Path
    output_dir: Path


PRESETS = {
    # Tiny smoke preset: safe default for checking script/build plumbing.
    "quick": MatrixPreset(
        name="quick",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=(1, 2, 3, 4),
        compute_node_count_values=(4,),
        object_count_values=(128,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probability=0.1,
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("moderate",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(16,),
        epoch_count_values=(3,),
        hot_set_size_values=(8,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(1.0,),
        cross_node_overlaps=("medium",),
        object_size_modes=("fixed",),
    ),
    # Recreates the original Phase 8 style policy comparison shape.
    "phase8": MatrixPreset(
        name="phase8",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=(1, 2, 3, 4, 5, 6, 7, 8),
        compute_node_count_values=(8,),
        object_count_values=(128,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probability=0.1,
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("moderate",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(512,),
        epoch_count_values=(10,),
        hot_set_size_values=(8,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(1.0,),
        cross_node_overlaps=("medium",),
        object_size_modes=("fixed",),
    ),
    # Workload realism sweep: temporal churn, overlap, and object-size modes.
    "phase_b": MatrixPreset(
        name="phase_b",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=(1, 2, 3, 4, 5, 6, 7, 8),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probability=0.2,
        cache_capacity_hotset_multipliers=(1.0,),
        memory_bandwidth_levels=("moderate",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(16, 64, 256, 1024),
        epoch_count_values=(8,),
        hot_set_size_values=(8,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.0, 0.25, 0.5, 0.75, 1.0),
        cross_node_overlaps=("low", "medium", "high"),
        object_size_modes=("fixed", "bimodal"),
    ),
    # Architecture/bottleneck sweep: node count, cache pressure, and memory BW.
    "phase_c": MatrixPreset(
        name="phase_c",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(4, 8),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probability=0.2,
        cache_capacity_hotset_multipliers=(0.5, 1.0, 2.0),
        memory_bandwidth_levels=("moderate", "severe"),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(256,),
        epoch_count_values=(8,),
        hot_set_size_values=(8,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.5,),
        cross_node_overlaps=("medium",),
        object_size_modes=("fixed",),
    ),
    # Phase E keeps architecture/workload fixed enough to compare policy
    # variants, while still probing temporal stability through epoch length
    # and partial hot-set churn.
    "phase_e": MatrixPreset(
        name="phase_e",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probability=0.2,
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(16, 64),
        epoch_count_values=(8,),
        hot_set_size_values=(8,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.25, 0.5),
        cross_node_overlaps=("medium",),
        object_size_modes=("fixed",),
    ),
}


def parse_args() -> argparse.Namespace:
    """Parse CLI options that select a preset and optionally override sweeps.

    User-facing parameters:
        --preset: Built-in MatrixPreset to start from.
        --binary: Path to the compiled dm_simulator executable.
        --results-dir: Root directory for generated configs, run outputs, and
            aggregate_summary.csv.
        --seeds: Comma-separated workload seeds.
        --policies: Comma-separated cache policy names to compare.
        --node-counts: Comma-separated compute-node counts.
        --object-counts: Comma-separated object-universe sizes.
        --hot-set-sizes: Comma-separated hot-set sizes.
        --hot-access-probabilities: Comma-separated hot-access probabilities.
        --epoch-counts: Comma-separated epoch counts.
        --cache-hotset-multipliers: Comma-separated cache capacity multipliers.
        --memory-bandwidth-levels: Comma-separated named bandwidth levels.
        --memory-base-latency-levels: Comma-separated named base latencies.
        --link-latency-levels: Comma-separated named link latencies.
        --churn-fractions: Comma-separated hot-set churn fractions in [0, 1].
        --epoch-lengths: Comma-separated requests-per-node-per-epoch values.
        --overlaps: Comma-separated cross-node overlap levels.
        --object-size-modes: Comma-separated object-size generation modes.
        --expect-runs: Optional dry-run assertion for generated run count.
        --dry-run: Generate configs and aggregate rows without simulation.
        --keep-going: Continue the matrix after a simulator failure.
    """

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
        default=None,
        help=(
            "Comma-separated policy list. Defaults to the base policies, "
            "except phase_e which uses the Phase E variant comparison set."
        ),
    )
    parser.add_argument(
        "--node-counts",
        default=None,
        help="Comma-separated compute-node counts overriding the preset.",
    )
    parser.add_argument(
        "--object-counts",
        default=None,
        help="Comma-separated synthetic object counts overriding the preset.",
    )
    parser.add_argument(
        "--hot-set-sizes",
        default=None,
        help="Comma-separated hot-set sizes overriding the preset.",
    )
    parser.add_argument(
        "--hot-access-probabilities",
        default=None,
        help="Comma-separated hot-access probabilities in [0, 1].",
    )
    parser.add_argument(
        "--epoch-counts",
        default=None,
        help="Comma-separated epoch counts overriding the preset.",
    )
    parser.add_argument(
        "--cache-hotset-multipliers",
        default=None,
        help="Comma-separated cache capacities as multiples of hot-set bytes.",
    )
    parser.add_argument(
        "--memory-bandwidth-levels",
        default=None,
        help="Comma-separated memory bandwidth levels: mild, moderate, severe.",
    )
    parser.add_argument(
        "--memory-base-latency-levels",
        default=None,
        help="Comma-separated memory base-latency levels: low, medium, high.",
    )
    parser.add_argument(
        "--link-latency-levels",
        default=None,
        help="Comma-separated link-latency levels: low, medium, high.",
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
    """Split a comma-separated CLI value while ignoring empty items."""

    return [item.strip() for item in value.split(",") if item.strip()]


def parse_seeds(value: str) -> list[int]:
    """Parse the workload seed list used to generate independent trials."""

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
    """Parse and validate selected cache policies."""

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
    """Parse comma-separated positive integers for sweep dimensions."""

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


def parse_positive_floats(value: str, label: str) -> list[float]:
    """Parse comma-separated positive finite floats for sweep dimensions."""

    values = []
    for item in parse_csv_list(value):
        try:
            parsed = float(item)
        except ValueError as error:
            raise SystemExit(f"Invalid {label} '{item}'") from error
        if not math.isfinite(parsed) or parsed <= 0.0:
            raise SystemExit(f"Invalid {label} '{item}': expected > 0")
        values.append(parsed)
    if not values:
        raise SystemExit(f"At least one {label} value is required")
    return values


def parse_probabilities(value: str, label: str) -> list[float]:
    """Parse comma-separated probabilities constrained to [0, 1]."""

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


def parse_levels(value: str,
                 allowed_levels: dict[str, int],
                 label: str) -> list[str]:
    """Parse named level values such as mild/moderate/severe."""

    levels = parse_csv_list(value)
    invalid = [level for level in levels if level not in allowed_levels]
    if invalid:
        raise SystemExit(
            f"Invalid {label} value(s): "
            + ", ".join(invalid)
            + ". Expected one of: "
            + ", ".join(allowed_levels)
        )
    if not levels:
        raise SystemExit(f"At least one {label} value is required")
    return levels


def parse_overlaps(value: str) -> list[str]:
    """Parse cross-node hot-set overlap names."""

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
    """Parse synthetic workload object-size modes."""

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
    """Convert a float into a path-safe token for run directory names."""

    return f"{value:g}".replace(".", "p")


def hot_set_churn_label(hot_set_mode: str, churn_fraction: float) -> str:
    """Return a compact human label for churn behavior."""

    if hot_set_mode == "static" or churn_fraction == 0.0:
        return "none"
    if churn_fraction == 1.0:
        return "full"
    return "partial"


def node_ids_for_run(run: MatrixRun) -> tuple[int, ...]:
    """Select the compute-node IDs used by this run."""

    if run.compute_node_count > len(run.preset.compute_node_ids):
        raise SystemExit(
            f"Preset {run.preset.name} only defines "
            f"{len(run.preset.compute_node_ids)} compute node IDs"
        )
    return run.preset.compute_node_ids[:run.compute_node_count]


def memory_bandwidth_bytes_per_time(run: MatrixRun) -> int:
    """Resolve a run's named bandwidth level to simulator units."""

    return MEMORY_BANDWIDTH_BYTES_PER_TIME_BY_LEVEL[run.memory_bandwidth_level]


def memory_base_latency(run: MatrixRun) -> int:
    """Resolve a run's named memory base-latency level."""

    return MEMORY_BASE_LATENCY_BY_LEVEL[run.memory_base_latency_level]


def one_way_link_latency(run: MatrixRun) -> int:
    """Resolve a run's named one-way link-latency level."""

    return LINK_LATENCY_BY_LEVEL[run.link_latency_level]


def cache_capacity_bytes(run: MatrixRun) -> int:
    """Compute cache capacity from hot-set size and multiplier."""

    representative_object_size = run.preset.object_size_bytes
    hot_set_bytes = run.hot_set_size * representative_object_size
    return max(
        1,
        int(round(hot_set_bytes * run.cache_capacity_hotset_multiplier)),
    )


def run_slug(preset: MatrixPreset,
             policy: str,
             seed: int,
             compute_node_count: int,
             object_count: int,
             epoch_count: int,
             epoch_length: int,
             hot_set_size: int,
             hot_access_probability: float,
             churn_fraction: float,
             overlap: str,
             object_size_mode: str,
             cache_multiplier: float,
             memory_bandwidth_level: str,
             memory_base_latency_level: str,
             link_latency_level: str) -> str:
    """Build a deterministic run name encoding all matrix dimensions."""

    return (
        f"{preset.name}"
        f"__policy-{policy}"
        f"__seed-{seed}"
        f"__nodes-{compute_node_count}"
        f"__objects-{object_count}"
        f"__epochs-{epoch_count}"
        f"__rpe-{epoch_length}"
        f"__hotset-{hot_set_size}"
        f"__hotp-{slug_float(hot_access_probability)}"
        f"__mode-{preset.hot_set_mode}"
        f"__churn-{slug_float(churn_fraction)}"
        f"__overlap-{overlap}"
        f"__size-{object_size_mode}"
        f"__cachex-{slug_float(cache_multiplier)}"
        f"__bw-{memory_bandwidth_level}"
        f"__base-{memory_base_latency_level}"
        f"__link-{link_latency_level}"
    )


def build_runs(
    preset: MatrixPreset,
    policies: list[str],
    seeds: list[int],
    node_counts: list[int],
    object_counts: list[int],
    epoch_counts: list[int],
    epoch_lengths: list[int],
    hot_set_sizes: list[int],
    hot_access_probabilities: list[float],
    churn_fractions: list[float],
    overlaps: list[str],
    object_size_modes: list[str],
    cache_multipliers: list[float],
    memory_bandwidth_levels: list[str],
    memory_base_latency_levels: list[str],
    link_latency_levels: list[str],
    results_dir: Path,
) -> list[MatrixRun]:
    """Expand selected sweep dimensions into concrete MatrixRun records."""

    config_dir = results_dir / "generated_configs"
    output_root = results_dir / "runs"
    runs = []
    # This intentionally forms the full Cartesian product of selected
    # dimensions so every policy is evaluated against matched workloads and
    # architecture settings.
    for seed in seeds:
        for policy in policies:
            for node_count in node_counts:
                for object_count in object_counts:
                    for epoch_count in epoch_counts:
                        for epoch_length in epoch_lengths:
                            for hot_set_size in hot_set_sizes:
                                for hot_access_probability in hot_access_probabilities:
                                    for churn_fraction in churn_fractions:
                                        for overlap in overlaps:
                                            for object_size_mode in object_size_modes:
                                                for cache_multiplier in cache_multipliers:
                                                    for bandwidth_level in memory_bandwidth_levels:
                                                        for base_level in memory_base_latency_levels:
                                                            for link_level in link_latency_levels:
                                                                name = run_slug(
                                                                    preset,
                                                                    policy,
                                                                    seed,
                                                                    node_count,
                                                                    object_count,
                                                                    epoch_count,
                                                                    epoch_length,
                                                                    hot_set_size,
                                                                    hot_access_probability,
                                                                    churn_fraction,
                                                                    overlap,
                                                                    object_size_mode,
                                                                    cache_multiplier,
                                                                    bandwidth_level,
                                                                    base_level,
                                                                    link_level,
                                                                )
                                                                runs.append(
                                                                    MatrixRun(
                                                                        preset=preset,
                                                                        policy=policy,
                                                                        seed=seed,
                                                                        compute_node_count=node_count,
                                                                        object_count=object_count,
                                                                        epoch_count=epoch_count,
                                                                        requests_per_node_per_epoch=epoch_length,
                                                                        hot_set_size=hot_set_size,
                                                                        hot_access_probability=hot_access_probability,
                                                                        hot_set_churn_fraction=churn_fraction,
                                                                        cross_node_overlap=overlap,
                                                                        object_size_mode=object_size_mode,
                                                                        cache_capacity_hotset_multiplier=cache_multiplier,
                                                                        memory_bandwidth_level=bandwidth_level,
                                                                        memory_base_latency_level=base_level,
                                                                        link_latency_level=link_level,
                                                                        run_name=name,
                                                                        config_path=config_dir / f"{name}.yaml",
                                                                        output_dir=output_root / name,
                                                                    )
                                                                )
    return runs


def yaml_string(value: Any) -> str:
    """Quote a scalar as a simple YAML single-quoted string."""

    escaped = str(value).replace("'", "''")
    return f"'{escaped}'"


def yaml_bool(value: bool) -> str:
    """Render a Python bool as YAML's lowercase boolean spelling."""

    return "true" if value else "false"


def yaml_policy_name(policy: str) -> str:
    """Translate a matrix policy label into the simulator's YAML policy name."""

    if policy == "hotness_only_cumulative":
        return "hotness_only"
    if policy.startswith("contention_aware_"):
        return "contention_aware"
    return policy


def contention_variant_for_policy(policy: str) -> str | None:
    """Return the contention variant encoded by a matrix policy label."""

    # Plain contention_aware intentionally remains the legacy v1 baseline.
    variants = {
        "contention_aware": "v1",
        "contention_aware_v1": "v1",
        "contention_aware_smoothed": "smoothed",
        "contention_aware_reuse_gated": "reuse_gated",
        "contention_aware_hysteresis": "hysteresis",
    }
    return variants.get(policy)


def hotness_reset_on_epoch_change(policy: str) -> bool:
    """Return whether the generated hotness policy should reset per epoch."""

    return policy != "hotness_only_cumulative"


def write_yaml_config(run: MatrixRun) -> None:
    """Write the simulator YAML config for one matrix run."""

    preset = run.preset
    run.config_path.parent.mkdir(parents=True, exist_ok=True)
    node_ids = ", ".join(str(node_id) for node_id in node_ids_for_run(run))
    yaml_policy = yaml_policy_name(run.policy)
    contention_variant = contention_variant_for_policy(run.policy)

    lines = [
        "experiment:",
        f"  name: {yaml_string(run.run_name)}",
        f"  output_dir: {yaml_string(run.output_dir)}",
        "",
        "memory:",
        f"  node_id: {preset.memory_node_id}",
        f"  base_latency: {memory_base_latency(run)}",
        f"  bandwidth_bytes_per_time: {memory_bandwidth_bytes_per_time(run)}",
        "",
        "link:",
        f"  one_way_latency: {one_way_link_latency(run)}",
        "",
        "local_cache:",
        f"  capacity_bytes: {cache_capacity_bytes(run)}",
        f"  hit_latency: {preset.cache_hit_latency}",
        f"  policy: {yaml_policy}",
    ]

    # Most policies need only the shared local_cache fields. These blocks add
    # policy-specific knobs while keeping the workload/architecture matched.
    if yaml_policy == "hotness_only":
        lines.extend(
            [
                "  hotness:",
                "    min_admit_count: 2",
                "    # Phase E uses hotness_only_cumulative to test whether",
                "    # carrying local demand across epochs is a stronger baseline.",
                "    reset_on_epoch_change: "
                f"{yaml_bool(hotness_reset_on_epoch_change(run.policy))}",
            ]
        )
    elif yaml_policy == "contention_aware":
        lines.extend(
            [
                "  contention:",
                f"    variant: {contention_variant}",
                "    local_hotness_weight: 1.0",
                "    remote_access_weight: 1.0",
                "    distinct_requester_weight: 1.5",
                "    queue_wait_weight: 2.0",
                "    remote_service_time_weight: 1.0",
                "    size_penalty_weight: 0.5",
                "    min_admit_score: 1.0",
                "    local_hotness_threshold: 2",
                f"    reset_on_epoch_change: {yaml_bool(False)}",
            ]
        )
        if contention_variant == "smoothed":
            lines.extend(
                [
                    "    # Smoothed telemetry blends the last few prior epochs",
                    "    # so one unlucky epoch does not dominate admissions.",
                    "    telemetry_history_epochs: 3",
                    "    telemetry_decay: 0.5",
                ]
            )
        elif contention_variant == "reuse_gated":
            lines.extend(
                [
                    "    # Reuse gating requires local evidence before a",
                    "    # globally contended object can occupy a private cache.",
                    "    local_reuse_gate_threshold: 2",
                ]
            )
        elif contention_variant == "hysteresis":
            lines.extend(
                [
                    "    # Hysteresis avoids evicting a resident for a nearly",
                    "    # equal-scored incoming object.",
                    "    eviction_score_margin: 0.25",
                ]
            )

    lines.extend(
        [
            "",
            "workload:",
            f"  seed: {run.seed}",
            f"  compute_node_ids: [{node_ids}]",
            f"  object_count: {run.object_count}",
            f"  object_size_bytes: {preset.object_size_bytes}",
            f"  hot_set_churn_fraction: {run.hot_set_churn_fraction:g}",
            f"  object_size_mode: {run.object_size_mode}",
            f"  object_size_small_bytes: {preset.object_size_small_bytes}",
            f"  object_size_large_bytes: {preset.object_size_large_bytes}",
            f"  large_object_probability: {preset.large_object_probability:g}",
            f"  requests_per_node_per_epoch: {run.requests_per_node_per_epoch}",
            f"  epoch_count: {run.epoch_count}",
            f"  hot_set_size: {run.hot_set_size}",
            f"  hot_access_probability: {run.hot_access_probability:g}",
            f"  hot_set_mode: {preset.hot_set_mode}",
            f"  cross_node_overlap: {run.cross_node_overlap}",
            "",
        ]
    )

    run.config_path.write_text("\n".join(lines), encoding="utf-8")


def base_row(run: MatrixRun, status: str, error: str = "") -> dict[str, Any]:
    """Create aggregate CSV columns that are known before a run executes."""

    preset = run.preset
    return {
        "status": status,
        "error": error,
        "preset": preset.name,
        "experiment_name": run.run_name,
        "policy": run.policy,
        "seed": run.seed,
        "node_count": run.compute_node_count,
        "compute_node_count": run.compute_node_count,
        "epoch_count": run.epoch_count,
        "requests_per_node_per_epoch": run.requests_per_node_per_epoch,
        "hot_set_mode": preset.hot_set_mode,
        "hot_set_churn_label": hot_set_churn_label(
            preset.hot_set_mode,
            run.hot_set_churn_fraction,
        ),
        "hot_set_churn_fraction": run.hot_set_churn_fraction,
        "cross_node_overlap": run.cross_node_overlap,
        "object_count": run.object_count,
        "hot_set_size": run.hot_set_size,
        "hot_access_probability": run.hot_access_probability,
        "object_size_mode": run.object_size_mode,
        "object_size_bytes": preset.object_size_bytes,
        "object_size_small_bytes": preset.object_size_small_bytes,
        "object_size_large_bytes": preset.object_size_large_bytes,
        "large_object_probability": preset.large_object_probability,
        "cache_capacity_hotset_multiplier": (
            run.cache_capacity_hotset_multiplier
        ),
        "cache_capacity_bytes": cache_capacity_bytes(run),
        "memory_bandwidth_level": run.memory_bandwidth_level,
        "memory_bandwidth_bytes_per_time": memory_bandwidth_bytes_per_time(run),
        "memory_base_latency_level": run.memory_base_latency_level,
        "memory_base_latency": memory_base_latency(run),
        "link_latency_level": run.link_latency_level,
        "one_way_link_latency": one_way_link_latency(run),
        "config_path": run.config_path,
        "output_dir": run.output_dir,
    }


def read_json(path: Path) -> dict[str, Any]:
    """Read a JSON object from a simulator output file."""

    with path.open("r", encoding="utf-8") as input_file:
        return json.load(input_file)


def number_or_blank(value: Any) -> Any:
    """Keep numeric values but render missing JSON fields as CSV blanks."""

    return "" if value is None else value


def spread(values: list[float]) -> Any:
    """Return max-min spread, or a blank for empty inputs."""

    if not values:
        return ""
    return max(values) - min(values)


def aggregate_contention(output_dir: Path) -> dict[str, Any]:
    """Summarize object-level contention rows into run-level totals."""

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
    """Build one aggregate CSV row from simulator outputs."""

    row = base_row(run, status, error)
    if status != "success":
        return row

    summary = read_json(run.output_dir / "summary.json")
    latency = summary.get("latency", {})
    cache = summary.get("cache", {})
    memory = summary.get("memory", {})
    policy = summary.get("policy", {})
    viability = summary.get("viability", {})
    per_node = summary.get("per_node", [])
    # Fairness-facing columns are derived from the per-node section so a single
    # aggregate CSV can flag node imbalance without opening per_node.csv.
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
            "admission_yield": number_or_blank(
                viability.get("admission_yield")
            ),
            "reuse_after_admit_rate": number_or_blank(
                viability.get("reuse_after_admit_rate")
            ),
            "stale_telemetry_rate": number_or_blank(
                viability.get("stale_telemetry_rate")
            ),
            "average_top_object_overlap": number_or_blank(
                viability.get("average_top_object_overlap")
            ),
            "estimated_avoided_remote_accesses": number_or_blank(
                viability.get("estimated_avoided_remote_accesses")
            ),
            "estimated_avoided_queue_wait": number_or_blank(
                viability.get("estimated_avoided_queue_wait")
            ),
            "estimated_avoided_remote_service_time": number_or_blank(
                viability.get("estimated_avoided_remote_service_time")
            ),
            "eviction_regret_count": number_or_blank(
                viability.get("eviction_regret_count")
            ),
            "remote_eviction_regret_count": number_or_blank(
                viability.get("remote_eviction_regret_count")
            ),
            "jain_inverse_latency_fairness": number_or_blank(
                viability.get("jain_inverse_latency_fairness")
            ),
        }
    )
    row.update(aggregate_contention(run.output_dir))
    return row


def write_aggregate_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    """Write the matrix-level aggregate CSV with a stable column order."""

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=AGGREGATE_COLUMNS)
        writer.writeheader()
        for row in rows:
            writer.writerow({column: row.get(column, "") for column in AGGREGATE_COLUMNS})


def run_simulator(binary: Path, run: MatrixRun) -> None:
    """Launch the C++ simulator for one generated config."""

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
    """Sanity-check dry-run output without launching the simulator."""

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

    # Dry-run tests only have generated YAML and synthetic aggregate rows, so
    # verify the generated configs contain the key fields needed by later runs.
    for run in runs:
        config_text = run.config_path.read_text(encoding="utf-8")
        required_fields = (
            f"compute_node_ids: [{', '.join(str(node_id) for node_id in node_ids_for_run(run))}]",
            f"base_latency: {memory_base_latency(run)}",
            f"bandwidth_bytes_per_time: {memory_bandwidth_bytes_per_time(run)}",
            f"one_way_latency: {one_way_link_latency(run)}",
            f"capacity_bytes: {cache_capacity_bytes(run)}",
            f"object_count: {run.object_count}",
            f"epoch_count: {run.epoch_count}",
            f"hot_set_size: {run.hot_set_size}",
            f"hot_access_probability: {run.hot_access_probability:g}",
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

        yaml_policy = yaml_policy_name(run.policy)
        if f"policy: {yaml_policy}" not in config_text:
            raise RuntimeError(
                f"Dry run config {run.config_path} has wrong YAML policy"
            )

        variant = contention_variant_for_policy(run.policy)
        if variant is not None:
            # Variant labels are experiment-facing aliases; generated YAML must
            # keep the simulator policy stable and make the variant explicit.
            for field in ("policy: contention_aware", f"variant: {variant}"):
                if field not in config_text:
                    raise RuntimeError(
                        f"Dry run config {run.config_path} is missing {field}"
                    )

        if run.policy == "hotness_only_cumulative":
            for field in ("policy: hotness_only",
                          "reset_on_epoch_change: false"):
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
    """Entry point: parse sweeps, generate configs, run, and aggregate."""

    repo_root = Path(__file__).resolve().parents[1]
    args = parse_args()
    preset = PRESETS[args.preset]
    seeds = parse_seeds(args.seeds)
    default_policies = (
        PHASE_E_DEFAULT_POLICIES
        if preset.name == "phase_e"
        else BASE_POLICIES
    )
    policies = (
        parse_policies(args.policies)
        if args.policies is not None
        else list(default_policies)
    )
    node_counts = (
        parse_positive_ints(args.node_counts)
        if args.node_counts is not None
        else list(preset.compute_node_count_values)
    )
    epoch_lengths = (
        parse_positive_ints(args.epoch_lengths)
        if args.epoch_lengths is not None
        else list(preset.requests_per_node_per_epoch_values)
    )
    object_counts = (
        parse_positive_ints(args.object_counts)
        if args.object_counts is not None
        else list(preset.object_count_values)
    )
    epoch_counts = (
        parse_positive_ints(args.epoch_counts)
        if args.epoch_counts is not None
        else list(preset.epoch_count_values)
    )
    hot_set_sizes = (
        parse_positive_ints(args.hot_set_sizes)
        if args.hot_set_sizes is not None
        else list(preset.hot_set_size_values)
    )
    hot_access_probabilities = (
        parse_probabilities(args.hot_access_probabilities,
                            "hot-access probability")
        if args.hot_access_probabilities is not None
        else list(preset.hot_access_probabilities)
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
    cache_multipliers = (
        parse_positive_floats(args.cache_hotset_multipliers,
                              "cache hot-set multiplier")
        if args.cache_hotset_multipliers is not None
        else list(preset.cache_capacity_hotset_multipliers)
    )
    memory_bandwidth_levels = (
        parse_levels(args.memory_bandwidth_levels,
                     MEMORY_BANDWIDTH_BYTES_PER_TIME_BY_LEVEL,
                     "memory bandwidth level")
        if args.memory_bandwidth_levels is not None
        else list(preset.memory_bandwidth_levels)
    )
    memory_base_latency_levels = (
        parse_levels(args.memory_base_latency_levels,
                     MEMORY_BASE_LATENCY_BY_LEVEL,
                     "memory base-latency level")
        if args.memory_base_latency_levels is not None
        else list(preset.memory_base_latency_levels)
    )
    link_latency_levels = (
        parse_levels(args.link_latency_levels,
                     LINK_LATENCY_BY_LEVEL,
                     "link latency level")
        if args.link_latency_levels is not None
        else list(preset.link_latency_levels)
    )
    oversized_node_counts = [
        node_count
        for node_count in node_counts
        if node_count > len(preset.compute_node_ids)
    ]
    if oversized_node_counts:
        raise SystemExit(
            f"Preset {preset.name} supports at most "
            f"{len(preset.compute_node_ids)} compute nodes; invalid counts: "
            + ", ".join(str(count) for count in oversized_node_counts)
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
                      node_counts,
                      object_counts,
                      epoch_counts,
                      epoch_lengths,
                      hot_set_sizes,
                      hot_access_probabilities,
                      churn_fractions,
                      overlaps,
                      object_size_modes,
                      cache_multipliers,
                      memory_bandwidth_levels,
                      memory_base_latency_levels,
                      link_latency_levels,
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
