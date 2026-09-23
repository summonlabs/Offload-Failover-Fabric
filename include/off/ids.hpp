// Offload Failover Fabric - the complete set of correctness-critical typed
// identities, generations, incarnations, epochs and counters.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "off/strong.hpp"

namespace off {

// Tag types. Each tag yields an incompatible family of identities.
struct ServiceTag;
struct ScopeTag;
struct HostTag;
struct DeviceTag;
struct TargetTag;
struct SourceTag;
struct EvidenceTag;
struct AttemptTag;
struct LeaseIdTag;
struct LeaseTermTag;
struct FenceTag;
struct PolicyTag;
struct TopologyTag;
struct CapabilityTag;
struct FailoverTag;
struct EpochTag;
struct BootTag;
struct IncarnationTag;
struct PlanTag;
struct RequestTag;
struct EffectTag;
struct StateTag;
struct EvidenceSeqTag;
struct TickTag;

// Names: the stable, human-inspectable identity of a registered entity.
using ServiceName = Name<ServiceTag>;
using ScopeName = Name<ScopeTag>;
using HostName = Name<HostTag>;
using DeviceName = Name<DeviceTag>;
using TargetName = Name<TargetTag>;
using SourceName = Name<SourceTag>;

// Opaque handles minted by the coordinator. Nil is never a valid identity.
using EvidenceId = Id<EvidenceTag>;
using AttemptId = Id<AttemptTag>;
using LeaseId = Id<LeaseIdTag>;
using EffectId = Id<EffectTag>;
using RequestKey = Id<RequestTag>;
using PlanId = Id<PlanTag>;

// Monotonic generations. Zero means "never advanced".
using TopologyGeneration = Gen<TopologyTag>;
using CapabilityGeneration = Gen<CapabilityTag>;
using PolicyGeneration = Gen<PolicyTag>;
using FailoverGeneration = Gen<FailoverTag>;
using StateGeneration = Gen<StateTag>;
using LeaseTerm = Gen<LeaseTermTag>;
using FenceToken = Gen<FenceTag>;
using Incarnation = Gen<IncarnationTag>;
using CoordinatorEpoch = Gen<EpochTag>;
using BootId = Gen<BootTag>;
using EvidenceSeq = Seq<EvidenceSeqTag>;

// Logical coordinator time. Never wall clock.
using LogicalTick = Tick<TickTag>;

}  // namespace off
