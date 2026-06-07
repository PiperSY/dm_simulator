# Implementation And Execution Guide

This guide is a medium-level handbook for understanding the simulator as a
working research artifact. It is written for mixed readers: project authors,
grad-course reviewers, developers, and LLMs that need enough context to help
with reports or presentation slides.

Use this document when you want the "what is this project and how do I run it?"
view. For deeper references:

- `docs/architecture.md` describes the intended architecture and design
  decomposition.
- `docs/implementation_map.md` traces the current implemented control flow.
- `docs/testing_evaluation_plan.md` describes the evaluation strategy and
  experiment presets.

## Project Motivation

The simulator studies cache placement in a simplified disaggregated-memory
system. The core research question is whether private local caches at compute
nodes can make better admission and eviction decisions when they are informed by
coarse contention telemetry from a shared remote memory node.

The motivation is that hotness alone is not always enough in disaggregated
memory. An object may matter because many nodes request it, because it consumes
large remote service time, or because its requests suffer high queue wait at a
shared bottleneck. The project therefore compares traditional baselines such as
LRU and hotness-only caching against contention-aware policies that use
previous-epoch summaries of remote-memory pressure.

The simulator is intentionally not a full CXL, RDMA, or operating-system memory
stack. It is a deterministic policy-study simulator. That choice keeps the
system transparent enough to reason about policy behavior, run controlled
matrices, and explain when contention-aware caching helps or fails.

## Simulator At A Glance

The current implementation is a single-process, deterministic, object-level
simulator. It models enough of a disaggregated-memory system to study private
cache admission and eviction under shared remote-memory contention, while
avoiding protocol details that would obscure the policy question.

The active architecture has these layers:

| Layer | What It Represents | Main Responsibility |
| --- | --- | --- |
| Config and runner | YAML files, `ExperimentRunner`, matrix scripts | Convert experiment settings into a complete simulation run. |
| Simulation core | `Simulator`, request table, response handling | Own nodes, global state, epoch release, and run completion. |
| Event engine | `Scheduler` | Execute deterministic simulated-time events in timestamp order. |
| Compute side | `ComputeNode`, `WorkloadCursor` | Issue per-node request streams and receive responses. |
| Local cache side | `LocalCache`, `CachePolicy` implementations | Serve hits and decide admission/eviction after misses return. |
| Remote memory side | `MemoryNode` | Model one shared memory node with configurable per-channel FIFO service queues. |
| Measurement side | `Stats` | Record latency, cache behavior, contention, diagnostics, and summaries. |
| Analysis side | result files, `run_matrix.py`, `plot_results.py` | Aggregate runs and generate comparison plots/reports. |

This means the simulator is not trying to be a full CXL/RDMA stack. It is a
controlled experimental pipeline: generate requests, route them through local
caches and one shared remote-memory bottleneck, collect metrics, then compare
policies under matched conditions.

The main conceptual flow is:

```text
YAML config
  -> ExperimentRunner
  -> Simulator
  -> Scheduler
  -> ComputeNode + LocalCache + CachePolicy
  -> MemoryNode channel FIFO(s)
  -> Stats
  -> per-run result files
  -> run_matrix.py aggregate_summary.csv
  -> plot_results.py report.md + plots
```

The main component ownership model is:

| Component | Owns Or Coordinates | Does Not Own |
| --- | --- | --- |
| `Simulator` | Nodes, scheduler, global request/response records, epoch barrier, shared `Stats`. | Policy scoring details or generated workload logic. |
| `Scheduler` | Event ordering by simulated time and deterministic tie-breaks. | Cache, memory, or workload semantics. |
| `ComputeNode` | Per-node request cursor, local lookup path, remote miss forwarding, response completion. | Remote service queueing. |
| `LocalCache` | Resident cached entries, capacity checks, admission lifecycle diagnostics. | The policy-specific scoring model. |
| `CachePolicy` | Admission and victim-selection strategy. | Direct memory-node scheduling or global orchestration. |
| `MemoryNode` | Remote channel queue depth, service start/completion, object-level and channel-level contention counters. | Local cache contents. |
| `Stats` | Run-level metrics and diagnostic records. | Simulation control flow. |

## Execution Model

The simulator is discrete-event based. It does not use wall-clock sleeps,
threads, real packets, or real memory accesses. Instead, event handlers schedule
future events at simulated timestamps.

One complete run can be read as a pipeline:

1. YAML is loaded into experiment and simulation configuration.
2. A synthetic workload is generated, or explicit per-node request streams are
   used directly.
3. The `Simulator` constructs compute nodes, local caches, cache policies, the
   memory node, global stats, and the scheduler.
4. The scheduler releases requests for the current epoch and dispatches events
   until that epoch drains. In completion-driven workloads, each node issues the
   next request after the prior request completes; in scheduled-bursty
   workloads, each node schedules the next same-epoch arrival by planned issue
   time.
5. Each request either hits locally or misses to remote memory.
6. Remote requests queue at the mapped memory channel and consume simulated service time.
7. Responses return to compute nodes, where local-cache admission and eviction
   may happen.
8. `Stats` records the request outcome, cache effects, contention telemetry,
   and policy diagnostics.
9. The experiment runner writes per-run output files after simulation finishes.

At a request level, the active path is:

```text
GenerateRequest
  -> LocalCacheLookup
  -> LocalCacheHitComplete        if local hit
  -> ForwardToMemory              if local miss
  -> MemoryServiceStart
  -> MemoryServiceComplete
  -> ReturnResponse
  -> cache admission decision
  -> RequestComplete
```

The same lifecycle, mapped to architecture, looks like this:

| Stage | Primary Component | What Happens | Metrics Or Telemetry Affected |
| --- | --- | --- | --- |
| Generate request | `ComputeNode` | Pulls the next request from that node's workload cursor. | Request start time and epoch tracking. |
| Local lookup | `LocalCache` | Checks whether the object is resident in the private cache. | Cache hit/miss counters, policy lookup observations. |
| Hit completion | `ComputeNode` / `LocalCache` | Pays local hit latency and completes without remote memory. | Latency, local hits, placement reuse diagnostics. |
| Forward miss | `ComputeNode` | Sends a miss toward the shared memory node. | Remote access path begins. |
| Memory enqueue/start | `MemoryNode` | Request maps to a memory channel, enters that channel's FIFO, and later begins service. | Queue depth, per-channel queue depth, queue wait, object and channel contention. |
| Memory complete | `MemoryNode` | Service time is charged using object size and memory bandwidth. | Bytes served, remote service time, total remote pressure. |
| Return response | `ComputeNode` | Response travels back to the requester. | End-to-end latency path. |
| Admission/eviction | `LocalCache` / `CachePolicy` | Policy decides whether to cache the object and what to evict. | Admission records, policy diagnostics, eviction regret inputs. |
| Request complete | `Simulator` / `Stats` | Final response is recorded and future work may be released. | Latency CSVs, summaries, per-node metrics. |

The local hit path pays only local cache latency. The miss path pays link
latency to memory, maps the object to a memory channel, waits in that channel's
queue if needed, consumes memory service time based on object size and
per-channel bandwidth, then pays link latency back to the compute node.

Synthetic workloads support two request issue modes. The default
`completion_driven` mode is response-paced: a compute node schedules its next
same-epoch request only after the previous request completes. This keeps at most
one outstanding request per node and matches the simulator's original behavior.
The optional `scheduled_bursty` mode is arrival-paced: generated requests carry
scheduled offsets from the epoch release time, and each `ComputeNode` schedules
the next same-epoch `GenerateRequest` after issuing the current one. This allows
bursts to create multiple outstanding requests from the same node without
having `Simulator` dump an entire epoch into the event queue at once.

When `memory.channel_count` is `1`, this is the original one-channel FIFO memory
model. When it is greater than `1`, the memory node behaves like several
independent FIFO servers inside the same memory node. Objects are mapped
deterministically with `(object_id - 1) % channel_count`. This static mapping is
simple on purpose: it gives repeatable channel-local contention without adding
a full memory-controller, bank, or placement-routing model.

Epochs are important because contention-aware policies consume prior-epoch
telemetry. The simulator uses a global epoch barrier: a later epoch is not
released until current-epoch work drains. This ensures previous-epoch
contention summaries are complete before policies build their next snapshot.
Without that barrier, a policy could accidentally mix current-epoch partial
telemetry into current-epoch cache decisions, which would make the policy look
more informed than the intended low-overhead design.

## Cache Policies And Telemetry

The local cache policy interface observes lookups, epoch starts, accesses,
admission decisions, eviction decisions, and final admission outcomes. The
`LocalCache` owns resident entries and capacity checks, while the selected
policy owns the scoring or victim-selection rule.

Current policies are:

| Policy | What It Models | Main Use In Evaluation |
| --- | --- | --- |
| `always_remote` | No local caching; every access goes to remote memory. | Calibration baseline for maximum remote pressure. |
| `lru` | Admit misses and evict the least recently used resident. | Strong practical recency baseline. |
| `hotness_only` | Admit/evict using local access counts. | Hotness baseline without remote contention signals. |
| `global_hottest_replication` | Oracle-style replication of globally hot generated objects. | Upper-bound style baseline for synthetic workloads. |
| `contention_aware` | Weighted score from local demand and prior remote contention. | Main research policy family. |

### Hotness-Only Policy

The hotness-only policy counts local demand for each object and admits an object
once its count reaches `min_admit_count`. Eviction chooses the resident object
with the lowest hotness count, then falls back to older last-access time and
smaller object ID for deterministic ties.

Hotness history is configurable:

| Mode | Behavior | Interpretation |
| --- | --- | --- |
| `epoch` | Clears hotness counts on every epoch start. | Very reactive baseline; forgets old demand quickly. |
| `cumulative` | Keeps counts for the whole run. | All-history endpoint; can over-favor old hot objects. |
| `windowed` | Keeps counts for the last `history_window_epochs` epochs. | Bounded-history baseline and usually the most realistic hotness-only comparison. |

Windowed hotness is useful because a real system would rarely maintain exact
unbounded history forever, but it also would not necessarily forget everything
at a fixed epoch boundary. The windowed mode gives hotness-only a fairer memory
of recent reuse while still limiting history.

### Contention-Aware Policy

The contention-aware policy scores objects for admission and eviction. Its
central idea is that an object can be valuable to cache not only because this
node has requested it often, but also because requests for that object were
expensive at the shared remote-memory bottleneck in a completed prior epoch.

The scoring model combines:

| Score Component | Source | Why It Matters |
| --- | --- | --- |
| `local_hotness` | Current node's local lookup count. | Prevents pure global telemetry from caching objects this node does not reuse. |
| `remote_accesses` | Previous-epoch memory-node count. | Captures object-level remote demand. |
| `distinct_requesters` | Previous-epoch requester set size. | Identifies objects creating shared pressure across nodes. |
| `queue_wait` | Previous-epoch total queue wait. | Measures bottleneck pain suffered by requests for the object. |
| `remote_service_time` | Previous-epoch memory service time. | Estimates remote service capacity consumed by the object. |
| `cost_density` | Previous-epoch `(queue wait + service time) / estimated object size`. | Rewards objects that avoid high remote pain per byte of private cache space. |
| `size_penalty` | Incoming or resident object size vs cache capacity. | Accounts for local cache opportunity cost. |

Remote contention components are normalized by the maximum value observed in the
policy's previous-epoch snapshot. Cost density is optional and defaults to zero
weight; it is mainly used by `contention_aware_size_value` experiments to study
large-object treatment under smoothed telemetry. Benefit components are added
with configurable weights, while `size_penalty` is subtracted. Admission
requires the resulting score to meet `min_admit_score`.

Eviction uses the same score model for resident entries. When space is needed,
the policy identifies the lowest-scoring resident as the candidate victim. It
refuses admission if the incoming object is not better than that victim, which
helps avoid replacing a resident with an object that has equal or weaker
expected value. Ties are deterministic: lower score first, then older
last-access time, then smaller object ID.

The key realism boundary is that contention-aware policies use completed
previous-epoch summaries, not instant same-request global knowledge. This keeps
the policy closer to low-overhead fabric/device telemetry rather than an
unrealistic globally synchronized oracle.

Current contention-aware variants are:

| Variant | Main Config Knobs | What It Changes | Failure Mode It Targets |
| --- | --- | --- | --- |
| `v1` | scoring weights, `min_admit_score`, `local_hotness_threshold` | Uses epoch `N-1` telemetry directly. | Baseline Phase 8 behavior. |
| `smoothed` | `telemetry_history_epochs`, `telemetry_decay` | Blends multiple prior epochs with decay. | Noisy or partially persistent contention. |
| `reuse_gated` | `local_reuse_gate_threshold` | Requires local demand before admission. | Global hot objects that this node will not reuse. |
| `hysteresis` | `eviction_score_margin` | Requires incoming score to beat the victim by a margin. | Cache churn from borderline evictions. |
| `smoothed_reuse_gated` | smoothing knobs, confirmation window/threshold, bypass margin, optional eviction margin | Uses bounded recent local evidence, but lets exceptionally strong scores bypass pending confirmation. | Noisy telemetry and hard-gate under-admission. |

The composite variant is exposed through
`contention_aware_smoothed_reuse_gated` and
`contention_aware_smoothed_reuse_gated_hysteresis`. The second alias adds a
mild eviction margin while keeping admission mechanics identical, allowing the
effect of hysteresis to be measured separately.

The memory node collects object-level contention metrics by `(epoch_id,
object_id)`, including remote accesses, distinct requesters, bytes served,
remote service time, total queue wait, maximum queue wait, mapped channel, and
observed queue depth. It also records per-channel contention summaries so
experiments can distinguish global memory pressure from pressure concentrated
on one overloaded channel.

## Workloads And Configuration

Experiments are driven by YAML configuration files. Hand-written configs in
`configs/` are useful for focused manual runs. Matrix-generated configs from
`scripts/run_matrix.py` are useful for matched comparisons across policies,
seeds, and architecture/workload dimensions.

A config describes:

- experiment name and output directory,
- memory node base latency and bandwidth,
- link latency,
- local cache capacity and policy,
- synthetic workload shape.

The most important knobs fall into four categories.

### Architecture And Bottleneck Knobs

| Knob | Meaning | Why It Matters |
| --- | --- | --- |
| `compute_node_ids` or matrix node count | Number of compute nodes issuing requests. | More nodes can increase aggregate remote demand and queue pressure. |
| `memory_bandwidth_bytes_per_time` | Remote service throughput. | Lower bandwidth makes large or frequent remote accesses serialize longer. |
| `memory.channel_count` | Number of independent FIFO memory-channel servers inside the memory node. | Higher counts add memory-side parallelism and reduce artificial global queueing when requests spread across channels. |
| `memory_base_latency` | Base remote-memory latency before queueing. | Raises miss cost even without contention. |
| `one_way_link_latency` | Simulated link latency in each direction. | Separates local-hit benefit from remote path cost. |

These parameters determine whether the simulated architecture has a meaningful
remote bottleneck. If remote memory is cheap and never queues, contention-aware
policy signals have little room to help.

### Cache And Policy Knobs

| Knob | Meaning | Why It Matters |
| --- | --- | --- |
| `local_cache.capacity_bytes` | Private cache capacity per compute node. | Controls cache pressure and eviction frequency. |
| `cache_capacity_hotset_multiplier` | Matrix helper that computes cache size from hot-set footprint. | Makes cache pressure easier to reason about across hot-set sizes. |
| `local_cache.hit_latency` | Cost of a local hit. | Determines how much latency a successful placement avoids. |
| `local_cache.policy` | Cache policy under test. | Selects LRU, hotness-only, contention-aware, or a baseline. |
| `local_cache.hotness.history_mode` | Hotness memory model. | Controls whether local-demand history is epoch, cumulative, or windowed. |
| `local_cache.contention.variant` | Contention-aware variant. | Selects direct, smoothed, reuse-gated, or hysteresis behavior. |
| contention weights | Relative importance of scoring components. | Determines whether the policy prioritizes local hotness, shared pressure, queue wait, service time, or size. |

Cache capacity is often expressed in matrix experiments as a multiple of one
hot set's representative footprint. For example, if hot set size is `16` and
representative object size is `64` bytes, a cache multiplier of `0.5` gives a
cache capacity of `16 * 64 * 0.5 = 512` bytes. That cache can hold roughly half
of one hot set when all objects have the representative size.

### Workload Locality And Stability Knobs

| Knob | Meaning | Why It Matters |
| --- | --- | --- |
| `object_count` | Total object universe size. | Larger universes reduce accidental reuse outside the hot set. |
| `hot_set_size` | Number of hot objects per node per epoch. | Sets the working-set pressure against each private cache. |
| `hot_access_probability` | Probability of choosing from the hot set. | Higher values concentrate demand and improve cacheability. |
| `cross_node_overlap` | Low/medium/high overlap between node hot sets. | Controls whether nodes pressure the same objects or mostly private objects. |
| `epoch_count` | Number of generated epochs. | More epochs expose churn, policy adaptation, and telemetry reuse. |
| `requests_per_node_per_epoch` | Requests each node issues per epoch. | Longer epochs provide more reuse opportunities before the hot set changes. |
| `hot_set_churn_fraction` | Fraction of hot objects replaced between epochs. | Directly controls telemetry staleness for previous-epoch policies. |
| `issue_mode` | Selects completion-driven or scheduled-bursty request issue. | Controls whether arrivals are smoothed by completions or can overlap in bursts. |
| `burst_size`, `burst_interval`, `intra_burst_gap` | Shape scheduled-bursty arrival timing. | Creates sharper queue pressure without changing object popularity. |
| `node_phase_jitter` | Adds deterministic per-node/epoch timing jitter. | Lets burst experiments compare synchronized and slightly desynchronized arrivals. |
| `hot_object_channel_count` | Optional workload control that restricts hot objects to the first `N` memory channels. | Creates channel-local hotspots for calibration; `0` leaves hot-object selection unrestricted. |

These knobs determine whether prior telemetry is predictive. Contention-aware
policies should be expected to perform better when hot objects and remote
pressure persist across epochs, and worse when hot sets churn faster than
telemetry can adapt.

Channel hotspot controls are intentionally workload-side controls, not cache or
placement policy. They are used to answer a specific simulator-realism
question: if memory has multiple independent channels, can a workload still
create localized contention on one subset of those channels? Cold objects remain
eligible across the full object universe, so the restriction targets hot-object
pressure rather than all traffic.

### Object Size And Service-Cost Knobs

| Knob | Meaning | Why It Matters |
| --- | --- | --- |
| `object_size_mode` | `fixed` or `bimodal`. | Fixed sizes isolate policy behavior; bimodal sizes expose service-cost tradeoffs. |
| `object_size_bytes` | Size used by fixed mode and as the representative matrix size. | Affects service time and cache capacity calculations. |
| `object_size_small_bytes` | Small-object size in bimodal mode. | Makes cheap-to-cache objects available. |
| `object_size_large_bytes` | Large-object size in bimodal mode. | Creates objects that are expensive remotely but expensive to cache. |
| `large_object_probability` | Fraction of objects assigned the large size. | Controls how often the policy faces size/service-cost tradeoffs. |

Object size is especially important for contention-aware policies because the
same object can be valuable in two opposing ways: caching it can avoid expensive
remote service time, but it may also consume enough local capacity to evict
several smaller objects. This is why fixed-size experiments are useful for
clean baselines, while bimodal experiments are useful for studying more
realistic service-cost behavior.

## Metrics And Outputs

Each simulator run writes a result directory. Important files include:

- `summary.json`: run-level summary with latency, cache, memory, contention,
  policy, viability, and per-node sections.
- `per_node.csv`: per-compute-node latency and cache summaries.
- `latencies.csv`: one row per completed response.
- `contention_by_object.csv`: one row per `(epoch_id, object_id)` with remote
  contention counters.
- `contention_by_channel.csv`: one row per `(epoch_id, memory_channel_id)` with
  channel-local queue and service counters.
- `policy_diagnostics.csv`: contention-aware score components and admission
  decisions, including recent confirmation count, required confirmation count,
  and whether admission used the high-score bypass.
- `cache_admissions.csv`: policy-neutral admission and placement lifecycle
  diagnostics.
- `epoch_diagnostics.csv`: epoch-level telemetry predictiveness summaries that
  compare the previous epoch's most contended objects against the current
  epoch's most requested objects using the configured synthetic hot-set size.
- `viability_metrics.csv`: one-row diagnostic summary for reuse, hot-set-sized
  stale telemetry, contention relief, eviction regret, and fairness.

Matrix runs additionally write:

- `generated_configs/`: generated YAML files for each matrix point.
- `runs/`: one output directory per generated config.
- `aggregate_summary.csv`: one row per run, used by the plotting/reporting
  pipeline.

## Running The Simulator

Configure and build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Run one hand-written YAML experiment:

```bash
./build/dm_simulator \
  --config configs/phase8_contention_aware.yaml \
  --output-dir results/phase8_contention_aware
```

Run a matrix preset:

```bash
scripts/run_matrix.py \
  --preset eval_policy_viability \
  --results-dir results/eval_policy_viability
```

Dry-run a matrix without launching simulations:

```bash
scripts/run_matrix.py \
  --preset eval_contention_weight_sensitivity \
  --dry-run \
  --expect-runs 25 \
  --results-dir results/eval_contention_weight_sensitivity_dry_run
```

Generate plots and a Markdown report from an aggregate CSV:

```bash
scripts/plot_results.py \
  --aggregate results/eval_policy_viability/aggregate_summary.csv \
  --output-dir results/eval_policy_viability/analysis \
  --baseline lru \
  --report-mode policy_viability
```

Common report modes include:

- `contention_calibration`
- `policy_viability`
- `interaction`
- `parameter_demo`
- `weight_sensitivity`
- `generic`

Use `--report-mode auto` when the aggregate comes from a known evaluation
preset and only contains one preset.

## Current Evaluation Presets And Report Modes

The matrix runner now includes several focused evaluation families. These are
intended to keep simulator-validity evidence separate from policy-viability
evidence.

| Preset Family | Main Purpose | Default Report Mode |
| --- | --- | --- |
| `eval_contention_calibration` | Show how node count, bandwidth, overlap, and hot-access concentration create remote-memory pressure. | `contention_calibration` |
| `eval_channel_calibration` | Show how independent memory channels reduce artificial global FIFO queueing. | `contention_calibration` |
| `eval_channel_hotspots` | Show channel-local hotspots by concentrating hot objects onto fewer channels. | `contention_calibration` |
| `eval_bursty_calibration` | Compare completion-driven issue against scheduled-bursty arrivals. | `contention_calibration` |
| `eval_policy_viability` | Compare LRU, windowed hotness, oracle replication, and contention-aware variants in a known contention regime. | `policy_viability` |
| `eval_interactions_*` | Produce two-dimensional interaction studies such as churn-by-epoch, cache-by-hot-set, and node-by-bandwidth. | `interaction` |
| `eval_knob_*` | Produce small single-knob presentation studies. | `parameter_demo` |
| `eval_contention_weight_sensitivity` | Sweep one contention-aware score weight at a time against one shared LRU baseline. | `weight_sensitivity` |

The plotting tool can infer these modes with `--report-mode auto`, but using
the explicit mode in final analysis commands makes reports easier to reproduce.

## Testing And Plotting Pipeline

The C++ simulator and Python helpers are tested through CMake and CTest:

```bash
ctest --test-dir build --output-on-failure
```

The test suite covers scheduler behavior, request paths, cache policies,
workload generation, config loading, metrics, experiment output, matrix dry
runs, and plotting smoke tests. Plotting tests skip cleanly when Matplotlib is
not installed.

The typical evaluation flow is:

1. Use `scripts/run_matrix.py --dry-run` to verify generated configs and run
   counts.
2. Run the matrix for real.
3. Inspect `aggregate_summary.csv`.
4. Generate `report.md`, comparison CSVs, and SVG plots with
   `scripts/plot_results.py`.
5. Use the generated report and plots to decide whether a narrowed rerun or
   multi-seed confirmation is needed.

This pipeline is designed to support scientific comparison rather than manual
one-off tuning. Policies should be compared on matched workload and architecture
settings.

## Design Choices

The simulator is object-level rather than page-level. This keeps policy
experiments readable and avoids prematurely modeling OS page tables, page
faults, or migration mechanics. Page-level memory is a natural future extension,
but object-level simulation is enough to study cache admission under shared
remote-memory contention.

The simulator currently has one memory node, but that node can contain multiple
independent memory channels. This preserves one clear remote-memory endpoint
while avoiding the most artificial part of a single global FIFO: unrelated
objects can proceed on different channels. Multiple memory nodes would be
useful for placement and routing questions, but they would also require
additional metadata and load-balancing semantics.

The transport model is simplified. Link latency and memory service time are
modeled through scheduled events, not through real packets or protocol stacks.
This is intentional: the project studies policy behavior under controllable
latency, bandwidth, and queueing assumptions.

Contention telemetry is epoch-summary based. Policies use previous-epoch
snapshots rather than current-request feedback. This avoids assuming perfect
instant global knowledge and keeps the overhead model closer to plausible
fabric/device counters.

The experiment harness emphasizes matrix comparisons. This makes it easier to
separate policy behavior from workload stability, cache pressure, memory
bandwidth, object size mix, and other architectural conditions.

## Limitations

Current limitations include:

- one shared memory node, optionally with several simplified channels,
- no detailed bank, row-buffer, memory-controller, or scheduling model,
- no page-based memory model,
- no writes, coherence, invalidation, or write-sharing protocol,
- no real CXL, RDMA, NIC, DPU, or switch implementation,
- simplified link and service modeling,
- synthetic workloads rather than application traces,
- object-level rather than byte-address or page-level placement,
- approximate queue-wait attribution,
- no shared near-fabric cache in the active request path,
- no multi-memory-node placement or routing,
- no hardware monitoring overhead beyond modeled low-overhead epoch summaries.

These limitations are not accidental. They keep the simulator focused on the
central question: when does coarse contention telemetry make private-cache
placement better than recency or hotness alone?

## Future Additions

Implemented realism improvements include cost-density scoring for large-object treatment, configurable memory
channels, channel-local hotspot controls, and scheduled-bursty workload issue.

Near-term future improvements include:

- broader bursty policy-viability presets after calibration results are
  reviewed,
- cost-density or size-value sensitivity studies if large-object treatment
  becomes a central result.

Other plausible future additions include:

- multiple memory nodes,
- lightweight object metadata such as size class, cacheability, and home memory
  node after multiple memory nodes make that metadata operationally useful,
- page-level memory modeling,
- shared near-fabric cache or SmartNIC-like cache tier,
- richer object placement metadata,
- heterogeneous memory tiers,
- fairness-aware admission,
- prefetching,
- write and coherence modeling,
- application trace import,
- more detailed fabric congestion and routing models.


