// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/process.h"

#include <windows.h>

#include <format>
#include <string>

#include "util/win_strings.h"

namespace knobs {

Status StartProcess(const std::filesystem::path& exe, std::wstring_view arguments,
                    const std::filesystem::path& folder) {
  std::wstring command = std::format(L"\"{}\"", exe.native());
  if (!arguments.empty()) command += std::format(L" {}", arguments);
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup,
                      &process)) {
    return Error{DescribeWinError(GetLastError())};
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return Ok{};
}

}  // namespace knobs
