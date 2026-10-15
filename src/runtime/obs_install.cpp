// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/obs_install.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <format>
#include <vector>

#include "app_info.h"
#include "runtime/obs_layout.h"
#include "util/pick_folder.h"
#include "util/win_strings.h"

namespace knobs::runtime {
namespace {

namespace fs = std::filesystem;

Result<ObsVersion> ReadFileVersion(const fs::path& file) {
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
  if (size == 0) {
    return Error{std::format("Couldn't read the version of {}: {}", ToUtf8(file),
                             DescribeWinError(GetLastError()))};
  }
  std::vector<std::byte> data(size);
  VS_FIXEDFILEINFO* info = nullptr;
  UINT info_size = 0;
  if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data()) ||
      !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ||
      info_size < sizeof(*info)) {
    return Error{std::format("{} has no version information.", ToUtf8(file))};
  }
  return ObsVersion{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                    HIWORD(info->dwFileVersionLS)};
}

std::optional<fs::path> ReadRegistryPath(HKEY hive, const wchar_t* key, DWORD view_flag) {
  const DWORD flags = RRF_RT_REG_SZ | view_flag;
  DWORD bytes = 0;
  if (RegGetValueW(hive, key, nullptr, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegGetValueW(hive, key, nullptr, flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  value.resize(wcsnlen(value.c_str(), value.size()));
  if (value.empty()) return std::nullopt;
  return fs::path(value);
}

std::optional<fs::path> ProgramFilesDir() {
  wchar_t* raw = nullptr;
  std::optional<fs::path> path;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &raw))) {
    path = raw;
  }
  CoTaskMemFree(raw);
  return path;
}

}  // namespace

Result<ObsInstall> InspectObsInstall(const fs::path& folder) {
  std::error_code ec;
  fs::path root = fs::absolute(folder, ec).lexically_normal();
  if (!root.has_filename()) root = root.parent_path();  // Trailing separator.
  if (!fs::exists(ObsDll(root), ec) && fs::exists(root / L"obs.dll", ec)) {
    root = root.parent_path().parent_path();
  }

  auto missing = [&](const fs::path& path) {
    return Error{std::format("{} isn't a complete OBS Studio install: {} is missing.", ToUtf8(root),
                             ToUtf8(path.lexically_relative(root)))};
  };
  if (!fs::exists(ObsDll(root), ec)) return missing(ObsDll(root));
  auto version = ReadFileVersion(ObsDll(root));
  if (!version) return Error{version.error()};
  // Where the rest is depends on the version, and an OBS knobs doesn't
  // support may keep it somewhere knobs doesn't know. Its caller says it
  // isn't supported, which beats saying it's incomplete.
  if (!IsSupportedObsVersion(*version)) return ObsInstall{root, *version};

  std::vector<fs::path> required = {BinDir(root) / (fs::path(kGraphicsModule) += L".dll"), LibobsDataDir(root)};
  for (std::string_view module : kObsModules) {
    required.push_back(PluginDll(root, module, *version));
    required.push_back(PluginDataDir(root, module, *version));
  }
  for (const fs::path& path : required) {
    if (!fs::exists(path, ec)) return missing(path);
  }
  return ObsInstall{root, *version};
}

std::vector<fs::path> ObsInstallCandidates() {
  // The OBS installer records its folder as the default value of this key.
  constexpr const wchar_t* kInstallerKey = L"SOFTWARE\\OBS Studio";
  std::vector<fs::path> found;
  const std::pair<HKEY, DWORD> registry_views[] = {
      {HKEY_LOCAL_MACHINE, RRF_SUBKEY_WOW6464KEY},
      {HKEY_LOCAL_MACHINE, RRF_SUBKEY_WOW6432KEY},
      {HKEY_CURRENT_USER, 0},
  };
  for (const auto& [hive, view] : registry_views) {
    if (auto path = ReadRegistryPath(hive, kInstallerKey, view)) found.push_back(*path);
  }
  if (auto program_files = ProgramFilesDir()) found.push_back(*program_files / L"obs-studio");

  std::vector<fs::path> candidates;
  std::vector<std::string> seen;
  for (const fs::path& path : found) {
    std::error_code ec;
    std::string key = AsciiLower(ToUtf8(path.lexically_normal()));
    if (!fs::is_directory(path, ec) || std::find(seen.begin(), seen.end(), key) != seen.end()) {
      continue;
    }
    seen.push_back(std::move(key));
    candidates.push_back(path);
  }
  return candidates;
}

Result<ObsInstall> FindObsInstall() { return FindObsInstall(ObsInstallCandidates()); }

Result<ObsInstall> FindObsInstall(const std::vector<fs::path>& candidates) {
  std::string failures;
  // An unsupported OBS passes InspectObsInstall without its files checked,
  // so it may be no more than a leftover obs.dll. A supported one found later
  // wins over it.
  std::optional<ObsInstall> unsupported;
  for (const fs::path& candidate : candidates) {
    auto install = InspectObsInstall(candidate);
    if (install && IsSupportedObsVersion(install->version)) return install;
    if (install) {
      if (!unsupported) unsupported = *install;
      continue;
    }
    failures += "\n  " + install.error();
  }
  if (unsupported) return *unsupported;
  // What was found, and why it can't be used, says more than advice: the
  // first run's page gives that.
  if (failures.empty()) {
    return Error{std::format("OBS Studio isn't where its installer puts it. {} needs {}.", kDisplayName,
                             DescribeSupportedObsVersions())};
  }
  return Error{"The OBS Studio where its installer puts it can't be used:" + failures};
}

std::optional<fs::path> PickObsInstallFolder(void* owner) {
  return PickFolder(owner, L"Choose your OBS Studio folder");
}

}  // namespace knobs::runtime
