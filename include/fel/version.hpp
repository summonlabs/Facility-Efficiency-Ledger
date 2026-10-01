#pragma once

#include <cstdint>
#include <string_view>

// Version of the FacilityEfficiencyLedger library and of the on-disk formats it
// publishes. Format versions are part of the durability contract: a reader
// refuses data whose format version it does not implement.

namespace fel {

inline constexpr std::uint32_t kLibraryVersionMajor = 1;
inline constexpr std::uint32_t kLibraryVersionMinor = 0;
inline constexpr std::uint32_t kLibraryVersionPatch = 3;

// On-disk journal record format.
inline constexpr std::uint16_t kJournalFormatVersion = 1;

// On-disk manifest format.
inline constexpr std::uint16_t kManifestFormatVersion = 1;

// On-disk snapshot format.
inline constexpr std::uint16_t kSnapshotFormatVersion = 1;

// Stable machine identity string for this runtime.
inline constexpr std::string_view kRuntimeIdentity = "facility-efficiency-ledger";

std::string_view library_version_string() noexcept;
std::string_view library_commit_string() noexcept;

}  // namespace fel
