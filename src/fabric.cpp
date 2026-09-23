// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "off/fabric.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <utility>

#include "detail/codec_util.hpp"

namespace off {
namespace detail {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

struct WitnessKey {
  ServiceName service{};
  TargetName target{};

  friend bool operator==(const WitnessKey&, const WitnessKey&) noexcept = default;
  friend auto operator<=>(const WitnessKey&, const WitnessKey&) noexcept = default;
};

/// Records that a target held a placement at a durable fence token. The pair
/// (token, incarnation) is what makes "this target may not resume" decidable
/// after a restart: a target only becomes usable again once its incarnation has
/// advanced past the incarnation that was fenced.
struct FenceWitness {
  FenceToken token{};
  TargetIncarnation incarnation{};
  TopologyGeneration topology_generation{};

  friend bool operator==(const FenceWitness&, const FenceWitness&) noexcept = default;
};

struct SourceWatermark {
  EvidenceSeq sequence{};
  Digest digest{};
  LogicalTick tick{};

  friend bool operator==(const SourceWatermark&, const SourceWatermark&) noexcept = default;
};

struct CapabilitySlot {
  CapabilityEvidence evidence{};
  /// Set on restart. Persisted dynamic evidence never silently becomes current.
  bool requires_reconfirmation{true};
};

struct DependencySlot {
  DependencyObservation observation{};
  bool requires_reconfirmation{true};
};

enum class RequestKind : u8 {
  Failover = 0,
  AmbiguityResolution = 1,
  Failback = 2,
  Resume = 3,
  Effect = 4,
  IntentAck = 5,
  Placement = 6,
};

struct RequestCacheEntry {
  RequestKind kind{RequestKind::Failover};
  Reason outcome{Reason::Ok};
  bool accepted{false};
  bool duplicate{false};
  AttemptId attempt{};
  FailoverGeneration generation{};
  FenceToken fence{};
  LeaseTerm lease_term{};
  PolicyGeneration policy_generation{};
  TopologyGeneration topology_generation{};
  TargetRef from{};
  TargetRef to{};
  bool has_intent{false};
  u32 plan_step_mask{0};
  Digest response_digest{};
  LogicalTick recorded{};
};

struct ServiceRuntime {
  ServiceDescriptor descriptor{};
  LifecyclePhase lifecycle{LifecyclePhase::Registered};
  RecoveryPhase recovery{RecoveryPhase::None};
  ContinuityClass continuity{ContinuityClass::Unknown};
  std::optional<TargetRef> active{};
  std::optional<TargetRef> fallback{};
  FailoverGeneration generation{};
  FenceToken fence{};
  std::optional<LeaseRecord> lease{};
  u64 next_attempt{1};
  AttemptId current_attempt{};
  std::deque<AttemptRecord> history{};
  std::deque<EffectId> effects_seen{};
  u64 attempts_evicted{0};
  /// Recovery attempts started for this service since the last verified effect.
  /// The policy ceiling bounds unbounded retry loops.
  u32 attempts_since_verified{0};
  bool activation_accepted{false};
  bool effect_verified{false};
  bool state_transfer_verified{false};
  bool failover_recorded{false};
  bool ambiguity_pending{false};
  EvidenceId pending_ambiguity{};
  std::optional<TargetRef> pre_failover_target{};
  CapabilityGeneration pre_failover_capability{};
  TopologyGeneration pre_failover_topology{};
  PolicyGeneration pre_failover_policy{};
  LogicalTick last_change{};
  Reason last_reason{Reason::Ok};

  [[nodiscard]] const AttemptRecord* current_attempt_record() const {
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
      if (it->id == current_attempt) {
        return &(*it);
      }
    }
    return nullptr;
  }

  [[nodiscard]] AttemptRecord* current_attempt_record() {
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
      if (it->id == current_attempt) {
        return &(*it);
      }
    }
    return nullptr;
  }
};

struct State {
  CoordinatorEpoch epoch{};
  BootId boot{};
  LogicalTick tick{};
  TopologyGeneration topology_generation{};
  PolicyGeneration policy_generation{};
  PolicyDescriptor policy{};
  u64 next_evidence_id{1};
  u64 next_attempt_id{1};
  u64 next_lease_id{1};
  u64 next_plan_id{1};

  std::map<TargetName, TargetRecord> targets{};
  std::map<TargetName, CapabilitySlot> capabilities{};
  std::map<ServiceName, ServiceRuntime> services{};
  std::map<ServiceName, std::map<ServiceName, DependencySlot>> dependencies{};
  std::map<EvidenceId, FailureEvidence> failures{};
  std::deque<EvidenceId> failure_order{};
  std::map<ServiceName, EvidenceId> latest_failure{};
  std::map<ServiceName, FenceRecord> fences{};
  std::map<WitnessKey, FenceWitness> witnesses{};
  std::map<SourceName, SourceRole> sources{};
  /// Per-source high-water mark. A record whose sequence is not strictly
  /// greater than the watermark is a replay and is refused.
  std::map<SourceName, SourceWatermark> watermarks{};
  std::map<RequestKey, RequestCacheEntry> request_cache{};
  std::deque<RequestKey> request_order{};
  FabricStats stats{};
};

// ---------------------------------------------------------------------------
// Cancellation registry
//
// The registry has its own mutex and is never acquired while holding the state
// mutex in the opposite order: the state lock is always taken first. A
// cancellation recorded before the commit check aborts the operation; a
// cancellation recorded after the durable commit is reported as too late and
// the operation still reports its committed outcome.
// ---------------------------------------------------------------------------

enum class CancelPhase : u8 { Active = 0, Cancelled = 1, Committed = 2 };

class CancelRegistry {
 public:
  bool begin(RequestKey key, std::size_t capacity, bool& capacity_exceeded) {
    std::lock_guard<std::mutex> guard(mutex_);
    capacity_exceeded = false;
    if (key.is_nil()) {
      return false;
    }
    capacity_ = capacity == 0 ? 1 : capacity;
    if (requests_.find(key) != requests_.end()) {
      return false;
    }
    if (requests_.size() >= capacity_) {
      trim();
    }
    if (requests_.size() >= capacity_) {
      capacity_exceeded = true;
      return false;
    }
    requests_.emplace(key, CancelPhase::Active);
    order_.push_back(key);
    return true;
  }

  /// Drops an entry that never reached a commit. A refused decision must not
  /// keep its key reserved, otherwise a retry would be reported as a protocol
  /// violation instead of replaying the recorded decision.
  void release_if_active(RequestKey key) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = requests_.find(key);
    if (it != requests_.end() && it->second == CancelPhase::Active) {
      requests_.erase(it);
      std::erase(order_, key);
    }
  }

  [[nodiscard]] bool is_cancelled(RequestKey key) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = requests_.find(key);
    return it != requests_.end() && it->second == CancelPhase::Cancelled;
  }

  /// Marks the request committed. Returns true when a cancellation had already
  /// been recorded, in which case the caller committed anyway and the
  /// cancellation is reported as too late.
  bool mark_committed(RequestKey key) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = requests_.find(key);
    if (it == requests_.end()) {
      return false;
    }
    const bool was_cancelled = it->second == CancelPhase::Cancelled;
    it->second = CancelPhase::Committed;
    return was_cancelled;
  }

  void finish(RequestKey key) {
    std::lock_guard<std::mutex> guard(mutex_);
    requests_.erase(key);
  }

  /// Returns true when the cancellation was recorded. Sets too_late when the
  /// request had already committed, and unknown when no such request is known.
  bool cancel(RequestKey key, bool& too_late, bool& unknown) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = requests_.find(key);
    if (it == requests_.end()) {
      unknown = true;
      return false;
    }
    if (it->second == CancelPhase::Committed) {
      too_late = true;
      return false;
    }
    it->second = CancelPhase::Cancelled;
    return true;
  }

  /// Held across the durable commit so a cancellation either lands before the
  /// commit point or is reported as too late. The registry lock is the innermost
  /// lock in the runtime: nothing acquired while it is held may take a lock that
  /// is ever taken before it.
  class Guard {
   public:
    Guard(CancelRegistry& registry, RequestKey key)
        : registry_(registry), lock_(registry.mutex_), key_(key) {
      const auto it = registry_.requests_.find(key_);
      if (it != registry_.requests_.end()) {
        known_ = true;
        cancelled_ = it->second == CancelPhase::Cancelled;
      }
    }

    [[nodiscard]] bool known() const noexcept { return known_; }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }

    void mark_committed() {
      const auto it = registry_.requests_.find(key_);
      if (it != registry_.requests_.end()) {
        it->second = CancelPhase::Committed;
      }
    }

   private:
    CancelRegistry& registry_;
    std::unique_lock<std::mutex> lock_;
    RequestKey key_{};
    bool known_{false};
    bool cancelled_{false};
  };

 private:
  /// Evicts settled entries in arrival order. Entries that are still active are
  /// left alone; they are released by release_if_active when their operation
  /// finishes without committing.
  void trim() {
    while (!order_.empty() && requests_.size() >= capacity_) {
      const RequestKey oldest = order_.front();
      order_.pop_front();
      const auto it = requests_.find(oldest);
      if (it != requests_.end() && it->second != CancelPhase::Active) {
        requests_.erase(it);
      }
    }
  }

  mutable std::mutex mutex_{};
  std::map<RequestKey, CancelPhase> requests_{};
  std::deque<RequestKey> order_{};
  std::size_t capacity_{1};
};

// ---------------------------------------------------------------------------
// Bounded intent queue
// ---------------------------------------------------------------------------

class IntentQueue {
 public:
  explicit IntentQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  bool push(const FailoverIntent& intent) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (closed_ || queue_.size() >= capacity_) {
      return false;
    }
    queue_.push_back(intent);
    ready_.notify_all();
    return true;
  }

  bool try_pop(FailoverIntent& out) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (queue_.empty()) {
      return false;
    }
    out = queue_.front();
    queue_.pop_front();
    return true;
  }

  bool wait_pop(FailoverIntent& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;
    }
    out = queue_.front();
    queue_.pop_front();
    return true;
  }

  [[nodiscard]] std::vector<FailoverIntent> snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return std::vector<FailoverIntent>(queue_.begin(), queue_.end());
  }

  [[nodiscard]] std::size_t depth() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return queue_.size();
  }

  void close() {
    std::lock_guard<std::mutex> guard(mutex_);
    closed_ = true;
    ready_.notify_all();
  }

  void reopen() {
    std::lock_guard<std::mutex> guard(mutex_);
    closed_ = false;
  }

 private:
  mutable std::mutex mutex_{};
  std::condition_variable ready_{};
  std::deque<FailoverIntent> queue_{};
  std::size_t capacity_{1};
  bool closed_{false};
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

[[nodiscard]] AttemptId make_attempt_id(u64 raw) { return AttemptId::from_value(raw); }
[[nodiscard]] EvidenceId make_evidence_id(u64 raw) { return EvidenceId::from_value(raw); }
[[nodiscard]] LeaseId make_lease_id(u64 raw) { return LeaseId::from_value(raw); }
[[nodiscard]] PlanId make_plan_id(u64 raw) { return PlanId::from_value(raw); }

[[nodiscard]] bool enum_valid(u64 raw, std::size_t count) noexcept {
  return raw < static_cast<u64>(count);
}

void encode_string_list(CanonicalWriter& writer, const std::vector<std::string>& items) {
  if (!writer.put_count(items.size())) {
    return;
  }
  for (const std::string& item : items) {
    writer.put_text(item);
  }
}

[[nodiscard]] bool decode_string_list(CanonicalReader& reader, std::vector<std::string>& items) {
  std::size_t count = 0;
  if (!reader.get_count(4, count)) {
    return false;
  }
  items.clear();
  items.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    std::string item;
    if (!reader.get_text(item)) {
      return false;
    }
    items.push_back(std::move(item));
  }
  return true;
}

void encode_request_key(CanonicalWriter& writer, const RequestKey& key) {
  writer.put_u64(key.value());
}

bool decode_request_key(CanonicalReader& reader, RequestKey& key) {
  u64 raw = 0;
  if (!reader.get_u64(raw)) {
    return false;
  }
  key = RequestKey::from_value(raw);
  return true;
}

void encode_evidence_id(CanonicalWriter& writer, const EvidenceId& id) { writer.put_u64(id.value()); }

bool decode_evidence_id(CanonicalReader& reader, EvidenceId& id) {
  u64 raw = 0;
  if (!reader.get_u64(raw)) {
    return false;
  }
  id = EvidenceId::from_value(raw);
  return true;
}

[[nodiscard]] JsonValue target_ref_json_or_null(const std::optional<TargetRef>& value) {
  if (!value.has_value()) {
    return JsonValue(nullptr);
  }
  return to_json(*value);
}

// ---------------------------------------------------------------------------
// Attempt history codec
// ---------------------------------------------------------------------------

void encode_attempt_history(CanonicalWriter& writer, const std::deque<AttemptRecord>& history) {
  if (!writer.put_count(history.size())) {
    return;
  }
  for (const AttemptRecord& record : history) {
    encode(writer, record);
  }
}

bool decode_attempt_history(CanonicalReader& reader, std::deque<AttemptRecord>& history,
                            std::size_t ceiling) {
  std::size_t count = 0;
  if (!reader.get_count(1, count)) {
    return false;
  }
  if (count > ceiling) {
    reader.fail(Reason::Oversized);
    return false;
  }
  history.clear();
  for (std::size_t index = 0; index < count; ++index) {
    AttemptRecord record;
    if (!decode(reader, record)) {
      return false;
    }
    history.push_back(record);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Service runtime codec
// ---------------------------------------------------------------------------

void encode_service_runtime(CanonicalWriter& writer, const ServiceRuntime& runtime) {
  encode(writer, runtime.descriptor);
  writer.put_u8(static_cast<u8>(runtime.lifecycle));
  writer.put_u8(static_cast<u8>(runtime.recovery));
  writer.put_u8(static_cast<u8>(runtime.continuity));
  if (runtime.active.has_value()) {
    writer.put_presence();
    encode(writer, *runtime.active);
  } else {
    writer.put_absence();
  }
  if (runtime.fallback.has_value()) {
    writer.put_presence();
    encode(writer, *runtime.fallback);
  } else {
    writer.put_absence();
  }
  writer.put_u64(runtime.generation.value());
  writer.put_u64(runtime.fence.value());
  if (runtime.lease.has_value()) {
    writer.put_presence();
    encode(writer, *runtime.lease);
  } else {
    writer.put_absence();
  }
  writer.put_u64(runtime.next_attempt);
  writer.put_u64(runtime.current_attempt.value());
  encode_attempt_history(writer, runtime.history);
  if (writer.put_count(runtime.effects_seen.size())) {
    for (const EffectId& id : runtime.effects_seen) {
      writer.put_u64(id.value());
    }
  }
  writer.put_u64(runtime.attempts_evicted);
  writer.put_u32(runtime.attempts_since_verified);
  writer.put_bool(runtime.activation_accepted);
  writer.put_bool(runtime.effect_verified);
  writer.put_bool(runtime.state_transfer_verified);
  writer.put_bool(runtime.failover_recorded);
  writer.put_bool(runtime.ambiguity_pending);
  writer.put_u64(runtime.pending_ambiguity.value());
  if (runtime.pre_failover_target.has_value()) {
    writer.put_presence();
    encode(writer, *runtime.pre_failover_target);
  } else {
    writer.put_absence();
  }
  writer.put_u64(runtime.pre_failover_capability.value());
  writer.put_u64(runtime.pre_failover_topology.value());
  writer.put_u64(runtime.pre_failover_policy.value());
  writer.put_u64(runtime.last_change.value());
  writer.put_u16(static_cast<u16>(runtime.last_reason));
}

bool decode_optional_target_ref(CanonicalReader& reader, std::optional<TargetRef>& out) {
  bool present = false;
  if (!reader.get_presence(present)) {
    return false;
  }
  if (!present) {
    out.reset();
    return true;
  }
  TargetRef value;
  if (!decode(reader, value)) {
    return false;
  }
  out = value;
  return true;
}

bool decode_service_runtime(CanonicalReader& reader, ServiceRuntime& runtime,
                            const FabricConfig& config) {
  if (!decode(reader, runtime.descriptor)) {
    return false;
  }
  u8 lifecycle = 0;
  u8 recovery = 0;
  u8 continuity = 0;
  if (!reader.get_u8(lifecycle) || !reader.get_u8(recovery) || !reader.get_u8(continuity)) {
    return false;
  }
  if (!enum_valid(lifecycle, kLifecyclePhaseCount) || !enum_valid(recovery, kRecoveryPhaseCount) ||
      !enum_valid(continuity, kContinuityClassCount)) {
    reader.fail(Reason::UnsupportedSemantics);
    return false;
  }
  runtime.lifecycle = static_cast<LifecyclePhase>(lifecycle);
  runtime.recovery = static_cast<RecoveryPhase>(recovery);
  runtime.continuity = static_cast<ContinuityClass>(continuity);
  if (!decode_optional_target_ref(reader, runtime.active) ||
      !decode_optional_target_ref(reader, runtime.fallback)) {
    return false;
  }
  u64 generation = 0;
  u64 fence = 0;
  if (!reader.get_u64(generation) || !reader.get_u64(fence)) {
    return false;
  }
  runtime.generation = FailoverGeneration::from_value(generation);
  runtime.fence = FenceToken::from_value(fence);
  bool has_lease = false;
  if (!reader.get_presence(has_lease)) {
    return false;
  }
  if (has_lease) {
    LeaseRecord lease;
    if (!decode(reader, lease)) {
      return false;
    }
    runtime.lease = lease;
  } else {
    runtime.lease.reset();
  }
  u64 next_attempt = 0;
  u64 current_attempt = 0;
  if (!reader.get_u64(next_attempt) || !reader.get_u64(current_attempt)) {
    return false;
  }
  runtime.next_attempt = next_attempt;
  runtime.current_attempt = make_attempt_id(current_attempt);
  if (!decode_attempt_history(reader, runtime.history, config.max_attempt_history)) {
    return false;
  }
  std::size_t effect_count = 0;
  if (!reader.get_count(8, effect_count) || effect_count > config.max_attempt_history) {
    reader.fail(Reason::Oversized);
    return false;
  }
  runtime.effects_seen.clear();
  for (std::size_t index = 0; index < effect_count; ++index) {
    u64 raw = 0;
    if (!reader.get_u64(raw)) {
      return false;
    }
    runtime.effects_seen.push_back(EffectId::from_value(raw));
  }
  if (!reader.get_u64(runtime.attempts_evicted) ||
      !reader.get_u32(runtime.attempts_since_verified)) {
    return false;
  }
  if (!reader.get_bool(runtime.activation_accepted) || !reader.get_bool(runtime.effect_verified) ||
      !reader.get_bool(runtime.state_transfer_verified) ||
      !reader.get_bool(runtime.failover_recorded) || !reader.get_bool(runtime.ambiguity_pending)) {
    return false;
  }
  u64 pending_ambiguity = 0;
  if (!reader.get_u64(pending_ambiguity)) {
    return false;
  }
  runtime.pending_ambiguity = make_evidence_id(pending_ambiguity);
  if (!decode_optional_target_ref(reader, runtime.pre_failover_target)) {
    return false;
  }
  u64 pre_capability = 0;
  u64 pre_topology = 0;
  u64 pre_policy = 0;
  if (!reader.get_u64(pre_capability) || !reader.get_u64(pre_topology) ||
      !reader.get_u64(pre_policy)) {
    return false;
  }
  runtime.pre_failover_capability = CapabilityGeneration::from_value(pre_capability);
  runtime.pre_failover_topology = TopologyGeneration::from_value(pre_topology);
  runtime.pre_failover_policy = PolicyGeneration::from_value(pre_policy);
  u64 last_change = 0;
  if (!reader.get_u64(last_change)) {
    return false;
  }
  runtime.last_change = LogicalTick::from_value(last_change);
  return decode_reason(reader, runtime.last_reason);
}

// ---------------------------------------------------------------------------
// Stats codec
// ---------------------------------------------------------------------------

#define OFF_STATS_FIELDS(X)     \
  X(services_registered)        \
  X(targets_registered)         \
  X(sources_enrolled)           \
  X(capabilities_ingested)      \
  X(failures_ingested)          \
  X(dependencies_ingested)      \
  X(evidence_rejected_stale)    \
  X(evidence_rejected_conflict) \
  X(evidence_rejected_duplicate) \
  X(evidence_rejected_unauthorized) \
  X(evidence_rejected_capacity) \
  X(evidence_evicted)           \
  X(placements_established)     \
  X(failovers_accepted)         \
  X(failovers_refused)          \
  X(failovers_duplicate)        \
  X(failbacks_accepted)         \
  X(failbacks_refused)          \
  X(effects_accepted)           \
  X(effects_refused)            \
  X(effects_verified)           \
  X(ambiguity_pending)          \
  X(ambiguity_resolved)         \
  X(intents_emitted)            \
  X(intents_dropped)            \
  X(intents_consumed)           \
  X(attempts_recorded)          \
  X(attempts_evicted)           \
  X(explanations_recorded)      \
  X(explanations_evicted)       \
  X(fallback_candidates_truncated) \
  X(dependency_depth_truncated) \
  X(requests_cancelled)         \
  X(cancels_too_late)           \
  X(journal_transactions)       \
  X(journal_compactions)        \
  X(recovery_reissues)          \
  X(effect_reports_replayed)    \
  X(withdrawn_services)

void encode_stats(CanonicalWriter& writer, const FabricStats& stats) {
#define OFF_ENCODE_STAT(field) writer.put_u64(stats.field);
  OFF_STATS_FIELDS(OFF_ENCODE_STAT)
#undef OFF_ENCODE_STAT
}

bool decode_stats(CanonicalReader& reader, FabricStats& stats) {
#define OFF_DECODE_STAT(field)                  \
  if (!reader.get_u64(stats.field)) {           \
    return false;                               \
  }
  OFF_STATS_FIELDS(OFF_DECODE_STAT)
#undef OFF_DECODE_STAT
  return true;
}

// ---------------------------------------------------------------------------
// Full-state snapshot codec
// ---------------------------------------------------------------------------

void encode_state(CanonicalWriter& writer, const State& state) {
  writer.put_u16(kCanonicalSchemaVersion);
  writer.put_u64(state.epoch.value());
  writer.put_u64(state.boot.value());
  writer.put_u64(state.tick.value());
  writer.put_u64(state.topology_generation.value());
  writer.put_u64(state.policy_generation.value());
  writer.put_u64(state.next_evidence_id);
  writer.put_u64(state.next_attempt_id);
  writer.put_u64(state.next_lease_id);
  writer.put_u64(state.next_plan_id);
  encode(writer, state.policy);

  if (writer.put_count(state.targets.size())) {
    for (const auto& entry : state.targets) {
      encode(writer, entry.second);
    }
  }
  if (writer.put_count(state.capabilities.size())) {
    for (const auto& entry : state.capabilities) {
      encode_name(writer, entry.first);
      encode(writer, entry.second.evidence);
      writer.put_bool(entry.second.requires_reconfirmation);
    }
  }
  if (writer.put_count(state.services.size())) {
    for (const auto& entry : state.services) {
      encode_name(writer, entry.first);
      encode_service_runtime(writer, entry.second);
    }
  }
  if (writer.put_count(state.dependencies.size())) {
    for (const auto& service_entry : state.dependencies) {
      encode_name(writer, service_entry.first);
      if (!writer.put_count(service_entry.second.size())) {
        return;
      }
      for (const auto& dep_entry : service_entry.second) {
        encode_name(writer, dep_entry.first);
        encode(writer, dep_entry.second.observation);
        writer.put_bool(dep_entry.second.requires_reconfirmation);
      }
    }
  }
  if (writer.put_count(state.failures.size())) {
    for (const auto& entry : state.failures) {
      encode(writer, entry.second);
    }
  }
  if (writer.put_count(state.fences.size())) {
    for (const auto& entry : state.fences) {
      encode(writer, entry.second);
    }
  }
  if (writer.put_count(state.witnesses.size())) {
    for (const auto& entry : state.witnesses) {
      encode_name(writer, entry.first.service);
      encode_name(writer, entry.first.target);
      encode_gen(writer, entry.second.token);
      encode(writer, entry.second.incarnation);
      encode_gen(writer, entry.second.topology_generation);
    }
  }
  if (writer.put_count(state.sources.size())) {
    for (const auto& entry : state.sources) {
      encode_name(writer, entry.first);
      writer.put_u8(static_cast<u8>(entry.second));
    }
  }
  if (writer.put_count(state.watermarks.size())) {
    for (const auto& entry : state.watermarks) {
      encode_name(writer, entry.first);
      encode_gen(writer, entry.second.sequence);
      encode_digest(writer, entry.second.digest);
      encode_tick(writer, entry.second.tick);
    }
  }
  if (writer.put_count(state.request_cache.size())) {
    for (const auto& entry : state.request_cache) {
      encode_request_key(writer, entry.first);
      writer.put_u8(static_cast<u8>(entry.second.kind));
      writer.put_u16(static_cast<u16>(entry.second.outcome));
      writer.put_bool(entry.second.accepted);
      writer.put_bool(entry.second.duplicate);
      writer.put_u64(entry.second.attempt.value());
      writer.put_u64(entry.second.generation.value());
      writer.put_u64(entry.second.fence.value());
      writer.put_u64(entry.second.lease_term.value());
      writer.put_u64(entry.second.policy_generation.value());
      writer.put_u64(entry.second.topology_generation.value());
      encode(writer, entry.second.from);
      encode(writer, entry.second.to);
      writer.put_bool(entry.second.has_intent);
      writer.put_u32(entry.second.plan_step_mask);
      encode_digest(writer, entry.second.response_digest);
      writer.put_u64(entry.second.recorded.value());
    }
  }
  encode_stats(writer, state.stats);
}

bool decode_state(CanonicalReader& reader, State& state, const FabricConfig& config) {
  u16 schema = 0;
  if (!reader.get_u16(schema)) {
    return false;
  }
  if (schema != kCanonicalSchemaVersion) {
    reader.fail(Reason::StoreSemanticsIncompatible);
    return false;
  }
  u64 epoch = 0;
  u64 boot = 0;
  u64 tick = 0;
  u64 topology = 0;
  u64 policy_generation = 0;
  if (!reader.get_u64(epoch) || !reader.get_u64(boot) || !reader.get_u64(tick) ||
      !reader.get_u64(topology) || !reader.get_u64(policy_generation)) {
    return false;
  }
  state.epoch = CoordinatorEpoch::from_value(epoch);
  state.boot = BootId::from_value(boot);
  state.tick = LogicalTick::from_value(tick);
  state.topology_generation = TopologyGeneration::from_value(topology);
  state.policy_generation = PolicyGeneration::from_value(policy_generation);
  if (!reader.get_u64(state.next_evidence_id) || !reader.get_u64(state.next_attempt_id) ||
      !reader.get_u64(state.next_lease_id) || !reader.get_u64(state.next_plan_id)) {
    return false;
  }
  if (state.next_evidence_id == 0 || state.next_attempt_id == 0 || state.next_lease_id == 0 ||
      state.next_plan_id == 0) {
    reader.fail(Reason::ImpossibleValue);
    return false;
  }
  if (!decode(reader, state.policy)) {
    return false;
  }

  std::size_t count = 0;
  if (!reader.get_count(1, count) || count > config.max_targets) {
    reader.fail(Reason::RegistryCapacityExceeded);
    return false;
  }
  if (!reader.push_depth()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    TargetRecord record;
    if (!decode(reader, record)) {
      reader.pop_depth();
      return false;
    }
    if (record.name.empty() || !state.targets.emplace(record.name, record).second) {
      reader.fail(Reason::DuplicateIdentity);
      reader.pop_depth();
      return false;
    }
  }
  reader.pop_depth();

  if (!reader.get_count(1, count) || count > config.max_targets) {
    reader.fail(Reason::RegistryCapacityExceeded);
    return false;
  }
  if (!reader.push_depth()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    CapabilitySlot slot;
    TargetName target;
    if (!decode_name(reader, target) || !decode(reader, slot.evidence) ||
        !reader.get_bool(slot.requires_reconfirmation)) {
      reader.pop_depth();
      return false;
    }
    if (!state.capabilities.emplace(target, slot).second) {
      reader.fail(Reason::DuplicateIdentity);
      reader.pop_depth();
      return false;
    }
  }
  reader.pop_depth();

  if (!reader.get_count(1, count) || count > config.max_services) {
    reader.fail(Reason::RegistryCapacityExceeded);
    return false;
  }
  if (!reader.push_depth()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    ServiceName name;
    ServiceRuntime runtime;
    if (!decode_name(reader, name) || !decode_service_runtime(reader, runtime, config)) {
      reader.pop_depth();
      return false;
    }
    runtime.descriptor.name = name;
    if (!state.services.emplace(name, std::move(runtime)).second) {
      reader.fail(Reason::DuplicateIdentity);
      reader.pop_depth();
      return false;
    }
  }
  reader.pop_depth();

  if (!reader.get_count(1, count) || count > config.max_services) {
    reader.fail(Reason::RegistryCapacityExceeded);
    return false;
  }
  if (!reader.push_depth()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    ServiceName service;
    if (!decode_name(reader, service)) {
      reader.pop_depth();
      return false;
    }
    std::size_t dep_count = 0;
    if (!reader.get_count(1, dep_count) || dep_count > config.max_dependencies_per_service) {
      reader.fail(Reason::DependencyCapacityExceeded);
      reader.pop_depth();
      return false;
    }
    auto& bucket = state.dependencies[service];
    for (std::size_t dep_index = 0; dep_index < dep_count; ++dep_index) {
      ServiceName dependency;
      DependencySlot slot;
      if (!decode_name(reader, dependency) || !decode(reader, slot.observation) ||
          !reader.get_bool(slot.requires_reconfirmation)) {
        reader.pop_depth();
        return false;
      }
      if (!bucket.emplace(dependency, slot).second) {
        reader.fail(Reason::DuplicateIdentity);
        reader.pop_depth();
        return false;
      }
    }
  }
  reader.pop_depth();

  if (!reader.get_count(1, count) || count > config.max_evidence_records) {
    reader.fail(Reason::EvidenceCapacityExceeded);
    return false;
  }
  if (!reader.push_depth()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    FailureEvidence evidence;
    if (!decode(reader, evidence)) {
      reader.pop_depth();
      return false;
    }
    if (evidence.id.is_nil() || !state.failures.emplace(evidence.id, evidence).second) {
      reader.fail(Reason::DuplicateIdentity);
      reader.pop_depth();
      return false;
    }
    state.failure_order.push_back(evidence.id);
  }
  reader.pop_depth();

  if (!reader.get_count(1, count) || count > config.max_services) {
    reader.fail(Reason::RegistryCapacityExceeded);
    return false;
  }
  if (!reader.push_depth()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    FenceRecord fence;
    if (!decode(reader, fence)) {
      reader.pop_depth();
      return false;
    }
    if (!state.fences.emplace(fence.service, fence).second) {
      reader.fail(Reason::DuplicateIdentity);
      reader.pop_depth();
      return false;
    }
  }
  reader.pop_depth();

  u64 witness_ceiling = 0;
  if (mul_overflow(static_cast<u64>(config.max_targets),
                   static_cast<u64>(config.max_services), witness_ceiling)) {
    witness_ceiling = (std::numeric_limits<u64>::max)();
  }
  if (!reader.get_count(1, count) || static_cast<u64>(count) > witness_ceiling) {
    reader.fail(Reason::RegistryCapacityExceeded);
    return false;
  }
  if (!reader.push_depth()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    WitnessKey key;
    FenceWitness witness;
    if (!decode_name(reader, key.service) || !decode_name(reader, key.target) ||
        !decode_gen(reader, witness.token) || !decode(reader, witness.incarnation) ||
        !decode_gen(reader, witness.topology_generation)) {
      reader.pop_depth();
      return false;
    }
    if (!state.witnesses.emplace(key, witness).second) {
      reader.fail(Reason::DuplicateIdentity);
      reader.pop_depth();
      return false;
    }
  }
  reader.pop_depth();

  if (!reader.get_count(1, count) || count > config.max_sources) {
    reader.fail(Reason::RegistryCapacityExceeded);
    return false;
  }
  if (!reader.push_depth()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    SourceName source;
    u8 role = 0;
    if (!decode_name(reader, source) || !reader.get_u8(role)) {
      reader.pop_depth();
      return false;
    }
    if (!enum_valid(role, kSourceRoleCount)) {
      reader.fail(Reason::UnsupportedSemantics);
      reader.pop_depth();
      return false;
    }
    if (!state.sources.emplace(source, static_cast<SourceRole>(role)).second) {
      reader.fail(Reason::DuplicateIdentity);
      reader.pop_depth();
      return false;
    }
  }
  reader.pop_depth();

  if (!reader.get_count(1, count) || count > config.max_sources) {
    reader.fail(Reason::RegistryCapacityExceeded);
    return false;
  }
  if (!reader.push_depth()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    SourceName source;
    SourceWatermark watermark;
    if (!decode_name(reader, source) || !decode_gen(reader, watermark.sequence) ||
        !decode_digest(reader, watermark.digest) || !decode_tick(reader, watermark.tick)) {
      reader.pop_depth();
      return false;
    }
    if (!state.watermarks.emplace(source, watermark).second) {
      reader.fail(Reason::DuplicateIdentity);
      reader.pop_depth();
      return false;
    }
  }
  reader.pop_depth();

  if (!reader.get_count(1, count) || count > config.max_request_cache) {
    reader.fail(Reason::RegistryCapacityExceeded);
    return false;
  }
  if (!reader.push_depth()) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    RequestKey key;
    RequestCacheEntry entry;
    u8 kind = 0;
    u16 outcome = 0;
    if (!decode_request_key(reader, key) || !reader.get_u8(kind) || !reader.get_u16(outcome)) {
      reader.pop_depth();
      return false;
    }
    if (!enum_valid(kind, 7)) {
      reader.fail(Reason::UnsupportedSemantics);
      reader.pop_depth();
      return false;
    }
    if (reason_code_text(static_cast<Reason>(outcome)) == "UNRECOGNIZED_REASON") {
      reader.fail(Reason::UnsupportedSemantics);
      reader.pop_depth();
      return false;
    }
    entry.kind = static_cast<RequestKind>(kind);
    entry.outcome = static_cast<Reason>(outcome);
    u64 attempt_raw = 0;
    u64 generation_raw = 0;
    u64 fence_raw = 0;
    u64 lease_raw = 0;
    u64 policy_raw = 0;
    u64 topology_raw = 0;
    u64 recorded_raw = 0;
    if (!reader.get_bool(entry.accepted) || !reader.get_bool(entry.duplicate) ||
        !reader.get_u64(attempt_raw) || !reader.get_u64(generation_raw) ||
        !reader.get_u64(fence_raw) || !reader.get_u64(lease_raw) ||
        !reader.get_u64(policy_raw) || !reader.get_u64(topology_raw) ||
        !decode(reader, entry.from) || !decode(reader, entry.to) ||
        !reader.get_bool(entry.has_intent) || !reader.get_u32(entry.plan_step_mask) ||
        !reader.get_u64(entry.response_digest.hi) || !reader.get_u64(entry.response_digest.lo) ||
        !reader.get_u64(recorded_raw)) {
      reader.pop_depth();
      return false;
    }
    entry.attempt = make_attempt_id(attempt_raw);
    entry.generation = FailoverGeneration::from_value(generation_raw);
    entry.fence = FenceToken::from_value(fence_raw);
    entry.lease_term = LeaseTerm::from_value(lease_raw);
    entry.policy_generation = PolicyGeneration::from_value(policy_raw);
    entry.topology_generation = TopologyGeneration::from_value(topology_raw);
    entry.recorded = LogicalTick::from_value(recorded_raw);
    if (!state.request_cache.emplace(key, entry).second) {
      reader.fail(Reason::DuplicateIdentity);
      reader.pop_depth();
      return false;
    }
    state.request_order.push_back(key);
  }
  reader.pop_depth();

  return decode_stats(reader, state.stats);
}

}  // namespace detail

// The implementation types live in off::detail so they never collide with the
// public surface. This translation unit is the only place they are visible.
using namespace detail;

// ---------------------------------------------------------------------------
// Source roles
// ---------------------------------------------------------------------------

std::string_view source_role_text(SourceRole value) noexcept {
  switch (value) {
    case SourceRole::FailureReporter:
      return "failure_reporter";
    case SourceRole::CapabilityReporter:
      return "capability_reporter";
    case SourceRole::DependencyReporter:
      return "dependency_reporter";
    case SourceRole::EffectReporter:
      return "effect_reporter";
    case SourceRole::Operator:
      return "operator";
    case SourceRole::TopologyReporter:
      return "topology_reporter";
    default:
      return "unknown";
  }
}

bool source_role_parse(std::string_view text, SourceRole& out) noexcept {
  for (std::size_t index = 0; index < kSourceRoleCount; ++index) {
    const auto role = static_cast<SourceRole>(index);
    if (source_role_text(role) == text) {
      out = role;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Plan and decision rendering
// ---------------------------------------------------------------------------

std::string_view plan_step_kind_text(PlanStepKind value) noexcept {
  switch (value) {
    case PlanStepKind::AdvanceFence:
      return "advance_fence";
    case PlanStepKind::FencePreviousTarget:
      return "fence_previous_target";
    case PlanStepKind::TransferLease:
      return "transfer_lease";
    case PlanStepKind::EmitIntent:
      return "emit_intent";
    case PlanStepKind::AwaitActivationEffect:
      return "await_activation_effect";
    case PlanStepKind::AwaitStateTransfer:
      return "await_state_transfer";
    case PlanStepKind::AwaitServiceHealth:
      return "await_service_health";
    case PlanStepKind::ClaimContinuity:
      return "claim_continuity";
    default:
      return "unknown";
  }
}

bool plan_step_kind_parse(std::string_view text, PlanStepKind& out) noexcept {
  for (std::size_t index = 0; index < kPlanStepKindCount; ++index) {
    const auto kind = static_cast<PlanStepKind>(index);
    if (plan_step_kind_text(kind) == text) {
      out = kind;
      return true;
    }
  }
  return false;
}

JsonValue to_json(const PlanStepKind& value) {
  return JsonValue(std::string(plan_step_kind_text(value)));
}

JsonValue to_json(const PlanStep& value) {
  JsonValue object;
  object.set("kind", JsonValue(std::string(plan_step_kind_text(value.kind))));
  object.set("mandatory", JsonValue(value.mandatory));
  object.set("subject", JsonValue(value.subject));
  return object;
}

JsonValue RecoveryPlan::to_json() const {
  JsonValue step_array{JsonValue::Array{}};
  for (const PlanStep& step : this->steps) {
    step_array.push(off::to_json(step));
  }
  JsonValue object;
  object.set("fence", JsonValue(fence.value()));
  object.set("from", off::to_json(from));
  object.set("generation", JsonValue(generation.value()));
  object.set("id", JsonValue(id.value()));
  object.set("lease_term", JsonValue(lease_term.value()));
  object.set("omitted_steps", JsonValue(static_cast<u64>(omitted_steps)));
  object.set("service", JsonValue(service.str()));
  object.set("steps", std::move(step_array));
  object.set("to", off::to_json(to));
  return object;
}

JsonValue RecoveryDecision::to_json() const {
  JsonValue object;
  object.set("accepted", JsonValue(accepted));
  object.set("duplicate", JsonValue(duplicate));
  object.set("explanation", explanation.to_json());
  if (intent.has_value()) {
    object.set("intent", off::to_json(*intent));
  } else {
    object.set("intent", JsonValue(nullptr));
  }
  object.set("outcome", JsonValue(std::string(reason_code_text(outcome))));
  object.set("outcome_family", JsonValue(std::string(reason_family(outcome))));
  object.set("plan", plan.to_json());
  return object;
}

// ---------------------------------------------------------------------------
// Explanations
// ---------------------------------------------------------------------------

Digest Explanation::semantic_digest() const {
  CanonicalWriter writer;
  encode_name(writer, service);
  writer.put_u16(static_cast<u16>(outcome));
  writer.put_bool(accepted);
  encode_gen(writer, epoch);
  encode_gen(writer, policy_generation);
  encode_gen(writer, topology_generation);
  encode_gen(writer, failover_generation);
  encode_gen(writer, fence);
  writer.put_u64(request.value());
  if (writer.put_count(steps.size())) {
    for (const ExplanationStep& step : steps) {
      writer.put_u16(static_cast<u16>(step.code));
      writer.put_text(step.subject);
      writer.put_text(step.detail);
      writer.put_u64(step.value);
    }
  }
  return writer.digest();
}

std::string Explanation::render() const {
  std::string out;
  out += "decision ";
  out += accepted ? "ACCEPTED" : "REFUSED";
  out += " code=";
  out.append(reason_code_text(outcome));
  out += " service=";
  if (service.empty()) {
    out += "<none>";
  } else {
    out.append(service.view());
  }
  out += " epoch=";
  out.append(to_decimal(epoch.value()));
  out += " policy_gen=";
  out.append(to_decimal(policy_generation.value()));
  out += " topology_gen=";
  out.append(to_decimal(topology_generation.value()));
  out += " failover_gen=";
  out.append(to_decimal(failover_generation.value()));
  out += " fence=";
  out.append(to_decimal(fence.value()));
  out += '\n';
  for (std::size_t index = 0; index < steps.size(); ++index) {
    const ExplanationStep& step = steps[index];
    out += "  [";
    out.append(to_decimal(static_cast<u64>(index)));
    out += "] ";
    out.append(reason_code_text(step.code));
    if (!step.subject.empty()) {
      out += " subject=";
      out.append(step.subject);
    }
    if (step.value != 0) {
      out += " value=";
      out.append(to_decimal(step.value));
    }
    if (!step.detail.empty()) {
      out += " :: ";
      out.append(step.detail);
    }
    out += '\n';
  }
  return out;
}

JsonValue Explanation::to_json() const {
  JsonValue step_array{JsonValue::Array{}};
  for (const ExplanationStep& step : steps) {
    JsonValue item;
    item.set("code", JsonValue(std::string(reason_code_text(step.code))));
    item.set("detail", JsonValue(step.detail));
    item.set("family", JsonValue(std::string(reason_family(step.code))));
    item.set("subject", JsonValue(step.subject));
    item.set("value", JsonValue(step.value));
    step_array.push(std::move(item));
  }
  JsonValue object;
  object.set("accepted", JsonValue(accepted));
  object.set("epoch", JsonValue(epoch.value()));
  object.set("failover_generation", JsonValue(failover_generation.value()));
  object.set("fence", JsonValue(fence.value()));
  object.set("outcome", JsonValue(std::string(reason_code_text(outcome))));
  object.set("policy_generation", JsonValue(policy_generation.value()));
  object.set("request", JsonValue(request.value()));
  object.set("semantic_digest", JsonValue(semantic_digest().to_hex()));
  object.set("service", JsonValue(service.str()));
  object.set("steps", std::move(step_array));
  object.set("tick", JsonValue(tick.value()));
  object.set("topology_generation", JsonValue(topology_generation.value()));
  return object;
}

void ExplanationLog::record(Explanation explanation) {
  if (capacity_ == 0) {
    ++evicted_;
    return;
  }
  if (entries_.size() < capacity_) {
    entries_.push_back(std::move(explanation));
    return;
  }
  entries_[next_] = std::move(explanation);
  next_ = (next_ + 1) % capacity_;
  ++evicted_;
}

std::vector<Explanation> ExplanationLog::all() const {
  std::vector<Explanation> out;
  out.reserve(entries_.size());
  if (entries_.size() < capacity_) {
    out.assign(entries_.begin(), entries_.end());
    return out;
  }
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    out.push_back(entries_[(next_ + index) % entries_.size()]);
  }
  return out;
}

std::vector<Explanation> ExplanationLog::for_service(const ServiceName& service) const {
  std::vector<Explanation> out;
  for (const Explanation& entry : all()) {
    if (entry.service == service) {
      out.push_back(entry);
    }
  }
  return out;
}

void ExplanationLog::clear() noexcept {
  entries_.clear();
  next_ = 0;
  evicted_ = 0;
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------

JsonValue ServiceView::to_json() const {
  JsonValue object;
  object.set("active", target_ref_json_or_null(active));
  object.set("ambiguity_pending", JsonValue(ambiguity_pending));
  object.set("attempts_evicted", JsonValue(static_cast<u64>(attempts_evicted)));
  object.set("attempts_recorded", JsonValue(static_cast<u64>(attempts_recorded)));
  object.set("continuity", JsonValue(std::string(continuity_class_text(continuity))));
  object.set("current_attempt", JsonValue(current_attempt.value()));
  object.set("effect_verified", JsonValue(effect_verified));
  object.set("failover_generation", JsonValue(generation.value()));
  object.set("failover_recorded", JsonValue(failover_recorded));
  object.set("fallback", target_ref_json_or_null(fallback));
  object.set("fence", JsonValue(fence.value()));
  object.set("last_change", JsonValue(last_change.value()));
  object.set("last_outcome", JsonValue(std::string(attempt_outcome_text(last_outcome))));
  object.set("last_reason", JsonValue(std::string(reason_code_text(last_reason))));
  object.set("lease_term", JsonValue(lease_term.value()));
  object.set("lifecycle", JsonValue(std::string(lifecycle_phase_text(lifecycle))));
  object.set("name", JsonValue(name.str()));
  object.set("recovery", JsonValue(std::string(recovery_phase_text(recovery))));
  object.set("scope", JsonValue(scope.str()));
  object.set("state_transfer_verified", JsonValue(state_transfer_verified));
  object.set("statefulness", JsonValue(std::string(statefulness_text(statefulness))));
  return object;
}

JsonValue FabricStats::to_json() const {
  JsonValue object;
#define OFF_STAT_JSON(field) object.set(#field, JsonValue(field));
  OFF_STATS_FIELDS(OFF_STAT_JSON)
#undef OFF_STAT_JSON
  return object;
}

JsonValue RestartSummary::to_json() const {
  JsonValue object;
  object.set("attempts_marked_unknown", JsonValue(static_cast<u64>(attempts_marked_unknown)));
  object.set("boot", JsonValue(boot.value()));
  object.set("capability_evidence_invalidated",
             JsonValue(static_cast<u64>(capability_evidence_invalidated)));
  object.set("conservative", JsonValue(conservative));
  object.set("dependency_observations_invalidated",
             JsonValue(static_cast<u64>(dependency_observations_invalidated)));
  object.set("epoch", JsonValue(epoch.value()));
  object.set("failure_evidence_invalidated",
             JsonValue(static_cast<u64>(failure_evidence_invalidated)));
  object.set("previous_epoch", JsonValue(previous_epoch.value()));
  object.set("services_downgraded", JsonValue(static_cast<u64>(services_downgraded)));
  JsonValue store_object;
  store_object.set("bytes_accepted", JsonValue(store.bytes_accepted));
  store_object.set("bytes_discarded", JsonValue(store.bytes_discarded));
  store_object.set("classification",
                   JsonValue(std::string(recovery_class_text(store.classification))));
  store_object.set("format_version", JsonValue(static_cast<u64>(store.format_version)));
  store_object.set("journal_generation", JsonValue(static_cast<u64>(store.journal_generation)));
  store_object.set("reason", JsonValue(std::string(reason_code_text(store.reason))));
  store_object.set("replayed_digest", JsonValue(store.replayed_digest.to_hex()));
  store_object.set("snapshot_generation", JsonValue(static_cast<u64>(store.snapshot_generation)));
  store_object.set("transactions_applied", JsonValue(store.transactions_applied));
  store_object.set("transactions_dropped", JsonValue(store.transactions_dropped));
  store_object.set("truncated_tail_repaired", JsonValue(store.truncated_tail_repaired));
  object.set("store", std::move(store_object));
  return object;
}

JsonValue FabricView::semantic_json() const {
  JsonValue service_array{JsonValue::Array{}};
  for (const ServiceView& service : services) {
    JsonValue item;
    item.set("active", target_ref_json_or_null(service.active));
    item.set("ambiguity_pending", JsonValue(service.ambiguity_pending));
    item.set("continuity", JsonValue(std::string(continuity_class_text(service.continuity))));
    item.set("current_attempt", JsonValue(service.current_attempt.value()));
    item.set("effect_verified", JsonValue(service.effect_verified));
    item.set("failover_generation", JsonValue(service.generation.value()));
    item.set("failover_recorded", JsonValue(service.failover_recorded));
    item.set("fallback", target_ref_json_or_null(service.fallback));
    item.set("fence", JsonValue(service.fence.value()));
    item.set("last_reason", JsonValue(std::string(reason_code_text(service.last_reason))));
    item.set("lease_term", JsonValue(service.lease_term.value()));
    item.set("lifecycle", JsonValue(std::string(lifecycle_phase_text(service.lifecycle))));
    item.set("name", JsonValue(service.name.str()));
    item.set("recovery", JsonValue(std::string(recovery_phase_text(service.recovery))));
    item.set("scope", JsonValue(service.scope.str()));
    item.set("state_transfer_verified", JsonValue(service.state_transfer_verified));
    item.set("statefulness", JsonValue(std::string(statefulness_text(service.statefulness))));
    service_array.push(std::move(item));
  }
  JsonValue target_array{JsonValue::Array{}};
  for (const TargetRecord& target : targets) {
    target_array.push(off::to_json(target));
  }
  JsonValue object;
  object.set("boot", JsonValue(boot.value()));
  object.set("epoch", JsonValue(epoch.value()));
  object.set("policy", off::to_json(policy));
  object.set("policy_generation", JsonValue(policy_generation.value()));
  object.set("schema_version", JsonValue(static_cast<u64>(schema_version)));
  object.set("services", std::move(service_array));
  object.set("targets", std::move(target_array));
  object.set("topology_generation", JsonValue(topology_generation.value()));
  return object;
}

Digest FabricView::semantic_digest() const {
  const std::string text = semantic_json().dump();
  DigestBuilder builder;
  builder.update(reinterpret_cast<const u8*>(text.data()), text.size());
  return builder.value();
}

JsonValue FabricView::to_json() const {
  JsonValue service_array{JsonValue::Array{}};
  for (const ServiceView& service : services) {
    service_array.push(service.to_json());
  }
  JsonValue target_array{JsonValue::Array{}};
  for (const TargetRecord& target : targets) {
    target_array.push(off::to_json(target));
  }
  JsonValue object;
  object.set("boot", JsonValue(boot.value()));
  object.set("epoch", JsonValue(epoch.value()));
  object.set("policy", off::to_json(policy));
  object.set("policy_generation", JsonValue(policy_generation.value()));
  object.set("runtime_version", JsonValue(std::string(version_string())));
  object.set("schema_version", JsonValue(static_cast<u64>(schema_version)));
  object.set("services", std::move(service_array));
  object.set("stats", stats.to_json());
  object.set("targets", std::move(target_array));
  object.set("tick", JsonValue(tick.value()));
  object.set("topology_generation", JsonValue(topology_generation.value()));
  return object;
}

std::string FabricView::canonical_text(bool pretty) const {
  const JsonValue value = to_json();
  return pretty ? value.dump_pretty() : value.dump();
}

std::string recovery_report_text(const RecoveryReport& report) {
  std::string out;
  out += "recovery=";
  out.append(recovery_class_text(report.classification));
  out += " reason=";
  out.append(reason_code_text(report.reason));
  out += " snapshot_gen=";
  out.append(to_decimal(report.snapshot_generation));
  out += " journal_gen=";
  out.append(to_decimal(report.journal_generation));
  out += " transactions_applied=";
  out.append(to_decimal(report.transactions_applied));
  out += " transactions_dropped=";
  out.append(to_decimal(report.transactions_dropped));
  out += " records_replayed=";
  out.append(to_decimal(report.records_replayed));
  out += " bytes_accepted=";
  out.append(to_decimal(report.bytes_accepted));
  out += " bytes_discarded=";
  out.append(to_decimal(report.bytes_discarded));
  out += " torn_tail=";
  out += report.truncated_tail_repaired ? "yes" : "no";
  return out;
}

namespace detail {

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

/// Canonical payload of a record that changes exactly one service runtime.
struct RuntimeChange {
  ServiceName service{};
  ServiceRuntime runtime{};
  FenceRecord fence{};
  bool has_fence{false};
  std::vector<std::pair<TargetName, FenceWitness>> witnesses{};
};

void encode_runtime_change(CanonicalWriter& writer, const RuntimeChange& change) {
  encode_name(writer, change.service);
  encode_service_runtime(writer, change.runtime);
  writer.put_bool(change.has_fence);
  if (change.has_fence) {
    encode(writer, change.fence);
  }
  if (!writer.put_count(change.witnesses.size())) {
    return;
  }
  for (const auto& entry : change.witnesses) {
    encode_name(writer, entry.first);
    encode_gen(writer, entry.second.token);
    encode(writer, entry.second.incarnation);
    encode_gen(writer, entry.second.topology_generation);
  }
}

bool decode_runtime_change(CanonicalReader& reader, RuntimeChange& change, const FabricConfig& config) {
  if (!decode_name(reader, change.service) ||
      !decode_service_runtime(reader, change.runtime, config) ||
      !reader.get_bool(change.has_fence)) {
    return false;
  }
  if (change.has_fence && !decode(reader, change.fence)) {
    return false;
  }
  std::size_t count = 0;
  if (!reader.get_count(1, count) || count > config.max_targets) {
    return false;
  }
  change.witnesses.clear();
  for (std::size_t index = 0; index < count; ++index) {
    TargetName name;
    FenceWitness witness;
    if (!decode_name(reader, name) || !decode_gen(reader, witness.token) ||
        !decode(reader, witness.incarnation) || !decode_gen(reader, witness.topology_generation)) {
      return false;
    }
    change.witnesses.emplace_back(name, witness);
  }
  return true;
}

[[nodiscard]] Record make_record(RecordKind kind, CanonicalWriter& writer) {
  Record record;
  record.kind = kind;
  record.payload = writer.buffer();
  return record;
}

}  // namespace detail

struct Fabric::Impl {
  explicit Impl(FabricConfig configuration)
      : config(std::move(configuration)),
        explanations(config.max_explanations),
        intents(config.max_intent_queue) {}

  FabricConfig config{};
  mutable std::mutex mutex{};
  State state{};
  ExplanationLog explanations;
  IntentQueue intents;
  CancelRegistry cancels{};
  RestartSummary restart{};
  std::optional<Store> store{};
  bool opened{false};

  // ---- generation counters -------------------------------------------------

  [[nodiscard]] bool next_tick(LogicalTick& out) {
    const std::optional<LogicalTick> next = state.tick.successor();
    if (!next.has_value()) {
      return false;
    }
    state.tick = *next;
    out = state.tick;
    return true;
  }

  [[nodiscard]] bool alloc_evidence_id(EvidenceId& out) {
    if (state.next_evidence_id == 0 || state.next_evidence_id == (std::numeric_limits<u64>::max)()) {
      return false;
    }
    out = make_evidence_id(state.next_evidence_id);
    ++state.next_evidence_id;
    return true;
  }

  [[nodiscard]] bool alloc_attempt_id(AttemptId& out) {
    if (state.next_attempt_id == 0 || state.next_attempt_id == (std::numeric_limits<u64>::max)()) {
      return false;
    }
    out = make_attempt_id(state.next_attempt_id);
    ++state.next_attempt_id;
    return true;
  }

  [[nodiscard]] bool alloc_lease_id(LeaseId& out) {
    if (state.next_lease_id == 0 || state.next_lease_id == (std::numeric_limits<u64>::max)()) {
      return false;
    }
    out = make_lease_id(state.next_lease_id);
    ++state.next_lease_id;
    return true;
  }

  [[nodiscard]] bool alloc_plan_id(PlanId& out) {
    if (state.next_plan_id == 0 || state.next_plan_id == (std::numeric_limits<u64>::max)()) {
      return false;
    }
    out = make_plan_id(state.next_plan_id);
    ++state.next_plan_id;
    return true;
  }

  // ---- lookups -------------------------------------------------------------

  [[nodiscard]] const SourceRole* role_of(const SourceName& source) const {
    const auto it = state.sources.find(source);
    return it == state.sources.end() ? nullptr : &it->second;
  }

  [[nodiscard]] bool role_satisfies(const SourceName& source, SourceRole required) const {
    const SourceRole* role = role_of(source);
    if (role == nullptr) {
      return false;
    }
    return *role == required || *role == SourceRole::Operator;
  }

  [[nodiscard]] ServiceRuntime* service_of(const ServiceName& name) {
    const auto it = state.services.find(name);
    return it == state.services.end() ? nullptr : &it->second;
  }

  [[nodiscard]] const ServiceRuntime* service_of(const ServiceName& name) const {
    const auto it = state.services.find(name);
    return it == state.services.end() ? nullptr : &it->second;
  }

  [[nodiscard]] const TargetRecord* target_of(const TargetName& name) const {
    const auto it = state.targets.find(name);
    return it == state.targets.end() ? nullptr : &it->second;
  }

  [[nodiscard]] const CapabilitySlot* capability_of(const TargetName& name) const {
    const auto it = state.capabilities.find(name);
    return it == state.capabilities.end() ? nullptr : &it->second;
  }

  [[nodiscard]] FenceToken fence_token_of(const ServiceName& service) const {
    const auto it = state.fences.find(service);
    return it == state.fences.end() ? FenceToken{} : it->second.token;
  }

  [[nodiscard]] const FenceWitness* witness_of(const ServiceName& service,
                                               const TargetName& target) const {
    const auto it = state.witnesses.find(WitnessKey{service, target});
    return it == state.witnesses.end() ? nullptr : &it->second;
  }

  [[nodiscard]] const FailureEvidence* latest_failure_of(const ServiceName& service) const {
    const auto it = state.latest_failure.find(service);
    if (it == state.latest_failure.end() || it->second.is_nil()) {
      return nullptr;
    }
    const auto evidence = state.failures.find(it->second);
    return evidence == state.failures.end() ? nullptr : &evidence->second;
  }

  [[nodiscard]] const FailureEvidence* failure_of(const EvidenceId& id) const {
    const auto it = state.failures.find(id);
    return it == state.failures.end() ? nullptr : &it->second;
  }

  // ---- digest helpers ------------------------------------------------------

  /// Digest of exactly what a reporter asserted. Coordinator-assigned fields
  /// (the acceptance tick, the assigned evidence identifier and the digest
  /// itself) are excluded, so a byte-identical re-delivery fingerprints the
  /// same and can be recognised as a duplicate instead of a conflict.
  [[nodiscard]] static Digest capability_fingerprint(const CapabilityEvidence& value) {
    CanonicalWriter writer;
    encode_name(writer, value.target);
    encode_gen(writer, value.generation);
    encode(writer, value.capabilities);
    encode_gen(writer, value.topology_generation);
    encode(writer, value.incarnation);
    encode_name(writer, value.source);
    return writer.digest();
  }

  [[nodiscard]] static Digest failure_fingerprint(const FailureEvidence& value) {
    CanonicalWriter writer;
    encode_name(writer, value.service);
    encode(writer, value.target);
    writer.put_u8(static_cast<u8>(value.failure_class));
    writer.put_u8(static_cast<u8>(value.ambiguity));
    encode_name(writer, value.provenance.source);
    encode_gen(writer, value.provenance.sequence);
    encode_gen(writer, value.topology_generation);
    encode_gen(writer, value.capability_generation);
    encode_gen(writer, value.policy_generation);
    return writer.digest();
  }

  [[nodiscard]] static Digest dependency_fingerprint(const DependencyObservation& value) {
    CanonicalWriter writer;
    encode_name(writer, value.service);
    encode_name(writer, value.dependency);
    writer.put_u8(static_cast<u8>(value.health));
    encode(writer, value.observed_target);
    encode_name(writer, value.provenance.source);
    encode_gen(writer, value.provenance.sequence);
    encode_gen(writer, value.topology_generation);
    return writer.digest();
  }

  // ---- persistence ---------------------------------------------------------

  [[nodiscard]] Reason persist(const std::vector<Record>& records) {
    if (!store.has_value()) {
      return Reason::Ok;
    }
    const Reason code = store->commit_transaction(records);
    if (code != Reason::Ok) {
      return code;
    }
    state.stats.journal_transactions = saturating_inc(state.stats.journal_transactions);
    // Rotation is deliberately NOT triggered here. A snapshot taken before the
    // in-memory state reflects this commit would replace the journal that holds
    // the only durable copy of it. Callers rotate after applying the change.
    return Reason::Ok;
  }

  /// Compacts only when the journal has grown past its bound. Calling this
  /// unconditionally would rewrite the whole state on every commit.
  [[nodiscard]] Reason rotate_if_due() {
    if (!store.has_value() || !store->rotation_due()) {
      return Reason::Ok;
    }
    return maybe_compact();
  }

  [[nodiscard]] Reason maybe_compact() {
    if (!store.has_value()) {
      return Reason::Ok;
    }
    CanonicalWriter writer;
    encode_state(writer, state);
    if (!writer.ok()) {
      return Reason::Oversized;
    }
    const std::vector<u8>& buffer = writer.buffer();
    const Reason code = store->compact(buffer.data(), buffer.size());
    if (code == Reason::Ok) {
      state.stats.journal_compactions = saturating_inc(state.stats.journal_compactions);
    }
    return code;
  }

  // ---- explanation plumbing ------------------------------------------------

  void record_explanation(Explanation explanation) {
    state.stats.explanations_recorded = saturating_inc(state.stats.explanations_recorded);
    explanations.record(std::move(explanation));
    state.stats.explanations_evicted = explanations.evicted();
  }

  [[nodiscard]] RecoveryDecision refusal(Explanation explanation, Reason code, std::string subject,
                                         std::string detail, u64 value = 0) {
    explanation.outcome = code;
    explanation.accepted = false;
    explanation.steps.push_back(
        ExplanationStep{code, std::move(subject), std::move(detail), value});
    // A decision keyed by a request key is remembered even when it is refused,
    // so a duplicate delivery returns the same answer instead of being
    // re-evaluated against state that has since moved on. Transport-level
    // conditions are excluded because they describe the delivery, not the
    // decision, and a retry of those is legitimate.
    if (!explanation.request.is_nil() && code != Reason::ProtocolViolation &&
        code != Reason::QueueCapacityExceeded && code != Reason::RequestUnknown) {
      RequestCacheEntry entry;
      entry.kind = RequestKind::Failover;
      entry.outcome = code;
      entry.accepted = false;
      entry.recorded = state.tick;
      entry.policy_generation = state.policy_generation;
      entry.topology_generation = state.topology_generation;
      remember(explanation.request, entry);
    }
    RecoveryDecision decision;
    decision.outcome = code;
    decision.accepted = false;
    decision.explanation = std::move(explanation);
    record_explanation(decision.explanation);
    return decision;
  }

  [[nodiscard]] RecoveryDecision refusal_simple(Explanation explanation, Reason code) {
    return refusal(std::move(explanation), code, std::string{}, std::string{});
  }

  Reason note(Explanation& explanation, Reason code, std::string subject, std::string detail,
              u64 value = 0) {
    explanation.steps.push_back(
        ExplanationStep{code, std::move(subject), std::move(detail), value});
    return code;
  }

  /// Clears every claim that depended on a verified effect. Continuity is a
  /// claim about the present, so anything that ends or reverses a placement
  /// must drop it back to Unknown rather than leave a stale claim standing.
  static void invalidate_verification(ServiceRuntime& runtime) {
    runtime.activation_accepted = false;
    runtime.effect_verified = false;
    runtime.state_transfer_verified = false;
    runtime.continuity = ContinuityClass::Unknown;
  }

  /// Registers a request key for cancellation tracking. A nil key means the
  /// caller opted out of idempotency and cancellation; it never blocks progress.
  [[nodiscard]] Reason begin_request(RequestKey key) {
    if (key.is_nil()) {
      return Reason::Ok;
    }
    bool capacity = false;
    if (cancels.begin(key, config.max_request_cache, capacity)) {
      return Reason::Ok;
    }
    return capacity ? Reason::QueueCapacityExceeded : Reason::ProtocolViolation;
  }

  // ---- request cache -------------------------------------------------------

  [[nodiscard]] const RequestCacheEntry* cached(const RequestKey& key) const {
    if (key.is_nil()) {
      return nullptr;
    }
    const auto it = state.request_cache.find(key);
    return it == state.request_cache.end() ? nullptr : &it->second;
  }

  void remember(const RequestKey& key, const RequestCacheEntry& entry) {
    if (key.is_nil()) {
      return;
    }
    const auto existing = state.request_cache.find(key);
    if (existing != state.request_cache.end()) {
      existing->second = entry;
      return;
    }
    while (state.request_cache.size() >= config.max_request_cache &&
           !state.request_order.empty()) {
      const RequestKey oldest = state.request_order.front();
      state.request_order.pop_front();
      state.request_cache.erase(oldest);
    }
    state.request_cache.emplace(key, entry);
    state.request_order.push_back(key);
  }

  [[nodiscard]] RecoveryDecision replay_cached(const RequestCacheEntry& entry, Explanation explanation,
                                               const FailoverRequest& request) {
    RecoveryPlan plan;
    plan.service = request.service;
    plan.generation = entry.generation;
    plan.fence = entry.fence;
    plan.lease_term = entry.lease_term;
    plan.from = entry.from;
    plan.to = entry.to;
    for (std::size_t index = 0; index < kPlanStepKindCount; ++index) {
      if ((entry.plan_step_mask & (1U << index)) != 0U) {
        plan.steps.push_back(PlanStep{static_cast<PlanStepKind>(index), std::string{}, true});
      }
    }
    std::optional<FailoverIntent> intent;
    if (entry.has_intent) {
      FailoverIntent rebuilt;
      rebuilt.attempt = entry.attempt;
      rebuilt.service = request.service;
      rebuilt.generation = entry.generation;
      rebuilt.fence = entry.fence;
      rebuilt.lease_term = entry.lease_term;
      rebuilt.from = entry.from;
      rebuilt.to = entry.to;
      rebuilt.epoch = explanation.epoch;
      rebuilt.policy_generation = entry.policy_generation;
      rebuilt.topology_generation = entry.topology_generation;
      rebuilt.issued = entry.recorded;
      intent = rebuilt;
    }

    explanation.outcome = entry.outcome;
    explanation.accepted = entry.accepted;
    explanation.failover_generation = entry.generation;
    explanation.fence = entry.fence;
    note(explanation, Reason::AcceptDuplicateReplayCached, request.service.str(),
         "request key already decided", request.key.value());

    RecoveryDecision decision;
    decision.outcome = entry.outcome;
    decision.accepted = entry.accepted;
    decision.duplicate = true;
    decision.plan = std::move(plan);
    decision.intent = std::move(intent);
    decision.explanation = std::move(explanation);
    record_explanation(decision.explanation);
    state.stats.failovers_duplicate = saturating_inc(state.stats.failovers_duplicate);
    return decision;
  }

  // ---- record replay -------------------------------------------------------

  [[nodiscard]] bool apply_record(const Record& record);

  // ---- operations ----------------------------------------------------------

  Reason do_enroll_source(SourceName source, SourceRole role, Explanation& explanation);
  Reason do_register_service(const ServiceDescriptor& descriptor, SourceName registrar,
                             Explanation& explanation);
  Reason do_register_target(const TargetRecord& target, SourceName registrar,
                            Explanation& explanation);
  Reason do_set_topology_generation(TopologyGeneration generation, SourceName reporter,
                                    Explanation& explanation);
  Reason do_set_policy(const PolicyDescriptor& policy, Explanation& explanation);
  Reason do_withdraw_service(ServiceName service, SourceName requester, Explanation& explanation);
  Reason do_retire_target(TargetName target, SourceName requester, Explanation& explanation);
  Reason do_ingest_capability(const CapabilityEvidence& evidence, Explanation& explanation);
  Reason do_ingest_failure(FailureEvidence evidence, Explanation& explanation);
  Reason do_ingest_dependency(const DependencyObservation& observation, Explanation& explanation);

  [[nodiscard]] RecoveryDecision do_request_failover(const FailoverRequest& request, bool resume);
  [[nodiscard]] RecoveryDecision do_resolve_ambiguity(const AmbiguityResolution& resolution);
  [[nodiscard]] RecoveryDecision do_request_failback(const FailbackRequest& request);
  [[nodiscard]] RecoveryDecision do_acknowledge_intent(const IntentAck& ack);
  [[nodiscard]] RecoveryDecision do_report_effect(const EffectReport& report);

  // ---- recovery support ----------------------------------------------------

  [[nodiscard]] Reason open();
  [[nodiscard]] Reason close();
  [[nodiscard]] FenceVerdict verify_fence(const FenceCheck& check) const;
  [[nodiscard]] Reason recover_from_store(std::vector<Record>& records);
  void apply_restart_recovery();
  [[nodiscard]] FabricView build_view() const;
  [[nodiscard]] Explanation begin_explanation(const ServiceName& service,
                                              const RequestKey& key) const;
  void finish_ingest_explanation(Explanation& explanation, Explanation* out);
  [[nodiscard]] bool valid_policy(const PolicyDescriptor& policy) const;
  [[nodiscard]] Reason validate_descriptor(const ServiceDescriptor& descriptor) const;
  void evict_failures_if_needed();
  void append_attempt(ServiceRuntime& runtime, const AttemptRecord& record);
  [[nodiscard]] Reason select_fallback(const ServiceRuntime& runtime, const TargetRef& failed,
                                       Explanation& explanation, TargetRecord& chosen,
                                       Reason& refusal_code);
  [[nodiscard]] Reason build_plan(RecoveryPlan& plan, const ServiceRuntime& runtime,
                                  const TargetRef& from, const TargetRecord& to,
                                  Explanation& explanation);
  [[nodiscard]] bool peek_tick(LogicalTick& out) const;
  [[nodiscard]] Reason qualify_dependencies(const ServiceRuntime& runtime,
                                            Explanation& explanation);
  [[nodiscard]] Reason evaluate_candidate(const ServiceRuntime& runtime,
                                          const TargetRecord& record, Reason& reject) const;
  [[nodiscard]] static u8 device_rank(DeviceKind kind);
  enum class CommitMode : u8 {
    /// First authoritative placement of a service that had none.
    InitialPlacement = 0,
    /// Recovery of an existing placement onto a compatible fallback.
    Failover = 1,
    /// Re-issue of a restart-downgraded attempt at the same fence.
    Resume = 2,
  };

  struct CommitResult {
    Reason code{Reason::Ok};
    RecoveryPlan plan{};
    FailoverIntent intent{};
    bool has_intent{false};
  };
  [[nodiscard]] CommitResult commit_recovery(const FailoverRequest& request, ServiceRuntime& runtime,
                                             const TargetRecord& to, const TargetRef& from,
                                             Reason trigger_code, CommitMode mode,
                                             Explanation& explanation);
  [[nodiscard]] RecoveryDecision do_establish_placement(const PlacementRequest& request);
};

void Fabric::Impl::append_attempt(ServiceRuntime& runtime, const AttemptRecord& record) {
  runtime.history.push_back(record);
  while (runtime.history.size() > config.max_attempt_history) {
    runtime.history.pop_front();
    runtime.attempts_evicted = saturating_inc(runtime.attempts_evicted);
  }
}

bool Fabric::Impl::valid_policy(const PolicyDescriptor& policy) const {
  if (policy.max_attempts_per_generation == 0 || policy.max_plan_steps == 0 ||
      policy.max_fallback_candidates == 0 || policy.dependency_max_depth == 0 ||
      policy.evidence_max_age_ticks == 0) {
    return false;
  }
  return true;
}

Reason Fabric::Impl::validate_descriptor(const ServiceDescriptor& descriptor) const {
  if (descriptor.name.empty() || descriptor.scope.empty()) {
    return Reason::Malformed;
  }
  if (descriptor.dependencies.size() > config.max_dependencies_per_service) {
    return Reason::DependencyCapacityExceeded;
  }
  std::set<std::string_view> seen;
  for (const DependencySpec& spec : descriptor.dependencies) {
    if (spec.service.empty()) {
      return Reason::Malformed;
    }
    if (spec.service == descriptor.name) {
      return Reason::DependencySelfReference;
    }
    if (!seen.insert(spec.service.view()).second) {
      return Reason::DuplicateIdentity;
    }
  }
  return Reason::Ok;
}

Explanation Fabric::Impl::begin_explanation(const ServiceName& service,
                                            const RequestKey& key) const {
  Explanation explanation;
  explanation.service = service;
  explanation.request = key;
  explanation.tick = state.tick;
  explanation.epoch = state.epoch;
  explanation.policy_generation = state.policy_generation;
  explanation.topology_generation = state.topology_generation;
  explanation.fence = fence_token_of(service);
  return explanation;
}

void Fabric::Impl::finish_ingest_explanation(Explanation& explanation, Explanation* out) {
  if (explanation.outcome == Reason::Ok) {
    explanation.accepted = true;
  }
  record_explanation(explanation);
  if (out != nullptr) {
    *out = explanation;
  }
}

void Fabric::Impl::evict_failures_if_needed() {
  while (state.failures.size() >= config.max_evidence_records && !state.failure_order.empty()) {
    const EvidenceId oldest = state.failure_order.front();
    state.failure_order.pop_front();
    if (state.failures.erase(oldest) > 0) {
      state.stats.evidence_evicted = saturating_inc(state.stats.evidence_evicted);
    }
    for (auto& entry : state.latest_failure) {
      if (entry.second == oldest) {
        entry.second = EvidenceId{};
      }
    }
  }
}

bool Fabric::Impl::apply_record(const Record& record) {
  CanonicalReader reader(record.payload.data(), record.payload.size());
  switch (record.kind) {
    case RecordKind::ServiceRegistered: {
      ServiceDescriptor descriptor;
      if (!decode(reader, descriptor)) {
        return false;
      }
      ServiceRuntime runtime;
      runtime.descriptor = descriptor;
      runtime.lifecycle = LifecyclePhase::Registered;
      state.services[descriptor.name] = std::move(runtime);
      return true;
    }
    case RecordKind::TargetUpserted: {
      TargetRecord target;
      if (!decode(reader, target)) {
        return false;
      }
      state.targets[target.name] = target;
      return true;
    }
    case RecordKind::TopologyGenerationSet: {
      u64 raw = 0;
      if (!reader.get_u64(raw)) {
        return false;
      }
      state.topology_generation = TopologyGeneration::from_value(raw);
      return true;
    }
    case RecordKind::PolicySet: {
      PolicyDescriptor policy;
      if (!decode(reader, policy)) {
        return false;
      }
      state.policy = policy;
      state.policy_generation = policy.generation;
      return true;
    }
    case RecordKind::CapabilityIngested: {
      CapabilityEvidence evidence;
      if (!decode(reader, evidence)) {
        return false;
      }
      CapabilitySlot slot;
      slot.evidence = evidence;
      slot.requires_reconfirmation = false;
      state.capabilities[evidence.target] = slot;
      return true;
    }
    case RecordKind::FailureIngested: {
      FailureEvidence evidence;
      if (!decode(reader, evidence)) {
        return false;
      }
      state.failures[evidence.id] = evidence;
      state.failure_order.push_back(evidence.id);
      state.latest_failure[evidence.service] = evidence.id;
      SourceWatermark watermark;
      watermark.sequence = evidence.provenance.sequence;
      watermark.digest = evidence.content_digest;
      watermark.tick = evidence.provenance.tick;
      state.watermarks[evidence.provenance.source] = watermark;
      return true;
    }
    case RecordKind::DependencyIngested: {
      DependencyObservation observation;
      if (!decode(reader, observation)) {
        return false;
      }
      DependencySlot slot;
      slot.observation = observation;
      slot.requires_reconfirmation = false;
      state.dependencies[observation.service][observation.dependency] = slot;
      SourceWatermark watermark;
      watermark.sequence = observation.provenance.sequence;
      watermark.digest = observation.content_digest;
      watermark.tick = observation.provenance.tick;
      state.watermarks[observation.provenance.source] = watermark;
      return true;
    }
    case RecordKind::AttemptCommitted:
    case RecordKind::AttemptAcknowledged:
    case RecordKind::EffectRecorded:
    case RecordKind::AmbiguityResolved:
    case RecordKind::FailbackCommitted:
    case RecordKind::RecoveryResumed: {
      RuntimeChange change;
      if (!decode_runtime_change(reader, change, config)) {
        return false;
      }
      state.services[change.service] = change.runtime;
      if (change.has_fence) {
        state.fences[change.service] = change.fence;
      }
      for (const auto& entry : change.witnesses) {
        FenceWitness witness;
        witness.token = entry.second.token;
        witness.incarnation = entry.second.incarnation;
        witness.topology_generation = entry.second.topology_generation;
        state.witnesses[WitnessKey{change.service, entry.first}] = witness;
      }
      return true;
    }
    case RecordKind::TargetRetired: {
      TargetName target;
      if (!decode_name(reader, target)) {
        return false;
      }
      state.targets.erase(target);
      state.capabilities.erase(target);
      return true;
    }
    case RecordKind::ServiceWithdrawn: {
      ServiceName service;
      if (!decode_name(reader, service)) {
        return false;
      }
      const auto it = state.services.find(service);
      if (it != state.services.end()) {
        it->second.lifecycle = LifecyclePhase::Withdrawn;
      }
      return true;
    }
    case RecordKind::SourceRegistered: {
      SourceName source;
      u8 role = 0;
      if (!decode_name(reader, source) || !reader.get_u8(role)) {
        return false;
      }
      if (!enum_valid(role, kSourceRoleCount)) {
        return false;
      }
      state.sources[source] = static_cast<SourceRole>(role);
      return true;
    }
    case RecordKind::EpochAdvanced: {
      u64 epoch = 0;
      u64 boot = 0;
      if (!reader.get_u64(epoch) || !reader.get_u64(boot)) {
        return false;
      }
      state.epoch = CoordinatorEpoch::from_value(epoch);
      state.boot = BootId::from_value(boot);
      return true;
    }
    case RecordKind::BeginTransaction:
    case RecordKind::CommitTransaction:
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Ingestion operations
// ---------------------------------------------------------------------------

bool Fabric::Impl::peek_tick(LogicalTick& out) const {
  const std::optional<LogicalTick> next = state.tick.successor();
  if (!next.has_value()) {
    return false;
  }
  out = *next;
  return true;
}

u8 Fabric::Impl::device_rank(DeviceKind kind) {
  switch (kind) {
    case DeviceKind::Dpu:
      return 3;
    case DeviceKind::SmartNic:
      return 2;
    case DeviceKind::HostCpu:
      return 1;
    case DeviceKind::Synthetic:
      return 0;
    default:
      return 0;
  }
}

Reason Fabric::Impl::do_enroll_source(SourceName source, SourceRole role, Explanation& explanation) {
  if (!opened) {
    return note(explanation, Reason::ShuttingDown, source.str(), "runtime is not open");
  }
  if (source.empty()) {
    return note(explanation, Reason::Malformed, std::string{}, "source name is empty");
  }
  const auto existing = state.sources.find(source);
  if (existing != state.sources.end()) {
    if (existing->second == role) {
      return note(explanation, Reason::AcceptDuplicateIdempotent, source.str(),
                  "source already enrolled with this role");
    }
    return note(explanation, Reason::DuplicateIdentity, source.str(),
                "source already enrolled with a different role");
  }
  if (state.sources.size() >= config.max_sources) {
    return note(explanation, Reason::RegistryCapacityExceeded, source.str(),
                "source registry is full");
  }
  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return note(explanation, Reason::GenerationExhausted, source.str(), "logical tick exhausted");
  }
  CanonicalWriter writer;
  encode_name(writer, source);
  writer.put_u8(static_cast<u8>(role));
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::SourceRegistered, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return note(explanation, code, source.str(), "durable commit failed");
  }
  state.tick = stamp;
  state.sources[source] = role;
  state.stats.sources_enrolled = saturating_inc(state.stats.sources_enrolled);
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return note(explanation, compact, source.str(), "compaction failed");
  }
  return note(explanation, Reason::AcceptSourceEnrolled, source.str(), "source enrolled");
}

Reason Fabric::Impl::do_register_service(const ServiceDescriptor& descriptor, SourceName registrar,
                                         Explanation& explanation) {
  if (!opened) {
    return note(explanation, Reason::ShuttingDown, descriptor.name.str(), "runtime is not open");
  }
  if (!role_satisfies(registrar, SourceRole::TopologyReporter)) {
    state.stats.evidence_rejected_unauthorized =
        saturating_inc(state.stats.evidence_rejected_unauthorized);
    return note(explanation, Reason::EvidenceReporterUnauthorized, registrar.str(),
                "registrar is not an enrolled topology reporter");
  }
  const Reason valid = validate_descriptor(descriptor);
  if (valid != Reason::Ok) {
    return note(explanation, valid, descriptor.name.str(), "service descriptor is not usable");
  }
  const auto existing = state.services.find(descriptor.name);
  if (existing != state.services.end()) {
    if (existing->second.descriptor == descriptor) {
      return note(explanation, Reason::AcceptDuplicateIdempotent, descriptor.name.str(),
                  "service already registered with an identical descriptor");
    }
    return note(explanation, Reason::DuplicateService, descriptor.name.str(),
                "service already registered with a different descriptor");
  }
  if (state.services.size() >= config.max_services) {
    return note(explanation, Reason::RegistryCapacityExceeded, descriptor.name.str(),
                "service registry is full");
  }
  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return note(explanation, Reason::GenerationExhausted, descriptor.name.str(),
                "logical tick exhausted");
  }
  CanonicalWriter writer;
  encode(writer, descriptor);
  if (!writer.ok()) {
    return note(explanation, Reason::Oversized, descriptor.name.str(),
                "descriptor encoding exceeded the configured ceiling");
  }
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::ServiceRegistered, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return note(explanation, code, descriptor.name.str(), "durable commit failed");
  }
  state.tick = stamp;
  ServiceRuntime runtime;
  runtime.descriptor = descriptor;
  runtime.lifecycle = LifecyclePhase::Registered;
  runtime.last_change = stamp;
  runtime.last_reason = Reason::Ok;
  state.services.emplace(descriptor.name, std::move(runtime));
  state.stats.services_registered = saturating_inc(state.stats.services_registered);
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return note(explanation, compact, descriptor.name.str(), "compaction failed");
  }
  return note(explanation, Reason::AcceptServiceRegistered, descriptor.name.str(),
              "service registered");
}

Reason Fabric::Impl::do_register_target(const TargetRecord& target, SourceName registrar,
                                        Explanation& explanation) {
  if (!opened) {
    return note(explanation, Reason::ShuttingDown, target.name.str(), "runtime is not open");
  }
  if (!role_satisfies(registrar, SourceRole::TopologyReporter)) {
    state.stats.evidence_rejected_unauthorized =
        saturating_inc(state.stats.evidence_rejected_unauthorized);
    return note(explanation, Reason::EvidenceReporterUnauthorized, registrar.str(),
                "registrar is not an enrolled topology reporter");
  }
  if (target.name.empty() || target.host.empty() || target.device.empty() || target.scope.empty()) {
    return note(explanation, Reason::Malformed, target.name.str(),
                "target identity is incomplete");
  }
  if (target.incarnation.host.is_initial() || target.incarnation.device.is_initial()) {
    return note(explanation, Reason::ImpossibleValue, target.name.str(),
                "incarnation zero never names an adopted target");
  }
  const auto existing = state.targets.find(target.name);
  if (existing != state.targets.end()) {
    if (target.incarnation < existing->second.incarnation) {
      return note(explanation, Reason::IncarnationRegression, target.name.str(),
                  "incarnation may not move backwards", existing->second.incarnation.host.value());
    }
    if (target.incarnation == existing->second.incarnation) {
      if (existing->second == target) {
        return note(explanation, Reason::AcceptDuplicateIdempotent, target.name.str(),
                    "target already registered at this incarnation");
      }
      return note(explanation, Reason::DuplicateTarget, target.name.str(),
                  "target redefined at the same incarnation");
    }
  } else if (state.targets.size() >= config.max_targets) {
    return note(explanation, Reason::RegistryCapacityExceeded, target.name.str(),
                "target registry is full");
  }
  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return note(explanation, Reason::GenerationExhausted, target.name.str(), "logical tick exhausted");
  }
  // The coordinator stamps the topology generation the target was adopted
  // under; a caller-supplied value is replaced rather than trusted, so a target
  // can never claim to belong to a topology view it was not admitted in.
  TargetRecord stamped = target;
  stamped.topology_generation = state.topology_generation;
  CanonicalWriter writer;
  encode(writer, stamped);
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::TargetUpserted, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return note(explanation, code, target.name.str(), "durable commit failed");
  }
  const bool adopted = existing != state.targets.end();
  state.tick = stamp;
  state.targets[target.name] = stamped;
  state.stats.targets_registered = saturating_inc(state.stats.targets_registered);
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return note(explanation, compact, target.name.str(), "compaction failed");
  }
  return note(explanation, Reason::AcceptTargetRegistered, target.name.str(),
              adopted ? "target re-adopted at a newer incarnation" : "target registered");
}

Reason Fabric::Impl::do_set_topology_generation(TopologyGeneration generation, SourceName reporter,
                                                Explanation& explanation) {
  if (!opened) {
    return note(explanation, Reason::ShuttingDown, std::string{}, "runtime is not open");
  }
  if (!role_satisfies(reporter, SourceRole::TopologyReporter)) {
    state.stats.evidence_rejected_unauthorized =
        saturating_inc(state.stats.evidence_rejected_unauthorized);
    return note(explanation, Reason::EvidenceReporterUnauthorized, reporter.str(),
                "reporter is not an enrolled topology reporter");
  }
  if (generation < state.topology_generation) {
    return note(explanation, Reason::StaleGeneration, reporter.str(),
                "topology generation may not move backwards", state.topology_generation.value());
  }
  if (generation == state.topology_generation) {
    return note(explanation, Reason::AcceptDuplicateIdempotent, reporter.str(),
                "topology generation already current");
  }
  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return note(explanation, Reason::GenerationExhausted, reporter.str(), "logical tick exhausted");
  }
  CanonicalWriter writer;
  writer.put_u64(generation.value());
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::TopologyGenerationSet, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return note(explanation, code, reporter.str(), "durable commit failed");
  }
  state.tick = stamp;
  state.topology_generation = generation;
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return note(explanation, compact, reporter.str(), "compaction failed");
  }
  return note(explanation, Reason::AcceptTopologyGenerationAdvanced, reporter.str(),
              "topology generation advanced; capability evidence must be re-confirmed",
              generation.value());
}

Reason Fabric::Impl::do_set_policy(const PolicyDescriptor& policy, Explanation& explanation) {
  if (!opened) {
    return note(explanation, Reason::ShuttingDown, std::string{}, "runtime is not open");
  }
  if (!valid_policy(policy)) {
    return note(explanation, Reason::ImpossibleValue, policy.scope.str(),
                "policy contains a zero budget that would forbid all progress");
  }
  if (policy.generation < state.policy_generation) {
    return note(explanation, Reason::StaleGeneration, policy.scope.str(),
                "policy generation may not move backwards", state.policy_generation.value());
  }
  if (policy.generation == state.policy_generation) {
    if (policy == state.policy) {
      return note(explanation, Reason::AcceptDuplicateIdempotent, policy.scope.str(),
                  "policy already current");
    }
    return note(explanation, Reason::GenerationConflict, policy.scope.str(),
                "policy redefined at the same generation");
  }
  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return note(explanation, Reason::GenerationExhausted, policy.scope.str(),
                "logical tick exhausted");
  }
  CanonicalWriter writer;
  encode(writer, policy);
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::PolicySet, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return note(explanation, code, policy.scope.str(), "durable commit failed");
  }
  state.tick = stamp;
  state.policy = policy;
  state.policy_generation = policy.generation;
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return note(explanation, compact, policy.scope.str(), "compaction failed");
  }
  return note(explanation, Reason::AcceptPolicyGenerationAdvanced, policy.scope.str(),
              "policy generation advanced", policy.generation.value());
}

Reason Fabric::Impl::do_withdraw_service(ServiceName service, SourceName requester,
                                         Explanation& explanation) {
  if (!opened) {
    return note(explanation, Reason::ShuttingDown, service.str(), "runtime is not open");
  }
  if (!role_satisfies(requester, SourceRole::Operator)) {
    state.stats.evidence_rejected_unauthorized =
        saturating_inc(state.stats.evidence_rejected_unauthorized);
    return note(explanation, Reason::NoAuthority, requester.str(),
                "withdrawal requires an enrolled operator");
  }
  ServiceRuntime* runtime = service_of(service);
  if (runtime == nullptr) {
    return note(explanation, Reason::UnknownService, service.str(), "service is not registered");
  }
  if (runtime->lifecycle == LifecyclePhase::Withdrawn) {
    return note(explanation, Reason::AcceptDuplicateIdempotent, service.str(),
                "service already withdrawn");
  }
  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return note(explanation, Reason::GenerationExhausted, service.str(), "logical tick exhausted");
  }
  CanonicalWriter writer;
  encode_name(writer, service);
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::ServiceWithdrawn, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return note(explanation, code, service.str(), "durable commit failed");
  }
  state.tick = stamp;
  runtime = service_of(service);
  runtime->lifecycle = LifecyclePhase::Withdrawn;
  runtime->recovery = RecoveryPhase::None;
  runtime->continuity = ContinuityClass::None;
  runtime->last_change = stamp;
  runtime->last_reason = Reason::TransitionRefused;
  state.stats.withdrawn_services = saturating_inc(state.stats.withdrawn_services);
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return note(explanation, compact, service.str(), "compaction failed");
  }
  return note(explanation, Reason::AcceptServiceWithdrawn, service.str(), "service withdrawn");
}

Reason Fabric::Impl::do_retire_target(TargetName target, SourceName requester,
                                      Explanation& explanation) {
  if (!opened) {
    return note(explanation, Reason::ShuttingDown, target.str(), "runtime is not open");
  }
  if (!role_satisfies(requester, SourceRole::TopologyReporter)) {
    state.stats.evidence_rejected_unauthorized =
        saturating_inc(state.stats.evidence_rejected_unauthorized);
    return note(explanation, Reason::EvidenceReporterUnauthorized, requester.str(),
                "retiring a target requires an enrolled topology reporter");
  }
  if (target_of(target) == nullptr) {
    return note(explanation, Reason::UnknownTarget, target.str(), "target is not registered");
  }
  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return note(explanation, Reason::GenerationExhausted, target.str(), "logical tick exhausted");
  }
  CanonicalWriter writer;
  encode_name(writer, target);
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::TargetRetired, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return note(explanation, code, target.str(), "durable commit failed");
  }
  state.tick = stamp;
  state.targets.erase(target);
  state.capabilities.erase(target);
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return note(explanation, compact, target.str(), "compaction failed");
  }
  return note(explanation, Reason::AcceptTargetRetired, target.str(),
              "target removed from the governed topology; any placement on it is unchanged");
}

Reason Fabric::Impl::do_ingest_capability(const CapabilityEvidence& evidence,
                                          Explanation& explanation) {
  if (!opened) {
    return note(explanation, Reason::ShuttingDown, evidence.target.str(), "runtime is not open");
  }
  if (!role_satisfies(evidence.source, SourceRole::CapabilityReporter)) {
    state.stats.evidence_rejected_unauthorized =
        saturating_inc(state.stats.evidence_rejected_unauthorized);
    return note(explanation, Reason::EvidenceReporterUnauthorized, evidence.source.str(),
                "source is not an enrolled capability reporter");
  }
  const TargetRecord* target = target_of(evidence.target);
  if (target == nullptr) {
    return note(explanation, Reason::UnknownTarget, evidence.target.str(), "target is not registered");
  }
  if (evidence.incarnation != target->incarnation) {
    state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
    return note(explanation, Reason::StaleGeneration, evidence.target.str(),
                "capability evidence names a different target incarnation",
                target->incarnation.host.value());
  }
  if (evidence.topology_generation != state.topology_generation) {
    state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
    return note(explanation, Reason::EvidenceTopologyMismatch, evidence.target.str(),
                "capability evidence was observed under a different topology generation",
                state.topology_generation.value());
  }
  const CapabilitySlot* existing = capability_of(evidence.target);
  if (existing != nullptr && existing->evidence.incarnation != evidence.incarnation) {
    // The target was re-adopted: the incarnation, not the generation, is the
    // stronger identity, so the previous generation space does not apply.
    existing = nullptr;
  }
  if (existing != nullptr) {
    if (evidence.generation < existing->evidence.generation) {
      state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
      return note(explanation, Reason::SupersededGeneration, evidence.target.str(),
                  "capability generation is older than the retained one",
                  existing->evidence.generation.value());
    }
    if (evidence.generation == existing->evidence.generation &&
        evidence.capabilities == existing->evidence.capabilities &&
        evidence.incarnation == existing->evidence.incarnation) {
      return note(explanation, Reason::AcceptDuplicateIdempotent, evidence.target.str(),
                  "capability generation already recorded");
    }
    if (evidence.generation == existing->evidence.generation) {
      state.stats.evidence_rejected_conflict = saturating_inc(state.stats.evidence_rejected_conflict);
      return note(explanation, Reason::EvidenceConflicting, evidence.target.str(),
                  "capability generation reused with different content");
    }
  }
  const Digest computed = capability_fingerprint(evidence);
  if (!evidence.content_digest.is_zero() && evidence.content_digest != computed) {
    return note(explanation, Reason::Malformed, evidence.target.str(),
                "capability content digest does not match the asserted content");
  }
  CapabilityEvidence record = evidence;
  record.content_digest = computed;
  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return note(explanation, Reason::GenerationExhausted, evidence.target.str(),
                "logical tick exhausted");
  }
  record.tick = stamp;
  CanonicalWriter writer;
  encode(writer, record);
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::CapabilityIngested, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return note(explanation, code, evidence.target.str(), "durable commit failed");
  }
  state.tick = stamp;
  CapabilitySlot slot;
  slot.evidence = record;
  slot.requires_reconfirmation = false;
  state.capabilities[evidence.target] = slot;
  state.stats.capabilities_ingested = saturating_inc(state.stats.capabilities_ingested);
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return note(explanation, compact, evidence.target.str(), "compaction failed");
  }
  return note(explanation, Reason::AcceptCapabilityRecorded, evidence.target.str(),
              "capability evidence accepted", record.generation.value());
}

Reason Fabric::Impl::do_ingest_failure(FailureEvidence evidence, Explanation& explanation) {
  if (!opened) {
    return note(explanation, Reason::ShuttingDown, evidence.service.str(), "runtime is not open");
  }
  if (!role_satisfies(evidence.provenance.source, SourceRole::FailureReporter)) {
    state.stats.evidence_rejected_unauthorized =
        saturating_inc(state.stats.evidence_rejected_unauthorized);
    return note(explanation, Reason::EvidenceReporterUnauthorized, evidence.provenance.source.str(),
                "source is not an enrolled failure reporter");
  }
  ServiceRuntime* runtime = service_of(evidence.service);
  if (runtime == nullptr) {
    return note(explanation, Reason::UnknownService, evidence.service.str(),
                "service is not registered");
  }
  if (target_of(evidence.target.target) == nullptr) {
    return note(explanation, Reason::UnknownTarget, evidence.target.target.str(),
                "target is not registered");
  }
  if (evidence.provenance.sequence.is_initial()) {
    return note(explanation, Reason::EvidenceIncomplete, evidence.provenance.source.str(),
                "evidence sequence zero never names a delivered observation");
  }
  if (evidence.provenance.epoch != state.epoch) {
    state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
    return note(explanation, Reason::AuthoritySuperseded, evidence.provenance.source.str(),
                "evidence was produced under a previous coordinator epoch",
                evidence.provenance.epoch.value());
  }
  if (evidence.provenance.boot != state.boot) {
    state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
    return note(explanation, Reason::BootMismatch, evidence.provenance.source.str(),
                "evidence was produced during a previous boot", evidence.provenance.boot.value());
  }
  const Digest fingerprint = failure_fingerprint(evidence);
  const auto watermark = state.watermarks.find(evidence.provenance.source);
  if (watermark != state.watermarks.end() &&
      evidence.provenance.sequence <= watermark->second.sequence) {
    const Digest computed = fingerprint;
    if (evidence.provenance.sequence == watermark->second.sequence) {
      if (computed == watermark->second.digest) {
        return note(explanation, Reason::AcceptDuplicateIdempotent,
                    evidence.provenance.source.str(), "evidence sequence already applied",
                    evidence.provenance.sequence.value());
      }
      // The same sequence carrying different content is a source conflict, not
      // a replay: the source reused an identity for two different assertions.
      state.stats.evidence_rejected_conflict =
          saturating_inc(state.stats.evidence_rejected_conflict);
      return note(explanation, Reason::EvidenceConflicting, evidence.provenance.source.str(),
                  "evidence sequence reused with different content",
                  evidence.provenance.sequence.value());
    }
    state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
    return note(explanation, Reason::EvidenceReplayRejected, evidence.provenance.source.str(),
                "evidence sequence is older than the recorded watermark",
                watermark->second.sequence.value());
  }
  if (evidence.topology_generation != state.topology_generation) {
    state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
    return note(explanation, Reason::EvidenceTopologyMismatch, evidence.service.str(),
                "evidence was observed under a different topology generation",
                state.topology_generation.value());
  }
  if (evidence.policy_generation > state.policy_generation) {
    return note(explanation, Reason::FutureGeneration, evidence.service.str(),
                "evidence cites a policy generation newer than the current one",
                evidence.policy_generation.value());
  }
  if (const CapabilitySlot* slot = capability_of(evidence.target.target); slot != nullptr) {
    if (evidence.capability_generation > slot->evidence.generation) {
      return note(explanation, Reason::FutureGeneration, evidence.target.target.str(),
                  "evidence cites a capability generation newer than the recorded one",
                  evidence.capability_generation.value());
    }
  }
  if (evidence.target.incarnation.host.is_initial() ||
      evidence.target.incarnation.device.is_initial()) {
    return note(explanation, Reason::ImpossibleValue, evidence.target.target.str(),
                "evidence names an incarnation of zero");
  }
  if (runtime->active.has_value() && evidence.target != *runtime->active) {
    return note(explanation, Reason::EvidenceTargetMismatch, evidence.target.target.str(),
                "evidence does not name the current active placement");
  }
  if (!evidence.content_digest.is_zero() && evidence.content_digest != fingerprint) {
    return note(explanation, Reason::Malformed, evidence.service.str(),
                "evidence content digest does not match the asserted content");
  }
  evidence.content_digest = fingerprint;

  if (!evidence.id.is_nil()) {
    const FailureEvidence* retained = failure_of(evidence.id);
    if (retained != nullptr) {
      if (retained->content_digest == fingerprint && retained->service == evidence.service) {
        return note(explanation, Reason::AcceptDuplicateIdempotent, evidence.service.str(),
                    "evidence identifier already retained", evidence.id.value());
      }
      state.stats.evidence_rejected_conflict =
          saturating_inc(state.stats.evidence_rejected_conflict);
      return note(explanation, Reason::EvidenceConflicting, evidence.service.str(),
                  "evidence identifier reused with different content", evidence.id.value());
    }
  }

  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return note(explanation, Reason::GenerationExhausted, evidence.service.str(),
                "logical tick exhausted");
  }
  EvidenceId assigned = evidence.id;
  u64 next_evidence = state.next_evidence_id;
  if (assigned.is_nil()) {
    if (next_evidence == 0 || next_evidence == (std::numeric_limits<u64>::max)()) {
      return note(explanation, Reason::GenerationExhausted, evidence.service.str(),
                  "evidence identifier space exhausted");
    }
    assigned = make_evidence_id(next_evidence);
  }
  evidence.id = assigned;
  evidence.provenance.tick = stamp;

  CanonicalWriter writer;
  encode(writer, evidence);
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::FailureIngested, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return note(explanation, code, evidence.service.str(), "durable commit failed");
  }
  state.tick = stamp;
  if (assigned == make_evidence_id(next_evidence)) {
    state.next_evidence_id = next_evidence + 1U;
  }
  evict_failures_if_needed();
  state.failures[evidence.id] = evidence;
  state.failure_order.push_back(evidence.id);
  state.latest_failure[evidence.service] = evidence.id;
  SourceWatermark mark;
  mark.sequence = evidence.provenance.sequence;
  mark.digest = evidence.content_digest;
  mark.tick = evidence.provenance.tick;
  state.watermarks[evidence.provenance.source] = mark;
  state.stats.failures_ingested = saturating_inc(state.stats.failures_ingested);

  runtime = service_of(evidence.service);
  if (evidence.ambiguity != AmbiguityState::Unambiguous) {
    runtime->ambiguity_pending = true;
    runtime->pending_ambiguity = evidence.id;
    runtime->recovery = RecoveryPhase::AmbiguityPending;
    runtime->lifecycle = LifecyclePhase::RecoveryPending;
    invalidate_verification(*runtime);
    runtime->last_change = stamp;
    runtime->last_reason = Reason::AmbiguityUnresolved;
    state.stats.ambiguity_pending = saturating_inc(state.stats.ambiguity_pending);
    const Reason compact = rotate_if_due();
    if (compact != Reason::Ok) {
      return note(explanation, compact, evidence.service.str(), "compaction failed");
    }
    // Ingestion is accepted because the observation was recorded; the ambiguity
    // is what blocks a recovery decision later.
    note(explanation, Reason::AmbiguityRequiresOperator, evidence.service.str(),
         "ambiguous failure evidence recorded; recovery is blocked until resolved",
         evidence.id.value());
    return note(explanation, Reason::AcceptFailureRecorded, evidence.service.str(),
                "ambiguous failure evidence accepted", evidence.id.value());
  }
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return note(explanation, compact, evidence.service.str(), "compaction failed");
  }
  return note(explanation, Reason::AcceptFailureRecorded, evidence.service.str(),
              "failure evidence accepted", evidence.id.value());
}

Reason Fabric::Impl::do_ingest_dependency(const DependencyObservation& observation,
                                          Explanation& explanation) {
  if (!opened) {
    return note(explanation, Reason::ShuttingDown, observation.service.str(),
                "runtime is not open");
  }
  if (!role_satisfies(observation.provenance.source, SourceRole::DependencyReporter)) {
    state.stats.evidence_rejected_unauthorized =
        saturating_inc(state.stats.evidence_rejected_unauthorized);
    return note(explanation, Reason::EvidenceReporterUnauthorized,
                observation.provenance.source.str(),
                "source is not an enrolled dependency reporter");
  }
  if (service_of(observation.service) == nullptr) {
    return note(explanation, Reason::UnknownService, observation.service.str(),
                "service is not registered");
  }
  const ServiceRuntime* dependency_runtime = service_of(observation.dependency);
  if (dependency_runtime == nullptr) {
    return note(explanation, Reason::UnknownService, observation.dependency.str(),
                "dependency service is not registered");
  }
  if (observation.service == observation.dependency) {
    return note(explanation, Reason::DependencySelfReference, observation.service.str(),
                "a service cannot depend on itself");
  }
  if (observation.provenance.sequence.is_initial()) {
    return note(explanation, Reason::EvidenceIncomplete, observation.provenance.source.str(),
                "observation sequence zero never names a delivered observation");
  }
  if (observation.provenance.epoch != state.epoch) {
    state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
    return note(explanation, Reason::AuthoritySuperseded, observation.provenance.source.str(),
                "observation was produced under a previous coordinator epoch");
  }
  if (observation.provenance.boot != state.boot) {
    state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
    return note(explanation, Reason::BootMismatch, observation.provenance.source.str(),
                "observation was produced during a previous boot");
  }
  const Digest fingerprint = dependency_fingerprint(observation);
  const auto watermark = state.watermarks.find(observation.provenance.source);
  if (watermark != state.watermarks.end() &&
      observation.provenance.sequence <= watermark->second.sequence) {
    const Digest computed = fingerprint;
    if (observation.provenance.sequence == watermark->second.sequence) {
      if (computed == watermark->second.digest) {
        return note(explanation, Reason::AcceptDuplicateIdempotent,
                    observation.provenance.source.str(), "observation sequence already applied");
      }
      state.stats.evidence_rejected_conflict =
          saturating_inc(state.stats.evidence_rejected_conflict);
      return note(explanation, Reason::EvidenceConflicting, observation.provenance.source.str(),
                  "observation sequence reused with different content");
    }
    state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
    return note(explanation, Reason::EvidenceReplayRejected, observation.provenance.source.str(),
                "observation sequence is older than the recorded watermark");
  }
  if (observation.topology_generation != state.topology_generation) {
    state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
    return note(explanation, Reason::EvidenceTopologyMismatch, observation.service.str(),
                "observation was made under a different topology generation");
  }
  if (!observation.content_digest.is_zero() && observation.content_digest != fingerprint) {
    return note(explanation, Reason::Malformed, observation.service.str(),
                "observation content digest does not match the asserted content");
  }
  DependencyObservation record = observation;
  record.content_digest = fingerprint;
  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return note(explanation, Reason::GenerationExhausted, observation.service.str(),
                "logical tick exhausted");
  }
  record.provenance.tick = stamp;
  CanonicalWriter writer;
  encode(writer, record);
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::DependencyIngested, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return note(explanation, code, observation.service.str(), "durable commit failed");
  }
  state.tick = stamp;
  auto& bucket = state.dependencies[observation.service];
  if (bucket.size() >= config.max_dependencies_per_service) {
    const auto victim = bucket.find(observation.dependency);
    if (victim == bucket.end()) {
      state.stats.dependency_depth_truncated =
          saturating_inc(state.stats.dependency_depth_truncated);
    }
  }
  DependencySlot slot;
  slot.observation = record;
  slot.requires_reconfirmation = false;
  bucket[observation.dependency] = slot;
  SourceWatermark mark;
  mark.sequence = record.provenance.sequence;
  mark.digest = record.content_digest;
  mark.tick = record.provenance.tick;
  state.watermarks[record.provenance.source] = mark;
  state.stats.dependencies_ingested = saturating_inc(state.stats.dependencies_ingested);
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return note(explanation, compact, observation.service.str(), "compaction failed");
  }
  return note(explanation, Reason::AcceptDependencyRecorded, observation.service.str(),
              "dependency observation accepted");
}

// ---------------------------------------------------------------------------
// Qualification, selection and planning
// ---------------------------------------------------------------------------

Reason Fabric::Impl::qualify_dependencies(const ServiceRuntime& runtime,
                                          Explanation& explanation) {
  struct Node {
    ServiceName owner{};
    const ServiceDescriptor* descriptor{nullptr};
    u32 depth{0};
  };
  Reason blocking = Reason::Ok;
  std::set<std::string_view> visited;
  visited.insert(runtime.descriptor.name.view());
  std::vector<Node> queue;
  queue.push_back(Node{runtime.descriptor.name, &runtime.descriptor, 0U});
  std::size_t emitted = 0;
  const u32 max_depth = state.policy.dependency_max_depth;

  for (std::size_t index = 0; index < queue.size(); ++index) {
    const Node node = queue[index];
    if (node.descriptor == nullptr) {
      continue;
    }
    if (node.depth >= max_depth) {
      if (!node.descriptor->dependencies.empty()) {
        state.stats.dependency_depth_truncated =
            saturating_inc(state.stats.dependency_depth_truncated);
        if (emitted < 8) {
          note(explanation, Reason::DependencyDepthExceeded, node.owner.str(),
               "dependency traversal stopped at the policy depth ceiling",
               static_cast<u64>(node.depth));
          ++emitted;
        }
      }
      continue;
    }
    for (const DependencySpec& spec : node.descriptor->dependencies) {
      if (!visited.insert(spec.service.view()).second) {
        if (emitted < 8) {
          note(explanation, Reason::DependencyCycle, spec.service.str(),
               "dependency already visited; traversal stopped on the cycle edge");
          ++emitted;
        }
        if (spec.kind == DependencyKind::Hard && blocking == Reason::Ok) {
          blocking = Reason::DependencyCycle;
        }
        continue;
      }
      const ServiceRuntime* dependency_runtime = service_of(spec.service);
      if (dependency_runtime != nullptr) {
        queue.push_back(Node{spec.service, &dependency_runtime->descriptor, node.depth + 1U});
      }
      const DependencyObservation* observation = nullptr;
      bool stale = false;
      const auto bucket = state.dependencies.find(node.owner);
      if (bucket != state.dependencies.end()) {
        const auto slot = bucket->second.find(spec.service);
        if (slot != bucket->second.end()) {
          observation = &slot->second.observation;
          stale = slot->second.requires_reconfirmation;
        }
      }
      Reason code = Reason::Ok;
      if (dependency_runtime == nullptr) {
        code = Reason::UnknownService;
      } else if (observation == nullptr) {
        code = Reason::DependencyUnknown;
      } else if (stale) {
        code = Reason::DependencyUnresolved;
      } else if (observation->provenance.epoch != state.epoch ||
                 observation->provenance.boot != state.boot) {
        code = Reason::DependencyUnresolved;
      } else if (observation->topology_generation != state.topology_generation) {
        code = Reason::DependencyUnresolved;
      } else if (observation->health == DependencyHealth::Unknown) {
        code = Reason::DependencyUnknown;
      } else if (observation->health == DependencyHealth::Failed) {
        code = Reason::DependencyHardUnmet;
      } else if (observation->health == DependencyHealth::Degraded &&
                 spec.kind == DependencyKind::Hard) {
        code = Reason::DependencyHardUnmet;
      }
      if (code == Reason::Ok) {
        if (emitted < 8) {
          note(explanation, Reason::Ok, spec.service.str(), "dependency satisfied");
          ++emitted;
        }
        continue;
      }
      if (emitted < 8) {
        note(explanation, code, spec.service.str(),
             spec.kind == DependencyKind::Hard ? "hard dependency" : "soft dependency");
        ++emitted;
      }
      if (spec.kind == DependencyKind::Hard && blocking == Reason::Ok) {
        blocking = code;
      }
    }
  }
  return blocking;
}

Reason Fabric::Impl::evaluate_candidate(const ServiceRuntime& runtime, const TargetRecord& record,
                                        Reason& reject) const {
  if (record.scope != runtime.descriptor.scope) {
    reject = Reason::FallbackNotInGovernedScope;
    return reject;
  }
  if (record.incarnation.host.is_initial() || record.incarnation.device.is_initial()) {
    reject = Reason::ImpossibleValue;
    return reject;
  }
  const FenceWitness* witness = witness_of(runtime.descriptor.name, record.name);
  if (witness != nullptr && record.incarnation <= witness->incarnation) {
    reject = Reason::FallbackFenced;
    return reject;
  }
  const CapabilitySlot* slot = capability_of(record.name);
  if (slot == nullptr) {
    reject = Reason::FallbackUnsupported;
    return reject;
  }
  if (slot->requires_reconfirmation) {
    reject = Reason::FallbackCapabilityGenerationStale;
    return reject;
  }
  if (slot->evidence.topology_generation != state.topology_generation) {
    reject = Reason::FallbackCapabilityGenerationStale;
    return reject;
  }
  if (slot->evidence.incarnation != record.incarnation) {
    reject = Reason::FallbackCapabilityGenerationStale;
    return reject;
  }
  if (!slot->evidence.capabilities.contains_all(runtime.descriptor.required_capabilities)) {
    reject = Reason::FallbackIncompatible;
    return reject;
  }
  const FailureEvidence* failure = latest_failure_of(runtime.descriptor.name);
  if (failure != nullptr && failure->target.target == record.name) {
    reject = Reason::FallbackUnhealthy;
    return reject;
  }
  reject = Reason::Ok;
  return Reason::Ok;
}

Reason Fabric::Impl::select_fallback(const ServiceRuntime& runtime, const TargetRef& failed,
                                     Explanation& explanation, TargetRecord& chosen,
                                     Reason& refusal_code) {
  struct Entry {
    const TargetRecord* record{nullptr};
    bool same_host{false};
    u8 rank{0};
  };
  const TargetRecord* failed_record = failed.valid() ? target_of(failed.target) : nullptr;
  std::vector<Entry> entries;
  entries.reserve(state.targets.size());
  for (const auto& entry : state.targets) {
    if (failed.valid() && entry.first == failed.target) {
      continue;
    }
    if (runtime.active.has_value() && entry.first == runtime.active->target) {
      continue;
    }
    Entry candidate;
    candidate.record = &entry.second;
    candidate.same_host = failed_record != nullptr && entry.second.host == failed_record->host;
    candidate.rank = device_rank(entry.second.kind);
    entries.push_back(candidate);
  }
  std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
    if (left.same_host != right.same_host) {
      return !left.same_host;
    }
    if (left.rank != right.rank) {
      return left.rank > right.rank;
    }
    return left.record->name < right.record->name;
  });

  const std::size_t ceiling =
      (std::min)(config.max_fallback_candidates,
                 static_cast<std::size_t>(state.policy.max_fallback_candidates == 0
                                              ? 1
                                              : state.policy.max_fallback_candidates));
  std::size_t considered = entries.size();
  if (considered > ceiling) {
    considered = ceiling;
    state.stats.fallback_candidates_truncated =
        saturating_inc(state.stats.fallback_candidates_truncated);
    note(explanation, Reason::FallbackCandidateLimitReached, runtime.descriptor.name.str(),
         "candidate set ordered canonically then truncated at the configured ceiling",
         static_cast<u64>(entries.size() - considered));
  }

  std::map<Reason, u32> rejections;
  std::size_t emitted = 0;
  for (std::size_t index = 0; index < considered; ++index) {
    Reason reject = Reason::Ok;
    if (evaluate_candidate(runtime, *entries[index].record, reject) == Reason::Ok) {
      chosen = *entries[index].record;
      return Reason::Ok;
    }
    ++rejections[reject];
    if (emitted < 8) {
      note(explanation, reject, entries[index].record->name.str(), "fallback candidate rejected");
      ++emitted;
    }
  }

  if (rejections.empty()) {
    refusal_code = Reason::NoEligibleFallback;
    return refusal_code;
  }
  // Deterministic choice of the reported reason: severity order first, then the
  // numeric reason value.
  const auto severity = [](Reason code) -> int {
    switch (code) {
      case Reason::FallbackFenced:
        return 0;
      case Reason::FallbackIncompatible:
        return 1;
      case Reason::FallbackCapabilityGenerationStale:
        return 2;
      case Reason::FallbackUnsupported:
        return 3;
      case Reason::FallbackUnhealthy:
        return 4;
      case Reason::FallbackNotInGovernedScope:
        return 5;
      default:
        return 6;
    }
  };
  Reason best = rejections.begin()->first;
  for (const auto& entry : rejections) {
    const int left = severity(entry.first);
    const int right = severity(best);
    if (left < right || (left == right && entry.first < best)) {
      best = entry.first;
    }
  }
  refusal_code = best;
  return refusal_code;
}

Reason Fabric::Impl::build_plan(RecoveryPlan& plan, const ServiceRuntime& runtime,
                                const TargetRef& from, const TargetRecord& to,
                                Explanation& explanation) {
  std::vector<PlanStep> steps;
  steps.push_back(PlanStep{PlanStepKind::AdvanceFence, runtime.descriptor.name.str(), true});
  if (from.valid()) {
    steps.push_back(PlanStep{PlanStepKind::FencePreviousTarget, from.target.str(), true});
  }
  steps.push_back(PlanStep{PlanStepKind::TransferLease, to.name.str(), true});
  steps.push_back(PlanStep{PlanStepKind::EmitIntent, to.name.str(), true});
  steps.push_back(PlanStep{PlanStepKind::AwaitActivationEffect, to.name.str(), true});
  if (runtime.descriptor.statefulness == Statefulness::Stateful &&
      runtime.descriptor.preconditions.contains(Precondition::StateTransfer)) {
    steps.push_back(PlanStep{PlanStepKind::AwaitStateTransfer, to.name.str(), true});
  }
  steps.push_back(PlanStep{PlanStepKind::AwaitServiceHealth, to.name.str(), true});
  steps.push_back(PlanStep{PlanStepKind::ClaimContinuity, to.name.str(), true});

  const std::size_t config_ceiling = config.max_plan_steps == 0 ? 1 : config.max_plan_steps;
  const std::size_t policy_ceiling =
      state.policy.max_plan_steps == 0 ? 1 : static_cast<std::size_t>(state.policy.max_plan_steps);
  const std::size_t ceiling = (std::min)(config_ceiling, policy_ceiling);
  if (steps.size() > ceiling) {
    plan.omitted_steps = steps.size() - ceiling;
    plan.steps.assign(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(ceiling));
    note(explanation, Reason::PlanStepLimitReached, runtime.descriptor.name.str(),
         "required recovery steps exceed the policy ceiling", plan.omitted_steps);
    return Reason::PlanStepLimitReached;
  }
  plan.steps = std::move(steps);
  return Reason::Ok;
}

// ---------------------------------------------------------------------------
// Durable commit of one recovery attempt
// ---------------------------------------------------------------------------

Fabric::Impl::CommitResult Fabric::Impl::commit_recovery(const FailoverRequest& request,
                                                         ServiceRuntime& runtime,
                                                         const TargetRecord& to,
                                                         const TargetRef& from, Reason trigger_code,
                                                         CommitMode mode,
                                                         Explanation& explanation) {
  CommitResult result;
  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    result.code = Reason::GenerationExhausted;
    return result;
  }

  std::optional<CancelRegistry::Guard> guard;
  if (!request.key.is_nil()) {
    guard.emplace(cancels, request.key);
    if (!guard->known()) {
      result.code = Reason::RequestUnknown;
      return result;
    }
    if (guard->cancelled()) {
      state.stats.requests_cancelled = saturating_inc(state.stats.requests_cancelled);
      result.code = Reason::Cancelled;
      return result;
    }
  }

  // A resume re-issues the persisted attempt: the fence, the generation and the
  // attempt identity are all reused, so an executor that already saw this intent
  // treats the re-delivery as a duplicate rather than as fresh authority.
  const bool resume_mode = mode == CommitMode::Resume;
  std::optional<FailoverGeneration> next_generation = runtime.generation.successor();
  std::optional<FenceToken> next_fence = fence_token_of(request.service).successor();
  std::optional<LeaseTerm> next_term =
      runtime.lease.has_value() ? runtime.lease->term.successor()
                                : std::optional<LeaseTerm>(LeaseTerm::from_value(1));
  if (resume_mode) {
    next_generation = runtime.generation;
    next_fence = runtime.fence;
    next_term = runtime.lease.has_value() ? runtime.lease->term
                                          : std::optional<LeaseTerm>(LeaseTerm::from_value(1));
  }
  if (!next_generation.has_value()) {
    result.code = Reason::GenerationExhausted;
    return result;
  }
  if (!next_fence.has_value()) {
    result.code = Reason::FenceExhausted;
    return result;
  }
  if (!next_term.has_value()) {
    result.code = Reason::LeaseTermExhausted;
    return result;
  }
  const u64 next_attempt_raw =
      resume_mode ? runtime.current_attempt.value() : state.next_attempt_id;
  if (next_attempt_raw == 0 || next_attempt_raw == (std::numeric_limits<u64>::max)()) {
    result.code = Reason::GenerationExhausted;
    return result;
  }
  const u64 next_lease_raw = state.next_lease_id;
  if (next_lease_raw == 0 || next_lease_raw == (std::numeric_limits<u64>::max)()) {
    result.code = Reason::GenerationExhausted;
    return result;
  }
  const u64 next_plan_raw = state.next_plan_id;
  if (next_plan_raw == 0 || next_plan_raw == (std::numeric_limits<u64>::max)()) {
    result.code = Reason::GenerationExhausted;
    return result;
  }

  const AttemptId attempt = make_attempt_id(next_attempt_raw);
  const LeaseId lease_id = make_lease_id(next_lease_raw);
  const TargetRef to_ref{to.name, to.incarnation};

  RecoveryPlan plan;
  const Reason plan_code = build_plan(plan, runtime, from, to, explanation);
  plan.id = make_plan_id(next_plan_raw);
  plan.service = request.service;
  plan.generation = *next_generation;
  plan.fence = *next_fence;
  plan.lease_term = *next_term;
  plan.from = from;
  plan.to = to_ref;
  result.plan = plan;
  if (plan_code != Reason::Ok) {
    result.code = plan_code;
    return result;
  }

  AttemptRecord attempt_record;
  attempt_record.id = attempt;
  attempt_record.service = request.service;
  attempt_record.generation = *next_generation;
  attempt_record.fence = *next_fence;
  attempt_record.from = from;
  attempt_record.to = to_ref;
  attempt_record.lease_term = *next_term;
  attempt_record.epoch = state.epoch;
  attempt_record.opened = stamp;
  attempt_record.phase = RecoveryPhase::Authorized;
  attempt_record.outcome = AttemptOutcome::Committed;
  attempt_record.durable_committed = true;
  attempt_record.acknowledged = false;
  attempt_record.restart_downgraded = false;
  attempt_record.last_reason = trigger_code;

  ServiceRuntime next = runtime;
  bool replaced = false;
  if (resume_mode) {
    for (AttemptRecord& existing : next.history) {
      if (existing.id == attempt) {
        existing.epoch = state.epoch;
        existing.phase = RecoveryPhase::Authorized;
        existing.outcome = AttemptOutcome::Committed;
        existing.durable_committed = true;
        existing.acknowledged = false;
        existing.restart_downgraded = false;
        existing.last_reason = trigger_code;
        replaced = true;
        break;
      }
    }
  }
  if (!replaced) {
    next.history.push_back(attempt_record);
  }
  while (next.history.size() > config.max_attempt_history) {
    next.history.pop_front();
    next.attempts_evicted = saturating_inc(next.attempts_evicted);
  }
  const u64 evicted_delta = next.attempts_evicted - runtime.attempts_evicted;
  next.generation = *next_generation;
  next.fence = *next_fence;
  next.lease = LeaseRecord{request.service, lease_id, *next_term, runtime.descriptor.scope, to_ref,
                           state.epoch, stamp};
  next.current_attempt = attempt;
  // The attempt ceiling bounds recovery attempts, not the first authoritative
  // placement of a service.
  next.attempts_since_verified =
      mode == CommitMode::InitialPlacement ? runtime.attempts_since_verified
                                           : runtime.attempts_since_verified + 1U;
  next.active = to_ref;
  next.fallback = to_ref;
  next.recovery = RecoveryPhase::Authorized;
  next.lifecycle = LifecyclePhase::RecoveryAuthorized;
  invalidate_verification(next);
  next.failover_recorded = true;
  next.ambiguity_pending = false;
  next.pending_ambiguity = EvidenceId{};
  next.last_reason = trigger_code;
  next.last_change = stamp;
  if (!runtime.pre_failover_target.has_value() && from.valid()) {
    next.pre_failover_target = from;
    const CapabilitySlot* slot = capability_of(from.target);
    next.pre_failover_capability =
        slot != nullptr ? slot->evidence.generation : CapabilityGeneration{};
    next.pre_failover_topology = state.topology_generation;
    next.pre_failover_policy = state.policy_generation;
  }
  if (mode == CommitMode::InitialPlacement) {
    // An initial placement is not a recovery: the placement becomes active
    // immediately, but continuity stays Unknown until an effect is verified.
    next.recovery = RecoveryPhase::IntentRecorded;
    next.lifecycle = LifecyclePhase::Active;
    next.failover_recorded = false;
    next.pre_failover_target.reset();
  }

  FenceRecord fence_record;
  fence_record.service = request.service;
  fence_record.token = *next_fence;
  fence_record.holder = to_ref;
  fence_record.attempt = attempt;
  fence_record.epoch = state.epoch;
  fence_record.issued = stamp;

  std::vector<std::pair<TargetName, FenceWitness>> witness_updates;
  if (from.valid()) {
    FenceWitness witness;
    witness.token = *next_fence;
    witness.incarnation = from.incarnation;
    witness.topology_generation = state.topology_generation;
    witness_updates.emplace_back(from.target, witness);
  }

  RuntimeChange change;
  change.service = request.service;
  change.runtime = next;
  change.fence = fence_record;
  change.has_fence = true;
  change.witnesses = witness_updates;

  CanonicalWriter writer;
  encode_runtime_change(writer, change);
  if (!writer.ok()) {
    result.code = Reason::Oversized;
    return result;
  }
  std::vector<Record> records;
  records.push_back(make_record(
      mode == CommitMode::Resume ? RecordKind::RecoveryResumed : RecordKind::AttemptCommitted,
      writer));
  const Reason persist_code = persist(records);
  if (persist_code != Reason::Ok) {
    result.code = persist_code;
    return result;
  }

  state.tick = stamp;
  if (!resume_mode) {
    state.next_attempt_id = next_attempt_raw + 1U;
  }
  state.next_lease_id = next_lease_raw + 1U;
  state.next_plan_id = next_plan_raw + 1U;
  state.services[request.service] = next;
  state.fences[request.service] = fence_record;
  for (const auto& entry : witness_updates) {
    state.witnesses[WitnessKey{request.service, entry.first}] = entry.second;
  }
  state.stats.attempts_recorded = saturating_inc(state.stats.attempts_recorded);
  for (u64 step = 0; step < evicted_delta; ++step) {
    state.stats.attempts_evicted = saturating_inc(state.stats.attempts_evicted);
  }

  FailoverIntent intent;
  intent.attempt = attempt;
  intent.service = request.service;
  intent.generation = *next_generation;
  intent.fence = *next_fence;
  intent.lease_term = *next_term;
  intent.from = from;
  intent.to = to_ref;
  intent.epoch = state.epoch;
  intent.policy_generation = state.policy_generation;
  intent.topology_generation = state.topology_generation;
  intent.issued = stamp;
  intent.reissue_after_restart = mode == CommitMode::Resume;
  if (intents.push(intent)) {
    state.stats.intents_emitted = saturating_inc(state.stats.intents_emitted);
  } else {
    state.stats.intents_dropped = saturating_inc(state.stats.intents_dropped);
  }

  RequestCacheEntry cache;
  cache.kind = mode == CommitMode::Resume
                   ? RequestKind::Resume
                   : (mode == CommitMode::InitialPlacement ? RequestKind::Placement
                                                           : RequestKind::Failover);
  cache.outcome = Reason::AcceptIntentEmitted;
  cache.accepted = true;
  cache.attempt = attempt;
  cache.generation = *next_generation;
  cache.fence = *next_fence;
  cache.lease_term = *next_term;
  cache.policy_generation = state.policy_generation;
  cache.topology_generation = state.topology_generation;
  cache.from = from;
  cache.to = to_ref;
  cache.has_intent = true;
  cache.plan_step_mask = 0;
  for (const PlanStep& step : plan.steps) {
    cache.plan_step_mask |= (1U << static_cast<u32>(step.kind));
  }
  cache.recorded = stamp;
  {
    const std::string rendered = plan.to_json().dump();
    DigestBuilder builder;
    builder.update(reinterpret_cast<const u8*>(rendered.data()), rendered.size());
    cache.response_digest = builder.value();
  }
  remember(request.key, cache);
  if (guard.has_value()) {
    guard->mark_committed();
  }

  if (!witness_updates.empty()) {
    note(explanation, Reason::AcceptFenceAdvanced, from.target.str(),
         "previous target fenced out at the new durable token", next_fence->value());
  }
  note(explanation, Reason::AcceptFallbackCapabilityCompatible, to.name.str(),
       "fallback capability is supported, current and compatible");
  note(explanation, Reason::AcceptLeaseTransferred, to.name.str(), "lease term transferred",
       next_term->value());
  note(explanation, Reason::AcceptIntentEmitted, to.name.str(), "governed failover intent emitted");

  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    note(explanation, compact, to.name.str(), "compaction after commit failed");
  }

  result.code = Reason::Ok;
  result.intent = intent;
  result.has_intent = true;
  return result;
}

// ---------------------------------------------------------------------------
// Recovery decisions
// ---------------------------------------------------------------------------

RecoveryDecision Fabric::Impl::do_establish_placement(const PlacementRequest& request) {
  Explanation explanation = begin_explanation(request.service, request.key);
  if (!opened) {
    return refusal_simple(std::move(explanation), Reason::ShuttingDown);
  }
  if (const RequestCacheEntry* entry = cached(request.key); entry != nullptr) {
    FailoverRequest replay;
    replay.key = request.key;
    replay.service = request.service;
    return replay_cached(*entry, std::move(explanation), replay);
  }
  ServiceRuntime* runtime = service_of(request.service);
  if (runtime == nullptr) {
    return refusal(std::move(explanation), Reason::UnknownService, request.service.str(),
                   "service is not registered");
  }
  if (!role_satisfies(request.requester, SourceRole::Operator)) {
    state.stats.evidence_rejected_unauthorized =
        saturating_inc(state.stats.evidence_rejected_unauthorized);
    return refusal(std::move(explanation), Reason::NoAuthority, request.requester.str(),
                   "establishing a placement requires an enrolled operator");
  }
  if (runtime->lifecycle == LifecyclePhase::Withdrawn) {
    return refusal(std::move(explanation), Reason::ServiceStateIncompatible,
                   request.service.str(), "service is withdrawn");
  }
  if (runtime->active.has_value()) {
    return refusal(std::move(explanation), Reason::ServiceStateIncompatible, request.service.str(),
                   "service already has an authoritative placement; use recovery or failback");
  }
  const TargetRecord* target = target_of(request.target);
  if (target == nullptr) {
    return refusal(std::move(explanation), Reason::UnknownTarget, request.target.str(),
                   "target is not registered");
  }
  Reason reject = Reason::Ok;
  if (evaluate_candidate(*runtime, *target, reject) != Reason::Ok) {
    return refusal(std::move(explanation), reject, target->name.str(),
                   "the requested placement target is not eligible");
  }
  const Reason dependency_code = qualify_dependencies(*runtime, explanation);
  if (dependency_code != Reason::Ok) {
    return refusal(std::move(explanation), dependency_code, request.service.str(),
                   "a hard dependency is not satisfied by current, fresh evidence");
  }

  FailoverRequest placement_request;
  placement_request.key = request.key;
  placement_request.service = request.service;
  placement_request.requester = request.requester;
  placement_request.trigger = RecoveryTrigger::OperatorCommand;
  placement_request.operator_authorized = true;

  const CommitResult result =
      commit_recovery(placement_request, *runtime, *target, TargetRef{},
                      Reason::AcceptPlacementEstablished, CommitMode::InitialPlacement, explanation);
  if (result.code != Reason::Ok) {
    state.stats.failovers_refused = saturating_inc(state.stats.failovers_refused);
    return refusal(std::move(explanation), result.code, request.service.str(),
                   "the placement commit was refused");
  }
  explanation.outcome = Reason::AcceptPlacementEstablished;
  explanation.accepted = true;
  explanation.failover_generation = result.plan.generation;
  explanation.fence = result.plan.fence;
  state.stats.placements_established = saturating_inc(state.stats.placements_established);
  RecoveryDecision decision;
  decision.outcome = explanation.outcome;
  decision.accepted = true;
  decision.plan = result.plan;
  decision.intent = result.intent;
  decision.explanation = std::move(explanation);
  record_explanation(decision.explanation);
  return decision;
}

RecoveryDecision Fabric::Impl::do_request_failover(const FailoverRequest& request, bool resume) {
  Explanation explanation = begin_explanation(request.service, request.key);
  if (!opened) {
    return refusal_simple(std::move(explanation), Reason::ShuttingDown);
  }
  if (const RequestCacheEntry* entry = cached(request.key); entry != nullptr) {
    return replay_cached(*entry, std::move(explanation), request);
  }
  ServiceRuntime* runtime = service_of(request.service);
  if (runtime == nullptr) {
    return refusal(std::move(explanation), Reason::UnknownService, request.service.str(),
                   "service is not registered");
  }
  const auto refuse = [&](Reason code, std::string detail, u64 value = 0) -> RecoveryDecision {
    runtime->last_reason = code;
    return refusal(std::move(explanation), code, request.service.str(), std::move(detail), value);
  };

  if (runtime->lifecycle == LifecyclePhase::Withdrawn) {
    return refuse(Reason::ServiceStateIncompatible, "service is withdrawn");
  }

  if (resume) {
    if (runtime->recovery != RecoveryPhase::IntentRecorded) {
      return refuse(Reason::NoFailoverInProgress,
                    "no restart-downgraded attempt is waiting for authorization");
    }
    if (!role_satisfies(request.requester, SourceRole::Operator)) {
      return refuse(Reason::NoAuthority, "resuming recovery requires an enrolled operator");
    }
    const AttemptRecord* previous = runtime->current_attempt_record();
    if (previous == nullptr) {
      return refuse(Reason::NoFailoverInProgress, "the downgraded attempt record is missing");
    }
    const TargetRecord* to = target_of(previous->to.target);
    if (to == nullptr) {
      return refuse(Reason::UnknownTarget, "the planned fallback target is no longer registered");
    }
    if (to->incarnation != previous->to.incarnation) {
      return refuse(Reason::IncarnationMismatch,
                    "the planned fallback target has been re-adopted at a different incarnation");
    }
    Reason reject = Reason::Ok;
    if (evaluate_candidate(*runtime, *to, reject) != Reason::Ok) {
      return refuse(reject, "the planned fallback target is no longer eligible");
    }
    note(explanation, Reason::AcceptRecoveryReissueSameFence, to->name.str(),
         "re-issuing the persisted attempt under the new epoch with the same fence",
         previous->fence.value());
    state.stats.recovery_reissues = saturating_inc(state.stats.recovery_reissues);
    const CommitResult result =
        commit_recovery(request, *runtime, *to, previous->from,
                        Reason::AcceptRecoveryReissueSameFence, CommitMode::Resume, explanation);
    if (result.code != Reason::Ok) {
      state.stats.failovers_refused = saturating_inc(state.stats.failovers_refused);
      return refuse(result.code, "the re-issued recovery was refused");
    }
    explanation.outcome = Reason::AcceptIntentEmitted;
    explanation.accepted = true;
    explanation.failover_generation = result.plan.generation;
    explanation.fence = result.plan.fence;
    RecoveryDecision decision;
    decision.outcome = explanation.outcome;
    decision.accepted = true;
    decision.plan = result.plan;
    decision.intent = result.intent;
    decision.explanation = std::move(explanation);
    record_explanation(decision.explanation);
    return decision;
  }

  if (runtime->lifecycle == LifecyclePhase::RecoveryAuthorized ||
      runtime->lifecycle == LifecyclePhase::RecoveryInFlight) {
    return refuse(Reason::FailoverAlreadyInProgress,
                  "a recovery attempt is already authorized for this service");
  }
  if (runtime->attempts_since_verified >= state.policy.max_attempts_per_generation) {
    return refuse(Reason::AttemptBudgetExhausted,
                  "the policy attempt ceiling was reached without a verified effect",
                  runtime->attempts_since_verified);
  }

  const FailureEvidence* evidence = nullptr;
  Reason trigger_code = Reason::AcceptQualifiedPrimaryFailure;
  if (request.trigger == RecoveryTrigger::FailureEvidence) {
    if (request.evidence.is_nil()) {
      const TargetRecord* active_target =
          runtime->active.has_value() ? target_of(runtime->active->target) : nullptr;
      if (runtime->active.has_value() && active_target == nullptr) {
        return refuse(Reason::TargetDisappearanceIsNotPermission,
                      "the active target is absent from topology, but absence is not permission "
                      "to activate a replacement without authoritative failure evidence");
      }
      return refuse(Reason::EvidenceMissing,
                    "no failure evidence was supplied for this recovery request");
    }
    evidence = failure_of(request.evidence);
    if (evidence == nullptr) {
      return refuse(Reason::EvidenceMissing,
                    "the referenced failure evidence is not retained", request.evidence.value());
    }
    if (evidence->service != request.service) {
      return refuse(Reason::EvidenceIncomplete, "evidence names a different service");
    }
    if (!runtime->active.has_value()) {
      return refuse(Reason::InvalidState, "service has no active placement to recover from");
    }
    if (evidence->target != *runtime->active) {
      return refuse(Reason::EvidenceTargetMismatch,
                    "evidence does not name the current active placement");
    }
    if (evidence->provenance.epoch != state.epoch) {
      return refuse(Reason::AuthoritySuperseded,
                    "evidence was accepted under a previous coordinator epoch",
                    evidence->provenance.epoch.value());
    }
    if (evidence->provenance.boot != state.boot) {
      return refuse(Reason::BootMismatch, "evidence was accepted during a previous boot",
                    evidence->provenance.boot.value());
    }
    if (evidence->provenance.tick > state.tick) {
      return refuse(Reason::ImpossibleValue, "evidence carries a future logical tick");
    }
    const u64 age = state.tick.value() - evidence->provenance.tick.value();
    if (age > static_cast<u64>(state.policy.evidence_max_age_ticks)) {
      state.stats.evidence_rejected_stale = saturating_inc(state.stats.evidence_rejected_stale);
      return refuse(Reason::EvidenceStale,
                    "evidence is older than the policy freshness bound", age);
    }
    if (evidence->topology_generation != state.topology_generation) {
      return refuse(Reason::EvidenceTopologyMismatch,
                    "evidence was observed under a different topology generation",
                    evidence->topology_generation.value());
    }
    if (evidence->policy_generation > state.policy_generation) {
      return refuse(Reason::FutureGeneration,
                    "evidence cites a policy generation newer than the current one",
                    evidence->policy_generation.value());
    }
    if (evidence->ambiguity != AmbiguityState::Unambiguous) {
      runtime->ambiguity_pending = true;
      runtime->pending_ambiguity = evidence->id;
      runtime->recovery = RecoveryPhase::AmbiguityPending;
      runtime->lifecycle = LifecyclePhase::RecoveryPending;
      invalidate_verification(*runtime);
      runtime->last_reason = Reason::AmbiguityUnresolved;
      state.stats.ambiguity_pending = saturating_inc(state.stats.ambiguity_pending);
      note(explanation, Reason::AmbiguityDualActiveRisk, request.service.str(),
           "activating a replacement while the previous target may still be live risks dual "
           "active operation");
      return refuse(Reason::AmbiguityUnresolved,
                    "the failure evidence is ambiguous and cannot justify recovery",
                    evidence->id.value());
    }
    switch (evidence->failure_class) {
      case FailureClass::TargetMissing:
        trigger_code = Reason::AcceptQualifiedPrimaryFailure;
        break;
      case FailureClass::OperatorDeclared:
        trigger_code = Reason::AcceptQualifiedPlannedRecovery;
        break;
      default:
        trigger_code = Reason::AcceptQualifiedPrimaryFailure;
        break;
    }
  } else {
    if (!request.operator_authorized) {
      return refuse(Reason::NoAuthority,
                    "planned or operator-triggered recovery requires explicit authorization");
    }
    if (!role_satisfies(request.requester, SourceRole::Operator)) {
      return refuse(Reason::NoAuthority, "requester is not an enrolled operator");
    }
    trigger_code = Reason::AcceptQualifiedPlannedRecovery;
  }

  if (runtime->descriptor.preconditions.contains(Precondition::OperatorAuthorization) &&
      !request.operator_authorized) {
    return refuse(Reason::NoAuthority,
                  "the service requires explicit operator authorization for recovery");
  }
  if (evidence != nullptr) {
    note(explanation, trigger_code, request.service.str(), "failure evidence qualified",
         evidence->id.value());
  } else {
    note(explanation, trigger_code, request.service.str(), "authorized non-evidence trigger");
  }

  const Reason dependency_code = qualify_dependencies(*runtime, explanation);
  if (dependency_code != Reason::Ok) {
    return refuse(dependency_code,
                  "a hard dependency is not satisfied by current, fresh evidence");
  }

  const TargetRef from = runtime->active.value_or(TargetRef{});
  TargetRecord chosen;
  Reason fallback_code = Reason::Ok;
  if (select_fallback(*runtime, from, explanation, chosen, fallback_code) != Reason::Ok) {
    return refuse(fallback_code,
                  "no eligible fallback satisfies scope, fence, capability and health rules");
  }
  const CapabilitySlot* chosen_capability = capability_of(chosen.name);
  note(explanation, Reason::AcceptFallbackCapabilityCompatible, chosen.name.str(),
       "fallback capability set covers the required set",
       chosen_capability != nullptr ? chosen_capability->evidence.generation.value() : 0);

  const CommitResult result = commit_recovery(request, *runtime, chosen, from, trigger_code,
                                              CommitMode::Failover, explanation);
  if (result.code != Reason::Ok) {
    state.stats.failovers_refused = saturating_inc(state.stats.failovers_refused);
    RecoveryDecision refused = refuse(result.code, "the recovery commit was refused");
    refused.plan = result.plan;
    return refused;
  }

  explanation.outcome = Reason::AcceptIntentEmitted;
  explanation.accepted = true;
  explanation.failover_generation = result.plan.generation;
  explanation.fence = result.plan.fence;
  state.stats.failovers_accepted = saturating_inc(state.stats.failovers_accepted);
  RecoveryDecision decision;
  decision.outcome = explanation.outcome;
  decision.accepted = true;
  decision.plan = result.plan;
  decision.intent = result.intent;
  decision.explanation = std::move(explanation);
  record_explanation(decision.explanation);
  return decision;
}

RecoveryDecision Fabric::Impl::do_resolve_ambiguity(const AmbiguityResolution& resolution) {
  Explanation explanation = begin_explanation(resolution.service, resolution.key);
  if (!opened) {
    return refusal_simple(std::move(explanation), Reason::ShuttingDown);
  }
  if (const RequestCacheEntry* entry = cached(resolution.key); entry != nullptr) {
    FailoverRequest request;
    request.key = resolution.key;
    request.service = resolution.service;
    return replay_cached(*entry, std::move(explanation), request);
  }
  ServiceRuntime* runtime = service_of(resolution.service);
  if (runtime == nullptr) {
    return refusal(std::move(explanation), Reason::UnknownService, resolution.service.str(),
                   "service is not registered");
  }
  if (!role_satisfies(resolution.resolved_by, SourceRole::Operator)) {
    state.stats.evidence_rejected_unauthorized =
        saturating_inc(state.stats.evidence_rejected_unauthorized);
    return refusal(std::move(explanation), Reason::NoAuthority, resolution.resolved_by.str(),
                   "ambiguity resolution requires an enrolled operator");
  }
  if (!runtime->ambiguity_pending) {
    return refusal(std::move(explanation), Reason::AmbiguityAlreadyResolved,
                   resolution.service.str(), "no ambiguous outcome is pending");
  }
  if (resolution.evidence != runtime->pending_ambiguity) {
    return refusal(std::move(explanation), Reason::EvidenceIncomplete, resolution.service.str(),
                   "resolution does not name the pending ambiguous evidence",
                   runtime->pending_ambiguity.value());
  }
  const FailureEvidence* evidence = failure_of(resolution.evidence);
  if (evidence == nullptr) {
    return refusal(std::move(explanation), Reason::EvidenceMissing, resolution.service.str(),
                   "the pending ambiguous evidence is no longer retained");
  }

  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return refusal(std::move(explanation), Reason::GenerationExhausted, resolution.service.str(),
                   "logical tick exhausted");
  }

  FailoverRequest request;
  request.key = resolution.key;
  request.service = resolution.service;
  request.evidence = resolution.evidence;
  request.requester = resolution.resolved_by;
  request.operator_authorized = resolution.confirm_failure;
  request.trigger = resolution.confirm_failure ? RecoveryTrigger::FailureEvidence
                                               : RecoveryTrigger::OperatorCommand;

  if (!resolution.confirm_failure) {
    ServiceRuntime next = *runtime;
    next.ambiguity_pending = false;
    next.pending_ambiguity = EvidenceId{};
    next.recovery = RecoveryPhase::Refused;
    next.lifecycle = LifecyclePhase::Failed;
    next.continuity = ContinuityClass::None;
    next.last_change = stamp;
    next.last_reason = Reason::AcceptAmbiguityResolvedNoFailover;
    RuntimeChange change;
    change.service = resolution.service;
    change.runtime = next;
    CanonicalWriter writer;
    encode_runtime_change(writer, change);
    std::vector<Record> records;
    records.push_back(make_record(RecordKind::AmbiguityResolved, writer));
    const Reason code = persist(records);
    if (code != Reason::Ok) {
      return refusal(std::move(explanation), code, resolution.service.str(),
                     "durable commit failed");
    }
    state.tick = stamp;
    state.services[resolution.service] = next;
    state.stats.ambiguity_resolved = saturating_inc(state.stats.ambiguity_resolved);
    const Reason compact = rotate_if_due();
    if (compact != Reason::Ok) {
      return refusal(std::move(explanation), compact, resolution.service.str(),
                     "compaction failed");
    }
    explanation.outcome = Reason::AcceptAmbiguityResolvedNoFailover;
    explanation.accepted = true;
    note(explanation, Reason::AcceptAmbiguityResolvedNoFailover, resolution.service.str(),
         "operator declared the target alive; no replacement was activated");
    RecoveryDecision decision;
    decision.outcome = explanation.outcome;
    decision.accepted = true;
    decision.explanation = std::move(explanation);
    record_explanation(decision.explanation);
    return decision;
  }

  // The operator confirmed the disappearance. The evidence is promoted to
  // unambiguous for this service and the recovery proceeds through the ordinary
  // qualification path; nothing is bypassed.
  FailureEvidence promoted = *evidence;
  promoted.ambiguity = AmbiguityState::Unambiguous;
  promoted.content_digest = Digest{};
  CanonicalWriter writer;
  encode(writer, promoted);
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::FailureIngested, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return refusal(std::move(explanation), code, resolution.service.str(),
                   "durable commit failed");
  }
  state.tick = stamp;
  state.failures[promoted.id] = promoted;
  state.services[resolution.service].ambiguity_pending = false;
  state.services[resolution.service].pending_ambiguity = EvidenceId{};
  state.stats.ambiguity_resolved = saturating_inc(state.stats.ambiguity_resolved);
  note(explanation, Reason::AcceptAmbiguityResolvedFailover, resolution.service.str(),
       "operator confirmed the failure; recovery proceeds through ordinary qualification");
  return do_request_failover(request, false);
}

RecoveryDecision Fabric::Impl::do_request_failback(const FailbackRequest& request) {
  Explanation explanation = begin_explanation(request.service, request.key);
  if (!opened) {
    return refusal_simple(std::move(explanation), Reason::ShuttingDown);
  }
  if (const RequestCacheEntry* entry = cached(request.key); entry != nullptr) {
    FailoverRequest replay;
    replay.key = request.key;
    replay.service = request.service;
    return replay_cached(*entry, std::move(explanation), replay);
  }
  ServiceRuntime* runtime = service_of(request.service);
  if (runtime == nullptr) {
    return refusal(std::move(explanation), Reason::UnknownService, request.service.str(),
                   "service is not registered");
  }
  const auto refuse = [&](Reason code, std::string detail, u64 value = 0) -> RecoveryDecision {
    state.stats.failbacks_refused = saturating_inc(state.stats.failbacks_refused);
    return refusal(std::move(explanation), code, request.service.str(), std::move(detail), value);
  };
  if (!state.policy.allow_failback) {
    return refuse(Reason::FailbackNotEligible, "the current policy does not permit failback");
  }
  if (!runtime->failover_recorded || !runtime->pre_failover_target.has_value()) {
    return refuse(Reason::FailbackNoFailoverRecorded,
                  "no failover was recorded for this service");
  }
  if (request.observed_topology_generation != state.topology_generation) {
    return refuse(Reason::FailbackStaleGeneration,
                  "the request cites a topology generation that is not current",
                  state.topology_generation.value());
  }
  if (request.observed_policy_generation != state.policy_generation) {
    return refuse(Reason::FailbackStaleGeneration,
                  "the request cites a policy generation that is not current",
                  state.policy_generation.value());
  }
  const TargetRef original = *runtime->pre_failover_target;
  if (request.preferred_target != original.target) {
    return refuse(Reason::FailbackNotEligible,
                  "the preferred target is not the recorded pre-failover target");
  }
  const TargetRecord* record = target_of(original.target);
  if (record == nullptr) {
    return refuse(Reason::FailbackOriginalUnavailable,
                  "the original target is no longer registered");
  }
  if (record->incarnation <= original.incarnation) {
    return refuse(Reason::FailbackOriginalFenceStale,
                  "the original target has not been re-adopted at a newer incarnation",
                  original.incarnation.host.value());
  }
  const CapabilitySlot* slot = capability_of(original.target);
  if (slot == nullptr) {
    return refuse(Reason::FallbackUnsupported,
                  "no capability evidence is retained for the original target");
  }
  if (slot->requires_reconfirmation || slot->evidence.topology_generation != state.topology_generation ||
      slot->evidence.incarnation != record->incarnation) {
    return refuse(Reason::FallbackCapabilityGenerationStale,
                  "capability evidence for the original target is not current for its new "
                  "incarnation");
  }
  if (runtime->pre_failover_capability.is_initial() ||
      slot->evidence.generation <= runtime->pre_failover_capability) {
    return refuse(Reason::FailbackStaleGeneration,
                  "failback would reuse the capability generation that was already superseded",
                  runtime->pre_failover_capability.value());
  }
  if (!slot->evidence.capabilities.contains_all(runtime->descriptor.required_capabilities)) {
    return refuse(Reason::FallbackIncompatible,
                  "the original target no longer supports the required capability set");
  }
  if (runtime->lifecycle == LifecyclePhase::RecoveryAuthorized ||
      runtime->lifecycle == LifecyclePhase::RecoveryInFlight) {
    return refuse(Reason::FailbackInProgress, "a recovery attempt is already authorized");
  }

  const Reason dependency_code = qualify_dependencies(*runtime, explanation);
  if (dependency_code != Reason::Ok) {
    return refuse(dependency_code, "a hard dependency is not satisfied");
  }
  note(explanation, Reason::AcceptFailbackGenerationFresh, record->name.str(),
       "original target re-adopted at a newer incarnation with a newer capability generation",
       slot->evidence.generation.value());

  const TargetRef from = runtime->active.value_or(TargetRef{});
  const CommitResult result =
      commit_recovery(FailoverRequest{request.key, request.service, RecoveryTrigger::OperatorCommand,
                                      EvidenceId{}, request.requester, true},
                      *runtime, *record, from, Reason::AcceptFailbackGenerationFresh,
                      CommitMode::Failover, explanation);
  if (result.code != Reason::Ok) {
    return refuse(result.code, "the failback commit was refused");
  }
  ServiceRuntime& updated = state.services[request.service];
  updated.pre_failover_target.reset();
  updated.failover_recorded = false;
  explanation.outcome = Reason::AcceptFailbackGenerationFresh;
  explanation.accepted = true;
  explanation.failover_generation = result.plan.generation;
  explanation.fence = result.plan.fence;
  state.stats.failbacks_accepted = saturating_inc(state.stats.failbacks_accepted);
  RecoveryDecision decision;
  decision.outcome = explanation.outcome;
  decision.accepted = true;
  decision.plan = result.plan;
  decision.intent = result.intent;
  decision.explanation = std::move(explanation);
  record_explanation(decision.explanation);
  return decision;
}

RecoveryDecision Fabric::Impl::do_acknowledge_intent(const IntentAck& ack) {
  Explanation explanation = begin_explanation(ServiceName{}, ack.key);
  if (!opened) {
    return refusal_simple(std::move(explanation), Reason::ShuttingDown);
  }
  ServiceRuntime* runtime = nullptr;
  for (auto& entry : state.services) {
    if (entry.second.current_attempt == ack.attempt) {
      runtime = &entry.second;
      break;
    }
  }
  if (runtime == nullptr) {
    return refusal(std::move(explanation), Reason::NoFailoverInProgress, std::string{},
                   "no attempt matches the acknowledgement", ack.attempt.value());
  }
  explanation.service = runtime->descriptor.name;
  explanation.epoch = state.epoch;
  explanation.policy_generation = state.policy_generation;
  explanation.topology_generation = state.topology_generation;
  explanation.failover_generation = runtime->generation;
  explanation.fence = runtime->fence;

  if (ack.generation != runtime->generation) {
    return refusal(std::move(explanation), Reason::SupersededGeneration, runtime->descriptor.name.str(),
                   "the acknowledgement cites a superseded failover generation");
  }
  if (ack.fence < runtime->fence) {
    return refusal(std::move(explanation), Reason::FenceStale, runtime->descriptor.name.str(),
                   "the acknowledgement cites a stale durable fence", runtime->fence.value());
  }
  if (ack.fence > runtime->fence) {
    return refusal(std::move(explanation), Reason::FenceRegression, runtime->descriptor.name.str(),
                   "the acknowledgement cites a fence this runtime never minted");
  }
  AttemptRecord* attempt = runtime->current_attempt_record();
  if (attempt == nullptr) {
    return refusal(std::move(explanation), Reason::NoFailoverInProgress,
                   runtime->descriptor.name.str(), "the attempt record is missing");
  }
  if (attempt->acknowledged) {
    return refusal(std::move(explanation), Reason::AcceptDuplicateIdempotent,
                   runtime->descriptor.name.str(), "the attempt was already acknowledged");
  }
  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return refusal(std::move(explanation), Reason::GenerationExhausted,
                   runtime->descriptor.name.str(), "logical tick exhausted");
  }
  ServiceRuntime next = *runtime;
  AttemptRecord* next_attempt = nullptr;
  for (auto it = next.history.rbegin(); it != next.history.rend(); ++it) {
    if (it->id == ack.attempt) {
      next_attempt = &(*it);
      break;
    }
  }
  if (next_attempt == nullptr) {
    return refusal(std::move(explanation), Reason::NoFailoverInProgress,
                   runtime->descriptor.name.str(), "the attempt record is missing");
  }
  next_attempt->acknowledged = true;
  if (ack.accepted) {
    if (next_attempt->outcome == AttemptOutcome::Committed) {
      next_attempt->outcome = AttemptOutcome::Acknowledged;
    }
    // An acknowledgement never rewinds a phase that already reached a verified
    // or reported state for this attempt.
    if (next.recovery != RecoveryPhase::Verified &&
        next.recovery != RecoveryPhase::EffectReported) {
      next.recovery = RecoveryPhase::ActivationRequested;
    }
    next.last_reason = Reason::AcceptIntentEmitted;
  } else {
    next_attempt->outcome = AttemptOutcome::Refused;
    next_attempt->last_reason = Reason::TransitionRefused;
    next.recovery = RecoveryPhase::Refused;
    next.lifecycle = LifecyclePhase::Failed;
    next.last_reason = Reason::TransitionRefused;
    invalidate_verification(next);
  }
  next.last_change = stamp;

  RuntimeChange change;
  change.service = runtime->descriptor.name;
  change.runtime = next;
  CanonicalWriter writer;
  encode_runtime_change(writer, change);
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::AttemptAcknowledged, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return refusal(std::move(explanation), code, runtime->descriptor.name.str(),
                   "durable commit failed");
  }
  const ServiceName service = runtime->descriptor.name;
  state.tick = stamp;
  state.services[service] = next;
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return refusal(std::move(explanation), compact, service.str(), "compaction failed");
  }
  explanation.outcome = ack.accepted ? Reason::AcceptIntentEmitted : Reason::TransitionRefused;
  explanation.accepted = ack.accepted;
  note(explanation, explanation.outcome, ack.executor.str(),
       ack.accepted ? "executor accepted the governed intent"
                    : "executor refused the governed intent");
  RecoveryDecision decision;
  decision.outcome = explanation.outcome;
  decision.accepted = ack.accepted;
  decision.explanation = std::move(explanation);
  record_explanation(decision.explanation);
  return decision;
}

RecoveryDecision Fabric::Impl::do_report_effect(const EffectReport& report) {
  Explanation explanation = begin_explanation(report.service, RequestKey{});
  if (!opened) {
    return refusal_simple(std::move(explanation), Reason::ShuttingDown);
  }
  ServiceRuntime* runtime = service_of(report.service);
  if (runtime == nullptr) {
    return refusal(std::move(explanation), Reason::UnknownService, report.service.str(),
                   "service is not registered");
  }
  const auto refuse = [&](Reason code, std::string detail, u64 value = 0) -> RecoveryDecision {
    state.stats.effects_refused = saturating_inc(state.stats.effects_refused);
    return refusal(std::move(explanation), code, report.service.str(), std::move(detail), value);
  };
  if (config.require_enrolled_effect_reporter &&
      !role_satisfies(report.provenance.source, SourceRole::EffectReporter)) {
    state.stats.evidence_rejected_unauthorized =
        saturating_inc(state.stats.evidence_rejected_unauthorized);
    return refuse(Reason::EffectReporterUnauthorized,
                  "source is not an enrolled effect reporter");
  }
  if (report.provenance.epoch != state.epoch) {
    return refuse(Reason::AuthoritySuperseded,
                  "the effect was reported under a previous coordinator epoch",
                  report.provenance.epoch.value());
  }
  if (report.provenance.boot != state.boot) {
    return refuse(Reason::BootMismatch, "the effect was reported during a previous boot",
                  report.provenance.boot.value());
  }
  if (runtime->current_attempt.is_nil()) {
    return refuse(Reason::NoFailoverInProgress, "no recovery attempt is outstanding");
  }
  if (report.attempt != runtime->current_attempt) {
    return refuse(Reason::EffectAttemptMismatch, "the effect names a different attempt",
                  runtime->current_attempt.value());
  }
  if (report.generation != runtime->generation) {
    return refuse(Reason::SupersededGeneration, "the effect cites a superseded generation",
                  runtime->generation.value());
  }
  if (report.fence < runtime->fence) {
    return refuse(Reason::FenceStale, "the effect cites a stale durable fence",
                  runtime->fence.value());
  }
  if (report.fence > runtime->fence) {
    return refuse(Reason::FenceRegression, "the effect cites a fence never minted here");
  }
  if (!runtime->fallback.has_value() || report.target.target != runtime->fallback->target) {
    return refuse(Reason::EffectMismatch, "the effect names a target that is not the planned one");
  }
  const TargetRecord* record = target_of(report.target.target);
  if (record == nullptr) {
    return refuse(Reason::UnknownTarget, "the effect names an unregistered target");
  }
  if (record->incarnation != report.target.incarnation) {
    return refuse(Reason::IncarnationMismatch,
                  "the effect names an incarnation that is not current for this target");
  }
  if (report.target != *runtime->fallback) {
    return refuse(Reason::EffectMismatch,
                  "the effect names an incarnation that was never planned for this attempt");
  }
  for (const EffectId& seen : runtime->effects_seen) {
    if (!report.id.is_nil() && seen == report.id) {
      state.stats.effect_reports_replayed = saturating_inc(state.stats.effect_reports_replayed);
      explanation.outcome = Reason::AcceptDuplicateIdempotent;
      explanation.accepted = true;
      note(explanation, Reason::AcceptDuplicateIdempotent, report.service.str(),
           "effect report already applied", report.id.value());
      RecoveryDecision decision;
      decision.outcome = explanation.outcome;
      decision.accepted = true;
      decision.duplicate = true;
      decision.explanation = std::move(explanation);
      record_explanation(decision.explanation);
      return decision;
    }
  }
  if (runtime->recovery == RecoveryPhase::Verified && report.kind != EffectKind::ServiceHealthy) {
    return refuse(Reason::EffectAlreadyVerified,
                  "the attempt is already verified; no further effect is accepted");
  }

  LogicalTick stamp;
  if (!peek_tick(stamp)) {
    return refuse(Reason::GenerationExhausted, "logical tick exhausted");
  }

  ServiceRuntime next = *runtime;
  next.effects_seen.push_back(report.id);
  while (next.effects_seen.size() > config.max_attempt_history) {
    next.effects_seen.pop_front();
  }
  next.last_change = stamp;
  Reason outcome = Reason::AcceptEffectVerified;

  if (report.result == EffectResult::Negative) {
    // A negative effect is a completed, unsuccessful activation: the attempt is
    // over, the placement is not serving, and a further attempt is admissible
    // (subject to the policy attempt ceiling).
    next.last_reason = Reason::EffectNegative;
    next.recovery = RecoveryPhase::EffectReported;
    next.lifecycle = LifecyclePhase::Failed;
    invalidate_verification(next);
    outcome = Reason::EffectNegative;
  } else {
    switch (report.kind) {
      case EffectKind::ActivationAccepted:
        next.activation_accepted = true;
        next.recovery = RecoveryPhase::EffectReported;
        next.last_reason = Reason::AcceptEffectVerified;
        outcome = Reason::AcceptEffectVerified;
        break;
      case EffectKind::DeactivationAcknowledged:
        next.recovery = RecoveryPhase::EffectReported;
        next.last_reason = Reason::AcceptEffectVerified;
        outcome = Reason::AcceptEffectVerified;
        break;
      case EffectKind::StateTransferComplete:
        if (report.state_generation < runtime->descriptor.last_known_state_generation) {
          return refuse(Reason::StateGenerationStale,
                        "the transferred state generation is older than the last known durable "
                        "generation",
                        runtime->descriptor.last_known_state_generation.value());
        }
        next.state_transfer_verified = true;
        next.last_reason = Reason::AcceptStateTransferVerified;
        outcome = Reason::AcceptStateTransferVerified;
        break;
      case EffectKind::ServiceHealthy:
        if (!runtime->activation_accepted) {
          return refuse(Reason::EffectUnverified,
                        "a healthy report is not sufficient without a preceding accepted "
                        "activation for this attempt");
        }
        next.effect_verified = true;
        next.recovery = RecoveryPhase::Verified;
        next.lifecycle = LifecyclePhase::FallbackActive;
        next.attempts_since_verified = 0;
        if (runtime->descriptor.statefulness == Statefulness::Stateless) {
          next.continuity = ContinuityClass::Full;
          next.last_reason = Reason::AcceptContinuityVerified;
          outcome = Reason::AcceptContinuityVerified;
        } else if (runtime->state_transfer_verified) {
          next.continuity = ContinuityClass::Full;
          next.last_reason = Reason::AcceptContinuityVerified;
          outcome = Reason::AcceptContinuityVerified;
        } else if (state.policy.allow_degraded_continuity) {
          next.continuity = ContinuityClass::Degraded;
          next.last_reason = Reason::AcceptContinuityDegradedStateful;
          outcome = Reason::AcceptContinuityDegradedStateful;
        } else {
          next.continuity = ContinuityClass::None;
          next.last_reason = Reason::ContinuityNotVerified;
          outcome = Reason::ContinuityNotVerified;
        }
        break;
      default:
        return refuse(Reason::EffectUnsupportedKind, "unsupported effect kind");
    }
  }

  RuntimeChange change;
  change.service = report.service;
  change.runtime = next;
  CanonicalWriter writer;
  encode_runtime_change(writer, change);
  if (!writer.ok()) {
    return refuse(Reason::Oversized, "effect record exceeded the configured ceiling");
  }
  std::vector<Record> records;
  records.push_back(make_record(RecordKind::EffectRecorded, writer));
  const Reason code = persist(records);
  if (code != Reason::Ok) {
    return refuse(code, "durable commit failed");
  }
  state.tick = stamp;
  state.services[report.service] = next;
  if (report.result == EffectResult::Negative) {
    state.stats.effects_refused = saturating_inc(state.stats.effects_refused);
  } else {
    state.stats.effects_accepted = saturating_inc(state.stats.effects_accepted);
    if (next.effect_verified) {
      state.stats.effects_verified = saturating_inc(state.stats.effects_verified);
    }
  }
  const Reason compact = rotate_if_due();
  if (compact != Reason::Ok) {
    return refuse(compact, "compaction failed");
  }

  explanation.outcome = outcome;
  explanation.accepted = report.result != EffectResult::Negative &&
                         next.recovery != RecoveryPhase::None;
  if (next.effect_verified) {
    explanation.failover_generation = next.generation;
    explanation.fence = next.fence;
  }
  note(explanation, outcome, report.service.str(), "effect report evaluated",
       report.id.value());
  if (next.effect_verified && next.continuity != ContinuityClass::Full) {
    note(explanation, Reason::ContinuityNotVerified, report.service.str(),
         "continuity is not claimed at the full class for this service");
  }
  RecoveryDecision decision;
  decision.outcome = explanation.outcome;
  decision.accepted = explanation.accepted;
  decision.explanation = std::move(explanation);
  record_explanation(decision.explanation);
  return decision;
}

// ---------------------------------------------------------------------------
// Lifecycle, recovery and observation
// ---------------------------------------------------------------------------

Reason Fabric::Impl::recover_from_store(std::vector<Record>& records) {
  for (const Record& record : records) {
    if (!apply_record(record)) {
      return Reason::StoreCorrupt;
    }
  }
  return Reason::Ok;
}

void Fabric::Impl::apply_restart_recovery() {
  const RecoveryReport store_report = restart.store;
  restart = RestartSummary{};
  restart.store = store_report;
  restart.previous_epoch = state.epoch;

  const std::optional<CoordinatorEpoch> next_epoch = state.epoch.successor();
  const std::optional<BootId> next_boot = state.boot.successor();
  state.epoch = next_epoch.value_or(CoordinatorEpoch::from_value(1));
  state.boot = next_boot.value_or(BootId::from_value(1));
  if (state.epoch.is_initial()) {
    state.epoch = CoordinatorEpoch::from_value(1);
  }
  if (state.boot.is_initial()) {
    state.boot = BootId::from_value(1);
  }
  restart.epoch = state.epoch;
  restart.boot = state.boot;

  u32 attempts_unknown = 0;
  for (auto& entry : state.services) {
    ServiceRuntime& runtime = entry.second;
    bool downgraded = false;
    for (AttemptRecord& attempt : runtime.history) {
      if (attempt.durable_committed && !attempt.acknowledged) {
        attempt.outcome = AttemptOutcome::OutcomeUnknown;
        attempt.restart_downgraded = true;
        attempt.last_reason = Reason::RestartConservativeDowngrade;
        downgraded = true;
        ++attempts_unknown;
      }
    }
    switch (runtime.recovery) {
      case RecoveryPhase::IntentRecorded:
      case RecoveryPhase::Authorized:
      case RecoveryPhase::ActivationRequested:
      case RecoveryPhase::EffectReported:
        runtime.recovery = RecoveryPhase::IntentRecorded;
        runtime.lifecycle = LifecyclePhase::RecoveryPending;
        runtime.last_reason = Reason::RestartConservativeDowngrade;
        downgraded = true;
        break;
      case RecoveryPhase::AmbiguityPending:
        runtime.lifecycle = LifecyclePhase::RecoveryPending;
        runtime.last_reason = Reason::AmbiguityUnresolved;
        downgraded = true;
        break;
      default:
        break;
    }
    runtime.activation_accepted = false;
    runtime.effect_verified = false;
    runtime.state_transfer_verified = false;
    runtime.continuity = ContinuityClass::Unknown;
    runtime.effects_seen.clear();
    if (downgraded) {
      ++restart.services_downgraded;
    }
  }
  restart.attempts_marked_unknown = attempts_unknown;

  for (auto& entry : state.capabilities) {
    entry.second.requires_reconfirmation = true;
    ++restart.capability_evidence_invalidated;
  }
  for (auto& entry : state.dependencies) {
    for (auto& slot : entry.second) {
      slot.second.requires_reconfirmation = true;
      ++restart.dependency_observations_invalidated;
    }
  }
  restart.failure_evidence_invalidated = static_cast<u32>(state.failures.size());
  restart.conservative = true;
}

Reason Fabric::Impl::open() {
  std::lock_guard<std::mutex> guard(mutex);
  if (opened) {
    return Reason::InvalidState;
  }
  state = State{};
  state.policy = config.initial_policy;
  if (!valid_policy(state.policy)) {
    state.policy = PolicyDescriptor{};
  }
  state.policy_generation = state.policy.generation;
  state.epoch = CoordinatorEpoch::from_value(1);
  state.boot = BootId::from_value(1);
  state.tick = LogicalTick::zero();
  state.topology_generation = TopologyGeneration::initial();
  explanations.clear();
  intents.reopen();
  restart = RestartSummary{};

  if (!config.store_directory.has_value()) {
    opened = true;
    return Reason::Ok;
  }

  Store::Options options;
  options.directory = *config.store_directory;
  options.max_journal_bytes = config.max_journal_bytes;
  options.retained_generations = 4;
  options.max_record_payload_bytes = 1U << 20U;

  RecoveryReport report;
  std::vector<u8> snapshot;
  std::vector<Record> records;
  std::optional<Store> candidate = Store::open(options, report, snapshot, records);
  restart.store = report;
  if (!candidate.has_value()) {
    return report.reason == Reason::Ok ? Reason::StoreUnavailable : report.reason;
  }
  store = std::move(*candidate);

  bool recovered = false;
  if (!snapshot.empty()) {
    CanonicalReader reader(snapshot.data(), snapshot.size());
    State loaded;
    if (!decode_state(reader, loaded, config)) {
      return reader.ok() ? Reason::StoreCorrupt : reader.error();
    }
    if (!reader.at_end()) {
      return Reason::StoreCorrupt;
    }
    state = std::move(loaded);
    recovered = true;
  }
  if (!records.empty()) {
    recovered = true;
  }
  const Reason replay = recover_from_store(records);
  if (replay != Reason::Ok) {
    return replay;
  }
  if (!valid_policy(state.policy)) {
    return Reason::StoreSemanticsIncompatible;
  }

  if (recovered) {
    apply_restart_recovery();
    CanonicalWriter writer;
    writer.put_u64(state.epoch.value());
    writer.put_u64(state.boot.value());
    std::vector<Record> epoch_records;
    epoch_records.push_back(make_record(RecordKind::EpochAdvanced, writer));
    const Reason code = persist(epoch_records);
    if (code != Reason::Ok) {
      return code;
    }
    const Reason compact = rotate_if_due();
    if (compact != Reason::Ok) {
      return compact;
    }
  }
  opened = true;
  return Reason::Ok;
}

Reason Fabric::Impl::close() {
  std::lock_guard<std::mutex> guard(mutex);
  if (!opened) {
    return Reason::InvalidState;
  }
  opened = false;
  intents.close();
  store.reset();
  return Reason::Ok;
}

FenceVerdict Fabric::Impl::verify_fence(const FenceCheck& check) const {
  std::lock_guard<std::mutex> guard(mutex);
  FenceVerdict verdict;
  const auto fence = state.fences.find(check.service);
  if (fence != state.fences.end()) {
    verdict.current = fence->second.token;
    verdict.current_holder = fence->second.holder;
  }
  if (const FenceWitness* witness = witness_of(check.service, check.holder.target);
      witness != nullptr) {
    verdict.had_witness = true;
    verdict.fenced_incarnation = witness->incarnation;
  }
  if (fence == state.fences.end()) {
    verdict.code = Reason::NoAuthority;
    return verdict;
  }
  if (check.presented < fence->second.token) {
    verdict.code = Reason::FenceStale;
    return verdict;
  }
  if (check.presented > fence->second.token) {
    verdict.code = Reason::FenceRegression;
    return verdict;
  }
  if (check.holder != fence->second.holder) {
    verdict.code = Reason::FenceHeldByOther;
    return verdict;
  }
  if (verdict.had_witness && check.holder.incarnation <= verdict.fenced_incarnation) {
    verdict.code = Reason::FallbackFenced;
    return verdict;
  }
  verdict.authorized = true;
  verdict.code = Reason::AcceptFenceAdvanced;
  return verdict;
}

FabricView Fabric::Impl::build_view() const {
  FabricView view;
  view.runtime_version = kRuntimeVersion;
  view.epoch = state.epoch;
  view.boot = state.boot;
  view.tick = state.tick;
  view.topology_generation = state.topology_generation;
  view.policy_generation = state.policy_generation;
  view.policy = state.policy;
  view.targets.reserve(state.targets.size());
  for (const auto& entry : state.targets) {
    view.targets.push_back(entry.second);
  }
  view.services.reserve(state.services.size());
  for (const auto& entry : state.services) {
    const ServiceRuntime& runtime = entry.second;
    ServiceView service;
    service.name = entry.first;
    service.scope = runtime.descriptor.scope;
    service.statefulness = runtime.descriptor.statefulness;
    service.lifecycle = runtime.lifecycle;
    service.recovery = runtime.recovery;
    service.continuity = runtime.continuity;
    service.active = runtime.active;
    service.fallback = runtime.fallback;
    service.generation = runtime.generation;
    service.fence = runtime.fence;
    service.lease_term = runtime.lease.has_value() ? runtime.lease->term : LeaseTerm{};
    service.current_attempt = runtime.current_attempt;
    if (const AttemptRecord* record = runtime.current_attempt_record(); record != nullptr) {
      service.last_outcome = record->outcome;
    }
    service.effect_verified = runtime.effect_verified;
    service.state_transfer_verified = runtime.state_transfer_verified;
    service.ambiguity_pending = runtime.ambiguity_pending;
    service.failover_recorded = runtime.failover_recorded;
    service.last_reason = runtime.last_reason;
    service.attempts_recorded = runtime.history.size();
    service.attempts_evicted = static_cast<std::size_t>(runtime.attempts_evicted);
    service.last_change = runtime.last_change;
    view.services.push_back(std::move(service));
  }
  view.stats = state.stats;
  return view;
}

JsonValue FenceVerdict::to_json() const {
  JsonValue object;
  object.set("authorized", JsonValue(authorized));
  object.set("code", JsonValue(std::string(reason_code_text(code))));
  object.set("current", JsonValue(current.value()));
  object.set("current_holder", off::to_json(current_holder));
  object.set("fenced_incarnation", off::to_json(fenced_incarnation));
  object.set("had_witness", JsonValue(had_witness));
  return object;
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

Fabric::Fabric(FabricConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Fabric::~Fabric() = default;

Reason Fabric::open() { return impl_->open(); }

Reason Fabric::close() { return impl_->close(); }

bool Fabric::is_open() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->opened;
}

const RestartSummary& Fabric::restart_summary() const noexcept { return impl_->restart; }

Reason Fabric::enroll_source(SourceName source, SourceRole role, Explanation* explanation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Explanation local = impl_->begin_explanation(ServiceName{}, RequestKey{});
  const Reason code = impl_->do_enroll_source(source, role, local);
  impl_->finish_ingest_explanation(local, explanation);
  return code;
}

Reason Fabric::register_service(const ServiceDescriptor& descriptor, SourceName registrar,
                                Explanation* explanation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Explanation local = impl_->begin_explanation(descriptor.name, RequestKey{});
  const Reason code = impl_->do_register_service(descriptor, registrar, local);
  impl_->finish_ingest_explanation(local, explanation);
  return code;
}

Reason Fabric::register_target(const TargetRecord& target, SourceName registrar,
                               Explanation* explanation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Explanation local = impl_->begin_explanation(ServiceName{}, RequestKey{});
  const Reason code = impl_->do_register_target(target, registrar, local);
  impl_->finish_ingest_explanation(local, explanation);
  return code;
}

Reason Fabric::set_topology_generation(TopologyGeneration generation, SourceName reporter,
                                       Explanation* explanation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Explanation local = impl_->begin_explanation(ServiceName{}, RequestKey{});
  const Reason code = impl_->do_set_topology_generation(generation, reporter, local);
  impl_->finish_ingest_explanation(local, explanation);
  return code;
}

Reason Fabric::set_policy(const PolicyDescriptor& policy, Explanation* explanation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Explanation local = impl_->begin_explanation(ServiceName{}, RequestKey{});
  const Reason code = impl_->do_set_policy(policy, local);
  impl_->finish_ingest_explanation(local, explanation);
  return code;
}

Reason Fabric::withdraw_service(ServiceName service, SourceName requester,
                                Explanation* explanation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Explanation local = impl_->begin_explanation(service, RequestKey{});
  const Reason code = impl_->do_withdraw_service(service, requester, local);
  impl_->finish_ingest_explanation(local, explanation);
  return code;
}

Reason Fabric::retire_target(TargetName target, SourceName requester, Explanation* explanation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Explanation local = impl_->begin_explanation(ServiceName{}, RequestKey{});
  const Reason code = impl_->do_retire_target(target, requester, local);
  impl_->finish_ingest_explanation(local, explanation);
  return code;
}

Reason Fabric::ingest_capability(const CapabilityEvidence& evidence, Explanation* explanation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Explanation local = impl_->begin_explanation(ServiceName{}, RequestKey{});
  const Reason code = impl_->do_ingest_capability(evidence, local);
  impl_->finish_ingest_explanation(local, explanation);
  return code;
}

Reason Fabric::ingest_failure(FailureEvidence evidence, Explanation* explanation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Explanation local = impl_->begin_explanation(evidence.service, RequestKey{});
  const Reason code = impl_->do_ingest_failure(std::move(evidence), local);
  impl_->finish_ingest_explanation(local, explanation);
  return code;
}

Reason Fabric::ingest_dependency(const DependencyObservation& observation,
                                 Explanation* explanation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Explanation local = impl_->begin_explanation(observation.service, RequestKey{});
  const Reason code = impl_->do_ingest_dependency(observation, local);
  impl_->finish_ingest_explanation(local, explanation);
  return code;
}

RecoveryDecision Fabric::establish_placement(const PlacementRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!request.key.is_nil()) {
    if (const RequestCacheEntry* entry = impl_->cached(request.key); entry != nullptr) {
      Explanation local = impl_->begin_explanation(request.service, request.key);
      FailoverRequest replay;
      replay.key = request.key;
      replay.service = request.service;
      return impl_->replay_cached(*entry, std::move(local), replay);
    }
  }
  const Reason registered = impl_->begin_request(request.key);
  if (registered != Reason::Ok) {
    Explanation local = impl_->begin_explanation(request.service, request.key);
    return impl_->refusal(std::move(local), registered, request.service.str(),
                          registered == Reason::QueueCapacityExceeded
                              ? "cancellation registry is full"
                              : "request key is already in flight");
  }
  RecoveryDecision decision = impl_->do_establish_placement(request);
  impl_->cancels.release_if_active(request.key);
  return decision;
}

RecoveryDecision Fabric::request_failover(const FailoverRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!request.key.is_nil()) {
    if (const RequestCacheEntry* entry = impl_->cached(request.key); entry != nullptr) {
      Explanation local = impl_->begin_explanation(request.service, request.key);
      return impl_->replay_cached(*entry, std::move(local), request);
    }
  }
  const Reason registered = impl_->begin_request(request.key);
  if (registered != Reason::Ok) {
    Explanation local = impl_->begin_explanation(request.service, request.key);
    return impl_->refusal(std::move(local), registered, request.service.str(),
                          registered == Reason::QueueCapacityExceeded
                              ? "cancellation registry is full"
                              : "request key is already in flight");
  }
  RecoveryDecision decision = impl_->do_request_failover(request, false);
  impl_->cancels.release_if_active(request.key);
  return decision;
}

RecoveryDecision Fabric::resume_recovery(const FailoverRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!request.key.is_nil()) {
    if (const RequestCacheEntry* entry = impl_->cached(request.key); entry != nullptr) {
      Explanation local = impl_->begin_explanation(request.service, request.key);
      return impl_->replay_cached(*entry, std::move(local), request);
    }
  }
  const Reason registered = impl_->begin_request(request.key);
  if (registered != Reason::Ok) {
    Explanation local = impl_->begin_explanation(request.service, request.key);
    return impl_->refusal(std::move(local), registered, request.service.str(),
                          registered == Reason::QueueCapacityExceeded
                              ? "cancellation registry is full"
                              : "request key is already in flight");
  }
  RecoveryDecision decision = impl_->do_request_failover(request, true);
  impl_->cancels.release_if_active(request.key);
  return decision;
}

RecoveryDecision Fabric::resolve_ambiguity(const AmbiguityResolution& resolution) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!resolution.key.is_nil()) {
    if (const RequestCacheEntry* entry = impl_->cached(resolution.key); entry != nullptr) {
      Explanation local = impl_->begin_explanation(resolution.service, resolution.key);
      FailoverRequest replay;
      replay.key = resolution.key;
      replay.service = resolution.service;
      return impl_->replay_cached(*entry, std::move(local), replay);
    }
  }
  const Reason registered = impl_->begin_request(resolution.key);
  if (registered != Reason::Ok) {
    Explanation local = impl_->begin_explanation(resolution.service, resolution.key);
    return impl_->refusal(std::move(local), registered, resolution.service.str(),
                          registered == Reason::QueueCapacityExceeded
                              ? "cancellation registry is full"
                              : "request key is already in flight");
  }
  RecoveryDecision decision = impl_->do_resolve_ambiguity(resolution);
  impl_->cancels.release_if_active(resolution.key);
  return decision;
}

RecoveryDecision Fabric::request_failback(const FailbackRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!request.key.is_nil()) {
    if (const RequestCacheEntry* entry = impl_->cached(request.key); entry != nullptr) {
      Explanation local = impl_->begin_explanation(request.service, request.key);
      FailoverRequest replay;
      replay.key = request.key;
      replay.service = request.service;
      return impl_->replay_cached(*entry, std::move(local), replay);
    }
  }
  const Reason registered = impl_->begin_request(request.key);
  if (registered != Reason::Ok) {
    Explanation local = impl_->begin_explanation(request.service, request.key);
    return impl_->refusal(std::move(local), registered, request.service.str(),
                          registered == Reason::QueueCapacityExceeded
                              ? "cancellation registry is full"
                              : "request key is already in flight");
  }
  RecoveryDecision decision = impl_->do_request_failback(request);
  impl_->cancels.release_if_active(request.key);
  return decision;
}

RecoveryDecision Fabric::acknowledge_intent(const IntentAck& ack) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->do_acknowledge_intent(ack);
}

RecoveryDecision Fabric::report_effect(const EffectReport& report) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->do_report_effect(report);
}

Reason Fabric::cancel(RequestKey key, Explanation* explanation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Explanation local = impl_->begin_explanation(ServiceName{}, key);
  bool too_late = false;
  bool unknown = false;
  const bool recorded = impl_->cancels.cancel(key, too_late, unknown);
  Reason code = Reason::Cancelled;
  if (recorded) {
    impl_->state.stats.requests_cancelled = saturating_inc(impl_->state.stats.requests_cancelled);
    impl_->note(local, Reason::Cancelled, std::string{}, "cancellation recorded before commit",
                key.value());
  } else if (too_late) {
    impl_->state.stats.cancels_too_late = saturating_inc(impl_->state.stats.cancels_too_late);
    code = Reason::CancelTooLate;
    impl_->note(local, Reason::CancelTooLate, std::string{},
                "the request already committed durably", key.value());
  } else {
    code = Reason::RequestUnknown;
    impl_->note(local, Reason::RequestUnknown, std::string{},
                "no such request is in flight", key.value());
  }
  local.outcome = code;
  local.accepted = false;
  impl_->record_explanation(local);
  if (explanation != nullptr) {
    *explanation = local;
  }
  return code;
}

FenceVerdict Fabric::verify_fence(const FenceCheck& check) const { return impl_->verify_fence(check); }

FabricView Fabric::view() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->build_view();
}

std::string Fabric::export_canonical(bool pretty) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->build_view().canonical_text(pretty);
}

std::vector<Explanation> Fabric::explain(ServiceName service) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->explanations.for_service(service);
}

std::vector<Explanation> Fabric::explain_all() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->explanations.all();
}

std::vector<FailoverIntent> Fabric::pending_intents() const { return impl_->intents.snapshot(); }

bool Fabric::take_intent(FailoverIntent& out) {
  if (!impl_->intents.try_pop(out)) {
    return false;
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->state.stats.intents_consumed = saturating_inc(impl_->state.stats.intents_consumed);
  return true;
}

bool Fabric::wait_for_intent(FailoverIntent& out) { return impl_->intents.wait_pop(out); }

FabricStats Fabric::stats() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.stats;
}

std::size_t Fabric::intent_queue_depth() const { return impl_->intents.depth(); }

std::string Fabric::snapshot_digest_text() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  CanonicalWriter writer;
  encode_state(writer, impl_->state);
  return writer.digest().to_hex();
}

}  // namespace off
