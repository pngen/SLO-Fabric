# SLO Fabric

SLO Fabric is an open-source, vendor-neutral C++20 runtime for defining, evaluating, and
enforcing service-level objectives across latency, availability, throughput, recovery time,
memory pressure, and cost, with explicit policy and enforcement hooks into heterogeneous AI
infrastructure.

The core systems question it answers:

> What service obligations apply to this workload or service now, what evidence proves whether
> those obligations are being met or threatened, which constraint is binding, and what runtime
> action is authorized to preserve or restore compliance?

The thesis is simple: **an SLO is not a dashboard threshold.** A latency target without an
enforcement consequence is monitoring; an availability target without failure/recovery semantics is
reporting; a throughput target without admission/resource consequences is aspiration; a cost
ceiling without execution policy is accounting; a recovery-time objective without recovery
authority is documentation; a memory-pressure target without resource action is telemetry. SLO
Fabric turns explicit service obligations into governed runtime contracts.

## Systems boundary

SLO Fabric **owns**:

- service-level objective definition;
- objective identity and generation;
- scoped workload/service contracts;
- target and budget semantics;
- evaluation against current evidence;
- violation prediction where evidence supports it;
- compliance state;
- binding-constraint identification;
- enforcement intent, authorization, and generation fencing;
- explicit typed action hooks into adjacent scheduler/resource/recovery runtimes;
- explanation of why an action is required;
- re-evaluation after enforcement;
- durable contract/history state where applicable.

SLO Fabric **does not own** adjacent runtime mechanisms. It emits typed enforcement *intents* and
*authorizations* to those layers. The architectural boundary is:

> SLO Fabric determines WHAT obligation is binding and WHAT class of runtime response is authorized
> or required. Adjacent runtimes determine HOW their mechanism executes that response.

SLO Fabric does not absorb Tail Governor, Cost Governor, Recovery Planner, Admission Fabric, or any
other adjacent runtime; it exposes narrow typed enforcement hooks to them.

## Objective dimensions

Each SLO dimension is a typed objective with centralized semantics (dimension, unit, target,
directionality, hard/soft, window, minimum evidence, breach/recovery thresholds, hysteresis,
priority, enforcement class):

- **Latency** - `LatencyMean`, `LatencyPercentile` (p50/p95/p99/p999 via `percentile_rank`),
  `LatencyMaxDeadline`. Percentiles are exact nearest-rank over a bounded sample store; a
  percentile is never fabricated from too few samples.
- **Availability** - `AvailabilityRequest` (successful/eligible requests) and `AvailabilityTime`
  (serving/eligible time) modes.
- **Throughput** - typed rates (requests/s, tokens/s, bytes/s, operations/s); never interchanged.
- **Recovery time** - `RecoveryMaxDuration`; measured from the authoritative failure boundary to
  the restored/verified boundary supplied by Recovery Planner / Failover Fabric evidence.
- **Memory pressure** - `MemoryPressureMax`, `MemoryMinHeadroom`; governed headroom, not absolute
  bytes. SLO Fabric accepts pressure evidence and emits enforcement intent; it does not implement
  eviction/reclamation.
- **Cost** - `CostPerRequest`, `CostPerToken`, `CostWindowBudget`; typed cost micros. If no cost
  model/evidence is supplied, evaluation returns insufficient evidence.

## Contract model

A `SloContract` is generation-bound and lifecycle-managed. It carries an `SloContractId`,
generation, service/workload/tenant scope, one or more objectives, an objective priority,
hard/soft semantics, an evaluation window, freshness requirements, an enforcement policy and its
generation, an effective-from time, an optional expiration, a coordinator epoch, provenance, and a
lifecycle. Lifecycle states are `Draft`, `Active`, `Superseded`, `Suspended`, `Expired`,
`Retired`. A contract can authorize enforcement only when it is `Active` **and** within its time
window. An expired or superseded contract cannot authorize new enforcement.

## Evidence and freshness

Evaluation consumes typed `EvidenceRecord`s carrying the subject identity and generation, source
identity/boot, coordinator epoch, observation time, sample count, provenance, confidence,
freshness, unit, aggregation type, and measurement status. Provenance categories are `Measured`,
`Derived`, `Estimated`, `Reported`, `Synthetic`, `Unknown`. Synthetic evidence is never
reported as measured.

Freshness is explicit (`Current`, `Stale`, `Expired`, `RevalidationRequired`, `Unknown`).
Stale or unknown evidence cannot silently produce a current compliance state. Recovered dynamic
observations **become `RevalidationRequired`** unless a contract explicitly makes them durably
valid; nothing is silently carried across a coordinator restart.

## Evaluation windows and percentiles

Windows are `Instantaneous`, `Sliding`, `Tumbling`, `FixedInterval`, and `RollingCount`,
with an injectable clock. The window engine rejects clock rollback, duplicate sequence numbers, and
(for tumbling/fixed windows) late samples; sliding windows evict by age and rolling-count windows by
count. Percentiles use the exact, deterministic nearest-rank method over a **bounded** sample store;
`INSUFFICIENT_EVIDENCE` is produced where the sample count is inadequate.

## Error budgets

Where applicable, an `ErrorBudget` tracks total, consumed, remaining, and burn rate with exact
integer arithmetic. Consumption is deduplicated so a duplicate event never double-consumes budget.
The invariant *remaining <= total* always holds; excess consumption is tracked as overrun. Burn rate
is classified as `Normal`, `Elevated`, or `Critical`. Budgets replenish at window rollover.

## Compliance states

Deterministic outcomes: `Compliant`, `NearLimit`, `AtRisk`, `Violating`, `Recovering`,
`InsufficientEvidence`, `RevalidationRequired`, `Suspended`. `AtRisk` (predicted future risk)
and `Violating` (current violation) are distinct and never collapsed. `InsufficientEvidence`
never becomes unconditional compliance.

## Multi-objective policy and binding constraint

Policy is first-class, versioned state (`Lexicographic`, `HardThenSoft`). The default
resolves deterministically: hard constraints first, then violation severity, then priority, then a
deterministic tie-break. The **binding constraint** is the highest-ranked objective; all other
non-compliant hard objectives are preserved as secondary constraints. It is never an unexplained
scalar score. Identical canonical input produces an identical decision (verified by digest).

## Enforcement model

An `EnforcementIntent` is typed (admission, scheduler, resource, recovery, memory, preemption,
power, cost actions) and **generation-bound** -- bound to the coordinator epoch and the relevant
contract, policy, objective, evaluation, and evidence generations. A stale or superseded intent is
rejected before it can mutate runtime state. Enforcement has a lifecycle
(`Proposed` to `Authorized` to `Dispatched` to `Acknowledged` to `Effective`, etc.) with
legal transitions; a cancelled or superseded enforcement cannot become authoritative success unless
it crossed the `Effective` commit boundary. Narrow typed adapters (`IAdmissionEnforcer`,
`ISchedulerEnforcer`, `IResourceEnforcer`, `IRecoveryEnforcer`, `IMemoryEnforcer`,
`IPreemptionEnforcer`, `IPowerEnforcer`) prevent invalid cross-action calls; a deterministic
`ReferenceEnforcementSink` records every dispatch for tests and closed-loop verification. Cooldown
prevents enforcement thrash.

## Closed-loop evaluation

Evaluation is a deterministic pipeline: resolve active contract, validate authority, validate
objective generation, ingest evidence, validate freshness and minimum evidence, compute objective
state, identify violations/risks, identify binding constraint, resolve multi-objective conflicts,
select enforcement intent, authorize action, publish explanation. SLO restoration is never confused
with action acknowledgment: post-action fresh evidence must independently show recovery.

## Authority and generation model

Distinct authority domains are strongly typed (`CoordinatorEpoch`, `SourceBootId`/WorkerBootId,
`ServiceId`/Generation, `WorkloadId`/Generation, `SloContractId`/Generation,
`ObjectiveId`/Generation, `PolicyId`/Generation, `EvidenceId`/Generation,
`EvaluationId`/Generation, `EnforcementId`/Generation, `DispatchId`, `AttemptId`,
`Resource`/Topology/RecoveryPlanGeneration). Equality, ordering, hashing, formatting, and canonical
encoding are deterministic. Stale epoch/boot/contract/policy/evidence/evaluation/enforcement/
completion traffic is rejected.

## Persistence and restart

State is persisted with a versioned, integrity-checked format: explicit version, deterministic
canonical encoding, checked lengths and arithmetic, bounded allocation, and a CRC-32 footer.
Corruption, truncation, trailing garbage, and unknown versions are rejected. Saves are transactional
(temp, write, flush, verify, atomic rename). After a coordinator restart the coordinator epoch
advances, durable contract/policy/history state is reloaded, dynamic measurements become
`RevalidationRequired`, old-epoch traffic is rejected, and surviving sources/workers must republish
fresh dynamic evidence.

## Distributed proof

A real multiprocess proof uses framed, checksummed TCP. Frames are length-bounded
(magic + kind + flags + length + payload + CRC-32), reject oversized/truncated/corrupt frames, and
handle partial reads/writes. The proof (in `distributed/`) spawns a coordinator and workers as real
OS processes, kills worker A as a real OS process, observes the failure transition, fails over to
worker B, verifies post-recovery compliance, rejects a stale boot id, restarts the coordinator
(epoch advances to 2), and rejects old-epoch worker traffic.

## CUDA proof

When CUDA is available, `cuda/cuda_proof.cu` runs real repeated CUDA kernels on the attached GPU,
measures completed end-to-end latency, computes completed-work throughput, collects real device
memory usage, evaluates configured latency/throughput/memory-pressure objectives, shows different
objectives binding across workloads, emits enforcement intents, verifies the final CUDA output
against a CPU reference, and returns device memory to baseline. Measured values are **REAL**;
configured SLO thresholds are **policy**. No multi-GPU / NVLink / RDMA / MIG / hardware-failover
claims are made. The core builds without CUDA.

## REAL / SYNTHETIC / UNSUPPORTED distinctions

- Measured latency/throughput/device-memory in the CUDA proof are **REAL** (RTX 5090, sm_120).
- Configured SLO thresholds are **policy** (never presented as hardware measurements).
- Multi-GPU, NVLink, RDMA, MIG, hardware failover/preemption, and distributed accelerator metrics are
  **UNSUPPORTED** (not claimed).

## Build and install

Prerequisites: CMake 3.20+, a C++20 toolchain (MSVC on Windows; GCC/Clang elsewhere). CUDA is
optional and isolated (`-DSLOFABRIC_BUILD_CUDA=ON`).

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --build build --config Debug
ctest --test-dir build -C Release
cmake --install build --config Release --prefix install
```

An independent downstream consumer uses `find_package(SLOFabric CONFIG REQUIRED)` and links
`slofabric::slofabric`; a validated consumer project is built and run against the installed package.

## Examples, CLI, benchmarks

- `examples/` - runnable examples for each dimension, plus multi-objective conflict, supersession,
  stale-evidence rejection, enforcement explanation, and error budget.
- `tools/slofabric_cli` - inspection CLI (inspect contract, evaluate, ingest sample, show
  compliance/budgets/binding/intent, what-would-change, inspect state, replay).
- `benchmarks/benchmark_slofabric` - measures completed operations (evidence ingestion,
  single/multi-objective evaluation, percentile, budget, binding, explanation, lookups, replay,
  persistence).

## Thread safety

The public `SloFabric` core is **internally thread-safe**. Every public method that reads or
mutates shared state (contract/policy registration, contract lifecycle, evidence ingestion,
recovery events, budgets, evaluation, enforcement authorization/transition/completion,
persistence, and inspection/query) acquires a single internal mutex, so concurrent use of one
`SloFabric` instance by multiple threads is safe. Lock discipline is explicit: the fabric never
holds its lock while performing an external call or blocking I/O (`dispatch` releases it before
the adapter call; `save` builds the durable snapshot under the lock and writes the file outside
it; `load` reads the file outside the lock and applies it under the lock), and no public method
re-enters the lock. A multi-threaded stress test exercises concurrent evidence ingestion, budget
consumption, and evaluation and proves exact accounting (no lost updates) under concurrency.

## Limitations

- The availability accumulator is cumulative over the window reported by evidence sources; SLO
  Fabric does not implement the distributed telemetry pipeline that would produce it.
- Multi-objective resolution is `HardThenSoft` (and `Lexicographic`) only; no Pareto mode is shipped.
- Recovery-time evaluation relies on explicit authoritative/restored/verified boundaries from an
  adjacent Recovery Planner / Failover Fabric; SLO Fabric does not duplicate recovery planning.
- Cost evaluation requires typed cost evidence; without a cost model it returns insufficient
  evidence (it does not fabricate monetary values).
- The distributed proof uses a reference local process model; it is not a production Failover Fabric.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
