// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using namespace offtest;

off::RecoveryDecision recover(Fixture& fixture) {
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  return fixture.failover(evidence);
}

OFF_TEST(stateful, continuity_is_unknown_until_an_effect_is_verified) {
  Fixture fixture;
  fixture.build();
  const off::RecoveryDecision decision = recover(fixture);
  OFF_REQUIRE(decision.accepted);
  const off::ServiceView after = fixture.service_view();
  OFF_CHECK_EQ(after.continuity, off::ContinuityClass::Unknown);
  OFF_CHECK(!after.effect_verified);
  OFF_CHECK_EQ(after.recovery, off::RecoveryPhase::Authorized);
}

OFF_TEST(stateful, stateful_recovery_is_degraded_without_a_verified_transfer) {
  Fixture fixture;
  fixture.statefulness = off::Statefulness::Stateful;
  fixture.preconditions.insert(off::Precondition::StateTransfer);
  fixture.build();
  const off::RecoveryDecision decision = recover(fixture);
  OFF_REQUIRE(decision.accepted);
  const off::ServiceView pending = fixture.service_view();
  OFF_CHECK_REASON(fixture.report_effect(pending.current_attempt, pending.generation, pending.fence,
                                         fixture.t2, off::EffectKind::ActivationAccepted,
                                         off::EffectResult::Positive, 0, 1)
                       .outcome,
                   off::Reason::AcceptEffectVerified);
  const off::ServiceView after = fixture.service_view();
  OFF_CHECK_REASON(fixture.report_effect(after.current_attempt, after.generation, after.fence,
                                         fixture.t2, off::EffectKind::ServiceHealthy,
                                         off::EffectResult::Positive, 0, 2)
                       .outcome,
                   off::Reason::AcceptContinuityDegradedStateful);
  const off::ServiceView final_view = fixture.service_view();
  OFF_CHECK_EQ(final_view.continuity, off::ContinuityClass::Degraded);
  OFF_CHECK(final_view.effect_verified);
  OFF_CHECK(!final_view.state_transfer_verified);
}

OFF_TEST(stateful, verified_transfer_promotes_continuity_to_full) {
  Fixture fixture;
  fixture.statefulness = off::Statefulness::Stateful;
  fixture.preconditions.insert(off::Precondition::StateTransfer);
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  off::ServiceView view = fixture.service_view();
  OFF_CHECK_REASON(fixture.report_effect(view.current_attempt, view.generation, view.fence,
                                         fixture.t2, off::EffectKind::StateTransferComplete,
                                         off::EffectResult::Positive, 1, 1)
                       .outcome,
                   off::Reason::AcceptStateTransferVerified);
  view = fixture.service_view();
  OFF_CHECK(view.state_transfer_verified);
  OFF_CHECK_REASON(fixture.report_effect(view.current_attempt, view.generation, view.fence,
                                         fixture.t2, off::EffectKind::ActivationAccepted,
                                         off::EffectResult::Positive, 0, 2)
                       .outcome,
                   off::Reason::AcceptEffectVerified);
  OFF_CHECK_REASON(fixture.report_effect(view.current_attempt, view.generation, view.fence,
                                         fixture.t2, off::EffectKind::ServiceHealthy,
                                         off::EffectResult::Positive, 0, 3)
                       .outcome,
                   off::Reason::AcceptContinuityVerified);
  OFF_CHECK_EQ(fixture.service_view().continuity, off::ContinuityClass::Full);
}

OFF_TEST(stateful, a_stale_transferred_state_generation_is_refused) {
  Fixture fixture;
  fixture.statefulness = off::Statefulness::Stateful;
  fixture.preconditions.insert(off::Precondition::StateTransfer);
  fixture.service_state_generation = off::StateGeneration::from_value(10);
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView view = fixture.service_view();
  const off::RecoveryDecision decision =
      fixture.report_effect(view.current_attempt, view.generation, view.fence, fixture.t2,
                            off::EffectKind::StateTransferComplete, off::EffectResult::Positive,
                            3, 4);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::StateGenerationStale);
}

OFF_TEST(stateful, degraded_continuity_can_be_forbidden_by_policy) {
  Fixture fixture;
  fixture.statefulness = off::Statefulness::Stateful;
  fixture.preconditions.insert(off::Precondition::StateTransfer);
  fixture.build();
  off::PolicyDescriptor policy;
  policy.generation = off::PolicyGeneration::from_value(1);
  policy.scope = fixture.scope;
  policy.allow_degraded_continuity = false;
  OFF_CHECK(off::reason_is_accept(fixture.fabric.set_policy(policy, nullptr)));
  OFF_REQUIRE(recover(fixture).accepted);
  off::ServiceView view = fixture.service_view();
  OFF_CHECK_REASON(fixture.report_effect(view.current_attempt, view.generation, view.fence,
                                         fixture.t2, off::EffectKind::ActivationAccepted,
                                         off::EffectResult::Positive, 0, 1)
                       .outcome,
                   off::Reason::AcceptEffectVerified);
  view = fixture.service_view();
  const off::RecoveryDecision decision =
      fixture.report_effect(view.current_attempt, view.generation, view.fence, fixture.t2,
                            off::EffectKind::ServiceHealthy, off::EffectResult::Positive, 0, 2);
  OFF_CHECK(decision.outcome == off::Reason::ContinuityNotVerified);
  OFF_CHECK_EQ(fixture.service_view().continuity, off::ContinuityClass::None);
}

OFF_TEST(stateful, a_healthy_report_without_an_accepted_activation_is_not_verified) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView view = fixture.service_view();
  const off::RecoveryDecision decision =
      fixture.report_effect(view.current_attempt, view.generation, view.fence, fixture.t2,
                            off::EffectKind::ServiceHealthy, off::EffectResult::Positive, 0, 9);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::EffectUnverified);
  OFF_CHECK_EQ(fixture.service_view().continuity, off::ContinuityClass::Unknown);
}

}  // namespace
