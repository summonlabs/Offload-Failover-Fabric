// Offload Failover Fabric - versioned, integrity-checked durable store.
// Records are framed, checksummed and grouped into transactions; a transaction
// is committed only when its commit frame reaches stable storage.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "off/canonical.hpp"
#include "off/digest.hpp"
#include "off/reason.hpp"
#include "off/version.hpp"

namespace off {

/// Record kinds persisted in the journal. The numeric values are part of the
/// on-disk format and never change.
enum class RecordKind : u16 {
  BeginTransaction = 1,
  CommitTransaction = 2,
  ServiceRegistered = 3,
  TargetUpserted = 4,
  TopologyGenerationSet = 5,
  PolicySet = 6,
  CapabilityIngested = 7,
  FailureIngested = 8,
  DependencyIngested = 9,
  AttemptCommitted = 10,
  AttemptAcknowledged = 11,
  EffectRecorded = 12,
  AmbiguityResolved = 13,
  FailbackCommitted = 14,
  EpochAdvanced = 15,
  ServiceWithdrawn = 16,
  SourceRegistered = 17,
  RecoveryResumed = 18,
  TargetRetired = 19,
};

inline constexpr u16 kRecordKindMax = 19;
[[nodiscard]] std::string_view record_kind_text(RecordKind kind) noexcept;
[[nodiscard]] bool record_kind_parse(std::string_view text, RecordKind& out) noexcept;

struct Record {
  RecordKind kind{RecordKind::BeginTransaction};
  std::vector<u8> payload{};
};

/// How the previous contents of the store were classified on open.
enum class RecoveryClass : u8 {
  Empty = 0,
  CleanReopen = 1,
  TornTailRepaired = 2,
  TransactionDropped = 3,
  Corrupt = 4,
  VersionIncompatible = 5,
  Unavailable = 6,
};

inline constexpr std::size_t kRecoveryClassCount = 7;
[[nodiscard]] std::string_view recovery_class_text(RecoveryClass value) noexcept;
[[nodiscard]] bool recovery_class_parse(std::string_view text, RecoveryClass& out) noexcept;
/// True when the store contents were refused and no state may be trusted.
[[nodiscard]] bool recovery_class_is_refusal(RecoveryClass value) noexcept;

struct RecoveryReport {
  RecoveryClass classification{RecoveryClass::Empty};
  Reason reason{Reason::Ok};
  u32 format_version{0};
  u64 bytes_accepted{0};
  u64 bytes_discarded{0};
  u64 transactions_applied{0};
  u64 transactions_dropped{0};
  u64 records_replayed{0};
  u32 snapshot_generation{0};
  u32 journal_generation{0};
  Digest replayed_digest{};
  bool truncated_tail_repaired{false};
};

/// Append-only transaction journal with generation-based snapshot compaction.
class Store {
 public:
  struct Options {
    std::filesystem::path directory{};
    /// Journal size that triggers compaction. Growth is bounded by this value
    /// plus at most one transaction.
    u64 max_journal_bytes{8ULL << 20U};
    /// Number of snapshot generations retained after a successful compaction.
    u32 retained_generations{2};
    /// Hard ceiling on a single record payload accepted from disk or memory.
    u32 max_record_payload_bytes{1U << 20U};
    /// When set, the journal is opened for replay only and never appended to.
    bool read_only{false};
  };

  Store() = default;
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&& other) noexcept;
  Store& operator=(Store&& other) noexcept;

  /// Opens the store, validates the snapshot and journal, repairs a torn tail,
  /// and returns the committed records that must be replayed.
  [[nodiscard]] static std::optional<Store> open(const Options& options, RecoveryReport& report,
                                                 std::vector<u8>& snapshot_payload,
                                                 std::vector<Record>& recovered_records);

  /// Durably commits one transaction. Either every record in the transaction is
  /// readable after a crash or none of them is. Returns Ok only after the
  /// commit frame reached stable storage.
  [[nodiscard]] Reason commit_transaction(const std::vector<Record>& records);

  /// Writes a new snapshot generation and starts a fresh journal. Old
  /// generations beyond the retention bound are removed.
  [[nodiscard]] Reason compact(const u8* snapshot, std::size_t size);

  [[nodiscard]] u64 journal_bytes() const noexcept { return journal_bytes_; }
  [[nodiscard]] bool rotation_due() const noexcept;
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return options_.directory; }
  [[nodiscard]] u32 journal_generation() const noexcept { return journal_generation_; }
  [[nodiscard]] u32 snapshot_generation() const noexcept { return snapshot_generation_; }
  [[nodiscard]] const Options& options() const noexcept { return options_; }

 private:
  void close() noexcept;

  Options options_{};
  std::FILE* journal_{nullptr};
  u64 journal_bytes_{0};
  u64 next_sequence_{1};
  u32 journal_generation_{0};
  u32 snapshot_generation_{0};
};

/// Frame geometry shared with the tests and the inspection tooling.
inline constexpr std::size_t kFrameHeaderBytes = 24;
inline constexpr std::size_t kFrameTrailerBytes = 4;
inline constexpr u32 kFrameMagic = 0x4F46414AU;  // 'OFAJ'

[[nodiscard]] std::string snapshot_file_name(u32 generation);
[[nodiscard]] std::string journal_file_name(u32 generation);

}  // namespace off
