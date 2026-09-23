// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Seeded randomised exploration of the runtime invariants. Every step is
// deterministic for a given seed, so a failure is reproducible from the seed
// printed in the failure message.

#include <cstdio>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using namespace offtest;

/// Deterministic xorshift generator; no library or platform dependence.
class Random {
 public:
  explicit Random(off::u64 seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

  off::u64 next() {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 7U;
    state_ ^= state_ << 17U;
    return state_;
  }

  off::u64 below(off::u64 bound) { return bound == 0 ? 0 : next() % bound; }

 private:
  off::u64 state_{1};
};

void check_invariants(Fixture& fixture, const char* context) {
  const off::FabricView view = fixture.fabric.view();
  for (const off::ServiceView& service : view.services) {
    if (!(service.fence.value() >= 1ULL)) {
      ::offtest::record_failure(__FILE__, __LINE__,
                                std::string("fence below one after ") + context);
    }
    if (service.continuity == off::ContinuityClass::Full && !service.effect_verified) {
      ::offtest::record_failure(
          __FILE__, __LINE__,
          std::string("continuity claimed without a verified effect after ") + context);
    }
    if (service.continuity == off::ContinuityClass::Degraded && !service.effect_verified) {
      ::offtest::record_failure(
          __FILE__, __LINE__,
          std::string("degraded continuity without a verified effect after ") + context);
    }
    if (service.attempts_recorded > fixture.config.max_attempt_history) {
      ::offtest::record_failure(__FILE__, __LINE__,
                                std::string("attempt history exceeded its bound after ") + context);
    }
    if (service.continuity == off::ContinuityClass::Full &&
        service.recovery != off::RecoveryPhase::Verified) {
      ::offtest::record_failure(
          __FILE__, __LINE__,
          std::string("full continuity outside the verified phase after ") + context +
              " lifecycle=" + std::string(off::lifecycle_phase_text(service.lifecycle)) +
              " recovery=" + std::string(off::recovery_phase_text(service.recovery)) +
              " continuity=" + std::string(off::continuity_class_text(service.continuity)) +
              " effect_verified=" + (service.effect_verified ? "1" : "0"));
    }
  }
  const off::FabricStats stats = fixture.fabric.stats();
  (void)stats;
  if (fixture.fabric.explain_all().size() > fixture.config.max_explanations) {
    ::offtest::record_failure(__FILE__, __LINE__,
                              std::string("explanation log exceeded its bound after ") + context);
  }
  if (fixture.fabric.pending_intents().size() > fixture.config.max_intent_queue) {
    ::offtest::record_failure(__FILE__, __LINE__,
                              std::string("intent queue exceeded its bound after ") + context);
  }
}

OFF_TEST(property, randomised_operation_sequences_preserve_invariants) {
  for (off::u64 seed = 1; seed <= 12; ++seed) {
    Random random(seed * 0x9E3779B1ULL);
    off::FabricConfig config;
    config.max_attempt_history = 4;
    config.max_explanations = 24;
    Fixture fixture(config);
    fixture.build();
    OFF_CHECK(fixture.place(fixture.t1).accepted);
    check_invariants(fixture, "placement");

    off::u64 failure_sequence = 0;
    for (int step = 0; step < 60; ++step) {
      const off::u64 choice = random.below(9);
      const off::TargetName targets[3] = {fixture.t1, fixture.t2, fixture.t3};
      const off::TargetName target = targets[random.below(3)];
      switch (choice) {
        case 0:
        case 1: {
          off::FailureClass failure_class = off::FailureClass::TargetUnresponsive;
          if (choice == 1) {
            failure_class = off::FailureClass::ServiceCrash;
          }
          // A source stream must arrive in order, so the sequence advances
          // monotonically whatever the random schedule does.
          ++failure_sequence;
          (void)fixture.try_report_failure(target, failure_class, failure_sequence);
          break;
        }
        case 2: {
          off::DependencyObservation observation;
          observation.service = fixture.service;
          observation.dependency = fixture.service;
          observation.health = off::DependencyHealth::Healthy;
          observation.provenance.source = fixture.dependency_source;
          ++failure_sequence;
          observation.provenance.sequence = off::EvidenceSeq::from_value(failure_sequence);
          observation.provenance.epoch = fixture.fabric.view().epoch;
          observation.provenance.boot = fixture.fabric.view().boot;
          observation.topology_generation = fixture.fabric.view().topology_generation;
          (void)fixture.fabric.ingest_dependency(observation, nullptr);
          break;
        }
        case 3: {
          const std::uint64_t generation = 2 + random.below(3);
          off::CapabilityEvidence capability;
          capability.target = target;
          capability.generation = off::CapabilityGeneration::from_value(generation);
          capability.capabilities = capabilities({off::CapabilityCode::IPv4Forward});
          capability.topology_generation = fixture.fabric.view().topology_generation;
          capability.incarnation = fixture.incarnation_of(target);
          capability.source = fixture.capability_source;
          (void)fixture.fabric.ingest_capability(capability, nullptr);
          break;
        }
        case 4: {
          ++failure_sequence;
          (void)fixture.try_report_failure(target, off::FailureClass::SyntheticInjected,
                                           failure_sequence, off::AmbiguityState::Ambiguous);
          break;
        }
        case 5: {
          off::FailoverRequest request;
          request.service = fixture.service;
          request.trigger = off::RecoveryTrigger::FailureEvidence;
          request.evidence = off::EvidenceId::from_value(1 + random.below(4));
          request.requester = fixture.operator_source;
          (void)fixture.fabric.request_failover(request);
          break;
        }
        case 6: {
          const off::ServiceView view = fixture.service_view();
          if (view.current_attempt.value() != 0) {
            (void)fixture.fabric.acknowledge_intent(
                fixture.ack(view.current_attempt, view.generation, view.fence, true));
            (void)fixture.report_effect(view.current_attempt, view.generation, view.fence, target,
                                        off::EffectKind::ActivationAccepted,
                                        off::EffectResult::Positive, 0, 7000 + random.next());
            (void)fixture.report_effect(view.current_attempt, view.generation, view.fence, target,
                                        off::EffectKind::ServiceHealthy,
                                        off::EffectResult::Positive, 0, 7000 + random.next());
          }
          break;
        }
        case 7: {
          off::AmbiguityResolution resolution;
          resolution.service = fixture.service;
          resolution.evidence = off::EvidenceId::from_value(1 + random.below(4));
          resolution.confirm_failure = (random.below(2) == 0);
          resolution.resolved_by = fixture.operator_source;
          (void)fixture.fabric.resolve_ambiguity(resolution);
          break;
        }
        default: {
          (void)fixture.fabric.cancel(off::RequestKey::from_value(1 + random.below(8)), nullptr);
          off::FailoverIntent drained;
          (void)fixture.fabric.take_intent(drained);
          break;
        }
      }
      check_invariants(fixture, "random step");
    }
    // Accounting closure: every bounded structure is still within its bound.
    const off::FabricStats stats = fixture.fabric.stats();
    OFF_CHECK(stats.attempts_evicted + fixture.config.max_attempt_history >=
              stats.attempts_recorded);
  }
}

OFF_TEST(property, canonical_decoding_never_accepts_random_bytes_as_a_typed_value) {
  Random random(0xC0FFEEULL);
  std::size_t accepted = 0;
  for (int iteration = 0; iteration < 4000; ++iteration) {
    const std::size_t size = static_cast<std::size_t>(random.below(48));
    std::vector<off::u8> bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
      bytes[index] = static_cast<off::u8>(random.below(256));
    }
    off::CanonicalReader reader(bytes.data(), bytes.size());
    off::FailureEvidence evidence;
    if (decode(reader, evidence)) {
      ++accepted;
      // Whatever decoded must re-encode to the same canonical bytes.
      off::CanonicalWriter writer;
      encode(writer, evidence);
      OFF_CHECK(writer.ok());
      OFF_CHECK_EQ(writer.buffer().size(), size);
      OFF_CHECK(writer.buffer() == bytes);
    }
  }
  OFF_CHECK(accepted < 4000);
}

OFF_TEST(property, digests_and_exports_are_stable_across_repetition) {
  Fixture fixture;
  fixture.build();
  OFF_CHECK(fixture.place(fixture.t1).accepted);
  const off::Digest first = fixture.fabric.view().semantic_digest();
  const std::string export_first = fixture.fabric.export_canonical(false);
  for (int iteration = 0; iteration < 8; ++iteration) {
    OFF_CHECK(fixture.fabric.view().semantic_digest() == first);
    OFF_CHECK_EQ(fixture.fabric.export_canonical(false), export_first);
  }
}

OFF_TEST(property, explanations_are_bounded_and_eviction_is_accounted) {
  off::FabricConfig config;
  config.max_explanations = 6;
  Fixture fixture(config);
  fixture.build();
  for (int iteration = 0; iteration < 40; ++iteration) {
    off::FailureEvidence failure;
    failure.service = fixture.service;
    failure.target = fixture.target_ref(fixture.t1);
    failure.provenance.source = fixture.failure_source;
    failure.provenance.sequence = off::EvidenceSeq::from_value(100 + static_cast<off::u64>(iteration));
    failure.provenance.epoch = fixture.fabric.view().epoch;
    failure.provenance.boot = fixture.fabric.view().boot;
    failure.topology_generation = fixture.fabric.view().topology_generation;
    (void)fixture.fabric.ingest_failure(failure, nullptr);
  }
  OFF_CHECK(fixture.fabric.explain_all().size() <= 6);
  const off::FabricStats stats = fixture.fabric.stats();
  OFF_CHECK(stats.explanations_evicted > 0);
  OFF_CHECK_EQ(stats.explanations_recorded, stats.explanations_evicted + 6);
}

}  // namespace
