// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "off/digest.hpp"

#include <array>

namespace off {
namespace {

constexpr u32 kCrc32cPolynomial = 0x82F63B78U;

constexpr std::array<u32, 256> make_crc_table() noexcept {
  std::array<u32, 256> table{};
  for (u32 index = 0; index < 256; ++index) {
    u32 value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1U) != 0U ? (value >> 1U) ^ kCrc32cPolynomial : value >> 1U;
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<u32, 256> kCrcTable = make_crc_table();

constexpr std::array<char, 16> kHexDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

[[nodiscard]] constexpr int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

constexpr u64 kFnvOffsetA = 0xcbf29ce484222325ULL;
constexpr u64 kFnvPrime = 0x00000100000001B3ULL;
constexpr u64 kFnvOffsetB = 0x9e3779b97f4a7c15ULL;
constexpr u64 kFnvPrimeB = 0x00000100000001B5ULL;

}  // namespace

u32 crc32c(const u8* data, std::size_t size, u32 seed) noexcept {
  u32 state = seed ^ 0xFFFFFFFFU;
  for (std::size_t index = 0; index < size; ++index) {
    state = kCrcTable[(state ^ data[index]) & 0xFFU] ^ (state >> 8U);
  }
  return state ^ 0xFFFFFFFFU;
}

void Crc32c::update(const u8* data, std::size_t size) noexcept {
  u32 state = state_ ^ 0xFFFFFFFFU;
  for (std::size_t index = 0; index < size; ++index) {
    state = kCrcTable[(state ^ data[index]) & 0xFFU] ^ (state >> 8U);
  }
  state_ = state ^ 0xFFFFFFFFU;
}

void DigestBuilder::update(const u8* data, std::size_t size) noexcept {
  for (std::size_t index = 0; index < size; ++index) {
    const u8 byte = data[index];
    hi_ ^= byte;
    hi_ *= kFnvPrime;
    lo_ += byte;
    lo_ ^= lo_ >> 29U;
    lo_ *= kFnvPrimeB;
  }
}

Digest DigestBuilder::value() const noexcept {
  Digest out;
  out.hi = hi_;
  out.lo = lo_ ^ (hi_ >> 17U);
  return out;
}

std::string Digest::to_hex() const {
  std::string out;
  out.reserve(32);
  for (int shift = 60; shift >= 0; shift -= 4) {
    out.push_back(kHexDigits[static_cast<std::size_t>((hi >> static_cast<unsigned>(shift)) & 0xFU)]);
  }
  for (int shift = 60; shift >= 0; shift -= 4) {
    out.push_back(kHexDigits[static_cast<std::size_t>((lo >> static_cast<unsigned>(shift)) & 0xFU)]);
  }
  return out;
}

bool Digest::from_hex(std::string_view text, Digest& out) noexcept {
  if (text.size() != 32) {
    return false;
  }
  u64 hi = 0;
  u64 lo = 0;
  for (std::size_t index = 0; index < 32; ++index) {
    const int digit = hex_value(text[index]);
    if (digit < 0) {
      return false;
    }
    if (index < 16) {
      hi = (hi << 4U) | static_cast<u64>(digit);
    } else {
      lo = (lo << 4U) | static_cast<u64>(digit);
    }
  }
  out.hi = hi;
  out.lo = lo;
  return true;
}

}  // namespace off
