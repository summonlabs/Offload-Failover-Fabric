// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using namespace offtest;

off::FenceVerdict check_fence(Fixture& fixture, const off::TargetName& target, std::uint64_t token) {
  off::FenceCheck check;
  check.service = fixture.service;
  check.holder = fixture.target_ref(target);
  check.presented = off::FenceToken::from_value(token);
  return fixture.fabric.verify_fence(check);
}

OFF_TEST(fence, placement_and_recovery_monotonically_advance_the_durable_token) {
  Fixture fixture;
  fixture.build();
  const off::RecoveryDecision placement = fixture.place(fixture.t1);
  OFF_CHECK(placement.accepted);
  OFF_CHECK_EQ(placement.plan.fence.value(), 1ULL);
  OFF_CHECK_EQ(fixture.service_view().fence.value(), 1ULL);

  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision recovery = fixture.failover(evidence);
  OFF_CHECK(recovery.accepted);
  OFF_CHECK_EQ(recovery.plan.fence.value(), 2ULL);
  OFF_CHECK_EQ(recovery.plan.generation.value(), 2ULL);
  OFF_CHECK_EQ(fixture.service_view().fence.value(), 2ULL);
}

OFF_TEST(fence, an_old_target_cannot_resume_behind_a_higher_token) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  OFF_CHECK(fixture.failover(evidence).accepted);

  const off::FenceVerdict stale = check_fence(fixture, fixture.t1, 1);
  OFF_CHECK(!stale.authorized);
  OFF_CHECK(stale.code == off::Reason::FenceStale);
  OFF_CHECK(stale.had_witness);
  OFF_CHECK_EQ(stale.current.value(), 2ULL);
  OFF_CHECK(stale.current_holder.target == fixture.t2);

  // Presenting the current token from the fenced target is still refused: the
  // token belongs to the recorded holder.
  const off::FenceVerdict impostor = check_fence(fixture, fixture.t1, 2);
  OFF_CHECK(!impostor.authorized);
  OFF_CHECK(impostor.code == off::Reason::FenceHeldByOther);

  const off::FenceVerdict holder = check_fence(fixture, fixture.t2, 2);
  OFF_CHECK(holder.authorized);
  OFF_CHECK(holder.code == off::Reason::AcceptFenceAdvanced);

  const off::FenceVerdict future = check_fence(fixture, fixture.t2, 3);
  OFF_CHECK(!future.authorized);
  OFF_CHECK(future.code == off::Reason::FenceRegression);
}

namespace {

/// Ends the outstanding recovery attempt unsuccessfully so a further recovery
/// is admissible again.
void end_attempt_unsuccessfully(Fixture& fixture) {
  const off::ServiceView view = fixture.service_view();
  OFF_REQUIRE(view.active.has_value());
  OFF_CHECK_REASON(fixture.report_effect(view.current_attempt, view.generation, view.fence,
                                         view.active->target, off::EffectKind::ActivationAccepted,
                                         off::EffectResult::Negative, 0, 1)
                       .outcome,
                   off::Reason::EffectNegative);
  OFF_CHECK_EQ(fixture.service_view().lifecycle, off::LifecyclePhase::Failed);
}

}  // namespace

OFF_TEST(fence, a_fenced_target_stays_out_until_its_incarnation_advances) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  OFF_CHECK(fixture.failover(fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive,
                                                    1))
                .accepted);
  end_attempt_unsuccessfully(fixture);

  // Re-adopting the original target at the same incarnation changes nothing.
  fixture.add_target(fixture.t1, "h1", "nic1", off::DeviceKind::SmartNic, 1, 1);
  fixture.add_capability(fixture.t1, 2, capabilities({off::CapabilityCode::IPv4Forward}), 1, 1);
  const off::EvidenceId second =
      fixture.report_failure(fixture.t2, off::FailureClass::TargetUnresponsive, 2);
  const off::RecoveryDecision decision = fixture.failover(second);
  OFF_REQUIRE(decision.accepted);
  OFF_CHECK(decision.plan.to.target != fixture.t1);
  OFF_CHECK(decision.plan.to.target == fixture.t3);
}

OFF_TEST(fence, a_new_incarnation_releases_the_fence) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  OFF_CHECK(fixture.failover(fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive,
                                                    1))
                .accepted);
  end_attempt_unsuccessfully(fixture);
  fixture.add_target(fixture.t1, "h1", "nic1", off::DeviceKind::SmartNic, 2, 1);
  fixture.add_capability(fixture.t1, 3, capabilities({off::CapabilityCode::IPv4Forward}), 2, 1);
  const off::EvidenceId second =
      fixture.report_failure(fixture.t2, off::FailureClass::TargetUnresponsive, 2);
  const off::RecoveryDecision decision = fixture.failover(second);
  OFF_REQUIRE(decision.accepted);
  OFF_CHECK(decision.plan.to.target == fixture.t1);
  OFF_CHECK(decision.plan.fence.value() > 2ULL);
}

OFF_TEST(fence, unknown_service_has_no_authority_at_all) {
  Fixture fixture;
  fixture.build();
  off::FenceCheck check;
  check.service = service_name("absent");
  check.holder = fixture.target_ref(fixture.t1);
  check.presented = off::FenceToken::from_value(1);
  const off::FenceVerdict verdict = fixture.fabric.verify_fence(check);
  OFF_CHECK(!verdict.authorized);
  OFF_CHECK(verdict.code == off::Reason::NoAuthority);
}

OFF_TEST(fence, retired_target_never_becomes_a_candidate) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  OFF_CHECK(fixture.failover(evidence).accepted);
  OFF_CHECK_REASON(fixture.fabric.retire_target(fixture.t1, fixture.topology_source, nullptr),
                   off::Reason::AcceptTargetRetired);
  end_attempt_unsuccessfully(fixture);
  const off::EvidenceId second =
      fixture.report_failure(fixture.t2, off::FailureClass::TargetUnresponsive, 2);
  const off::RecoveryDecision decision = fixture.failover(second);
  OFF_CHECK_REASON(decision.outcome, off::Reason::AcceptIntentEmitted);
  OFF_REQUIRE(decision.accepted);
  OFF_CHECK(decision.plan.to.target == fixture.t3);
}

}  // namespace
