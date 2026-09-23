// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The journal geometry is re-implemented here from the documented layout so
// the tests validate the on-disk format independently of the writer.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "off/off.hpp"
#include "testing.hpp"

namespace {

using namespace offtest;

constexpr off::u32 kMagic = 0x4F46414AU;

void put_u16(std::vector<off::u8>& buffer, std::size_t offset, off::u16 value) {
  buffer[offset] = static_cast<off::u8>((value >> 8U) & 0xFFU);
  buffer[offset + 1] = static_cast<off::u8>(value & 0xFFU);
}

void put_u32(std::vector<off::u8>& buffer, std::size_t offset, off::u32 value) {
  for (int index = 0; index < 4; ++index) {
    buffer[offset + static_cast<std::size_t>(index)] =
        static_cast<off::u8>((value >> static_cast<unsigned>(24 - (index * 8))) & 0xFFU);
  }
}

void put_u64(std::vector<off::u8>& buffer, std::size_t offset, off::u64 value) {
  for (int index = 0; index < 8; ++index) {
    buffer[offset + static_cast<std::size_t>(index)] =
        static_cast<off::u8>((value >> static_cast<unsigned>(56 - (index * 8))) & 0xFFU);
  }
}

off::u32 read_u32(const off::u8* data) {
  off::u32 value = 0;
  for (int index = 0; index < 4; ++index) {
    value = (value << 8U) | static_cast<off::u32>(data[index]);
  }
  return value;
}

/// Builds one journal frame exactly as documented, independently of the runtime.
std::vector<off::u8> build_frame(off::RecordKind kind, off::u64 sequence,
                                  const std::vector<off::u8>& payload) {
  std::vector<off::u8> frame(off::kFrameHeaderBytes + payload.size() + off::kFrameTrailerBytes, 0);
  put_u32(frame, 0, kMagic);
  put_u16(frame, 4, off::kStoreFormatVersion);
  put_u16(frame, 6, static_cast<off::u16>(kind));
  put_u32(frame, 8, static_cast<off::u32>(payload.size()));
  put_u64(frame, 12, sequence);
  put_u32(frame, 20, payload.empty() ? 0U : off::crc32c(payload.data(), payload.size()));
  for (std::size_t index = 0; index < payload.size(); ++index) {
    frame[off::kFrameHeaderBytes + index] = payload[index];
  }
  put_u32(frame, off::kFrameHeaderBytes + payload.size(),
          off::crc32c(frame.data(), off::kFrameHeaderBytes + payload.size()));
  return frame;
}

std::vector<off::u8> encode_u64(off::u64 value) {
  std::vector<off::u8> payload(8, 0);
  put_u64(payload, 0, value);
  return payload;
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

std::vector<off::u8> read_bytes(const std::filesystem::path& path) {
  std::vector<off::u8> out;
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return out;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  out.assign(static_cast<std::size_t>(size), 0);
  if (size > 0) {
    OFF_CHECK(std::fread(out.data(), 1, out.size(), file) == out.size());
  }
  std::fclose(file);
  return out;
}

void write_bytes(const std::filesystem::path& path, const std::vector<off::u8>& data) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    OFF_CHECK(false);
    return;
  }
  if (!data.empty()) {
    OFF_CHECK(std::fwrite(data.data(), 1, data.size(), file) == data.size());
  }
  std::fclose(file);
}

off::Record make_record(off::RecordKind kind, const std::string& text) {
  off::Record record;
  record.kind = kind;
  record.payload.assign(text.begin(), text.end());
  return record;
}

off::Store::Options options_for(const std::filesystem::path& directory) {
  off::Store::Options options;
  options.directory = directory;
  options.max_journal_bytes = 4096;
  options.retained_generations = 2;
  return options;
}

OFF_TEST(store, empty_directory_reports_empty_recovery) {
  const std::filesystem::path directory = unique_directory("store-empty");
  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  std::optional<off::Store> store = off::Store::open(options_for(directory), report, snapshot, records);
  OFF_REQUIRE(store.has_value());
  OFF_CHECK(report.classification == off::RecoveryClass::Empty);
  OFF_CHECK(records.empty());
  OFF_CHECK(snapshot.empty());
  OFF_CHECK(std::filesystem::exists(directory / off::journal_file_name(1)));
}

OFF_TEST(store, committed_transactions_replay_in_order) {
  const std::filesystem::path directory = unique_directory("store-replay");
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::SourceRegistered, "one")}),
                     off::Reason::Ok);
    OFF_CHECK_REASON(
        store->commit_transaction({make_record(off::RecordKind::TargetUpserted, "two"),
                                   make_record(off::RecordKind::PolicySet, "three")}),
        off::Reason::Ok);
  }
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK(report.classification == off::RecoveryClass::CleanReopen);
    OFF_CHECK_EQ(report.transactions_applied, 2ULL);
    OFF_REQUIRE(records.size() == 3);
    OFF_CHECK(records[0].kind == off::RecordKind::SourceRegistered);
    OFF_CHECK(records[1].kind == off::RecordKind::TargetUpserted);
    OFF_CHECK(records[2].kind == off::RecordKind::PolicySet);
    OFF_CHECK_EQ(std::string(records[0].payload.begin(), records[0].payload.end()),
                 std::string("one"));
  }
}

OFF_TEST(store, torn_tail_is_repaired_without_losing_committed_records) {
  const std::filesystem::path directory = unique_directory("store-torn");
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::PolicySet, "kept")}),
                     off::Reason::Ok);
  }
  const std::filesystem::path journal = directory / off::journal_file_name(1);
  const std::vector<off::u8> before = read_bytes(journal);
  OFF_CHECK(!before.empty());
  // A partially written frame: header claims a payload that never arrived.
  std::vector<off::u8> partial = build_frame(off::RecordKind::PolicySet, 9, encode_u64(1));
  partial.resize(partial.size() - 3);
  append_bytes(journal, partial);

  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  std::optional<off::Store> store = off::Store::open(options_for(directory), report, snapshot, records);
  OFF_REQUIRE(store.has_value());
  OFF_CHECK(report.classification == off::RecoveryClass::TornTailRepaired);
  OFF_CHECK(report.reason == off::Reason::TornTailTruncated);
  OFF_CHECK(report.truncated_tail_repaired);
  OFF_CHECK(report.bytes_discarded > 0);
  OFF_REQUIRE(records.size() == 1);
  OFF_CHECK_EQ(std::string(records[0].payload.begin(), records[0].payload.end()),
               std::string("kept"));
  OFF_CHECK(read_bytes(journal).size() == before.size());
}

OFF_TEST(store, corruption_before_a_valid_frame_fails_closed) {
  const std::filesystem::path directory = unique_directory("store-corrupt");
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::PolicySet, "one")}),
                     off::Reason::Ok);
    OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::PolicySet, "two")}),
                     off::Reason::Ok);
  }
  const std::filesystem::path journal = directory / off::journal_file_name(1);
  std::vector<off::u8> bytes = read_bytes(journal);
  OFF_CHECK(bytes.size() > 64);
  bytes[30] ^= 0xFFU;
  write_bytes(journal, bytes);

  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  std::optional<off::Store> store = off::Store::open(options_for(directory), report, snapshot, records);
  OFF_CHECK(!store.has_value());
  OFF_CHECK(report.classification == off::RecoveryClass::Corrupt);
  OFF_CHECK(off::recovery_class_is_refusal(report.classification));
}

OFF_TEST(store, payload_checksum_failure_fails_closed) {
  const std::filesystem::path directory = unique_directory("store-crc");
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::PolicySet, "abcdef")}),
                     off::Reason::Ok);
    OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::PolicySet, "ghijkl")}),
                     off::Reason::Ok);
  }
  const std::filesystem::path journal = directory / off::journal_file_name(1);
  std::vector<off::u8> bytes = read_bytes(journal);
  OFF_CHECK(bytes.size() > 64);
  bytes[26] ^= 0x01U;  // inside the first payload
  write_bytes(journal, bytes);

  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  std::optional<off::Store> store = off::Store::open(options_for(directory), report, snapshot, records);
  OFF_CHECK(!store.has_value());
  OFF_CHECK(off::recovery_class_is_refusal(report.classification));
}

OFF_TEST(store, future_and_unknown_format_versions_are_refused) {
  const std::filesystem::path directory = unique_directory("store-version");
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::PolicySet, "x")}),
                     off::Reason::Ok);
  }
  const std::filesystem::path journal = directory / off::journal_file_name(1);
  std::vector<off::u8> bytes = read_bytes(journal);
  bytes[4] = 0x00;
  bytes[5] = 0x63;  // version 99
  write_bytes(journal, bytes);
  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  std::optional<off::Store> store = off::Store::open(options_for(directory), report, snapshot, records);
  OFF_CHECK(!store.has_value());
  OFF_CHECK(report.classification == off::RecoveryClass::VersionIncompatible);
  OFF_CHECK(report.reason == off::Reason::StoreVersionIncompatible);
}

OFF_TEST(store, unknown_record_kind_is_refused) {
  const std::filesystem::path directory = unique_directory("store-kind");
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::PolicySet, "x")}),
                     off::Reason::Ok);
  }
  const std::filesystem::path journal = directory / off::journal_file_name(1);
  std::vector<off::u8> bytes = read_bytes(journal);
  bytes[6] = 0x00;
  bytes[7] = 0xEE;
  write_bytes(journal, bytes);
  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  std::optional<off::Store> store = off::Store::open(options_for(directory), report, snapshot, records);
  OFF_CHECK(!store.has_value());
  OFF_CHECK(off::recovery_class_is_refusal(report.classification));
}

OFF_TEST(store, incomplete_tail_transaction_is_dropped_and_counted) {
  const std::filesystem::path directory = unique_directory("store-incomplete");
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::PolicySet, "committed")}),
                     off::Reason::Ok);
  }
  const std::filesystem::path journal = directory / off::journal_file_name(1);
  // A begin frame with no matching commit frame: a crash between the two.
  append_bytes(journal, build_frame(off::RecordKind::BeginTransaction, 50, encode_u64(50)));
  append_bytes(journal, build_frame(off::RecordKind::PolicySet, 51,
                                    std::vector<off::u8>{'u', 'n', 'c'}));

  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  std::optional<off::Store> store = off::Store::open(options_for(directory), report, snapshot, records);
  OFF_REQUIRE(store.has_value());
  OFF_CHECK(report.classification == off::RecoveryClass::TransactionDropped);
  OFF_CHECK_EQ(report.transactions_dropped, 1ULL);
  OFF_REQUIRE(records.size() == 1);
  OFF_CHECK_EQ(std::string(records[0].payload.begin(), records[0].payload.end()),
               std::string("committed"));
}

OFF_TEST(store, mismatched_transaction_identifiers_are_refused) {
  const std::filesystem::path directory = unique_directory("store-mismatch");
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::PolicySet, "x")}),
                     off::Reason::Ok);
  }
  const std::filesystem::path journal = directory / off::journal_file_name(1);
  append_bytes(journal, build_frame(off::RecordKind::BeginTransaction, 41, encode_u64(41)));
  append_bytes(journal, build_frame(off::RecordKind::CommitTransaction, 42, encode_u64(42)));
  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  std::optional<off::Store> store = off::Store::open(options_for(directory), report, snapshot, records);
  OFF_CHECK(!store.has_value());
  OFF_CHECK(off::recovery_class_is_refusal(report.classification));
}

OFF_TEST(store, non_monotonic_sequence_is_refused) {
  const std::filesystem::path directory = unique_directory("store-sequence");
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::PolicySet, "x")}),
                     off::Reason::Ok);
  }
  const std::filesystem::path journal = directory / off::journal_file_name(1);
  append_bytes(journal, build_frame(off::RecordKind::BeginTransaction, 1, encode_u64(1)));
  append_bytes(journal, build_frame(off::RecordKind::CommitTransaction, 2, encode_u64(1)));
  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  std::optional<off::Store> store = off::Store::open(options_for(directory), report, snapshot, records);
  OFF_CHECK(!store.has_value());
  OFF_CHECK(off::recovery_class_is_refusal(report.classification));
}

OFF_TEST(store, snapshot_compaction_round_trips_and_bounds_generations) {
  const std::filesystem::path directory = unique_directory("store-compact");
  const std::vector<off::u8> payload = {'s', 'n', 'a', 'p'};
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK_REASON(store->compact(payload.data(), payload.size()), off::Reason::Ok);
    OFF_CHECK_EQ(store->journal_generation(), 2U);
    OFF_CHECK_EQ(store->journal_bytes(), 0ULL);
  }
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    OFF_CHECK_EQ(report.snapshot_generation, 2U);
    OFF_CHECK(snapshot == payload);
    OFF_CHECK(records.empty());
  }
}

OFF_TEST(store, corrupted_snapshot_fails_closed) {
  const std::filesystem::path directory = unique_directory("store-snapshot-bad");
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    std::optional<off::Store> store =
        off::Store::open(options_for(directory), report, snapshot, records);
    OFF_REQUIRE(store.has_value());
    const std::vector<off::u8> payload = {'a', 'b', 'c', 'd'};
    OFF_CHECK_REASON(store->compact(payload.data(), payload.size()), off::Reason::Ok);
  }
  const std::filesystem::path snapshot_path = directory / off::snapshot_file_name(2);
  std::vector<off::u8> bytes = read_bytes(snapshot_path);
  OFF_CHECK(!bytes.empty());
  bytes.back() ^= 0xFFU;
  write_bytes(snapshot_path, bytes);
  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  std::optional<off::Store> store = off::Store::open(options_for(directory), report, snapshot, records);
  OFF_CHECK(!store.has_value());
  OFF_CHECK(report.reason == off::Reason::SnapshotCorrupt);
}

OFF_TEST(store, declared_payload_beyond_the_record_ceiling_is_refused) {
  const std::filesystem::path directory = unique_directory("store-oversize");
  {
    off::RecoveryReport report;
    std::vector<off::u8> snapshot;
    std::vector<off::Record> records;
    off::Store::Options options = options_for(directory);
    options.max_record_payload_bytes = 8;
    std::optional<off::Store> store = off::Store::open(options, report, snapshot, records);
    OFF_REQUIRE(store.has_value());
  }
  const std::filesystem::path journal = directory / off::journal_file_name(1);
  append_bytes(journal, build_frame(off::RecordKind::BeginTransaction, 1, encode_u64(1)));
  std::vector<off::u8> huge(64, 'x');
  append_bytes(journal, build_frame(off::RecordKind::PolicySet, 2, huge));
  append_bytes(journal, build_frame(off::RecordKind::CommitTransaction, 3, encode_u64(1)));
  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  off::Store::Options options = options_for(directory);
  options.max_record_payload_bytes = 8;
  std::optional<off::Store> store = off::Store::open(options, report, snapshot, records);
  OFF_CHECK(!store.has_value());
  OFF_CHECK(off::recovery_class_is_refusal(report.classification));
}

OFF_TEST(store, read_only_open_never_writes) {
  const std::filesystem::path directory = unique_directory("store-readonly");
  off::RecoveryReport report;
  std::vector<off::u8> snapshot;
  std::vector<off::Record> records;
  off::Store::Options options = options_for(directory);
  options.read_only = true;
  std::optional<off::Store> store = off::Store::open(options, report, snapshot, records);
  OFF_REQUIRE(store.has_value());
  OFF_CHECK_REASON(store->commit_transaction({make_record(off::RecordKind::PolicySet, "x")}),
                   off::Reason::StoreUnavailable);
  OFF_CHECK(!std::filesystem::exists(directory / off::journal_file_name(1)));
}

}  // namespace
