// Offload Failover Fabric - the public runtime interface.
//
// The fabric owns failover eligibility, recovery intent, authority, fencing,
// fallback selection and verified recovery state for offloaded network
// services. It does not diagnose hardware, implement drivers, forward packets,
// route traffic or execute the offloaded service. It consumes authoritative
// failure, capability and dependency evidence and emits governed failover
// intent.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "off/explain.hpp"
#include "off/model.hpp"
#include "off/plan.hpp"
#include "off/store.hpp"
#include "off/version.hpp"

namespace off {

/// Role of an enrolled reporting source. Roles gate which evidence a source
/// may contribute; an unenrolled source contributes nothing.
enum class SourceRole : u8 {
  FailureReporter = 0,
  CapabilityReporter = 1,
  DependencyReporter = 2,
  EffectReporter = 3,
  Operator = 4,
  TopologyReporter = 5,
};

inline constexpr std::size_t kSourceRoleCount = 6;
[[nodiscard]] std::string_view source_role_text(SourceRole value) noexcept;
[[nodiscard]] bool source_role_parse(std::string_view text, SourceRole& out) noexcept;

struct FabricConfig {
  /// When set, all correctness-critical state is persisted here.
  std::optional<std::filesystem::path> store_directory{};
  std::size_t max_services{1024};
  std::size_t max_targets{4096};
  std::size_t max_evidence_records{4096};
  std::size_t max_dependencies_per_service{64};
  std::size_t max_attempt_history{64};
  std::size_t max_explanations{2048};
  std::size_t max_request_cache{2048};
  std::size_t max_intent_queue{1024};
  std::size_t max_plan_steps{32};
  std::size_t max_sources{256};
  /// Ceiling on the candidate set considered during fallback selection. The set
  /// is ordered first, so the bound never changes which candidate wins.
  std::size_t max_fallback_candidates{256};
  u64 max_journal_bytes{8ULL << 20U};
  /// When true, effect reports are accepted only from enrolled EffectReporter
  /// sources. It is never disabled implicitly.
  bool require_enrolled_effect_reporter{true};
  PolicyDescriptor initial_policy{};
};

struct ServiceView {
  ServiceName name{};
  ScopeName scope{};
  Statefulness statefulness{Statefulness::Stateless};
  LifecyclePhase lifecycle{LifecyclePhase::Unregistered};
  RecoveryPhase recovery{RecoveryPhase::None};
  ContinuityClass continuity{ContinuityClass::Unknown};
  std::optional<TargetRef> active{};
  std::optional<TargetRef> fallback{};
  FailoverGeneration generation{};
  FenceToken fence{};
  LeaseTerm lease_term{};
  AttemptId current_attempt{};
  AttemptOutcome last_outcome{AttemptOutcome::Pending};
  bool effect_verified{false};
  bool state_transfer_verified{false};
  bool ambiguity_pending{false};
  bool failover_recorded{false};
  Reason last_reason{Reason::Ok};
  std::size_t attempts_recorded{0};
  std::size_t attempts_evicted{0};
  LogicalTick last_change{};

  [[nodiscard]] JsonValue to_json() const;
};

struct FabricStats {
  u64 services_registered{0};
  u64 targets_registered{0};
  u64 sources_enrolled{0};
  u64 capabilities_ingested{0};
  u64 failures_ingested{0};
  u64 dependencies_ingested{0};
  u64 evidence_rejected_stale{0};
  u64 evidence_rejected_conflict{0};
  u64 evidence_rejected_duplicate{0};
  u64 evidence_rejected_unauthorized{0};
  u64 evidence_rejected_capacity{0};
  u64 evidence_evicted{0};
  u64 placements_established{0};
  u64 failovers_accepted{0};
  u64 failovers_refused{0};
  u64 failovers_duplicate{0};
  u64 failbacks_accepted{0};
  u64 failbacks_refused{0};
  u64 effects_accepted{0};
  u64 effects_refused{0};
  u64 effects_verified{0};
  u64 ambiguity_pending{0};
  u64 ambiguity_resolved{0};
  u64 intents_emitted{0};
  u64 intents_dropped{0};
  u64 intents_consumed{0};
  u64 attempts_recorded{0};
  u64 attempts_evicted{0};
  u64 explanations_recorded{0};
  u64 explanations_evicted{0};
  u64 fallback_candidates_truncated{0};
  u64 dependency_depth_truncated{0};
  u64 requests_cancelled{0};
  u64 cancels_too_late{0};
  u64 journal_transactions{0};
  u64 journal_compactions{0};
  u64 recovery_reissues{0};
  u64 effect_reports_replayed{0};
  u64 withdrawn_services{0};

  [[nodiscard]] JsonValue to_json() const;
};

struct FabricView {
  Version runtime_version{};
  u16 schema_version{kCanonicalSchemaVersion};
  CoordinatorEpoch epoch{};
  BootId boot{};
  LogicalTick tick{};
  TopologyGeneration topology_generation{};
  PolicyGeneration policy_generation{};
  PolicyDescriptor policy{};
  std::vector<ServiceView> services{};
  std::vector<TargetRecord> targets{};
  FabricStats stats{};

  /// Digest of the decision-relevant projection only. Two fabrics that
  /// accepted the same evidence always produce the same value regardless of
  /// delivery order, concurrency or logical tick stamps.
  [[nodiscard]] Digest semantic_digest() const;
  [[nodiscard]] JsonValue semantic_json() const;
  [[nodiscard]] JsonValue to_json() const;
  [[nodiscard]] std::string canonical_text(bool pretty) const;
};

struct RestartSummary {
  RecoveryReport store{};
  CoordinatorEpoch previous_epoch{};
  CoordinatorEpoch epoch{};
  BootId boot{};
  u32 services_downgraded{0};
  u32 attempts_marked_unknown{0};
  u32 capability_evidence_invalidated{0};
  u32 dependency_observations_invalidated{0};
  u32 failure_evidence_invalidated{0};
  bool conservative{true};

  [[nodiscard]] JsonValue to_json() const;
};

/// A claim by a target that it holds the placement for a service.
struct FenceCheck {
  ServiceName service{};
  TargetRef holder{};
  FenceToken presented{};
};

/// The runtime verdict on a fence claim. Authorized is true only when the
/// presented token is at least the current durable fence and the holder matches
/// the recorded holder; a target that was fenced out can never be authorized
/// again until its incarnation advances.
struct FenceVerdict {
  bool authorized{false};
  Reason code{Reason::StaleGeneration};
  FenceToken current{};
  TargetRef current_holder{};
  TargetIncarnation fenced_incarnation{};
  bool had_witness{false};

  [[nodiscard]] JsonValue to_json() const;
};

class Fabric {
 public:
  explicit Fabric(FabricConfig config = {});
  ~Fabric();
  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;

  /// Opens the runtime. When a store directory is configured the persisted
  /// state is validated and replayed first; a store that cannot be trusted
  /// refuses to open instead of starting from a guess.
  [[nodiscard]] Reason open();
  [[nodiscard]] Reason close();
  [[nodiscard]] bool is_open() const noexcept;

  [[nodiscard]] const RestartSummary& restart_summary() const noexcept;

  // ---- enrolment and topology -------------------------------------------
  [[nodiscard]] Reason enroll_source(SourceName source, SourceRole role, Explanation* explanation = nullptr);
  [[nodiscard]] Reason register_service(const ServiceDescriptor& descriptor, SourceName registrar,
                                        Explanation* explanation = nullptr);
  [[nodiscard]] Reason register_target(const TargetRecord& target, SourceName registrar,
                                       Explanation* explanation = nullptr);
  [[nodiscard]] Reason set_topology_generation(TopologyGeneration generation, SourceName reporter,
                                               Explanation* explanation = nullptr);
  [[nodiscard]] Reason set_policy(const PolicyDescriptor& policy, Explanation* explanation = nullptr);
  [[nodiscard]] Reason withdraw_service(ServiceName service, SourceName requester,
                                        Explanation* explanation = nullptr);
  /// Removes a target from the governed topology. A service still placed on it
  /// is left exactly as it was: disappearance is observable, never permission.
  [[nodiscard]] Reason retire_target(TargetName target, SourceName requester,
                                     Explanation* explanation = nullptr);

  // ---- evidence ingestion ------------------------------------------------
  [[nodiscard]] Reason ingest_capability(const CapabilityEvidence& evidence,
                                         Explanation* explanation = nullptr);
  [[nodiscard]] Reason ingest_failure(FailureEvidence evidence, Explanation* explanation = nullptr);
  [[nodiscard]] Reason ingest_dependency(const DependencyObservation& observation,
                                         Explanation* explanation = nullptr);

  // ---- recovery decisions ------------------------------------------------
  [[nodiscard]] RecoveryDecision establish_placement(const PlacementRequest& request);
  [[nodiscard]] RecoveryDecision request_failover(const FailoverRequest& request);
  [[nodiscard]] RecoveryDecision resolve_ambiguity(const AmbiguityResolution& resolution);
  [[nodiscard]] RecoveryDecision resume_recovery(const FailoverRequest& request);
  [[nodiscard]] RecoveryDecision request_failback(const FailbackRequest& request);
  [[nodiscard]] RecoveryDecision acknowledge_intent(const IntentAck& ack);
  [[nodiscard]] RecoveryDecision report_effect(const EffectReport& report);

  // ---- cancellation ------------------------------------------------------
  /// Requests cancellation of an in-flight request. Returns Ok when the
  /// cancellation was recorded before the request committed, CancelTooLate
  /// when the durable commit already happened, and RequestUnknown when no such
  /// request is in flight.
  [[nodiscard]] Reason cancel(RequestKey key, Explanation* explanation = nullptr);

  // ---- observation -------------------------------------------------------
  [[nodiscard]] FabricView view() const;
  [[nodiscard]] std::string export_canonical(bool pretty) const;
  [[nodiscard]] std::vector<Explanation> explain(ServiceName service) const;
  [[nodiscard]] std::vector<Explanation> explain_all() const;
  [[nodiscard]] std::vector<FailoverIntent> pending_intents() const;
  /// Non-blocking removal of the oldest emitted intent. Returns false when the
  /// queue is empty; it never waits and never fabricates an intent.
  [[nodiscard]] bool take_intent(FailoverIntent& out);
  /// Blocks until an intent is available or the runtime is closed. Returns
  /// false only once the runtime is closed and the queue is drained.
  [[nodiscard]] bool wait_for_intent(FailoverIntent& out);
  [[nodiscard]] FabricStats stats() const;
  [[nodiscard]] std::size_t intent_queue_depth() const;
  /// Verdict on a placement-holder claim. This is the surface that proves an
  /// old target cannot resume behind a higher durable fence.
  [[nodiscard]] FenceVerdict verify_fence(const FenceCheck& check) const;
  /// Encodes the complete persisted state canonically. Public so the
  /// persistence round-trip can be validated from an independent process.
  [[nodiscard]] std::string snapshot_digest_text() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Human-readable rendering of a recovery class together with its reason code.
[[nodiscard]] std::string recovery_report_text(const RecoveryReport& report);

}  // namespace off
