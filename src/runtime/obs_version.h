// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace knobs::runtime {

struct ObsVersion {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t patch = 0;

  friend auto operator<=>(const ObsVersion&, const ObsVersion&) = default;

  // "32.2.2"
  std::string ToString() const;

  // Unpacks obs_get_version(): (major << 24) | (minor << 16) | patch.
  static ObsVersion FromLibobs(uint32_t packed);
};

// The vendored headers are 33.0's (third_party/libobs). Every export knobs
// requires has the same declaration back to 32.2.0, and the ones 33.0 adds
// are optional (KNOBS_OBS_API_OPTIONAL), so the floor stays at 32.2. Patch
// releases don't change the libobs API (semantic versioning, obs-config.h).
// The cap is the next major, where libobs makes breaking changes.
inline constexpr ObsVersion kMinSupportedObs{32, 2, 0};
inline constexpr ObsVersion kFirstUnsupportedObs{34, 0, 0};

bool IsSupportedObsVersion(const ObsVersion& version);

// An unsupported OBS that's newer than knobs supports, which a knobs update
// can support, rather than one that's too old.
inline bool IsNewerThanSupportedObs(const ObsVersion& version) { return version >= kFirstUnsupportedObs; }

// For messages, e.g. "OBS Studio 32.2 to 33.x".
std::string DescribeSupportedObsVersions();

// "OBS <version_text> isn't supported. knobs supports ..."
std::string UnsupportedObsMessage(std::string_view version_text);

}  // namespace knobs::runtime
