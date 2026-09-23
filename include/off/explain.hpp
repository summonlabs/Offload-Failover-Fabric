// Offload Failover Fabric - deterministic explanation surface. Every decision
// the runtime makes is reconstructible from an ordered list of reason codes and
// the evidence/generation/policy that made it legal.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <string>
#include <vector>

#include "off/digest.hpp"
#include "off/ids.hpp"
#include "off/reason.hpp"

namespace off {

class JsonValue;

/// One ordered step of a decision. Steps are appended in evaluation order, so
/// the same state always produces the same sequence.
struct ExplanationStep {
  Reason code{Reason::Ok};
  std::string subject{};
  std::string detail{};
  u64 value{0};

  friend bool operator==(const ExplanationStep&, const ExplanationStep&) noexcept = default;
};

/// A complete, canonical account of one decision.
struct Explanation {
  ServiceName service{};
  LogicalTick tick{};
  Reason outcome{Reason::Ok};
  bool accepted{false};
  CoordinatorEpoch epoch{};
  PolicyGeneration policy_generation{};
  TopologyGeneration topology_generation{};
  FailoverGeneration failover_generation{};
  FenceToken fence{};
  RequestKey request{};
  std::vector<ExplanationStep> steps{};

  [[nodiscard]] bool accepted_decision() const noexcept { return accepted; }
  /// Digest over the ordered steps and the generations that produced them.
  [[nodiscard]] Digest semantic_digest() const;
  /// Stable multi-line human rendering used by the inspection CLI.
  [[nodiscard]] std::string render() const;
  [[nodiscard]] JsonValue to_json() const;
};

/// Bounded, append-only explanation log. Truncation is explicit and counted so
/// a caller can tell "no explanation" from "explanation evicted".
class ExplanationLog {
 public:
  explicit ExplanationLog(std::size_t capacity) : capacity_(capacity) {}

  void record(Explanation explanation);
  [[nodiscard]] std::vector<Explanation> for_service(const ServiceName& service) const;
  [[nodiscard]] std::vector<Explanation> all() const;
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t evicted() const noexcept { return evicted_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  void clear() noexcept;

 private:
  std::size_t capacity_{0};
  std::size_t evicted_{0};
  std::size_t next_{0};
  std::vector<Explanation> entries_{};
};

}  // namespace off
