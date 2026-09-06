#pragma once

// SLO Fabric version constants.
#define SLOFABRIC_VERSION_MAJOR 1
#define SLOFABRIC_VERSION_MINOR 0
#define SLOFABRIC_VERSION_PATCH 0
#define SLOFABRIC_VERSION_STRING "1.0.0"

namespace slofabric {
inline constexpr int kVersionMajor = SLOFABRIC_VERSION_MAJOR;
inline constexpr int kVersionMinor = SLOFABRIC_VERSION_MINOR;
inline constexpr int kVersionPatch = SLOFABRIC_VERSION_PATCH;
inline constexpr const char* kVersionString = SLOFABRIC_VERSION_STRING;

// Canonical library version symbols (defined in version.cpp).
const char* library_version() noexcept;
int library_version_major() noexcept;
int library_version_minor() noexcept;
int library_version_patch() noexcept;
}  // namespace slofabric
