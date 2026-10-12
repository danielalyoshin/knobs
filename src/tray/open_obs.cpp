// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/open_obs.h"

#include <format>
#include <string>

#include "util/process.h"
#include "util/win_strings.h"

namespace knobs::tray {

Status OpenObs(const std::filesystem::path& install_root, std::wstring_view exe) {
  if (install_root.empty()) return Error{"OBS Studio wasn't found."};
  const std::filesystem::path bin = runtime::BinDir(install_root);
  const std::filesystem::path program = bin / exe;
  if (const Status started = StartProcess(program, L"", bin); !started) {
    return Error{std::format("Couldn't open OBS ({}): {}", ToUtf8(program), started.error())};
  }
  return Ok{};
}

}  // namespace knobs::tray
