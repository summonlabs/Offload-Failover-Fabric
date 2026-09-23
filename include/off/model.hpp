// Offload Failover Fabric - the governed domain model. Every entity carries
// explicit typed identity, generation, incarnation, lifecycle and provenance.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "off/canonical.hpp"
#include "off/ids.hpp"

namespace off {

class CanonicalWriter;
class CanonicalReader;
class JsonValue;

// ---------------------------------------------------------------------------
// Topology: hosts, offload devices and governed scopes.
// ---------------------------------------------------------------------------

/// Kind of offload endpoint. Synthetic endpoints exist only to exercise the
/// runtime contract and are labelled SYNTHETIC everywhere they surface.
enum class DeviceKind : u8 {
  HostCpu = 0,
  SmartNic = 1,
  Dpu = 2,
  Synthetic = 3,
};

inline constexpr std::size_t kDeviceKindCount = 4;
[[nodiscard]] std::string_view device_kind_text(DeviceKind kind) noexcept;
[[nodiscard]] bool device_kind_parse(std::string_view text, DeviceKind& out) noexcept;
/// The canonical REAL / SYNTHETIC / UNSUPPORTED label for a device kind.
[[nodiscard]] std::string_view device_kind_fidelity(DeviceKind kind) noexcept;

/// Immutable boot identity of a concrete host and device. Incarnations advance
/// when a target is re-adopted; they never move backwards.
struct TargetIncarnation {
  Incarnation host{};
  Incarnation device{};

  friend bool operator==(const TargetIncarnation&, const TargetIncarnation&) noexcept = default;
  friend auto operator<=>(const TargetIncarnation&, const TargetIncarnation&) noexcept = default;
};

/// A concrete placement: which target, at which incarnation.
struct TargetRef {
  TargetName target{};
  TargetIncarnation incarnation{};

  [[nodiscard]] bool valid() const noexcept { return !target.empty(); }
  friend bool operator==(const TargetRef&, const TargetRef&) noexcept = default;
  friend auto operator<=>(const TargetRef&, const TargetRef&) noexcept = default;
};

/// Topology record for one governed offload endpoint.
struct TargetRecord {
  TargetName name{};
  HostName host{};
  DeviceName device{};
  DeviceKind kind{DeviceKind::Synthetic};
  ScopeName scope{};
  TargetIncarnation incarnation{};
  TopologyGeneration topology_generation{};

  friend bool operator==(const TargetRecord&, const TargetRecord&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Capability evidence.
// ---------------------------------------------------------------------------

/// Offload capabilities a service function may require of a target.
enum class CapabilityCode : u8 {
  IPv4Forward = 0,
  IPv6Forward = 1,
  L4LoadBalance = 2,
  TlsTerminate = 3,
  VxlanEncap = 4,
  Srv6Encap = 5,
  StatefulFirewall = 6,
  RdmaVerbs = 7,
  DpuProgrammable = 8,
  RateLimit = 9,
  SyntheticDatapath = 10,
};

inline constexpr std::size_t kCapabilityCodeCount = 11;
[[nodiscard]] std::string_view capability_code_text(CapabilityCode code) noexcept;
[[nodiscard]] bool capability_code_parse(std::string_view text, CapabilityCode& out) noexcept;

/// Canonical, order-independent capability set. Iteration is always in
/// ascending code order so encodings and explanations are stable.
class CapabilitySet {
 public:
  CapabilitySet() = default;

  void insert(CapabilityCode code) noexcept;
  void erase(CapabilityCode code) noexcept;
  [[nodiscard]] bool contains(CapabilityCode code) const noexcept;
  [[nodiscard]] bool contains_all(const CapabilitySet& other) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return mask_ == 0; }
  [[nodiscard]] u64 mask() const noexcept { return mask_; }
  [[nodiscard]] static CapabilitySet from_mask(u64 mask) noexcept;
  [[nodiscard]] std::vector<CapabilityCode> codes() const;
  [[nodiscard]] std::string render() const;

  friend bool operator==(const CapabilitySet&, const CapabilitySet&) noexcept = default;
  friend auto operator<=>(const CapabilitySet&, const CapabilitySet&) noexcept = default;

 private:
  u64 mask_{0};
};

/// Capability evidence supplied by an authoritative reporter. A capability
/// generation is only usable while it is the newest generation for its target
/// and is still bound to the current topology generation.
struct CapabilityEvidence {
  TargetName target{};
  CapabilityGeneration generation{};
  CapabilitySet capabilities{};
  TopologyGeneration topology_generation{};
  TargetIncarnation incarnation{};
  SourceName source{};
  LogicalTick tick{};
  Digest content_digest{};

  friend bool operator==(const CapabilityEvidence&, const CapabilityEvidence&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Service descriptors.
// ---------------------------------------------------------------------------

enum class Statefulness : u8 { Stateless = 0, Stateful = 1 };
[[nodiscard]] std::string_view statefulness_text(Statefulness value) noexcept;
[[nodiscard]] bool statefulness_parse(std::string_view text, Statefulness& out) noexcept;

/// What must be true before continuity may be claimed for a service.
enum class Precondition : u8 {
  FenceAdvance = 0,
  VerifiedEffect = 1,
  StateTransfer = 2,
  DependencyHealth = 3,
  OperatorAuthorization = 4,
  FallbackCapabilitySupport = 5,
};

inline constexpr std::size_t kPreconditionCount = 6;
[[nodiscard]] std::string_view precondition_text(Precondition value) noexcept;
[[nodiscard]] bool precondition_parse(std::string_view text, Precondition& out) noexcept;

class PreconditionSet {
 public:
  PreconditionSet() = default;
  void insert(Precondition value) noexcept;
  [[nodiscard]] bool contains(Precondition value) const noexcept;
  [[nodiscard]] u32 mask() const noexcept { return mask_; }
  [[nodiscard]] static PreconditionSet from_mask(u32 mask) noexcept;
  [[nodiscard]] std::vector<Precondition> values() const;

  friend bool operator==(const PreconditionSet&, const PreconditionSet&) noexcept = default;

 private:
  u32 mask_{0};
};

enum class DependencyKind : u8 { Hard = 0, Soft = 1 };
inline constexpr std::size_t kDependencyKindCount = 2;
[[nodiscard]] std::string_view dependency_kind_text(DependencyKind kind) noexcept;
[[nodiscard]] bool dependency_kind_parse(std::string_view text, DependencyKind& out) noexcept;

/// What the dependency must provide for a candidate target to be acceptable.
enum class DependencyRequirement : u8 {
  AnyTarget = 0,
  SameScope = 1,
  DistinctTarget = 2,
};
inline constexpr std::size_t kDependencyRequirementCount = 3;
[[nodiscard]] std::string_view dependency_requirement_text(DependencyRequirement value) noexcept;
[[nodiscard]] bool dependency_requirement_parse(std::string_view text, DependencyRequirement& out) noexcept;

struct DependencySpec {
  ServiceName service{};
  DependencyKind kind{DependencyKind::Hard};
  DependencyRequirement requirement{DependencyRequirement::AnyTarget};

  friend bool operator==(const DependencySpec&, const DependencySpec&) noexcept = default;
  friend auto operator<=>(const DependencySpec&, const DependencySpec&) noexcept = default;
};

struct ServiceDescriptor {
  ServiceName name{};
  ScopeName scope{};
  Statefulness statefulness{Statefulness::Stateless};
  CapabilitySet required_capabilities{};
  PreconditionSet preconditions{};
  std::vector<DependencySpec> dependencies{};
  StateGeneration last_known_state_generation{};

  friend bool operator==(const ServiceDescriptor&, const ServiceDescriptor&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Failure and dependency evidence.
// ---------------------------------------------------------------------------

enum class FailureClass : u8 {
  TargetMissing = 0,
  TargetUnresponsive = 1,
  ServiceCrash = 2,
  LinkDown = 3,
  OffloadEngineFault = 4,
  PlannedMaintenance = 5,
  OperatorDeclared = 6,
  SyntheticInjected = 7,
};

inline constexpr std::size_t kFailureClassCount = 8;
[[nodiscard]] std::string_view failure_class_text(FailureClass value) noexcept;
[[nodiscard]] bool failure_class_parse(std::string_view text, FailureClass& out) noexcept;
/// True when the class is only ever produced by test fixtures.
[[nodiscard]] bool failure_class_is_synthetic(FailureClass value) noexcept;

/// Distinguishes an unambiguous report from one that cannot justify unilateral
/// activation. Ambiguity is a first-class outcome, never collapsed to failure.
enum class AmbiguityState : u8 {
  Unambiguous = 0,
  Ambiguous = 1,
  SplitBrainSuspected = 2,
};
inline constexpr std::size_t kAmbiguityStateCount = 3;
[[nodiscard]] std::string_view ambiguity_state_text(AmbiguityState value) noexcept;
[[nodiscard]] bool ambiguity_state_parse(std::string_view text, AmbiguityState& out) noexcept;

/// Provenance and freshness stamp. A stamp names the coordinator epoch and boot
/// that accepted the observation, so evidence from a previous boot can never be
/// mistaken for current evidence.
struct Provenance {
  SourceName source{};
  EvidenceSeq sequence{};
  LogicalTick tick{};
  CoordinatorEpoch epoch{};
  BootId boot{};

  friend bool operator==(const Provenance&, const Provenance&) noexcept = default;
};

struct FailureEvidence {
  EvidenceId id{};
  ServiceName service{};
  TargetRef target{};
  FailureClass failure_class{FailureClass::TargetUnresponsive};
  AmbiguityState ambiguity{AmbiguityState::Unambiguous};
  Provenance provenance{};
  TopologyGeneration topology_generation{};
  CapabilityGeneration capability_generation{};
  PolicyGeneration policy_generation{};
  Digest content_digest{};

  friend bool operator==(const FailureEvidence&, const FailureEvidence&) noexcept = default;
};

enum class DependencyHealth : u8 {
  Unknown = 0,
  Healthy = 1,
  Degraded = 2,
  Failed = 3,
};
inline constexpr std::size_t kDependencyHealthCount = 4;
[[nodiscard]] std::string_view dependency_health_text(DependencyHealth value) noexcept;
[[nodiscard]] bool dependency_health_parse(std::string_view text, DependencyHealth& out) noexcept;

struct DependencyObservation {
  ServiceName service{};
  ServiceName dependency{};
  DependencyHealth health{DependencyHealth::Unknown};
  TargetRef observed_target{};
  Provenance provenance{};
  TopologyGeneration topology_generation{};
  Digest content_digest{};

  friend bool operator==(const DependencyObservation&, const DependencyObservation&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Policy.
// ---------------------------------------------------------------------------

struct PolicyDescriptor {
  PolicyGeneration generation{};
  ScopeName scope{};
  u32 max_attempts_per_generation{4};
  u32 max_plan_steps{16};
  u32 max_fallback_candidates{32};
  u32 evidence_max_age_ticks{1000000};
  u32 dependency_max_depth{8};
  bool require_operator_for_ambiguous{true};
  bool allow_degraded_continuity{true};
  bool allow_failback{true};

  friend bool operator==(const PolicyDescriptor&, const PolicyDescriptor&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Leases, fences and authority.
// ---------------------------------------------------------------------------

/// Durable fence. The token is monotonic per service and survives restarts and
/// coordinator changes; any holder presenting a lower token is fenced out.
struct FenceRecord {
  ServiceName service{};
  FenceToken token{};
  TargetRef holder{};
  AttemptId attempt{};
  CoordinatorEpoch epoch{};
  LogicalTick issued{};

  friend bool operator==(const FenceRecord&, const FenceRecord&) noexcept = default;
};

struct LeaseRecord {
  ServiceName service{};
  LeaseId id{};
  LeaseTerm term{};
  ScopeName scope{};
  TargetRef holder{};
  CoordinatorEpoch epoch{};
  LogicalTick issued{};

  friend bool operator==(const LeaseRecord&, const LeaseRecord&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Lifecycle, recovery and continuity.
// ---------------------------------------------------------------------------

enum class LifecyclePhase : u8 {
  Unregistered = 0,
  Registered = 1,
  Active = 2,
  RecoveryPending = 3,
  RecoveryAuthorized = 4,
  RecoveryInFlight = 5,
  FallbackActive = 6,
  Failed = 7,
  Withdrawn = 8,
};
inline constexpr std::size_t kLifecyclePhaseCount = 9;
[[nodiscard]] std::string_view lifecycle_phase_text(LifecyclePhase value) noexcept;
[[nodiscard]] bool lifecycle_phase_parse(std::string_view text, LifecyclePhase& out) noexcept;

enum class RecoveryPhase : u8 {
  None = 0,
  IntentRecorded = 1,
  Authorized = 2,
  ActivationRequested = 3,
  EffectReported = 4,
  Verified = 5,
  Refused = 6,
  AmbiguityPending = 7,
};
inline constexpr std::size_t kRecoveryPhaseCount = 8;
[[nodiscard]] std::string_view recovery_phase_text(RecoveryPhase value) noexcept;
[[nodiscard]] bool recovery_phase_parse(std::string_view text, RecoveryPhase& out) noexcept;

/// Continuity is never assumed. It begins Unknown and can only reach Full or
/// Degraded through a verified effect, or None through an explicit refusal.
enum class ContinuityClass : u8 {
  Unknown = 0,
  None = 1,
  Degraded = 2,
  Full = 3,
};
inline constexpr std::size_t kContinuityClassCount = 4;
[[nodiscard]] std::string_view continuity_class_text(ContinuityClass value) noexcept;
[[nodiscard]] bool continuity_class_parse(std::string_view text, ContinuityClass& out) noexcept;

enum class AttemptOutcome : u8 {
  Pending = 0,
  Committed = 1,
  Acknowledged = 2,
  EffectReported = 3,
  Verified = 4,
  Refused = 5,
  Cancelled = 6,
  OutcomeUnknown = 7,
  Superseded = 8,
};
inline constexpr std::size_t kAttemptOutcomeCount = 9;
[[nodiscard]] std::string_view attempt_outcome_text(AttemptOutcome value) noexcept;
[[nodiscard]] bool attempt_outcome_parse(std::string_view text, AttemptOutcome& out) noexcept;

/// Persistent failover attempt record. The durable_committed flag records that
/// the attempt survived a durable commit; the acknowledged flag records only
/// that a caller learned about it. Neither implies verified effect.
struct AttemptRecord {
  AttemptId id{};
  ServiceName service{};
  FailoverGeneration generation{};
  FenceToken fence{};
  TargetRef from{};
  TargetRef to{};
  LeaseTerm lease_term{};
  CoordinatorEpoch epoch{};
  LogicalTick opened{};
  RecoveryPhase phase{RecoveryPhase::None};
  AttemptOutcome outcome{AttemptOutcome::Pending};
  bool durable_committed{false};
  bool acknowledged{false};
  bool restart_downgraded{false};
  Reason last_reason{Reason::Ok};

  friend bool operator==(const AttemptRecord&, const AttemptRecord&) noexcept = default;
};

/// Governed failover intent. This is the only thing the fabric emits towards an
/// executor; it carries the fence and generation that make it legal.
struct FailoverIntent {
  AttemptId attempt{};
  ServiceName service{};
  FailoverGeneration generation{};
  FenceToken fence{};
  LeaseTerm lease_term{};
  TargetRef from{};
  TargetRef to{};
  CoordinatorEpoch epoch{};
  PolicyGeneration policy_generation{};
  TopologyGeneration topology_generation{};
  LogicalTick issued{};
  bool reissue_after_restart{false};

  friend bool operator==(const FailoverIntent&, const FailoverIntent&) noexcept = default;
};

enum class EffectKind : u8 {
  ActivationAccepted = 0,
  ServiceHealthy = 1,
  StateTransferComplete = 2,
  DeactivationAcknowledged = 3,
};
inline constexpr std::size_t kEffectKindCount = 4;
[[nodiscard]] std::string_view effect_kind_text(EffectKind value) noexcept;
[[nodiscard]] bool effect_kind_parse(std::string_view text, EffectKind& out) noexcept;

enum class EffectResult : u8 { Positive = 0, Negative = 1 };
inline constexpr std::size_t kEffectResultCount = 2;
[[nodiscard]] std::string_view effect_result_text(EffectResult value) noexcept;
[[nodiscard]] bool effect_result_parse(std::string_view text, EffectResult& out) noexcept;

struct EffectReport {
  EffectId id{};
  ServiceName service{};
  AttemptId attempt{};
  FailoverGeneration generation{};
  FenceToken fence{};
  TargetRef target{};
  EffectKind kind{EffectKind::ActivationAccepted};
  EffectResult result{EffectResult::Positive};
  StateGeneration state_generation{};
  Provenance provenance{};

  friend bool operator==(const EffectReport&, const EffectReport&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Requests.
// ---------------------------------------------------------------------------

enum class RecoveryTrigger : u8 {
  FailureEvidence = 0,
  PlannedMaintenance = 1,
  OperatorCommand = 2,
};
inline constexpr std::size_t kRecoveryTriggerCount = 3;
[[nodiscard]] std::string_view recovery_trigger_text(RecoveryTrigger value) noexcept;
[[nodiscard]] bool recovery_trigger_parse(std::string_view text, RecoveryTrigger& out) noexcept;

struct FailoverRequest {
  RequestKey key{};
  ServiceName service{};
  RecoveryTrigger trigger{RecoveryTrigger::FailureEvidence};
  EvidenceId evidence{};
  SourceName requester{};
  /// Explicit operator consent. It authorizes the request but never replaces a
  /// requirement for supported, compatible fallback capability.
  bool operator_authorized{false};

  friend bool operator==(const FailoverRequest&, const FailoverRequest&) noexcept = default;
};

struct AmbiguityResolution {
  RequestKey key{};
  ServiceName service{};
  EvidenceId evidence{};
  /// True when an operator declares the disappearance real and consents to
  /// recovery; false when the operator declares the target alive.
  bool confirm_failure{false};
  SourceName resolved_by{};

  friend bool operator==(const AmbiguityResolution&, const AmbiguityResolution&) noexcept = default;
};

/// Establishes the first authoritative placement for a service that has none.
/// It is the only way an active placement comes into existence; recovery then
/// moves that placement under a new failover generation and a higher fence.
struct PlacementRequest {
  RequestKey key{};
  ServiceName service{};
  TargetName target{};
  SourceName requester{};

  friend bool operator==(const PlacementRequest&, const PlacementRequest&) noexcept = default;
};

struct FailbackRequest {
  RequestKey key{};
  ServiceName service{};
  TargetName preferred_target{};
  TopologyGeneration observed_topology_generation{};
  PolicyGeneration observed_policy_generation{};
  SourceName requester{};

  friend bool operator==(const FailbackRequest&, const FailbackRequest&) noexcept = default;
};

struct IntentAck {
  RequestKey key{};
  AttemptId attempt{};
  FenceToken fence{};
  FailoverGeneration generation{};
  bool accepted{false};
  SourceName executor{};
};

// ---------------------------------------------------------------------------
// Canonical codecs.
// ---------------------------------------------------------------------------

void encode(CanonicalWriter& writer, const TargetIncarnation& value);
bool decode(CanonicalReader& reader, TargetIncarnation& value);

void encode(CanonicalWriter& writer, const TargetRef& value);
bool decode(CanonicalReader& reader, TargetRef& value);

void encode(CanonicalWriter& writer, const TargetRecord& value);
bool decode(CanonicalReader& reader, TargetRecord& value);

void encode(CanonicalWriter& writer, const CapabilitySet& value);
bool decode(CanonicalReader& reader, CapabilitySet& value);

void encode(CanonicalWriter& writer, const CapabilityEvidence& value);
bool decode(CanonicalReader& reader, CapabilityEvidence& value);

void encode(CanonicalWriter& writer, const PreconditionSet& value);
bool decode(CanonicalReader& reader, PreconditionSet& value);

void encode(CanonicalWriter& writer, const DependencySpec& value);
bool decode(CanonicalReader& reader, DependencySpec& value);

void encode(CanonicalWriter& writer, const ServiceDescriptor& value);
bool decode(CanonicalReader& reader, ServiceDescriptor& value);

void encode(CanonicalWriter& writer, const Provenance& value);
bool decode(CanonicalReader& reader, Provenance& value);

void encode(CanonicalWriter& writer, const FailureEvidence& value);
bool decode(CanonicalReader& reader, FailureEvidence& value);

void encode(CanonicalWriter& writer, const DependencyObservation& value);
bool decode(CanonicalReader& reader, DependencyObservation& value);

void encode(CanonicalWriter& writer, const PolicyDescriptor& value);
bool decode(CanonicalReader& reader, PolicyDescriptor& value);

void encode(CanonicalWriter& writer, const FenceRecord& value);
bool decode(CanonicalReader& reader, FenceRecord& value);

void encode(CanonicalWriter& writer, const LeaseRecord& value);
bool decode(CanonicalReader& reader, LeaseRecord& value);

void encode(CanonicalWriter& writer, const AttemptRecord& value);
bool decode(CanonicalReader& reader, AttemptRecord& value);

void encode(CanonicalWriter& writer, const FailoverIntent& value);
bool decode(CanonicalReader& reader, FailoverIntent& value);

void encode(CanonicalWriter& writer, const EffectReport& value);
bool decode(CanonicalReader& reader, EffectReport& value);

// ---------------------------------------------------------------------------
// JSON renderers (deterministic; keys are lexicographically ordered).
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue to_json(const TargetIncarnation& value);
[[nodiscard]] JsonValue to_json(const TargetRef& value);
[[nodiscard]] JsonValue to_json(const TargetRecord& value);
[[nodiscard]] JsonValue to_json(const CapabilitySet& value);
[[nodiscard]] JsonValue to_json(const CapabilityEvidence& value);
[[nodiscard]] JsonValue to_json(const ServiceDescriptor& value);
[[nodiscard]] JsonValue to_json(const Provenance& value);
[[nodiscard]] JsonValue to_json(const FailureEvidence& value);
[[nodiscard]] JsonValue to_json(const DependencyObservation& value);
[[nodiscard]] JsonValue to_json(const PolicyDescriptor& value);
[[nodiscard]] JsonValue to_json(const FenceRecord& value);
[[nodiscard]] JsonValue to_json(const LeaseRecord& value);
[[nodiscard]] JsonValue to_json(const AttemptRecord& value);
[[nodiscard]] JsonValue to_json(const FailoverIntent& value);
[[nodiscard]] JsonValue to_json(const EffectReport& value);

/// Content digest of a value canonical encoding. Used to detect conflicting
/// evidence that claims the same identity and sequence.
template <class T>
[[nodiscard]] Digest canonical_digest(const T& value) {
  CanonicalWriter writer;
  encode(writer, value);
  return writer.digest();
}

}  // namespace off
