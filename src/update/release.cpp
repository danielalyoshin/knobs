// SPDX-License-Identifier: GPL-2.0-or-later
#include "update/release.h"

#include <charconv>
#include <format>

#include "app_info.h"
#include "util/win_strings.h"

namespace knobs::update {
namespace {

// A number in a version: digits only, with no leading zero unless it's 0.
std::optional<uint32_t> ParsePart(std::string_view text) {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) return std::nullopt;
  uint32_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

}  // namespace

std::string Version::ToString() const { return std::format("{}.{}.{}", major, minor, patch); }

std::optional<Version> ParseVersion(std::string_view text) {
  const size_t first = text.find('.');
  const size_t second = first == std::string_view::npos ? first : text.find('.', first + 1);
  if (second == std::string_view::npos) return std::nullopt;
  const auto major = ParsePart(text.substr(0, first));
  const auto minor = ParsePart(text.substr(first + 1, second - first - 1));
  const auto patch = ParsePart(text.substr(second + 1));
  if (!major || !minor || !patch) return std::nullopt;
  return Version{*major, *minor, *patch};
}

std::optional<Version> VersionFromReleaseUrl(std::string_view location) {
  // GitHub may give the owner and repository in another case than asked.
  const std::string prefix = std::format("{}/tag/v", kReleasesUrl);
  if (location.size() <= prefix.size() || AsciiLower(location.substr(0, prefix.size())) != AsciiLower(prefix)) {
    return std::nullopt;
  }
  return ParseVersion(location.substr(prefix.size()));
}

std::string ReleasePageUrl(const Version& version) {
  return std::format("{}/tag/v{}", kReleasesUrl, version.ToString());
}

std::string InstallerName(const Version& version) {
  return std::format("{}-{}-setup.exe", kDisplayName, version.ToString());
}

std::string InstallerUrl(const Version& version) {
  return std::format("{}/download/v{}/{}", kReleasesUrl, version.ToString(), InstallerName(version));
}

}  // namespace knobs::update
