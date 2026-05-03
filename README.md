# dm_simulator

A single-machine discrete-event simulator for a simplified disaggregated
memory system.

## Build

Phase 5 uses `yaml-cpp` for experiment config loading. On macOS with
Homebrew:

```bash
brew install yaml-cpp
```

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run An Experiment

```bash
./build/dm_simulator --config configs/small.yaml
```

This writes `summary.json`, `per_node.csv`, and `latencies.csv` under the
configured output directory.

Supported `local_cache.policy` values are `always_remote`, `lru`,
`hotness_only`, and `global_hottest_replication`. The hotness-only policy also
supports:

```yaml
local_cache:
  hotness:
    min_admit_count: 2
    reset_on_epoch_change: true
```
