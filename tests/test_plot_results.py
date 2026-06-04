#!/usr/bin/env python3

"""Smoke-test the Phase F plotting/reporting script."""

from __future__ import annotations

import csv
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


SKIP_RETURN_CODE = 77


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as input_file:
        return list(csv.DictReader(input_file))


def write_rows(path: Path,
               fieldnames: list[str],
               rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_plot_results_module(repo_root: Path):
    spec = importlib.util.spec_from_file_location(
        "plot_results",
        repo_root / "scripts" / "plot_results.py",
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_heatmap_color_scale_helper(repo_root: Path) -> None:
    plot_results = load_plot_results_module(repo_root)

    vmin, vmax, cmap = plot_results.heatmap_color_scale(
        "mean_latency_delta_pct",
        [-7.1, 0.0, 8.8],
    )
    require(cmap == "coolwarm",
            "Delta heatmaps should use a diverging color map")
    require(abs(vmin + vmax) < 1e-9,
            "Delta heatmaps should be centered on zero")
    require(abs(vmax - 8.8) < 1e-9,
            "Delta heatmaps should share the largest absolute bound")

    vmin, vmax, cmap = plot_results.heatmap_color_scale(
        "local_hit_rate",
        [0.1, 0.2, 0.5],
    )
    require(cmap == "viridis",
            "Absolute heatmaps should use a sequential color map")
    require(abs(vmin - 0.1) < 1e-9 and abs(vmax - 0.5) < 1e-9,
            "Absolute heatmaps should use global min/max bounds")


def run_plot_results(script: Path,
                     fixture: Path,
                     output_dir: Path,
                     report_mode: Optional[str] = None) -> None:
    command = [
        sys.executable,
        str(script),
        "--aggregate",
        str(fixture),
        "--output-dir",
        str(output_dir),
        "--baseline",
        "lru",
        "--tie-threshold",
        "0.02",
    ]
    if report_mode is not None:
        command.extend(["--report-mode", report_mode])
    subprocess.run(command, check=True)


def write_contention_file(output_dir: Path,
                          objects: list[tuple[int, int, int]]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    path = output_dir / "contention_by_object.csv"
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.writer(output_file)
        writer.writerow([
            "epoch_id",
            "object_id",
            "remote_accesses",
            "distinct_requesters",
            "bytes_served",
            "total_remote_service_time",
            "total_queue_wait",
            "max_queue_wait",
            "queue_wait_samples",
            "max_observed_queue_depth",
            "average_queue_wait",
        ])
        for object_id, remote_accesses, distinct_requesters in objects:
            writer.writerow([
                0,
                object_id,
                remote_accesses,
                distinct_requesters,
                remote_accesses * 64,
                remote_accesses * 40,
                remote_accesses * 10,
                10,
                remote_accesses,
                distinct_requesters,
                10,
            ])


def seed_calibration_contention_fixture(repo_root: Path) -> None:
    write_contention_file(
        repo_root / "fixture" / "calibration" / "lru1",
        [(1, 80, 1), (2, 40, 1), (3, 20, 1)],
    )
    write_contention_file(
        repo_root / "fixture" / "calibration" / "remote1",
        [(1, 100, 1), (2, 50, 1), (3, 25, 1)],
    )
    write_contention_file(
        repo_root / "fixture" / "calibration" / "lru2",
        [(10, 500, 12), (11, 400, 10), (12, 350, 9)],
    )
    # Intentionally omit remote2 to verify missing object-contention files are
    # ignored rather than making report generation fail.


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    test_heatmap_color_scale_helper(repo_root)

    if importlib.util.find_spec("matplotlib") is None:
        print("Skipping plot_results smoke test: matplotlib is not installed")
        return SKIP_RETURN_CODE

    output_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        repo_root / "build" / "plot_results_smoke"
    )
    if output_dir.exists():
        shutil.rmtree(output_dir)

    script = repo_root / "scripts" / "plot_results.py"
    fixture = repo_root / "tests" / "fixtures" / "aggregate_summary_phase_f.csv"
    run_plot_results(script, fixture, output_dir)

    expected_files = [
        output_dir / "report.md",
        output_dir / "policy_comparison.csv",
        output_dir / "policy_summary.csv",
        output_dir / "condition_summary.csv",
        output_dir / "plots" / "policy_latency_overview.svg",
        output_dir / "plots" / "latency_delta_vs_baseline.svg",
        output_dir / "plots" / "viability_scatter.svg",
    ]
    for path in expected_files:
        require(path.exists(), f"Missing expected output: {path}")

    comparison_rows = read_rows(output_dir / "policy_comparison.csv")
    by_experiment = {
        row["experiment_name"]: row
        for row in comparison_rows
    }
    stable_contention = by_experiment["fixture_stable_contention"]
    require(stable_contention["classification"] == "win",
            "Stable contention row should be a win")
    require(abs(float(stable_contention["mean_latency_delta_pct"]) + 0.10) <
            1e-6,
            "Stable contention delta should be -10%")

    stable_hotness = by_experiment["fixture_stable_hotness"]
    require(stable_hotness["classification"] == "tie",
            "Hotness row should be inside the 2% tie threshold")

    unstable_contention = by_experiment["fixture_unstable_contention"]
    require(unstable_contention["classification"] == "loss",
            "Unstable contention row should be a loss")

    report = (output_dir / "report.md").read_text(encoding="utf-8")
    require("Interpretation Guidance" in report,
            "Report should include interpretation guidance")
    require("policy_latency_overview" in report,
            "Report should link generated plots")

    calibration_dir = output_dir.parent / "plot_results_calibration"
    seed_calibration_contention_fixture(repo_root)
    run_plot_results(
        script,
        repo_root / "tests" / "fixtures" / "aggregate_summary_calibration.csv",
        calibration_dir,
    )
    calibration_files = [
        calibration_dir / "plots" / "calibration_node_count_pressure.svg",
        calibration_dir / "plots" / "calibration_memory_pressure_heatmap.svg",
        calibration_dir / "plots" / "calibration_hotness_memory_pressure_heatmap.svg",
        calibration_dir / "plots" / "calibration_overlap_hotness_heatmap.svg",
        calibration_dir / "plots" / "calibration_overlap_hotness_concentration.svg",
        calibration_dir / "plots" / "calibration_policy_pressure_reduction.svg",
    ]
    for path in calibration_files:
        require(path.exists(), f"Missing calibration plot: {path}")
    calibration_report = (calibration_dir / "report.md").read_text(
        encoding="utf-8",
    )
    require("Report mode: `contention_calibration`" in calibration_report,
            "Calibration report should auto-detect contention mode")
    require("churn_sweep" not in calibration_report,
            "Calibration report should not include fixed churn sweep plot")

    channel_fields = [
        "status",
        "preset",
        "experiment_name",
        "policy",
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
        "mean_latency",
        "p99_latency",
        "local_hit_rate",
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
        "jain_inverse_latency_fairness",
        "per_node_mean_latency_spread",
        "per_node_p99_latency_max",
        "output_dir",
        "config_path",
    ]

    def channel_row(preset: str,
                    name: str,
                    policy: str,
                    memory_channels: str,
                    hot_channels: str,
                    mean_latency: str,
                    memory_wait: str,
                    channel_wait: str,
                    channel_depth: str,
                    imbalance: str,
                    issue_mode: str = "completion_driven") -> dict[str, str]:
        row = {field: "" for field in channel_fields}
        row.update({
            "status": "success",
            "preset": preset,
            "experiment_name": name,
            "policy": policy,
            "seed": "8888",
            "node_count": "8",
            "epoch_count": "16",
            "requests_per_node_per_epoch": "128",
            "workload_issue_mode": issue_mode,
            "burst_size": "4",
            "burst_interval": "20",
            "intra_burst_gap": "1",
            "node_phase_jitter": "0",
            "hot_set_mode": "epoch_shift",
            "hot_set_churn_fraction": "0.2",
            "cross_node_overlap": "high",
            "object_count": "256",
            "hot_set_size": "16",
            "hot_access_probability": "0.8",
            "object_size_mode": "fixed",
            "object_size_bytes": "64",
            "object_size_small_bytes": "64",
            "object_size_large_bytes": "256",
            "large_object_probability": "0.1",
            "cache_capacity_hotset_multiplier": "0.5",
            "cache_capacity_bytes": "512",
            "memory_channel_count": memory_channels,
            "hot_object_channel_count": hot_channels,
            "memory_bandwidth_level": "severe",
            "memory_bandwidth_bytes_per_time": "4",
            "memory_base_latency_level": "medium",
            "memory_base_latency": "25",
            "link_latency_level": "medium",
            "one_way_link_latency": "6",
            "mean_latency": mean_latency,
            "p99_latency": str(float(mean_latency) * 1.8),
            "local_hit_rate": "0.4" if policy == "lru" else "0.0",
            "average_memory_wait": memory_wait,
            "max_memory_wait": str(float(memory_wait) * 2),
            "peak_memory_queue_depth": channel_depth,
            "peak_memory_channel_queue_depth": channel_depth,
            "max_channel_total_queue_wait": channel_wait,
            "channel_queue_imbalance": imbalance,
            "total_remote_accesses": "1000",
            "total_queue_wait": str(float(channel_wait) * 2),
            "total_remote_service_time": "64000",
            "max_observed_queue_depth": channel_depth,
            "jain_inverse_latency_fairness": "1.0",
            "per_node_mean_latency_spread": "0",
            "per_node_p99_latency_max": str(float(mean_latency) * 1.8),
            "output_dir": f"fixture/channel/{name}",
            "config_path": f"fixture/channel/{name}.yaml",
        })
        return row

    channel_count_fixture = output_dir.parent / "aggregate_channel_count.csv"
    write_rows(
        channel_count_fixture,
        channel_fields,
        [
            channel_row("eval_channel_calibration", "ch1_lru", "lru",
                        "1", "0", "200", "80", "100000", "12", "1.0"),
            channel_row("eval_channel_calibration", "ch1_remote",
                        "always_remote", "1", "0", "330", "140",
                        "180000", "20", "1.0"),
            channel_row("eval_channel_calibration", "ch4_lru", "lru",
                        "4", "0", "130", "30", "25000", "4", "1.2"),
            channel_row("eval_channel_calibration", "ch4_remote",
                        "always_remote", "4", "0", "210", "55",
                        "45000", "8", "1.3"),
        ],
    )
    channel_count_dir = output_dir.parent / "plot_results_channel_count"
    run_plot_results(script, channel_count_fixture, channel_count_dir)
    require((channel_count_dir / "plots" /
             "calibration_channel_count_pressure.svg").exists(),
            "Channel-count calibration plot should be generated")

    channel_hotspot_fixture = output_dir.parent / "aggregate_channel_hotspots.csv"
    write_rows(
        channel_hotspot_fixture,
        channel_fields,
        [
            channel_row("eval_channel_hotspots", "hotch1_lru", "lru",
                        "4", "1", "190", "75", "120000", "16", "3.5"),
            channel_row("eval_channel_hotspots", "hotch1_ca",
                        "contention_aware_smoothed", "4", "1", "170",
                        "60", "95000", "14", "3.0"),
            channel_row("eval_channel_hotspots", "hotch4_lru", "lru",
                        "4", "4", "130", "30", "30000", "5", "1.1"),
            channel_row("eval_channel_hotspots", "hotch4_ca",
                        "contention_aware_smoothed", "4", "4", "125",
                        "28", "26000", "4", "1.0"),
        ],
    )
    channel_hotspot_dir = output_dir.parent / "plot_results_channel_hotspots"
    run_plot_results(script, channel_hotspot_fixture, channel_hotspot_dir)
    require((channel_hotspot_dir / "plots" /
             "calibration_channel_hotspot_pressure.svg").exists(),
            "Channel-hotspot calibration plot should be generated")

    bursty_fixture = output_dir.parent / "aggregate_bursty_calibration.csv"
    write_rows(
        bursty_fixture,
        channel_fields,
        [
            channel_row("eval_bursty_calibration", "comp_lru", "lru",
                        "4", "2", "140", "35", "30000", "5", "1.1",
                        "completion_driven"),
            channel_row("eval_bursty_calibration", "burst_lru", "lru",
                        "4", "2", "190", "75", "90000", "14", "2.8",
                        "scheduled_bursty"),
            channel_row("eval_bursty_calibration", "comp_remote",
                        "always_remote", "4", "2", "210", "55",
                        "50000", "8", "1.2", "completion_driven"),
            channel_row("eval_bursty_calibration", "burst_remote",
                        "always_remote", "4", "2", "330", "130",
                        "160000", "22", "3.2", "scheduled_bursty"),
        ],
    )
    bursty_dir = output_dir.parent / "plot_results_bursty_calibration"
    run_plot_results(script, bursty_fixture, bursty_dir)
    require((bursty_dir / "plots" /
             "calibration_issue_mode_pressure.svg").exists(),
            "Issue-mode calibration plot should be generated")

    viability_dir = output_dir.parent / "plot_results_viability"
    run_plot_results(
        script,
        repo_root / "tests" / "fixtures" / "aggregate_summary_viability.csv",
        viability_dir,
    )
    viability_files = [
        viability_dir / "plots" / "viability_churn_epoch_heatmap.svg",
        viability_dir / "plots" / "viability_cache_pressure_by_churn.svg",
        viability_dir / "plots" / "viability_admission_quality.svg",
        viability_dir / "plots" / "viability_admission_quality_by_churn.svg",
        viability_dir / "plots" / "viability_remote_pressure_by_churn.svg",
        viability_dir / "plots" / "viability_fairness_by_churn.svg",
        viability_dir / "plots" / "viability_scatter.svg",
        viability_dir / "plots" / "viability_best_run_latency_examples.svg",
        viability_dir / "plots" / "viability_best_run_pressure_examples.svg",
        viability_dir / "viability_best_runs.csv",
        viability_dir / "viability_best_run_policy_context.csv",
    ]
    for path in viability_files:
        require(path.exists(), f"Missing viability plot: {path}")
    best_rows = read_rows(viability_dir / "viability_best_runs.csv")
    best_policies = {row["target_policy"] for row in best_rows}
    require("lru" not in best_policies,
            "Best-run examples should exclude LRU baseline")
    require("global_hottest_replication" not in best_policies,
            "Best-run examples should exclude oracle replication")
    require("always_remote" not in best_policies,
            "Best-run examples should exclude always-remote baseline")
    best_contention = next(
        row for row in best_rows
        if row["target_policy"] == "contention_aware_smoothed"
    )
    require(best_contention["hot_set_churn_fraction"] == "0.0",
            "Contention best run should choose the lowest mean-latency delta")
    require(best_contention["cache_capacity_hotset_multiplier"] == "0.25",
            "Best-run CSV should preserve sweep values")
    require(abs(float(best_contention["mean_latency_delta_pct"]) +
                (20.0 / 120.0)) < 1e-6,
            "Best-run selection should use mean latency delta")
    context_rows = read_rows(
        viability_dir / "viability_best_run_policy_context.csv",
    )
    context_policies = {row["comparison_policy"] for row in context_rows}
    require("hotness_only_windowed" in context_policies,
            "Best-run context should include peer cache policies")
    require("global_hottest_replication" not in context_policies,
            "Best-run context should exclude oracle policies")
    viability_report = (viability_dir / "report.md").read_text(
        encoding="utf-8",
    )
    require("Report mode: `policy_viability`" in viability_report,
            "Viability report should auto-detect policy viability mode")
    require("Churn-Conditioned Policy Viability" in viability_report,
            "Viability report should group churn-conditioned plots")
    require("Best Run Examples" in viability_report,
            "Viability report should include best-run examples")
    require("0.25" in viability_report,
            "Best-run report table should include sweep values")

    single_churn_fixture = output_dir.parent / "aggregate_viability_single_churn.csv"
    with (repo_root / "tests" / "fixtures" /
          "aggregate_summary_viability.csv").open(
              "r",
              encoding="utf-8",
              newline="",
          ) as input_file:
        reader = csv.DictReader(input_file)
        fieldnames = list(reader.fieldnames or [])
        single_churn_rows = [
            row for row in reader
            if row["hot_set_churn_fraction"] == "0.0"
        ]
    extra_cache_rows = []
    for row in single_churn_rows:
        duplicate = dict(row)
        duplicate["experiment_name"] = duplicate["experiment_name"] + "_cache1"
        duplicate["cache_capacity_hotset_multiplier"] = "1.0"
        duplicate["cache_capacity_bytes"] = "512"
        duplicate["output_dir"] = duplicate["output_dir"] + "_cache1"
        extra_cache_rows.append(duplicate)
    single_churn_rows.extend(extra_cache_rows)
    write_rows(single_churn_fixture, fieldnames, single_churn_rows)
    single_churn_dir = output_dir.parent / "plot_results_viability_single_churn"
    run_plot_results(script, single_churn_fixture, single_churn_dir)
    single_churn_plot = (
        single_churn_dir / "plots" / "viability_cache_pressure_by_churn.svg"
    )
    require(single_churn_plot.exists(),
            "Single-churn viability report should still render the facet plot")

    interaction_dir = output_dir.parent / "plot_results_interaction"
    run_plot_results(
        script,
        repo_root / "tests" / "fixtures" / "aggregate_summary_interaction.csv",
        interaction_dir,
    )
    interaction_plot = (
        interaction_dir / "plots" / "interaction_mean_latency_delta_heatmap.svg"
    )
    require(interaction_plot.exists(),
            f"Missing interaction heatmap: {interaction_plot}")
    interaction_report = (interaction_dir / "report.md").read_text(
        encoding="utf-8",
    )
    require("Report mode: `interaction`" in interaction_report,
            "Interaction report should auto-detect interaction mode")

    parameter_dir = output_dir.parent / "plot_results_parameter_demo"
    run_plot_results(
        script,
        repo_root / "tests" / "fixtures" /
        "aggregate_summary_parameter_demo.csv",
        parameter_dir,
    )
    parameter_files = [
        parameter_dir / "plots" / "parameter_demo_latency_hit_rate.svg",
        parameter_dir / "plots" / "parameter_demo_memory_pressure.svg",
        parameter_dir / "plots" / "parameter_demo_admission_quality.svg",
        parameter_dir / "plots" / "parameter_demo_tail_latency.svg",
    ]
    for path in parameter_files:
        require(path.exists(), f"Missing parameter-demo plot: {path}")
    parameter_report = (parameter_dir / "report.md").read_text(
        encoding="utf-8",
    )
    require("Report mode: `parameter_demo`" in parameter_report,
            "Parameter-demo report should auto-detect eval_knob presets")
    require("Isolated Parameter Demonstrations" in parameter_report,
            "Parameter-demo report should group isolated knob plots")

    weight_dir = output_dir.parent / "plot_results_weight_sensitivity"
    run_plot_results(
        script,
        repo_root / "tests" / "fixtures" /
        "aggregate_summary_weight_sensitivity.csv",
        weight_dir,
    )
    weight_files = [
        weight_dir / "plots" / "weight_sensitivity_latency.svg",
        weight_dir / "plots" / "weight_sensitivity_memory_hit_rate.svg",
        weight_dir / "plots" / "weight_sensitivity_admission_regret.svg",
    ]
    for path in weight_files:
        require(path.exists(), f"Missing weight-sensitivity plot: {path}")
    weight_report = (weight_dir / "report.md").read_text(encoding="utf-8")
    require("Report mode: `weight_sensitivity`" in weight_report,
            "Weight report should auto-detect weight-sensitivity mode")
    require("Contention Weight Sensitivity" in weight_report,
            "Weight report should group weight-sensitivity plots")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
