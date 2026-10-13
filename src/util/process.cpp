// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/process.h"

#include <windows.h>

#include <format>
#include <string>

#include "util/win_strings.h"

namespace knobs {

Result<HANDLE> StartProcessToWait(const std::filesystem::path& exe, std::wstring_view arguments,
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
  return process.hProcess;
}

Status StartProcess(const std::filesystem::path& exe, std::wstring_view arguments,
                    const std::filesystem::path& folder) {
  auto process = StartProcessToWait(exe, arguments, folder);
  if (!process) return Error{process.error()};
  CloseHandle(*process);
  return Ok{};
}

}  // namespace knobs
