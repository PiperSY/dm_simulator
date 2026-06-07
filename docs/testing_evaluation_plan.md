# Testing And Evaluation Plan

This document is the practical testing playbook for the final project stage.

The goal is not to tune the simulator until contention-aware caching always
wins. The goal is to show, with repeatable matrix experiments, when the
simulator creates meaningful remote-memory contention and when contention-aware
cache placement is a useful response to that contention.

## Research Framing

The evaluation has two main questions:

- Does the simulator create plausible shared remote-memory bottlenecks as
  architecture and workload pressure increase?
- Once those bottlenecks exist, which cache policies reduce latency, memory
  queueing, remote accesses, and unfairness?

Contention-aware caching should not be expected to win everywhere. It should be
most useful when remote memory is a real bottleneck, hot objects remain
predictive across epochs, private caches are constrained but still useful, and
coarse prior-epoch telemetry is not too stale. It should tie or lose when memory
is underloaded, hot sets churn too quickly, local reuse is sparse, or LRU already
captures nearly all useful locality.

This matches the conservative disaggregated-memory research framing used
throughout the project: shared-fabric and memory-device telemetry can be useful,
but it should be coarse, periodic, and evaluated against collection and staleness
limits rather than treated as perfect global knowledge.

## Parameter Importance Map

The matrix suite should vary parameters that explain either contention creation
or cache-policy usefulness. The table below is the decision guide for which
knobs matter most.

| Parameter | Why It Matters | Expected Contention Effect | Expected Policy Effect |
| --- | --- | --- | --- |
| `compute_node_count` | More compute nodes can miss concurrently against one memory node. | Higher node counts should increase queue depth and memory wait. | Contention-aware policies become more relevant as cross-node pressure grows. |
| `memory_bandwidth_bytes_per_time` | Determines serialization time for remote objects. | Lower bandwidth should increase service time and queueing. | Policies that avoid remote service should look better under severe bandwidth limits. |
| `memory_base_latency` | Adds fixed remote service cost independent of object size. | Higher base latency increases remote cost but may be less queue-sensitive than bandwidth. | All caching policies benefit more from hits when base latency is high. |
| `object_size_mode` | Fixed sizes make service costs uniform; bimodal sizes create expensive objects. | Large objects consume more memory service time under limited bandwidth. | Size/service-aware contention signals become more meaningful with bimodal sizes. |
| `cross_node_overlap` | Controls whether nodes request the same hot objects. | Higher overlap should concentrate remote misses on shared objects. | Contention-aware caching should help most when overlap creates shared pressure. |
| `hot_access_probability` | Controls concentration of accesses into the hot set. | Higher hot probability increases repeated pressure on hot objects. | Helps telemetry and caching when hot objects remain stable enough to exploit. |
| `hot_set_churn_fraction` | Controls how much the hot set changes between epochs. | High churn can keep remote traffic high but makes object-level telemetry stale. | Contention-aware policies should struggle at high churn and improve at low/partial churn. |
| `requests_per_node_per_epoch` | Controls epoch length and opportunity for reuse. | Longer epochs create more requests per telemetry period. | Longer epochs give policies more chance to benefit from stable hot objects. |
| `epoch_count` | Determines how many telemetry update cycles occur. | More epochs expose repeated queueing patterns. | Needed for previous-epoch policies to show learning or staleness effects. |
| `cache_capacity_hotset_multiplier` | Expresses cache size relative to hot-set footprint. | Does not create contention directly, but controls how many misses remain remote. | Too small causes thrashing; too large lets LRU win easily; middle values are most informative. |
| `hot_set_size` | Controls working-set pressure per node. | Larger hot sets can increase misses if cache capacity does not scale. | Larger hot sets make admission and eviction quality more important. |
| `object_count` | Sets the total object universe and cold-set size. | Larger cold sets reduce accidental reuse. | Should scale with hot-set size so cold accesses remain realistic. |
| `local_cache.policy` | Selects baseline or experimental cache behavior. | Indirectly changes remote traffic by changing hit/miss behavior. | Main comparison axis for policy viability. |
| `local_cache.hotness.history_mode` | Selects epoch-only, cumulative, or bounded-window hotness history. | No direct contention effect. | Windowed hotness is the realistic bounded-history baseline; cumulative and epoch modes are useful endpoints. |
| Contention variant | Selects v1, smoothed, reuse-gated, or hysteresis behavior. | No direct contention effect. | Tests which refinement handles stale telemetry, pollution, or eviction churn best. |
| Scoring weights | Tune the contention-aware scoring model. | No direct contention effect. | Leave broad weight tuning for later; first identify regimes where telemetry is useful. |

## Metrics To Prioritize

Use these metrics when deciding whether a result is meaningful enough for the
final report or presentation.

Simulator contention evidence:

- `average_memory_wait`
- `max_memory_wait`
- `peak_memory_queue_depth`
- `total_queue_wait`
- `total_remote_accesses`
- `total_remote_service_time`

Policy performance:

- `mean_latency`
- `p95_latency`
- `p99_latency`
- `local_hit_rate`
- `total_remote_accesses_delta_pct`
- `estimated_avoided_remote_accesses`
- `estimated_avoided_queue_wait`
- `estimated_avoided_remote_service_time`

Telemetry and placement quality:

- `stale_telemetry_rate`: one minus the overlap between previous-epoch
  contended objects and current-epoch requested objects, compared over the
  configured synthetic hot-set size.
- `average_top_object_overlap`
- `admission_yield`
- `reuse_after_admit_rate`
- `eviction_regret_count`
- `remote_eviction_regret_count`

Fairness:

- `jain_inverse_latency_fairness`
- `per_node_mean_latency_spread`
- `per_node_p99_latency_max`

Hit rate alone is not enough. A contention-aware policy can be interesting even
when hit rate is similar to LRU if it reduces remote queue wait, remote service
time, or tail latency.

## Phase 1: Matrix Harness Extension For Evaluation

Purpose:

- Add the matrix-runner flexibility needed for the final evaluation phases.
- Keep this phase implementation-focused and avoid running expensive
  simulations until the generated configurations are reviewed.

Work

- Extend `scripts/run_matrix.py` so evaluation presets can sweep:
  - `hot_set_size`
  - `hot_access_probability`
  - `object_count`
  - `epoch_count`
- Keep object-size distribution fields fixed at the preset level for now. Size
  variation remains available through the existing workload schema, but broader
  object-size sweeps are deferred until the first evaluation runs show whether
  they are needed.
- Keep existing presets working; later phases will add the named evaluation
  presets that use these sweep dimensions.
- Add dry-run CTest coverage for new evaluation presets.
- Ensure generated run names and aggregate rows include the new dimensions when
  they are swept.

User work:

- Run dry-runs before full simulation:
  ```bash
  scripts/run_matrix.py --preset <preset-name> --dry-run --results-dir results/<preset-name>_dry_run
  ```
- Review generated YAML files under:
  ```bash
  results/<preset-name>_dry_run/generated_configs/
  ```
- Confirm that run counts are manageable before launching full runs.

Acceptance criteria:

- Dry-runs produce the expected run counts.
- Existing presets still dry-run successfully.
- `hot_set_size`, `hot_access_probability`, `object_count`, and `epoch_count`
  appear in generated YAML and aggregate rows.

## Phase 2: Simulator Contention Calibration

Goal:

- Show that architecture and workload parameters create predictable
  remote-memory bottleneck behavior.

Proposed preset name:

- `eval_contention_calibration`

Policies:

- `always_remote`
- `lru`

Sweep values:

| Dimension | Values |
| --- | --- |
| `compute_node_count` | `2, 4, 8, 16` |
| `memory_bandwidth_level` | `mild, moderate, severe` |
| `cross_node_overlap` | `low, medium, high` |
| `hot_access_probability` | `0.6, 0.8, 0.95` |
| `cache_capacity_hotset_multiplier` | `0.2, 0.5, 0.8` |
| `hot_set_churn_fraction` | fixed `0.3` |
| `requests_per_node_per_epoch` | fixed `8` |
| `workload_issue_mode` | fixed `scheduled_bursty` |
| `burst_size` | fixed `4` |
| `burst_interval` | fixed `20` |
| `intra_burst_gap` | fixed `1` |
| `node_phase_jitter` | fixed `5` |
| `object_count` | fixed `1024` |
| `hot_set_size` | fixed `16` |
| `epoch_count` | fixed `32` |
| `object_size_mode` | fixed `bimodal` |
| `object_size_bytes` | fixed `64` |
| `object_size_small_bytes` | fixed `64` |
| `object_size_large_bytes` | fixed `256` |
| `large_object_probability` | fixed `0.1` |
| `memory_channel_count` | fixed `4` |
| `hot_object_channel_count` | fixed `2` |
| `memory_base_latency_level` | fixed `medium` |
| `link_latency_level` | fixed `medium` |
| seed | first pass `8888` |

Expected first-pass run count:

- `4 node counts * 3 bandwidths * 3 overlaps * 3 hot probabilities * 3 cache multipliers * 2 policies = 648`

Work

- Add the `eval_contention_calibration` preset.
- Add dry-run test coverage with expected run count `648`.
- Ensure the preset is designed to answer simulator-validity questions, not to
  favor a specific cache policy.

User run step:

```bash
scripts/run_matrix.py \
  --preset eval_contention_calibration \
  --results-dir results/eval_contention_calibration
```

User analysis step:

```bash
scripts/plot_results.py \
  --aggregate results/eval_contention_calibration/aggregate_summary.csv \
  --output-dir results/eval_contention_calibration/analysis \
  --baseline lru \
  --report-mode contention_calibration
```

Interpretation target:

- Increasing node count should increase memory pressure.
- Lower bandwidth should increase service time, queue wait, and tail latency.
- Higher overlap and higher hot-access probability should concentrate pressure
  on fewer objects.
- Scheduled-bursty issue should create overlapping requests and expose queue
  pressure that completion-driven request pacing can suppress.
- Four memory channels with hot objects restricted to two channels should
  preserve memory-side parallelism while creating a moderate local hotspot.
- The 1,024-object universe leaves 512 objects eligible for the two-channel
  hotspot, enough for 16 low-overlap hot sets plus partial-churn replacements.
- Bimodal object sizes should make remote service demand more realistic by
  mixing 64-byte and 256-byte transfers.
- If these trends are not visible, inspect workload generation and memory queue
  accounting before making policy claims.

Optional follow-up:

- Repeat the most informative configurations with two additional seeds after
  reviewing the first-pass plots.

## Phase 3: Policy Viability Matrix

Goal:

- Evaluate contention-aware policies inside a regime where contention is known
  to exist.

Proposed preset name:

- `eval_policy_viability`

Policies:

- `lru`
- `hotness_only_windowed`
- `global_hottest_replication`
- `contention_aware_v1`
- `contention_aware_smoothed`
- `contention_aware_reuse_gated`
- `contention_aware_hysteresis`

Sweep values:

| Dimension | Values |
| --- | --- |
| `hot_set_churn_fraction` | `0.1, 0.2, 0.3, 0.4, 0.5` |
| `cache_capacity_hotset_multiplier` | `0.2, 0.4, 0.6, 0.8` |
| `requests_per_node_per_epoch` | `4, 8` |
| `workload_issue_mode` | fixed `scheduled_bursty` |
| `burst_size` | fixed `4` |
| `burst_interval` | fixed `20` |
| `intra_burst_gap` | fixed `1` |
| `node_phase_jitter` | fixed `5` |
| `cross_node_overlap` | fixed `high` |
| `compute_node_count` | fixed `16` |
| `memory_bandwidth_level` | fixed `severe` |
| `hot_access_probability` | fixed `0.8` |
| `object_count` | fixed `256` |
| `hot_set_size` | fixed `16` |
| `epoch_count` | fixed `32` |
| `object_size_mode` | fixed `bimodal` |
| `object_size_bytes` | fixed `64` |
| `object_size_small_bytes` | fixed `64` |
| `object_size_large_bytes` | fixed `256` |
| `large_object_probability` | fixed `0.1` |
| `memory_channel_count` | fixed `4` |
| `hot_object_channel_count` | fixed `2` |
| `memory_base_latency_level` | fixed `medium` |
| `link_latency_level` | fixed `medium` |
| seed | first pass `8888` |

Expected first-pass run count:

- `7 policies * 5 churn values * 4 cache multipliers * 2 requests-per-epoch values = 280`

Work

- Add the `eval_policy_viability` preset.
- Add dry-run test coverage with expected run count `280`.
- Ensure all policies use matched architecture and workload settings.
- Preserve `lru` as the default analysis baseline.

User run step:

```bash
scripts/run_matrix.py \
  --preset eval_policy_viability \
  --results-dir results/eval_policy_viability
```

User analysis step:

```bash
scripts/plot_results.py \
  --aggregate results/eval_policy_viability/aggregate_summary.csv \
  --output-dir results/eval_policy_viability/analysis \
  --baseline lru \
  --report-mode policy_viability
```

Interpretation target:

- Contention-aware variants should do best when churn is low or partial and
  epochs are long enough for telemetry to remain predictive.
- LRU, cumulative hotness, or windowed hotness should often do better when hot
  sets change quickly or local reuse dominates.
- Global hottest replication acts as an oracle-style upper reference, not a
  deployable baseline.
- `stale_telemetry_rate`, `admission_yield`, and `reuse_after_admit_rate` should
  explain many wins and losses. Interpret stale telemetry as a hot-set-sized
  predictiveness metric, not a fixed-width object match.

Optional follow-up:

- Repeat only the most important churn/epoch/cache combinations with additional
  seeds.
- Probe the softened composite policy and optional mild hysteresis without
  changing the preset's default policy set:

```bash
scripts/run_matrix.py \
  --preset eval_policy_viability \
  --policies lru,hotness_only_windowed,contention_aware_smoothed,contention_aware_smoothed_reuse_gated,contention_aware_smoothed_reuse_gated_hysteresis,contention_aware_reuse_gated,contention_aware_hysteresis \
  --results-dir results/eval_policy_viability_composite
```

The base composite alias uses bounded confirmation with a high-score bypass.
The `_hysteresis` alias adds a mild eviction margin so its incremental effect
can be evaluated separately.

### Focused Policy Iteration Study

The `eval_policy_iteration` preset provides a smaller follow-up to the broad
viability matrix. It compares the current policy iteration without mixing in
the older contention variants or the oracle-style replication baseline.

Policies:

- `lru`
- `hotness_only_windowed`
- `contention_aware_smoothed`
- `contention_aware_smoothed_reuse_gated`
- `contention_aware_smoothed_reuse_gated_hysteresis`
- `contention_aware_size_value`

Focused sweeps:

| Dimension | Values |
| --- | --- |
| `hot_set_churn_fraction` | `0.1, 0.3, 0.5` |
| `cache_capacity_hotset_multiplier` | `0.2, 0.4, 0.6` |
| `requests_per_node_per_epoch` | `4, 16` |

All other architecture and workload values match the broad viability regime:
16 compute nodes, 32 epochs, hot-set size 16, severe memory bandwidth, four
memory channels, two hot-object channels, high overlap, and bimodal objects
with large-object probability `0.1`.

Expected run count:

- `6 policies * 3 churn values * 3 cache multipliers * 2 requests-per-epoch values = 108`

Run and analyze:

```bash
scripts/run_matrix.py \
  --preset eval_policy_iteration \
  --results-dir results/eval_policy_iteration

scripts/plot_results.py \
  --aggregate results/eval_policy_iteration/aggregate_summary.csv \
  --output-dir results/eval_policy_iteration/analysis \
  --baseline lru \
  --report-mode policy_viability
```

This study should be interpreted as a comparison of the current design
directions: smoothing alone, softened confirmation, optional mild hysteresis,
and cost-density scoring. LRU remains the numeric baseline, while windowed
hotness remains the practical non-contention-aware comparison.

## Phase 4: Focused Interaction Studies

Goal:

- Create smaller, presentation-friendly studies that isolate the clearest
  parameter interactions.

Proposed preset family:

- `eval_interactions_churn_epoch`
- `eval_interactions_cache_hotset`
- `eval_interactions_node_bandwidth`

Common policies:

- `lru`
- `hotness_only_windowed`
- `contention_aware_smoothed`
- `contention_aware_reuse_gated`
- `contention_aware_hysteresis`

Common defaults:

- `object_count`: `256`
- `epoch_count`: `32`
- `hot_set_size`: `16` unless the study sweeps it
- `hot_access_probability`: `0.8`
- `object_size_mode`: `bimodal`
- `object_size_bytes`: `64`
- `object_size_small_bytes`: `64`
- `object_size_large_bytes`: `256`
- `large_object_probability`: `0.1`
- `memory_base_latency_level`: `medium`
- `link_latency_level`: `medium`

Channel settings are preset-specific in the implemented runner. The
churn-by-epoch study uses the default single-channel memory model with
unrestricted hot-object channel selection. The cache-by-hot-set and
node-by-bandwidth studies use `memory_channel_count = 4` and
`hot_object_channel_count = 2` so they run in the same channel-aware hotspot
regime used by the current manual and viability probes.

### Interaction 1: Churn By Epoch Length

Sweep values:

- `hot_set_churn_fraction`: `0.0, 0.25, 0.5, 0.75, 1.0`
- `requests_per_node_per_epoch`: `8, 16, 32, 64`
- fixed `compute_node_count`: `8`
- fixed `memory_bandwidth_level`: `severe`
- fixed `cache_capacity_hotset_multiplier`: `0.5`
- fixed `cross_node_overlap`: `high`
- fixed `memory_channel_count`: `1`
- fixed `hot_object_channel_count`: `0`

Expected first-pass run count:

- `5 policies * 5 churn values * 4 epoch lengths = 100`

Interpretation target:

- Show how telemetry usefulness changes as hot-set stability and epoch length
  change together.

### Interaction 2: Cache Capacity By Hot-Set Size

Sweep values:

- `cache_capacity_hotset_multiplier`: `0.2, 0.4, 0.6, 0.8`
- `hot_set_size`: `8, 16, 32`
- fixed `compute_node_count`: `8`
- fixed `memory_bandwidth_level`: `severe`
- fixed `hot_set_churn_fraction`: `0.3`
- fixed `requests_per_node_per_epoch`: `4`
- fixed `cross_node_overlap`: `high`
- fixed `memory_channel_count`: `4`
- fixed `hot_object_channel_count`: `2`

Expected first-pass run count:

- `5 policies * 4 cache multipliers * 3 hot-set sizes = 60`

Interpretation target:

- Show where cache capacity is too small, useful, or large enough that LRU is
  already sufficient.

### Interaction 3: Node Count By Memory Bandwidth

Sweep values:

- `compute_node_count`: `4, 8, 16`
- `memory_bandwidth_level`: `mild, moderate, severe`
- fixed `cache_capacity_hotset_multiplier`: `0.5`
- fixed `hot_set_churn_fraction`: `0.3`
- fixed `requests_per_node_per_epoch`: `4`
- fixed `cross_node_overlap`: `high`
- fixed `memory_channel_count`: `4`
- fixed `hot_object_channel_count`: `2`

Expected first-pass run count:

- `5 policies * 3 node counts * 3 bandwidth levels = 45`

Interpretation target:

- Show where the architecture crosses from lightly loaded to meaningfully
  contended.

Work

- Add the three interaction presets or one preset mode that can generate all
  three intentionally.
- Add dry-run tests for expected run counts.
- Ensure `plot_results.py` can consume each aggregate independently and all
  aggregates together.

User run steps:

```bash
scripts/run_matrix.py \
  --preset eval_interactions_churn_epoch \
  --results-dir results/eval_interactions_churn_epoch

scripts/run_matrix.py \
  --preset eval_interactions_cache_hotset \
  --results-dir results/eval_interactions_cache_hotset

scripts/run_matrix.py \
  --preset eval_interactions_node_bandwidth \
  --results-dir results/eval_interactions_node_bandwidth
```

User analysis steps:

```bash
scripts/plot_results.py \
  --aggregate results/eval_interactions_churn_epoch/aggregate_summary.csv \
  --output-dir results/eval_interactions_churn_epoch/analysis \
  --baseline lru \
  --report-mode interaction

scripts/plot_results.py \
  --aggregate results/eval_interactions_cache_hotset/aggregate_summary.csv \
  --output-dir results/eval_interactions_cache_hotset/analysis \
  --baseline lru \
  --report-mode interaction

scripts/plot_results.py \
  --aggregate results/eval_interactions_node_bandwidth/aggregate_summary.csv \
  --output-dir results/eval_interactions_node_bandwidth/analysis \
  --baseline lru \
  --report-mode interaction
```

Combined analysis step:

```bash
scripts/plot_results.py \
  --aggregate results/eval_interactions_churn_epoch/aggregate_summary.csv \
  --aggregate results/eval_interactions_cache_hotset/aggregate_summary.csv \
  --aggregate results/eval_interactions_node_bandwidth/aggregate_summary.csv \
  --output-dir results/eval_interactions_combined/analysis \
  --baseline lru
```

## Phase 4B: Isolated Parameter Demonstrations

Goal:

- Create smaller, presentation-friendly studies that show how individual
  configuration knobs affect system and policy behavior.
- Explain why configuration matters without replacing the broader policy
  viability matrix or the two-dimensional interaction studies.

Default policy trio:

- `lru`
- `hotness_only_windowed`
- `contention_aware_smoothed`

These studies intentionally use fewer policies than the full viability matrix so
plots remain readable. They should be used to explain mechanisms, not to claim a
complete policy ranking.

Fixed default regime unless a study says otherwise:

| Dimension | Value |
| --- | --- |
| `compute_node_count` | `8` |
| `memory_bandwidth_level` | `severe` |
| `memory_base_latency_level` | `medium` |
| `link_latency_level` | `medium` |
| `cross_node_overlap` | `high` |
| `hot_set_churn_fraction` | `0.2` |
| `hot_set_size` | `16` |
| `object_count` | `256` |
| `epoch_count` | `32` |
| `object_size_mode` | `bimodal` |
| `object_size_small_bytes` | `64` |
| `object_size_large_bytes` | `256` |
| `cache_capacity_hotset_multiplier` | `0.5` |
| `requests_per_node_per_epoch` | `8` |
| `hot_access_probability` | `0.8` |
| `large_object_probability` | `0.1` |
| `memory_channel_count` | `1` unless a study notes otherwise |
| `hot_object_channel_count` | `0` unless a study notes otherwise |
| seed | first pass `8888` |

### Knob Study 1: Cache Pressure

Proposed preset name:

- `eval_knob_cache_pressure`

Sweep values:

- `cache_capacity_hotset_multiplier`:
  `0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.8, 1.0`
- fixed `memory_channel_count`: `4`
- fixed `hot_object_channel_count`: `2`

Expected first-pass run count:

- `3 policies * 8 values = 24`

Interpretation target:

- Show where caches are too small, useful, or large enough that LRU/hotness
  baselines become strong.
- Demonstrate that the interesting policy-comparison region is usually the
  middle of cache pressure, not the extremes.

Expected plots:

- Mean latency delta versus LRU by cache multiplier.
- Local hit rate by cache multiplier.
- Average memory wait and total remote accesses by cache multiplier.
- Admission yield and reuse-after-admit rate when available.

### Knob Study 2: Epoch Reuse

Proposed preset name:

- `eval_knob_epoch_reuse`

Sweep values:

- `requests_per_node_per_epoch`: `4, 8, 16, 32, 64, 128`

Expected first-pass run count:

- `3 policies * 6 values = 18`

Interpretation target:

- Show how increasing per-epoch reuse changes policy behavior.
- Hotness-based policies should generally benefit from more repeated local
  demand within an epoch.
- Contention-aware policy may not improve monotonically if prior-epoch
  telemetry is less important than immediate local reuse.

Expected plots:

- Mean and P99 latency versus requests per node per epoch.
- Hit rate versus requests per node per epoch.
- Admission yield and reuse-after-admit rate versus requests per node per
  epoch.

### Knob Study 3: Object Size Mix

Proposed preset name:

- `eval_knob_object_size_mix`

Sweep values:

- `large_object_probability`: `0.0, 0.05, 0.1, 0.2, 0.3`
- fixed `object_size_mode`: `bimodal`

Expected first-pass run count:

- `3 policies * 5 values = 15`

Interpretation target:

- Show that fixed or near-fixed object costs make hotness/recency baselines
  harder to beat.
- Show whether contention-aware placement improves when a small number of large
  objects create high remote service cost and stronger contention signals.
- Avoid assuming that more large objects always helps contention-aware caching:
  too many large objects can increase cache pressure and reduce selectivity.

Expected plots:

- Mean latency delta versus LRU by large-object probability.
- Hit rate and average memory wait by large-object probability.
- Estimated avoided remote service time by large-object probability.
- Admission yield and eviction regret by large-object probability.

Optional size-value probe:

- `contention_aware_size_value` is an opt-in matrix policy alias for the
  smoothed contention-aware policy with cost-density scoring enabled.
- Use it when you want to test whether previous-epoch remote cost per cache
  byte improves large-object treatment:

```bash
scripts/run_matrix.py \
  --preset eval_knob_object_size_mix \
  --policies lru,hotness_only_windowed,contention_aware_smoothed,contention_aware_size_value \
  --results-dir results/eval_knob_object_size_mix_size_value
```

### Knob Study 4: Hot-Access Concentration

Proposed preset name:

- `eval_knob_hot_concentration`

Sweep values:

- `hot_access_probability`: `0.5, 0.6, 0.7, 0.8, 0.9, 0.95`

Expected first-pass run count:

- `3 policies * 6 values = 18`

Interpretation target:

- Show how stronger hot-set concentration improves cacheability.
- Demonstrate where LRU/hotness already capture most locality and where
  contention-aware needs additional service-cost or queue-pressure signal to
  add value.

Expected plots:

- Mean latency delta versus LRU by hot-access probability.
- Hit rate by hot-access probability.
- Average memory wait and remote accesses by hot-access probability.
- Admission yield and reuse-after-admit rate by hot-access probability.

Work

- Add the four `eval_knob_*` presets in `scripts/run_matrix.py`.
- Add support for sweeping `large_object_probability`; the other required
  sweep fields already exist.
- Add dry-run tests for expected run counts:
  - `eval_knob_cache_pressure`: `24`
  - `eval_knob_epoch_reuse`: `18`
  - `eval_knob_object_size_mix`: `15`
  - `eval_knob_hot_concentration`: `18`
- Extend `plot_results.py` with a `parameter_demo` report mode that is
  auto-detected from presets beginning with `eval_knob_`.
- Add single-knob line plots with panels for latency delta, hit rate, memory
  wait, remote accesses, admission yield, and reuse-after-admit rate.
- Add report guidance that these plots are explanatory slices, not complete
  policy viability matrices.

User run steps:

```bash
scripts/run_matrix.py \
  --preset eval_knob_cache_pressure \
  --results-dir results/eval_knob_cache_pressure

scripts/run_matrix.py \
  --preset eval_knob_epoch_reuse \
  --results-dir results/eval_knob_epoch_reuse

scripts/run_matrix.py \
  --preset eval_knob_object_size_mix \
  --results-dir results/eval_knob_object_size_mix

scripts/run_matrix.py \
  --preset eval_knob_hot_concentration \
  --results-dir results/eval_knob_hot_concentration
```

User analysis pattern:

```bash
scripts/plot_results.py \
  --aggregate results/<preset-name>/aggregate_summary.csv \
  --output-dir results/<preset-name>/analysis \
  --baseline lru \
  --report-mode parameter_demo
```

## Phase 4C: Contention-Aware Weight Sensitivity

Goal:

- Explain which contention-aware score components matter most in a reasonable
  workload/architecture regime.
- Keep weight testing interpretable by sweeping one weight at a time while all
  other weights remain at their defaults.

Proposed preset name:

- `eval_contention_weight_sensitivity`

Fixed regime:

- Use the same fixed defaults as Phase 4B.
- Policy under test: `contention_aware_smoothed`.
- Baseline policy for analysis: `lru`.

One-at-a-time weight sweeps:

| Weight | Values |
| --- | --- |
| `local_hotness_weight` | `0.0, 0.5, 1.0, 2.0` |
| `remote_access_weight` | `0.0, 0.5, 1.0, 2.0` |
| `distinct_requester_weight` | `0.0, 1.0, 1.5, 3.0` |
| `queue_wait_weight` | `0.0, 1.0, 2.0, 4.0` |
| `remote_service_time_weight` | `0.0, 0.5, 1.0, 2.0` |
| `size_penalty_weight` | `0.0, 0.25, 0.5, 1.0` |

Expected first-pass run count:

- `24` contention-aware runs plus one shared matched `lru` baseline run,
  for `25` total runs.

Interpretation target:

- Show whether the contention-aware policy is mainly driven by local hotness,
  global remote accesses, cross-node requester breadth, queue pressure, service
  time, or cache-size penalty.
- Identify unreasonable weight regions where the policy over-admits,
  under-admits, or produces high eviction regret.
- Avoid presenting broad weight tuning as proof of superiority; use it to
  explain policy sensitivity.

Work

- Extend `scripts/run_matrix.py` with generated contention-weight profiles.
- Add aggregate columns such as:
  - `contention_weight_profile`
  - `contention_weight_name`
  - `contention_weight_value`
- Ensure generated YAML changes only the active weight while preserving all
  other default contention weights.
- Add dry-run test coverage for expected profile count.
- Extend `plot_results.py` with weight-sensitivity plots.

Expected plots:

- Mean latency delta versus LRU by weight value.
- Hit rate by weight value.
- Average memory wait by weight value.
- Admission yield by weight value.
- Eviction regret by weight value.

User run step:

```bash
scripts/run_matrix.py \
  --preset eval_contention_weight_sensitivity \
  --results-dir results/eval_contention_weight_sensitivity
```

User analysis step:

```bash
scripts/plot_results.py \
  --aggregate results/eval_contention_weight_sensitivity/aggregate_summary.csv \
  --output-dir results/eval_contention_weight_sensitivity/analysis \
  --baseline lru \
  --report-mode weight_sensitivity
```

## Phase 4D: Memory Channel Calibration And Hotspots

Goal:

- Replace the most artificial part of the original memory model, a single
  global FIFO, with a configurable one-memory-node/multiple-channel model.
- Show how memory-side parallelism changes queue pressure.
- Show that contention can still be localized when hot objects map to a smaller
  subset of memory channels.

New simulator knobs:

| Knob | Meaning |
| --- | --- |
| `memory.channel_count` | Number of independent FIFO memory-channel servers inside the single memory node. |
| `workload.hot_object_channel_count` | Number of channels eligible for hot-object selection; `0` means unrestricted. |

The first knob changes memory parallelism. The second knob is an explicit
workload control for creating channel-local hotspots. It is not a placement
policy and does not restrict cold objects.

### Channel Study 1: Channel Count Calibration

Proposed preset name:

- `eval_channel_calibration`

Purpose:

- Show how increasing memory-channel count reduces artificial global queueing
  when hot objects are not intentionally concentrated onto a subset of channels.

Policies:

- `always_remote`
- `lru`

Sweep values:

- `memory_channel_count`: `1, 2, 4, 8`
- fixed `hot_object_channel_count`: `0`

Expected first-pass run count:

- `2 policies * 4 channel counts = 8`

Interpretation target:

- Average memory wait and total queue wait should generally decrease as channel
  count increases, if requests spread across channels.
- A single-channel run should match the earlier FIFO behavior.
- If channel count increases but queue wait does not change, inspect whether
  the workload maps most hot objects to the same channel.

### Channel Study 2: Channel-Local Hotspots

Proposed preset name:

- `eval_channel_hotspots`

Purpose:

- Hold memory parallelism fixed and deliberately concentrate hot objects onto
  fewer channels to show resource-local contention.

Policies:

- `lru`
- `hotness_only_windowed`
- `contention_aware_smoothed`

Sweep values:

- fixed `memory_channel_count`: `4`
- `hot_object_channel_count`: `1, 2, 4`

Expected first-pass run count:

- `3 policies * 3 hotspot settings = 9`

Interpretation target:

- `hot_object_channel_count = 1` should create the strongest channel-local
  queue buildup.
- `hot_object_channel_count = 4` should distribute hot objects across all
  channels and reduce peak channel pressure.
- Channel imbalance should explain why average global memory wait can hide
  localized contention.

Work

- Add the channel config fields and memory-node channel queues.
- Add object-to-channel mapping and hotspot-aware workload generation.
- Add per-channel contention telemetry and output files.
- Add the two matrix presets and dry-run tests.
- Extend plotting so channel columns produce channel-count and hotspot plots.

User run steps:

```bash
scripts/run_matrix.py \
  --preset eval_channel_calibration \
  --results-dir results/eval_channel_calibration

scripts/run_matrix.py \
  --preset eval_channel_hotspots \
  --results-dir results/eval_channel_hotspots
```

User analysis steps:

```bash
scripts/plot_results.py \
  --aggregate results/eval_channel_calibration/aggregate_summary.csv \
  --output-dir results/eval_channel_calibration/analysis \
  --baseline lru \
  --report-mode contention_calibration

scripts/plot_results.py \
  --aggregate results/eval_channel_hotspots/aggregate_summary.csv \
  --output-dir results/eval_channel_hotspots/analysis \
  --baseline lru \
  --report-mode contention_calibration
```

Deferred channel work:

- Multiple memory nodes.
- Detailed bank, row-buffer, or memory-controller scheduling.
- Dynamic placement or routing based on channel state.
- A larger `eval_channel_policy_viability` preset after the first two channel
  studies validate the model.

## Phase 4E: Bursty Workload Calibration

Goal:

- Add a more realistic request-arrival model without changing object selection,
  cache policy decisions, or memory-channel mapping.
- Show that scheduled bursts can create sharper queue pressure and tail latency
  than the original completion-driven issue model.

New simulator knobs:

| Knob | Meaning |
| --- | --- |
| `workload.issue_mode` | `completion_driven` keeps the historical response-paced model; `scheduled_bursty` uses planned per-node arrivals. |
| `workload.burst_size` | Number of requests in one scheduled burst. |
| `workload.burst_interval` | Time between burst starts. |
| `workload.intra_burst_gap` | Time between requests inside one burst. |
| `workload.node_phase_jitter` | Deterministic per-node/epoch timing jitter range. |

Implementation note:

- The simulator releases only the first request for each node in an epoch.
- In `scheduled_bursty`, each `ComputeNode` schedules its own next same-epoch
  `GenerateRequest` by the next request's scheduled offset.
- `RequestComplete` continues to drive future requests only in
  `completion_driven` mode.

Proposed preset name:

- `eval_bursty_calibration`

Policies:

- `always_remote`
- `lru`

Sweep values:

- `workload_issue_mode`: `completion_driven`, `scheduled_bursty`
- fixed `burst_size`: `4`
- fixed `burst_interval`: `20`
- fixed `intra_burst_gap`: `1`
- fixed `node_phase_jitter`: `0`
- fixed `memory_channel_count`: `4`
- fixed `hot_object_channel_count`: `2`

Expected first-pass run count:

- `2 policies * 2 issue modes = 4`

Interpretation target:

- `scheduled_bursty` should increase average memory wait, total queue wait,
  peak channel queue depth, or P99 latency under matched workload/object
  settings.
- If bursty mode does not increase pressure, inspect whether burst spacing is
  too wide, cache capacity is too generous, or memory channels are absorbing
  the arrival spikes.

User run step:

```bash
scripts/run_matrix.py \
  --preset eval_bursty_calibration \
  --results-dir results/eval_bursty_calibration
```

User analysis step:

```bash
scripts/plot_results.py \
  --aggregate results/eval_bursty_calibration/aggregate_summary.csv \
  --output-dir results/eval_bursty_calibration/analysis \
  --baseline lru \
  --report-mode contention_calibration
```

## Phase 5: Final Report And Presentation Synthesis

Goal:

- Produce a compact, multi-seed evidence package for the final paper.
- Separate simulator-validity evidence from policy-performance and
  policy-mechanism evidence.
- Preserve churn, cache pressure, and RPE in the policy figures rather than
  averaging predictive and stale-telemetry regimes together.

Final presets:

| Preset | Policies | Runs Per Seed | Main Question |
| --- | --- | ---: | --- |
| `eval_final_contention_scaling` | always-remote, LRU | 24 | Do node count and bandwidth create predictable queue pressure? |
| `eval_bursty_calibration` | always-remote, LRU | 4 | Do overlapping arrivals create sharper queues than response pacing? |
| `eval_final_node_bandwidth` | LRU, windowed hotness, smoothed, hybrid hysteresis, size-value | 30 | Does contention-aware benefit increase with shared-memory pressure? |
| `eval_final_policy_iteration` | LRU, windowed hotness, smoothed, both hybrids, size-value | 108 | Where do the latest policy designs work, and why? |

Use the matched seeds `8888,1729,31415`. The final plots should report paired
policy deltas against LRU and variation across seed-level means rather than
treating every matrix cell as an independent replicate.

Final figure bundle:

1. `final_simulator_contention.svg`: node/bandwidth queue scaling plus the
   completion-driven versus scheduled-bursty comparison.
2. `final_pressure_policy_scaling.svg`: latency improvement and memory-wait
   reduction versus node count at mild and severe bandwidth.
3. `final_policy_operating_region.svg`: cache-pressure response conditioned on
   churn and requests per node per epoch.
4. `final_policy_mechanisms.svg`: remote-access reduction, memory-wait
   reduction, admission yield, and reuse after admission versus churn.

The report also writes `final_policy_table.csv` with mean latency improvement,
P99 improvement, memory-wait reduction, hit rate, win rate, and sample standard
deviation across per-seed means.

Run step:

```bash
scripts/run_matrix.py --preset eval_final_contention_scaling \
  --seeds 8888,1729,31415 \
  --results-dir results/final_contention_scaling
scripts/run_matrix.py --preset eval_bursty_calibration \
  --seeds 8888,1729,31415 \
  --results-dir results/final_bursty_calibration
scripts/run_matrix.py --preset eval_final_node_bandwidth \
  --seeds 8888,1729,31415 \
  --results-dir results/final_node_bandwidth
scripts/run_matrix.py --preset eval_final_policy_iteration \
  --seeds 8888,1729,31415 \
  --results-dir results/final_policy_iteration
```

Analysis step:

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

Keep broad scatter plots, best-run examples, weight sensitivity, fairness
figures, oracle replication, and detailed channel/object diagnostics in an
appendix unless the final narrative specifically depends on them.

## Command Reference

Dry-run pattern:

```bash
scripts/run_matrix.py \
  --preset <preset-name> \
  --dry-run \
  --results-dir results/<preset-name>_dry_run
```

Run pattern:

```bash
scripts/run_matrix.py \
  --preset <preset-name> \
  --results-dir results/<preset-name>
```

Analysis pattern:

```bash
scripts/plot_results.py \
  --aggregate results/<preset-name>/aggregate_summary.csv \
  --output-dir results/<preset-name>/analysis \
  --baseline lru
```

Combined report pattern:

```bash
scripts/plot_results.py \
  --aggregate results/eval_contention_calibration/aggregate_summary.csv \
  --aggregate results/eval_policy_viability/aggregate_summary.csv \
  --output-dir results/final_combined_analysis \
  --baseline lru
```

Analysis dependency setup:

```bash
python3 -m pip install -r requirements-analysis.txt
```

## Acceptance Checklist

Before running an expensive matrix:

- Dry-run completes successfully.
- Expected run count matches the plan.
- Generated YAMLs contain the intended policies and sweep values.
- Result directory names clearly encode key parameters.

After running a matrix:

- `aggregate_summary.csv` exists.
- `scripts/plot_results.py` generates `report.md`, `policy_summary.csv`,
  `policy_comparison.csv`, `condition_summary.csv`, and plot files.
- The analysis distinguishes simulator contention evidence from policy
  performance evidence.
- Optional multi-seed reruns are reserved for the most important narrowed
  scenarios, not every exploratory matrix.
