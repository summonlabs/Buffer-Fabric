#pragma once

#include <cstdint>

// Buffer Fabric version and format constants.
//
// BUFFER_FABRIC_VERSION_* is the product version.
// BUFFER_FABRIC_ABI_VERSION is bumped whenever the public C++ surface changes
// in a source-incompatible way.
// BUFFER_FABRIC_PERSISTENCE_FORMAT_VERSION is bumped whenever the on-disk
// snapshot or journal encoding changes. Readers refuse unknown versions.

#define BUFFER_FABRIC_VERSION_MAJOR 1
#define BUFFER_FABRIC_VERSION_MINOR 0
#define BUFFER_FABRIC_VERSION_PATCH 0
#define BUFFER_FABRIC_VERSION_STRING "1.0.0"

#define BUFFER_FABRIC_ABI_VERSION 1u

// Snapshot container format.
#define BUFFER_FABRIC_PERSISTENCE_FORMAT_VERSION 1u
// Append-only journal record format.
#define BUFFER_FABRIC_JOURNAL_FORMAT_VERSION 1u
// Framed transport wire format.
#define BUFFER_FABRIC_WIRE_FORMAT_VERSION 1u

namespace buffer_fabric {

/// Product version as a packed integer (major * 10000 + minor * 100 + patch).
inline constexpr std::uint32_t kVersion =
    BUFFER_FABRIC_VERSION_MAJOR * 10000u + BUFFER_FABRIC_VERSION_MINOR * 100u +
    BUFFER_FABRIC_VERSION_PATCH;

inline constexpr const char* kVersionString = BUFFER_FABRIC_VERSION_STRING;

}  // namespace buffer_fabric
