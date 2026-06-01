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
    if importlib.util.find_spec("matplotlib") is None:
        print("Skipping plot_results smoke test: matplotlib is not installed")
        return SKIP_RETURN_CODE

    repo_root = Path(__file__).resolve().parents[1]
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
    ]
    for path in viability_files:
        require(path.exists(), f"Missing viability plot: {path}")
    viability_report = (viability_dir / "report.md").read_text(
        encoding="utf-8",
    )
    require("Report mode: `policy_viability`" in viability_report,
            "Viability report should auto-detect policy viability mode")
    require("Churn-Conditioned Policy Viability" in viability_report,
            "Viability report should group churn-conditioned plots")

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
