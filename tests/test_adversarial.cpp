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

off::Reason ingest_named(Fixture& fixture, const char* name) {
  off::ServiceDescriptor descriptor;
  descriptor.name = service_name(name);
  descriptor.scope = fixture.scope;
  return fixture.fabric.register_service(descriptor, fixture.topology_source, nullptr);
}

OFF_TEST(adversarial, malformed_service_and_target_identities_are_refused) {
  Fixture fixture;
  fixture.build();
  off::ServiceDescriptor descriptor;
  descriptor.name = service_name("ok");
  OFF_CHECK_REASON(fixture.fabric.register_service(descriptor, fixture.topology_source, nullptr),
                   off::Reason::Malformed);
  descriptor.scope = fixture.scope;
  OFF_CHECK_REASON(fixture.fabric.register_service(descriptor, fixture.failure_source, nullptr),
                   off::Reason::EvidenceReporterUnauthorized);

  off::TargetRecord record;
  record.name = target_name("t9");
  record.host = host_name("h9");
  record.device = device_name("nic9");
  record.scope = fixture.scope;
  OFF_CHECK_REASON(fixture.fabric.register_target(record, fixture.topology_source, nullptr),
                   off::Reason::ImpossibleValue);
}

OFF_TEST(adversarial, duplicate_and_conflicting_registrations_are_refused) {
  Fixture fixture;
  fixture.build();
  off::ServiceDescriptor descriptor;
  descriptor.name = fixture.service;
  descriptor.scope = fixture.scope;
  descriptor.required_capabilities = fixture.required_capabilities;
  OFF_CHECK_REASON(fixture.fabric.register_service(descriptor, fixture.topology_source, nullptr),
                   off::Reason::AcceptDuplicateIdempotent);
  descriptor.required_capabilities.insert(off::CapabilityCode::Srv6Encap);
  OFF_CHECK_REASON(fixture.fabric.register_service(descriptor, fixture.topology_source, nullptr),
                   off::Reason::DuplicateService);

  off::TargetRecord record;
  record.name = fixture.t1;
  record.host = host_name("h1");
  record.device = device_name("nic1");
  record.kind = off::DeviceKind::SmartNic;
  record.scope = fixture.scope;
  record.incarnation.host = off::Incarnation::from_value(1);
  record.incarnation.device = off::Incarnation::from_value(1);
  OFF_CHECK_REASON(fixture.fabric.register_target(record, fixture.topology_source, nullptr),
                   off::Reason::AcceptDuplicateIdempotent);
  record.device = device_name("nicX");
  OFF_CHECK_REASON(fixture.fabric.register_target(record, fixture.topology_source, nullptr),
                   off::Reason::DuplicateTarget);
  record.device = device_name("nic1");
  record.incarnation.host = off::Incarnation::from_value(0);
  OFF_CHECK_REASON(fixture.fabric.register_target(record, fixture.topology_source, nullptr),
                   off::Reason::ImpossibleValue);
  record.incarnation.host = off::Incarnation::from_value(1);
  record.incarnation.device = off::Incarnation::from_value(0);
  OFF_CHECK_REASON(fixture.fabric.register_target(record, fixture.topology_source, nullptr),
                   off::Reason::ImpossibleValue);
}

OFF_TEST(adversarial, incarnation_regression_is_refused) {
  Fixture fixture;
  fixture.build();
  fixture.add_target(fixture.t1, "h1", "nic1", off::DeviceKind::SmartNic, 4, 4);
  off::TargetRecord record;
  record.name = fixture.t1;
  record.host = host_name("h1");
  record.device = device_name("nic1");
  record.kind = off::DeviceKind::SmartNic;
  record.scope = fixture.scope;
  record.incarnation.host = off::Incarnation::from_value(3);
  record.incarnation.device = off::Incarnation::from_value(4);
  OFF_CHECK_REASON(fixture.fabric.register_target(record, fixture.topology_source, nullptr),
                   off::Reason::IncarnationRegression);
}

OFF_TEST(adversarial, unenrolled_sources_contribute_nothing) {
  Fixture fixture;
  fixture.build();
  off::CapabilityEvidence capability;
  capability.target = fixture.t1;
  capability.generation = off::CapabilityGeneration::from_value(9);
  capability.capabilities = capabilities({off::CapabilityCode::IPv4Forward});
  capability.topology_generation = fixture.fabric.view().topology_generation;
  capability.incarnation = fixture.incarnation_of(fixture.t1);
  capability.source = source_name("stranger");
  OFF_CHECK_REASON(fixture.fabric.ingest_capability(capability, nullptr),
                   off::Reason::EvidenceReporterUnauthorized);

  off::FailureEvidence failure;
  failure.service = fixture.service;
  failure.target = fixture.target_ref(fixture.t1);
  failure.provenance.source = source_name("stranger");
  failure.provenance.sequence = off::EvidenceSeq::from_value(1);
  failure.provenance.epoch = fixture.fabric.view().epoch;
  failure.provenance.boot = fixture.fabric.view().boot;
  failure.topology_generation = fixture.fabric.view().topology_generation;
  OFF_CHECK_REASON(fixture.fabric.ingest_failure(failure, nullptr),
                   off::Reason::EvidenceReporterUnauthorized);
}

OFF_TEST(adversarial, zero_sequence_and_zero_incarnation_evidence_are_refused) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  off::FailureEvidence failure;
  failure.service = fixture.service;
  failure.target = fixture.target_ref(fixture.t1);
  failure.provenance.source = fixture.failure_source;
  failure.provenance.epoch = fixture.fabric.view().epoch;
  failure.provenance.boot = fixture.fabric.view().boot;
  failure.topology_generation = fixture.fabric.view().topology_generation;
  OFF_CHECK_REASON(fixture.fabric.ingest_failure(failure, nullptr),
                   off::Reason::EvidenceIncomplete);

  failure.provenance.sequence = off::EvidenceSeq::from_value(1);
  failure.target.incarnation.host = off::Incarnation::from_value(0);
  OFF_CHECK_REASON(fixture.fabric.ingest_failure(failure, nullptr),
                   off::Reason::ImpossibleValue);
}

OFF_TEST(adversarial, future_generations_are_refused) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  off::FailureEvidence failure;
  failure.service = fixture.service;
  failure.target = fixture.target_ref(fixture.t1);
  failure.provenance.source = fixture.failure_source;
  failure.provenance.sequence = off::EvidenceSeq::from_value(1);
  failure.provenance.epoch = fixture.fabric.view().epoch;
  failure.provenance.boot = fixture.fabric.view().boot;
  failure.topology_generation = fixture.fabric.view().topology_generation;
  failure.policy_generation = off::PolicyGeneration::from_value(9);
  OFF_CHECK_REASON(fixture.fabric.ingest_failure(failure, nullptr),
                   off::Reason::FutureGeneration);
  failure.policy_generation = off::PolicyGeneration::from_value(0);
  failure.topology_generation = off::TopologyGeneration::from_value(9);
  OFF_CHECK_REASON(fixture.fabric.ingest_failure(failure, nullptr),
                   off::Reason::EvidenceTopologyMismatch);
}

OFF_TEST(adversarial, impossible_policy_budgets_are_refused) {
  Fixture fixture;
  fixture.build();
  off::PolicyDescriptor policy;
  policy.generation = off::PolicyGeneration::from_value(1);
  policy.scope = fixture.scope;
  policy.max_plan_steps = 0;
  OFF_CHECK_REASON(fixture.fabric.set_policy(policy, nullptr), off::Reason::ImpossibleValue);
  policy.max_plan_steps = 4;
  policy.max_attempts_per_generation = 0;
  OFF_CHECK_REASON(fixture.fabric.set_policy(policy, nullptr), off::Reason::ImpossibleValue);
  policy.max_attempts_per_generation = 1;
  OFF_CHECK(off::reason_is_accept(fixture.fabric.set_policy(policy, nullptr)));
  OFF_CHECK_REASON(fixture.fabric.set_policy(policy, nullptr),
                   off::Reason::AcceptDuplicateIdempotent);
  policy.max_plan_steps = 5;
  OFF_CHECK_REASON(fixture.fabric.set_policy(policy, nullptr), off::Reason::GenerationConflict);
}

OFF_TEST(adversarial, topology_generation_may_not_move_backwards) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK_REASON(fixture.fabric.set_topology_generation(
                       off::TopologyGeneration::from_value(0), fixture.topology_source, nullptr),
                   off::Reason::StaleGeneration);
  OFF_CHECK_REASON(fixture.fabric.set_topology_generation(
                       off::TopologyGeneration::from_value(1), fixture.topology_source, nullptr),
                   off::Reason::AcceptDuplicateIdempotent);
  OFF_CHECK_REASON(fixture.fabric.set_topology_generation(
                       off::TopologyGeneration::from_value(3), fixture.topology_source, nullptr),
                   off::Reason::AcceptTopologyGenerationAdvanced);
  OFF_CHECK_REASON(fixture.fabric.set_topology_generation(
                       off::TopologyGeneration::from_value(2), fixture.topology_source, nullptr),
                   off::Reason::StaleGeneration);
}

OFF_TEST(adversarial, self_referential_and_duplicate_dependencies_are_refused) {
  Fixture fixture;
  fixture.dependencies.push_back(off::DependencySpec{
      fixture.service, off::DependencyKind::Hard, off::DependencyRequirement::AnyTarget});
  OFF_CHECK_REASON(fixture.fabric.open(), off::Reason::Ok);
  fixture.enroll(fixture.operator_source, off::SourceRole::Operator);
  fixture.enroll(fixture.topology_source, off::SourceRole::TopologyReporter);
  off::ServiceDescriptor descriptor;
  descriptor.name = fixture.service;
  descriptor.scope = fixture.scope;
  descriptor.dependencies = fixture.dependencies;
  OFF_CHECK_REASON(fixture.fabric.register_service(descriptor, fixture.topology_source, nullptr),
                   off::Reason::DependencySelfReference);

  descriptor.dependencies.clear();
  descriptor.dependencies.push_back(off::DependencySpec{
      service_name("dns"), off::DependencyKind::Hard, off::DependencyRequirement::AnyTarget});
  descriptor.dependencies.push_back(off::DependencySpec{
      service_name("dns"), off::DependencyKind::Soft, off::DependencyRequirement::AnyTarget});
  OFF_CHECK_REASON(fixture.fabric.register_service(descriptor, fixture.topology_source, nullptr),
                   off::Reason::DuplicateIdentity);
}

OFF_TEST(adversarial, unknown_services_and_targets_are_refused) {
  Fixture fixture;
  fixture.build();
  off::FailureEvidence failure;
  failure.service = service_name("absent");
  failure.target = fixture.target_ref(fixture.t1);
  failure.provenance.source = fixture.failure_source;
  failure.provenance.sequence = off::EvidenceSeq::from_value(1);
  failure.provenance.epoch = fixture.fabric.view().epoch;
  failure.provenance.boot = fixture.fabric.view().boot;
  failure.topology_generation = fixture.fabric.view().topology_generation;
  OFF_CHECK_REASON(fixture.fabric.ingest_failure(failure, nullptr), off::Reason::UnknownService);

  failure.service = fixture.service;
  failure.target.target = target_name("absent");
  OFF_CHECK_REASON(fixture.fabric.ingest_failure(failure, nullptr), off::Reason::UnknownTarget);
}

OFF_TEST(adversarial, effects_for_a_service_without_an_attempt_are_refused) {
  Fixture fixture;
  fixture.build();
  const off::RecoveryDecision decision =
      fixture.report_effect(off::AttemptId::from_value(1), off::FailoverGeneration::from_value(1),
                            off::FenceToken::from_value(1), fixture.t1,
                            off::EffectKind::ActivationAccepted, off::EffectResult::Positive, 0, 1);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::NoFailoverInProgress);
}

OFF_TEST(adversarial, frame_codec_rejects_every_malformed_shape) {
  off::Frame frame;
  frame.kind = off::WireMessage::Request;
  frame.correlation = 7;
  frame.payload = {1, 2, 3, 4};
  std::vector<off::u8> encoded;
  OFF_CHECK_REASON(off::encode_frame(frame, encoded), off::Reason::Ok);

  const auto decode = [](const std::vector<off::u8>& bytes, off::Frame& out, std::size_t& consumed) {
    return off::decode_frame(bytes.data(), bytes.size(), out, consumed);
  };

  for (std::size_t size = 0; size < encoded.size(); ++size) {
    off::Frame partial;
    std::size_t consumed = 0;
    const off::Reason code = decode(std::vector<off::u8>(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(size)), partial, consumed);
    OFF_CHECK(code != off::Reason::Ok);
  }

  std::vector<off::u8> bad_magic = encoded;
  bad_magic[0] = 0;
  off::Frame out;
  std::size_t consumed = 0;
  OFF_CHECK_REASON(decode(bad_magic, out, consumed), off::Reason::Malformed);

  std::vector<off::u8> bad_version = encoded;
  bad_version[5] = 0x63;
  OFF_CHECK_REASON(decode(bad_version, out, consumed), off::Reason::ProtocolVersionMismatch);

  std::vector<off::u8> bad_crc = encoded;
  bad_crc.back() ^= 0xFFU;
  OFF_CHECK_REASON(decode(bad_crc, out, consumed), off::Reason::ChecksumMismatch);

  std::vector<off::u8> bad_kind = encoded;
  bad_kind[6] = 0;
  bad_kind[7] = 0x77;
  OFF_CHECK_REASON(decode(bad_kind, out, consumed), off::Reason::UnsupportedSemantics);

  std::vector<off::u8> oversize = encoded;
  oversize[16] = 0xFF;
  oversize[17] = 0xFF;
  oversize[18] = 0xFF;
  oversize[19] = 0xFF;
  OFF_CHECK_REASON(decode(oversize, out, consumed), off::Reason::FrameTooLarge);

  OFF_CHECK_REASON(decode(encoded, out, consumed), off::Reason::Ok);
  OFF_CHECK_EQ(consumed, encoded.size());
  OFF_CHECK(out == frame);
}

OFF_TEST(adversarial, oversized_frames_are_refused_before_allocation) {
  off::Frame frame;
  frame.payload.assign((1U << 20U) + 1U, 0);
  std::vector<off::u8> encoded;
  OFF_CHECK_REASON(off::encode_frame(frame, encoded, 1U << 20U), off::Reason::Oversized);
  OFF_CHECK(encoded.empty());
}

OFF_TEST(adversarial, request_codec_rejects_trailing_and_unknown_bytes) {
  off::WireRequest request;
  request.kind = off::WireRequestKind::QueryView;
  request.key = 5;
  request.body = {9, 9, 9};
  std::vector<off::u8> encoded;
  OFF_CHECK_REASON(off::encode_request(request, encoded), off::Reason::Ok);
  off::WireRequest decoded;
  OFF_CHECK_REASON(off::decode_request(encoded.data(), encoded.size(), decoded), off::Reason::Ok);
  OFF_CHECK(decoded == request);

  std::vector<off::u8> trailing = encoded;
  trailing.push_back(0);
  OFF_CHECK_REASON(off::decode_request(trailing.data(), trailing.size(), decoded),
                   off::Reason::Malformed);

  std::vector<off::u8> unknown = encoded;
  unknown[0] = 0x00;
  unknown[1] = 0xFE;
  OFF_CHECK_REASON(off::decode_request(unknown.data(), unknown.size(), decoded),
                   off::Reason::UnsupportedSemantics);

  std::vector<off::u8> truncated(encoded.begin(), encoded.begin() + 3);
  OFF_CHECK(off::decode_request(truncated.data(), truncated.size(), decoded) != off::Reason::Ok);
}

OFF_TEST(adversarial, response_codec_rejects_unknown_reason_codes) {
  off::WireResponse response;
  response.status = off::Reason::FenceStale;
  response.accepted = false;
  response.body = "{}";
  std::vector<off::u8> encoded;
  OFF_CHECK_REASON(off::encode_response(response, encoded), off::Reason::Ok);
  off::WireResponse decoded;
  OFF_CHECK_REASON(off::decode_response(encoded.data(), encoded.size(), decoded), off::Reason::Ok);
  OFF_CHECK(decoded.status == off::Reason::FenceStale);
  OFF_CHECK_EQ(decoded.body, std::string("{}"));

  encoded[1] = 0x77;
  OFF_CHECK_REASON(off::decode_response(encoded.data(), encoded.size(), decoded),
                   off::Reason::UnsupportedSemantics);
}

OFF_TEST(adversarial, execution_of_unknown_request_kinds_fails_closed) {
  Fixture fixture;
  fixture.build();
  off::WireRequest request;
  request.kind = static_cast<off::WireRequestKind>(0x3FFF);
  off::WireResponse response;
  const off::Reason code = off::execute_request(fixture.fabric, request, response);
  OFF_CHECK(code == off::Reason::RequestUnknown);
}

OFF_TEST(adversarial, absurd_request_bodies_never_produce_a_valid_looking_success) {
  Fixture fixture;
  fixture.build();
  off::WireRequest request;
  request.kind = off::WireRequestKind::RegisterService;
  request.body.assign(64, 0xFF);
  off::WireResponse response;
  const off::Reason code = off::execute_request(fixture.fabric, request, response);
  OFF_CHECK(code != off::Reason::Ok);
  OFF_CHECK(!response.accepted);
}

OFF_TEST(adversarial, maximum_and_boundary_counters_do_not_wrap) {
  off::Gen<off::ServiceTag> generation =
      off::Gen<off::ServiceTag>::from_value((std::numeric_limits<off::u64>::max)());
  OFF_CHECK(!generation.successor().has_value());
  off::LogicalTick tick = off::LogicalTick::from_value((std::numeric_limits<off::u64>::max)());
  OFF_CHECK(!tick.successor().has_value());
  OFF_CHECK_EQ(off::saturating_inc((std::numeric_limits<off::u64>::max)()),
               (std::numeric_limits<off::u64>::max)());
  off::u64 product = 0;
  OFF_CHECK(off::mul_overflow(1ULL << 32U, 1ULL << 32U, product));
  OFF_CHECK(!off::mul_overflow(1ULL << 31U, 2ULL, product));
  OFF_CHECK_EQ(product, 1ULL << 32U);
}

}  // namespace
