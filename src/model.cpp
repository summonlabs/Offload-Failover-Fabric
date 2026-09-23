// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "off/model.hpp"

#include <array>
#include <utility>

#include "detail/codec_util.hpp"

namespace off {
namespace {

template <class E, std::size_t N>
[[nodiscard]] std::string_view lookup_text(
    const std::array<std::pair<E, std::string_view>, N>& table, E value) noexcept {
  for (const auto& entry : table) {
    if (entry.first == value) {
      return entry.second;
    }
  }
  return "unknown";
}

template <class E, std::size_t N>
[[nodiscard]] bool lookup_value(const std::array<std::pair<E, std::string_view>, N>& table,
                                std::string_view text, E& out) noexcept {
  for (const auto& entry : table) {
    if (entry.second == text) {
      out = entry.first;
      return true;
    }
  }
  return false;
}

constexpr std::array<std::pair<DeviceKind, std::string_view>, kDeviceKindCount> kDeviceKinds = {{
    {DeviceKind::HostCpu, "host_cpu"},
    {DeviceKind::SmartNic, "smart_nic"},
    {DeviceKind::Dpu, "dpu"},
    {DeviceKind::Synthetic, "synthetic"},
}};

constexpr std::array<std::pair<CapabilityCode, std::string_view>, kCapabilityCodeCount>
    kCapabilityCodes = {{
        {CapabilityCode::IPv4Forward, "ipv4_forward"},
        {CapabilityCode::IPv6Forward, "ipv6_forward"},
        {CapabilityCode::L4LoadBalance, "l4_load_balance"},
        {CapabilityCode::TlsTerminate, "tls_terminate"},
        {CapabilityCode::VxlanEncap, "vxlan_encap"},
        {CapabilityCode::Srv6Encap, "srv6_encap"},
        {CapabilityCode::StatefulFirewall, "stateful_firewall"},
        {CapabilityCode::RdmaVerbs, "rdma_verbs"},
        {CapabilityCode::DpuProgrammable, "dpu_programmable"},
        {CapabilityCode::RateLimit, "rate_limit"},
        {CapabilityCode::SyntheticDatapath, "synthetic_datapath"},
    }};

constexpr std::array<std::pair<Statefulness, std::string_view>, 2> kStatefulness = {{
    {Statefulness::Stateless, "stateless"},
    {Statefulness::Stateful, "stateful"},
}};

constexpr std::array<std::pair<Precondition, std::string_view>, kPreconditionCount> kPreconditions = {{
    {Precondition::FenceAdvance, "fence_advance"},
    {Precondition::VerifiedEffect, "verified_effect"},
    {Precondition::StateTransfer, "state_transfer"},
    {Precondition::DependencyHealth, "dependency_health"},
    {Precondition::OperatorAuthorization, "operator_authorization"},
    {Precondition::FallbackCapabilitySupport, "fallback_capability_support"},
}};

constexpr std::array<std::pair<DependencyKind, std::string_view>, kDependencyKindCount>
    kDependencyKinds = {{
        {DependencyKind::Hard, "hard"},
        {DependencyKind::Soft, "soft"},
    }};

constexpr std::array<std::pair<DependencyRequirement, std::string_view>,
                     kDependencyRequirementCount>
    kDependencyRequirements = {{
        {DependencyRequirement::AnyTarget, "any_target"},
        {DependencyRequirement::SameScope, "same_scope"},
        {DependencyRequirement::DistinctTarget, "distinct_target"},
    }};

constexpr std::array<std::pair<FailureClass, std::string_view>, kFailureClassCount> kFailureClasses = {{
    {FailureClass::TargetMissing, "target_missing"},
    {FailureClass::TargetUnresponsive, "target_unresponsive"},
    {FailureClass::ServiceCrash, "service_crash"},
    {FailureClass::LinkDown, "link_down"},
    {FailureClass::OffloadEngineFault, "offload_engine_fault"},
    {FailureClass::PlannedMaintenance, "planned_maintenance"},
    {FailureClass::OperatorDeclared, "operator_declared"},
    {FailureClass::SyntheticInjected, "synthetic_injected"},
}};

constexpr std::array<std::pair<AmbiguityState, std::string_view>, kAmbiguityStateCount> kAmbiguity = {{
    {AmbiguityState::Unambiguous, "unambiguous"},
    {AmbiguityState::Ambiguous, "ambiguous"},
    {AmbiguityState::SplitBrainSuspected, "split_brain_suspected"},
}};

constexpr std::array<std::pair<DependencyHealth, std::string_view>, kDependencyHealthCount>
    kDependencyHealth = {{
        {DependencyHealth::Unknown, "unknown"},
        {DependencyHealth::Healthy, "healthy"},
        {DependencyHealth::Degraded, "degraded"},
        {DependencyHealth::Failed, "failed"},
    }};

constexpr std::array<std::pair<LifecyclePhase, std::string_view>, kLifecyclePhaseCount>
    kLifecyclePhases = {{
        {LifecyclePhase::Unregistered, "unregistered"},
        {LifecyclePhase::Registered, "registered"},
        {LifecyclePhase::Active, "active"},
        {LifecyclePhase::RecoveryPending, "recovery_pending"},
        {LifecyclePhase::RecoveryAuthorized, "recovery_authorized"},
        {LifecyclePhase::RecoveryInFlight, "recovery_in_flight"},
        {LifecyclePhase::FallbackActive, "fallback_active"},
        {LifecyclePhase::Failed, "failed"},
        {LifecyclePhase::Withdrawn, "withdrawn"},
    }};

constexpr std::array<std::pair<RecoveryPhase, std::string_view>, kRecoveryPhaseCount> kRecoveryPhases = {{
    {RecoveryPhase::None, "none"},
    {RecoveryPhase::IntentRecorded, "intent_recorded"},
    {RecoveryPhase::Authorized, "authorized"},
    {RecoveryPhase::ActivationRequested, "activation_requested"},
    {RecoveryPhase::EffectReported, "effect_reported"},
    {RecoveryPhase::Verified, "verified"},
    {RecoveryPhase::Refused, "refused"},
    {RecoveryPhase::AmbiguityPending, "ambiguity_pending"},
}};

constexpr std::array<std::pair<ContinuityClass, std::string_view>, kContinuityClassCount>
    kContinuityClasses = {{
        {ContinuityClass::Unknown, "unknown"},
        {ContinuityClass::None, "none"},
        {ContinuityClass::Degraded, "degraded"},
        {ContinuityClass::Full, "full"},
    }};

constexpr std::array<std::pair<AttemptOutcome, std::string_view>, kAttemptOutcomeCount>
    kAttemptOutcomes = {{
        {AttemptOutcome::Pending, "pending"},
        {AttemptOutcome::Committed, "committed"},
        {AttemptOutcome::Acknowledged, "acknowledged"},
        {AttemptOutcome::EffectReported, "effect_reported"},
        {AttemptOutcome::Verified, "verified"},
        {AttemptOutcome::Refused, "refused"},
        {AttemptOutcome::Cancelled, "cancelled"},
        {AttemptOutcome::OutcomeUnknown, "outcome_unknown"},
        {AttemptOutcome::Superseded, "superseded"},
    }};

constexpr std::array<std::pair<EffectKind, std::string_view>, kEffectKindCount> kEffectKinds = {{
    {EffectKind::ActivationAccepted, "activation_accepted"},
    {EffectKind::ServiceHealthy, "service_healthy"},
    {EffectKind::StateTransferComplete, "state_transfer_complete"},
    {EffectKind::DeactivationAcknowledged, "deactivation_acknowledged"},
}};

constexpr std::array<std::pair<EffectResult, std::string_view>, kEffectResultCount> kEffectResults = {{
    {EffectResult::Positive, "positive"},
    {EffectResult::Negative, "negative"},
}};

constexpr std::array<std::pair<RecoveryTrigger, std::string_view>, kRecoveryTriggerCount>
    kRecoveryTriggers = {{
        {RecoveryTrigger::FailureEvidence, "failure_evidence"},
        {RecoveryTrigger::PlannedMaintenance, "planned_maintenance"},
        {RecoveryTrigger::OperatorCommand, "operator_command"},
    }};

// The canonical encoding of an enum is its table index, so the tables are
// required to be in declaration order. These assertions make a reordering a
// compile error rather than a silent format change.
template <class E, std::size_t N>
[[nodiscard]] constexpr bool table_is_ordinal(
    const std::array<std::pair<E, std::string_view>, N>& table) noexcept {
  for (std::size_t index = 0; index < N; ++index) {
    if (static_cast<std::size_t>(table[index].first) != index) {
      return false;
    }
  }
  return true;
}

static_assert(table_is_ordinal(kDeviceKinds), "device kind table must be ordinal");
static_assert(table_is_ordinal(kCapabilityCodes), "capability table must be ordinal");
static_assert(table_is_ordinal(kStatefulness), "statefulness table must be ordinal");
static_assert(table_is_ordinal(kPreconditions), "precondition table must be ordinal");
static_assert(table_is_ordinal(kDependencyKinds), "dependency kind table must be ordinal");
static_assert(table_is_ordinal(kDependencyRequirements), "dependency requirement table must be ordinal");
static_assert(table_is_ordinal(kFailureClasses), "failure class table must be ordinal");
static_assert(table_is_ordinal(kAmbiguity), "ambiguity table must be ordinal");
static_assert(table_is_ordinal(kDependencyHealth), "dependency health table must be ordinal");
static_assert(table_is_ordinal(kLifecyclePhases), "lifecycle table must be ordinal");
static_assert(table_is_ordinal(kRecoveryPhases), "recovery phase table must be ordinal");
static_assert(table_is_ordinal(kContinuityClasses), "continuity table must be ordinal");
static_assert(table_is_ordinal(kAttemptOutcomes), "attempt outcome table must be ordinal");
static_assert(table_is_ordinal(kEffectKinds), "effect kind table must be ordinal");
static_assert(table_is_ordinal(kEffectResults), "effect result table must be ordinal");
static_assert(table_is_ordinal(kRecoveryTriggers), "recovery trigger table must be ordinal");

// ---- canonical codec helpers ---------------------------------------------

template <class Enum, std::size_t N>
void encode_enum(CanonicalWriter& writer,
                 const std::array<std::pair<Enum, std::string_view>, N>& table, Enum value) {
  // Unknown values are encoded as 0xFF so a decoder rejects them explicitly
  // instead of silently reinterpreting them as a known variant.
  for (std::size_t index = 0; index < N; ++index) {
    if (table[index].first == value) {
      writer.put_u8(static_cast<u8>(index));
      return;
    }
  }
  writer.put_u8(0xFFU);
}

template <class Enum, std::size_t N>
bool decode_enum(CanonicalReader& reader,
                 const std::array<std::pair<Enum, std::string_view>, N>& table, Enum& out) {
  u8 index = 0;
  if (!reader.get_u8(index)) {
    return false;
  }
  if (static_cast<std::size_t>(index) >= N) {
    reader.fail(Reason::UnsupportedSemantics);
    return false;
  }
  out = table[index].first;
  return true;
}

using detail::decode_digest;
using detail::decode_gen;
using detail::decode_id;
using detail::decode_name;
using detail::decode_tick;
using detail::encode_digest;
using detail::encode_gen;
using detail::encode_id;
using detail::encode_list;
using detail::encode_name;
using detail::encode_tick;

[[nodiscard]] JsonValue capability_json(const CapabilitySet& value) {
  JsonValue array{JsonValue::Array{}};
  for (const CapabilityCode code : value.codes()) {
    array.push(JsonValue(std::string(capability_code_text(code))));
  }
  return array;
}

[[nodiscard]] JsonValue precondition_json(const PreconditionSet& value) {
  JsonValue array{JsonValue::Array{}};
  for (const Precondition item : value.values()) {
    array.push(JsonValue(std::string(precondition_text(item))));
  }
  return array;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enum surfaces
// ---------------------------------------------------------------------------

std::string_view device_kind_text(DeviceKind kind) noexcept {
  return lookup_text(kDeviceKinds, kind);
}

bool device_kind_parse(std::string_view text, DeviceKind& out) noexcept {
  return lookup_value(kDeviceKinds, text, out);
}

std::string_view device_kind_fidelity(DeviceKind kind) noexcept {
  switch (kind) {
    case DeviceKind::SmartNic:
    case DeviceKind::Dpu:
      return "REAL";
    case DeviceKind::HostCpu:
      return "SYNTHETIC";
    case DeviceKind::Synthetic:
      return "SYNTHETIC";
    default:
      return "UNSUPPORTED";
  }
}

std::string_view capability_code_text(CapabilityCode code) noexcept {
  return lookup_text(kCapabilityCodes, code);
}

bool capability_code_parse(std::string_view text, CapabilityCode& out) noexcept {
  return lookup_value(kCapabilityCodes, text, out);
}

std::string_view statefulness_text(Statefulness value) noexcept {
  return lookup_text(kStatefulness, value);
}

bool statefulness_parse(std::string_view text, Statefulness& out) noexcept {
  return lookup_value(kStatefulness, text, out);
}

std::string_view precondition_text(Precondition value) noexcept {
  return lookup_text(kPreconditions, value);
}

bool precondition_parse(std::string_view text, Precondition& out) noexcept {
  return lookup_value(kPreconditions, text, out);
}

std::string_view dependency_kind_text(DependencyKind kind) noexcept {
  return lookup_text(kDependencyKinds, kind);
}

bool dependency_kind_parse(std::string_view text, DependencyKind& out) noexcept {
  return lookup_value(kDependencyKinds, text, out);
}

std::string_view dependency_requirement_text(DependencyRequirement value) noexcept {
  return lookup_text(kDependencyRequirements, value);
}

bool dependency_requirement_parse(std::string_view text, DependencyRequirement& out) noexcept {
  return lookup_value(kDependencyRequirements, text, out);
}

std::string_view failure_class_text(FailureClass value) noexcept {
  return lookup_text(kFailureClasses, value);
}

bool failure_class_parse(std::string_view text, FailureClass& out) noexcept {
  return lookup_value(kFailureClasses, text, out);
}

bool failure_class_is_synthetic(FailureClass value) noexcept {
  return value == FailureClass::SyntheticInjected;
}

std::string_view ambiguity_state_text(AmbiguityState value) noexcept {
  return lookup_text(kAmbiguity, value);
}

bool ambiguity_state_parse(std::string_view text, AmbiguityState& out) noexcept {
  return lookup_value(kAmbiguity, text, out);
}

std::string_view dependency_health_text(DependencyHealth value) noexcept {
  return lookup_text(kDependencyHealth, value);
}

bool dependency_health_parse(std::string_view text, DependencyHealth& out) noexcept {
  return lookup_value(kDependencyHealth, text, out);
}

std::string_view lifecycle_phase_text(LifecyclePhase value) noexcept {
  return lookup_text(kLifecyclePhases, value);
}

bool lifecycle_phase_parse(std::string_view text, LifecyclePhase& out) noexcept {
  return lookup_value(kLifecyclePhases, text, out);
}

std::string_view recovery_phase_text(RecoveryPhase value) noexcept {
  return lookup_text(kRecoveryPhases, value);
}

bool recovery_phase_parse(std::string_view text, RecoveryPhase& out) noexcept {
  return lookup_value(kRecoveryPhases, text, out);
}

std::string_view continuity_class_text(ContinuityClass value) noexcept {
  return lookup_text(kContinuityClasses, value);
}

bool continuity_class_parse(std::string_view text, ContinuityClass& out) noexcept {
  return lookup_value(kContinuityClasses, text, out);
}

std::string_view attempt_outcome_text(AttemptOutcome value) noexcept {
  return lookup_text(kAttemptOutcomes, value);
}

bool attempt_outcome_parse(std::string_view text, AttemptOutcome& out) noexcept {
  return lookup_value(kAttemptOutcomes, text, out);
}

std::string_view effect_kind_text(EffectKind value) noexcept {
  return lookup_text(kEffectKinds, value);
}

bool effect_kind_parse(std::string_view text, EffectKind& out) noexcept {
  return lookup_value(kEffectKinds, text, out);
}

std::string_view effect_result_text(EffectResult value) noexcept {
  return lookup_text(kEffectResults, value);
}

bool effect_result_parse(std::string_view text, EffectResult& out) noexcept {
  return lookup_value(kEffectResults, text, out);
}

std::string_view recovery_trigger_text(RecoveryTrigger value) noexcept {
  return lookup_text(kRecoveryTriggers, value);
}

bool recovery_trigger_parse(std::string_view text, RecoveryTrigger& out) noexcept {
  return lookup_value(kRecoveryTriggers, text, out);
}

// ---------------------------------------------------------------------------
// Sets
// ---------------------------------------------------------------------------

void CapabilitySet::insert(CapabilityCode code) noexcept {
  mask_ |= (1ULL << static_cast<u64>(code));
}

void CapabilitySet::erase(CapabilityCode code) noexcept {
  mask_ &= ~(1ULL << static_cast<u64>(code));
}

bool CapabilitySet::contains(CapabilityCode code) const noexcept {
  return (mask_ & (1ULL << static_cast<u64>(code))) != 0ULL;
}

bool CapabilitySet::contains_all(const CapabilitySet& other) const noexcept {
  return (mask_ & other.mask_) == other.mask_;
}

CapabilitySet CapabilitySet::from_mask(u64 mask) noexcept {
  CapabilitySet out;
  out.mask_ = mask;
  return out;
}

std::vector<CapabilityCode> CapabilitySet::codes() const {
  std::vector<CapabilityCode> out;
  for (std::size_t index = 0; index < kCapabilityCodeCount; ++index) {
    const auto code = static_cast<CapabilityCode>(index);
    if (contains(code)) {
      out.push_back(code);
    }
  }
  return out;
}

std::string CapabilitySet::render() const {
  std::string out;
  const std::vector<CapabilityCode> all = codes();
  for (std::size_t index = 0; index < all.size(); ++index) {
    if (index > 0) {
      out.push_back(',');
    }
    out.append(capability_code_text(all[index]));
  }
  return out;
}

void PreconditionSet::insert(Precondition value) noexcept {
  mask_ |= (1U << static_cast<u32>(value));
}

bool PreconditionSet::contains(Precondition value) const noexcept {
  return (mask_ & (1U << static_cast<u32>(value))) != 0U;
}

PreconditionSet PreconditionSet::from_mask(u32 mask) noexcept {
  PreconditionSet out;
  out.mask_ = mask;
  return out;
}

std::vector<Precondition> PreconditionSet::values() const {
  std::vector<Precondition> out;
  for (std::size_t index = 0; index < kPreconditionCount; ++index) {
    const auto value = static_cast<Precondition>(index);
    if (contains(value)) {
      out.push_back(value);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Canonical codecs
// ---------------------------------------------------------------------------

void encode(CanonicalWriter& writer, const TargetIncarnation& value) {
  encode_gen(writer, value.host);
  encode_gen(writer, value.device);
}

bool decode(CanonicalReader& reader, TargetIncarnation& value) {
  return decode_gen(reader, value.host) && decode_gen(reader, value.device);
}

void encode(CanonicalWriter& writer, const TargetRef& value) {
  encode_name(writer, value.target);
  encode(writer, value.incarnation);
}

bool decode(CanonicalReader& reader, TargetRef& value) {
  return decode_name(reader, value.target) && decode(reader, value.incarnation);
}

void encode(CanonicalWriter& writer, const TargetRecord& value) {
  encode_name(writer, value.name);
  encode_name(writer, value.host);
  encode_name(writer, value.device);
  encode_enum(writer, kDeviceKinds, value.kind);
  encode_name(writer, value.scope);
  encode(writer, value.incarnation);
  encode_gen(writer, value.topology_generation);
}

bool decode(CanonicalReader& reader, TargetRecord& value) {
  return decode_name(reader, value.name) && decode_name(reader, value.host) &&
         decode_name(reader, value.device) && decode_enum(reader, kDeviceKinds, value.kind) &&
         decode_name(reader, value.scope) && decode(reader, value.incarnation) &&
         decode_gen(reader, value.topology_generation);
}

void encode(CanonicalWriter& writer, const CapabilitySet& value) { writer.put_u64(value.mask()); }

bool decode(CanonicalReader& reader, CapabilitySet& value) {
  u64 mask = 0;
  if (!reader.get_u64(mask)) {
    return false;
  }
  const u64 known = (1ULL << static_cast<u64>(kCapabilityCodeCount)) - 1ULL;
  if ((mask & ~known) != 0ULL) {
    // A capability bit this build does not understand must never be silently
    // dropped: refusing keeps unknown capability from becoming "not required".
    reader.fail(Reason::UnsupportedSemantics);
    return false;
  }
  value = CapabilitySet::from_mask(mask);
  return true;
}

void encode(CanonicalWriter& writer, const CapabilityEvidence& value) {
  encode_name(writer, value.target);
  encode_gen(writer, value.generation);
  encode(writer, value.capabilities);
  encode_gen(writer, value.topology_generation);
  encode(writer, value.incarnation);
  encode_name(writer, value.source);
  encode_tick(writer, value.tick);
  encode_digest(writer, value.content_digest);
}

bool decode(CanonicalReader& reader, CapabilityEvidence& value) {
  return decode_name(reader, value.target) && decode_gen(reader, value.generation) &&
         decode(reader, value.capabilities) && decode_gen(reader, value.topology_generation) &&
         decode(reader, value.incarnation) && decode_name(reader, value.source) &&
         decode_tick(reader, value.tick) && decode_digest(reader, value.content_digest);
}

void encode(CanonicalWriter& writer, const PreconditionSet& value) { writer.put_u32(value.mask()); }

bool decode(CanonicalReader& reader, PreconditionSet& value) {
  u32 mask = 0;
  if (!reader.get_u32(mask)) {
    return false;
  }
  const u32 known = (1U << static_cast<u32>(kPreconditionCount)) - 1U;
  if ((mask & ~known) != 0U) {
    reader.fail(Reason::UnsupportedSemantics);
    return false;
  }
  value = PreconditionSet::from_mask(mask);
  return true;
}

void encode(CanonicalWriter& writer, const DependencySpec& value) {
  encode_name(writer, value.service);
  encode_enum(writer, kDependencyKinds, value.kind);
  encode_enum(writer, kDependencyRequirements, value.requirement);
}

bool decode(CanonicalReader& reader, DependencySpec& value) {
  return decode_name(reader, value.service) && decode_enum(reader, kDependencyKinds, value.kind) &&
         decode_enum(reader, kDependencyRequirements, value.requirement);
}

void encode(CanonicalWriter& writer, const ServiceDescriptor& value) {
  encode_name(writer, value.name);
  encode_name(writer, value.scope);
  encode_enum(writer, kStatefulness, value.statefulness);
  encode(writer, value.required_capabilities);
  encode(writer, value.preconditions);
  encode_list(writer, value.dependencies,
              [](CanonicalWriter& target, const DependencySpec& item) { encode(target, item); });
  encode_gen(writer, value.last_known_state_generation);
}

bool decode(CanonicalReader& reader, ServiceDescriptor& value) {
  if (!decode_name(reader, value.name) || !decode_name(reader, value.scope) ||
      !decode_enum(reader, kStatefulness, value.statefulness) ||
      !decode(reader, value.required_capabilities) || !decode(reader, value.preconditions)) {
    return false;
  }
  std::size_t count = 0;
  if (!reader.get_count(3, count)) {
    return false;
  }
  if (!reader.push_depth()) {
    return false;
  }
  value.dependencies.clear();
  value.dependencies.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    DependencySpec item;
    if (!decode(reader, item)) {
      reader.pop_depth();
      return false;
    }
    value.dependencies.push_back(item);
  }
  reader.pop_depth();
  return decode_gen(reader, value.last_known_state_generation);
}

void encode(CanonicalWriter& writer, const Provenance& value) {
  encode_name(writer, value.source);
  encode_gen(writer, value.sequence);
  encode_tick(writer, value.tick);
  encode_gen(writer, value.epoch);
  encode_gen(writer, value.boot);
}

bool decode(CanonicalReader& reader, Provenance& value) {
  return decode_name(reader, value.source) && decode_gen(reader, value.sequence) &&
         decode_tick(reader, value.tick) && decode_gen(reader, value.epoch) &&
         decode_gen(reader, value.boot);
}

void encode(CanonicalWriter& writer, const FailureEvidence& value) {
  encode_id(writer, value.id);
  encode_name(writer, value.service);
  encode(writer, value.target);
  encode_enum(writer, kFailureClasses, value.failure_class);
  encode_enum(writer, kAmbiguity, value.ambiguity);
  encode(writer, value.provenance);
  encode_gen(writer, value.topology_generation);
  encode_gen(writer, value.capability_generation);
  encode_gen(writer, value.policy_generation);
  encode_digest(writer, value.content_digest);
}

bool decode(CanonicalReader& reader, FailureEvidence& value) {
  return decode_id(reader, value.id) && decode_name(reader, value.service) &&
         decode(reader, value.target) && decode_enum(reader, kFailureClasses, value.failure_class) &&
         decode_enum(reader, kAmbiguity, value.ambiguity) && decode(reader, value.provenance) &&
         decode_gen(reader, value.topology_generation) &&
         decode_gen(reader, value.capability_generation) &&
         decode_gen(reader, value.policy_generation) && decode_digest(reader, value.content_digest);
}

void encode(CanonicalWriter& writer, const DependencyObservation& value) {
  encode_name(writer, value.service);
  encode_name(writer, value.dependency);
  encode_enum(writer, kDependencyHealth, value.health);
  encode(writer, value.observed_target);
  encode(writer, value.provenance);
  encode_gen(writer, value.topology_generation);
  encode_digest(writer, value.content_digest);
}

bool decode(CanonicalReader& reader, DependencyObservation& value) {
  return decode_name(reader, value.service) && decode_name(reader, value.dependency) &&
         decode_enum(reader, kDependencyHealth, value.health) &&
         decode(reader, value.observed_target) && decode(reader, value.provenance) &&
         decode_gen(reader, value.topology_generation) && decode_digest(reader, value.content_digest);
}

void encode(CanonicalWriter& writer, const PolicyDescriptor& value) {
  encode_gen(writer, value.generation);
  encode_name(writer, value.scope);
  writer.put_u32(value.max_attempts_per_generation);
  writer.put_u32(value.max_plan_steps);
  writer.put_u32(value.max_fallback_candidates);
  writer.put_u32(value.evidence_max_age_ticks);
  writer.put_u32(value.dependency_max_depth);
  writer.put_bool(value.require_operator_for_ambiguous);
  writer.put_bool(value.allow_degraded_continuity);
  writer.put_bool(value.allow_failback);
}

bool decode(CanonicalReader& reader, PolicyDescriptor& value) {
  return decode_gen(reader, value.generation) && decode_name(reader, value.scope) &&
         reader.get_u32(value.max_attempts_per_generation) &&
         reader.get_u32(value.max_plan_steps) && reader.get_u32(value.max_fallback_candidates) &&
         reader.get_u32(value.evidence_max_age_ticks) && reader.get_u32(value.dependency_max_depth) &&
         reader.get_bool(value.require_operator_for_ambiguous) &&
         reader.get_bool(value.allow_degraded_continuity) && reader.get_bool(value.allow_failback);
}

void encode(CanonicalWriter& writer, const FenceRecord& value) {
  encode_name(writer, value.service);
  encode_gen(writer, value.token);
  encode(writer, value.holder);
  encode_id(writer, value.attempt);
  encode_gen(writer, value.epoch);
  encode_tick(writer, value.issued);
}

bool decode(CanonicalReader& reader, FenceRecord& value) {
  return decode_name(reader, value.service) && decode_gen(reader, value.token) &&
         decode(reader, value.holder) && decode_id(reader, value.attempt) &&
         decode_gen(reader, value.epoch) && decode_tick(reader, value.issued);
}

void encode(CanonicalWriter& writer, const LeaseRecord& value) {
  encode_name(writer, value.service);
  encode_id(writer, value.id);
  encode_gen(writer, value.term);
  encode_name(writer, value.scope);
  encode(writer, value.holder);
  encode_gen(writer, value.epoch);
  encode_tick(writer, value.issued);
}

bool decode(CanonicalReader& reader, LeaseRecord& value) {
  return decode_name(reader, value.service) && decode_id(reader, value.id) &&
         decode_gen(reader, value.term) && decode_name(reader, value.scope) &&
         decode(reader, value.holder) && decode_gen(reader, value.epoch) &&
         decode_tick(reader, value.issued);
}

void encode(CanonicalWriter& writer, const AttemptRecord& value) {
  encode_id(writer, value.id);
  encode_name(writer, value.service);
  encode_gen(writer, value.generation);
  encode_gen(writer, value.fence);
  encode(writer, value.from);
  encode(writer, value.to);
  encode_gen(writer, value.lease_term);
  encode_gen(writer, value.epoch);
  encode_tick(writer, value.opened);
  encode_enum(writer, kRecoveryPhases, value.phase);
  encode_enum(writer, kAttemptOutcomes, value.outcome);
  writer.put_bool(value.durable_committed);
  writer.put_bool(value.acknowledged);
  writer.put_bool(value.restart_downgraded);
  writer.put_u16(static_cast<u16>(value.last_reason));
}

bool decode(CanonicalReader& reader, AttemptRecord& value) {
  if (!decode_id(reader, value.id) || !decode_name(reader, value.service) ||
      !decode_gen(reader, value.generation) || !decode_gen(reader, value.fence) ||
      !decode(reader, value.from) || !decode(reader, value.to) ||
      !decode_gen(reader, value.lease_term) || !decode_gen(reader, value.epoch) ||
      !decode_tick(reader, value.opened) || !decode_enum(reader, kRecoveryPhases, value.phase) ||
      !decode_enum(reader, kAttemptOutcomes, value.outcome) ||
      !reader.get_bool(value.durable_committed) || !reader.get_bool(value.acknowledged) ||
      !reader.get_bool(value.restart_downgraded)) {
    return false;
  }
  u16 raw_reason = 0;
  if (!reader.get_u16(raw_reason)) {
    return false;
  }
  const auto parsed = static_cast<Reason>(raw_reason);
  if (reason_code_text(parsed) == "UNRECOGNIZED_REASON") {
    reader.fail(Reason::UnsupportedSemantics);
    return false;
  }
  value.last_reason = parsed;
  return true;
}

void encode(CanonicalWriter& writer, const FailoverIntent& value) {
  encode_id(writer, value.attempt);
  encode_name(writer, value.service);
  encode_gen(writer, value.generation);
  encode_gen(writer, value.fence);
  encode_gen(writer, value.lease_term);
  encode(writer, value.from);
  encode(writer, value.to);
  encode_gen(writer, value.epoch);
  encode_gen(writer, value.policy_generation);
  encode_gen(writer, value.topology_generation);
  encode_tick(writer, value.issued);
  writer.put_bool(value.reissue_after_restart);
}

bool decode(CanonicalReader& reader, FailoverIntent& value) {
  return decode_id(reader, value.attempt) && decode_name(reader, value.service) &&
         decode_gen(reader, value.generation) && decode_gen(reader, value.fence) &&
         decode_gen(reader, value.lease_term) && decode(reader, value.from) &&
         decode(reader, value.to) && decode_gen(reader, value.epoch) &&
         decode_gen(reader, value.policy_generation) &&
         decode_gen(reader, value.topology_generation) && decode_tick(reader, value.issued) &&
         reader.get_bool(value.reissue_after_restart);
}

void encode(CanonicalWriter& writer, const EffectReport& value) {
  encode_id(writer, value.id);
  encode_name(writer, value.service);
  encode_id(writer, value.attempt);
  encode_gen(writer, value.generation);
  encode_gen(writer, value.fence);
  encode(writer, value.target);
  encode_enum(writer, kEffectKinds, value.kind);
  encode_enum(writer, kEffectResults, value.result);
  encode_gen(writer, value.state_generation);
  encode(writer, value.provenance);
}

bool decode(CanonicalReader& reader, EffectReport& value) {
  return decode_id(reader, value.id) && decode_name(reader, value.service) &&
         decode_id(reader, value.attempt) && decode_gen(reader, value.generation) &&
         decode_gen(reader, value.fence) && decode(reader, value.target) &&
         decode_enum(reader, kEffectKinds, value.kind) &&
         decode_enum(reader, kEffectResults, value.result) &&
         decode_gen(reader, value.state_generation) && decode(reader, value.provenance);
}

// ---------------------------------------------------------------------------
// JSON renderers
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] JsonValue incarnation_json(const TargetIncarnation& value) {
  JsonValue object;
  object.set("device", JsonValue(value.device.value()));
  object.set("host", JsonValue(value.host.value()));
  return object;
}

[[nodiscard]] JsonValue target_ref_json(const TargetRef& value) {
  JsonValue object;
  object.set("incarnation", incarnation_json(value.incarnation));
  object.set("target", JsonValue(value.target.str()));
  return object;
}

[[nodiscard]] JsonValue provenance_json(const Provenance& value) {
  JsonValue object;
  object.set("boot", JsonValue(value.boot.value()));
  object.set("epoch", JsonValue(value.epoch.value()));
  object.set("sequence", JsonValue(value.sequence.value()));
  object.set("source", JsonValue(value.source.str()));
  object.set("tick", JsonValue(value.tick.value()));
  return object;
}

}  // namespace

JsonValue to_json(const TargetIncarnation& value) { return incarnation_json(value); }

JsonValue to_json(const TargetRef& value) { return target_ref_json(value); }

JsonValue to_json(const TargetRecord& value) {
  JsonValue object;
  object.set("device", JsonValue(value.device.str()));
  object.set("fidelity", JsonValue(std::string(device_kind_fidelity(value.kind))));
  object.set("host", JsonValue(value.host.str()));
  object.set("incarnation", incarnation_json(value.incarnation));
  object.set("kind", JsonValue(std::string(device_kind_text(value.kind))));
  object.set("name", JsonValue(value.name.str()));
  object.set("scope", JsonValue(value.scope.str()));
  object.set("topology_generation", JsonValue(value.topology_generation.value()));
  return object;
}

JsonValue to_json(const CapabilitySet& value) { return capability_json(value); }

JsonValue to_json(const CapabilityEvidence& value) {
  JsonValue object;
  object.set("capabilities", capability_json(value.capabilities));
  object.set("content_digest", JsonValue(value.content_digest.to_hex()));
  object.set("generation", JsonValue(value.generation.value()));
  object.set("incarnation", incarnation_json(value.incarnation));
  object.set("source", JsonValue(value.source.str()));
  object.set("target", JsonValue(value.target.str()));
  object.set("tick", JsonValue(value.tick.value()));
  object.set("topology_generation", JsonValue(value.topology_generation.value()));
  return object;
}

JsonValue to_json(const ServiceDescriptor& value) {
  JsonValue dependencies{JsonValue::Array{}};
  for (const DependencySpec& spec : value.dependencies) {
    JsonValue item;
    item.set("kind", JsonValue(std::string(dependency_kind_text(spec.kind))));
    item.set("requirement", JsonValue(std::string(dependency_requirement_text(spec.requirement))));
    item.set("service", JsonValue(spec.service.str()));
    dependencies.push(std::move(item));
  }
  JsonValue object;
  object.set("dependencies", std::move(dependencies));
  object.set("last_known_state_generation", JsonValue(value.last_known_state_generation.value()));
  object.set("name", JsonValue(value.name.str()));
  object.set("preconditions", precondition_json(value.preconditions));
  object.set("required_capabilities", capability_json(value.required_capabilities));
  object.set("scope", JsonValue(value.scope.str()));
  object.set("statefulness", JsonValue(std::string(statefulness_text(value.statefulness))));
  return object;
}

JsonValue to_json(const Provenance& value) { return provenance_json(value); }

JsonValue to_json(const FailureEvidence& value) {
  JsonValue object;
  object.set("ambiguity", JsonValue(std::string(ambiguity_state_text(value.ambiguity))));
  object.set("capability_generation", JsonValue(value.capability_generation.value()));
  object.set("content_digest", JsonValue(value.content_digest.to_hex()));
  object.set("failure_class", JsonValue(std::string(failure_class_text(value.failure_class))));
  object.set("id", JsonValue(value.id.value()));
  object.set("policy_generation", JsonValue(value.policy_generation.value()));
  object.set("provenance", provenance_json(value.provenance));
  object.set("service", JsonValue(value.service.str()));
  object.set("target", target_ref_json(value.target));
  object.set("topology_generation", JsonValue(value.topology_generation.value()));
  return object;
}

JsonValue to_json(const DependencyObservation& value) {
  JsonValue object;
  object.set("content_digest", JsonValue(value.content_digest.to_hex()));
  object.set("dependency", JsonValue(value.dependency.str()));
  object.set("health", JsonValue(std::string(dependency_health_text(value.health))));
  object.set("observed_target", target_ref_json(value.observed_target));
  object.set("provenance", provenance_json(value.provenance));
  object.set("service", JsonValue(value.service.str()));
  object.set("topology_generation", JsonValue(value.topology_generation.value()));
  return object;
}

JsonValue to_json(const PolicyDescriptor& value) {
  JsonValue object;
  object.set("allow_degraded_continuity", JsonValue(value.allow_degraded_continuity));
  object.set("allow_failback", JsonValue(value.allow_failback));
  object.set("dependency_max_depth", JsonValue(value.dependency_max_depth));
  object.set("evidence_max_age_ticks", JsonValue(value.evidence_max_age_ticks));
  object.set("generation", JsonValue(value.generation.value()));
  object.set("max_attempts_per_generation", JsonValue(value.max_attempts_per_generation));
  object.set("max_fallback_candidates", JsonValue(value.max_fallback_candidates));
  object.set("max_plan_steps", JsonValue(value.max_plan_steps));
  object.set("require_operator_for_ambiguous", JsonValue(value.require_operator_for_ambiguous));
  object.set("scope", JsonValue(value.scope.str()));
  return object;
}

JsonValue to_json(const FenceRecord& value) {
  JsonValue object;
  object.set("attempt", JsonValue(value.attempt.value()));
  object.set("epoch", JsonValue(value.epoch.value()));
  object.set("holder", target_ref_json(value.holder));
  object.set("issued", JsonValue(value.issued.value()));
  object.set("service", JsonValue(value.service.str()));
  object.set("token", JsonValue(value.token.value()));
  return object;
}

JsonValue to_json(const LeaseRecord& value) {
  JsonValue object;
  object.set("epoch", JsonValue(value.epoch.value()));
  object.set("holder", target_ref_json(value.holder));
  object.set("id", JsonValue(value.id.value()));
  object.set("issued", JsonValue(value.issued.value()));
  object.set("scope", JsonValue(value.scope.str()));
  object.set("service", JsonValue(value.service.str()));
  object.set("term", JsonValue(value.term.value()));
  return object;
}

JsonValue to_json(const AttemptRecord& value) {
  JsonValue object;
  object.set("acknowledged", JsonValue(value.acknowledged));
  object.set("durable_committed", JsonValue(value.durable_committed));
  object.set("epoch", JsonValue(value.epoch.value()));
  object.set("fence", JsonValue(value.fence.value()));
  object.set("from", target_ref_json(value.from));
  object.set("generation", JsonValue(value.generation.value()));
  object.set("id", JsonValue(value.id.value()));
  object.set("last_reason", JsonValue(std::string(reason_code_text(value.last_reason))));
  object.set("lease_term", JsonValue(value.lease_term.value()));
  object.set("opened", JsonValue(value.opened.value()));
  object.set("outcome", JsonValue(std::string(attempt_outcome_text(value.outcome))));
  object.set("phase", JsonValue(std::string(recovery_phase_text(value.phase))));
  object.set("restart_downgraded", JsonValue(value.restart_downgraded));
  object.set("service", JsonValue(value.service.str()));
  object.set("to", target_ref_json(value.to));
  return object;
}

JsonValue to_json(const FailoverIntent& value) {
  JsonValue object;
  object.set("attempt", JsonValue(value.attempt.value()));
  object.set("epoch", JsonValue(value.epoch.value()));
  object.set("fence", JsonValue(value.fence.value()));
  object.set("from", target_ref_json(value.from));
  object.set("generation", JsonValue(value.generation.value()));
  object.set("issued", JsonValue(value.issued.value()));
  object.set("lease_term", JsonValue(value.lease_term.value()));
  object.set("policy_generation", JsonValue(value.policy_generation.value()));
  object.set("reissue_after_restart", JsonValue(value.reissue_after_restart));
  object.set("service", JsonValue(value.service.str()));
  object.set("to", target_ref_json(value.to));
  object.set("topology_generation", JsonValue(value.topology_generation.value()));
  return object;
}

JsonValue to_json(const EffectReport& value) {
  JsonValue object;
  object.set("attempt", JsonValue(value.attempt.value()));
  object.set("fence", JsonValue(value.fence.value()));
  object.set("generation", JsonValue(value.generation.value()));
  object.set("id", JsonValue(value.id.value()));
  object.set("kind", JsonValue(std::string(effect_kind_text(value.kind))));
  object.set("provenance", provenance_json(value.provenance));
  object.set("result", JsonValue(std::string(effect_result_text(value.result))));
  object.set("service", JsonValue(value.service.str()));
  object.set("state_generation", JsonValue(value.state_generation.value()));
  object.set("target", target_ref_json(value.target));
  return object;
}

}  // namespace off
