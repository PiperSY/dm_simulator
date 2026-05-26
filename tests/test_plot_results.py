#!/usr/bin/env python3

"""Smoke-test the Phase F plotting/reporting script."""

from __future__ import annotations

import csv
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path


SKIP_RETURN_CODE = 77


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as input_file:
        return list(csv.DictReader(input_file))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


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
    subprocess.run(
        [
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
        ],
        check=True,
    )

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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
