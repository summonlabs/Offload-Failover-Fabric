// Offload Failover Fabric - strongly typed identities, generations, units and
// checked arithmetic. No correctness-critical semantic is carried by a bare
// integer or an untyped string anywhere above this header.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace off {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// ---------------------------------------------------------------------------
// Checked arithmetic. Every externally derived size, timestamp and counter is
// promoted through these helpers before it is used, stored or allocated.
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr bool add_overflow(u64 a, u64 b, u64& out) noexcept {
  if (a > (std::numeric_limits<u64>::max)() - b) {
    return true;
  }
  out = a + b;
  return false;
}

[[nodiscard]] constexpr bool add_overflow(u32 a, u32 b, u32& out) noexcept {
  if (a > (std::numeric_limits<u32>::max)() - b) {
    return true;
  }
  out = a + b;
  return false;
}

[[nodiscard]] constexpr bool mul_overflow(u64 a, u64 b, u64& out) noexcept {
  if (a != 0 && b > (std::numeric_limits<u64>::max)() / a) {
    return true;
  }
  out = a * b;
  return false;
}

[[nodiscard]] constexpr bool mul_overflow(u32 a, u32 b, u32& out) noexcept {
  if (a != 0 && b > (std::numeric_limits<u32>::max)() / a) {
    return true;
  }
  out = a * b;
  return false;
}

/// Narrowing conversion that reports failure instead of wrapping.
[[nodiscard]] constexpr bool narrow(u64 value, u32& out) noexcept {
  if (value > static_cast<u64>((std::numeric_limits<u32>::max)())) {
    return true;
  }
  out = static_cast<u32>(value);
  return false;
}

[[nodiscard]] constexpr bool narrow(u64 value, u16& out) noexcept {
  if (value > static_cast<u64>((std::numeric_limits<u16>::max)())) {
    return true;
  }
  out = static_cast<u16>(value);
  return false;
}

/// Bounded conversion used for externally supplied sizes.
[[nodiscard]] constexpr bool fits_size(u64 value, std::size_t& out) noexcept {
  if (value > static_cast<u64>((std::numeric_limits<std::size_t>::max)())) {
    return true;
  }
  out = static_cast<std::size_t>(value);
  return false;
}

/// Saturating increment used only for observable accounting counters.
[[nodiscard]] constexpr u64 saturating_inc(u64 value) noexcept {
  return value == (std::numeric_limits<u64>::max)() ? value : value + 1;
}

// ---------------------------------------------------------------------------
// Expected<T> - explicit success/failure without exceptions in the core.
// ---------------------------------------------------------------------------

enum class ErrorTag { Value, Code };

template <class T>
class Expected {
 public:
  Expected(T value) : payload_(std::in_place_index<0>, std::move(value)) {}
  Expected(std::uint16_t code) : payload_(std::in_place_index<1>, code) {}

  [[nodiscard]] bool has_value() const noexcept { return payload_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<0>(payload_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(payload_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(payload_)); }
  [[nodiscard]] std::uint16_t code() const noexcept { return std::get<1>(payload_); }

 private:
  std::variant<T, std::uint16_t> payload_;
};

// ---------------------------------------------------------------------------
// Id<Tag> - opaque identity. Zero is the nil identity and never names a real
// entity; every allocator refuses to mint it.
// ---------------------------------------------------------------------------

template <class Tag>
class Id {
 public:
  using rep = u64;
  using tag = Tag;

  constexpr Id() noexcept = default;
  constexpr explicit Id(rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Id nil() noexcept { return Id{}; }
  [[nodiscard]] static constexpr Id from_value(rep value) noexcept { return Id{value}; }

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_ == 0; }

  friend constexpr bool operator==(Id, Id) noexcept = default;
  friend constexpr auto operator<=>(Id, Id) noexcept = default;

 private:
  rep value_{0};
};

// ---------------------------------------------------------------------------
// Gen<Tag> - monotonic generation. Generation zero means "never advanced".
// Advancing is checked; exhaustion is reported, never wrapped.
// ---------------------------------------------------------------------------

template <class Tag>
class Gen {
 public:
  using rep = u64;
  using tag = Tag;

  constexpr Gen() noexcept = default;
  constexpr explicit Gen(rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Gen initial() noexcept { return Gen{}; }
  [[nodiscard]] static constexpr Gen from_value(rep value) noexcept { return Gen{value}; }

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_initial() const noexcept { return value_ == 0; }

  /// Returns the successor, or nullopt when the generation space is exhausted.
  [[nodiscard]] constexpr std::optional<Gen> successor() const noexcept {
    if (value_ == (std::numeric_limits<rep>::max)()) {
      return std::nullopt;
    }
    return Gen{value_ + 1};
  }

  friend constexpr bool operator==(Gen, Gen) noexcept = default;
  friend constexpr auto operator<=>(Gen, Gen) noexcept = default;

 private:
  rep value_{0};
};

/// Monotonic sequence number supplied by an evidence source.
template <class Tag>
using Seq = Gen<Tag>;

/// Logical coordinator tick. Not wall clock; advances only on coordinator work.
template <class Tag>
class Tick {
 public:
  using rep = u64;
  using tag = Tag;

  constexpr Tick() noexcept = default;
  constexpr explicit Tick(rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Tick zero() noexcept { return Tick{}; }
  [[nodiscard]] static constexpr Tick from_value(rep value) noexcept { return Tick{value}; }
  [[nodiscard]] constexpr rep value() const noexcept { return value_; }

  [[nodiscard]] constexpr std::optional<Tick> successor() const noexcept {
    if (value_ == (std::numeric_limits<rep>::max)()) {
      return std::nullopt;
    }
    return Tick{value_ + 1};
  }

  friend constexpr bool operator==(Tick, Tick) noexcept = default;
  friend constexpr auto operator<=>(Tick, Tick) noexcept = default;

 private:
  rep value_{0};
};

/// Strong quantity wrapper used for sizes, durations and counts. The tag makes
/// bytes, milliseconds and counts mutually incompatible at compile time.
template <class Tag>
class Qty {
 public:
  using rep = u64;
  using tag = Tag;

  constexpr Qty() noexcept = default;
  constexpr explicit Qty(rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Qty from_value(rep value) noexcept { return Qty{value}; }
  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  friend constexpr bool operator==(Qty, Qty) noexcept = default;
  friend constexpr auto operator<=>(Qty, Qty) noexcept = default;

 private:
  rep value_{0};
};

// ---------------------------------------------------------------------------
// Name<Tag> - canonical, bounded, validated entity name. Parsing is the only
// route from external bytes into a name; the result is a distinct type per tag.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxNameLength = 64;

[[nodiscard]] bool is_valid_name_char(char c) noexcept;

template <class Tag>
class Name {
 public:
  using tag = Tag;

  Name() = default;

  /// Parse from external bytes. Rejects empty, over-long, non-canonical and
  /// control-bearing inputs. Returns nullopt rather than a partial value.
  [[nodiscard]] static std::optional<Name> parse(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxNameLength) {
      return std::nullopt;
    }
    for (char c : text) {
      if (!is_valid_name_char(c)) {
        return std::nullopt;
      }
    }
    Name result;
    result.value_.assign(text);
    return result;
  }

  /// Parse a canonical encoding field. An empty name is a well-formed encoding
  /// of "no entity"; whether that is acceptable is a domain decision made by
  /// the operation, never by the decoder.
  [[nodiscard]] static std::optional<Name> parse_canonical(std::string_view text) noexcept {
    if (text.size() > kMaxNameLength) {
      return std::nullopt;
    }
    for (char c : text) {
      if (!is_valid_name_char(c)) {
        return std::nullopt;
      }
    }
    Name result;
    result.value_.assign(text);
    return result;
  }

  /// Trusted-input constructor for compile-time fixtures and tests.
  [[nodiscard]] static Name literal(std::string_view text) noexcept;

  [[nodiscard]] const std::string& str() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  friend bool operator==(const Name&, const Name&) noexcept = default;
  friend auto operator<=>(const Name&, const Name&) noexcept = default;

 private:
  std::string value_{};
};

template <class Tag>
Name<Tag> Name<Tag>::literal(std::string_view text) noexcept {
  Name result;
  result.value_.assign(text);
  return result;
}

}  // namespace off

namespace std {

template <class Tag>
struct hash<off::Id<Tag>> {
  [[nodiscard]] size_t operator()(const off::Id<Tag>& id) const noexcept {
    return std::hash<off::u64>{}(id.value());
  }
};

template <class Tag>
struct hash<off::Gen<Tag>> {
  [[nodiscard]] size_t operator()(const off::Gen<Tag>& gen) const noexcept {
    return std::hash<off::u64>{}(gen.value());
  }
};

template <class Tag>
struct hash<off::Name<Tag>> {
  [[nodiscard]] size_t operator()(const off::Name<Tag>& name) const noexcept {
    return std::hash<std::string_view>{}(name.view());
  }
};

}  // namespace std
