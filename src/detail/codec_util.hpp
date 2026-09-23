// Offload Failover Fabric - shared canonical codec helpers used by the model
// codecs and the runtime state codecs. Internal to the implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "off/canonical.hpp"
#include "off/ids.hpp"
#include "off/reason.hpp"

namespace off {
namespace detail {

template <class Tag>
inline void encode_name(CanonicalWriter& writer, const Name<Tag>& value) {
  writer.put_text(value.view());
}

template <class Tag>
inline bool decode_name(CanonicalReader& reader, Name<Tag>& out) {
  std::string text;
  if (!reader.get_text(text)) {
    return false;
  }
  const std::optional<Name<Tag>> parsed = Name<Tag>::parse_canonical(text);
  if (!parsed.has_value()) {
    reader.fail(Reason::Malformed);
    return false;
  }
  out = *parsed;
  return true;
}

template <class IdTag>
inline void encode_id(CanonicalWriter& writer, const Id<IdTag>& value) {
  writer.put_u64(value.value());
}

template <class IdTag>
inline bool decode_id(CanonicalReader& reader, Id<IdTag>& out) {
  u64 raw = 0;
  if (!reader.get_u64(raw)) {
    return false;
  }
  out = Id<IdTag>::from_value(raw);
  return true;
}

template <class GenTag>
inline void encode_gen(CanonicalWriter& writer, const Gen<GenTag>& value) {
  writer.put_u64(value.value());
}

template <class GenTag>
inline bool decode_gen(CanonicalReader& reader, Gen<GenTag>& out) {
  u64 raw = 0;
  if (!reader.get_u64(raw)) {
    return false;
  }
  out = Gen<GenTag>::from_value(raw);
  return true;
}

inline void encode_tick(CanonicalWriter& writer, const LogicalTick& value) {
  writer.put_u64(value.value());
}

inline bool decode_tick(CanonicalReader& reader, LogicalTick& out) {
  u64 raw = 0;
  if (!reader.get_u64(raw)) {
    return false;
  }
  out = LogicalTick::from_value(raw);
  return true;
}

inline void encode_digest(CanonicalWriter& writer, const Digest& value) {
  writer.put_u64(value.hi);
  writer.put_u64(value.lo);
}

inline bool decode_digest(CanonicalReader& reader, Digest& out) {
  u64 hi = 0;
  u64 lo = 0;
  if (!reader.get_u64(hi)) {
    return false;
  }
  if (!reader.get_u64(lo)) {
    return false;
  }
  out.hi = hi;
  out.lo = lo;
  return true;
}

/// Reads a reason code as a u16 and rejects any value this build does not know.
inline bool decode_reason(CanonicalReader& reader, Reason& out) {
  u16 raw = 0;
  if (!reader.get_u16(raw)) {
    return false;
  }
  const auto parsed = static_cast<Reason>(raw);
  if (reason_code_text(parsed) == "UNRECOGNIZED_REASON") {
    reader.fail(Reason::UnsupportedSemantics);
    return false;
  }
  out = parsed;
  return true;
}

/// Reads a small enum stored as one byte, rejecting out-of-range ordinals.
inline bool decode_ordinal_u8(CanonicalReader& reader, std::size_t count, u8& out) {
  u8 raw = 0;
  if (!reader.get_u8(raw)) {
    return false;
  }
  if (static_cast<std::size_t>(raw) >= count) {
    reader.fail(Reason::UnsupportedSemantics);
    return false;
  }
  out = raw;
  return true;
}

template <class T, class EncodeFn>
inline void encode_list(CanonicalWriter& writer, const std::vector<T>& items, EncodeFn encode_one) {
  if (!writer.put_count(items.size())) {
    return;
  }
  for (const T& item : items) {
    encode_one(writer, item);
  }
}

}  // namespace detail
}  // namespace off
