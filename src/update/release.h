// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// knobs's releases on GitHub, which the update check follows (docs/design.md,
// Updates): their versions, and where each one's page and installer are.
namespace knobs::update {

struct Version {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t patch = 0;

  friend auto operator<=>(const Version&, const Version&) = default;

  // "0.3.0"
  std::string ToString() const;
};

// "0.3.0", as a release's tag has it after its "v", and nothing else: no
// suffix, sign, space or leading zero.
std::optional<Version> ParseVersion(std::string_view text);

// The version that kLatestReleaseUrl redirects to, from its Location header:
// "https://github.com/danielalyoshin/knobs/releases/tag/v0.3.0". Nullopt for
// any other address.
std::optional<Version> VersionFromReleaseUrl(std::string_view location);

// A release's page, which has its notes.
std::string ReleasePageUrl(const Version& version);
// Its installer, as packaging/package.ps1 names it and release.yml attaches
// it: "knobs-0.3.0-setup.exe".
std::string InstallerName(const Version& version);
std::string InstallerUrl(const Version& version);

}  // namespace knobs::update
