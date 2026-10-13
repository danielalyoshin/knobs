// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <filesystem>
#include <string_view>

#include "runtime/obs_version.h"

// Folder layout shared by an OBS install and knobs's runtime copy of it. The
// copy mirrors the install because libobs finds its own data relative to
// bin\64bit, in "../../data/libobs/" (obs-windows.c).
namespace knobs::runtime {

// The modules knobs always loads.
inline constexpr std::array<std::string_view, 2> kObsModules = {"win-wasapi", "obs-filters"};

// Modules knobs loads only for a chain with their filter, since each brings
// what nothing else in knobs needs: obs-vst runs on Qt, and nv-filters starts
// NVIDIA's Audio Effects runtime on the GPU. An install may lack them.
struct OptionalObsModule {
  std::string_view name;
  // The filter it registers, by its saved "id".
  std::string_view filter_id;
};
inline constexpr std::array<OptionalObsModule, 2> kOptionalObsModules = {{
    {"obs-vst", "vst_filter"},
    {"nv-filters", "nvidia_audiofx_filter"},
}};

// obs-vst's "Open interface when active" setting. On, obs-vst opens the
// plugin's window as the filter loads, in a QWidget, and with no
// QApplication, Qt aborts the process. knobs always loads it off.
inline constexpr char kVstOpenWhenActive[] = "open_when_active_vst_settings";

// Graphics module for the dummy-video fallback (see VideoMode::kDummy).
inline constexpr std::string_view kGraphicsModule = "libobs-d3d11";

// OBS's program, in bin\64bit.
inline constexpr std::wstring_view kObsExe = L"obs64.exe";

inline std::filesystem::path BinDir(const std::filesystem::path& root) {
  return root / L"bin" / L"64bit";
}

// libobs, in bin\64bit.
inline constexpr std::wstring_view kObsDll = L"obs.dll";

inline std::filesystem::path ObsDll(const std::filesystem::path& root) {
  return BinDir(root) / kObsDll;
}

// From OBS 33.0, each module has a folder of its own, core\<module>, with its
// data in core\<module>\data. Before, the modules shared obs-plugins\64bit,
// with their data in data\obs-plugins\<module> (cmake/windows/defaults.cmake
// and helpers.cmake in obs-studio).
inline constexpr ObsVersion kFirstObsWithCoreModules{33, 0, 0};

inline bool HasCoreModules(const ObsVersion& version) { return version >= kFirstObsWithCoreModules; }

inline std::filesystem::path PluginBinDir(const std::filesystem::path& root, std::string_view module,
                                          const ObsVersion& version) {
  if (HasCoreModules(version)) return root / L"core" / std::filesystem::path(module);
  return root / L"obs-plugins" / L"64bit";
}

inline std::filesystem::path PluginDll(const std::filesystem::path& root, std::string_view module,
                                       const ObsVersion& version) {
  return PluginBinDir(root, module, version) / (std::filesystem::path(module) += L".dll");
}

inline std::filesystem::path LibobsDataDir(const std::filesystem::path& root) {
  return root / L"data" / L"libobs";
}

inline std::filesystem::path PluginDataDir(const std::filesystem::path& root, std::string_view module,
                                           const ObsVersion& version) {
  if (HasCoreModules(version)) return root / L"core" / std::filesystem::path(module) / L"data";
  return root / L"data" / L"obs-plugins" / std::filesystem::path(module);
}

}  // namespace knobs::runtime
