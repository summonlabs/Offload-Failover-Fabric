// Offload Failover Fabric - canonical binary codec and deterministic JSON
// rendering. Every persisted and exported byte sequence is produced here, so
// digests and explanations are stable across platforms and builds.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "off/digest.hpp"
#include "off/reason.hpp"
#include "off/strong.hpp"

namespace off {

/// Allocation and traversal ceilings applied to every decode. Decoding never
/// trusts a length it has not first validated against these bounds.
struct CanonicalLimits {
  std::size_t max_text_bytes{4096};
  std::size_t max_blob_bytes{1U << 20U};
  std::size_t max_items{1U << 20U};
  std::size_t max_depth{32};
  std::size_t max_total_bytes{256U << 20U};

  /// Hard ceiling accepted for a single encoded value on the wire or on disk.
  static constexpr std::size_t kAbsoluteMaxTotalBytes = 1U << 30U;
};

/// Append-only canonical encoder. Integers are big-endian fixed width, text and
/// blobs are length-prefixed with a checked u32, optional values carry an
/// explicit presence byte, and collections carry a checked count that is
/// validated against the configured ceiling before any element is written.
class CanonicalWriter {
 public:
  explicit CanonicalWriter(CanonicalLimits limits = {}) : limits_(limits) {}

  void put_u8(u8 value);
  void put_u16(u16 value);
  void put_u32(u32 value);
  void put_u64(u64 value);
  void put_bool(bool value);
  void put_text(std::string_view value);
  void put_blob(const u8* data, std::size_t size);
  void put_absence();
  void put_presence();

  /// Collection counts. Returns false and records nothing when the count
  /// exceeds the configured item ceiling.
  [[nodiscard]] bool put_count(std::size_t count);

  /// True while every value fit within the configured ceilings. A failed
  /// writer clears its buffer, so a caller that ignored ok() cannot mistake a
  /// truncated encoding for a valid one.
  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  [[nodiscard]] const std::vector<u8>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] Digest digest() const noexcept;
  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view(reinterpret_cast<const char*>(buffer_.data()), buffer_.size());
  }

 private:
  [[nodiscard]] bool reserve(std::size_t additional);

  CanonicalLimits limits_{};
  std::vector<u8> buffer_{};
  bool failed_{false};
};

/// Bounds-checked canonical decoder. Any structural violation sets a sticky
/// error reason; the caller must check ok() before using decoded values.
class CanonicalReader {
 public:
  CanonicalReader(const u8* data, std::size_t size, CanonicalLimits limits = {})
      : data_(data), size_(size), limits_(limits) {
    if (size > limits_.max_total_bytes || size > CanonicalLimits::kAbsoluteMaxTotalBytes) {
      fail(Reason::Oversized);
    }
  }

  [[nodiscard]] bool get_u8(u8& out) noexcept;
  [[nodiscard]] bool get_u16(u16& out) noexcept;
  [[nodiscard]] bool get_u32(u32& out) noexcept;
  [[nodiscard]] bool get_u64(u64& out) noexcept;
  [[nodiscard]] bool get_bool(bool& out) noexcept;
  [[nodiscard]] bool get_text(std::string& out) noexcept;
  [[nodiscard]] bool get_blob(std::vector<u8>& out) noexcept;

  /// Reads an explicit presence byte. Absent values are distinguishable from
  /// zero/false/empty and are never coerced into one another.
  [[nodiscard]] bool get_presence(bool& present) noexcept;

  /// Reads a collection count, validating it against the ceiling and against
  /// the number of bytes that could possibly remain.
  [[nodiscard]] bool get_count(std::size_t element_min_bytes, std::size_t& count) noexcept;

  [[nodiscard]] bool push_depth() noexcept;
  void pop_depth() noexcept;

  [[nodiscard]] bool at_end() const noexcept { return !failed_ && offset_ == size_; }
  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  [[nodiscard]] Reason error() const noexcept { return error_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }

  void fail(Reason reason) noexcept;

 private:
  [[nodiscard]] bool need(std::size_t bytes) noexcept;

  const u8* data_{nullptr};
  std::size_t size_{0};
  std::size_t offset_{0};
  std::size_t depth_{0};
  bool failed_{false};
  Reason error_{Reason::Ok};
  CanonicalLimits limits_{};
};

// ---------------------------------------------------------------------------
// Deterministic JSON. Objects are held in an ordered map so key order is always
// lexicographic, numbers are always integers, and no locale or floating-point
// formatting can perturb the output.
// ---------------------------------------------------------------------------

class JsonValue {
 public:
  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue, std::less<>>;

  JsonValue() : payload_(nullptr) {}
  JsonValue(std::nullptr_t) : payload_(nullptr) {}
  JsonValue(bool value) : payload_(value) {}
  JsonValue(u32 value) : payload_(static_cast<u64>(value)) {}
  JsonValue(u64 value) : payload_(value) {}
  JsonValue(i64 value) : payload_(value) {}
  JsonValue(const char* value) : payload_(std::string(value)) {}
  JsonValue(std::string value) : payload_(std::move(value)) {}
  JsonValue(Array value) : payload_(std::move(value)) {}
  JsonValue(Object value) : payload_(std::move(value)) {}

  [[nodiscard]] bool is_null() const noexcept { return payload_.index() == kNullIndex; }
  [[nodiscard]] bool is_object() const noexcept { return payload_.index() == kObjectIndex; }
  [[nodiscard]] bool is_array() const noexcept { return payload_.index() == kArrayIndex; }

  [[nodiscard]] const Object& as_object() const { return std::get<Object>(payload_); }
  [[nodiscard]] Object& as_object() { return std::get<Object>(payload_); }
  [[nodiscard]] const Array& as_array() const { return std::get<Array>(payload_); }

  /// Insertion into an object. A duplicate key is a programming error: it trips
  /// an assertion in debug builds and, in release builds, is refused so a later
  /// value can never silently shadow an earlier one. Returns false when the key
  /// already existed.
  bool set(std::string key, JsonValue value);

  void push(JsonValue value);

  /// Compact canonical rendering: no spaces, keys sorted, integers only.
  [[nodiscard]] std::string dump() const;
  /// Two-space indented rendering for human inspection. Identical semantics.
  [[nodiscard]] std::string dump_pretty() const;

 private:
  using Payload = std::variant<std::nullptr_t, bool, u64, i64, std::string, Array, Object>;

  /// Alternative indices of the payload variant. Derived from the variant
  /// itself so a reordering can never silently desynchronise the predicates
  /// from the stored alternative.
  static constexpr std::size_t kNullIndex = 0;
  static constexpr std::size_t kArrayIndex = std::variant_size_v<Payload> - 2;
  static constexpr std::size_t kObjectIndex = std::variant_size_v<Payload> - 1;

  void dump_into(std::string& out, int indent) const;

  Payload payload_;
};

/// Renders bytes as lowercase hexadecimal with no separators.
[[nodiscard]] std::string to_hex(const u8* data, std::size_t size);
[[nodiscard]] std::string to_hex(std::string_view bytes);

/// Renders an unsigned value as canonical decimal.
[[nodiscard]] std::string to_decimal(u64 value);
[[nodiscard]] bool parse_decimal(std::string_view text, u64& out) noexcept;

/// Strict two-digit uppercase hex byte formatting used by text tooling.
[[nodiscard]] std::string byte_hex(u8 value);

}  // namespace off
