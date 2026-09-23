// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using namespace offtest;

off::FabricConfig persistent_config(const std::filesystem::path& directory) {
  off::FabricConfig config;
  config.store_directory = directory;
  return config;
}

void register_extra_service(Fixture& fixture, const char* name) {
  off::ServiceDescriptor descriptor;
  descriptor.name = service_name(name);
  descriptor.scope = fixture.scope;
  descriptor.statefulness = off::Statefulness::Stateless;
  descriptor.required_capabilities = capabilities({off::CapabilityCode::IPv4Forward});
  OFF_CHECK(off::reason_is_accept(
      fixture.fabric.register_service(descriptor, fixture.topology_source, nullptr)));
}

void reingest_capabilities(Fixture& fixture, std::uint64_t generation) {
  fixture.add_capability(fixture.t1, generation,
                         capabilities({off::CapabilityCode::IPv4Forward}), 1, 1);
  fixture.add_capability(fixture.t2, generation,
                         capabilities({off::CapabilityCode::IPv4Forward}), 1, 1);
  fixture.add_capability(fixture.t3, generation,
                         capabilities({off::CapabilityCode::IPv4Forward}), 1, 1);
}

void append_bytes(const std::filesystem::path& path, const std::vector<off::u8>& data) {
  std::FILE* file = std::fopen(path.string().c_str(), "ab");
  if (file == nullptr) {
    OFF_CHECK(false);
    return;
  }
  if (!data.empty()) {
    OFF_CHECK(std::fwrite(data.data(), 1, data.size(), file) == data.size());
  }
  std::fclose(file);
}

void corrupt_first_byte(const std::filesystem::path& path, std::size_t offset) {
  std::FILE* file = std::fopen(path.string().c_str(), "r+b");
  if (file == nullptr) {
    OFF_CHECK(false);
    return;
  }
  std::fseek(file, static_cast<long>(offset), SEEK_SET);
  int byte = std::fgetc(file);
  if (byte == EOF) {
    OFF_CHECK(false);
    std::fclose(file);
    return;
  }
  std::fseek(file, static_cast<long>(offset), SEEK_SET);
  OFF_CHECK(std::fputc(byte ^ 0x5A, file) != EOF);
  std::fclose(file);
}

OFF_TEST(restart, clean_reopen_preserves_placements_fences_and_history) {
  const std::filesystem::path directory = unique_directory("restart-clean");
  off::ServiceView before{};
  {
    Fixture fixture(persistent_config(directory));
    fixture.build();
    OFF_CHECK(fixture.place(fixture.t1).accepted);
    const off::EvidenceId evidence =
        fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
    OFF_REQUIRE(fixture.failover(evidence).accepted);
    before = fixture.service_view();
    OFF_CHECK_REASON(fixture.fabric.close(), off::Reason::Ok);
  }
  Fixture reopened(persistent_config(directory));
  OFF_CHECK_REASON(reopened.fabric.open(), off::Reason::Ok);
  const off::ServiceView after = reopened.service_view();
  OFF_CHECK(after.active.has_value());
  OFF_CHECK(before.active.has_value());
  OFF_CHECK(after.active->target == before.active->target);
  OFF_CHECK(after.active->incarnation == before.active->incarnation);
  OFF_CHECK_EQ(after.generation.value(), before.generation.value());
  OFF_CHECK_EQ(after.fence.value(), before.fence.value());
  OFF_CHECK_EQ(after.lease_term.value(), before.lease_term.value());
  OFF_CHECK_EQ(after.attempts_recorded, before.attempts_recorded);
  OFF_CHECK_EQ(after.current_attempt.value(), before.current_attempt.value());
  OFF_CHECK(after.failover_recorded);
}

OFF_TEST(restart, epoch_and_boot_advance_and_previous_authority_is_fenced) {
  const std::filesystem::path directory = unique_directory("restart-epoch");
  off::u64 first_epoch = 0;
  off::u64 first_boot = 0;
  {
    Fixture fixture(persistent_config(directory));
    fixture.build();
    OFF_CHECK(fixture.place(fixture.t1).accepted);
    const off::FabricView view = fixture.fabric.view();
    first_epoch = view.epoch.value();
    first_boot = view.boot.value();
  }
  Fixture reopened(persistent_config(directory));
  OFF_CHECK_REASON(reopened.fabric.open(), off::Reason::Ok);
  const off::FabricView view = reopened.fabric.view();
  OFF_CHECK(view.epoch.value() > first_epoch);
  OFF_CHECK(view.boot.value() > first_boot);
  const off::RestartSummary& summary = reopened.fabric.restart_summary();
  OFF_CHECK(summary.conservative);
  OFF_CHECK_EQ(summary.previous_epoch.value(), first_epoch);
  OFF_CHECK_EQ(summary.epoch.value(), view.epoch.value());
}

OFF_TEST(restart, in_flight_recovery_is_downgraded_and_never_resurrected) {
  const std::filesystem::path directory = unique_directory("restart-downgrade");
  {
    Fixture fixture(persistent_config(directory));
    fixture.build();
    OFF_CHECK(fixture.place(fixture.t1).accepted);
    const off::EvidenceId evidence =
        fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
    OFF_REQUIRE(fixture.failover(evidence).accepted);
    OFF_CHECK_EQ(fixture.service_view().recovery, off::RecoveryPhase::Authorized);
  }
  Fixture reopened(persistent_config(directory));
  OFF_CHECK_REASON(reopened.fabric.open(), off::Reason::Ok);
  const off::ServiceView view = reopened.service_view();
  OFF_CHECK_EQ(view.recovery, off::RecoveryPhase::IntentRecorded);
  OFF_CHECK_EQ(view.lifecycle, off::LifecyclePhase::RecoveryPending);
  OFF_CHECK_EQ(view.continuity, off::ContinuityClass::Unknown);
  OFF_CHECK(!view.effect_verified);
  OFF_CHECK_EQ(view.last_outcome, off::AttemptOutcome::OutcomeUnknown);
  // Both the placement attempt and the recovery attempt were durable but
  // unacknowledged when the process stopped.
  OFF_CHECK(reopened.fabric.restart_summary().attempts_marked_unknown >= 1U);
  OFF_CHECK_EQ(reopened.fabric.restart_summary().services_downgraded, 1U);
}

OFF_TEST(restart, verified_effect_does_not_survive_as_claimed_continuity) {
  const std::filesystem::path directory = unique_directory("restart-verified");
  {
    Fixture fixture(persistent_config(directory));
    fixture.build();
    OFF_CHECK(fixture.place(fixture.t1).accepted);
    const off::EvidenceId evidence =
        fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
    OFF_REQUIRE(fixture.failover(evidence).accepted);
    const off::ServiceView view = fixture.service_view();
    fixture.verify_continuity(view.generation, view.fence, fixture.t2);
    OFF_CHECK_EQ(fixture.service_view().continuity, off::ContinuityClass::Full);
  }
  Fixture reopened(persistent_config(directory));
  OFF_CHECK_REASON(reopened.fabric.open(), off::Reason::Ok);
  const off::ServiceView view = reopened.service_view();
  OFF_CHECK_EQ(view.continuity, off::ContinuityClass::Unknown);
  OFF_CHECK(!view.effect_verified);
}

OFF_TEST(restart, capability_evidence_requires_reconfirmation) {
  const std::filesystem::path directory = unique_directory("restart-capability");
  {
    Fixture fixture(persistent_config(directory));
    fixture.build();
    register_extra_service(fixture, "svc2");
    OFF_CHECK(fixture.place(fixture.t1).accepted);
  }
  Fixture reopened(persistent_config(directory));
  OFF_CHECK_REASON(reopened.fabric.open(), off::Reason::Ok);
  off::PlacementRequest request;
  request.service = service_name("svc2");
  request.target = reopened.t1;
  request.requester = reopened.operator_source;
  const off::RecoveryDecision decision = reopened.fabric.establish_placement(request);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::FallbackCapabilityGenerationStale);
  OFF_CHECK(reopened.fabric.restart_summary().capability_evidence_invalidated >= 3U);

  reingest_capabilities(reopened, 2);
  const off::RecoveryDecision retry = reopened.fabric.establish_placement(request);
  OFF_CHECK(retry.accepted);
}

OFF_TEST(restart, dependency_observations_require_refresh) {
  const std::filesystem::path directory = unique_directory("restart-dependency");
  {
    Fixture fixture(persistent_config(directory));
    fixture.dependencies.push_back(off::DependencySpec{
        service_name("dns"), off::DependencyKind::Hard, off::DependencyRequirement::AnyTarget});
    fixture.build();
    register_extra_service(fixture, "dns");
    off::DependencyObservation observation;
    observation.service = fixture.service;
    observation.dependency = service_name("dns");
    observation.health = off::DependencyHealth::Healthy;
    observation.provenance.source = fixture.dependency_source;
    observation.provenance.sequence = off::EvidenceSeq::from_value(1);
    observation.provenance.epoch = fixture.fabric.view().epoch;
    observation.provenance.boot = fixture.fabric.view().boot;
    observation.topology_generation = fixture.fabric.view().topology_generation;
    OFF_CHECK(off::reason_is_accept(fixture.fabric.ingest_dependency(observation, nullptr)));
    OFF_CHECK(fixture.place(fixture.t1).accepted);
  }
  Fixture reopened(persistent_config(directory));
  OFF_CHECK_REASON(reopened.fabric.open(), off::Reason::Ok);
  OFF_CHECK(reopened.fabric.restart_summary().dependency_observations_invalidated >= 1U);
  const off::EvidenceId evidence =
      reopened.report_failure(reopened.t1, off::FailureClass::TargetUnresponsive, 2);
  reingest_capabilities(reopened, 2);
  const off::RecoveryDecision decision = reopened.failover(evidence);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::DependencyUnresolved);
}

OFF_TEST(restart, evidence_from_a_previous_epoch_cannot_justify_new_authority) {
  const std::filesystem::path directory = unique_directory("restart-evidence");
  off::EvidenceId evidence{};
  {
    Fixture fixture(persistent_config(directory));
    fixture.build();
    OFF_CHECK(fixture.place(fixture.t1).accepted);
    evidence = fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  }
  Fixture reopened(persistent_config(directory));
  OFF_CHECK_REASON(reopened.fabric.open(), off::Reason::Ok);
  reingest_capabilities(reopened, 2);
  const off::RecoveryDecision decision = reopened.failover(evidence);
  OFF_CHECK(!decision.accepted);
  OFF_CHECK(decision.outcome == off::Reason::AuthoritySuperseded);
  OFF_CHECK(reopened.fabric.restart_summary().failure_evidence_invalidated >= 1U);
}

OFF_TEST(restart, resume_reissues_the_same_attempt_at_the_same_fence) {
  const std::filesystem::path directory = unique_directory("restart-resume");
  off::AttemptId attempt{};
  off::FenceToken fence{};
  off::FailoverGeneration generation{};
  {
    Fixture fixture(persistent_config(directory));
    fixture.build();
    OFF_CHECK(fixture.place(fixture.t1).accepted);
    const off::EvidenceId evidence =
        fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
    OFF_REQUIRE(fixture.failover(evidence).accepted);
    const off::ServiceView view = fixture.service_view();
    attempt = view.current_attempt;
    fence = view.fence;
    generation = view.generation;
  }
  Fixture reopened(persistent_config(directory));
  OFF_CHECK_REASON(reopened.fabric.open(), off::Reason::Ok);

  off::FailoverRequest unauthorized;
  unauthorized.service = reopened.service;
  unauthorized.requester = reopened.failure_source;
  unauthorized.trigger = off::RecoveryTrigger::OperatorCommand;
  OFF_CHECK(reopened.fabric.resume_recovery(unauthorized).outcome == off::Reason::NoAuthority);

  reingest_capabilities(reopened, 2);
  off::FailoverRequest request;
  request.key = off::RequestKey::from_value(4242);
  request.service = reopened.service;
  request.requester = reopened.operator_source;
  request.trigger = off::RecoveryTrigger::OperatorCommand;
  request.operator_authorized = true;
  const off::RecoveryDecision decision = reopened.fabric.resume_recovery(request);
  OFF_CHECK(decision.accepted);
  OFF_REQUIRE(decision.intent.has_value());
  OFF_CHECK(decision.intent->attempt == attempt);
  OFF_CHECK(decision.intent->fence == fence);
  OFF_CHECK(decision.intent->generation == generation);
  OFF_CHECK(decision.intent->reissue_after_restart);
  OFF_CHECK_EQ(reopened.fabric.stats().recovery_reissues, 1);
}

OFF_TEST(restart, torn_tail_is_repaired_and_committed_state_survives) {
  const std::filesystem::path directory = unique_directory("restart-torn");
  {
    Fixture fixture(persistent_config(directory));
    fixture.build();
    OFF_CHECK(fixture.place(fixture.t1).accepted);
  }
  const std::filesystem::path journal = directory / off::journal_file_name(1);
  const std::vector<off::u8> partial = {0x4F, 0x46, 0x41, 0x4A, 0x00, 0x01};
  append_bytes(journal, partial);
  Fixture reopened(persistent_config(directory));
  OFF_CHECK_REASON(reopened.fabric.open(), off::Reason::Ok);
  OFF_CHECK(reopened.fabric.restart_summary().store.truncated_tail_repaired);
  OFF_CHECK(reopened.fabric.restart_summary().store.classification ==
            off::RecoveryClass::TornTailRepaired);
  const off::ServiceView view = reopened.service_view();
  OFF_CHECK(view.active.has_value());
  OFF_CHECK(view.active->target == reopened.t1);
  OFF_CHECK_EQ(view.fence.value(), 1ULL);
}

OFF_TEST(restart, corrupt_journal_refuses_to_open_rather_than_guessing) {
  const std::filesystem::path directory = unique_directory("restart-corrupt");
  {
    Fixture fixture(persistent_config(directory));
    fixture.build();
    OFF_CHECK(fixture.place(fixture.t1).accepted);
    const off::EvidenceId evidence =
        fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
    OFF_REQUIRE(fixture.failover(evidence).accepted);
  }
  const std::filesystem::path journal = directory / off::journal_file_name(1);
  corrupt_first_byte(journal, 30);
  Fixture reopened(persistent_config(directory));
  const off::Reason code = reopened.fabric.open();
  OFF_CHECK(code != off::Reason::Ok);
  OFF_CHECK(code == off::Reason::StoreCorrupt);
  OFF_CHECK(!reopened.fabric.is_open());
}

OFF_TEST(restart, snapshot_compaction_and_replay_round_trip_every_field) {
  const std::filesystem::path directory = unique_directory("restart-snapshot");
  off::FabricConfig config = persistent_config(directory);
  config.max_journal_bytes = 512;  // force compaction during the scenario
  off::ServiceView before{};
  std::size_t target_count = 0;
  {
    Fixture fixture(config);
    fixture.build();
    OFF_CHECK(fixture.place(fixture.t1).accepted);
    const off::EvidenceId evidence =
        fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
    OFF_REQUIRE(fixture.failover(evidence).accepted);
    OFF_CHECK(fixture.fabric.stats().journal_compactions > 0);
    before = fixture.service_view();
    target_count = fixture.fabric.view().targets.size();
  }
  Fixture reopened(config);
  OFF_CHECK_REASON(reopened.fabric.open(), off::Reason::Ok);
  const off::ServiceView after = reopened.service_view();
  OFF_CHECK_EQ(reopened.fabric.view().targets.size(), target_count);
  OFF_CHECK(after.active.has_value());
  OFF_CHECK(after.active->target == before.active->target);
  OFF_CHECK_EQ(after.generation.value(), before.generation.value());
  OFF_CHECK_EQ(after.fence.value(), before.fence.value());
  OFF_CHECK_EQ(after.lease_term.value(), before.lease_term.value());
  OFF_CHECK_EQ(after.current_attempt.value(), before.current_attempt.value());
  OFF_CHECK_EQ(after.attempts_recorded, before.attempts_recorded);
  OFF_CHECK(!after.effect_verified);
  OFF_CHECK_EQ(after.continuity, off::ContinuityClass::Unknown);
}

}  // namespace
