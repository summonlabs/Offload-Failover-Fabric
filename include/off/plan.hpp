// Offload Failover Fabric - bounded, deterministic recovery plans.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "off/explain.hpp"
#include "off/model.hpp"

namespace off {

/// Ordered recovery action kinds. The fabric never performs these actions
/// itself; it emits them as governed intent and records the resulting effect
/// reports.
enum class PlanStepKind : u8 {
  AdvanceFence = 0,
  FencePreviousTarget = 1,
  TransferLease = 2,
  EmitIntent = 3,
  AwaitActivationEffect = 4,
  AwaitStateTransfer = 5,
  AwaitServiceHealth = 6,
  ClaimContinuity = 7,
};

inline constexpr std::size_t kPlanStepKindCount = 8;
[[nodiscard]] std::string_view plan_step_kind_text(PlanStepKind value) noexcept;
[[nodiscard]] bool plan_step_kind_parse(std::string_view text, PlanStepKind& out) noexcept;

struct PlanStep {
  PlanStepKind kind{PlanStepKind::AdvanceFence};
  std::string subject{};
  bool mandatory{true};

  friend bool operator==(const PlanStep&, const PlanStep&) noexcept = default;
};

/// A bounded recovery plan. When required steps exceed the policy ceiling the
/// plan is refused rather than silently shortened, and the refusal carries the
/// number of steps that did not fit.
struct RecoveryPlan {
  PlanId id{};
  ServiceName service{};
  FailoverGeneration generation{};
  FenceToken fence{};
  LeaseTerm lease_term{};
  TargetRef from{};
  TargetRef to{};
  std::vector<PlanStep> steps{};
  std::size_t omitted_steps{0};

  [[nodiscard]] bool complete() const noexcept { return omitted_steps == 0; }
  [[nodiscard]] JsonValue to_json() const;
};

/// The result of a recovery request. A refusal still carries a full
/// explanation and, when a plan was built before the refusal, the plan.
struct RecoveryDecision {
  Reason outcome{Reason::Ok};
  bool accepted{false};
  bool duplicate{false};
  RecoveryPlan plan{};
  std::optional<FailoverIntent> intent{};
  Explanation explanation{};

  [[nodiscard]] JsonValue to_json() const;
};

[[nodiscard]] JsonValue to_json(const PlanStep& value);
[[nodiscard]] JsonValue to_json(const PlanStepKind& value);

}  // namespace off
