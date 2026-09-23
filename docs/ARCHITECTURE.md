# Architecture

Offload Failover Fabric (OFF) is a standalone C++20 runtime that governs the
recovery of offloaded network services when the offload hardware or software
that was serving them disappears. It owns failover eligibility, recovery intent,
authority, fencing, fallback selection and verified recovery state. It does not
diagnose arbitrary hardware faults, implement device drivers, forward packets,
route traffic or execute the offloaded service.

## Layering

```
                 evidence in                              intent out
   failure / capability / dependency / topology  ->  governed FailoverIntent
                        |                                    ^
                        v                                    |
   +--------------------------------------------------------------+
   |  Fabric                                                      |
   |   qualification -> dependency qualification -> fallback       |
   |   selection -> bounded plan -> durable commit -> effect       |
   |   verification -> continuity classification                   |
   +--------------------------------------------------------------+
        |             |              |               |
     Canonical     Durable        Explanation      Intent
      codec        store            log            queue
```

| Module | Header | Responsibility |
| --- | --- | --- |
| Strong types | `off/strong.hpp` | Checked arithmetic, `Id`, `Gen`, `Name`, quantities |
| Reason codes | `off/reason.hpp` | Append-only table of stable machine-readable codes |
| Digests | `off/digest.hpp` | CRC-32C integrity, 128-bit canonical digests |
| Canonical codec | `off/canonical.hpp` | Big-endian encoding, bounded decoding, canonical JSON |
| Domain model | `off/model.hpp` | Services, targets, dependencies, capability, policy, evidence |
| Planning | `off/plan.hpp` | Bounded recovery plans and decisions |
| Explanations | `off/explain.hpp` | Ordered reason traces with a semantic digest |
| Store | `off/store.hpp` | Versioned, checksummed, transactional journal plus snapshots |
| Fabric | `off/fabric.hpp` | The runtime itself |
| Protocol | `off/protocol.hpp` | Framed bounded wire protocol, sockets, request executor |

## Identity, generations and incarnations

Nothing correctness-critical is carried by a bare integer or an untyped string.

* **Names** (`ServiceName`, `ScopeName`, `HostName`, `DeviceName`,
  `TargetName`, `SourceName`) are distinct types per tag, validated to a
  bounded canonical character set. Parsing is the only route from external bytes
  into a name.
* **Handles** (`EvidenceId`, `AttemptId`, `LeaseId`, `EffectId`,
  `RequestKey`, `PlanId`) are opaque and minted by the coordinator. Zero is
  never a real identity.
* **Generations** (`TopologyGeneration`, `CapabilityGeneration`,
  `PolicyGeneration`, `FailoverGeneration`, `StateGeneration`,
  `LeaseTerm`, `FenceToken`, `Incarnation`, `CoordinatorEpoch`,
  `BootId`) are monotonic and are only ever advanced through a checked
  successor; exhaustion is reported, never wrapped.
* **Logical tick** is a coordinator clock, not wall time. It advances only when
  an operation reaches its commit phase, so evidence ages deterministically.

## Placement, recovery and continuity

```
                       establish_placement
                               |
                               v
   Registered ---------> Active ---------> RecoveryPending
                               |                   |
                     request_failover      evidence ambiguous
                               |                   |
                               v                   v
                     RecoveryAuthorized     AmbiguityPending
                               |                   |
                      intent acknowledged    operator resolution
                               |                   |
                               v                   v
                     ActivationRequested  Active  or  Failed  or  recovery
                               |
                    activation effect reported
                               |
                               v
                       EffectReported ----------------> Verified
                               |                            |
                     negative effect                 healthy + transfer
                               v                            v
                           Failed                  FallbackActive (Full/Degraded)
```

Continuity begins `Unknown` and is only ever raised by a verified effect.
A stateful service whose state transfer has not been verified can reach
`Degraded` at most, and only if policy allows it; otherwise continuity stays
`None` and the runtime says so explicitly.

## Fencing

Every time a placement is established or recovered, a durable fence token for
that service advances. The token is only ever minted upward and it survives
restarts. When a recovery moves a service off a target, the runtime records a
*witness* for that target: the token at which it was fenced and the incarnation
it held at that moment.

A target becomes an eligible candidate again only once its **incarnation**
advances past the incarnation that was fenced. Re-adopting a target at the same
incarnation does not release it. `Fabric::verify_fence` is the public surface
that answers a placement claim from a target, and it refuses a token below the
current one, a token above the current one, a token presented by the wrong
holder, and a holder whose incarnation is still fenced.

## Deterministic qualification pipeline

A recovery request is evaluated in a fixed order, and each step appends an
explanation step:

1. Runtime open, lifecycle gate, attempt ceiling.
2. Failure evidence: presence, retention, service match, placement match,
   coordinator epoch, boot, freshness against the policy age bound, topology
   generation, policy generation, ambiguity.
3. Operator-authorization precondition.
4. Dependency qualification over the transitive graph, bounded by the policy
   depth ceiling, with cycle detection.
5. Fallback selection over an ordered candidate list, bounded by the candidate
   ceiling, evaluating scope, incarnation, fence witness, capability evidence
   presence and freshness, capability compatibility and health.
6. Bounded plan construction.
7. Durable commit: transaction to the journal, then in-memory application, then
   rotation if the journal passed its bound.
8. Intent emission into a bounded queue.

Refusals carry the same explanation structure as acceptances, and a refusal
after planning carries the plan it evaluated.

## Determinism

The canonical export separates *decision-relevant* state from *observation*
state. `FabricView::semantic_json` contains only the former, and
`FabricView::semantic_digest` is a digest of it. Two fabrics that accepted the
same evidence produce the same semantic digest regardless of the order in which
that evidence arrived, provided each reporting source's own sequence order is
preserved — which the runtime enforces as a watermark. Logical tick stamps,
explanation logs and statistics are deliberately excluded because they describe
delivery, not the decision.

## Persistence

```
<store>/
  snapshot-00000002.ofab    magic | version | length | payload CRC | payload | header CRC
  journal-00000002.ofab     repeated frames:
                            magic | version | kind | length | sequence | payload CRC
                            payload | frame CRC
```

A journal transaction is `Begin(id)` … records … `Commit(id)`. Only complete
transactions are applied. A torn tail is truncated at the last complete frame; a
damaged frame that is followed by a valid one is treated as corruption and the
store refuses to open. Compaction writes a new snapshot generation and starts a
new journal; only after the in-memory state reflects the commit, so a snapshot
can never replace the journal that holds the only durable copy of a change.

## Concurrency

All fabric state is guarded by a single mutex. The lock order is
`state -> cancellation registry -> intent queue`; nothing acquired while
holding an inner lock ever takes an outer one. Store methods never call back
into the fabric. Intent emission happens after the state mutation and never
under a callback. The daemon bounds its worker count, its accept queue and its
frame size before any work is admitted, and shutdown closes the listening socket
and interrupts every live connection so no worker can outlive it.

## Transport

The wire format is a framed, bounded, checksummed protocol:

```
u32 magic | u16 protocol_version | u16 message_kind | u64 correlation
u32 payload_length | u32 payload_crc32c | payload
```

Requests and responses are canonical binary; the response body is canonical
JSON. The same `execute_request` function backs the daemon and the CLI local
mode, so the two surfaces cannot drift apart.
