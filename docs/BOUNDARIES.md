# Systems boundary

## What this runtime owns

* **Failover eligibility** — whether a service may be recovered at all, given
  evidence, dependencies, capability and policy.
* **Recovery intent** — the governed, fenced instruction that a fallback should
  become active. This is the only thing OFF emits towards an executor.
* **Authority** — coordinator epoch, boot identity, lease term and the
  authorization attached to a decision.
* **Fencing** — the durable token that proves an old target may not resume, and
  the witness that keeps a fenced target out until its incarnation advances.
* **Fallback selection** — deterministic, ordered, bounded candidate choice.
* **Verified recovery state** — what was actually confirmed to have happened,
  and therefore what continuity may be claimed.

## What this runtime deliberately does not do

* It does not diagnose arbitrary hardware faults. It consumes authoritative
  failure evidence and qualifies it; it never invents it.
* It does not implement device drivers, program SmartNICs or DPUs, or touch
  vendor registers.
* It does not forward packets, install routes, or manipulate data planes.
* It does not execute the offloaded service, checkpoint it, or move its state.
  It consumes a state-transfer effect report.
* It does not decide policy. Policy is an input, identified by generation.

## Adjacent runtimes

| Neighbour | Provides | OFF consumes as |
| --- | --- | --- |
| Topology service | Target records, host/device incarnations, topology generation | `TargetRecord`, `set_topology_generation` |
| Observation/failure reporters | Failure evidence with source and sequence | `FailureEvidence` |
| Capability service | What each target currently supports | `CapabilityEvidence` |
| Dependency monitor | Health of services OFF depends on | `DependencyObservation` |
| Policy engine | Budgets, ceilings, freshness bounds | `PolicyDescriptor` |
| Authority/consensus layer | Who is the coordinator, at which epoch | `CoordinatorEpoch`, `BootId` |
| Executor | Applied the placement, reports what happened | `IntentAck`, `EffectReport` |

Each of these is an explicit typed interface. OFF never reaches around them.

## REAL / SYNTHETIC / UNSUPPORTED

The runtime is vendor-neutral and hardware-agnostic. It has been exercised only
against synthetic targets and loopback transport.

| Element | Classification | Note |
| --- | --- | --- |
| `DeviceKind::SmartNic` target records | **SYNTHETIC** | Fixtures only; no NIC was programmed |
| `DeviceKind::Dpu` target records | **SYNTHETIC** | Fixtures only; no DPU was programmed |
| `DeviceKind::HostCpu` targets | **SYNTHETIC** | Fixtures only |
| `DeviceKind::Synthetic` targets | **SYNTHETIC** | Explicitly labelled synthetic |
| `device_kind_fidelity` | — | Reports **REAL** for the SmartNIC/DPU *kinds* because those kinds name real device classes; every instance used in this repository is a synthetic fixture |
| `FailureClass::SyntheticInjected` | **SYNTHETIC** | Only ever produced by fixtures |
| Loopback sockets, `offd`, `off` | **REAL** | Real OS processes, real TCP sockets, real framing |
| Durable journal and snapshots | **REAL** | Real files, real fsync, real crash boundaries |
| Switch/ASIC/RDMA/InfiniBand/NVLink/CUDA behaviour | **UNSUPPORTED** | No such hardware was used and no vendor protocol is implemented |
| SmartNIC/DPU firmware or driver interaction | **UNSUPPORTED** | Out of boundary; OFF emits intent only |
| Multi-host fabric behaviour | **UNSUPPORTED** | Concurrency is proven within one process and across processes on one host |
| AddressSanitizer coverage | **UNSUPPORTED** | The toolchain in use ships no ASan runtime; see `docs/VALIDATION.md` |

No claim in this repository rests on hardware that was not present.
