# DM Simulator

`dm_simulator` is a deterministic, discrete-event simulator for studying
private caching in a simplified disaggregated-memory system. It focuses on a
specific research question: when can coarse contention telemetry improve cache
placement compared with recency or hotness alone?

The simulator models compute nodes with private caches, one shared remote
memory node with configurable independent channels, synthetic workloads, and
several cache-policy families. It also includes a matrix runner and plotting
pipeline for reproducible policy and architecture evaluations.

Contention-aware caching is not expected to win in every configuration. The
project is designed to identify the workload and architecture regimes where
contention signals are predictive enough to justify using them.

## Quick Start

Install dependencies, build, and run the tests:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Run one YAML experiment:

```bash
./build/dm_simulator \
  --config configs/manual_phase8_lru.yaml \
  --output-dir results/manual_phase8_lru
```

Run a matched policy matrix:

```bash
scripts/run_matrix.py \
  --preset eval_policy_iteration \
  --results-dir results/eval_policy_iteration
```

Generate plots and a Markdown report:

```bash
scripts/plot_results.py \
  --aggregate results/eval_policy_iteration/aggregate_summary.csv \
  --output-dir results/eval_policy_iteration/analysis \
  --baseline lru \
  --report-mode policy_viability
```

## Project Overview

Disaggregated-memory architectures separate compute from a shared memory
resource. This can increase capacity and flexibility, but remote accesses pay
link, service, and queueing costs. A private cache can avoid those costs, though
its limited capacity creates a placement problem: the most frequently accessed
object is not always the object whose caching provides the most system-wide
benefit.

This project compares several placement strategies:

- LRU uses recent local access history.
- Hotness-only policies use local frequency over bounded or unbounded history.
- Contention-aware policies combine local reuse with prior-epoch remote
  pressure, requester diversity, queue wait, service cost, and object size.

The simulator provides controlled experiments for both sides of the research
claim:

1. Architecture and workload knobs should create understandable contention.
2. Cache policies should produce explainable wins, ties, and losses inside
   those contention regimes.

## Architecture At A Glance

```text
YAML config
    |
    v
ExperimentRunner -> Simulator -> deterministic Scheduler
                                  |
                                  v
Workload -> ComputeNode -> private LocalCache
                              | hit
                              +------> local completion
                              |
                              | miss
                              v
                         link latency
                              |
                              v
                    shared MemoryNode
                    object -> channel
                    FIFO/server per channel
                              |
                              v
                    response + cache admission
                              |
                              v
                   Stats and result files
```

The scheduler advances simulated time by removing the next timestamped event
from a deterministic priority queue. Requests may be response-paced
(`completion_driven`) or issued according to planned per-node burst schedules
(`scheduled_bursty`).

Epoch barriers ensure that all work from one epoch completes before the next
epoch begins. Contention-aware policies therefore consume completed
prior-epoch telemetry rather than same-epoch future knowledge.

The current memory model has one shared memory node and one FIFO/server per
configured channel. Objects map deterministically to channels. Workloads can
restrict hot objects to a subset of channels to create repeatable
channel-local hotspots.

For a fuller description, see [Architecture](docs/architecture.md) and the
[Implementation and Execution Guide](docs/implementation_execution_guide.md).

## Requirements And Build

Required tools and libraries:

- CMake 3.16 or newer
- A C++17 compiler
- Python 3.9 or newer
- `yaml-cpp`

On macOS with Homebrew:

```bash
brew install cmake yaml-cpp
```

On Ubuntu or Debian:

```bash
sudo apt update
sudo apt install build-essential cmake libyaml-cpp-dev python3
```

Configure and build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Run the test suite:

```bash
ctest --test-dir build --output-on-failure
```

Plot generation uses Matplotlib. Install the optional analysis dependency with:

```bash
python3 -m pip install -r requirements-analysis.txt
```

Without Matplotlib, the simulator and matrix runner still work, and plotting
tests skip with an installation message.

## Running One Experiment

The executable accepts a YAML configuration and an optional output override:

```bash
./build/dm_simulator --config <config-path> [--output-dir <output-path>]
```

Show the executable usage:

```bash
./build/dm_simulator --help
```

Three matched manual configurations are provided for quick comparisons:

```bash
./build/dm_simulator \
  --config configs/manual_phase8_lru.yaml

./build/dm_simulator \
  --config configs/manual_phase8_hotness_windowed.yaml

./build/dm_simulator \
  --config configs/manual_phase8_contention_size_value.yaml
```

These files use the same architecture and workload while changing the cache
policy. Each YAML experiment contains:

- `experiment`: experiment name and default output directory
- `memory`: remote-memory latency, bandwidth, and channel count
- `link`: one-way link latency
- `local_cache`: capacity, hit latency, policy, and policy-specific settings
- `workload`: nodes, objects, hot sets, churn, overlap, size distribution, and
  request-issue behavior

### Hotness History Example

Hotness-only history is explicit and can be epoch-local, cumulative, or
windowed:

```yaml
local_cache:
  policy: hotness_only
  hotness:
    min_admit_count: 2
    history_mode: windowed
    history_window_epochs: 4
```

Windowed history is the primary bounded-history hotness baseline. It preserves
recent evidence without retaining every observation for the entire run.

## Cache Policies

| Policy or Matrix Alias | Purpose |
| --- | --- |
| `always_remote` | Disables local caching and provides a remote-only calibration baseline. |
| `lru` | Practical recency baseline with no contention telemetry. |
| `hotness_only` | Local frequency policy using `epoch`, `cumulative`, or `windowed` history. |
| `hotness_only_windowed` | Matrix alias for a four-epoch bounded hotness history. |
| `global_hottest_replication` | Oracle-style synthetic baseline using global workload knowledge; not a deployable policy. |
| `contention_aware_v1` | Uses one completed prior epoch of local and remote contention signals. |
| `contention_aware_smoothed` | Blends bounded prior epochs to reduce telemetry noise. |
| `contention_aware_reuse_gated` | Requires local reuse evidence before admitting a remotely expensive object. |
| `contention_aware_hysteresis` | Requires an incoming object to beat the victim by a score margin. |
| `contention_aware_smoothed_reuse_gated` | Combines smoothing with bounded local confirmation and a high-score bypass. |
| `contention_aware_smoothed_reuse_gated_hysteresis` | Adds mild replacement hysteresis to the combined policy. |
| `contention_aware_size_value` | Smoothed policy with cost-density scoring for remote pain avoided per cache byte. |

The `contention_aware_*` names above are matrix-runner aliases. Generated YAML
uses `local_cache.policy: contention_aware` and selects behavior through the
contention `variant` and associated weights.

## Workload And Contention Controls

Important workload and architecture controls include:

| Control | What It Changes |
| --- | --- |
| Compute-node count | Aggregate request demand on shared remote memory. |
| Memory bandwidth and base latency | Remote service capacity and miss cost. |
| Memory channel count | Number of independent memory-side FIFO servers. |
| Hot-object channel count | Number of channels eligible to hold hot objects; smaller values create localized hotspots. |
| Hot-set size and cache capacity | Working-set pressure on each private cache. |
| Hot-access probability | Concentration of requests on the hot set. |
| Cross-node overlap | Degree to which compute nodes request the same hot objects. |
| Hot-set churn | Stability of popularity and prior-epoch telemetry. |
| Requests per node per epoch | Reuse opportunities before an epoch transition. |
| Fixed or bimodal object sizes | Remote service cost and cache-space competition. |
| Completion-driven or scheduled-bursty issue | Whether request arrivals are response-paced or can overlap in bursts. |

See the [Testing and Evaluation Plan](docs/testing_evaluation_plan.md) for
recommended ranges and the purpose of each evaluation phase.

## Matrix Evaluation

`scripts/run_matrix.py` creates matched YAML configurations, runs the simulator,
and aggregates one row per run into `aggregate_summary.csv`.

Show all available options:

```bash
scripts/run_matrix.py --help
```

### Dry-Run First

Evaluation presets can generate many runs. Always inspect a dry-run before
starting a full matrix:

```bash
scripts/run_matrix.py \
  --preset eval_policy_iteration \
  --dry-run \
  --results-dir results/eval_policy_iteration_dry_run
```

The dry-run validates the matrix, writes generated YAML files, and reports the
number of configurations without launching the simulator.

CTest also registers dry-run assertions for important presets. If a preset's
sweep dimensions or default policies change, update its corresponding
`--expect-runs` value in `CMakeLists.txt`.

### Run The Current Policy Iteration Study

The focused policy-iteration preset compares LRU and windowed hotness against
smoothed, combined reuse-gated, mild-hysteresis, and size-value
contention-aware policies:

```bash
scripts/run_matrix.py \
  --preset eval_policy_iteration \
  --results-dir results/eval_policy_iteration
```

Use `--keep-going` when a large exploratory matrix should record failures and
continue with its remaining configurations:

```bash
scripts/run_matrix.py \
  --preset eval_policy_iteration \
  --keep-going \
  --results-dir results/eval_policy_iteration
```

Matrix output structure:

```text
results/eval_policy_iteration/
  generated_configs/       # one generated YAML file per matrix point
  runs/                    # one simulator output directory per run
  aggregate_summary.csv    # matrix-level input for analysis
```

### Evaluation Presets

| Preset Family | Primary Question | Plot Report Mode |
| --- | --- | --- |
| `eval_contention_calibration` | Do architecture and workload knobs create predictable remote-memory pressure? | `contention_calibration` |
| `eval_channel_calibration` | How does memory-channel parallelism change global queueing? | `contention_calibration` |
| `eval_channel_hotspots` | How does concentrating hot objects create channel-local pressure? | `contention_calibration` |
| `eval_bursty_calibration` | How do scheduled bursts differ from completion-driven arrivals? | `contention_calibration` |
| `eval_policy_viability` | Where do established baselines and contention variants win or lose? | `policy_viability` |
| `eval_policy_iteration` | How do the latest combined and size-aware policies compare? | `policy_viability` |
| `eval_final_*` | Produce the narrowed multi-seed studies used by the final paper figures. | `final_report` |
| `eval_interactions_*` | How do paired parameters interact? | `interaction` |
| `eval_knob_*` | How does one isolated parameter affect performance? | `parameter_demo` |
| `eval_contention_weight_sensitivity` | Which score components drive contention-aware behavior? | `weight_sensitivity` |

Preset values can be overridden from the command line. Examples include
`--policies`, `--node-counts`, `--churn-fractions`, `--epoch-lengths`,
`--cache-hotset-multipliers`, and `--memory-channel-counts`. Use `--help` for
the complete list.

## Plotting And Reports

`scripts/plot_results.py` consumes one or more aggregate CSV files and produces
presentation-ready plots, derived comparison tables, and a Markdown report.

Install Matplotlib first:

```bash
python3 -m pip install -r requirements-analysis.txt
```

Analyze the focused policy study against LRU:

```bash
scripts/plot_results.py \
  --aggregate results/eval_policy_iteration/aggregate_summary.csv \
  --output-dir results/eval_policy_iteration/analysis \
  --baseline lru \
  --report-mode policy_viability
```

For a single recognized preset, the report mode can be inferred:

```bash
scripts/plot_results.py \
  --aggregate results/eval_policy_iteration/aggregate_summary.csv \
  --output-dir results/eval_policy_iteration/analysis \
  --baseline lru \
  --report-mode auto
```

Typical analysis outputs:

```text
analysis/
  report.md
  policy_comparison.csv
  policy_summary.csv
  condition_summary.csv
  plots/
    *.svg
```

SVG is the default format. Use `--formats svg,png` when both vector and raster
versions are useful.

### Final Report Evaluation

The final-report presets remove dimensions that are useful for exploration but
would obscure the paper's main comparisons. Dry-run counts below are per seed:

| Preset | Runs Per Seed | Purpose |
| --- | ---: | --- |
| `eval_final_contention_scaling` | 24 | Node-count and bandwidth pressure with other workload knobs fixed. |
| `eval_bursty_calibration` | 4 | Completion-driven versus scheduled-bursty arrivals. |
| `eval_final_node_bandwidth` | 30 | Latest-policy benefit at mild and severe bandwidth. |
| `eval_final_policy_iteration` | 108 | Latest policies across churn, cache pressure, and RPE. |

Run the final studies with matched seeds:

```bash
scripts/run_matrix.py \
  --preset eval_final_contention_scaling \
  --seeds 8888,1729,31415 \
  --results-dir results/final_contention_scaling

scripts/run_matrix.py \
  --preset eval_bursty_calibration \
  --seeds 8888,1729,31415 \
  --results-dir results/final_bursty_calibration

scripts/run_matrix.py \
  --preset eval_final_node_bandwidth \
  --seeds 8888,1729,31415 \
  --results-dir results/final_node_bandwidth

scripts/run_matrix.py \
  --preset eval_final_policy_iteration \
  --seeds 8888,1729,31415 \
  --results-dir results/final_policy_iteration
```

Combine the four aggregates into the compact paper bundle:

```bash
scripts/plot_results.py \
  --aggregate results/final_contention_scaling/aggregate_summary.csv \
  --aggregate results/final_bursty_calibration/aggregate_summary.csv \
  --aggregate results/final_node_bandwidth/aggregate_summary.csv \
  --aggregate results/final_policy_iteration/aggregate_summary.csv \
  --output-dir results/final_report_analysis \
  --baseline lru \
  --report-mode final_report
```

This mode writes four paper-oriented plots and `final_policy_table.csv`.
Uncertainty columns are the sample standard deviation of per-seed means after
matched conditions are averaged within each seed.

## Experiment Outputs

Each simulator run writes:

- `summary.json`: run-level latency, cache, memory, contention, fairness, and
  viability summaries
- `per_node.csv`: per-compute-node latency and cache behavior
- `latencies.csv`: one row per completed request
- `contention_by_object.csv`: per-epoch, per-object remote pressure
- `contention_by_channel.csv`: per-epoch memory-channel pressure
- `policy_diagnostics.csv`: contention-aware score and decision components
- `cache_admissions.csv`: admission, reuse, and eviction lifecycle records
- `epoch_diagnostics.csv`: telemetry predictiveness across epochs
- `viability_metrics.csv`: compact admission, reuse, staleness, and fairness
  diagnostics

The matrix runner extracts relevant values from these files into
`aggregate_summary.csv`. The plotting tool then performs matched comparisons
against the selected baseline.

## Design Boundaries And Limitations

The simulator intentionally abstracts away many production-system details:

- It models one shared memory node, not multiple memory servers with placement
  and routing.
- Memory channels are independent FIFO servers, not detailed banks, row
  buffers, or memory-controller scheduling.
- Requests are object-level reads rather than byte-addressed or page-based
  memory operations.
- Writes, coherence, invalidation, and consistency protocols are not modeled.
- Link latency and bandwidth are simplified rather than packet-level CXL or
  RDMA behavior.
- Workloads are synthetic rather than application traces.
- Contention telemetry uses bounded epoch summaries and approximate attribution
  rather than exact causal hardware monitoring.
- The optional shared near-fabric cache is not part of the active request path.

These boundaries keep experiments deterministic and make policy behavior easier
to explain. They also mean results should be presented as evidence about policy
and contention trends within the modeled architecture, not as direct hardware
performance predictions.

## Further Reading

- [Architecture](docs/architecture.md): current conceptual system structure
- [Implementation and Execution Guide](docs/implementation_execution_guide.md):
  detailed project handbook and execution workflow
- [Implementation Map](docs/implementation_map.md): concrete request and
  control-flow trace
- [Testing and Evaluation Plan](docs/testing_evaluation_plan.md): experiment
  phases, presets, metrics, and interpretation guidance

