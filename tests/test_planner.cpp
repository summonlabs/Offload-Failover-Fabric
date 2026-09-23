// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <string>
#include <vector>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using namespace offtest;

bool has_step(const off::RecoveryPlan& plan, off::PlanStepKind kind) {
  for (const off::PlanStep& step : plan.steps) {
    if (step.kind == kind) {
      return true;
    }
  }
  return false;
}

std::size_t step_index(const off::RecoveryPlan& plan, off::PlanStepKind kind) {
  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    if (plan.steps[index].kind == kind) {
      return index;
    }
  }
  return plan.steps.size();
}

OFF_TEST(planner, stateless_recovery_plan_is_ordered_and_complete) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_REQUIRE(decision.accepted);
  const off::RecoveryPlan& plan = decision.plan;
  OFF_CHECK(plan.complete());
  OFF_CHECK_EQ(plan.omitted_steps, 0ULL);
  OFF_CHECK(has_step(plan, off::PlanStepKind::AdvanceFence));
  OFF_CHECK(has_step(plan, off::PlanStepKind::FencePreviousTarget));
  OFF_CHECK(has_step(plan, off::PlanStepKind::TransferLease));
  OFF_CHECK(has_step(plan, off::PlanStepKind::EmitIntent));
  OFF_CHECK(has_step(plan, off::PlanStepKind::AwaitActivationEffect));
  OFF_CHECK(has_step(plan, off::PlanStepKind::AwaitServiceHealth));
  OFF_CHECK(has_step(plan, off::PlanStepKind::ClaimContinuity));
  OFF_CHECK(!has_step(plan, off::PlanStepKind::AwaitStateTransfer));
  OFF_CHECK(step_index(plan, off::PlanStepKind::AdvanceFence) <
            step_index(plan, off::PlanStepKind::FencePreviousTarget));
  OFF_CHECK(step_index(plan, off::PlanStepKind::FencePreviousTarget) <
            step_index(plan, off::PlanStepKind::EmitIntent));
  OFF_CHECK(step_index(plan, off::PlanStepKind::EmitIntent) <
            step_index(plan, off::PlanStepKind::ClaimContinuity));
  OFF_CHECK(plan.from.target == fixture.t1);
  OFF_CHECK(plan.to.target == fixture.t2);
}

OFF_TEST(planner, stateful_recovery_plan_requires_a_state_transfer_step) {
  Fixture fixture;
  fixture.statefulness = off::Statefulness::Stateful;
  fixture.preconditions.insert(off::Precondition::StateTransfer);
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_REQUIRE(decision.accepted);
  OFF_CHECK(has_step(decision.plan, off::PlanStepKind::AwaitStateTransfer));
}

OFF_TEST(planner, plan_ceiling_refuses_instead_of_silently_shortening) {
  Fixture fixture;
  fixture.statefulness = off::Statefulness::Stateful;
  fixture.preconditions.insert(off::Precondition::StateTransfer);
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  off::PolicyDescriptor policy;
  policy.generation = off::PolicyGeneration::from_value(1);
  policy.scope = fixture.scope;
  policy.max_plan_steps = 4;
  OFF_CHECK(off::reason_is_accept(fixture.fabric.set_policy(policy, nullptr)));
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::PlanStepLimitReached);
  OFF_CHECK_EQ(decision.plan.steps.size(), 4U);
  OFF_CHECK_EQ(decision.plan.omitted_steps, 4ULL);
  const off::ServiceView view = fixture.service_view();
  OFF_REQUIRE(view.active.has_value());
  OFF_CHECK(view.active->target == fixture.t1);
}

OFF_TEST(planner, candidate_truncation_is_observable_and_order_independent) {
  Fixture fixture;
  fixture.build();
  for (int index = 0; index < 12; ++index) {
    const std::string name = "x" + std::to_string(index);
    const off::TargetName extra = target_name(name.c_str());
    fixture.add_target(extra, name.c_str(), "nic", off::DeviceKind::SmartNic, 1, 1);
    fixture.add_capability(extra, 1, capabilities({off::CapabilityCode::IPv4Forward}), 1, 1);
  }
  off::PolicyDescriptor policy;
  policy.generation = off::PolicyGeneration::from_value(1);
  policy.scope = fixture.scope;
  policy.max_fallback_candidates = 3;
  OFF_CHECK(off::reason_is_accept(fixture.fabric.set_policy(policy, nullptr)));
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_CHECK(decision.accepted);
  OFF_CHECK(fixture.fabric.stats().fallback_candidates_truncated > 0);
  // The bound is applied after canonical ordering, so the same candidate wins.
  Fixture other;
  other.build();
  for (int index = 0; index < 12; ++index) {
    const std::string name = "x" + std::to_string(index);
    const off::TargetName extra = target_name(name.c_str());
    other.add_target(extra, name.c_str(), "nic", off::DeviceKind::SmartNic, 1, 1);
    other.add_capability(extra, 1, capabilities({off::CapabilityCode::IPv4Forward}), 1, 1);
  }
  OFF_CHECK(off::reason_is_accept(other.fabric.set_policy(policy, nullptr)));
  OFF_CHECK(other.place(other.t1).accepted);
  const off::EvidenceId other_evidence =
      other.report_failure(other.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision other_decision = other.failover(other_evidence);
  OFF_CHECK(other_decision.accepted);
  OFF_CHECK(other_decision.plan.to.target == decision.plan.to.target);
}

OFF_TEST(planner, candidates_prefer_a_different_host_then_a_higher_device_rank) {
  Fixture fixture;
  fixture.build();
  // t4 is a DPU on the same host as the failed target. It outranks t2 on the
  // device axis but must still lose, because the host axis is evaluated first.
  const off::TargetName t4 = target_name("t4");
  fixture.add_target(t4, "h1", "dpu2", off::DeviceKind::Dpu, 1, 1);
  fixture.add_capability(t4, 1, capabilities({off::CapabilityCode::IPv4Forward}), 1, 1);
  OFF_CHECK(off::reason_is_accept(
      fixture.fabric.retire_target(fixture.t3, fixture.topology_source, nullptr)));
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  const off::RecoveryDecision decision = fixture.failover(evidence);
  OFF_REQUIRE(decision.accepted);
  OFF_CHECK(decision.plan.to.target == fixture.t2);
}

OFF_TEST(planner, plan_step_kind_text_round_trips) {
  for (std::size_t index = 0; index < off::kPlanStepKindCount; ++index) {
    const auto kind = static_cast<off::PlanStepKind>(index);
    off::PlanStepKind parsed{};
    OFF_CHECK(off::plan_step_kind_parse(off::plan_step_kind_text(kind), parsed));
    OFF_CHECK(parsed == kind);
  }
}

}  // namespace
