# Current Implementation Map

This document is a practical map of how the simulator currently executes. It
complements `docs/architecture.md`, which describes the broader intended
architecture and future direction. Use this file when you want to trace the
implemented control flow from a YAML experiment config to request execution,
cache decisions, contention accounting, and result files.

---

## Big Picture

```text
src/main.cpp
  parses --config / --output-dir
  |
  v
src/experiments/runner.*
  load config, run Simulator, write result files
  |
  v
src/sim/config_loader.*
  YAML -> ExperimentConfig -> SimulationConfig
  |
  v
src/sim/simulator.*
  owns Scheduler, Stats, request table, responses
  owns ComputeNode[] and one MemoryNode
  |
  +--> src/workloads/generators.*
  |      SyntheticWorkloadConfig -> per-node RequestSpec streams
  |
  +--> src/sim/scheduler.*
  |      deterministic event queue and simulated time
  |
  +--> src/nodes/compute_node.*
  |      request generation, local cache lookup, response handling
  |      |
  |      v
  |    src/cache/local_cache.*
  |      cache storage, lookup, admission, eviction
  |      |
  |      v
  |    src/cache/cache_policy.hpp
  |      policy interface
  |      |
  |      +--> LRU / always-remote
  |      +--> hotness baselines
  |      +--> src/cache/contention_policy.*
  |
  +--> src/nodes/memory_node.*
         one FIFO remote-memory service queue per configured channel
         records object and channel contention into Stats
         |
         v
       src/metrics/stats.*
         latency, cache, memory queue, contention, diagnostics
```

The active version is still a single-process, event-driven simulator. There are
no real threads, packets, RDMA verbs, or CXL protocol operations.

---

## Execution Pipeline

1. `src/main.cpp` parses:
   - `--config <path>`
   - optional `--output-dir <path>`

2. `ExperimentRunner::run_config` in `src/experiments/runner.*` loads the YAML
   file and creates an `ExperimentConfig`.

3. `src/sim/config_loader.*` parses YAML into:
   - experiment metadata such as name and output directory
   - memory/link timing
   - local cache capacity, hit latency, and policy config
   - synthetic workload settings

4. `Simulator` construction in `src/sim/simulator.*` prepares the run:
   - generates synthetic workloads if `synthetic_workload` is present
   - validates compute-node IDs and request epoch ordering
   - creates one `ComputeNode` per configured node
   - passes the workload issue mode to each `ComputeNode`
   - creates one shared `MemoryNode` with one or more independent channels
   - creates one `LocalCache` and `CachePolicy` per compute node
   - owns the shared `Stats`, request table, response list, and event log

5. `Simulator::run()` releases the first epoch and starts the scheduler loop.
   The simulator only releases a later epoch when:
   - the scheduler is empty
   - all compute nodes have no outstanding requests
   - at least one compute node has work waiting in the next epoch

6. `Scheduler::run_until_empty` in `src/sim/scheduler.*` pops events in
   deterministic `(time, sequence)` order and calls back into
   `Simulator::dispatch_event`.

7. `Simulator::dispatch_event` routes each event by `target_id`:
   - compute-node target -> `ComputeNode::handle_event`
   - memory-node target -> `MemoryNode::handle_event`

8. After the simulator finishes, `ExperimentRunner` writes:
   - `summary.json`
   - `per_node.csv`
   - `latencies.csv`
   - `contention_by_object.csv`
   - `contention_by_channel.csv`
   - `cache_admissions.csv`
   - `epoch_diagnostics.csv`
   - `viability_metrics.csv`
   - `policy_diagnostics.csv`

---

## Request Lifecycle

The current active request path is local-cache plus one shared remote memory
node. That memory node defaults to one FIFO service channel, but it can be
configured with multiple independent channel FIFOs. The optional shared-cache
tier is not active yet.

```text
GenerateRequest
  ComputeNode creates Request from WorkloadCursor / RequestSpec
  stores it in Simulator's request table
  schedules LocalCacheLookup at the same simulated time

LocalCacheLookup
  ComputeNode asks LocalCache to look up the object
  |
  +-- hit:
  |     Stats records local cache hit
  |     schedules LocalCacheHitComplete at now + local_cache_hit_latency
  |
  +-- miss:
        Stats records local cache miss
        schedules ForwardToMemory at now + one_way_link_latency

LocalCacheHitComplete
  ComputeNode creates a Response served from LocalCache
  records latency in Stats
  schedules RequestComplete

ForwardToMemory
  MemoryNode maps the object to a channel
  enqueues the request ID in that channel's FIFO order
  records remote access and observed channel queue depth
  schedules MemoryServiceStart if that channel is idle

MemoryServiceStart
  MemoryNode pops the next queued request from the mapped channel
  records queue wait globally, by object/epoch, and by channel/epoch
  schedules MemoryServiceComplete after service time

MemoryServiceComplete
  MemoryNode records object/channel service time and bytes served
  schedules ReturnResponse at now + one_way_link_latency
  schedules the next MemoryServiceStart if queued requests remain on that channel

ReturnResponse
  ComputeNode creates a Response served from Memory
  records end-to-end latency
  asks LocalCache whether to admit the object
  schedules RequestComplete

RequestComplete
  ComputeNode decrements outstanding request count
  in completion_driven mode, schedules GenerateRequest for the next same-epoch request
  in scheduled_bursty mode, does not drive new arrivals
  if next request is in a later epoch, waits for Simulator's epoch release
```

In `scheduled_bursty` mode, `GenerateRequest` itself schedules the next
same-epoch `GenerateRequest` using the next request's
`scheduled_issue_offset`. This keeps arrival timing local to `ComputeNode` and
allows multiple outstanding same-node requests without having `Simulator`
preload every request in the epoch.

---

## Cache Policies

`LocalCache` in `src/cache/local_cache.*` owns the cache entries and asks its
policy how to behave. The policy does not schedule events.

The policy interface lives in `src/cache/cache_policy.hpp`:

- `on_lookup(...)` observes every hit/miss lookup.
- `on_epoch_start(...)` lets policies reset or snapshot epoch state.
- `on_access(...)` updates entry metadata such as recency.
- `should_admit(...)` decides whether a returned remote object is worth caching.
- `select_victim(...)` chooses an object to evict when capacity is needed.
- `on_admission_result(...)` records final admission diagnostics.

Current policies:

- `AlwaysRemotePolicy`: never admits objects.
- `LruPolicy`: admits on miss and evicts least-recently-used entries.
- `HotnessOnlyPolicy`: admits objects after local access count reaches a
  threshold, using epoch-only, cumulative, or bounded-window history.
- `GlobalHottestReplicationPolicy`: oracle-style baseline that pre-installs
  globally hottest generated-workload objects per epoch.
- `ContentionAwarePolicy`: scores objects using local hotness plus previous
  epoch contention telemetry from `Stats`.

The contention-aware policy intentionally consumes previous-epoch summaries, not
same-request or same-epoch telemetry. That keeps the model closer to a plausible
low-overhead fabric/device telemetry design.

Matrix policy aliases map onto these implementations. `hotness_only_windowed`
uses `HotnessOnlyPolicy` with `history_mode: windowed` and a bounded epoch
window. `contention_aware_smoothed`, `contention_aware_reuse_gated`, and
`contention_aware_hysteresis` select explicit contention-aware variants.
`contention_aware_smoothed_reuse_gated` uses smoothed telemetry plus bounded
local confirmation and a selective high-score bypass. The corresponding
`_hysteresis` alias adds a mild eviction margin without changing admission.
`contention_aware_size_value` is an opt-in smoothed contention-aware alias that
enables cost-density scoring through `cost_density_weight`.

---

## Metrics And Contention Telemetry

`Stats` in `src/metrics/stats.*` is the central metrics collector. It is owned
by `Simulator` and referenced by compute nodes, the memory node, and some cache
policies.

Compute-side metrics:

- completed request count
- total and average latency
- per-node latency samples
- local cache hits and misses
- per-node cache hit rate

Memory-side metrics:

- total memory queue wait
- average and max wait
- peak memory queue depth

Object contention metrics are grouped by `(epoch_id, object_id)`:

- remote accesses
- distinct requesters
- bytes served
- total remote service time
- total queue wait
- max queue wait
- wait sample count
- max observed queue depth
- average queue wait

These metrics are collected in `MemoryNode` at three points:

- `ForwardToMemory`: maps the object to a channel and records remote access plus queue depth.
- `MemoryServiceStart`: records queue wait for the object and channel.
- `MemoryServiceComplete`: records service time and bytes served for the object and channel.

`ContentionAwarePolicy` uses `Stats::previous_epoch_contention(epoch_id)` at
epoch start to build its scoring snapshot.

Viability diagnostics are derived after the run. They summarize admission
yield, reuse-after-admit rate, contention relief estimates, eviction regret,
fairness, and stale telemetry. For synthetic workloads, stale telemetry compares
the previous epoch's most contended objects against the current epoch's most
requested objects over the configured hot-set size, not a fixed-width object list.

---

## Workloads And Epochs

Synthetic workloads are generated in `src/workloads/generators.*`.

`SyntheticWorkloadConfig` controls:

- seed
- compute node IDs
- object count
- object size
- requests per node per epoch
- issue mode: completion driven or scheduled bursty
- burst size, burst interval, intra-burst gap, and node phase jitter
- epoch count
- hot set size
- hot access probability
- hot set mode: static or epoch shift
- cross-node overlap: low, medium, or high

Generated workloads become per-node `RequestSpec` streams. Each `RequestSpec`
includes object ID, size, epoch ID, and an optional scheduled issue offset.
Each `ComputeNode` owns a `WorkloadCursor` that returns the next request spec in
order.

Epoch handling is coordinated by `Simulator`:

- completion-driven compute nodes issue one response-paced request at a time
- scheduled-bursty compute nodes can have overlapping same-epoch requests
- a node does not self-advance into a later epoch
- the simulator releases the next epoch only after all active work drains

This barrier is important for contention-aware policies because it makes prior
epoch telemetry complete before the next epoch starts.

---

## Result Files

The experiment runner writes one output directory per run.

`summary.json`

- experiment name
- completed requests
- mean / median / P95 / P99 latency
- cache hit/miss summary
- memory wait summary
- top contention objects
- policy decision summary
- per-node summary rows

`per_node.csv`

- one row per compute node
- completed requests
- mean latency
- P99 latency
- cache hits/misses/hit rate

`latencies.csv`

- one row per completed response
- request ID, source node, object ID, epoch
- served tier
- completion time
- total latency
- bytes transferred

`contention_by_object.csv`

- one row per `(epoch_id, object_id)` that reached remote memory
- remote access count
- requester diversity
- byte/service/wait contention metrics

`contention_by_channel.csv`

- one row per `(epoch_id, memory_channel_id)` that served remote memory traffic
- remote access count
- bytes, service time, queue wait, queue depth, and average wait by channel

`policy_diagnostics.csv`

- one row per policy admission attempt
- object/request/node/epoch
- admitted or rejected
- reason
- total score and score components, including cost-density diagnostics when
  contention-aware scoring is active
- recent/required local confirmation counts and high-score bypass status
- evicted object IDs, if any

For non-diagnostic policies such as LRU, this file may contain only the header.

`cache_admissions.csv`

- policy-neutral admission and placement lifecycle records
- admitted/rejected status, reasons, evictions, and placement source
- used to derive reuse yield and eviction-regret diagnostics

`epoch_diagnostics.csv`

- one row per compared epoch
- previous contended-object set and current requested-object set
- hot-set-sized telemetry overlap and stale-telemetry rate

`viability_metrics.csv`

- one-row run-level summary for admission yield, reuse rate, stale telemetry,
  contention relief, eviction regret, and fairness

---

## Stretch And Placeholder Components

Some files exist to preserve the intended architecture but are not central to
the current active path.

- `src/nodes/shared_cache_node.*` is reserved for the Phase 9 shared near-fabric
  cache stretch goal.
- `src/transport/link.*` and `src/transport/queue_model.*` are reserved for
  future reusable transport/link abstractions. Current link delay and memory
  queueing are implemented directly through scheduled events and `MemoryNode`.
