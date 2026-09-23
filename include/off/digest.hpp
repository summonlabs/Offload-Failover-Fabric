// Offload Failover Fabric - integrity checksums and canonical digests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "off/strong.hpp"

namespace off {

/// CRC-32C (Castagnoli) used for record and frame integrity.
[[nodiscard]] u32 crc32c(const u8* data, std::size_t size, u32 seed = 0) noexcept;

/// Incremental CRC-32C so large payloads are never copied to be checksummed.
class Crc32c {
 public:
  void update(const u8* data, std::size_t size) noexcept;
  [[nodiscard]] u32 value() const noexcept { return state_; }

 private:
  u32 state_{0};
};

/// 128-bit content digest. Canonical and stable across platforms and builds.
struct Digest {
  u64 hi{0};
  u64 lo{0};

  friend constexpr bool operator==(Digest, Digest) noexcept = default;
  friend constexpr auto operator<=>(Digest, Digest) noexcept = default;
  [[nodiscard]] constexpr bool is_zero() const noexcept { return hi == 0 && lo == 0; }

  /// Fixed-width lowercase hexadecimal, 32 characters, no separators.
  [[nodiscard]] std::string to_hex() const;

  /// Parse the canonical 32-character hexadecimal form. Rejects any other
  /// shape, mixed case variance, or non-hex characters.
  [[nodiscard]] static bool from_hex(std::string_view text, Digest& out) noexcept;
};

/// Streaming digest over canonical byte representations.
class DigestBuilder {
 public:
  void update(const u8* data, std::size_t size) noexcept;
  [[nodiscard]] Digest value() const noexcept;

 private:
  u64 hi_{0xcbf29ce484222325ULL};
  u64 lo_{0x9e3779b97f4a7c15ULL};
};

}  // namespace off
