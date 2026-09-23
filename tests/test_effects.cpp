// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using namespace offtest;

/// Places the service on t1, fails t1 and recovers onto t2.
off::RecoveryDecision recover(Fixture& fixture) {
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  return fixture.failover(evidence);
}

OFF_TEST(effects, fence_mismatch_is_refused) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView view = fixture.service_view();

  const off::RecoveryDecision stale =
      fixture.report_effect(view.current_attempt, view.generation,
                            off::FenceToken::from_value(view.fence.value() - 1), fixture.t2,
                            off::EffectKind::ActivationAccepted, off::EffectResult::Positive, 0, 1);
  OFF_CHECK(!stale.accepted);
  OFF_CHECK(stale.outcome == off::Reason::FenceStale);

  const off::RecoveryDecision future =
      fixture.report_effect(view.current_attempt, view.generation,
                            off::FenceToken::from_value(view.fence.value() + 5), fixture.t2,
                            off::EffectKind::ActivationAccepted, off::EffectResult::Positive, 0, 2);
  OFF_CHECK(!future.accepted);
  OFF_CHECK(future.outcome == off::Reason::FenceRegression);
}

OFF_TEST(effects, attempt_and_generation_mismatches_are_refused) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView view = fixture.service_view();

  const off::RecoveryDecision wrong_attempt =
      fixture.report_effect(off::AttemptId::from_value(view.current_attempt.value() + 7),
                            view.generation, view.fence, fixture.t2,
                            off::EffectKind::ActivationAccepted, off::EffectResult::Positive, 0, 1);
  OFF_CHECK(wrong_attempt.outcome == off::Reason::EffectAttemptMismatch);

  const off::RecoveryDecision wrong_generation =
      fixture.report_effect(view.current_attempt,
                            off::FailoverGeneration::from_value(view.generation.value() - 1),
                            view.fence, fixture.t2, off::EffectKind::ActivationAccepted,
                            off::EffectResult::Positive, 0, 2);
  OFF_CHECK(wrong_generation.outcome == off::Reason::SupersededGeneration);
}

OFF_TEST(effects, reports_naming_the_wrong_target_or_incarnation_are_refused) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView view = fixture.service_view();

  const off::RecoveryDecision wrong_target =
      fixture.report_effect(view.current_attempt, view.generation, view.fence, fixture.t3,
                            off::EffectKind::ActivationAccepted, off::EffectResult::Positive, 0, 1);
  OFF_CHECK(wrong_target.outcome == off::Reason::EffectMismatch);

  off::EffectReport report;
  report.id = off::EffectId::from_value(2);
  report.service = fixture.service;
  report.attempt = view.current_attempt;
  report.generation = view.generation;
  report.fence = view.fence;
  report.target.target = fixture.t2;
  report.target.incarnation.host = off::Incarnation::from_value(9);
  report.target.incarnation.device = off::Incarnation::from_value(1);
  report.kind = off::EffectKind::ActivationAccepted;
  report.result = off::EffectResult::Positive;
  report.provenance.source = fixture.effect_source;
  const off::FabricView fabric_view = fixture.fabric.view();
  report.provenance.epoch = fabric_view.epoch;
  report.provenance.boot = fabric_view.boot;
  const off::RecoveryDecision decision = fixture.fabric.report_effect(report);
  OFF_CHECK(decision.outcome == off::Reason::IncarnationMismatch);
}

OFF_TEST(effects, unauthorized_reporter_is_refused) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView view = fixture.service_view();
  off::EffectReport report;
  report.service = fixture.service;
  report.attempt = view.current_attempt;
  report.generation = view.generation;
  report.fence = view.fence;
  report.target = fixture.target_ref(fixture.t2);
  report.kind = off::EffectKind::ActivationAccepted;
  report.result = off::EffectResult::Positive;
  report.provenance.source = source_name("stranger");
  const off::FabricView fabric_view = fixture.fabric.view();
  report.provenance.epoch = fabric_view.epoch;
  report.provenance.boot = fabric_view.boot;
  const off::RecoveryDecision decision = fixture.fabric.report_effect(report);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::EffectReporterUnauthorized);
}

OFF_TEST(effects, duplicate_effect_identifiers_are_idempotent) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView view = fixture.service_view();
  const off::RecoveryDecision first =
      fixture.report_effect(view.current_attempt, view.generation, view.fence, fixture.t2,
                            off::EffectKind::ActivationAccepted, off::EffectResult::Positive, 0, 42);
  OFF_CHECK(first.accepted);
  const off::RecoveryDecision replay =
      fixture.report_effect(view.current_attempt, view.generation, view.fence, fixture.t2,
                            off::EffectKind::ActivationAccepted, off::EffectResult::Positive, 0, 42);
  OFF_CHECK(replay.accepted);
  OFF_CHECK(replay.duplicate);
  OFF_CHECK(replay.outcome == off::Reason::AcceptDuplicateIdempotent);
  OFF_CHECK(fixture.fabric.stats().effect_reports_replayed == 1);
}

OFF_TEST(effects, a_negative_effect_ends_the_attempt_without_claiming_continuity) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView view = fixture.service_view();
  const off::RecoveryDecision decision =
      fixture.report_effect(view.current_attempt, view.generation, view.fence, fixture.t2,
                            off::EffectKind::ActivationAccepted, off::EffectResult::Negative, 0, 1);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::EffectNegative);
  const off::ServiceView after = fixture.service_view();
  OFF_CHECK_EQ(after.lifecycle, off::LifecyclePhase::Failed);
  OFF_CHECK_EQ(after.continuity, off::ContinuityClass::Unknown);
  OFF_CHECK(!after.effect_verified);
}

OFF_TEST(effects, epoch_and_boot_mismatches_are_refused) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView view = fixture.service_view();
  off::EffectReport report;
  report.service = fixture.service;
  report.attempt = view.current_attempt;
  report.generation = view.generation;
  report.fence = view.fence;
  report.target = fixture.target_ref(fixture.t2);
  report.kind = off::EffectKind::ActivationAccepted;
  report.result = off::EffectResult::Positive;
  report.provenance.source = fixture.effect_source;
  report.provenance.epoch = off::CoordinatorEpoch::from_value(99);
  report.provenance.boot = fixture.fabric.view().boot;
  OFF_CHECK(fixture.fabric.report_effect(report).outcome == off::Reason::AuthoritySuperseded);

  report.provenance.epoch = fixture.fabric.view().epoch;
  report.provenance.boot = off::BootId::from_value(99);
  OFF_CHECK(fixture.fabric.report_effect(report).outcome == off::Reason::BootMismatch);
}

OFF_TEST(effects, intent_acknowledgement_is_fenced_against_the_current_attempt) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView view = fixture.service_view();

  OFF_CHECK(fixture.fabric
                .acknowledge_intent(fixture.ack(view.current_attempt, view.generation,
                                                off::FenceToken::from_value(view.fence.value() - 1),
                                                true))
                .outcome == off::Reason::FenceStale);
  OFF_CHECK(fixture.fabric
                .acknowledge_intent(fixture.ack(view.current_attempt,
                                                off::FailoverGeneration::from_value(
                                                    view.generation.value() + 1),
                                                view.fence, true))
                .outcome == off::Reason::SupersededGeneration);
  OFF_CHECK_REASON(fixture.fabric
                       .acknowledge_intent(
                           fixture.ack(view.current_attempt, view.generation, view.fence, true))
                       .outcome,
                   off::Reason::AcceptIntentEmitted);
  const off::ServiceView after = fixture.service_view();
  OFF_CHECK_EQ(after.recovery, off::RecoveryPhase::ActivationRequested);
  OFF_CHECK_EQ(after.last_outcome, off::AttemptOutcome::Acknowledged);
  OFF_CHECK_REASON(fixture.fabric
                       .acknowledge_intent(
                           fixture.ack(view.current_attempt, view.generation, view.fence, true))
                       .outcome,
                   off::Reason::AcceptDuplicateIdempotent);
}

OFF_TEST(effects, rejected_intent_marks_the_attempt_refused) {
  Fixture fixture;
  fixture.build();
  OFF_REQUIRE(recover(fixture).accepted);
  const off::ServiceView view = fixture.service_view();
  const off::RecoveryDecision decision = fixture.fabric.acknowledge_intent(
      fixture.ack(view.current_attempt, view.generation, view.fence, false));
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::TransitionRefused);
  const off::ServiceView after = fixture.service_view();
  OFF_CHECK_EQ(after.lifecycle, off::LifecyclePhase::Failed);
  OFF_CHECK_EQ(after.last_outcome, off::AttemptOutcome::Refused);
}

}  // namespace
