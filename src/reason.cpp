// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "off/reason.hpp"

#include <array>

namespace off {
namespace {

struct ReasonEntry {
  Reason reason;
  std::string_view text;
};

constexpr ReasonEntry kReasons[] = {
#define OFF_REASON_ENTRY(identifier, text, value) {Reason::identifier, text},
    OFF_REASON_TABLE(OFF_REASON_ENTRY)
#undef OFF_REASON_ENTRY
};

[[nodiscard]] constexpr std::string_view family_of(u16 family_index) noexcept {
  switch (family_index) {
    case 0x00:
      return "success";
    case 0x01:
      return "success";
    case 0x02:
      return "shape";
    case 0x03:
      return "identity";
    case 0x04:
      return "generation";
    case 0x05:
      return "evidence";
    case 0x06:
      return "authority";
    case 0x07:
      return "fence";
    case 0x08:
      return "eligibility";
    case 0x09:
      return "dependency";
    case 0x0A:
      return "lifecycle";
    case 0x0B:
      return "effect";
    case 0x0C:
      return "ambiguity";
    case 0x0D:
      return "statefulness";
    case 0x0E:
      return "failback";
    case 0x0F:
      return "persistence";
    case 0x10:
      return "cancellation";
    case 0x11:
      return "protocol";
    case 0x12:
      return "internal";
    default:
      return "unknown";
  }
}

}  // namespace

std::string_view reason_code_text(Reason reason) noexcept {
  for (const ReasonEntry& entry : kReasons) {
    if (entry.reason == reason) {
      return entry.text;
    }
  }
  return "UNRECOGNIZED_REASON";
}

bool reason_is_accept(Reason reason) noexcept {
  const u16 value = static_cast<u16>(reason);
  return (value >> 8U) <= 0x01U;
}

std::string_view reason_family(Reason reason) noexcept {
  return family_of(static_cast<u16>(static_cast<u16>(reason) >> 8U));
}

std::string reason_summary(Reason reason) {
  const std::string_view code = reason_code_text(reason);
  std::string out;
  out.reserve(code.size());
  for (const char c : code) {
    if (c == '_') {
      out.push_back(' ');
      continue;
    }
    out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

bool reason_from_text(std::string_view text, Reason& out) noexcept {
  for (const ReasonEntry& entry : kReasons) {
    if (entry.text == text) {
      out = entry.reason;
      return true;
    }
  }
  return false;
}

}  // namespace off
