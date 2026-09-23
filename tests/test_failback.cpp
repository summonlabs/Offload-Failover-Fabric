// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using namespace offtest;

/// Places on t1, fails t1, recovers onto t2 and verifies the effect, so the
/// service is genuinely serving from the fallback before failback is asked for.
off::RecoveryDecision recover(Fixture& fixture) {
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(evidence);
  if (decision.accepted) {
    const off::ServiceView view = fixture.service_view();
    fixture.verify_continuity(view.generation, view.fence, fixture.t2);
  }
  return decision;
}

off::RecoveryDecision failback(Fixture& fixture, const off::TargetName& target,
                               std::uint64_t key = 0, bool current_generations = true) {
  const off::FabricView view = fixture.fabric.view();
  off::FailbackRequest request;
  request.key = off::RequestKey::from_value(key);
  request.service = fixture.service;
  request.preferred_target = target;
  request.observed_topology_generation =
      current_generations ? view.topology_generation
                          : off::TopologyGeneration::from_value(view.topology_generation.value() - 1);
  request.observed_policy_generation = view.policy_generation;
  request.requester = fixture.operator_source;
  return fixture.fabric.request_failback(request);
}

OFF_TEST(failback, without_a_recorded_failover_the_request_is_refused) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::RecoveryDecision decision = failback(fixture, fixture.t1);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::FailbackNoFailoverRecorded);
}

OFF_TEST(failback, stale_observed_generations_are_refused) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::RecoveryDecision decision = failback(fixture, fixture.t1, 0, false);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::FailbackStaleGeneration);
}

OFF_TEST(failback, the_original_target_must_be_re_adopted_at_a_newer_incarnation) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::RecoveryDecision decision = failback(fixture, fixture.t1);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::FailbackOriginalFenceStale);
}

OFF_TEST(failback, a_superseded_capability_generation_cannot_be_reused) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  fixture.add_target(fixture.t1, "h1", "nic1", off::DeviceKind::SmartNic, 2, 1);
  // Capability evidence at the new incarnation but at the same generation the
  // failover already superseded.
  off::CapabilityEvidence evidence;
  evidence.target = fixture.t1;
  evidence.generation = off::CapabilityGeneration::from_value(1);
  evidence.capabilities = capabilities({off::CapabilityCode::IPv4Forward});
  evidence.topology_generation = fixture.fabric.view().topology_generation;
  evidence.incarnation.host = off::Incarnation::from_value(2);
  evidence.incarnation.device = off::Incarnation::from_value(1);
  evidence.source = fixture.capability_source;
  OFF_CHECK(off::reason_is_accept(fixture.fabric.ingest_capability(evidence, nullptr)));
  const off::RecoveryDecision decision = failback(fixture, fixture.t1);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::FailbackStaleGeneration);
}

OFF_TEST(failback, fresh_generations_produce_a_governed_reverse_recovery) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView after_failover = fixture.service_view();
  fixture.add_target(fixture.t1, "h1", "nic1", off::DeviceKind::SmartNic, 2, 1);
  fixture.add_capability(fixture.t1, 5, capabilities({off::CapabilityCode::IPv4Forward}), 2, 1);

  const off::RecoveryDecision decision = failback(fixture, fixture.t1);
  OFF_CHECK(decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::AcceptFailbackGenerationFresh);
  OFF_CHECK(decision.plan.to.target == fixture.t1);
  OFF_CHECK(decision.plan.from.target == fixture.t2);
  OFF_CHECK(decision.plan.fence.value() > after_failover.fence.value());
  OFF_CHECK(decision.plan.generation.value() > after_failover.generation.value());
  const off::ServiceView view = fixture.service_view();
  OFF_CHECK(view.active->target == fixture.t1);
  OFF_CHECK(!view.failover_recorded);
  OFF_CHECK_EQ(view.continuity, off::ContinuityClass::Unknown);
}

OFF_TEST(failback, a_second_failback_without_a_new_failover_is_refused) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  fixture.add_target(fixture.t1, "h1", "nic1", off::DeviceKind::SmartNic, 2, 1);
  fixture.add_capability(fixture.t1, 5, capabilities({off::CapabilityCode::IPv4Forward}), 2, 1);
  OFF_CHECK(failback(fixture, fixture.t1).accepted);
  const off::RecoveryDecision repeat = failback(fixture, fixture.t1);
  OFF_CHECK(!repeat.accepted);
  OFF_CHECK(repeat.outcome == off::Reason::FailbackNoFailoverRecorded);
}

OFF_TEST(failback, policy_can_forbid_failback_entirely) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  fixture.add_target(fixture.t1, "h1", "nic1", off::DeviceKind::SmartNic, 2, 1);
  fixture.add_capability(fixture.t1, 5, capabilities({off::CapabilityCode::IPv4Forward}), 2, 1);
  off::PolicyDescriptor policy;
  policy.generation = off::PolicyGeneration::from_value(1);
  policy.scope = fixture.scope;
  policy.allow_failback = false;
  OFF_CHECK(off::reason_is_accept(fixture.fabric.set_policy(policy, nullptr)));
  const off::RecoveryDecision decision = failback(fixture, fixture.t1);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::FailbackNotEligible);
}

OFF_TEST(failback, a_different_preferred_target_is_refused) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::RecoveryDecision decision = failback(fixture, fixture.t3);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::FailbackNotEligible);
}

}  // namespace
