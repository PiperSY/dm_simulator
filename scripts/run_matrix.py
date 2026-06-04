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
import hashlib
import itertools
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
    "hotness_only_windowed",
    "global_hottest_replication",
    *CONTENTION_VARIANT_POLICIES,
)

EVAL_CONTENTION_CALIBRATION_POLICIES = (
    "always_remote",
    "lru",
)

EVAL_POLICY_VIABILITY_POLICIES = (
    "lru",
    "hotness_only_windowed",
    "global_hottest_replication",
    *CONTENTION_VARIANT_POLICIES,
)

EVAL_INTERACTION_POLICIES = (
    "lru",
    "hotness_only_windowed",
    "contention_aware_smoothed",
    "contention_aware_reuse_gated",
    "contention_aware_hysteresis",
)

EVAL_KNOB_POLICIES = (
    "lru",
    "hotness_only_windowed",
    "contention_aware_smoothed",
)

# Phase 4C intentionally compares only the practical baseline and one
# contention-aware variant so score-weight effects are not mixed with variant
# differences.
EVAL_CONTENTION_WEIGHT_POLICIES = (
    "lru",
    "contention_aware_smoothed",
)

EVAL_CHANNEL_CALIBRATION_POLICIES = (
    "always_remote",
    "lru",
)

EVAL_CHANNEL_HOTSPOT_POLICIES = (
    "lru",
    "hotness_only_windowed",
    "contention_aware_smoothed",
)

EVAL_BURSTY_CALIBRATION_POLICIES = (
    "always_remote",
    "lru",
)

POLICIES = (
    *BASE_POLICIES,
    "hotness_only_cumulative",
    "hotness_only_windowed",
    *CONTENTION_VARIANT_POLICIES,
    "contention_aware_size_value",
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

RUN_NAME_ALIASES = {
    "eval_interactions_churn_epoch": "int_churn_epoch",
    "eval_interactions_cache_hotset": "int_cache_hotset",
    "eval_interactions_node_bandwidth": "int_node_bw",
    "contention_aware_smoothed": "ca_smoothed",
    "contention_aware_size_value": "ca_size_value",
    "contention_aware_reuse_gated": "ca_reuse_gated",
    "contention_aware_hysteresis": "ca_hysteresis",
    "eval_knob_cache_pressure": "knob_cache",
    "eval_knob_epoch_reuse": "knob_epoch",
    "eval_knob_object_size_mix": "knob_size_mix",
    "eval_knob_hot_concentration": "knob_hotp",
    "eval_contention_weight_sensitivity": "weight_sensitivity",
    "eval_channel_calibration": "chcal",
    "eval_channel_hotspots": "chhot",
    "eval_bursty_calibration": "burstcal",
    "completion_driven": "comp",
    "scheduled_bursty": "burst",
}


@dataclass(frozen=True)
class ContentionWeightProfile:
    """One experimental override for a contention-aware score weight."""

    profile: str
    weight_name: str
    weight_value: float | None


# Empty profile used by non-contention policies and normal matrix presets. This
# keeps MatrixRun uniform without giving LRU fake contention-weight metadata.
NO_CONTENTION_WEIGHT_PROFILE = ContentionWeightProfile("", "", None)

# Defaults mirror the simulator's ContentionPolicyConfig defaults. Weight
# sensitivity profiles copy this map and override exactly one entry.
DEFAULT_CONTENTION_WEIGHTS = {
    "local_hotness_weight": 1.0,
    "remote_access_weight": 1.0,
    "distinct_requester_weight": 1.5,
    "queue_wait_weight": 2.0,
    "remote_service_time_weight": 1.0,
    "size_penalty_weight": 0.5,
    "cost_density_weight": 0.0,
}

# Run names have strict filesystem length limits, so profiles use short tokens
# while aggregate CSV columns preserve the full weight names.
CONTENTION_WEIGHT_NAME_TOKENS = {
    "local_hotness_weight": "lh",
    "remote_access_weight": "ra",
    "distinct_requester_weight": "dr",
    "queue_wait_weight": "qw",
    "remote_service_time_weight": "rs",
    "size_penalty_weight": "sp",
    "cost_density_weight": "cd",
}

# Phase 4C uses one-at-a-time sweeps around the default scoring model. These are
# explanatory sensitivity ranges, not an optimization grid.
CONTENTION_WEIGHT_SWEEPS = (
    ("local_hotness_weight", (0.0, 0.5, 1.0, 2.0)),
    ("remote_access_weight", (0.0, 0.5, 1.0, 2.0)),
    ("distinct_requester_weight", (0.0, 1.0, 1.5, 3.0)),
    ("queue_wait_weight", (0.0, 1.0, 2.0, 4.0)),
    ("remote_service_time_weight", (0.0, 0.5, 1.0, 2.0)),
    ("size_penalty_weight", (0.0, 0.25, 0.5, 1.0)),
)


def profile_value_token(value: float) -> str:
    """Return a compact numeric token for profile names and run names."""

    return f"{value:g}".replace(".", "p")


# Precompute the 24 profiles so dry-run validation, run generation, and docs all
# share the same source of truth.
CONTENTION_WEIGHT_PROFILES = tuple(
    ContentionWeightProfile(
        profile=(
            f"{CONTENTION_WEIGHT_NAME_TOKENS[weight_name]}-"
            f"{profile_value_token(value)}"
        ),
        weight_name=weight_name,
        weight_value=value,
    )
    for weight_name, values in CONTENTION_WEIGHT_SWEEPS
    for value in values
)

AGGREGATE_COLUMNS = (
    "status",
    "error",
    "preset",
    "experiment_name",
    "policy",
    "contention_weight_profile",
    "contention_weight_name",
    "contention_weight_value",
    "seed",
    "node_count",
    "compute_node_count",
    "epoch_count",
    "requests_per_node_per_epoch",
    "workload_issue_mode",
    "burst_size",
    "burst_interval",
    "intra_burst_gap",
    "node_phase_jitter",
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
    "memory_channel_count",
    "hot_object_channel_count",
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
    "peak_memory_channel_queue_depth",
    "max_channel_total_queue_wait",
    "channel_queue_imbalance",
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
        large_object_probabilities: Probability sweep for large objects in
            bimodal mode.
        cache_capacity_hotset_multipliers: Cache capacities expressed as
            multiples of one hot set's representative byte size.
        memory_bandwidth_levels: Named bandwidth sweep values. Names map
            through MEMORY_BANDWIDTH_BYTES_PER_TIME_BY_LEVEL.
        memory_base_latency_levels: Named memory base-latency sweep values.
        link_latency_levels: Named one-way link-latency sweep values.
        requests_per_node_per_epoch_values: Epoch-length sweep, measured as
            requests issued by each compute node per epoch.
        workload_issue_modes: Workload arrival process sweep. Completion-driven
            preserves the historical response-paced model; scheduled_bursty
            uses per-node planned arrivals that can overlap.
        burst_sizes: Number of requests in one scheduled burst.
        burst_intervals: Time between burst starts in scheduled_bursty mode.
        intra_burst_gaps: Time between requests within one burst.
        node_phase_jitters: Deterministic per-node/epoch phase jitter range.
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
        memory_channel_counts: Memory parallelism sweep. Each channel is an
            independent FIFO server in the simulator's memory-node model.
        hot_object_channel_counts: Hotspot concentration sweep. Zero leaves hot
            objects unrestricted; positive values restrict hot objects to the
            first N memory channels.
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
    large_object_probabilities: tuple[float, ...]
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
    workload_issue_modes: tuple[str, ...] = ("completion_driven",)
    burst_sizes: tuple[int, ...] = (4,)
    burst_intervals: tuple[int, ...] = (100,)
    intra_burst_gaps: tuple[int, ...] = (1,)
    node_phase_jitters: tuple[int, ...] = (0,)
    memory_channel_counts: tuple[int, ...] = (1,)
    hot_object_channel_counts: tuple[int, ...] = (0,)


@dataclass(frozen=True)
class MatrixRun:
    """One concrete generated experiment run from a preset's sweep space.

    Attributes:
        preset: The MatrixPreset this run was generated from.
        policy: Cache policy name written to local_cache.policy.
        contention_weight_profile: Optional one-at-a-time contention weight
            override used only by eval_contention_weight_sensitivity.
        seed: Synthetic workload RNG seed.
        compute_node_count: Number of compute nodes active in this run.
        object_count: Size of the synthetic object universe.
        epoch_count: Number of synthetic workload epochs.
        requests_per_node_per_epoch: Workload epoch length for each node.
        workload_issue_mode: Completion-driven or scheduled-bursty request
            arrival model for this run.
        burst_size: Number of planned arrivals per burst in bursty mode.
        burst_interval: Time between burst starts in bursty mode.
        intra_burst_gap: Time between requests inside one burst.
        node_phase_jitter: Per-node/epoch deterministic jitter range.
        hot_set_size: Number of hot objects per node per epoch.
        hot_access_probability: Probability that a generated request targets
            that node's current hot set.
        hot_set_churn_fraction: Fraction of hot-set entries replaced per epoch.
        cross_node_overlap: Hot-set overlap level: "low", "medium", or "high".
        object_size_mode: Object-size generation mode: "fixed" or "bimodal".
        large_object_probability: Probability that an object is large in
            bimodal mode.
        cache_capacity_hotset_multiplier: Cache size as a multiple of one hot
            set's representative byte footprint.
        memory_channel_count: Number of independent memory-channel FIFO
            servers available in this run.
        hot_object_channel_count: Number of channels eligible for hot objects;
            zero means hot objects are unrestricted.
        memory_bandwidth_level: Named memory bandwidth setting.
        memory_base_latency_level: Named memory base-latency setting.
        link_latency_level: Named one-way link-latency setting.
        run_name: Stable run identifier used for file and directory names.
        config_path: Generated YAML config path for this run.
        output_dir: Directory where the simulator writes this run's outputs.
    """

    preset: MatrixPreset
    policy: str
    contention_weight_profile: ContentionWeightProfile
    seed: int
    compute_node_count: int
    object_count: int
    epoch_count: int
    requests_per_node_per_epoch: int
    workload_issue_mode: str
    burst_size: int
    burst_interval: int
    intra_burst_gap: int
    node_phase_jitter: int
    hot_set_size: int
    hot_access_probability: float
    hot_set_churn_fraction: float
    cross_node_overlap: str
    object_size_mode: str
    large_object_probability: float
    cache_capacity_hotset_multiplier: float
    memory_channel_count: int
    hot_object_channel_count: int
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
        large_object_probabilities=(0.1,),
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
        large_object_probabilities=(0.1,),
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
        large_object_probabilities=(0.2,),
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
        large_object_probabilities=(0.2,),
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
    # Final evaluation calibration: isolate the architecture/workload knobs
    # expected to create remote-memory contention before comparing policies.
    "eval_contention_calibration": MatrixPreset(
        name="eval_contention_calibration",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(2, 4, 8, 16),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("mild", "moderate", "severe"),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(8,),
        epoch_count_values=(32,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.6, 0.8, 0.95),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.3,),
        cross_node_overlaps=("low", "medium", "high"),
        object_size_modes=("bimodal",),
        memory_channel_counts=(4,),
        hot_object_channel_counts= (2,),
    ),
    # Policy evaluation matrix: compare practical baselines and
    # contention-aware variants inside a known high-contention regime.
    "eval_policy_viability": MatrixPreset(
        name="eval_policy_viability",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(16,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.1,0.2,0.3,0.4,0.5,0.6),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(4, 8, 16,),
        epoch_count_values=(32,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.1, 0.2, 0.3, 0.4, 0.5),
        cross_node_overlaps=("high",),
        object_size_modes=("bimodal",),
        memory_channel_counts=(4,),
        hot_object_channel_counts= (2,),
    ),
    # Channel calibration: isolate how adding independent memory-channel FIFO
    # servers reduces the artificial global queueing of the original model.
    "eval_channel_calibration": MatrixPreset(
        name="eval_channel_calibration",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(128,),
        epoch_count_values=(16,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.2,),
        cross_node_overlaps=("high",),
        object_size_modes=("fixed",),
        memory_channel_counts=(1, 2, 4, 8),
        hot_object_channel_counts=(0,),
    ),
    
    # Channel hotspots: keep memory parallelism fixed, then concentrate hot
    # objects onto fewer channels to create localized resource contention.
    "eval_channel_hotspots": MatrixPreset(
        name="eval_channel_hotspots",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(128,),
        epoch_count_values=(16,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.2,),
        cross_node_overlaps=("high",),
        object_size_modes=("bimodal",),
        memory_channel_counts=(4,),
        hot_object_channel_counts=(1, 2, 4),
    ),

    # Bursty calibration: compare response-paced issue against planned burst
    # arrivals in the existing channel-hotspot regime before using bursts for
    # broader policy-viability claims.
    "eval_bursty_calibration": MatrixPreset(
        name="eval_bursty_calibration",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(8,),
        workload_issue_modes=("completion_driven", "scheduled_bursty"),
        burst_sizes=(4,),
        burst_intervals=(20,),
        intra_burst_gaps=(1,),
        node_phase_jitters=(0,),
        epoch_count_values=(16,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.2,),
        cross_node_overlaps=("high",),
        object_size_modes=("bimodal",),
        memory_channel_counts=(4,),
        hot_object_channel_counts=(2,),
    ),
    
    # Interaction study 1: isolate how telemetry staleness changes when hot
    # sets churn faster or slower than the per-epoch reuse window.
    "eval_interactions_churn_epoch": MatrixPreset(
        name="eval_interactions_churn_epoch",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(8, 16, 32, 64),
        epoch_count_values=(32,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.0, 0.25, 0.5, 0.75, 1.0),
        cross_node_overlaps=("high",),
        object_size_modes=("bimodal",),
    ),
    # Interaction study 2: isolate cache pressure by varying cache capacity
    # relative to hot-set size while keeping the contention regime fixed.
    "eval_interactions_cache_hotset": MatrixPreset(
        name="eval_interactions_cache_hotset",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.2,0.4, 0.6, 0.8),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(4,),
        epoch_count_values=(32,),
        hot_set_size_values=(8, 16, 32),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.3,),
        cross_node_overlaps=("high",),
        object_size_modes=("bimodal",),
        memory_channel_counts=(4,),
        hot_object_channel_counts= (2,),
    ),
    # Interaction study 3: isolate when architecture crosses from lightly
    # loaded to contended by varying demand sources and memory bandwidth.
    "eval_interactions_node_bandwidth": MatrixPreset(
        name="eval_interactions_node_bandwidth",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(4, 8, 16),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("mild", "moderate", "severe"),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(4,),
        epoch_count_values=(32,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.3,),
        cross_node_overlaps=("high",),
        object_size_modes=("bimodal",),
        memory_channel_counts=(4,),
        hot_object_channel_counts= (2,),
    ),
    # Knob demo 1: vary only cache capacity so presentation plots can show
    # where policies are starved, useful, or large enough for LRU to dominate.
    "eval_knob_cache_pressure": MatrixPreset(
        name="eval_knob_cache_pressure",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.1, 0.2, 0.3, 0.4,
                                           0.5, 0.6, 0.8, 1.0),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(8,),
        epoch_count_values=(32,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.2,),
        cross_node_overlaps=("high",),
        object_size_modes=("bimodal",),
        memory_channel_counts=(4,),
        hot_object_channel_counts= (2,),
    ),
    # Knob demo 2: vary per-epoch request volume to show how local reuse
    # opportunities change hotness and contention-aware policy behavior.
    "eval_knob_epoch_reuse": MatrixPreset(
        name="eval_knob_epoch_reuse",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(4, 8, 16, 32, 64, 128),
        epoch_count_values=(32,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.2,),
        cross_node_overlaps=("high",),
        object_size_modes=("bimodal",),
    ),
    # Knob demo 3: vary large-object probability to isolate when service-cost
    # heterogeneity gives contention-aware telemetry something useful to exploit.
    "eval_knob_object_size_mix": MatrixPreset(
        name="eval_knob_object_size_mix",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.0, 0.05, 0.1, 0.2, 0.3),
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(8,),
        epoch_count_values=(32,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.2,),
        cross_node_overlaps=("high",),
        object_size_modes=("bimodal",),
    ),
    # Knob demo 4: vary hot-access concentration to show when locality alone is
    # enough for LRU/hotness and when contention signals still add context.
    "eval_knob_hot_concentration": MatrixPreset(
        name="eval_knob_hot_concentration",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(8,),
        epoch_count_values=(32,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.5, 0.6, 0.7, 0.8, 0.9, 0.95),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.2,),
        cross_node_overlaps=("high",),
        object_size_modes=("bimodal",),
    ),
    # Weight sensitivity is a one-at-a-time policy study, not an automatic
    # tuning sweep. The runner adds one shared LRU baseline plus the 24
    # contention-aware weight profiles defined above.
    "eval_contention_weight_sensitivity": MatrixPreset(
        name="eval_contention_weight_sensitivity",
        memory_node_id=99,
        cache_hit_latency=1,
        compute_node_ids=tuple(range(1, 17)),
        compute_node_count_values=(8,),
        object_count_values=(256,),
        object_size_bytes=64,
        object_size_small_bytes=64,
        object_size_large_bytes=256,
        large_object_probabilities=(0.1,),
        cache_capacity_hotset_multipliers=(0.5,),
        memory_bandwidth_levels=("severe",),
        memory_base_latency_levels=("medium",),
        link_latency_levels=("medium",),
        requests_per_node_per_epoch_values=(8,),
        epoch_count_values=(32,),
        hot_set_size_values=(16,),
        hot_access_probabilities=(0.8,),
        hot_set_mode="epoch_shift",
        hot_set_churn_fractions=(0.2,),
        cross_node_overlaps=("high",),
        object_size_modes=("bimodal",),
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
        large_object_probabilities=(0.2,),
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
        --large-object-probabilities: Comma-separated large-object probabilities
            for bimodal workloads.
        --memory-channel-counts: Comma-separated memory-channel counts.
        --hot-object-channel-counts: Comma-separated hot-object channel limits;
            zero means unrestricted hot-object channel selection.
        --workload-issue-modes and burst knobs: Optional workload-arrival
            sweeps for comparing completion-driven and scheduled-bursty runs.
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
            "with evaluation presets using their matched comparison sets."
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
        "--workload-issue-modes",
        default=None,
        help="Comma-separated issue modes: completion_driven, scheduled_bursty.",
    )
    parser.add_argument(
        "--burst-sizes",
        default=None,
        help="Comma-separated scheduled-burst sizes.",
    )
    parser.add_argument(
        "--burst-intervals",
        default=None,
        help="Comma-separated times between scheduled burst starts.",
    )
    parser.add_argument(
        "--intra-burst-gaps",
        default=None,
        help="Comma-separated times between requests in a burst.",
    )
    parser.add_argument(
        "--node-phase-jitters",
        default=None,
        help="Comma-separated per-node phase jitter ranges.",
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
        "--large-object-probabilities",
        default=None,
        help="Comma-separated bimodal large-object probabilities in [0, 1].",
    )
    parser.add_argument(
        "--memory-channel-counts",
        default=None,
        help="Comma-separated memory channel counts overriding the preset.",
    )
    parser.add_argument(
        "--hot-object-channel-counts",
        default=None,
        help="Comma-separated hot-object channel limits; 0 means unrestricted.",
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


def parse_nonnegative_ints(value: str) -> list[int]:
    """Parse comma-separated nonnegative integers for sweep dimensions."""

    values = []
    for item in parse_csv_list(value):
        try:
            parsed = int(item)
        except ValueError as error:
            raise SystemExit(f"Invalid integer '{item}'") from error
        if parsed < 0:
            raise SystemExit(f"Invalid nonnegative integer '{item}'")
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


def parse_workload_issue_modes(value: str) -> list[str]:
    """Parse synthetic workload issue-mode names."""

    modes = parse_csv_list(value)
    invalid = [
        mode
        for mode in modes
        if mode not in {"completion_driven", "scheduled_bursty"}
    ]
    if invalid:
        raise SystemExit(
            "Invalid workload issue mode(s): "
            + ", ".join(invalid)
            + ". Expected completion_driven or scheduled_bursty"
        )
    if not modes:
        raise SystemExit("At least one workload issue mode is required")
    return modes


def slug_float(value: float) -> str:
    """Convert a float into a path-safe token for run directory names."""

    return f"{value:g}".replace(".", "p")


def run_name_token(value: str) -> str:
    """Use compact path tokens while preserving full names in aggregate rows."""

    return RUN_NAME_ALIASES.get(value, value)


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
             contention_weight_profile: ContentionWeightProfile,
             seed: int,
             compute_node_count: int,
             object_count: int,
             epoch_count: int,
             epoch_length: int,
             workload_issue_mode: str,
             burst_size: int,
             burst_interval: int,
             intra_burst_gap: int,
             node_phase_jitter: int,
             hot_set_size: int,
             hot_access_probability: float,
             churn_fraction: float,
             overlap: str,
             object_size_mode: str,
             large_object_probability: float,
             cache_multiplier: float,
             memory_channel_count: int,
             hot_object_channel_count: int,
             memory_bandwidth_level: str,
             memory_base_latency_level: str,
             link_latency_level: str) -> str:
    """Build a deterministic run name encoding all matrix dimensions."""

    weight_part = (
        f"__weight-{run_name_token(contention_weight_profile.profile)}"
        if contention_weight_profile.profile
        else ""
    )
    full_name = (
        f"{run_name_token(preset.name)}"
        f"__policy-{run_name_token(policy)}"
        f"{weight_part}"
        f"__seed-{seed}"
        f"__nodes-{compute_node_count}"
        f"__objects-{object_count}"
        f"__epochs-{epoch_count}"
        f"__rpe-{epoch_length}"
        f"__issue-{run_name_token(workload_issue_mode)}"
        f"__burst-{burst_size}-{burst_interval}-{intra_burst_gap}-{node_phase_jitter}"
        f"__hotset-{hot_set_size}"
        f"__hotp-{slug_float(hot_access_probability)}"
        f"__mode-{preset.hot_set_mode}"
        f"__churn-{slug_float(churn_fraction)}"
        f"__overlap-{overlap}"
        f"__size-{object_size_mode}"
        f"__largep-{slug_float(large_object_probability)}"
        f"__cachex-{slug_float(cache_multiplier)}"
        f"__memch-{memory_channel_count}"
        f"__hotch-{hot_object_channel_count}"
        f"__bw-{memory_bandwidth_level}"
        f"__base-{memory_base_latency_level}"
        f"__link-{link_latency_level}"
    )
    return bounded_run_name(full_name)


def bounded_run_name(name: str, max_length: int = 180) -> str:
    """Keep generated file/directory names below common filesystem limits.

    Matrix runs intentionally encode many dimensions in their names. Once
    channel dimensions were added, some presets crossed macOS's 255-character
    per-component limit. The aggregate CSV and generated YAML still preserve
    every dimension exactly, so a readable prefix plus a stable hash is a good
    compromise for filesystem safety.
    """

    if len(name) <= max_length:
        return name
    digest = hashlib.sha1(name.encode("utf-8")).hexdigest()[:10]
    return f"{name[:max_length - len(digest) - 2]}__{digest}"


def build_runs(
    preset: MatrixPreset,
    policies: list[str],
    seeds: list[int],
    node_counts: list[int],
    object_counts: list[int],
    epoch_counts: list[int],
    epoch_lengths: list[int],
    workload_issue_modes: list[str],
    burst_sizes: list[int],
    burst_intervals: list[int],
    intra_burst_gaps: list[int],
    node_phase_jitters: list[int],
    hot_set_sizes: list[int],
    hot_access_probabilities: list[float],
    churn_fractions: list[float],
    overlaps: list[str],
    object_size_modes: list[str],
    large_object_probabilities: list[float],
    cache_multipliers: list[float],
    memory_channel_counts: list[int],
    hot_object_channel_counts: list[int],
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
    dimensions = itertools.product(
        seeds,
        policies,
        node_counts,
        object_counts,
        epoch_counts,
        epoch_lengths,
        workload_issue_modes,
        burst_sizes,
        burst_intervals,
        intra_burst_gaps,
        node_phase_jitters,
        hot_set_sizes,
        hot_access_probabilities,
        churn_fractions,
        overlaps,
        object_size_modes,
        large_object_probabilities,
        cache_multipliers,
        memory_channel_counts,
        hot_object_channel_counts,
        memory_bandwidth_levels,
        memory_base_latency_levels,
        link_latency_levels,
    )
    for (seed,
         policy,
         node_count,
         object_count,
         epoch_count,
         epoch_length,
         workload_issue_mode,
         burst_size,
         burst_interval,
         intra_burst_gap,
         node_phase_jitter,
         hot_set_size,
         hot_access_probability,
         churn_fraction,
         overlap,
         object_size_mode,
         large_probability,
         cache_multiplier,
         memory_channel_count,
         hot_object_channel_count,
         bandwidth_level,
         base_level,
         link_level) in dimensions:
        if hot_object_channel_count > memory_channel_count:
            continue
        if (workload_issue_mode == "scheduled_bursty" and
                burst_size > 1 and
                burst_interval < (burst_size - 1) * intra_burst_gap):
            raise SystemExit(
                "Invalid scheduled_bursty timing: burst_interval must be at "
                "least (burst_size - 1) * intra_burst_gap"
            )
        # Most presets get the empty profile, but Phase 4C expands only
        # contention_aware_smoothed into 24 one-at-a-time weight overrides.
        for weight_profile in contention_weight_profiles_for_policy(
            preset,
            policy,
        ):
            name = run_slug(
                preset,
                policy,
                weight_profile,
                seed,
                node_count,
                object_count,
                epoch_count,
                epoch_length,
                workload_issue_mode,
                burst_size,
                burst_interval,
                intra_burst_gap,
                node_phase_jitter,
                hot_set_size,
                hot_access_probability,
                churn_fraction,
                overlap,
                object_size_mode,
                large_probability,
                cache_multiplier,
                memory_channel_count,
                hot_object_channel_count,
                bandwidth_level,
                base_level,
                link_level,
            )
            runs.append(
                MatrixRun(
                    preset=preset,
                    policy=policy,
                    contention_weight_profile=weight_profile,
                    seed=seed,
                    compute_node_count=node_count,
                    object_count=object_count,
                    epoch_count=epoch_count,
                    requests_per_node_per_epoch=epoch_length,
                    workload_issue_mode=workload_issue_mode,
                    burst_size=burst_size,
                    burst_interval=burst_interval,
                    intra_burst_gap=intra_burst_gap,
                    node_phase_jitter=node_phase_jitter,
                    hot_set_size=hot_set_size,
                    hot_access_probability=hot_access_probability,
                    hot_set_churn_fraction=churn_fraction,
                    cross_node_overlap=overlap,
                    object_size_mode=object_size_mode,
                    large_object_probability=large_probability,
                    cache_capacity_hotset_multiplier=cache_multiplier,
                    memory_channel_count=memory_channel_count,
                    hot_object_channel_count=hot_object_channel_count,
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

    if policy in ("hotness_only_cumulative", "hotness_only_windowed"):
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
        # Size-value experiments use smoothed telemetry so cost-per-byte
        # scoring is not evaluated against a noisy one-epoch-only snapshot.
        "contention_aware_size_value": "smoothed",
        "contention_aware_reuse_gated": "reuse_gated",
        "contention_aware_hysteresis": "hysteresis",
    }
    return variants.get(policy)


def hotness_history_mode(policy: str) -> str:
    """Return the generated hotness history mode for a matrix policy label."""

    if policy == "hotness_only_cumulative":
        return "cumulative"
    if policy == "hotness_only_windowed":
        return "windowed"
    return "epoch"


def contention_weight_profiles_for_policy(
    preset: MatrixPreset,
    policy: str,
) -> tuple[ContentionWeightProfile, ...]:
    """Return controlled weight profiles for a preset/policy pair."""

    if preset.name == "eval_contention_weight_sensitivity":
        # LRU is a single shared baseline. Only the smoothed contention policy
        # receives profiles, avoiding 24 identical baseline reruns.
        if policy == "contention_aware_smoothed":
            return CONTENTION_WEIGHT_PROFILES
        return (NO_CONTENTION_WEIGHT_PROFILE,)
    return (NO_CONTENTION_WEIGHT_PROFILE,)


def contention_weights_for_profile(
    profile: ContentionWeightProfile,
) -> dict[str, float]:
    """Apply a single weight override to the default score weights."""

    weights = dict(DEFAULT_CONTENTION_WEIGHTS)
    if profile.weight_name:
        weights[profile.weight_name] = float(profile.weight_value)
    return weights


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
        "  # Each channel is an independent FIFO service resource; bandwidth",
        "  # is interpreted per channel when channel_count is greater than one.",
        f"  channel_count: {run.memory_channel_count}",
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
        history_mode = hotness_history_mode(run.policy)
        lines.extend(
            [
                "  hotness:",
                "    min_admit_count: 2",
                "    # Hotness baselines use explicit history modes so bounded",
                "    # memory can be compared against epoch and cumulative modes.",
                f"    history_mode: {history_mode}",
            ]
        )
        if history_mode == "windowed":
            lines.append("    history_window_epochs: 4")
    elif yaml_policy == "contention_aware":
        weights = contention_weights_for_profile(run.contention_weight_profile)
        if run.policy == "contention_aware_size_value":
            weights["cost_density_weight"] = 1.0
        lines.extend(
            [
                "  contention:",
                f"    variant: {contention_variant}",
            ]
        )
        if run.policy == "contention_aware_size_value":
            lines.extend(
                [
                    "    # Size-value uses smoothed telemetry plus cost density",
                    "    # to reward high remote pain per cache byte.",
                ]
            )
        if run.contention_weight_profile.weight_name:
            lines.extend(
                [
                    "    # Weight-sensitivity profiles override exactly one",
                    "    # scoring weight while preserving all other defaults.",
                ]
            )
        lines.extend(
            [
                f"    local_hotness_weight: {weights['local_hotness_weight']:g}",
                f"    remote_access_weight: {weights['remote_access_weight']:g}",
                f"    distinct_requester_weight: {weights['distinct_requester_weight']:g}",
                f"    queue_wait_weight: {weights['queue_wait_weight']:g}",
                f"    remote_service_time_weight: {weights['remote_service_time_weight']:g}",
                f"    size_penalty_weight: {weights['size_penalty_weight']:g}",
                f"    cost_density_weight: {weights['cost_density_weight']:g}",
                "    min_admit_score: 1.0",
                "    local_hotness_threshold: 2",
                f"    reset_on_epoch_change: {yaml_bool(True)}",
            ]
        )
        if contention_variant == "smoothed":
            lines.extend(
                [
                    "    # Smoothed telemetry blends the last few prior epochs",
                    "    # so one unlucky epoch does not dominate admissions.",
                    "    telemetry_history_epochs: 2",
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
            "  # Zero leaves hot objects unrestricted. Positive values create",
            "  # controlled channel-local hot spots for memory-channel studies.",
            f"  hot_object_channel_count: {run.hot_object_channel_count}",
            f"  object_size_mode: {run.object_size_mode}",
            f"  object_size_small_bytes: {preset.object_size_small_bytes}",
            f"  object_size_large_bytes: {preset.object_size_large_bytes}",
            f"  large_object_probability: {run.large_object_probability:g}",
            f"  requests_per_node_per_epoch: {run.requests_per_node_per_epoch}",
            "  # completion_driven preserves response-paced issue. scheduled_bursty",
            "  # lets compute nodes issue by planned arrival time within an epoch.",
            f"  issue_mode: {run.workload_issue_mode}",
            f"  burst_size: {run.burst_size}",
            f"  burst_interval: {run.burst_interval}",
            f"  intra_burst_gap: {run.intra_burst_gap}",
            f"  node_phase_jitter: {run.node_phase_jitter}",
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
        "contention_weight_profile": run.contention_weight_profile.profile,
        "contention_weight_name": run.contention_weight_profile.weight_name,
        "contention_weight_value": (
            ""
            if run.contention_weight_profile.weight_value is None
            else run.contention_weight_profile.weight_value
        ),
        "seed": run.seed,
        "node_count": run.compute_node_count,
        "compute_node_count": run.compute_node_count,
        "epoch_count": run.epoch_count,
        "requests_per_node_per_epoch": run.requests_per_node_per_epoch,
        "workload_issue_mode": run.workload_issue_mode,
        "burst_size": run.burst_size,
        "burst_interval": run.burst_interval,
        "intra_burst_gap": run.intra_burst_gap,
        "node_phase_jitter": run.node_phase_jitter,
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
        "large_object_probability": run.large_object_probability,
        "cache_capacity_hotset_multiplier": (
            run.cache_capacity_hotset_multiplier
        ),
        "cache_capacity_bytes": cache_capacity_bytes(run),
        "memory_channel_count": run.memory_channel_count,
        "hot_object_channel_count": run.hot_object_channel_count,
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


def aggregate_channel_contention(output_dir: Path) -> dict[str, Any]:
    """Summarize channel-local queue pressure into run-level totals."""

    path = output_dir / "contention_by_channel.csv"
    totals = {
        "peak_memory_channel_queue_depth": 0,
        "max_channel_total_queue_wait": 0,
        "channel_queue_imbalance": 0.0,
    }
    if not path.exists():
        return {key: "" for key in totals}

    channel_waits: list[float] = []
    with path.open("r", encoding="utf-8", newline="") as input_file:
        for row in csv.DictReader(input_file):
            total_wait = float(row["total_queue_wait"])
            channel_waits.append(total_wait)
            totals["peak_memory_channel_queue_depth"] = max(
                totals["peak_memory_channel_queue_depth"],
                int(row["max_queue_depth"]),
            )
            totals["max_channel_total_queue_wait"] = max(
                totals["max_channel_total_queue_wait"],
                total_wait,
            )
    if channel_waits and sum(channel_waits) > 0.0:
        average_wait = sum(channel_waits) / len(channel_waits)
        totals["channel_queue_imbalance"] = (
            max(channel_waits) / average_wait
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
    channel_summary = memory.get("channels", {})
    row.update(
        {
            "peak_memory_channel_queue_depth": number_or_blank(
                channel_summary.get("peak_queue_depth")
            ),
            "max_channel_total_queue_wait": number_or_blank(
                channel_summary.get("max_total_queue_wait")
            ),
            "channel_queue_imbalance": number_or_blank(
                channel_summary.get("queue_imbalance")
            ),
        }
    )
    # Older outputs will not have memory.channels in summary.json, so fall back
    # to the CSV when present.
    for key, value in aggregate_channel_contention(run.output_dir).items():
        if row.get(key, "") == "":
            row[key] = value
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
            f"channel_count: {run.memory_channel_count}",
            f"one_way_latency: {one_way_link_latency(run)}",
            f"capacity_bytes: {cache_capacity_bytes(run)}",
            f"object_count: {run.object_count}",
            f"epoch_count: {run.epoch_count}",
            f"issue_mode: {run.workload_issue_mode}",
            f"burst_size: {run.burst_size}",
            f"burst_interval: {run.burst_interval}",
            f"intra_burst_gap: {run.intra_burst_gap}",
            f"node_phase_jitter: {run.node_phase_jitter}",
            f"hot_set_size: {run.hot_set_size}",
            f"hot_access_probability: {run.hot_access_probability:g}",
            f"hot_object_channel_count: {run.hot_object_channel_count}",
            "hot_set_churn_fraction:",
            "object_size_mode:",
            "object_size_small_bytes:",
            "object_size_large_bytes:",
            f"large_object_probability: {run.large_object_probability:g}",
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
            expected_weights = contention_weights_for_profile(
                run.contention_weight_profile,
            )
            if run.policy == "contention_aware_size_value":
                expected_weights["cost_density_weight"] = 1.0
            for weight_name, weight_value in expected_weights.items():
                field = f"{weight_name}: {weight_value:g}"
                if field not in config_text:
                    raise RuntimeError(
                        f"Dry run config {run.config_path} is missing {field}"
                    )

        if run.policy in ("hotness_only_cumulative", "hotness_only_windowed"):
            expected_fields = [
                "policy: hotness_only",
                f"history_mode: {hotness_history_mode(run.policy)}",
            ]
            if run.policy == "hotness_only_windowed":
                expected_fields.append("history_window_epochs: 4")
            for field in expected_fields:
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

    if runs and runs[0].preset.name == "eval_contention_weight_sensitivity":
        lru_runs = [run for run in runs if run.policy == "lru"]
        profiled_runs = [
            run for run in runs
            if run.policy == "contention_aware_smoothed"
        ]
        if any(run.contention_weight_profile.profile for run in lru_runs):
            raise RuntimeError("LRU baseline should not receive a weight profile")
        if any(not run.contention_weight_profile.profile for run in profiled_runs):
            raise RuntimeError("Contention weight runs must all carry profiles")
        if (set(selected_policies) == set(EVAL_CONTENTION_WEIGHT_POLICIES)
                and len(runs) == 25):
            if len(lru_runs) != 1 or len(profiled_runs) != len(
                CONTENTION_WEIGHT_PROFILES
            ):
                raise RuntimeError(
                    "Weight sensitivity dry run should produce one LRU baseline "
                    f"and {len(CONTENTION_WEIGHT_PROFILES)} profiled contention runs"
                )


def main() -> int:
    """Entry point: parse sweeps, generate configs, run, and aggregate."""

    repo_root = Path(__file__).resolve().parents[1]
    args = parse_args()
    preset = PRESETS[args.preset]
    seeds = parse_seeds(args.seeds)
    default_policies = (
        PHASE_E_DEFAULT_POLICIES
        if preset.name == "phase_e"
        else EVAL_CONTENTION_CALIBRATION_POLICIES
        if preset.name == "eval_contention_calibration"
        else EVAL_CHANNEL_CALIBRATION_POLICIES
        if preset.name == "eval_channel_calibration"
        else EVAL_CHANNEL_HOTSPOT_POLICIES
        if preset.name == "eval_channel_hotspots"
        else EVAL_BURSTY_CALIBRATION_POLICIES
        if preset.name == "eval_bursty_calibration"
        else EVAL_POLICY_VIABILITY_POLICIES
        if preset.name == "eval_policy_viability"
        else EVAL_INTERACTION_POLICIES
        if preset.name.startswith("eval_interactions_")
        else EVAL_KNOB_POLICIES
        if preset.name.startswith("eval_knob_")
        else EVAL_CONTENTION_WEIGHT_POLICIES
        if preset.name == "eval_contention_weight_sensitivity"
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
    workload_issue_modes = (
        parse_workload_issue_modes(args.workload_issue_modes)
        if args.workload_issue_modes is not None
        else list(preset.workload_issue_modes)
    )
    burst_sizes = (
        parse_positive_ints(args.burst_sizes)
        if args.burst_sizes is not None
        else list(preset.burst_sizes)
    )
    burst_intervals = (
        parse_positive_ints(args.burst_intervals)
        if args.burst_intervals is not None
        else list(preset.burst_intervals)
    )
    intra_burst_gaps = (
        parse_nonnegative_ints(args.intra_burst_gaps)
        if args.intra_burst_gaps is not None
        else list(preset.intra_burst_gaps)
    )
    node_phase_jitters = (
        parse_nonnegative_ints(args.node_phase_jitters)
        if args.node_phase_jitters is not None
        else list(preset.node_phase_jitters)
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
    large_object_probabilities = (
        parse_probabilities(args.large_object_probabilities,
                            "large-object probability")
        if args.large_object_probabilities is not None
        else list(preset.large_object_probabilities)
    )
    memory_channel_counts = (
        parse_positive_ints(args.memory_channel_counts)
        if args.memory_channel_counts is not None
        else list(preset.memory_channel_counts)
    )
    hot_object_channel_counts = (
        parse_nonnegative_ints(args.hot_object_channel_counts)
        if args.hot_object_channel_counts is not None
        else list(preset.hot_object_channel_counts)
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
                      workload_issue_modes,
                      burst_sizes,
                      burst_intervals,
                      intra_burst_gaps,
                      node_phase_jitters,
                      hot_set_sizes,
                      hot_access_probabilities,
                      churn_fractions,
                      overlaps,
                      object_size_modes,
                      large_object_probabilities,
                      cache_multipliers,
                      memory_channel_counts,
                      hot_object_channel_counts,
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
