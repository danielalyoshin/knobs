// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs.exe: the tray app. One copy runs per Windows session. It shows its
// state in the tray and runs the always-on core (src/core) on the user's OBS.
// Until the first run is finished, it opens that too.
//
//   --startup   Started by Windows at sign-in (the Run entry). If a copy is
//               already running, quit without a word. Doesn't open the first
//               run.
//   --resume-setup
//               Started by Windows at sign-in after a restart the first run
//               or the installer asked for (a RunOnce entry): as --startup,
//               but the first run opens, even if it was finished since. If a
//               copy is already running, it opens it there.
//   --restart   Started by a copy that's quitting to restart: wait for it.
//   --restarted-for <reason>
//               Why: obs-updated or audio-changed. The new copy says so in a
//               notification.
//   --updated   Started by the installer of an update this copy's
//               predecessor started: as --restart, and say in a notification
//               whether it updated.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include "app_info.h"
#include "core/core.h"
#include "update/github.h"
#include "update/signature.h"
#include "version.h"
#include "core/obs_backend.h"
#include "runtime/obs_log.h"
#include "tray/autostart.h"
#include "tray/notices.h"
#include "tray/single_instance.h"
#include "tray/tray_app.h"
#include "util/app_dirs.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace std::chrono_literals;

// How long a restarted copy waits for the old one to shut libobs down.
constexpr std::chrono::milliseconds kRestartWait = 30s;

std::filesystem::path ExePath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) return {};
    if (length < path.size()) {
      path.resize(length);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

Status StartCopy(const std::filesystem::path& exe, std::wstring_view arguments) {
  std::wstring command = std::format(L"\"{}\" {}", exe.native(), arguments);
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  // In the exe's folder: libobs has moved this process's working directory
  // into its runtime copy.
  const std::wstring folder = exe.parent_path().native();
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup,
                      &process)) {
    return Error{std::format("Couldn't start {} again: {}", kDisplayName, DescribeWinError(GetLastError()))};
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return Ok{};
}

// Whether the installer installed the exe in `folder`: the folder its entry
// in Windows' installed apps gives (kInstallerAppId). Then this copy can
// update itself by running the next installer.
bool InstalledByInstaller(const std::filesystem::path& folder) {
  const std::wstring key =
      std::format(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{}_is1", kInstallerAppId);
  wchar_t location[MAX_PATH * 2] = {};
  DWORD size = sizeof(location);
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"InstallLocation", RRF_RT_REG_SZ, nullptr, location, &size) !=
      ERROR_SUCCESS) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::equivalent(folder, std::filesystem::path(location), ec);
}

// Starts an update's installer, with no window: it closes this copy, installs
// over it, and starts the new copy with --updated, or this one again if it
// fails (packaging/knobs.iss). Its log goes in knobs's log folder.
Status StartInstaller(const std::filesystem::path& installer, const std::filesystem::path& logs) {
  std::error_code ec;
  std::filesystem::create_directories(logs, ec);
  std::wstring command =
      std::format(L"\"{}\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /update=1 /LOG=\"{}\"", installer.native(),
                  (logs / L"update.txt").native());
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  const std::wstring folder = installer.parent_path().native();
  if (!CreateProcessW(installer.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(),
                      &startup, &process)) {
    return Error{std::format("Couldn't start {}: {}", ToUtf8(installer.filename()), DescribeWinError(GetLastError()))};
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return Ok{};
}

int Run(HINSTANCE instance, bool startup, bool resume_setup, bool restart, core::RestartNeed restarted_for,
        bool updated) {
  const std::wstring window_class = std::format(L"{}.tray", kDisplayNameW);
  auto lock = tray::SingleInstance::Acquire(window_class, restart ? kRestartWait : 0ms);
  if (!lock) {
    tray::ShowStartupError(lock.error());
    return 1;
  }
  if (!*lock) {
    if (resume_setup) {
      // The copy that has the lock may be the one the Run entry started at
      // the same sign-in, and not have its window yet: it opens the first
      // run, as this one would have.
      for (int tries = 0; tries < 40 && !tray::ActivateTray(window_class, true); ++tries) Sleep(250);
    } else if (!startup && !updated) {
      // Show where the running copy is, unless Windows or an installer
      // started this one.
      tray::ActivateTray(window_class);
    }
    return 0;
  }
  const auto dirs = GetAppDirs();
  if (!dirs) {
    tray::ShowStartupError(dirs.error());
    return 1;
  }
  const std::filesystem::path exe = ExePath();
  // knobs's own log, from the start: libobs's lines join it once OBS starts.
  // knobs runs without one if it can't be opened.
  std::shared_ptr<runtime::ObsLog> log;
  if (auto opened = runtime::OpenNewLog(dirs->Logs(), std::format(L"{} ", kDisplayNameW), 20)) {
    log = std::move(*opened);
    log->Write(LOG_INFO, std::format("{} {} started{}{}{}{}", kDisplayName, KNOBS_VERSION, startup ? ", at sign-in" : "",
                                     resume_setup ? ", after a restart setup asked for" : "",
                                     restart ? ", to restart" : "", updated ? ", after an update" : ""));
  }
  const tray::RunEntry run{std::wstring(kDisplayNameW), std::format(L"\"{}\" --startup", exe.native())};

  tray::TrayOptions options;
  options.instance = instance;
  options.window_class = window_class;
  options.settings_file = dirs->roaming / L"settings.ini";
  // An unfinished first run comes back, but not at sign-in. After a restart
  // it asked for, it comes back even if it was finished since.
  options.open_first_run = !startup;
  options.resume_first_run = resume_setup;
  options.log_folder = dirs->Logs();
  if (log) options.log = [log](std::string_view line) { log->Write(LOG_INFO, line); };
  options.starts_with_windows = [run] { return tray::StartsWithWindows(run); };
  options.set_start_with_windows = [run](bool on) { return tray::SetStartWithWindows(run, on); };
  options.restart = [exe](core::RestartNeed why) {
    if (why == core::RestartNeed::kNone) return StartCopy(exe, L"--restart");
    return StartCopy(exe, std::format(L"--restart --restarted-for {}", FromUtf8(tray::RestartNeedName(why))));
  };
  options.restarted_for = restarted_for;
  options.resume_after_restart = [exe](bool on) {
    return tray::RunOnceAtNextSignIn({.name = std::wstring(kDisplayNameW),
                                      .command = std::format(L"\"{}\" --resume-setup", exe.native()),
                                      .run_key = std::wstring(tray::kRunOnceKey)},
                                     on);
  };
  // The update check (docs/design.md, Updates): only once the user turns it
  // on, and updating by itself only in a copy the installer installed, not a
  // portable one.
  options.updates.latest = [](update::Cancel& cancel) { return update::FindLatestRelease(cancel); };
  options.updates.download = [](const update::Version& version, const std::filesystem::path& file,
                                update::Cancel& cancel) {
    return update::Download(update::InstallerUrl(version), file, update::kMaxInstallerBytes, cancel);
  };
  options.updates.verify = [](const std::filesystem::path& file) { return update::CheckReleaseSignature(file); };
  options.updates.install = [logs = dirs->Logs()](const std::filesystem::path& file) {
    return StartInstaller(file, logs);
  };
  options.updates.folder = dirs->local / L"updates";
  options.updates.current = {KNOBS_VERSION_MAJOR, KNOBS_VERSION_MINOR, KNOBS_VERSION_PATCH};
  options.updates.can_install = !dirs->portable && InstalledByInstaller(exe.parent_path());
  options.updated = updated;
  options.start_core = [log](core::Observer& observer, const core::Settings& settings) {
    core::ObsBackendOptions backend;
    backend.log = log;
    backend.log_prefix = std::format(L"{} ", kDisplayNameW);
    backend.prune_runtime = true;
    core::CoreOptions core_options;
    core_options.settings = settings;
    return core::Core::Start(std::make_unique<core::ObsBackend>(std::move(backend)), observer,
                             std::move(core_options));
  };
  auto app = tray::TrayApp::Create(std::move(options));
  if (!app) {
    tray::ShowStartupError(app.error());
    return 1;
  }
  return (*app)->Run();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // Plain LoadLibrary calls skip PATH and the working directory; see
  // ObsRuntime::Load.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  bool startup = false;
  bool resume_setup = false;
  bool restart = false;
  bool updated = false;
  core::RestartNeed restarted_for = core::RestartNeed::kNone;
  int argc = 0;
  if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      const std::wstring_view arg = argv[i];
      startup |= arg == L"--startup";
      resume_setup |= arg == L"--resume-setup";
      restart |= arg == L"--restart";
      updated |= arg == L"--updated";
      if (arg == L"--restarted-for" && i + 1 < argc) {
        restarted_for = tray::RestartNeedNamed(ToUtf8(argv[++i])).value_or(core::RestartNeed::kNone);
      }
    }
    LocalFree(argv);
  }
  // The tray's thread is a single-threaded apartment: the shell's dialogs
  // and ShellExecute need one.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(com)) {
    tray::ShowStartupError(std::format("Couldn't initialize COM (0x{:08X}).", static_cast<uint32_t>(com)));
    return 1;
  }
  // An update's new copy waits for the old one, as a restart's does.
  const int code = Run(instance, startup, resume_setup, restart || updated, restarted_for, updated);
  CoUninitialize();
  return code;
}
