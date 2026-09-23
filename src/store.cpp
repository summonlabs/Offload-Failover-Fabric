// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "off/store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace off {
namespace {

constexpr u32 kSnapshotMagic = 0x4F464153U;  // 'OFAS'
constexpr std::size_t kSnapshotHeaderBytes = 20;
constexpr u64 kMaxReplayRecords = 1ULL << 20U;
constexpr u64 kJournalSlackBytes = 1ULL << 20U;
constexpr u64 kMaxSnapshotBytes = 512ULL << 20U;

struct RecordKindEntry {
  RecordKind kind;
  std::string_view text;
};

constexpr std::array<RecordKindEntry, kRecordKindMax> kRecordKinds = {{
    {RecordKind::BeginTransaction, "begin_transaction"},
    {RecordKind::CommitTransaction, "commit_transaction"},
    {RecordKind::ServiceRegistered, "service_registered"},
    {RecordKind::TargetUpserted, "target_upserted"},
    {RecordKind::TopologyGenerationSet, "topology_generation_set"},
    {RecordKind::PolicySet, "policy_set"},
    {RecordKind::CapabilityIngested, "capability_ingested"},
    {RecordKind::FailureIngested, "failure_ingested"},
    {RecordKind::DependencyIngested, "dependency_ingested"},
    {RecordKind::AttemptCommitted, "attempt_committed"},
    {RecordKind::AttemptAcknowledged, "attempt_acknowledged"},
    {RecordKind::EffectRecorded, "effect_recorded"},
    {RecordKind::AmbiguityResolved, "ambiguity_resolved"},
    {RecordKind::FailbackCommitted, "failback_committed"},
    {RecordKind::EpochAdvanced, "epoch_advanced"},
    {RecordKind::ServiceWithdrawn, "service_withdrawn"},
    {RecordKind::SourceRegistered, "source_registered"},
    {RecordKind::RecoveryResumed, "recovery_resumed"},
    {RecordKind::TargetRetired, "target_retired"},
}};

struct RecoveryClassEntry {
  RecoveryClass value;
  std::string_view text;
};

constexpr std::array<RecoveryClassEntry, kRecoveryClassCount> kRecoveryClasses = {{
    {RecoveryClass::Empty, "empty"},
    {RecoveryClass::CleanReopen, "clean_reopen"},
    {RecoveryClass::TornTailRepaired, "torn_tail_repaired"},
    {RecoveryClass::TransactionDropped, "transaction_dropped"},
    {RecoveryClass::Corrupt, "corrupt"},
    {RecoveryClass::VersionIncompatible, "version_incompatible"},
    {RecoveryClass::Unavailable, "unavailable"},
}};

void put_u16_at(std::vector<u8>& buffer, std::size_t offset, u16 value) {
  buffer[offset] = static_cast<u8>((value >> 8U) & 0xFFU);
  buffer[offset + 1] = static_cast<u8>(value & 0xFFU);
}

void put_u32_at(std::vector<u8>& buffer, std::size_t offset, u32 value) {
  for (int index = 0; index < 4; ++index) {
    buffer[offset + static_cast<std::size_t>(index)] =
        static_cast<u8>((value >> static_cast<unsigned>(24 - (index * 8))) & 0xFFU);
  }
}

void put_u64_at(std::vector<u8>& buffer, std::size_t offset, u64 value) {
  for (int index = 0; index < 8; ++index) {
    buffer[offset + static_cast<std::size_t>(index)] =
        static_cast<u8>((value >> static_cast<unsigned>(56 - (index * 8))) & 0xFFU);
  }
}

[[nodiscard]] u16 read_u16(const u8* data) noexcept {
  return static_cast<u16>((static_cast<u16>(data[0]) << 8U) | static_cast<u16>(data[1]));
}

[[nodiscard]] u32 read_u32(const u8* data) noexcept {
  u32 value = 0;
  for (int index = 0; index < 4; ++index) {
    value = (value << 8U) | static_cast<u32>(data[index]);
  }
  return value;
}

[[nodiscard]] u64 read_u64(const u8* data) noexcept {
  u64 value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8U) | static_cast<u64>(data[index]);
  }
  return value;
}

[[nodiscard]] bool sync_file(std::FILE* file) noexcept {
  if (file == nullptr) {
    return false;
  }
  if (std::fflush(file) != 0) {
    return false;
  }
#ifdef _WIN32
  return _commit(_fileno(file)) == 0;
#else
  return ::fsync(::fileno(file)) == 0;
#endif
}

[[nodiscard]] bool atomic_replace(const std::filesystem::path& from,
                                  const std::filesystem::path& to) noexcept {
#ifdef _WIN32
  return ::MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  return !ec;
#endif
}

[[nodiscard]] bool truncate_file(const std::filesystem::path& path, u64 size) noexcept {
#ifdef _WIN32
  if (size > static_cast<u64>((std::numeric_limits<i64>::max)())) {
    return false;
  }
  const int fd = _wopen(path.wstring().c_str(), _O_RDWR | _O_BINARY);
  if (fd < 0) {
    return false;
  }
  const bool ok = _chsize_s(fd, static_cast<i64>(size)) == 0;
  _close(fd);
  return ok;
#else
  std::error_code ec;
  std::filesystem::resize_file(path, size, ec);
  return !ec;
#endif
}

[[nodiscard]] bool read_entire_file(const std::filesystem::path& path, u64 max_bytes,
                                    std::vector<u8>& out) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size < 0 || static_cast<u64>(size) > max_bytes) {
    std::fclose(file);
    return false;
  }
  out.assign(static_cast<std::size_t>(size), 0);
  if (!out.empty() && std::fread(out.data(), 1, out.size(), file) != out.size()) {
    std::fclose(file);
    out.clear();
    return false;
  }
  std::fclose(file);
  return true;
}

[[nodiscard]] bool write_file_atomically(const std::filesystem::path& path, const u8* data,
                                         std::size_t size) {
  std::filesystem::path temp = path;
  temp += ".tmp";
  std::FILE* file = std::fopen(temp.string().c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  if (size > 0 && std::fwrite(data, 1, size, file) != size) {
    std::fclose(file);
    std::filesystem::remove(temp);
    return false;
  }
  if (!sync_file(file)) {
    std::fclose(file);
    std::filesystem::remove(temp);
    return false;
  }
  std::fclose(file);
  if (!atomic_replace(temp, path)) {
    std::filesystem::remove(temp);
    return false;
  }
  return true;
}

struct SnapshotHeader {
  u16 format_version{0};
  u32 payload_length{0};
};

[[nodiscard]] bool parse_snapshot_header(const u8* data, std::size_t size, SnapshotHeader& out) {
  if (size < kSnapshotHeaderBytes) {
    return false;
  }
  if (read_u32(data) != kSnapshotMagic) {
    return false;
  }
  if (crc32c(data, 16) != read_u32(data + 16)) {
    return false;
  }
  out.format_version = read_u16(data + 4);
  out.payload_length = read_u32(data + 8);
  return true;
}

[[nodiscard]] std::vector<u8> build_snapshot_frame(const u8* payload, std::size_t size) {
  std::vector<u8> frame(kSnapshotHeaderBytes + size, 0);
  put_u32_at(frame, 0, kSnapshotMagic);
  put_u16_at(frame, 4, kStoreFormatVersion);
  put_u16_at(frame, 6, 0);
  put_u32_at(frame, 8, static_cast<u32>(size));
  put_u32_at(frame, 12, size == 0 ? 0 : crc32c(payload, size));
  put_u32_at(frame, 16, crc32c(frame.data(), 16));
  if (size > 0) {
    std::memcpy(frame.data() + kSnapshotHeaderBytes, payload, size);
  }
  return frame;
}

[[nodiscard]] std::vector<u8> build_record_frame(RecordKind kind, u64 sequence, const u8* payload,
                                                 std::size_t size) {
  std::vector<u8> frame(kFrameHeaderBytes + size + kFrameTrailerBytes, 0);
  put_u32_at(frame, 0, kFrameMagic);
  put_u16_at(frame, 4, kStoreFormatVersion);
  put_u16_at(frame, 6, static_cast<u16>(kind));
  put_u32_at(frame, 8, static_cast<u32>(size));
  put_u64_at(frame, 12, sequence);
  put_u32_at(frame, 20, size == 0 ? 0 : crc32c(payload, size));
  if (size > 0) {
    std::memcpy(frame.data() + kFrameHeaderBytes, payload, size);
  }
  put_u32_at(frame, kFrameHeaderBytes + size,
             crc32c(frame.data(), kFrameHeaderBytes + size));
  return frame;
}

struct RawFrame {
  RecordKind kind{RecordKind::BeginTransaction};
  u64 sequence{0};
  std::vector<u8> payload{};
  u64 offset{0};
  u64 frame_bytes{0};
};

enum class ScanStatus : u8 { Clean, TornTail, Corrupt, VersionIncompatible, TooLarge };

/// Streams a journal file, validating every frame. On TornTail the offset of the
/// first unusable byte is reported. Corrupt means an unreadable frame was found
/// while a later readable frame still existed, so no truncation is safe.
[[nodiscard]] ScanStatus scan_journal(const std::filesystem::path& path, const Store::Options& options,
                                      std::vector<RawFrame>& frames, u64& bad_offset,
                                      u64& file_size) {
  frames.clear();
  bad_offset = 0;
  file_size = 0;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return ScanStatus::Clean;
  }
  file_size = static_cast<u64>(std::filesystem::file_size(path, ec));
  if (ec) {
    return ScanStatus::Corrupt;
  }
  const u64 read_bound =
      options.max_journal_bytes + kJournalSlackBytes + (4ULL * options.max_record_payload_bytes);
  if (file_size > read_bound) {
    return ScanStatus::TooLarge;
  }
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return ScanStatus::Corrupt;
  }
  std::vector<u8> header(kFrameHeaderBytes, 0);
  u64 offset = 0;
  while (offset + kFrameHeaderBytes + kFrameTrailerBytes <= file_size) {
    if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0 ||
        std::fread(header.data(), 1, kFrameHeaderBytes, file) != kFrameHeaderBytes) {
      std::fclose(file);
      bad_offset = offset;
      return ScanStatus::Corrupt;
    }
    if (read_u32(header.data()) != kFrameMagic) {
      std::fclose(file);
      bad_offset = offset;
      return ScanStatus::Corrupt;
    }
    if (read_u16(header.data() + 4) != kStoreFormatVersion) {
      std::fclose(file);
      bad_offset = offset;
      return ScanStatus::VersionIncompatible;
    }
    const u16 kind_raw = read_u16(header.data() + 6);
    if (kind_raw == 0 || kind_raw > kRecordKindMax) {
      std::fclose(file);
      bad_offset = offset;
      return ScanStatus::Corrupt;
    }
    const u32 payload_length = read_u32(header.data() + 8);
    if (payload_length > options.max_record_payload_bytes) {
      std::fclose(file);
      bad_offset = offset;
      return ScanStatus::Corrupt;
    }
    const u64 frame_bytes =
        kFrameHeaderBytes + static_cast<u64>(payload_length) + kFrameTrailerBytes;
    if (offset + frame_bytes > file_size) {
      std::fclose(file);
      bad_offset = offset;
      return ScanStatus::TornTail;
    }
    RawFrame frame;
    frame.kind = static_cast<RecordKind>(kind_raw);
    frame.sequence = read_u64(header.data() + 12);
    frame.payload.resize(payload_length);
    if (payload_length > 0 &&
        std::fread(frame.payload.data(), 1, payload_length, file) != payload_length) {
      std::fclose(file);
      bad_offset = offset;
      return ScanStatus::TornTail;
    }
    std::vector<u8> trailer(kFrameTrailerBytes, 0);
    if (std::fread(trailer.data(), 1, kFrameTrailerBytes, file) != kFrameTrailerBytes) {
      std::fclose(file);
      bad_offset = offset;
      return ScanStatus::TornTail;
    }
    const u32 expected_payload_crc = read_u32(header.data() + 20);
    const u32 actual_payload_crc =
        payload_length == 0 ? 0U : crc32c(frame.payload.data(), payload_length);
    if (expected_payload_crc != actual_payload_crc) {
      std::fclose(file);
      bad_offset = offset;
      return ScanStatus::Corrupt;
    }
    Crc32c crc;
    crc.update(header.data(), kFrameHeaderBytes);
    if (payload_length > 0) {
      crc.update(frame.payload.data(), payload_length);
    }
    if (crc.value() != read_u32(trailer.data())) {
      std::fclose(file);
      bad_offset = offset;
      return ScanStatus::Corrupt;
    }
    frame.offset = offset;
    frame.frame_bytes = frame_bytes;
    frames.push_back(std::move(frame));
    offset += frame_bytes;
  }
  if (offset != file_size) {
    // Trailing bytes that cannot hold a complete frame.
    std::fclose(file);
    bad_offset = offset;
    return ScanStatus::TornTail;
  }
  std::fclose(file);
  return ScanStatus::Clean;
}

[[nodiscard]] std::string generation_file_name(const char* prefix, u32 generation) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(prefix);
  out.push_back('-');
  for (int shift = 28; shift >= 0; shift -= 4) {
    out.push_back(kHex[(generation >> static_cast<unsigned>(shift)) & 0xFU]);
  }
  out += ".ofab";
  return out;
}

[[nodiscard]] bool parse_generation_file_name(std::string_view name, std::string_view prefix,
                                              u32& generation) {
  if (name.size() != prefix.size() + 1 + 8 + 5) {
    return false;
  }
  if (name.substr(0, prefix.size()) != prefix || name[prefix.size()] != '-') {
    return false;
  }
  if (name.substr(name.size() - 5) != ".ofab") {
    return false;
  }
  u32 value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    const char c = name[prefix.size() + 1 + index];
    u32 digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<u32>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<u32>(c - 'a') + 10U;
    } else {
      return false;
    }
    value = (value << 4U) | digit;
  }
  generation = value;
  return true;
}

[[nodiscard]] bool payload_u64(const std::vector<u8>& payload, u64& out) {
  if (payload.size() != 8) {
    return false;
  }
  out = read_u64(payload.data());
  return true;
}

[[nodiscard]] std::vector<u8> encode_u64(u64 value) {
  std::vector<u8> payload(8, 0);
  put_u64_at(payload, 0, value);
  return payload;
}

}  // namespace

std::string_view record_kind_text(RecordKind kind) noexcept {
  for (const RecordKindEntry& entry : kRecordKinds) {
    if (entry.kind == kind) {
      return entry.text;
    }
  }
  return "unknown";
}

bool record_kind_parse(std::string_view text, RecordKind& out) noexcept {
  for (const RecordKindEntry& entry : kRecordKinds) {
    if (entry.text == text) {
      out = entry.kind;
      return true;
    }
  }
  return false;
}

std::string_view recovery_class_text(RecoveryClass value) noexcept {
  for (const RecoveryClassEntry& entry : kRecoveryClasses) {
    if (entry.value == value) {
      return entry.text;
    }
  }
  return "unknown";
}

bool recovery_class_parse(std::string_view text, RecoveryClass& out) noexcept {
  for (const RecoveryClassEntry& entry : kRecoveryClasses) {
    if (entry.text == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

bool recovery_class_is_refusal(RecoveryClass value) noexcept {
  return value == RecoveryClass::Corrupt || value == RecoveryClass::VersionIncompatible ||
         value == RecoveryClass::Unavailable;
}

std::string snapshot_file_name(u32 generation) {
  return generation_file_name("snapshot", generation);
}

std::string journal_file_name(u32 generation) { return generation_file_name("journal", generation); }

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

Store::~Store() { close(); }

Store::Store(Store&& other) noexcept
    : options_(std::move(other.options_)),
      journal_(other.journal_),
      journal_bytes_(other.journal_bytes_),
      next_sequence_(other.next_sequence_),
      journal_generation_(other.journal_generation_),
      snapshot_generation_(other.snapshot_generation_) {
  other.journal_ = nullptr;
}

Store& Store::operator=(Store&& other) noexcept {
  if (this != &other) {
    close();
    options_ = std::move(other.options_);
    journal_ = other.journal_;
    journal_bytes_ = other.journal_bytes_;
    next_sequence_ = other.next_sequence_;
    journal_generation_ = other.journal_generation_;
    snapshot_generation_ = other.snapshot_generation_;
    other.journal_ = nullptr;
  }
  return *this;
}

void Store::close() noexcept {
  if (journal_ != nullptr) {
    std::fclose(journal_);
    journal_ = nullptr;
  }
}

bool Store::rotation_due() const noexcept { return journal_bytes_ >= options_.max_journal_bytes; }

std::optional<Store> Store::open(const Options& options, RecoveryReport& report,
                                 std::vector<u8>& snapshot_payload,
                                 std::vector<Record>& recovered_records) {
  snapshot_payload.clear();
  recovered_records.clear();
  report = RecoveryReport{};
  report.reason = Reason::Ok;

  if (options.directory.empty()) {
    report.classification = RecoveryClass::Unavailable;
    report.reason = Reason::StoreUnavailable;
    return std::nullopt;
  }

  std::error_code ec;
  std::filesystem::create_directories(options.directory, ec);
  if (ec) {
    report.classification = RecoveryClass::Unavailable;
    report.reason = Reason::StoreUnavailable;
    return std::nullopt;
  }

  Store store;
  store.options_ = options;

  u32 best_snapshot = 0;
  bool has_snapshot = false;
  u32 best_journal = 0;
  bool has_journal = false;

  for (const auto& entry : std::filesystem::directory_iterator(options.directory, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    u32 generation = 0;
    if (parse_generation_file_name(name, "snapshot", generation)) {
      if (!has_snapshot || generation > best_snapshot) {
        best_snapshot = generation;
        has_snapshot = true;
      }
      continue;
    }
    if (parse_generation_file_name(name, "journal", generation)) {
      if (!has_journal || generation > best_journal) {
        best_journal = generation;
        has_journal = true;
      }
    }
  }

  report.snapshot_generation = has_snapshot ? best_snapshot : 0;
  report.journal_generation = has_journal ? best_journal : 0;

  if (has_snapshot) {
    std::vector<u8> frame;
    if (!read_entire_file(options.directory / snapshot_file_name(best_snapshot), kMaxSnapshotBytes,
                          frame)) {
      report.classification = RecoveryClass::Corrupt;
      report.reason = Reason::SnapshotCorrupt;
      return std::nullopt;
    }
    SnapshotHeader header;
    if (!parse_snapshot_header(frame.data(), frame.size(), header)) {
      report.classification = RecoveryClass::Corrupt;
      report.reason = Reason::SnapshotCorrupt;
      return std::nullopt;
    }
    if (header.format_version != kStoreFormatVersion) {
      report.classification = RecoveryClass::VersionIncompatible;
      report.reason = Reason::StoreVersionIncompatible;
      report.format_version = header.format_version;
      return std::nullopt;
    }
    if (header.payload_length > kMaxSnapshotBytes ||
        kSnapshotHeaderBytes + static_cast<std::size_t>(header.payload_length) != frame.size()) {
      report.classification = RecoveryClass::Corrupt;
      report.reason = Reason::SnapshotCorrupt;
      return std::nullopt;
    }
    snapshot_payload.assign(frame.begin() + static_cast<std::ptrdiff_t>(kSnapshotHeaderBytes),
                            frame.end());
    if (!snapshot_payload.empty() &&
        crc32c(snapshot_payload.data(), snapshot_payload.size()) !=
            read_u32(frame.data() + 12)) {
      report.classification = RecoveryClass::Corrupt;
      report.reason = Reason::SnapshotCorrupt;
      return std::nullopt;
    }
    report.bytes_accepted += frame.size();
    store.snapshot_generation_ = best_snapshot;
  }

  u32 journal_generation = has_snapshot ? best_snapshot : (has_journal ? best_journal : 1);
  if (has_journal && best_journal > journal_generation) {
    journal_generation = best_journal;
  }

  std::vector<RawFrame> frames;
  u64 bad_offset = 0;
  u64 file_size = 0;
  bool replayed_any = false;
  if (has_journal && best_journal >= (has_snapshot ? best_snapshot : 0)) {
    const std::filesystem::path journal_path = options.directory / journal_file_name(best_journal);
    const ScanStatus status =
        scan_journal(journal_path, options, frames, bad_offset, file_size);
    switch (status) {
      case ScanStatus::Corrupt:
        report.classification = RecoveryClass::Corrupt;
        report.reason = Reason::StoreCorrupt;
        return std::nullopt;
      case ScanStatus::VersionIncompatible:
        report.classification = RecoveryClass::VersionIncompatible;
        report.reason = Reason::StoreVersionIncompatible;
        return std::nullopt;
      case ScanStatus::TooLarge:
        report.classification = RecoveryClass::Corrupt;
        report.reason = Reason::Oversized;
        return std::nullopt;
      case ScanStatus::TornTail:
        report.classification = RecoveryClass::TornTailRepaired;
        report.reason = Reason::TornTailTruncated;
        report.bytes_discarded += file_size - bad_offset;
        report.truncated_tail_repaired = true;
        break;
      case ScanStatus::Clean:
      default:
        break;
    }
    journal_generation = best_journal;
    replayed_any = true;
  } else if (!has_snapshot && !has_journal) {
    report.classification = RecoveryClass::Empty;
    report.reason = Reason::Ok;
  }

  // Assemble transactions. Only a complete Begin..Commit group is applied.
  bool in_transaction = false;
  u64 transaction_id = 0;
  std::vector<Record> pending;
  u64 previous_sequence = 0;
  bool have_previous_sequence = false;
  for (const RawFrame& frame : frames) {
    if (have_previous_sequence && frame.sequence <= previous_sequence) {
      report.classification = RecoveryClass::Corrupt;
      report.reason = Reason::Corrupt;
      return std::nullopt;
    }
    previous_sequence = frame.sequence;
    have_previous_sequence = true;

    if (frame.kind == RecordKind::BeginTransaction) {
      if (in_transaction) {
        report.classification = RecoveryClass::Corrupt;
        report.reason = Reason::Corrupt;
        return std::nullopt;
      }
      if (!payload_u64(frame.payload, transaction_id)) {
        report.classification = RecoveryClass::Corrupt;
        report.reason = Reason::Malformed;
        return std::nullopt;
      }
      in_transaction = true;
      pending.clear();
      continue;
    }

    if (frame.kind == RecordKind::CommitTransaction) {
      if (!in_transaction) {
        report.classification = RecoveryClass::Corrupt;
        report.reason = Reason::Corrupt;
        return std::nullopt;
      }
      u64 commit_id = 0;
      if (!payload_u64(frame.payload, commit_id) || commit_id != transaction_id) {
        report.classification = RecoveryClass::Corrupt;
        report.reason = Reason::Corrupt;
        return std::nullopt;
      }
      in_transaction = false;
      ++report.transactions_applied;
      for (const Record& record : pending) {
        recovered_records.push_back(record);
        ++report.records_replayed;
      }
      pending.clear();
      continue;
    }

    if (!in_transaction) {
      report.classification = RecoveryClass::Corrupt;
      report.reason = Reason::Corrupt;
      return std::nullopt;
    }
    if (recovered_records.size() + pending.size() >= kMaxReplayRecords) {
      report.classification = RecoveryClass::Corrupt;
      report.reason = Reason::JournalCapacityExceeded;
      return std::nullopt;
    }
    pending.push_back(Record{frame.kind, frame.payload});
  }

  if (in_transaction) {
    ++report.transactions_dropped;
    report.reason = Reason::IncompleteTransactionDropped;
    if (report.classification == RecoveryClass::Empty ||
        report.classification == RecoveryClass::CleanReopen) {
      report.classification = RecoveryClass::TransactionDropped;
    }
  }

  if (report.classification == RecoveryClass::Empty && (has_snapshot || replayed_any)) {
    report.classification = RecoveryClass::CleanReopen;
  }

  {
    DigestBuilder builder;
    for (const Record& record : recovered_records) {
      const u16 raw = static_cast<u16>(record.kind);
      builder.update(reinterpret_cast<const u8*>(&raw), sizeof(raw));
      if (!record.payload.empty()) {
        builder.update(record.payload.data(), record.payload.size());
      }
    }
    report.replayed_digest = builder.value();
  }

  if (!options.read_only) {
    const std::filesystem::path journal_path =
        options.directory / journal_file_name(journal_generation);
    if (report.truncated_tail_repaired) {
      if (!truncate_file(journal_path, bad_offset)) {
        report.classification = RecoveryClass::Unavailable;
        report.reason = Reason::StoreUnavailable;
        return std::nullopt;
      }
    }
    store.journal_ = std::fopen(journal_path.string().c_str(), "ab");
    if (store.journal_ == nullptr) {
      report.classification = RecoveryClass::Unavailable;
      report.reason = Reason::StoreUnavailable;
      return std::nullopt;
    }
    store.journal_bytes_ = report.truncated_tail_repaired
                               ? bad_offset
                               : (has_journal && best_journal == journal_generation
                                      ? file_size
                                      : static_cast<u64>(0));
    store.next_sequence_ = previous_sequence + 1;
    store.journal_generation_ = journal_generation;
  } else {
    store.journal_generation_ = journal_generation;
  }

  report.bytes_accepted += report.truncated_tail_repaired ? bad_offset : file_size;
  return store;
}

Reason Store::commit_transaction(const std::vector<Record>& records) {
  if (options_.read_only) {
    return Reason::StoreUnavailable;
  }
  if (journal_ == nullptr) {
    return Reason::StoreUnavailable;
  }
  if (records.empty()) {
    return Reason::Malformed;
  }
  for (const Record& record : records) {
    if (record.kind == RecordKind::BeginTransaction ||
        record.kind == RecordKind::CommitTransaction) {
      return Reason::Malformed;
    }
    if (record.payload.size() > options_.max_record_payload_bytes) {
      return Reason::PayloadTooLarge;
    }
  }

  u64 last_sequence = 0;
  if (add_overflow(next_sequence_, static_cast<u64>(records.size()) + 1U, last_sequence)) {
    return Reason::GenerationExhausted;
  }

  std::vector<std::vector<u8>> frames;
  frames.reserve(records.size() + 2U);
  const std::vector<u8> begin_payload = encode_u64(next_sequence_);
  frames.push_back(
      build_record_frame(RecordKind::BeginTransaction, next_sequence_, begin_payload.data(),
                         begin_payload.size()));
  u64 sequence = next_sequence_ + 1U;
  for (const Record& record : records) {
    frames.push_back(build_record_frame(
        record.kind, sequence,
        record.payload.empty() ? nullptr : record.payload.data(), record.payload.size()));
    ++sequence;
  }
  const std::vector<u8> commit_payload = encode_u64(next_sequence_);
  frames.push_back(
      build_record_frame(RecordKind::CommitTransaction, sequence, commit_payload.data(),
                         commit_payload.size()));

  for (const std::vector<u8>& frame : frames) {
    if (std::fwrite(frame.data(), 1, frame.size(), journal_) != frame.size()) {
      return Reason::PersistenceWriteFailed;
    }
  }
  if (!sync_file(journal_)) {
    return Reason::PersistenceWriteFailed;
  }
  for (const std::vector<u8>& frame : frames) {
    journal_bytes_ += frame.size();
  }
  next_sequence_ = sequence + 1U;
  return Reason::Ok;
}

Reason Store::compact(const u8* snapshot, std::size_t size) {
  if (options_.read_only) {
    return Reason::StoreUnavailable;
  }
  if (size > kMaxSnapshotBytes) {
    return Reason::Oversized;
  }
  if (journal_generation_ == (std::numeric_limits<u32>::max)()) {
    return Reason::GenerationExhausted;
  }
  const u32 next_generation = journal_generation_ + 1U;

  const std::vector<u8> frame = build_snapshot_frame(snapshot, size);
  const std::filesystem::path snapshot_path =
      options_.directory / snapshot_file_name(next_generation);
  if (!write_file_atomically(snapshot_path, frame.data(), frame.size())) {
    return Reason::PersistenceWriteFailed;
  }

  close();
  const std::filesystem::path journal_path =
      options_.directory / journal_file_name(next_generation);
  std::FILE* fresh = std::fopen(journal_path.string().c_str(), "wb");
  if (fresh == nullptr) {
    return Reason::PersistenceWriteFailed;
  }
  if (!sync_file(fresh)) {
    std::fclose(fresh);
    return Reason::PersistenceWriteFailed;
  }
  std::fclose(fresh);
  journal_ = std::fopen(journal_path.string().c_str(), "ab");
  if (journal_ == nullptr) {
    return Reason::PersistenceWriteFailed;
  }
  journal_bytes_ = 0;
  next_sequence_ = 1;
  snapshot_generation_ = next_generation;
  journal_generation_ = next_generation;

  // Remove generations older than the retention bound. Failures here are not
  // correctness-relevant: an extra retained file only costs disk space.
  const u32 retain = (std::max)(1U, options_.retained_generations);
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(options_.directory, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    u32 generation = 0;
    const bool is_snapshot = parse_generation_file_name(name, "snapshot", generation);
    const bool is_journal = !is_snapshot && parse_generation_file_name(name, "journal", generation);
    if (!is_snapshot && !is_journal) {
      continue;
    }
    if (next_generation > retain && generation < next_generation - retain) {
      std::filesystem::remove(entry.path(), ec);
    }
  }
  return Reason::Ok;
}

}  // namespace off
