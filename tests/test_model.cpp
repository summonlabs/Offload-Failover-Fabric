// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <limits>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using namespace offtest;

template <class T>
void round_trip(const T& value) {
  off::CanonicalWriter writer;
  encode(writer, value);
  OFF_CHECK(writer.ok());
  T decoded{};
  off::CanonicalReader reader(writer.buffer().data(), writer.buffer().size());
  OFF_CHECK(decode(reader, decoded));
  OFF_CHECK(reader.at_end());
  OFF_CHECK(decoded == value);
}

off::TargetRecord sample_target() {
  off::TargetRecord record;
  record.name = target_name("t1");
  record.host = host_name("h1");
  record.device = device_name("nic1");
  record.kind = off::DeviceKind::SmartNic;
  record.scope = scope_name("edge");
  record.incarnation.host = off::Incarnation::from_value(3);
  record.incarnation.device = off::Incarnation::from_value(4);
  record.topology_generation = off::TopologyGeneration::from_value(2);
  return record;
}

off::FailureEvidence sample_failure() {
  off::FailureEvidence evidence;
  evidence.id = off::EvidenceId::from_value(11);
  evidence.service = service_name("svc1");
  evidence.target.target = target_name("t1");
  evidence.target.incarnation.host = off::Incarnation::from_value(1);
  evidence.target.incarnation.device = off::Incarnation::from_value(1);
  evidence.failure_class = off::FailureClass::TargetUnresponsive;
  evidence.ambiguity = off::AmbiguityState::SplitBrainSuspected;
  evidence.provenance.source = source_name("failrep");
  evidence.provenance.sequence = off::EvidenceSeq::from_value(9);
  evidence.provenance.tick = off::LogicalTick::from_value(5);
  evidence.provenance.epoch = off::CoordinatorEpoch::from_value(2);
  evidence.provenance.boot = off::BootId::from_value(2);
  evidence.topology_generation = off::TopologyGeneration::from_value(1);
  evidence.capability_generation = off::CapabilityGeneration::from_value(1);
  evidence.policy_generation = off::PolicyGeneration::from_value(0);
  evidence.content_digest.hi = 0xAAAAULL;
  evidence.content_digest.lo = 0xBBBBULL;
  return evidence;
}

OFF_TEST(model, enum_text_round_trips) {
  for (std::size_t index = 0; index < off::kDeviceKindCount; ++index) {
    const auto value = static_cast<off::DeviceKind>(index);
    off::DeviceKind parsed{};
    OFF_CHECK(off::device_kind_parse(off::device_kind_text(value), parsed));
    OFF_CHECK(parsed == value);
  }
  for (std::size_t index = 0; index < off::kLifecyclePhaseCount; ++index) {
    const auto value = static_cast<off::LifecyclePhase>(index);
    off::LifecyclePhase parsed{};
    OFF_CHECK(off::lifecycle_phase_parse(off::lifecycle_phase_text(value), parsed));
    OFF_CHECK(parsed == value);
  }
  for (std::size_t index = 0; index < off::kEffectKindCount; ++index) {
    const auto value = static_cast<off::EffectKind>(index);
    off::EffectKind parsed{};
    OFF_CHECK(off::effect_kind_parse(off::effect_kind_text(value), parsed));
    OFF_CHECK(parsed == value);
  }
  off::DeviceKind unknown{};
  OFF_CHECK(!off::device_kind_parse("not_a_kind", unknown));
}

OFF_TEST(model, capability_sets_are_order_independent) {
  off::CapabilitySet first = capabilities({off::CapabilityCode::IPv4Forward,
                                           off::CapabilityCode::DpuProgrammable});
  off::CapabilitySet second;
  second.insert(off::CapabilityCode::DpuProgrammable);
  second.insert(off::CapabilityCode::IPv4Forward);
  OFF_CHECK(first == second);
  OFF_CHECK_EQ(first.render(), std::string("ipv4_forward,dpu_programmable"));
  OFF_CHECK(first.contains_all(second));
  off::CapabilitySet extra = second;
  extra.insert(off::CapabilityCode::Srv6Encap);
  OFF_CHECK(!second.contains_all(extra));
  OFF_CHECK(extra.contains_all(second));
  OFF_CHECK_EQ(off::CapabilitySet::from_mask(first.mask()), first);
}

OFF_TEST(model, preconditions_and_dependency_specs_round_trip) {
  off::ServiceDescriptor descriptor;
  descriptor.name = service_name("svc1");
  descriptor.scope = scope_name("edge");
  descriptor.statefulness = off::Statefulness::Stateful;
  descriptor.required_capabilities = capabilities({off::CapabilityCode::IPv4Forward});
  descriptor.preconditions.insert(off::Precondition::VerifiedEffect);
  descriptor.preconditions.insert(off::Precondition::StateTransfer);
  descriptor.dependencies.push_back(
      off::DependencySpec{service_name("dns"), off::DependencyKind::Hard,
                          off::DependencyRequirement::SameScope});
  descriptor.dependencies.push_back(
      off::DependencySpec{service_name("audit"), off::DependencyKind::Soft,
                          off::DependencyRequirement::AnyTarget});
  descriptor.last_known_state_generation = off::StateGeneration::from_value(7);
  round_trip(descriptor);
}

OFF_TEST(model, every_persisted_entity_round_trips) {
  round_trip(sample_target());
  round_trip(sample_failure());
  round_trip(off::TargetIncarnation{off::Incarnation::from_value(1), off::Incarnation::from_value(2)});
  round_trip(off::TargetRef{target_name("t1"),
                            off::TargetIncarnation{off::Incarnation::from_value(1),
                                                   off::Incarnation::from_value(1)}});
  round_trip(off::Provenance{source_name("s"), off::EvidenceSeq::from_value(3),
                             off::LogicalTick::from_value(4), off::CoordinatorEpoch::from_value(5),
                             off::BootId::from_value(6)});
  off::PolicyDescriptor policy;
  policy.generation = off::PolicyGeneration::from_value(3);
  policy.scope = scope_name("edge");
  policy.max_attempts_per_generation = 5;
  policy.allow_failback = false;
  round_trip(policy);
  off::FenceRecord fence;
  fence.service = service_name("svc1");
  fence.token = off::FenceToken::from_value(9);
  fence.holder = off::TargetRef{target_name("t2"),
                                off::TargetIncarnation{off::Incarnation::from_value(1),
                                                       off::Incarnation::from_value(1)}};
  fence.attempt = off::AttemptId::from_value(4);
  fence.epoch = off::CoordinatorEpoch::from_value(1);
  fence.issued = off::LogicalTick::from_value(8);
  round_trip(fence);
  off::LeaseRecord lease;
  lease.service = service_name("svc1");
  lease.id = off::LeaseId::from_value(2);
  lease.term = off::LeaseTerm::from_value(2);
  lease.scope = scope_name("edge");
  lease.holder = fence.holder;
  lease.epoch = off::CoordinatorEpoch::from_value(1);
  lease.issued = off::LogicalTick::from_value(8);
  round_trip(lease);
  off::AttemptRecord attempt;
  attempt.id = off::AttemptId::from_value(3);
  attempt.service = service_name("svc1");
  attempt.generation = off::FailoverGeneration::from_value(2);
  attempt.fence = off::FenceToken::from_value(2);
  attempt.from = fence.holder;
  attempt.to = fence.holder;
  attempt.lease_term = off::LeaseTerm::from_value(2);
  attempt.epoch = off::CoordinatorEpoch::from_value(1);
  attempt.opened = off::LogicalTick::from_value(9);
  attempt.phase = off::RecoveryPhase::Authorized;
  attempt.outcome = off::AttemptOutcome::Committed;
  attempt.durable_committed = true;
  attempt.last_reason = off::Reason::AcceptIntentEmitted;
  round_trip(attempt);
  off::FailoverIntent intent;
  intent.attempt = attempt.id;
  intent.service = attempt.service;
  intent.generation = attempt.generation;
  intent.fence = attempt.fence;
  intent.lease_term = attempt.lease_term;
  intent.from = attempt.from;
  intent.to = attempt.to;
  intent.epoch = attempt.epoch;
  intent.policy_generation = off::PolicyGeneration::from_value(1);
  intent.topology_generation = off::TopologyGeneration::from_value(1);
  intent.issued = attempt.opened;
  intent.reissue_after_restart = true;
  round_trip(intent);
  off::EffectReport effect;
  effect.id = off::EffectId::from_value(5);
  effect.service = service_name("svc1");
  effect.attempt = attempt.id;
  effect.generation = attempt.generation;
  effect.fence = attempt.fence;
  effect.target = attempt.to;
  effect.kind = off::EffectKind::ServiceHealthy;
  effect.result = off::EffectResult::Positive;
  effect.state_generation = off::StateGeneration::from_value(4);
  effect.provenance.source = source_name("exec");
  round_trip(effect);
  off::DependencyObservation observation;
  observation.service = service_name("svc1");
  observation.dependency = service_name("dns");
  observation.health = off::DependencyHealth::Healthy;
  observation.observed_target = attempt.to;
  observation.provenance.source = source_name("deprep");
  observation.provenance.sequence = off::EvidenceSeq::from_value(2);
  observation.topology_generation = off::TopologyGeneration::from_value(1);
  round_trip(observation);
  off::CapabilityEvidence capability;
  capability.target = target_name("t2");
  capability.generation = off::CapabilityGeneration::from_value(2);
  capability.capabilities = capabilities({off::CapabilityCode::IPv4Forward});
  capability.topology_generation = off::TopologyGeneration::from_value(1);
  capability.incarnation.host = off::Incarnation::from_value(1);
  capability.incarnation.device = off::Incarnation::from_value(1);
  capability.source = source_name("caprep");
  capability.tick = off::LogicalTick::from_value(3);
  round_trip(capability);
}

OFF_TEST(model, empty_optional_names_round_trip_as_absence) {
  const off::TargetRef absent;
  round_trip(absent);
  const off::PolicyDescriptor policy;
  round_trip(policy);
}

OFF_TEST(model, unknown_capability_bits_are_refused) {
  off::CanonicalWriter writer;
  writer.put_u64(1ULL << 40U);
  const std::vector<off::u8> payload = writer.buffer();
  off::CanonicalReader reader(payload.data(), payload.size());
  off::CapabilitySet set;
  OFF_CHECK(!decode(reader, set));
  OFF_CHECK(reader.error() == off::Reason::UnsupportedSemantics);
}

OFF_TEST(model, unknown_enum_ordinals_and_reason_codes_are_refused) {
  // The device kind ordinal of the sample record sits after the three
  // length-prefixed names: (4+2) + (4+2) + (4+4) = 20.
  const off::TargetRecord sample = sample_target();
  off::CanonicalWriter target_writer;
  encode(target_writer, sample);
  std::vector<off::u8> target_bytes = target_writer.buffer();
  OFF_CHECK_EQ(target_bytes[20], static_cast<off::u8>(off::DeviceKind::SmartNic));
  target_bytes[20] = 0xFF;
  off::TargetRecord record;
  off::CanonicalReader reader(target_bytes.data(), target_bytes.size());
  OFF_CHECK(!decode(reader, record));
  OFF_CHECK(reader.error() == off::Reason::UnsupportedSemantics);

  off::AttemptRecord attempt;
  attempt.last_reason = off::Reason::Ok;
  off::CanonicalWriter writer;
  encode(writer, attempt);
  std::vector<off::u8> bytes = writer.buffer();
  // The trailing reason code is the final two bytes.
  bytes[bytes.size() - 2] = 0x7F;
  bytes[bytes.size() - 1] = 0xFF;
  off::AttemptRecord decoded;
  off::CanonicalReader second(bytes.data(), bytes.size());
  OFF_CHECK(!decode(second, decoded));
  OFF_CHECK(second.error() == off::Reason::UnsupportedSemantics);
}

OFF_TEST(model, json_rendering_is_deterministic_and_complete) {
  const off::TargetRecord record = sample_target();
  const std::string first = to_json(record).dump();
  const std::string second = to_json(record).dump();
  OFF_CHECK_EQ(first, second);
  OFF_CHECK(first.find("\"fidelity\":\"REAL\"") != std::string::npos);
  OFF_CHECK(first.find("\"topology_generation\":2") != std::string::npos);
  const off::FailureEvidence evidence = sample_failure();
  const std::string rendered = to_json(evidence).dump();
  OFF_CHECK(rendered.find("split_brain_suspected") != std::string::npos);
  OFF_CHECK(rendered.find("target_unresponsive") != std::string::npos);
}

OFF_TEST(model, content_digest_changes_with_content) {
  off::FailureEvidence first = sample_failure();
  off::FailureEvidence second = sample_failure();
  OFF_CHECK(off::canonical_digest(first) == off::canonical_digest(second));
  second.failure_class = off::FailureClass::LinkDown;
  OFF_CHECK(off::canonical_digest(first) != off::canonical_digest(second));
}

}  // namespace
