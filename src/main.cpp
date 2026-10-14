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
//               or the installer asked for (a RunOnce entry), or by the
//               installer once it installed VB-Cable: the first run opens,
//               even if it was finished since, at the cable page then. If a
//               copy is already running, it opens it there.
//   --restart   Started by a copy that's quitting to restart: wait for it.
//   --restarted-for <reason>
//               Why: obs-updated, obs-changed, audio-changed or
//               nvidia-installed. The new copy says so in a notification.
//   --reopened  Started by Windows after an installer closed the copy before
//               to replace a file it had open (Restart Manager): as
//               --restart, but if a copy is already running, quit without a
//               word.
//   --paused    The copy that restarted it, started the update, or was
//               closed by an installer, was paused: start paused.
//   --updated   Started by the installer of an update this copy's
//               predecessor started: as --restart. Whether it updated is
//               said by whichever copy runs first after it.

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
#include "util/process.h"
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
  // In the exe's folder: libobs has moved this process's working directory
  // into its runtime copy.
  if (const Status started = StartProcess(exe, arguments, exe.parent_path()); !started) {
    return Error{std::format("Couldn't start {} again: {}", kDisplayName, started.error())};
  }
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
// over it, and starts the new copy with --updated, and --paused if `paused`,
// or this one again if it fails (packaging/knobs.iss). Its log goes in knobs's
// log folder. Returns its process, to wait on.
Result<HANDLE> StartInstaller(const std::filesystem::path& installer, const std::filesystem::path& logs, bool paused) {
  std::error_code ec;
  std::filesystem::create_directories(logs, ec);
  const std::wstring arguments =
      std::format(L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /update=1{} /LOG=\"{}\"", paused ? L" /paused=1" : L"",
                  (logs / L"update.txt").native());
  auto started = StartProcessToWait(installer, arguments, installer.parent_path());
  if (!started) return Error{std::format("Couldn't start {}: {}", ToUtf8(installer.filename()), started.error())};
  return *started;
}

// Windows 11, whose Sound settings word things otherwise than Windows 10's:
// build 22000 or later, as RtlGetVersion says, which compatibility settings
// don't change.
bool IsWindows11() {
  using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto get = ntdll ? reinterpret_cast<RtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
  OSVERSIONINFOW version = {sizeof(version)};
  return !get || get(&version) != 0 || version.dwBuildNumber >= 22000;
}

int Run(HINSTANCE instance, bool startup, bool resume_setup, bool restart, core::RestartNeed restarted_for,
        bool paused, bool updated, bool reopened) {
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
    } else if (!startup && !updated && !reopened) {
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
    log->Write(LOG_INFO, std::format("{} {} started{}{}{}{}{}{}", kDisplayName, KNOBS_VERSION,
                                     startup ? ", at sign-in" : "", resume_setup ? ", to carry on with setup" : "",
                                     restart ? ", to restart" : "", updated ? ", after an update" : "",
                                     reopened ? ", after an installer closed it" : "", paused ? ", paused" : ""));
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
  options.start_paused = paused;
  options.windows11 = IsWindows11();
  options.log_folder = dirs->Logs();
  if (log) options.log = [log](std::string_view line) { log->Write(LOG_INFO, line); };
  options.starts_with_windows = [run] { return tray::StartsWithWindows(run); };
  options.set_start_with_windows = [run](bool on) { return tray::SetStartWithWindows(run, on); };
  options.restart = [exe](core::RestartNeed why, bool paused) {
    std::wstring arguments = L"--restart";
    if (why != core::RestartNeed::kNone) {
      arguments += std::format(L" --restarted-for {}", FromUtf8(tray::RestartNeedName(why)));
    }
    if (paused) arguments += L" --paused";
    return StartCopy(exe, arguments);
  };
  options.reopen_after_installer = [](bool on, bool paused) -> Status {
    // Not after a crash or a hang, nor once Windows restarts for an update:
    // the Run entry starts knobs at sign-in, if the user wants that.
    const HRESULT result = on ? RegisterApplicationRestart(paused ? L"--reopened --paused" : L"--reopened",
                                                           RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT)
                              : UnregisterApplicationRestart();
    if (FAILED(result)) {
      return Error{std::format("Couldn't {} Windows to start {} again after an installer closes it (0x{:08X}).",
                               on ? "ask" : "stop asking", kDisplayName, static_cast<uint32_t>(result))};
    }
    return Ok{};
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
  options.updates.install = [logs = dirs->Logs()](const std::filesystem::path& file, bool paused) {
    return StartInstaller(file, logs, paused);
  };
  options.updates.folder = dirs->local / L"updates";
  options.updates.current = {KNOBS_VERSION_MAJOR, KNOBS_VERSION_MINOR, KNOBS_VERSION_PATCH};
  options.updates.can_install = !dirs->portable && InstalledByInstaller(exe.parent_path());
  options.start_core = [log](core::Observer& observer, const core::Settings& settings, bool paused) {
    core::ObsBackendOptions backend;
    backend.log = log;
    backend.log_prefix = std::format(L"{} ", kDisplayNameW);
    backend.prune_runtime = true;
    core::CoreOptions core_options;
    core_options.settings = settings;
    core_options.paused = paused;
    return core::Core::Start(std::make_unique<core::ObsBackend>(std::move(backend)), observer,
                             std::move(core_options));
  };
  auto app = tray::TrayApp::Create(std::move(options));
  if (!app) {
    tray::ShowStartupError(app.error());
    return 1;
  }
  if (const Status ran = (*app)->Run(); !ran) {
    tray::ShowStartupError(ran.error());
    return 1;
  }
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // Plain LoadLibrary calls skip PATH and the working directory; see
  // ObsRuntime::Load.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  bool startup = false;
  bool resume_setup = false;
  bool restart = false;
  bool paused = false;
  bool updated = false;
  bool reopened = false;
  core::RestartNeed restarted_for = core::RestartNeed::kNone;
  int argc = 0;
  if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      const std::wstring_view arg = argv[i];
      startup |= arg == L"--startup";
      resume_setup |= arg == L"--resume-setup";
      restart |= arg == L"--restart";
      paused |= arg == L"--paused";
      updated |= arg == L"--updated";
      reopened |= arg == L"--reopened";
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
  // An update's new copy waits for the old one, as a restart's does, and so
  // does one Windows reopens.
  const int code =
      Run(instance, startup, resume_setup, restart || updated || reopened, restarted_for, paused, updated, reopened);
  CoUninitialize();
  return code;
}
