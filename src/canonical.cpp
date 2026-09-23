// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "off/canonical.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace off {
namespace {

constexpr std::array<char, 16> kHexDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

void append_escaped(std::string& out, const std::string& text) {
  out.push_back('"');
  for (const char raw : text) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20U || c >= 0x7FU) {
          out += "\\u00";
          out.push_back(kHexDigits[(c >> 4U) & 0x0FU]);
          out.push_back(kHexDigits[c & 0x0FU]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

}  // namespace

// ---------------------------------------------------------------------------
// CanonicalWriter
// ---------------------------------------------------------------------------

bool CanonicalWriter::reserve(std::size_t additional) {
  if (failed_) {
    return false;
  }
  const std::size_t limit =
      (std::min)(limits_.max_total_bytes, CanonicalLimits::kAbsoluteMaxTotalBytes);
  if (additional > limit || buffer_.size() > limit - additional) {
    failed_ = true;
    buffer_.clear();
    return false;
  }
  return true;
}

void CanonicalWriter::put_u8(u8 value) {
  if (!reserve(1)) {
    return;
  }
  buffer_.push_back(value);
}

void CanonicalWriter::put_u16(u16 value) {
  if (!reserve(2)) {
    return;
  }
  buffer_.push_back(static_cast<u8>((value >> 8U) & 0xFFU));
  buffer_.push_back(static_cast<u8>(value & 0xFFU));
}

void CanonicalWriter::put_u32(u32 value) {
  if (!reserve(4)) {
    return;
  }
  for (int shift = 24; shift >= 0; shift -= 8) {
    buffer_.push_back(static_cast<u8>((value >> static_cast<unsigned>(shift)) & 0xFFU));
  }
}

void CanonicalWriter::put_u64(u64 value) {
  if (!reserve(8)) {
    return;
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    buffer_.push_back(static_cast<u8>((value >> static_cast<unsigned>(shift)) & 0xFFU));
  }
}

void CanonicalWriter::put_bool(bool value) { put_u8(value ? 1U : 0U); }

void CanonicalWriter::put_text(std::string_view value) {
  if (value.size() > limits_.max_text_bytes) {
    failed_ = true;
    buffer_.clear();
    return;
  }
  u32 encoded = 0;
  if (narrow(static_cast<u64>(value.size()), encoded)) {
    failed_ = true;
    buffer_.clear();
    return;
  }
  if (!reserve(4 + value.size())) {
    return;
  }
  put_u32(encoded);
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void CanonicalWriter::put_blob(const u8* data, std::size_t size) {
  if (size > limits_.max_blob_bytes) {
    failed_ = true;
    buffer_.clear();
    return;
  }
  u32 encoded = 0;
  if (narrow(static_cast<u64>(size), encoded)) {
    failed_ = true;
    buffer_.clear();
    return;
  }
  if (!reserve(4 + size)) {
    return;
  }
  put_u32(encoded);
  buffer_.insert(buffer_.end(), data, data + size);
}

void CanonicalWriter::put_absence() { put_u8(0); }
void CanonicalWriter::put_presence() { put_u8(1); }

bool CanonicalWriter::put_count(std::size_t count) {
  if (failed_) {
    return false;
  }
  if (count > limits_.max_items) {
    failed_ = true;
    buffer_.clear();
    return false;
  }
  u32 encoded = 0;
  if (narrow(static_cast<u64>(count), encoded)) {
    failed_ = true;
    buffer_.clear();
    return false;
  }
  put_u32(encoded);
  return !failed_;
}

Digest CanonicalWriter::digest() const noexcept {
  DigestBuilder builder;
  if (!buffer_.empty()) {
    builder.update(buffer_.data(), buffer_.size());
  }
  return builder.value();
}

// ---------------------------------------------------------------------------
// CanonicalReader
// ---------------------------------------------------------------------------

void CanonicalReader::fail(Reason reason) noexcept {
  if (!failed_) {
    failed_ = true;
    error_ = reason;
  }
}

bool CanonicalReader::need(std::size_t bytes) noexcept {
  if (failed_) {
    return false;
  }
  if (bytes > size_ - offset_) {
    fail(Reason::Truncated);
    return false;
  }
  return true;
}

bool CanonicalReader::get_u8(u8& out) noexcept {
  if (!need(1)) {
    return false;
  }
  out = data_[offset_];
  offset_ += 1;
  return true;
}

bool CanonicalReader::get_u16(u16& out) noexcept {
  if (!need(2)) {
    return false;
  }
  out = static_cast<u16>((static_cast<u16>(data_[offset_]) << 8U) |
                         static_cast<u16>(data_[offset_ + 1]));
  offset_ += 2;
  return true;
}

bool CanonicalReader::get_u32(u32& out) noexcept {
  if (!need(4)) {
    return false;
  }
  u32 value = 0;
  for (int index = 0; index < 4; ++index) {
    value = (value << 8U) | static_cast<u32>(data_[offset_ + static_cast<std::size_t>(index)]);
  }
  offset_ += 4;
  out = value;
  return true;
}

bool CanonicalReader::get_u64(u64& out) noexcept {
  if (!need(8)) {
    return false;
  }
  u64 value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8U) | static_cast<u64>(data_[offset_ + static_cast<std::size_t>(index)]);
  }
  offset_ += 8;
  out = value;
  return true;
}

bool CanonicalReader::get_bool(bool& out) noexcept {
  u8 raw = 0;
  if (!get_u8(raw)) {
    return false;
  }
  if (raw > 1U) {
    fail(Reason::Malformed);
    return false;
  }
  out = raw == 1U;
  return true;
}

bool CanonicalReader::get_text(std::string& out) noexcept {
  u32 length = 0;
  if (!get_u32(length)) {
    return false;
  }
  const std::size_t size = static_cast<std::size_t>(length);
  if (size > limits_.max_text_bytes) {
    fail(Reason::Oversized);
    return false;
  }
  if (!need(size)) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(data_ + offset_), size);
  offset_ += size;
  return true;
}

bool CanonicalReader::get_blob(std::vector<u8>& out) noexcept {
  u32 length = 0;
  if (!get_u32(length)) {
    return false;
  }
  const std::size_t size = static_cast<std::size_t>(length);
  if (size > limits_.max_blob_bytes) {
    fail(Reason::Oversized);
    return false;
  }
  if (!need(size)) {
    return false;
  }
  out.assign(data_ + offset_, data_ + offset_ + size);
  offset_ += size;
  return true;
}

bool CanonicalReader::get_presence(bool& present) noexcept {
  u8 raw = 0;
  if (!get_u8(raw)) {
    return false;
  }
  if (raw > 1U) {
    fail(Reason::Malformed);
    return false;
  }
  present = raw == 1U;
  return true;
}

bool CanonicalReader::get_count(std::size_t element_min_bytes, std::size_t& count) noexcept {
  u32 raw = 0;
  if (!get_u32(raw)) {
    return false;
  }
  const std::size_t value = static_cast<std::size_t>(raw);
  if (value > limits_.max_items) {
    fail(Reason::Oversized);
    return false;
  }
  if (element_min_bytes > 0) {
    const std::size_t affordable = (size_ - offset_) / element_min_bytes;
    if (value > affordable) {
      fail(Reason::Truncated);
      return false;
    }
  }
  count = value;
  return true;
}

bool CanonicalReader::push_depth() noexcept {
  if (failed_) {
    return false;
  }
  if (depth_ >= limits_.max_depth) {
    fail(Reason::DepthExceeded);
    return false;
  }
  ++depth_;
  return true;
}

void CanonicalReader::pop_depth() noexcept {
  if (depth_ > 0) {
    --depth_;
  }
}

// ---------------------------------------------------------------------------
// JsonValue
// ---------------------------------------------------------------------------

bool JsonValue::set(std::string key, JsonValue value) {
  if (!is_object()) {
    payload_ = Object{};
  }
  auto& object = std::get<Object>(payload_);
  // Refusing rather than overwriting is the safety property: a duplicate key
  // can never silently shadow a value that was already recorded.
  return object.emplace(std::move(key), std::move(value)).second;
}

void JsonValue::push(JsonValue value) {
  if (!is_array()) {
    payload_ = Array{};
  }
  std::get<Array>(payload_).push_back(std::move(value));
}

void JsonValue::dump_into(std::string& out, int indent) const {
  const bool pretty = indent >= 0;
  switch (payload_.index()) {
    case 0:
      out += "null";
      return;
    case 1:
      out += std::get<bool>(payload_) ? "true" : "false";
      return;
    case 2:
      out += to_decimal(std::get<u64>(payload_));
      return;
    case 3: {
      const i64 value = std::get<i64>(payload_);
      if (value < 0) {
        out.push_back('-');
        out += to_decimal(static_cast<u64>(-(value + 1)) + 1U);
      } else {
        out += to_decimal(static_cast<u64>(value));
      }
      return;
    }
    case 4:
      append_escaped(out, std::get<std::string>(payload_));
      return;
    case 5: {
      const auto& array = std::get<Array>(payload_);
      if (array.empty()) {
        out += "[]";
        return;
      }
      out.push_back('[');
      bool first = true;
      for (const JsonValue& element : array) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        if (pretty) {
          out.push_back('\n');
          out.append(static_cast<std::size_t>(indent + 2), ' ');
        }
        element.dump_into(out, pretty ? indent + 2 : -1);
      }
      if (pretty) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent), ' ');
      }
      out.push_back(']');
      return;
    }
    case 6: {
      const auto& object = std::get<Object>(payload_);
      if (object.empty()) {
        out += "{}";
        return;
      }
      out.push_back('{');
      bool first = true;
      for (const auto& entry : object) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        if (pretty) {
          out.push_back('\n');
          out.append(static_cast<std::size_t>(indent + 2), ' ');
        }
        append_escaped(out, entry.first);
        out.push_back(':');
        if (pretty) {
          out.push_back(' ');
        }
        entry.second.dump_into(out, pretty ? indent + 2 : -1);
      }
      if (pretty) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent), ' ');
      }
      out.push_back('}');
      return;
    }
    default:
      out += "null";
      return;
  }
}

std::string JsonValue::dump() const {
  std::string out;
  dump_into(out, -1);
  return out;
}

std::string JsonValue::dump_pretty() const {
  std::string out;
  dump_into(out, 0);
  return out;
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::string to_hex(const u8* data, std::size_t size) {
  std::string out;
  out.reserve(size * 2U);
  for (std::size_t index = 0; index < size; ++index) {
    out.push_back(kHexDigits[(data[index] >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[data[index] & 0x0FU]);
  }
  return out;
}

std::string to_hex(std::string_view bytes) {
  return to_hex(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
}

std::string to_decimal(u64 value) {
  if (value == 0) {
    return "0";
  }
  std::string out;
  while (value > 0) {
    out.push_back(static_cast<char>('0' + (value % 10U)));
    value /= 10U;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

bool parse_decimal(std::string_view text, u64& out) noexcept {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  u64 value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    u64 next = 0;
    if (mul_overflow(value, 10U, next)) {
      return false;
    }
    if (add_overflow(next, static_cast<u64>(c - '0'), next)) {
      return false;
    }
    value = next;
  }
  out = value;
  return true;
}

std::string byte_hex(u8 value) {
  std::string out;
  out.push_back(kHexDigits[(value >> 4U) & 0x0FU]);
  out.push_back(kHexDigits[value & 0x0FU]);
  return out;
}

}  // namespace off
