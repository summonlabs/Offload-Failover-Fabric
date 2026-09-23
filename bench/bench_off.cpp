// off_bench - throughput of completed work.
//
// Every figure reported here counts operations that returned an accepted
// decision AND whose effect is observable in the exported state afterwards.
// Enqueue-only or partially-completed work is never counted.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "off/off.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Report {
  const char* name;
  std::uint64_t completed;
  double seconds;
};

std::vector<Report>& reports() {
  static std::vector<Report> all;
  return all;
}

void record(const char* name, std::uint64_t completed, Clock::time_point start,
            Clock::time_point finish) {
  const double seconds = std::chrono::duration<double>(finish - start).count();
  reports().push_back(Report{name, completed, seconds});
}

off::FabricConfig bench_config(const std::filesystem::path& store) {
  off::FabricConfig config;
  config.store_directory = store;
  config.max_journal_bytes = 1ULL << 20U;
  return config;
}

bool build_scenario(off::Fabric& fabric, int targets) {
  const auto enroll = [&fabric](const char* name, off::SourceRole role) {
    return off::reason_is_accept(
        fabric.enroll_source(off::SourceName::literal(name), role, nullptr));
  };
  if (!enroll("op", off::SourceRole::Operator) || !enroll("topo", off::SourceRole::TopologyReporter) ||
      !enroll("cap", off::SourceRole::CapabilityReporter) ||
      !enroll("failrep", off::SourceRole::FailureReporter) ||
      !enroll("exec", off::SourceRole::EffectReporter)) {
    return false;
  }
  for (int index = 0; index < targets; ++index) {
    off::TargetRecord record;
    record.name = off::TargetName::literal("t" + std::to_string(index));
    record.host = off::HostName::literal("h" + std::to_string(index));
    record.device = off::DeviceName::literal("d" + std::to_string(index));
    record.kind = off::DeviceKind::SmartNic;
    record.scope = off::ScopeName::literal("edge");
    record.incarnation.host = off::Incarnation::from_value(1);
    record.incarnation.device = off::Incarnation::from_value(1);
    if (!off::reason_is_accept(
            fabric.register_target(record, off::SourceName::literal("topo"), nullptr))) {
      return false;
    }
  }
  if (!off::reason_is_accept(fabric.set_topology_generation(
          off::TopologyGeneration::from_value(1), off::SourceName::literal("topo"), nullptr))) {
    return false;
  }
  for (int index = 0; index < targets; ++index) {
    off::CapabilityEvidence capability;
    capability.target = off::TargetName::literal("t" + std::to_string(index));
    capability.generation = off::CapabilityGeneration::from_value(1);
    capability.capabilities.insert(off::CapabilityCode::IPv4Forward);
    capability.topology_generation = off::TopologyGeneration::from_value(1);
    capability.incarnation.host = off::Incarnation::from_value(1);
    capability.incarnation.device = off::Incarnation::from_value(1);
    capability.source = off::SourceName::literal("cap");
    if (!off::reason_is_accept(fabric.ingest_capability(capability, nullptr))) {
      return false;
    }
  }
  return true;
}

bool register_services(off::Fabric& fabric, int services) {
  for (int index = 0; index < services; ++index) {
    off::ServiceDescriptor descriptor;
    descriptor.name = off::ServiceName::literal("svc" + std::to_string(index));
    descriptor.scope = off::ScopeName::literal("edge");
    descriptor.required_capabilities.insert(off::CapabilityCode::IPv4Forward);
    if (!off::reason_is_accept(fabric.register_service(descriptor,
                                                       off::SourceName::literal("topo"), nullptr))) {
      return false;
    }
  }
  return true;
}

/// Durable ingestion: every counted report survived a journal commit.
void bench_ingest() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "offab-bench-ingest";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  off::Fabric fabric(bench_config(directory));
  if (fabric.open() != off::Reason::Ok || !build_scenario(fabric, 4) ||
      !register_services(fabric, 2)) {
    std::fprintf(stderr, "bench: ingestion scenario setup failed\n");
    return;
  }
  constexpr std::uint64_t kReports = 1000;
  std::uint64_t completed = 0;
  const Clock::time_point start = Clock::now();
  for (std::uint64_t index = 0; index < kReports; ++index) {
    off::FailureEvidence evidence;
    evidence.service = off::ServiceName::literal("svc1");
    evidence.target.target = off::TargetName::literal("t1");
    evidence.target.incarnation.host = off::Incarnation::from_value(1);
    evidence.target.incarnation.device = off::Incarnation::from_value(1);
    evidence.failure_class = off::FailureClass::TargetUnresponsive;
    evidence.provenance.source = off::SourceName::literal("failrep");
    evidence.provenance.sequence = off::EvidenceSeq::from_value(index + 1);
    evidence.provenance.epoch = fabric.view().epoch;
    evidence.provenance.boot = fabric.view().boot;
    evidence.topology_generation = off::TopologyGeneration::from_value(1);
    if (off::reason_is_accept(fabric.ingest_failure(evidence, nullptr))) {
      ++completed;
    }
  }
  const Clock::time_point finish = Clock::now();
  // Completion is asserted, not assumed: the durable journal must reflect the
  // same number of accepted observations.
  if (completed != kReports) {
    std::fprintf(stderr, "bench: ingestion completed %llu of %llu\n",
                 static_cast<unsigned long long>(completed),
                 static_cast<unsigned long long>(kReports));
  }
  record("evidence ingestion (durable journal)", completed, start, finish);
  (void)fabric.close();
  std::filesystem::remove_all(directory, error);
}

/// Volatile ingestion: the same work without any durable commit, measured so the
/// cost of durability is visible rather than assumed.
void bench_ingest_volatile() {
  off::Fabric fabric;
  if (fabric.open() != off::Reason::Ok || !build_scenario(fabric, 4) ||
      !register_services(fabric, 2)) {
    std::fprintf(stderr, "bench: volatile scenario setup failed\n");
    return;
  }
  constexpr std::uint64_t kReports = 200000;
  std::uint64_t completed = 0;
  const Clock::time_point start = Clock::now();
  for (std::uint64_t index = 0; index < kReports; ++index) {
    off::FailureEvidence evidence;
    evidence.service = off::ServiceName::literal("svc1");
    evidence.target.target = off::TargetName::literal("t1");
    evidence.target.incarnation.host = off::Incarnation::from_value(1);
    evidence.target.incarnation.device = off::Incarnation::from_value(1);
    evidence.failure_class = off::FailureClass::TargetUnresponsive;
    evidence.provenance.source = off::SourceName::literal("failrep");
    evidence.provenance.sequence = off::EvidenceSeq::from_value(index + 1);
    evidence.provenance.epoch = fabric.view().epoch;
    evidence.provenance.boot = fabric.view().boot;
    evidence.topology_generation = off::TopologyGeneration::from_value(1);
    if (off::reason_is_accept(fabric.ingest_failure(evidence, nullptr))) {
      ++completed;
    }
  }
  const Clock::time_point finish = Clock::now();
  record("evidence ingestion (volatile, bounded)", completed, start, finish);
  (void)fabric.close();
}

void bench_placement_and_recovery() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "offab-bench-recovery";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  off::Fabric fabric(bench_config(directory));
  if (fabric.open() != off::Reason::Ok || !build_scenario(fabric, 64) ||
      !register_services(fabric, 64)) {
    std::fprintf(stderr, "bench: recovery scenario setup failed\n");
    return;
  }
  constexpr int kServices = 64;
  std::uint64_t placed = 0;
  const Clock::time_point place_start = Clock::now();
  for (int index = 0; index < kServices; ++index) {
    off::PlacementRequest request;
    request.service = off::ServiceName::literal("svc" + std::to_string(index));
    request.target = off::TargetName::literal("t" + std::to_string(index));
    request.requester = off::SourceName::literal("op");
    const off::RecoveryDecision decision = fabric.establish_placement(request);
    if (decision.accepted) {
      ++placed;
    }
  }
  const Clock::time_point place_finish = Clock::now();
  record("completed placements (durable)", placed, place_start, place_finish);

  std::uint64_t recovered = 0;
  std::uint64_t verified = 0;
  const Clock::time_point recover_start = Clock::now();
  for (int index = 0; index < kServices; ++index) {
    const std::string name = "svc" + std::to_string(index);
    off::FailureEvidence evidence;
    evidence.service = off::ServiceName::literal(name.c_str());
    evidence.target.target = off::TargetName::literal("t" + std::to_string(index));
    evidence.target.incarnation.host = off::Incarnation::from_value(1);
    evidence.target.incarnation.device = off::Incarnation::from_value(1);
    evidence.failure_class = off::FailureClass::TargetUnresponsive;
    evidence.provenance.source = off::SourceName::literal("failrep");
    // Each service is reported by the same source, so the sequence advances
    // per service; a source stream has to arrive in order.
    evidence.provenance.sequence = off::EvidenceSeq::from_value(index + 1);
    evidence.provenance.epoch = fabric.view().epoch;
    evidence.provenance.boot = fabric.view().boot;
    evidence.topology_generation = off::TopologyGeneration::from_value(1);
    off::Explanation explanation;
    if (!off::reason_is_accept(fabric.ingest_failure(evidence, &explanation))) {
      continue;
    }
    off::FailoverRequest request;
    request.service = off::ServiceName::literal(name.c_str());
    request.evidence = off::EvidenceId::from_value(explanation.steps.back().value);
    request.requester = off::SourceName::literal("op");
    const off::RecoveryDecision decision = fabric.request_failover(request);
    if (!decision.accepted) {
      continue;
    }
    ++recovered;
    off::IntentAck ack;
    ack.attempt = decision.intent->attempt;
    ack.fence = decision.intent->fence;
    ack.generation = decision.intent->generation;
    ack.accepted = true;
    ack.executor = off::SourceName::literal("exec");
    (void)fabric.acknowledge_intent(ack);
    off::EffectReport effect;
    effect.service = off::ServiceName::literal(name.c_str());
    effect.attempt = decision.intent->attempt;
    effect.generation = decision.intent->generation;
    effect.fence = decision.intent->fence;
    effect.target = decision.intent->to;
    effect.kind = off::EffectKind::ActivationAccepted;
    effect.result = off::EffectResult::Positive;
    effect.provenance.source = off::SourceName::literal("exec");
    effect.provenance.epoch = fabric.view().epoch;
    effect.provenance.boot = fabric.view().boot;
    (void)fabric.report_effect(effect);
    effect.kind = off::EffectKind::ServiceHealthy;
    effect.id = off::EffectId::from_value(1);
    const off::RecoveryDecision health = fabric.report_effect(effect);
    if (health.accepted) {
      ++verified;
    }
  }
  const Clock::time_point recover_finish = Clock::now();
  record("completed failovers (durable commit)", recovered, recover_start, recover_finish);
  record("verified recoveries (continuity claimed)", verified, recover_start, recover_finish);

  const off::FabricStats stats = fabric.stats();
  std::printf("evidence retained=%llu evicted=%llu attempts=%llu compactions=%llu\n",
              static_cast<unsigned long long>(stats.failures_ingested),
              static_cast<unsigned long long>(stats.evidence_evicted),
              static_cast<unsigned long long>(stats.attempts_recorded),
              static_cast<unsigned long long>(stats.journal_compactions));
  (void)fabric.close();
  std::filesystem::remove_all(directory, error);
}

void bench_explain_and_export() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "offab-bench-explain";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  off::FabricConfig config;
  config.store_directory = directory;
  config.max_explanations = 4096;
  off::Fabric fabric(config);
  if (fabric.open() != off::Reason::Ok || !build_scenario(fabric, 8) ||
      !register_services(fabric, 100)) {
    std::fprintf(stderr, "bench: explanation scenario setup failed\n");
    return;
  }
  constexpr std::uint64_t kExports = 1000;
  const Clock::time_point start = Clock::now();
  std::uint64_t completed = 0;
  std::size_t characters = 0;
  for (std::uint64_t index = 0; index < kExports; ++index) {
    const std::string canonical = fabric.export_canonical(false);
    if (!canonical.empty() && canonical.front() == '{') {
      ++completed;
      characters += canonical.size();
    }
  }
  const Clock::time_point finish = Clock::now();
  record("canonical exports (complete documents)", completed, start, finish);
  std::printf("export bytes per document=%llu\n",
              static_cast<unsigned long long>(characters / (completed == 0 ? 1 : completed)));
  (void)fabric.close();
  std::filesystem::remove_all(directory, error);
}

}  // namespace

int main(int argc, char** argv) {
  int rounds = 1;
  if (argc > 1) {
    off::u64 parsed = 0;
    if (off::parse_decimal(argv[1], parsed) && parsed > 0 && parsed <= 10) {
      rounds = static_cast<int>(parsed);
    }
  }
  for (int round = 0; round < rounds; ++round) {
    bench_ingest();
    bench_ingest_volatile();
    bench_placement_and_recovery();
    bench_explain_and_export();
  }
  std::printf("\n%-44s %12s %10s %14s\n", "measurement", "completed", "seconds", "per second");
  for (const Report& report : reports()) {
    const double rate = report.seconds > 0 ? static_cast<double>(report.completed) / report.seconds
                                           : 0.0;
    std::printf("%-44s %12llu %10.4f %14.0f\n", report.name,
                static_cast<unsigned long long>(report.completed), report.seconds, rate);
  }
  return 0;
}
