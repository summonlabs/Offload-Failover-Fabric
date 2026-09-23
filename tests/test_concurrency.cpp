// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using namespace offtest;

/// One report from one source. The runtime requires each source's stream to
/// arrive in sequence order, so every delivery order below preserves that.
void ingest_stream_step(Fixture& fixture, std::size_t stream, std::uint64_t sequence) {
  switch (stream % 3) {
    case 0:
      OFF_CHECK(fixture
                    .report_failure(fixture.t1, off::FailureClass::LinkDown, sequence,
                                    off::AmbiguityState::Unambiguous, off::TargetIncarnation{},
                                    fixture.failure_source)
                    .value() > 0);
      break;
    case 1:
      OFF_CHECK(fixture
                    .report_failure(fixture.t2, off::FailureClass::TargetUnresponsive, sequence,
                                    off::AmbiguityState::Unambiguous, off::TargetIncarnation{},
                                    fixture.failure_source2)
                    .value() > 0);
      break;
    default:
      OFF_CHECK(fixture
                    .report_failure(fixture.t3, off::FailureClass::ServiceCrash, sequence,
                                    off::AmbiguityState::Unambiguous, off::TargetIncarnation{},
                                    fixture.failure_source3)
                    .value() > 0);
      break;
  }
  OFF_CHECK(off::reason_is_accept(fixture.add_capability_evidence(stream, sequence)));
}

constexpr std::size_t kStreams = 3;
constexpr std::uint64_t kPerStream = 8;

OFF_TEST(concurrency, ingest_order_does_not_change_the_semantic_result) {
  Fixture round_robin;
  round_robin.build();
  for (std::uint64_t sequence = 1; sequence <= kPerStream; ++sequence) {
    for (std::size_t stream = 0; stream < kStreams; ++stream) {
      ingest_stream_step(round_robin, stream, sequence);
    }
  }
  const std::string expected = round_robin.fabric.view().semantic_json().dump();

  Fixture blocked;
  blocked.build();
  for (std::size_t stream = 0; stream < kStreams; ++stream) {
    for (std::uint64_t sequence = 1; sequence <= kPerStream; ++sequence) {
      ingest_stream_step(blocked, stream, sequence);
    }
  }
  OFF_CHECK_EQ(blocked.fabric.view().semantic_json().dump(), expected);

  Fixture reversed_streams;
  reversed_streams.build();
  for (std::size_t index = 0; index < kStreams; ++index) {
    const std::size_t stream = kStreams - 1 - index;
    for (std::uint64_t sequence = 1; sequence <= kPerStream; ++sequence) {
      ingest_stream_step(reversed_streams, stream, sequence);
    }
  }
  OFF_CHECK_EQ(reversed_streams.fabric.view().semantic_json().dump(), expected);
}

OFF_TEST(concurrency, concurrent_ingest_matches_the_serial_result) {
  Fixture serial;
  serial.build();
  for (std::uint64_t sequence = 1; sequence <= kPerStream; ++sequence) {
    for (std::size_t stream = 0; stream < kStreams; ++stream) {
      ingest_stream_step(serial, stream, sequence);
    }
  }
  const std::string expected = serial.fabric.view().semantic_json().dump();

  Fixture concurrent;
  concurrent.build();
  std::vector<std::thread> workers;
  std::atomic<bool> start{false};
  for (std::size_t stream = 0; stream < kStreams; ++stream) {
    workers.emplace_back([&concurrent, &start, stream]() {
      while (!start.load()) {
        std::this_thread::yield();
      }
      for (std::uint64_t sequence = 1; sequence <= kPerStream; ++sequence) {
        ingest_stream_step(concurrent, stream, sequence);
      }
    });
  }
  start.store(true);
  for (std::thread& worker : workers) {
    worker.join();
  }
  OFF_CHECK_EQ(concurrent.fabric.view().semantic_json().dump(), expected);
}

OFF_TEST(concurrency, concurrent_queries_never_observe_torn_state) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  std::atomic<bool> stop{false};
  std::atomic<int> observations{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&fixture, &stop, &observations]() {
      while (!stop.load()) {
        const off::ServiceView view = fixture.service_view();
        if (view.active.has_value()) {
          OFF_CHECK(view.fence.value() >= 1ULL);
          OFF_CHECK(view.generation.value() >= 1ULL);
          OFF_CHECK(view.continuity != off::ContinuityClass::Full || view.effect_verified);
        }
        observations.fetch_add(1);
      }
    });
  }
  for (std::uint64_t sequence = 1; sequence <= 60; ++sequence) {
    off::DependencyObservation observation;
    observation.service = fixture.service;
    observation.dependency = fixture.service;  // refused: self reference
    observation.health = off::DependencyHealth::Healthy;
    observation.provenance.source = fixture.dependency_source;
    observation.provenance.sequence = off::EvidenceSeq::from_value(sequence);
    const off::FabricView snapshot = fixture.fabric.view();
    observation.provenance.epoch = snapshot.epoch;
    observation.provenance.boot = snapshot.boot;
    observation.topology_generation = snapshot.topology_generation;
    (void)fixture.fabric.ingest_dependency(observation, nullptr);
    OFF_CHECK(fixture
                  .report_failure(fixture.t1, off::FailureClass::LinkDown, 900 + sequence,
                                  off::AmbiguityState::Unambiguous, off::TargetIncarnation{},
                                  fixture.failure_source)
                  .value() > 0);
  }
  stop.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }
  OFF_CHECK(observations.load() > 0);
}

OFF_TEST(concurrency, duplicate_concurrent_requests_commit_exactly_once) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  constexpr std::size_t kThreads = 6;
  std::vector<std::thread> workers;
  std::atomic<int> accepted{0};
  std::atomic<bool> start{false};
  std::vector<off::Reason> outcomes(kThreads, off::Reason::Internal);
  for (std::size_t worker = 0; worker < kThreads; ++worker) {
    workers.emplace_back([&fixture, &start, &accepted, &outcomes, worker, evidence]() {
      while (!start.load()) {
        std::this_thread::yield();
      }
      const off::RecoveryDecision decision = fixture.failover(evidence, 909);
      outcomes[worker] = decision.outcome;
      if (decision.accepted && !decision.duplicate) {
        accepted.fetch_add(1);
      }
    });
  }
  start.store(true);
  for (std::thread& worker : workers) {
    worker.join();
  }
  // Exactly one delivery performs the durable commit; every other delivery is
  // either a cached replay or refused for reusing an in-flight key.
  OFF_CHECK_EQ(accepted.load(), 1);
  for (const off::Reason outcome : outcomes) {
    OFF_CHECK(outcome == off::Reason::AcceptIntentEmitted ||
              outcome == off::Reason::AcceptDuplicateReplayCached ||
              outcome == off::Reason::ProtocolViolation);
  }
  OFF_CHECK_EQ(fixture.service_view().generation.value(), 2ULL);
  OFF_CHECK_EQ(fixture.service_view().fence.value(), 2ULL);
}

OFF_TEST(concurrency, cancellation_never_publishes_success_after_being_recorded) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::EvidenceId evidence =
      fixture.report_failure(fixture.t1, off::FailureClass::TargetUnresponsive, 1);
  std::atomic<bool> stop{false};
  std::atomic<int> recorded{0};
  std::thread canceller([&fixture, &stop, &recorded]() {
    while (!stop.load()) {
      if (fixture.fabric.cancel(off::RequestKey::from_value(31), nullptr) ==
          off::Reason::Cancelled) {
        recorded.fetch_add(1);
      }
    }
  });
  const off::RecoveryDecision decision = fixture.failover(evidence, 31);
  stop.store(true);
  canceller.join();
  const off::ServiceView view = fixture.service_view();
  if (decision.outcome == off::Reason::Cancelled) {
    OFF_CHECK(!decision.accepted);
    OFF_CHECK_EQ(view.generation.value(), 1ULL);
    OFF_CHECK(!view.active.has_value() || view.active->target == fixture.t1);
  } else {
    OFF_CHECK(decision.accepted);
    OFF_CHECK_EQ(view.generation.value(), 2ULL);
  }
}

OFF_TEST(concurrency, repeated_start_and_stop_returns_accounting_to_baseline) {
  const std::filesystem::path directory = unique_directory("concurrency-lifecycle");
  {
    off::FabricConfig config;
    config.store_directory = directory;
    Fixture fixture(config);
    fixture.build();
    OFF_CHECK(fixture.place(fixture.t1).accepted);
    OFF_CHECK(fixture.fabric.intent_queue_depth() == 1);
    OFF_CHECK_REASON(fixture.fabric.close(), off::Reason::Ok);
    OFF_CHECK(!fixture.fabric.is_open());
    OFF_CHECK_REASON(fixture.fabric.close(), off::Reason::InvalidState);
  }
  for (int round = 0; round < 3; ++round) {
    off::FabricConfig config;
    config.store_directory = directory;
    Fixture reopened(config);
    OFF_CHECK_REASON(reopened.fabric.open(), off::Reason::Ok);
    const off::ServiceView view = reopened.service_view();
    OFF_CHECK(view.active.has_value());
    OFF_CHECK(view.active->target == reopened.t1);
    OFF_CHECK_EQ(view.fence.value(), 1ULL);
    OFF_CHECK_EQ(reopened.fabric.intent_queue_depth(), 0);
    OFF_CHECK_REASON(reopened.fabric.close(), off::Reason::Ok);
  }
}

OFF_TEST(concurrency, wait_for_intent_blocks_until_work_completes) {
  Fixture fixture;
  fixture.build();
  std::thread producer([&fixture]() { OFF_CHECK(fixture.place(fixture.t1).accepted); });
  off::FailoverIntent intent;
  OFF_CHECK(fixture.fabric.wait_for_intent(intent));
  OFF_CHECK(intent.service == fixture.service);
  producer.join();
  OFF_CHECK(fixture.fabric.intent_queue_depth() == 0);
  OFF_CHECK_REASON(fixture.fabric.close(), off::Reason::Ok);
  off::FailoverIntent unused;
  OFF_CHECK(!fixture.fabric.wait_for_intent(unused));
}

}  // namespace
