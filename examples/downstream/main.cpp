// Independent downstream consumer of the installed package.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is built only against the installed headers and library. It
// exercises one full governed recovery and checks the invariants the package
// promises to a consumer.

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include <off/off.hpp>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "downstream: FAILED %s\n", what);
    ++failures;
  }
}

off::ServiceName service_name(const char* text) { return off::ServiceName::literal(text); }
off::ScopeName scope_name(const char* text) { return off::ScopeName::literal(text); }
off::TargetName target_name(const char* text) { return off::TargetName::literal(text); }
off::SourceName source_name(const char* text) { return off::SourceName::literal(text); }

}  // namespace

int main() {
  std::printf("downstream: linked against Offload Failover Fabric %s (schema %u)\n",
              std::string(off::version_string()).c_str(),
              static_cast<unsigned>(off::kCanonicalSchemaVersion));

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "offab-downstream";
  std::error_code error;
  std::filesystem::remove_all(directory, error);

  off::FabricConfig config;
  config.store_directory = directory;
  off::Fabric fabric(config);

  const off::Reason opened = fabric.open();
  check(opened == off::Reason::Ok, "fabric opens");

  check(off::reason_is_accept(fabric.enroll_source(source_name("op"), off::SourceRole::Operator, nullptr)),
        "operator enrols");
  check(off::reason_is_accept(fabric.enroll_source(source_name("topo"),
                                                   off::SourceRole::TopologyReporter, nullptr)),
        "topology reporter enrols");
  check(off::reason_is_accept(fabric.enroll_source(source_name("cap"),
                                                   off::SourceRole::CapabilityReporter, nullptr)),
        "capability reporter enrols");
  check(off::reason_is_accept(fabric.enroll_source(source_name("failrep"),
                                                   off::SourceRole::FailureReporter, nullptr)),
        "failure reporter enrols");
  check(off::reason_is_accept(fabric.enroll_source(source_name("exec"),
                                                   off::SourceRole::EffectReporter, nullptr)),
        "effect reporter enrols");

  const off::TargetName primary = target_name("t1");
  const off::TargetName standby = target_name("t2");
  for (int index = 0; index < 2; ++index) {
    off::TargetRecord record;
    record.name = index == 0 ? primary : standby;
    record.host = off::HostName::literal(index == 0 ? "h1" : "h2");
    record.device = off::DeviceName::literal(index == 0 ? "nic1" : "nic2");
    record.kind = index == 0 ? off::DeviceKind::SmartNic : off::DeviceKind::Dpu;
    record.scope = scope_name("edge");
    record.incarnation.host = off::Incarnation::from_value(1);
    record.incarnation.device = off::Incarnation::from_value(1);
    check(off::reason_is_accept(
              fabric.register_target(record, source_name("topo"), nullptr)),
          "target registers");
  }
  check(off::reason_is_accept(fabric.set_topology_generation(
            off::TopologyGeneration::from_value(1), source_name("topo"), nullptr)),
        "topology generation advances");

  for (int index = 0; index < 2; ++index) {
    off::CapabilityEvidence capability;
    capability.target = index == 0 ? primary : standby;
    capability.generation = off::CapabilityGeneration::from_value(1);
    capability.capabilities.insert(off::CapabilityCode::IPv4Forward);
    capability.topology_generation = off::TopologyGeneration::from_value(1);
    capability.incarnation.host = off::Incarnation::from_value(1);
    capability.incarnation.device = off::Incarnation::from_value(1);
    capability.source = source_name("cap");
    check(off::reason_is_accept(fabric.ingest_capability(capability, nullptr)),
          "capability evidence is accepted");
  }

  off::ServiceDescriptor descriptor;
  descriptor.name = service_name("svc1");
  descriptor.scope = scope_name("edge");
  descriptor.required_capabilities.insert(off::CapabilityCode::IPv4Forward);
  check(off::reason_is_accept(fabric.register_service(descriptor, source_name("topo"), nullptr)),
        "service registers");

  off::PlacementRequest placement;
  placement.service = descriptor.name;
  placement.target = primary;
  placement.requester = source_name("op");
  const off::RecoveryDecision placed = fabric.establish_placement(placement);
  check(placed.accepted, "initial placement is authorized");
  check(placed.plan.fence.value() == 1, "initial fence is one");

  const off::FabricView before = fabric.view();
  const off::u64 epoch = before.epoch.value();
  const off::u64 boot = before.boot.value();

  off::FailureEvidence failure;
  failure.service = descriptor.name;
  failure.target.target = primary;
  failure.target.incarnation.host = off::Incarnation::from_value(1);
  failure.target.incarnation.device = off::Incarnation::from_value(1);
  failure.failure_class = off::FailureClass::TargetUnresponsive;
  failure.provenance.source = source_name("failrep");
  failure.provenance.sequence = off::EvidenceSeq::from_value(1);
  failure.provenance.epoch = off::CoordinatorEpoch::from_value(epoch);
  failure.provenance.boot = off::BootId::from_value(boot);
  failure.topology_generation = off::TopologyGeneration::from_value(1);
  off::Explanation ingest_explanation;
  check(off::reason_is_accept(fabric.ingest_failure(failure, &ingest_explanation)),
        "failure evidence is retained");

  off::FailoverRequest request;
  request.service = descriptor.name;
  request.evidence = off::EvidenceId::from_value(ingest_explanation.steps.back().value);
  request.requester = source_name("op");
  const off::RecoveryDecision decision = fabric.request_failover(request);
  check(decision.accepted, "recovery is authorized");
  check(decision.plan.fence.value() == 2, "durable fence advanced");
  check(decision.plan.to.target == standby, "the fallback target is selected");
  check(decision.intent.has_value(), "a governed intent was emitted");

  off::FenceCheck fence_check;
  fence_check.service = descriptor.name;
  fence_check.holder.target = primary;
  fence_check.holder.incarnation.host = off::Incarnation::from_value(1);
  fence_check.holder.incarnation.device = off::Incarnation::from_value(1);
  fence_check.presented = off::FenceToken::from_value(1);
  const off::FenceVerdict verdict = fabric.verify_fence(fence_check);
  check(!verdict.authorized, "the previous target is fenced out");
  check(verdict.code == off::Reason::FenceStale, "the refusal names a stale fence");

  off::ServiceView view = fabric.view().services.front();
  check(view.continuity == off::ContinuityClass::Unknown, "continuity is not claimed yet");

  off::EffectReport effect;
  effect.service = descriptor.name;
  effect.attempt = decision.intent->attempt;
  effect.generation = decision.intent->generation;
  effect.fence = decision.intent->fence;
  effect.target = decision.intent->to;
  effect.kind = off::EffectKind::ActivationAccepted;
  effect.result = off::EffectResult::Positive;
  effect.provenance.source = source_name("exec");
  effect.provenance.epoch = off::CoordinatorEpoch::from_value(epoch);
  effect.provenance.boot = off::BootId::from_value(boot);
  check(fabric.report_effect(effect).accepted, "activation effect is accepted");

  effect.kind = off::EffectKind::ServiceHealthy;
  effect.id = off::EffectId::from_value(1);
  check(fabric.report_effect(effect).accepted, "healthy effect is accepted");
  view = fabric.view().services.front();
  check(view.continuity == off::ContinuityClass::Full, "continuity is claimed only now");
  check(view.effect_verified, "a verified effect exists");

  const std::string export_text = fabric.export_canonical(false);
  check(export_text.find("\"svc1\"") != std::string::npos, "the export names the service");
  check(!fabric.view().semantic_digest().to_hex().empty(), "the semantic digest is stable");

  check(fabric.close() == off::Reason::Ok, "fabric closes");
  std::filesystem::remove_all(directory, error);

  if (failures == 0) {
    std::printf("downstream: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "downstream: %d check(s) failed\n", failures);
  return 1;
}
