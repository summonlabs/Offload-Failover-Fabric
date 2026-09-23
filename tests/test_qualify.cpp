// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using namespace offtest;

void enroll_service(Fixture& fixture, const char* name, const off::CapabilitySet& required) {
  off::ServiceDescriptor descriptor;
  descriptor.name = service_name(name);
  descriptor.scope = fixture.scope;
  descriptor.statefulness = off::Statefulness::Stateless;
  descriptor.required_capabilities = required;
  OFF_CHECK(off::reason_is_accept(
      fixture.fabric.register_service(descriptor, fixture.topology_source, nullptr)));
}

void observe_dependency(Fixture& fixture, const char* service, const char* dependency,
                        off::DependencyHealth health, std::uint64_t sequence) {
  off::DependencyObservation observation;
  observation.service = service_name(service);
  observation.dependency = service_name(dependency);
  observation.health = health;
  observation.provenance.source = fixture.dependency_source;
  observation.provenance.sequence = off::EvidenceSeq::from_value(sequence);
  const off::FabricView view = fixture.fabric.view();
  observation.provenance.epoch = view.epoch;
  observation.provenance.boot = view.boot;
  observation.topology_generation = view.topology_generation;
  OFF_CHECK(off::reason_is_accept(fixture.fabric.ingest_dependency(observation, nullptr)));
}

OFF_TEST(qualify, recovery_without_evidence_is_refused) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::RecoveryDecision decision = fixture.failover(off::EvidenceId{});
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::EvidenceMissing);
  OFF_CHECK_EQ(fixture.service_view().lifecycle, off::LifecyclePhase::Active);
}

OFF_TEST(qualify, disappeared_target_alone_is_never_permission) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  OFF_CHECK(off::reason_is_accept(
      fixture.fabric.retire_target(fixture.t1, fixture.topology_source, nullptr)));
  const off::RecoveryDecision decision = fixture.failover(off::EvidenceId{});
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::TargetDisappearanceIsNotPermission);
  const off::ServiceView view = fixture.service_view();
  OFF_CHECK(view.active.has_value());
  OFF_CHECK(view.active->target == fixture.t1);
  OFF_CHECK_EQ(view.continuity, off::ContinuityClass::Unknown);
}

OFF_TEST(qualify, evidence_for_a_target_that_moved_on_is_refused) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  // Evidence about a target that is not the current placement is refused at
  // ingestion, so it can never be stockpiled for a later recovery.
  off::FailureEvidence wrong;
  wrong.service = fixture.service;
  wrong.target = fixture.target_ref(fixture.t2);
  wrong.failure_class = off::FailureClass::TargetUnresponsive;
  wrong.provenance.source = fixture.failure_source;
  wrong.provenance.sequence = off::EvidenceSeq::from_value(2);
  const off::FabricView snapshot = fixture.fabric.view();
  wrong.provenance.epoch = snapshot.epoch;
  wrong.provenance.boot = snapshot.boot;
  wrong.topology_generation = snapshot.topology_generation;
  OFF_CHECK_REASON(fixture.fabric.ingest_failure(wrong, nullptr),
                   off::Reason::EvidenceTargetMismatch);
  OFF_REQUIRE(fixture.failover(evidence).accepted);
  // End the first attempt unsuccessfully so a second recovery is admissible.
  const off::ServiceView moved = fixture.service_view();
  OFF_CHECK_REASON(fixture.report_effect(moved.current_attempt, moved.generation, moved.fence,
                                         fixture.t2, off::EffectKind::ActivationAccepted,
                                         off::EffectResult::Negative, 0, 1)
                       .outcome,
                   off::Reason::EffectNegative);
  // The retained evidence still names t1, which is no longer the placement.
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::EvidenceTargetMismatch);
}

OFF_TEST(qualify, stale_evidence_cannot_justify_recovery) {
  Fixture fixture;
  fixture.build();
  off::PolicyDescriptor policy;
  policy.generation = off::PolicyGeneration::from_value(1);
  policy.scope = fixture.scope;
  policy.evidence_max_age_ticks = 2;
  OFF_CHECK(off::reason_is_accept(fixture.fabric.set_policy(policy, nullptr)));
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  for (std::uint64_t index = 0; index < 4; ++index) {
    fixture.add_capability(fixture.t1, 2 + index,
                           capabilities({off::CapabilityCode::IPv4Forward}), 1, 1);
  }
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_CHECK(decision.outcome == off::Reason::EvidenceStale);
  OFF_CHECK(fixture.fabric.stats().evidence_rejected_stale > 0);
}

OFF_TEST(qualify, replayed_and_conflicting_evidence_is_refused) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId first =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 5);

  off::FailureEvidence duplicate;
  duplicate.id = first;
  duplicate.service = fixture.service;
  duplicate.target = fixture.target_ref(fixture.t1);
  duplicate.failure_class = off::FailureClass::TargetUnresponsive;
  duplicate.provenance.source = fixture.failure_source;
  duplicate.provenance.sequence = off::EvidenceSeq::from_value(5);
  const off::FabricView view = fixture.fabric.view();
  duplicate.provenance.epoch = view.epoch;
  duplicate.provenance.boot = view.boot;
  duplicate.topology_generation = view.topology_generation;
  OFF_CHECK_REASON(fixture.fabric.ingest_failure(duplicate, nullptr),
                   off::Reason::AcceptDuplicateIdempotent);

  duplicate.failure_class = off::FailureClass::LinkDown;
  OFF_CHECK_REASON(fixture.fabric.ingest_failure(duplicate, nullptr),
                   off::Reason::EvidenceConflicting);

  off::FailureEvidence replay;
  replay.service = fixture.service;
  replay.target = fixture.target_ref(fixture.t1);
  replay.failure_class = off::FailureClass::LinkDown;
  replay.provenance.source = fixture.failure_source;
  replay.provenance.sequence = off::EvidenceSeq::from_value(4);
  replay.provenance.epoch = view.epoch;
  replay.provenance.boot = view.boot;
  replay.topology_generation = view.topology_generation;
  OFF_CHECK_REASON(fixture.fabric.ingest_failure(replay, nullptr),
                   off::Reason::EvidenceReplayRejected);
}

OFF_TEST(qualify, ambiguous_evidence_never_activates_a_replacement) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence = fixture.report_failure(
      fixture.t1, off::FailureClass::TargetUnresponsive, 1, off::AmbiguityState::Ambiguous);
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::AmbiguityUnresolved);
  const off::ServiceView pending = fixture.service_view();
  OFF_CHECK(pending.ambiguity_pending);
  OFF_CHECK_EQ(pending.recovery, off::RecoveryPhase::AmbiguityPending);
  OFF_CHECK(pending.active.has_value());
  OFF_CHECK(pending.active->target == fixture.t1);
  OFF_CHECK_EQ(pending.generation.value(), 1ULL);
}

OFF_TEST(qualify, operator_can_deny_an_ambiguous_outcome) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence = fixture.report_failure(
      fixture.t1, off::FailureClass::TargetUnresponsive, 1, off::AmbiguityState::SplitBrainSuspected);
  OFF_CHECK(fixture.failover(evidence).outcome == off::Reason::AmbiguityUnresolved);
  off::AmbiguityResolution resolution;
  resolution.service = fixture.service;
  resolution.evidence = evidence;
  resolution.confirm_failure = false;
  resolution.resolved_by = fixture.operator_source;
  const off::RecoveryDecision decision = fixture.fabric.resolve_ambiguity(resolution);
  OFF_CHECK(decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::AcceptAmbiguityResolvedNoFailover);
  const off::ServiceView view = fixture.service_view();
  OFF_CHECK(!view.ambiguity_pending);
  OFF_CHECK(view.active.has_value());
  OFF_CHECK(view.active->target == fixture.t1);
  OFF_CHECK_EQ(view.continuity, off::ContinuityClass::None);

  const off::RecoveryDecision repeat = fixture.fabric.resolve_ambiguity(resolution);
  OFF_CHECK(repeat.outcome == off::Reason::AmbiguityAlreadyResolved);
}

OFF_TEST(qualify, operator_can_confirm_an_ambiguous_outcome_and_recovery_proceeds) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence = fixture.report_failure(
      fixture.t1, off::FailureClass::TargetUnresponsive, 1, off::AmbiguityState::Ambiguous);
  OFF_CHECK(fixture.failover(evidence).outcome == off::Reason::AmbiguityUnresolved);
  off::AmbiguityResolution resolution;
  resolution.service = fixture.service;
  resolution.evidence = evidence;
  resolution.confirm_failure = true;
  resolution.resolved_by = fixture.operator_source;
  const off::RecoveryDecision decision = fixture.fabric.resolve_ambiguity(resolution);
  OFF_CHECK(decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::AcceptIntentEmitted);
  const off::ServiceView view = fixture.service_view();
  OFF_CHECK(!view.ambiguity_pending);
  OFF_CHECK(view.active.has_value());
  OFF_CHECK(view.active->target == fixture.t2);
  OFF_CHECK(view.fence.value() > 1ULL);
}

OFF_TEST(qualify, missing_dependency_service_is_reported_unknown) {
  Fixture fixture;
  fixture.dependencies.push_back(off::DependencySpec{
      service_name("dns"), off::DependencyKind::Hard, off::DependencyRequirement::AnyTarget});
  fixture.build();
  const off::RecoveryDecision decision = fixture.place(fixture.t1);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::UnknownService);
  OFF_CHECK(!fixture.service_view().active.has_value());
}

OFF_TEST(qualify, a_hard_dependency_without_evidence_blocks_placement) {
  Fixture fixture;
  fixture.extra_services.push_back(service_name("dns"));
  fixture.dependencies.push_back(off::DependencySpec{
      service_name("dns"), off::DependencyKind::Hard, off::DependencyRequirement::AnyTarget});
  fixture.build();
  const off::RecoveryDecision decision = fixture.place(fixture.t1);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::DependencyUnknown);
  OFF_CHECK(!fixture.service_view().active.has_value());
}

OFF_TEST(qualify, a_healthy_dependency_allows_placement) {
  Fixture fixture;
  fixture.extra_services.push_back(service_name("dns"));
  fixture.dependencies.push_back(off::DependencySpec{
      service_name("dns"), off::DependencyKind::Hard, off::DependencyRequirement::AnyTarget});
  fixture.build();
  fixture.observe_dependency("dns", off::DependencyHealth::Healthy, 1);
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  OFF_CHECK(fixture.service_view().active->target == fixture.t1);
}

OFF_TEST(qualify, a_dependency_that_fails_later_blocks_recovery) {
  Fixture fixture;
  fixture.extra_services.push_back(service_name("dns"));
  fixture.dependencies.push_back(off::DependencySpec{
      service_name("dns"), off::DependencyKind::Hard, off::DependencyRequirement::AnyTarget});
  fixture.build();
  fixture.observe_dependency("dns", off::DependencyHealth::Healthy, 1);
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  fixture.observe_dependency("dns", off::DependencyHealth::Failed, 2);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::DependencyHardUnmet);
  OFF_CHECK(fixture.service_view().active->target == fixture.t1);
}

OFF_TEST(qualify, an_unknown_dependency_health_blocks_recovery) {
  Fixture fixture;
  fixture.extra_services.push_back(service_name("dns"));
  fixture.dependencies.push_back(off::DependencySpec{
      service_name("dns"), off::DependencyKind::Hard, off::DependencyRequirement::AnyTarget});
  fixture.build();
  fixture.observe_dependency("dns", off::DependencyHealth::Healthy, 1);
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  fixture.observe_dependency("dns", off::DependencyHealth::Unknown, 2);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  OFF_CHECK(fixture.failover(evidence).outcome == off::Reason::DependencyUnknown);
}

OFF_TEST(qualify, a_soft_dependency_failure_does_not_block_recovery) {
  Fixture fixture;
  fixture.extra_services.push_back(service_name("audit"));
  fixture.dependencies.push_back(off::DependencySpec{
      service_name("audit"), off::DependencyKind::Soft, off::DependencyRequirement::AnyTarget});
  fixture.build();
  fixture.observe_dependency("audit", off::DependencyHealth::Healthy, 1);
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  fixture.observe_dependency("audit", off::DependencyHealth::Failed, 2);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  OFF_CHECK(fixture.failover(evidence).accepted);
}

OFF_TEST(qualify, an_observation_for_an_unregistered_dependency_is_refused) {
  Fixture fixture;
  fixture.extra_services.push_back(service_name("dns"));
  fixture.dependencies.push_back(off::DependencySpec{
      service_name("dns"), off::DependencyKind::Hard, off::DependencyRequirement::AnyTarget});
  fixture.build();
  fixture.observe_dependency("dns", off::DependencyHealth::Healthy, 1);
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  off::DependencyObservation observation;
  observation.service = fixture.service;
  observation.dependency = service_name("absent");
  observation.health = off::DependencyHealth::Healthy;
  observation.provenance.source = fixture.dependency_source;
  observation.provenance.sequence = off::EvidenceSeq::from_value(9);
  const off::FabricView view = fixture.fabric.view();
  observation.provenance.epoch = view.epoch;
  observation.provenance.boot = view.boot;
  observation.topology_generation = view.topology_generation;
  OFF_CHECK_REASON(fixture.fabric.ingest_dependency(observation, nullptr),
                   off::Reason::UnknownService);
}

OFF_TEST(qualify, fallback_must_support_the_required_capabilities) {
  Fixture fixture;
  fixture.required_capabilities =
      capabilities({off::CapabilityCode::IPv4Forward, off::CapabilityCode::DpuProgrammable});
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t2).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t2, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(evidence, 0, true);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::FallbackIncompatible);
  OFF_CHECK(fixture.service_view().active->target == fixture.t2);
}

OFF_TEST(qualify, missing_capability_evidence_is_not_absence_of_requirement) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  OFF_CHECK(off::reason_is_accept(
      fixture.fabric.retire_target(fixture.t2, fixture.topology_source, nullptr)));
  OFF_CHECK(off::reason_is_accept(
      fixture.fabric.retire_target(fixture.t3, fixture.topology_source, nullptr)));
  // A target admitted to the topology with no capability evidence at all.
  fixture.add_target(target_name("t4"), "h4", "nic4", off::DeviceKind::SmartNic, 1, 1);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::FallbackUnsupported);
}

OFF_TEST(qualify, no_candidate_at_all_is_reported_as_such) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  OFF_CHECK(off::reason_is_accept(
      fixture.fabric.retire_target(fixture.t2, fixture.topology_source, nullptr)));
  OFF_CHECK(off::reason_is_accept(
      fixture.fabric.retire_target(fixture.t3, fixture.topology_source, nullptr)));
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_CHECK(decision.outcome == off::Reason::NoEligibleFallback);
}

OFF_TEST(qualify, candidate_outside_the_governed_scope_is_never_used) {
  Fixture fixture;
  fixture.build();
  off::TargetRecord foreign;
  foreign.name = target_name("far1");
  foreign.host = host_name("h9");
  foreign.device = device_name("nic9");
  foreign.kind = off::DeviceKind::SmartNic;
  foreign.scope = scope_name("other");
  foreign.incarnation.host = off::Incarnation::from_value(1);
  foreign.incarnation.device = off::Incarnation::from_value(1);
  OFF_CHECK(off::reason_is_accept(
      fixture.fabric.register_target(foreign, fixture.topology_source, nullptr)));
  off::CapabilityEvidence capability;
  capability.target = foreign.name;
  capability.generation = off::CapabilityGeneration::from_value(1);
  capability.capabilities = capabilities({off::CapabilityCode::IPv4Forward});
  capability.topology_generation = fixture.fabric.view().topology_generation;
  capability.incarnation = foreign.incarnation;
  capability.source = fixture.capability_source;
  OFF_CHECK(off::reason_is_accept(fixture.fabric.ingest_capability(capability, nullptr)));

  OFF_CHECK(fixture.place(fixture.t1).accepted);
  OFF_CHECK(off::reason_is_accept(
      fixture.fabric.retire_target(fixture.t2, fixture.topology_source, nullptr)));
  OFF_CHECK(off::reason_is_accept(
      fixture.fabric.retire_target(fixture.t3, fixture.topology_source, nullptr)));
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::FallbackNotInGovernedScope);
}

OFF_TEST(qualify, attempt_ceiling_stops_an_unbounded_retry_loop) {
  Fixture fixture;
  fixture.build();
  off::PolicyDescriptor policy;
  policy.generation = off::PolicyGeneration::from_value(1);
  policy.scope = fixture.scope;
  policy.max_attempts_per_generation = 1;
  OFF_CHECK(off::reason_is_accept(fixture.fabric.set_policy(policy, nullptr)));
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId first =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(first);
  OFF_CHECK(decision.accepted);
  const off::AttemptId attempt = fixture.attempt_id();
  const off::ServiceView after = fixture.service_view();
  OFF_CHECK_REASON(fixture.report_effect(attempt, after.generation, after.fence, fixture.t2,
                                         off::EffectKind::ActivationAccepted,
                                         off::EffectResult::Negative, 0, 1)
                       .outcome,
                   off::Reason::EffectNegative);
  OFF_CHECK_EQ(fixture.service_view().lifecycle, off::LifecyclePhase::Failed);

  const off::EvidenceId second =
      fixture.report_failure(fixture.t2, off::FailureClass::TargetUnresponsive, 2);
  const off::RecoveryDecision retry = fixture.failover(second);
  OFF_CHECK(!retry.accepted);
  OFF_CHECK(retry.outcome == off::Reason::AttemptBudgetExhausted);
}

OFF_TEST(qualify, operator_authorization_does_not_replace_capability_support) {
  Fixture fixture;
  fixture.required_capabilities =
      capabilities({off::CapabilityCode::IPv4Forward, off::CapabilityCode::Srv6Encap});
  fixture.build();
  OFF_CHECK(!fixture.place(fixture.t2).accepted);
  const off::RecoveryDecision decision = fixture.place(fixture.t2, 0);
  OFF_CHECK(decision.outcome == off::Reason::FallbackIncompatible);
}

OFF_TEST(qualify, topology_generation_change_invalidates_evidence_bindings) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  OFF_CHECK(off::reason_is_accept(fixture.fabric.set_topology_generation(
      off::TopologyGeneration::from_value(2), fixture.topology_source, nullptr)));
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_CHECK(decision.outcome == off::Reason::EvidenceTopologyMismatch);
}

}  // namespace
