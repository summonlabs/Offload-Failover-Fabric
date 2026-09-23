// Offload Failover Fabric - version surface.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <string_view>

#define OFF_VERSION_MAJOR 1
#define OFF_VERSION_MINOR 0
#define OFF_VERSION_PATCH 0

namespace off {

/// Semantic version of the runtime and of the exported CMake package.
struct Version {
  std::uint32_t major{0};
  std::uint32_t minor{0};
  std::uint32_t patch{0};

  friend constexpr bool operator==(Version, Version) noexcept = default;
  friend constexpr auto operator<=>(Version, Version) noexcept = default;
};

inline constexpr Version kRuntimeVersion{OFF_VERSION_MAJOR, OFF_VERSION_MINOR, OFF_VERSION_PATCH};

/// On-disk store format version. Bumped only for incompatible layouts.
inline constexpr std::uint16_t kStoreFormatVersion = 1;

/// Wire protocol version for the service transport surface.
inline constexpr std::uint16_t kProtocolVersion = 1;

/// Canonical export schema version for machine-readable snapshots.
inline constexpr std::uint16_t kCanonicalSchemaVersion = 1;

[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace off
