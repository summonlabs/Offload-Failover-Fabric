// Offload Failover Fabric - shared test fixtures.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "off/off.hpp"
#include "testing.hpp"

namespace offtest {

inline std::filesystem::path unique_directory(const std::string& name) {
  static std::atomic<unsigned> counter{0};
  const unsigned index = counter.fetch_add(1);
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "offab-tests" /
                                     (name + "-" + std::to_string(index));
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path;
}

inline off::ServiceName service_name(const char* text) { return off::ServiceName::literal(text); }
inline off::ScopeName scope_name(const char* text) { return off::ScopeName::literal(text); }
inline off::HostName host_name(const char* text) { return off::HostName::literal(text); }
inline off::DeviceName device_name(const char* text) { return off::DeviceName::literal(text); }
inline off::TargetName target_name(const char* text) { return off::TargetName::literal(text); }
inline off::SourceName source_name(const char* text) { return off::SourceName::literal(text); }

inline off::CapabilitySet capabilities(std::initializer_list<off::CapabilityCode> codes) {
  off::CapabilitySet out;
  for (const off::CapabilityCode code : codes) {
    out.insert(code);
  }
  return out;
}

/// A fully wired scenario: six enrolled sources, three targets in one governed
/// scope with current capability evidence, and one registered service.
struct Fixture {
  off::FabricConfig config{};
  off::Fabric fabric{config};

  off::ScopeName scope{scope_name("edge")};
  off::ServiceName service{service_name("svc1")};
  off::TargetName t1{target_name("t1")};
  off::TargetName t2{target_name("t2")};
  off::TargetName t3{target_name("t3")};

  off::SourceName operator_source{source_name("op")};
  off::SourceName topology_source{source_name("topo")};
  off::SourceName failure_source{source_name("failrep")};
  off::SourceName failure_source2{source_name("failrep2")};
  off::SourceName failure_source3{source_name("failrep3")};
  off::SourceName capability_source{source_name("caprep")};
  off::SourceName dependency_source{source_name("deprep")};
  off::SourceName effect_source{source_name("exec")};

  off::Statefulness statefulness{off::Statefulness::Stateless};
  off::CapabilitySet required_capabilities{capabilities({off::CapabilityCode::IPv4Forward})};
  off::StateGeneration service_state_generation{};
  std::vector<off::DependencySpec> dependencies{};
  off::PreconditionSet preconditions{};

  explicit Fixture(off::FabricConfig configuration = {}) : config(std::move(configuration)) {}
  ~Fixture() { (void)fabric.close(); }

  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  void enroll(const off::SourceName& source, off::SourceRole role) {
    OFF_CHECK(off::reason_is_accept(fabric.enroll_source(source, role, nullptr)));
  }

  void add_target(const off::TargetName& name, const char* host, const char* device,
                  off::DeviceKind kind, std::uint64_t host_incarnation,
                  std::uint64_t device_incarnation) {
    off::TargetRecord record;
    record.name = name;
    record.host = host_name(host);
    record.device = device_name(device);
    record.kind = kind;
    record.scope = scope;
    record.incarnation.host = off::Incarnation::from_value(host_incarnation);
    record.incarnation.device = off::Incarnation::from_value(device_incarnation);
    OFF_CHECK(off::reason_is_accept(fabric.register_target(record, topology_source, nullptr)));
  }

  void add_capability(const off::TargetName& name, std::uint64_t generation,
                      const off::CapabilitySet& set, std::uint64_t host_incarnation,
                      std::uint64_t device_incarnation) {
    off::CapabilityEvidence evidence;
    evidence.target = name;
    evidence.generation = off::CapabilityGeneration::from_value(generation);
    evidence.capabilities = set;
    evidence.topology_generation = fabric.view().topology_generation;
    evidence.incarnation.host = off::Incarnation::from_value(host_incarnation);
    evidence.incarnation.device = off::Incarnation::from_value(device_incarnation);
    evidence.source = capability_source;
    OFF_CHECK(off::reason_is_accept(fabric.ingest_capability(evidence, nullptr)));
  }

  /// Dependency services registered before the primary service so a hard
  /// dependency can actually be resolved at placement time.
  std::vector<off::ServiceName> extra_services{};

  void observe_dependency(const char* dependency, off::DependencyHealth health,
                          std::uint64_t sequence) {
    off::DependencyObservation observation;
    observation.service = service;
    observation.dependency = service_name(dependency);
    observation.health = health;
    observation.provenance.source = dependency_source;
    observation.provenance.sequence = off::EvidenceSeq::from_value(sequence);
    const off::FabricView view = fabric.view();
    observation.provenance.epoch = view.epoch;
    observation.provenance.boot = view.boot;
    observation.topology_generation = view.topology_generation;
    OFF_CHECK(off::reason_is_accept(fabric.ingest_dependency(observation, nullptr)));
  }

  /// Capability evidence for one of the deterministic ingest streams, keyed so
  /// that every delivery order produces the same accepted set.
  [[nodiscard]] off::Reason add_capability_evidence(std::size_t stream, std::uint64_t sequence) {
    const off::TargetName targets[3] = {t1, t2, t3};
    off::CapabilityEvidence evidence;
    evidence.target = targets[stream % 3];
    evidence.generation = off::CapabilityGeneration::from_value(sequence + 1);
    evidence.capabilities = capabilities({off::CapabilityCode::IPv4Forward});
    evidence.topology_generation = fabric.view().topology_generation;
    evidence.incarnation = incarnation_of(evidence.target);
    evidence.source = capability_source;
    return fabric.ingest_capability(evidence, nullptr);
  }

  void register_service() {
    off::ServiceDescriptor descriptor;
    descriptor.name = service;
    descriptor.scope = scope;
    descriptor.statefulness = statefulness;
    descriptor.required_capabilities = required_capabilities;
    descriptor.preconditions = preconditions;
    descriptor.dependencies = dependencies;
    descriptor.last_known_state_generation = service_state_generation;
    OFF_CHECK(off::reason_is_accept(fabric.register_service(descriptor, topology_source, nullptr)));
  }

  void build() {
    OFF_CHECK_REASON(fabric.open(), off::Reason::Ok);
    enroll(operator_source, off::SourceRole::Operator);
    enroll(topology_source, off::SourceRole::TopologyReporter);
    enroll(failure_source, off::SourceRole::FailureReporter);
    enroll(failure_source2, off::SourceRole::FailureReporter);
    enroll(failure_source3, off::SourceRole::FailureReporter);
    enroll(capability_source, off::SourceRole::CapabilityReporter);
    enroll(dependency_source, off::SourceRole::DependencyReporter);
    enroll(effect_source, off::SourceRole::EffectReporter);
    add_target(t1, "h1", "nic1", off::DeviceKind::SmartNic, 1, 1);
    add_target(t2, "h2", "dpu1", off::DeviceKind::Dpu, 1, 1);
    add_target(t3, "h3", "cpu1", off::DeviceKind::HostCpu, 1, 1);
    OFF_CHECK(off::reason_is_accept(fabric.set_topology_generation(
        off::TopologyGeneration::from_value(1), topology_source, nullptr)));
    add_capability(t1, 1,
                   capabilities({off::CapabilityCode::IPv4Forward,
                                 off::CapabilityCode::L4LoadBalance}),
                   1, 1);
    add_capability(t2, 1,
                   capabilities({off::CapabilityCode::IPv4Forward,
                                 off::CapabilityCode::L4LoadBalance,
                                 off::CapabilityCode::DpuProgrammable}),
                   1, 1);
    add_capability(t3, 1, capabilities({off::CapabilityCode::IPv4Forward}), 1, 1);
    for (const off::ServiceName& extra : extra_services) {
      off::ServiceDescriptor descriptor;
      descriptor.name = extra;
      descriptor.scope = scope;
      descriptor.statefulness = off::Statefulness::Stateless;
      OFF_CHECK(off::reason_is_accept(
          fabric.register_service(descriptor, topology_source, nullptr)));
    }
    register_service();
  }

  [[nodiscard]] off::TargetIncarnation incarnation_of(const off::TargetName& name) const {
    for (const off::TargetRecord& record : fabric.view().targets) {
      if (record.name == name) {
        return record.incarnation;
      }
    }
    return off::TargetIncarnation{};
  }

  [[nodiscard]] off::TargetRef target_ref(const off::TargetName& name) const {
    off::TargetRef ref;
    ref.target = name;
    ref.incarnation = incarnation_of(name);
    return ref;
  }

  [[nodiscard]] off::RecoveryDecision place(const off::TargetName& name, std::uint64_t key = 0) {
    off::PlacementRequest request;
    request.key = off::RequestKey::from_value(key);
    request.service = service;
    request.target = name;
    request.requester = operator_source;
    return fabric.establish_placement(request);
  }

  [[nodiscard]] off::EvidenceId report_failure(
      const off::TargetName& name, off::FailureClass failure_class, std::uint64_t sequence,
      off::AmbiguityState ambiguity = off::AmbiguityState::Unambiguous,
      const off::TargetIncarnation& incarnation = {},
      const off::SourceName& source = off::SourceName{}) {
    off::EvidenceId assigned{};
    const off::Reason code =
        try_report_failure(name, failure_class, sequence, ambiguity, incarnation, source, &assigned);
    OFF_CHECK(off::reason_is_accept(code));
    return assigned;
  }

  /// Delivery without an acceptance expectation, for randomised schedules where
  /// a refusal is a legitimate outcome.
  [[nodiscard]] off::Reason try_report_failure(
      const off::TargetName& name, off::FailureClass failure_class, std::uint64_t sequence,
      off::AmbiguityState ambiguity = off::AmbiguityState::Unambiguous,
      const off::TargetIncarnation& incarnation = {},
      const off::SourceName& source = off::SourceName{},
      off::EvidenceId* assigned = nullptr) {
    off::FailureEvidence evidence;
    evidence.service = service;
    evidence.target.target = name;
    evidence.target.incarnation =
        incarnation.host.is_initial() ? incarnation_of(name) : incarnation;
    evidence.failure_class = failure_class;
    evidence.ambiguity = ambiguity;
    evidence.provenance.source = source.empty() ? failure_source : source;
    evidence.provenance.sequence = off::EvidenceSeq::from_value(sequence);
    const off::FabricView view = fabric.view();
    evidence.provenance.epoch = view.epoch;
    evidence.provenance.boot = view.boot;
    evidence.topology_generation = view.topology_generation;
    off::Explanation explanation;
    const off::Reason code = fabric.ingest_failure(evidence, &explanation);
    if (assigned != nullptr && !explanation.steps.empty()) {
      *assigned = off::EvidenceId::from_value(explanation.steps.back().value);
    }
    return code;
  }

  [[nodiscard]] off::RecoveryDecision failover(const off::EvidenceId& evidence,
                                               std::uint64_t key = 0,
                                               bool operator_authorized = false) {
    off::FailoverRequest request;
    request.key = off::RequestKey::from_value(key);
    request.service = service;
    request.trigger = off::RecoveryTrigger::FailureEvidence;
    request.evidence = evidence;
    request.requester = operator_source;
    request.operator_authorized = operator_authorized;
    return fabric.request_failover(request);
  }

  [[nodiscard]] off::RecoveryDecision report_effect(const off::AttemptId& attempt,
                                                    off::FailoverGeneration generation,
                                                    off::FenceToken fence,
                                                    const off::TargetName& target,
                                                    off::EffectKind kind,
                                                    off::EffectResult result,
                                                    std::uint64_t state_generation = 0,
                                                    std::uint64_t effect_id = 0) {
    off::EffectReport report;
    report.id = off::EffectId::from_value(effect_id);
    report.service = service;
    report.attempt = attempt;
    report.generation = generation;
    report.fence = fence;
    report.target = target_ref(target);
    report.kind = kind;
    report.result = result;
    report.state_generation = off::StateGeneration::from_value(state_generation);
    report.provenance.source = effect_source;
    const off::FabricView view = fabric.view();
    report.provenance.epoch = view.epoch;
    report.provenance.boot = view.boot;
    return fabric.report_effect(report);
  }

  [[nodiscard]] off::ServiceView service_view() const {
    for (const off::ServiceView& view : fabric.view().services) {
      if (view.name == service) {
        return view;
      }
    }
    return off::ServiceView{};
  }

  void verify_continuity(off::FailoverGeneration generation, off::FenceToken fence,
                         const off::TargetName& target, std::uint64_t effect_base = 1000) {
    OFF_CHECK_REASON(fabric.acknowledge_intent(ack(attempt_id(), generation, fence, true)).outcome,
                     off::Reason::AcceptIntentEmitted);
    OFF_CHECK_REASON(report_effect(attempt_id(), generation, fence, target,
                                   off::EffectKind::ActivationAccepted, off::EffectResult::Positive,
                                   0, effect_base)
                         .outcome,
                     off::Reason::AcceptEffectVerified);
    OFF_CHECK_REASON(report_effect(attempt_id(), generation, fence, target,
                                   off::EffectKind::ServiceHealthy, off::EffectResult::Positive, 0,
                                   effect_base + 1)
                         .outcome,
                     off::Reason::AcceptContinuityVerified);
  }

  [[nodiscard]] off::AttemptId attempt_id() const { return service_view().current_attempt; }

  [[nodiscard]] off::IntentAck ack(const off::AttemptId& attempt, off::FailoverGeneration generation,
                                   off::FenceToken fence, bool accepted) const {
    off::IntentAck value;
    value.attempt = attempt;
    value.generation = generation;
    value.fence = fence;
    value.accepted = accepted;
    value.executor = effect_source;
    return value;
  }
};

}  // namespace offtest
