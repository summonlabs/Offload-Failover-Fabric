# Offload Failover Fabric

A standalone, vendor-neutral C++20 runtime that governs the recovery of
offloaded network services when the offload hardware or software that was
serving them disappears.

Offload Failover Fabric (OFF) owns **failover eligibility**, **recovery
intent**, **authority**, **fencing**, **fallback selection** and **verified
recovery state**. It does not diagnose arbitrary hardware faults, implement
device drivers, forward packets, route traffic or execute the offloaded service.
It consumes authoritative failure, capability and dependency evidence and emits
governed failover intent.

```
target disappears  ->  evidence            ->  OFF                    ->  intent
                       capability              qualify                    fence
                       dependency              plan                       lease
                       policy                  commit durably             attempt
                       topology                verify effect              generation
```

## The rule that shapes everything

**Target disappearance is not permission to activate a replacement.**

A vanished target produces an *observation*. Turning that observation into
authority requires authoritative failure evidence that is retained, fresh,
unambiguous, bound to the current placement, bound to the current coordinator
epoch, boot and topology generation — plus an eligible fallback whose capability
evidence is current, compatible and unfenced. If any of those is missing,
unknown, stale or ambiguous, the runtime refuses and says exactly which one.

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requirements: CMake 3.20+, a C++20 compiler. There are no third-party
dependencies. On Windows, MSVC 19.40+ is supported and the first-party warning
profile is `/W4 /permissive- /WX`.

| Option | Default | Effect |
| --- | --- | --- |
| `OFF_BUILD_TOOLS` | `ON` | Build the `off` CLI and the `offd` daemon |
| `OFF_BUILD_BENCHMARKS` | `ON` | Build `off_bench` |
| `OFF_WARNINGS_AS_ERRORS` | `ON` | Treat first-party warnings as errors |
| `OFF_ENABLE_ASAN` | `OFF` | Enable AddressSanitizer when the toolchain provides a runtime |
| `OFF_BUILD_DOWNSTREAM_EXAMPLE` | `OFF` | Configure the packaged downstream consumer |

## Quick start

The CLI runs a complete scenario in one process with `--script`:

```
off --store ./store --script scenario.off
```

```
enroll op operator
enroll topo topology_reporter
enroll rep failure_reporter
enroll cap capability_reporter
enroll exec effect_reporter
target t1 h1 nic1 smart_nic edge 1 1
target t2 h2 dpu1 dpu edge 1 1
topology 1 topo
capability t1 1 ipv4_forward,l4_load_balance 1 1 cap
capability t2 1 ipv4_forward,l4_load_balance,dpu_programmable 1 1 cap
service s1 edge stateless --caps ipv4_forward
place s1 t1 op
fail s1 t1 target_unresponsive rep 1 --inc-host 1 --inc-dev 1
failover s1 --evidence 1
fence s1 t1 1 --inc-host 1 --inc-dev 1
```

The last line reports `FENCE_STALE` with `had_witness: true`: the target that
was recovered away from can no longer claim the placement.

The CLI also runs as a client of the service daemon, and the two surfaces share
one request executor:

```
offd --listen 127.0.0.1:7700 --store ./store
off --connect 127.0.0.1:7700 view
```

## Library

```cpp
#include <off/off.hpp>

off::FabricConfig config;
config.store_directory = "./store";
off::Fabric fabric(config);

fabric.open();
fabric.enroll_source(off::SourceName::literal("op"), off::SourceRole::Operator, nullptr);

off::PlacementRequest placement;
placement.service = off::ServiceName::literal("svc1");
placement.target = off::TargetName::literal("t1");
placement.requester = off::SourceName::literal("op");
const off::RecoveryDecision placed = fabric.establish_placement(placement);

// ... failure evidence arrives, is qualified, and a recovery is requested ...
const off::RecoveryDecision decision = fabric.request_failover(request);
if (decision.accepted) {
  // decision.plan  : bounded, ordered recovery steps
  // decision.intent: the governed, fenced instruction for an executor
  // decision.explanation: why it was legal, step by step
}
```

The public surface is a handful of types: `Fabric`, `FabricConfig`,
`FabricView`, `ServiceView`, `FabricStats`, `RecoveryDecision`,
`RecoveryPlan`, `Explanation`, `FenceCheck`/`FenceVerdict`,
`RestartSummary`, and the request/evidence value types in `off/model.hpp`.

## What it proves

The runtime is built around obligations, and every one of them is discharged by
an executable test. The full mapping lives in [docs/PROOF.md](docs/PROOF.md).

* An old target cannot resume behind a higher durable fence; reuse requires a
  strictly newer incarnation.
* Failover requires a supported, compatible, unfenced fallback. Operator
  authorization never substitutes for capability.
* Unknown, stale, superseded or ambiguous dependencies and evidence block
  recovery rather than being optimistically resolved.
* Duplicate and replayed deliveries are idempotent or explicitly fenced.
* A crash after the durable commit but before acknowledgement leaves the attempt
  classified `OutcomeUnknown` and the recovery downgraded — never successful.
* Failback cannot reuse a superseded incarnation or capability generation.
* Continuity is `Unknown` until a verified effect exists, and a stateful
  service without a verified state transfer reaches `Degraded` at most.
* Accepted state is a deterministic function of accepted evidence, independent
  of delivery order or concurrency.
* Persistence round-trips all correctness-critical state, and a restart advances
  the epoch and boot so that nothing from before the restart is treated as
  current.
* Malformed, truncated, corrupt or oversized input cannot produce a
  valid-looking success.

## Architecture at a glance

| Concern | Mechanism |
| --- | --- |
| Identity | Strongly typed names, opaque handles, monotonic generations and incarnations |
| Evidence | Per-source watermarks; epoch, boot, topology, capability and policy bindings; a policy freshness bound |
| Qualification | Fixed evaluation order; every step contributes an explanation step |
| Fencing | A durable token per service plus a witness recording the incarnation it fenced |
| Planning | Bounded, ordered, mandatory steps; an oversized plan is refused, never shortened silently |
| Commit | Journal transaction, then in-memory application, then rotation |
| Effects | Fenced by token, generation, attempt, incarnation and coordinator epoch |
| Export | Canonical JSON with a semantic digest over the decision-relevant projection |
| Transport | Framed, bounded, checksummed protocol over real sockets |

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Boundaries

OFF consumes topology, observations, capability evidence, dependency health,
policy, authority and execution reports from adjacent runtimes, and emits intent
back to an executor. It absorbs none of them. See
[docs/BOUNDARIES.md](docs/BOUNDARIES.md) for the exact interface and for the
REAL / SYNTHETIC / UNSUPPORTED classification of everything in this repository.

In short: the durable store, the loopback transport and the multi-process
daemon behaviour are real; every SmartNIC, DPU and host target used in tests and
benchmarks is a synthetic fixture; switch, ASIC, RDMA, InfiniBand, NVLink, CUDA
and vendor-protocol behaviour is unsupported and unclaimed.

## Status

Version 1.0.0. Every capability described in this repository is implemented and
covered by the test suite; nothing here is a stub, a plan or an aspiration.
Genuine limitations, including the absence of AddressSanitizer coverage in the
reference environment, are documented rather than hidden.

## Documentation

* [Architecture](docs/ARCHITECTURE.md) — layering, state model, determinism, persistence, concurrency
* [Boundaries](docs/BOUNDARIES.md) — what OFF owns, what it refuses to own, and fidelity labels
* [Proof obligations](docs/PROOF.md) — each obligation and the test that discharges it
* [Validation](docs/VALIDATION.md) — toolchain, configurations, test inventory, sanitizer status, packaging

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
