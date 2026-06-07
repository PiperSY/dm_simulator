# DM Simulator Architecture

This document gives a current high-level architectural view of the simulator.
Use it together with `docs/implementation_map.md`, and
`docs/implementation_execution_guide.md`: this file explains the system shape,
while those documents provide project scope, control-flow details, and running
instructions.

The simulator is a single-process, deterministic, object-level model of a
simplified disaggregated-memory system. It focuses on private local-cache
placement under remote-memory contention. It intentionally does not model a full
CXL, RDMA, OS paging, or coherence stack.

## 1. Architectural Intent

The simulator studies policy behavior under controlled contention. The active
architecture models:

- compute nodes that issue read requests,
- private local caches at each compute node,
- cache policies such as LRU, hotness-only, and contention-aware variants,
- one shared remote memory node with configurable independent memory channels,
- synthetic workloads with hot sets, churn, overlap, object-size modes, channel
  hotspots, and optional bursty arrivals,
- deterministic event scheduling and simulated latency accounting,
- metrics, diagnostics, matrix aggregation, and plotting/report generation.

This intentionally narrow model keeps the main research question visible: when
does coarse prior-epoch contention telemetry improve private-cache admission and
eviction compared with recency or hotness alone?

## 2. Current Request Path

The active request path is local cache -> remote memory channel -> response ->
local admission. The shared near-fabric cache is still a stretch component and
is not part of the current active request path.

```text
+-------------------+
| Workload Stream   |
| per Compute Node  |
+---------+---------+
          |
          v
+-------------------+
| ComputeNode i     |
| - WorkloadCursor  |
| - outstanding req |
+---------+---------+
          |
          v
+-------------------+
| LocalCache        |
| - resident objs   |
| - CachePolicy     |
+----+--------------+
     |
     +-- hit --> local-hit latency --> RequestComplete --> Stats
     |
     +-- miss
          |
          v
   one-way link latency
          |
          v
+-------------------------------+
| MemoryNode                    |
| object_id -> memory channel   |
| one FIFO/server per channel   |
+------------+------------------+
             |
             v
      queue wait + service time
             |
             v
      one-way link latency back
             |
             v
+-------------------------------+
| Response / Admission          |
| - record latency              |
| - policy admits or rejects    |
| - evict if needed             |
| - update diagnostics          |
+-------------------------------+
```

### Request-Path Notes

1. A local hit completes without using remote memory.
2. A miss is routed to the shared memory node after link latency.
3. The memory node maps the object to a channel with a deterministic object-ID
   mapping and queues the request in that channel's FIFO.
4. Each memory channel is an independent FIFO server. Channels are a simplified
   resource-local contention model, not a detailed bank/controller model.
5. Service time is based on memory base latency, object size, and per-channel
   bandwidth.
6. Admission and eviction happen after the remote response returns, so policy
   decisions can be tied to the completed request and recorded diagnostics.

## 3. Component View

```text
+---------------------------------------------------------------+
| Simulator                                                     |
| - Scheduler                                                   |
| - Stats                                                       |
| - request table / responses                                   |
| - epoch release barrier                                       |
+----------------------+-------------------+--------------------+
                       |                   |
                       v                   v
            +-------------------+   +----------------------------+
            | ComputeNode[]     |   | MemoryNode                 |
            | - WorkloadCursor  |   | - channel FIFO/server[]    |
            | - LocalCache      |   | - object/channel telemetry |
            +---------+---------+   +--------------+-------------+
                      |                            |
                      v                            v
             +-------------------+        +----------------------+
             | LocalCache        |        | Stats                |
             | - entries         |        | - latency            |
             | - capacity        |        | - cache metrics      |
             | - diagnostics     |        | - contention metrics |
             +---------+---------+        | - viability metrics  |
                       |                  +----------------------+
                       v
             +-------------------+
             | CachePolicy       |
             | - always_remote   |
             | - lru             |
             | - hotness_only    |
             | - global hottest  |
             | - contention      |
             +-------------------+

Optional future tier:

ComputeNode miss -> SharedCacheNode -> MemoryNode
```

## 4. Responsibilities By Module

### `sim/`

Owns the discrete-event core, config types, config loading, event definitions,
scheduler, simulator coordinator, request table, and epoch release logic. It
should not contain policy scoring details.

### `nodes/`

Contains active simulated components.

`ComputeNode` issues requests, performs local-cache lookups, forwards misses,
receives responses, drives completion-driven or scheduled-bursty arrivals, and
updates request completion state.

`MemoryNode` receives remote misses, maps objects to channels, manages one FIFO
service queue per channel, records object-level and channel-level contention,
and schedules response events.

`SharedCacheNode` exists as a placeholder for a future near-fabric cache tier.
It is not part of the current active path.

### `cache/`

Contains local cache storage and cache policy logic. `LocalCache` owns entries,
capacity accounting, admission lifecycle diagnostics, and eviction execution.
Policies decide admission and victim selection but do not schedule events.

Current policy families include:

- `always_remote`
- `lru`
- `hotness_only` with `epoch`, `cumulative`, and `windowed` history modes
- `global_hottest_replication`
- `contention_aware` with `v1`, `smoothed`, `reuse_gated`, `hysteresis`, and
  `smoothed_reuse_gated` variants
- opt-in composite aliases with and without mild eviction hysteresis
- the opt-in `contention_aware_size_value` matrix alias, which uses smoothed
  contention telemetry plus cost-density scoring

### `workloads/`

Generates deterministic synthetic request streams. Workloads control object
universe size, hot-set size, hot-access probability, cross-node overlap, hot-set
churn, object-size mode, large-object probability, issue mode, burst knobs, and
channel-local hotspot controls.

### `metrics/`

Collects latency, cache, memory, contention, policy, viability, and fairness
metrics. These metrics are written by the experiment runner and consumed by
matrix aggregation and plotting.

### `experiments/`

Loads YAML configs, constructs the simulator, runs an experiment, and writes
result files such as `summary.json`, `latencies.csv`,
`contention_by_object.csv`, `contention_by_channel.csv`, and diagnostic CSVs.

### `transport/`

Contains placeholder/reusable transport abstractions. Current link latency and
memory queueing are implemented through events and `MemoryNode`; there is no
real packet, NIC, RDMA, or CXL protocol simulation.

## 5. Execution Model

The simulator is discrete-event based. Simulated time advances when the
`Scheduler` pops the next event. Event ordering is deterministic by timestamp
and sequence number.

The common lifecycle is:

1. `Simulator` loads config, builds workloads, nodes, caches, policies, memory,
   stats, and the scheduler.
2. The first epoch is released.
3. Each compute node with work schedules the first request for that epoch.
4. `GenerateRequest` creates a request and schedules local lookup.
5. Local hits complete after local hit latency.
6. Local misses travel to memory, map to a channel, queue, receive service, and
   return.
7. Returned misses trigger cache admission and possible eviction.
8. Request completion records latency and diagnostics.
9. A later epoch is released only after the scheduler is empty and all compute
   nodes have no outstanding requests.

There are two workload issue modes:

- `completion_driven`: a node schedules its next same-epoch request only after
  the prior request completes. This preserves the original one-outstanding-
  request-per-node behavior.
- `scheduled_bursty`: a generated request has a scheduled offset from the epoch
  start. After issuing one request, the compute node schedules the next
  same-epoch `GenerateRequest` by that offset. This allows overlapping
  same-node requests without having the simulator preload an entire epoch.

The epoch barrier is deliberate. Contention-aware policies use completed
previous-epoch telemetry. The barrier prevents policies from accidentally using
partial same-epoch global knowledge.

## 6. Memory Model

The current memory model is one shared memory node with configurable independent
channels.

- `memory.channel_count: 1` preserves the original single-channel FIFO model.
- `memory.channel_count > 1` creates several independent FIFO servers inside
  the same memory node.
- Objects map to channels deterministically with
  `(object_id - 1) % channel_count`.
- `memory.bandwidth_bytes_per_time` is interpreted as per-channel bandwidth.
- `workload.hot_object_channel_count` can restrict synthetic hot-object
  selection to the first `N` channels, creating channel-local hotspots while
  leaving cold objects unrestricted.

This is more realistic than one global FIFO because unrelated objects can be
served on different channels. It is still simpler than real DRAM channels,
banks, row buffers, controller scheduling, routing, or multiple memory nodes.

## 7. Policy And Telemetry Boundary

Contention-aware policies use coarse prior-epoch telemetry:

- remote accesses,
- distinct requesters,
- bytes served,
- queue wait,
- remote service time,
- mapped channel context,
- local hotness from the requesting node,
- optional cost density, which estimates remote cost per cache byte.

The policy does not use same-request causal queue attribution or instant global
coordination. This keeps the policy closer to a low-overhead telemetry design
where memory-side counters are periodically summarized and distributed.

Hotness-only policy uses local access history only. Its `windowed` history mode
is the realistic bounded-history baseline; `epoch` and `cumulative` are useful
endpoints for comparison.

## 8. Architectural Invariants

The following invariants should remain true:

1. Simulated time never decreases.
2. Event ordering is deterministic.
3. Every request completes exactly once.
4. Cache occupancy never exceeds configured capacity.
5. Local hits do not enter remote memory.
6. Remote misses are queued through the mapped memory channel.
7. Epoch `N+1` is not released until epoch `N` work drains.
8. Contention-aware decisions use prior-epoch telemetry, not same-epoch oracle
   feedback.
9. Cache policies do not directly manipulate scheduler internals.
10. Existing configs remain valid through conservative defaults such as
    `memory.channel_count: 1` and `workload.issue_mode: completion_driven`.

## 9. Deliberate Simplifications

The simulator intentionally omits:

- real RDMA verbs, CXL protocol details, NICs, DPUs, and switches,
- writes, invalidation, coherence, and write-sharing protocols,
- page tables, page faults, and page migration,
- multiple memory nodes and placement-aware routing,
- detailed DRAM bank, row-buffer, and controller scheduling,
- real application traces,
- hardware monitoring overhead beyond coarse epoch summaries.

These are scope boundaries, not accidental omissions. They keep the simulator
small enough to explain policy behavior, run controlled matrices, and separate
simulator-validity questions from cache-policy questions.

## 10. Future Extensions

Important future directions include:

- multiple memory nodes and routing/placement decisions,
- page-level memory modeling,
- object metadata that becomes operationally useful once multiple memory nodes
  exist,
- shared near-fabric cache or SmartNIC-like cache tier,
- richer burst and trace-driven workload models,
- combined contention-aware variants such as smoothed plus reuse-gated,
- write/coherence modeling,
- more detailed fabric congestion and memory-controller models.

