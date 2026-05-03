#!/usr/bin/env python3

"""Run one or more dm_simulator experiment configs."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "configs",
        nargs="+",
        type=Path,
        help="YAML config files to run.",
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
        default=repo_root / "results",
        help="Directory where per-config result folders are written.",
    )
    return parser.parse_args()

def main() -> int:
    args = parse_args()
    args.results_dir.mkdir(parents=True, exist_ok=True)

    for config_path in args.configs:
        output_dir = args.results_dir / config_path.stem
        command = [
            str(args.binary),
            "--config",
            str(config_path),
            "--output-dir",
            str(output_dir),
        ]
        print(f"Running {config_path} -> {output_dir}", flush=True)
        subprocess.run(command, check=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
