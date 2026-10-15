// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

#include "core/core.h"
#include "test_harness.h"
#include "tray/fake_backend.h"
#include "tray/tray_app.h"
#include "util/files_in_use.h"
#include "util/process.h"

namespace {

using namespace knobs;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

fs::path SelfPath() {
  wchar_t path[32768] = {};
  GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  return path;
}

void Note(const fs::path& file, std::string_view text) {
  std::ofstream(file, std::ios::app) << text << '\n';
}

bool WaitForFile(const fs::path& file) {
  const auto due = std::chrono::steady_clock::now() + 5s;
  while (!fs::exists(file) && std::chrono::steady_clock::now() < due) Sleep(5);
  return fs::exists(file);
}

class ShutdownBackend : public tools::FakeBackend {
 public:
  ShutdownBackend(std::shared_ptr<tools::SharedWorld> world, std::wstring mode, const fs::path& folder,
                  std::promise<void>& entered)
      : FakeBackend(std::move(world), [](std::string_view) {}), mode_(std::move(mode)), folder_(folder),
        entered_(entered) {}

  ~ShutdownBackend() override {
    if (mode_ == L"destroy" || mode_.starts_with(L"restart")) Sleep(INFINITE);
    if (mode_ == L"thread" || mode_.starts_with(L"end-thread")) {
      const HMODULE dll = LoadLibraryW((SelfPath().parent_path() / L"knobs-test-stuck-thread.dll").c_str());
      const auto block = dll ? reinterpret_cast<void (*)()>(GetProcAddress(dll, "BlockThisThreadOnExit")) : nullptr;
      if (block) block();
    }
    Note(folder_ / L"clean", "destroyed");
  }

  Result<core::MicImport> ImportMic(const import::ActiveObsConfig& config, std::string_view pick) override {
    if (mode_ == L"busy" || mode_ == L"hold-busy" || mode_.starts_with(L"async") || mode_.starts_with(L"end-busy")) {
      entered_.set_value();
      Sleep(INFINITE);
    }
    return FakeBackend::ImportMic(config, pick);
  }

  Status StartChain(const core::ChainPlan& plan) override {
    auto result = FakeBackend::StartChain(plan);
    entered_.set_value();
    return result;
  }

  void StopChain() override {
    if (mode_.starts_with(L"end-clean")) {
      Note(folder_ / L"stopping", "stopping");
      WaitForFile(folder_ / L"finish-stop");
    }
    if (mode_ == L"stop" || mode_ == L"hold-stop") {
      Note(folder_ / L"stopping", "stopping");
      Sleep(INFINITE);
    }
    FakeBackend::StopChain();
  }

 private:
  std::wstring mode_;
  fs::path folder_;
  std::promise<void>& entered_;
};

TEST(TrayQuitBoundsAStuckCoreAndCleansUpANormalOne) {
  CHECK(tray::TrayOptions{}.core_stop_wait == 10s);
  for (const std::wstring mode : {L"clean", L"busy", L"stop", L"destroy", L"thread"}) {
    const fs::path folder = fs::temp_directory_path() /
                            std::format(L"knobs-tests-{}-quit-{}", GetCurrentProcessId(), mode);
    fs::create_directories(folder);
    auto child = StartProcessToWait(SelfPath(), std::format(L"--tray-shutdown {} \"{}\"", mode, folder.native()), folder);
    CHECK(child.ok());
    if (!child) continue;
    const DWORD ended = WaitForSingleObject(*child, 5000);
    CHECK(ended == WAIT_OBJECT_0);
    if (ended != WAIT_OBJECT_0) {
      TerminateProcess(*child, 1);
      WaitForSingleObject(*child, 5000);
    }
    DWORD code = 1;
    CHECK(GetExitCodeProcess(*child, &code));
    CHECK(code == (mode == L"clean" ? 0u : ERROR_TIMEOUT));
    CloseHandle(*child);
    std::ifstream log(folder / L"log");
    const std::string text(std::istreambuf_iterator<char>{log}, {});
    log.close();
    if (mode == L"clean") {
      CHECK(text.empty() && fs::exists(folder / L"clean"));
    } else {
      CHECK(text.find("Shutdown didn't finish before its deadline. Ending the process to release its files.") != std::string::npos);
      CHECK(text.find("icon removed") != std::string::npos);
    }
    CHECK(fs::exists(folder / L"closing"));
    CHECK(fs::exists(folder / L"icon-removed"));
    fs::remove_all(folder);
  }
}

TEST(TrayForcedQuitPreservesTheInstallersHandoff) {
  for (const std::wstring mode : {L"restart", L"restart-budget", L"restart-late", L"restart-late-ui"}) {
    const fs::path folder = fs::temp_directory_path() / std::format(L"knobs-tests-{}-quit-{}", GetCurrentProcessId(), mode);
    fs::create_directories(folder);
    fs::path dependency;
    for (const auto& entry : fs::recursive_directory_iterator(SelfPath().parent_path().parent_path() / L"test-dep")) {
      if (entry.path().filename() == L"knobs-test-dep.dll") dependency = entry.path();
    }
    CHECK(!dependency.empty());
    if (dependency.empty()) return;
    const fs::path file = folder / L"loaded.dll";
    fs::copy_file(dependency, file, fs::copy_options::overwrite_existing);
    auto child = StartProcessToWait(SelfPath(), std::format(L"--tray-shutdown {} \"{}\"", mode, folder.native()), folder);
    CHECK(child.ok());
    if (!child) return;
    CHECK(WaitForFile(folder / L"ready"));
    auto writer = std::async(std::launch::async, [&] {
      const HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
      const DWORD error = out == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
      if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
      return error;
    });
    const DWORD ended = WaitForSingleObject(*child, 10000);
    CHECK(ended == WAIT_OBJECT_0);
    if (ended != WAIT_OBJECT_0) {
      TerminateProcess(*child, 1);
      WaitForSingleObject(*child, 5000);
    }
    DWORD code = 1;
    CHECK(GetExitCodeProcess(*child, &code) && code == ERROR_TIMEOUT);
    CloseHandle(*child);
    CHECK(WaitForFile(folder / L"released"));
    CHECK(writer.get() == ERROR_SUCCESS);
    CHECK(fs::exists(folder / L"icon-removed"));
    fs::remove_all(folder);
  }
}

TEST(FileWatchdogWarnsAndReleasesWhileTheCoreIsStuck) {
  for (const std::wstring mode : {L"hold-busy", L"hold-stop"}) {
    const fs::path folder = fs::temp_directory_path() /
        std::format(L"knobs-tests-{}-{}", GetCurrentProcessId(), mode);
    fs::create_directories(folder);
    fs::path dependency;
    for (const auto& entry : fs::recursive_directory_iterator(SelfPath().parent_path().parent_path() / L"test-dep")) {
      if (entry.path().filename() == L"knobs-test-dep.dll") dependency = entry.path();
    }
    CHECK(!dependency.empty());
    if (dependency.empty()) return;
    const fs::path file = folder / L"loaded.dll";
    fs::copy_file(dependency, file, fs::copy_options::overwrite_existing);
    auto child = StartProcessToWait(SelfPath(), std::format(L"--tray-shutdown {} \"{}\"", mode, folder.native()), folder);
    CHECK(child.ok());
    if (!child) continue;
    CHECK(WaitForFile(folder / (mode == L"hold-stop" ? L"stopping" : L"ready")));
    auto writer = std::async(std::launch::async, [&] {
      const HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
      const DWORD error = out == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
      if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
      return error;
    });
    CHECK(WaitForFile(folder / L"waiting"));
    CHECK(writer.wait_for(5s) == std::future_status::ready);
    CHECK(WaitForFile(folder / L"released"));
    CHECK(fs::exists(folder / L"balloon"));
    CHECK(WaitForSingleObject(*child, 0) == WAIT_TIMEOUT);  // No Quit was needed to release the installer.
    const HWND window = FindWindowW(std::format(L"knobs-tests-{}-shutdown", GetProcessId(*child)).c_str(), nullptr);
    CHECK(window != nullptr);
    PostMessageW(window, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(*child, 5000) != WAIT_OBJECT_0) {
      CHECK(false);
      TerminateProcess(*child, 1);
      WaitForSingleObject(*child, 5000);
    }
    CloseHandle(*child);
    CHECK(writer.get() == ERROR_SHARING_VIOLATION);
    CHECK(fs::exists(folder / L"icon-removed"));
    fs::remove_all(folder);
  }
}

TEST(TrayQuitKeepsRespondingAndItsDeadlineSurvivesBlockedUiAndLogging) {
  for (const std::wstring mode : {L"async", L"async-ui", L"async-log", L"async-icon"}) {
    const fs::path folder = fs::temp_directory_path() /
        std::format(L"knobs-tests-{}-{}", GetCurrentProcessId(), mode);
    fs::create_directories(folder);
    auto child = StartProcessToWait(SelfPath(), std::format(L"--tray-shutdown {} \"{}\"", mode, folder.native()), folder);
    CHECK(child.ok());
    if (!child) continue;
    CHECK(WaitForFile(folder / L"quitting"));  // Quit returned with the core still blocked.
    const HWND window = FindWindowW(std::format(L"knobs-tests-{}-shutdown", GetProcessId(*child)).c_str(), nullptr);
    if (mode == L"async") {
      DWORD_PTR result = 1;
      CHECK(SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result));
      // Repeated requests and queued work must not postpone the first deadline.
      const auto due = std::chrono::steady_clock::now() + 5s;
      while (WaitForSingleObject(*child, 25) == WAIT_TIMEOUT && std::chrono::steady_clock::now() < due) {
        PostMessageW(window, WM_CLOSE, 0, 0);
      }
      CHECK(WaitForSingleObject(*child, 0) == WAIT_OBJECT_0);
    }
    const DWORD ended = WaitForSingleObject(*child, 5000);
    CHECK(ended == WAIT_OBJECT_0);
    if (ended != WAIT_OBJECT_0) {
      TerminateProcess(*child, 1);
      WaitForSingleObject(*child, 5000);
    }
    DWORD code = 0;
    CHECK(GetExitCodeProcess(*child, &code) && code == ERROR_TIMEOUT);
    CloseHandle(*child);
    CHECK(!fs::exists(folder / L"late-work"));
    std::ifstream input(folder / L"log");
    const std::string log(std::istreambuf_iterator<char>{input}, {});
    input.close();
    if (mode != L"async-icon") {
      CHECK(log.find("Shutdown didn't finish before its deadline") != std::string::npos);
      CHECK(log.find("icon removed") != std::string::npos);
    }
    CHECK(fs::exists(folder / L"icon-removed"));
    fs::remove_all(folder);
  }
}

TEST(EndSessionWaitsForTheCoreEvenAfterAnAsynchronousQuit) {
  std::vector<std::wstring> modes{L"end-clean-prior", L"end-busy-prior", L"end-thread-0"};
  for (const DWORD flag : {DWORD{0}, static_cast<DWORD>(ENDSESSION_LOGOFF), static_cast<DWORD>(ENDSESSION_CLOSEAPP)}) {
    modes.push_back(std::format(L"end-clean-{}", flag));
    modes.push_back(std::format(L"end-busy-{}", flag));
  }
  for (const auto& mode : modes) {
    const bool clean = mode.starts_with(L"end-clean");
    const fs::path folder = fs::temp_directory_path() /
        std::format(L"knobs-tests-{}-{}", GetCurrentProcessId(), mode);
    fs::create_directories(folder);
    auto child = StartProcessToWait(SelfPath(), std::format(L"--tray-shutdown {} \"{}\"", mode, folder.native()), folder);
    CHECK(child.ok());
    if (!child) continue;
    if (clean) {
      CHECK(WaitForFile(folder / L"stopping"));
      CHECK(!fs::exists(folder / L"session-returned"));
      CHECK(WaitForSingleObject(*child, 0) == WAIT_TIMEOUT);
      Note(folder / L"finish-stop", "finish");
    }
    const DWORD ended = WaitForSingleObject(*child, 5000);
    CHECK(ended == WAIT_OBJECT_0);
    if (ended != WAIT_OBJECT_0) {
      TerminateProcess(*child, 1);
      WaitForSingleObject(*child, 5000);
    }
    DWORD code = 1;
    CHECK(GetExitCodeProcess(*child, &code) && code == (clean ? 0u : ERROR_TIMEOUT));
    CloseHandle(*child);
    CHECK(fs::exists(folder / L"icon-removed"));
    CHECK(fs::exists(folder / L"session-returned") == clean);
    if (clean) {
      std::ifstream input(folder / L"session-returned");
      std::string result;
      std::getline(input, result);
      CHECK(result == "core cleaned up");
    }
    fs::remove_all(folder);
  }
}

TEST(TrayKeepsRespondingWhileAnUpdateJobStops) {
  const fs::path folder = fs::temp_directory_path() / std::format(L"knobs-tests-{}-quit-update", GetCurrentProcessId());
  fs::create_directories(folder);
  auto child = StartProcessToWait(SelfPath(), std::format(L"--tray-shutdown update \"{}\"", folder.native()), folder);
  CHECK(child.ok());
  if (!child) return;
  CHECK(WaitForFile(folder / L"update"));
  const HWND window = FindWindowW(std::format(L"knobs-tests-{}-shutdown", GetProcessId(*child)).c_str(), nullptr);
  PostMessageW(window, WM_CLOSE, 0, 0);
  CHECK(WaitForFile(folder / L"clean"));
  DWORD_PTR result = 1;
  CHECK(SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result));
  const DWORD ended = WaitForSingleObject(*child, 5000);
  CHECK(ended == WAIT_OBJECT_0);
  if (ended != WAIT_OBJECT_0) {
    TerminateProcess(*child, 1);
    WaitForSingleObject(*child, 5000);
  }
  DWORD code = 1;
  CHECK(GetExitCodeProcess(*child, &code) && code == 0);
  CloseHandle(*child);
  CHECK(fs::exists(folder / L"icon-removed"));
  fs::remove_all(folder);
}

}  // namespace

int knobs::test::RunShutdownHelper(const wchar_t* mode_arg, const wchar_t* folder_arg) {
  const std::wstring mode = mode_arg;
  const fs::path folder = folder_arg;
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
  std::unique_ptr<FileOplock> watch;
  if (mode.starts_with(L"restart") || mode.starts_with(L"hold-")) {
    const fs::path file = folder / L"loaded.dll";
    if (!LoadLibraryW(file.c_str())) return 2;
    auto taken = mode == L"restart" ? FileOplock::Take(file) : FileOplock::Take(file, 3s, 100ms);
    if (!taken) return 3;
    watch = std::move(*taken);
  }
  auto world = std::make_shared<tools::SharedWorld>(tools::DefaultWorld());
  world->Change([](tools::FakeWorld& value) {
    value.obs_later = value.obs;
    value.obs_later->install.version = {32, 2, 3};
  });
  std::promise<void> entered;
  auto ready = entered.get_future();
  core::Core* core = nullptr;
  HWND window = nullptr;
  std::atomic<bool> icon_added = false;
  tray::TrayOptions options;
  options.instance = GetModuleHandleW(nullptr);
  options.notify_icon = [&](DWORD message, NOTIFYICONDATAW& data) {
    if (message == NIM_ADD) icon_added = true;
    if (message == NIM_DELETE) {
      icon_added = false;
      Note(folder / L"icon-removed", "removed");
      if (mode == L"async-icon") Sleep(INFINITE);
    }
    if (message == NIM_MODIFY && (data.uFlags & NIF_TIP) && std::wstring_view(data.szTip).ends_with(L"Closing…")) {
      Note(folder / L"closing", "closing");
      if (mode == L"restart-late-ui") Sleep(INFINITE);
    }
    if (message == NIM_MODIFY && (data.uFlags & NIF_INFO) && data.szInfo[0]) {
      Note(folder / L"balloon", "shown");
    }
    return true;
  };
  options.window_class = std::format(L"knobs-tests-{}-shutdown", GetCurrentProcessId());
  options.core_stop_wait = mode.starts_with(L"async") || mode.starts_with(L"end-") ? 2s : 500ms;
  if (mode == L"clean") options.core_stop_wait = 2s;
  if (mode.starts_with(L"end-clean")) options.core_stop_wait = 10s;
  if (mode == L"restart-budget" || mode.starts_with(L"restart-late")) options.core_stop_wait = 6s;
  if (mode == L"update") {
    options.core_stop_wait = 2s;
    options.update_settings.check = true;
    options.updates.first_check = 0s;
    options.updates.current = {0, 3, 0};
    options.updates.folder = folder / L"updates";
    options.updates.latest = [folder](update::Cancel&) -> Result<update::Version> {
      Note(folder / L"update", "started");
      Sleep(INFINITE);  // A signature check can outlive cancellation too.
      return Error{"Stopped."};
    };
  }
  options.start_core = [&](core::Observer& observer, const core::Settings&, bool) {
    core::CoreOptions settings;
    settings.watch_devices = false;
    settings.obs_running = [] { return false; };
    auto started = core::Core::Start(std::make_unique<ShutdownBackend>(world, mode, folder, entered), observer, settings);
    if (started) core = started->get();
    return started;
  };
  options.log = [&](std::string_view line) {
    Note(folder / L"log", line);
    if (!icon_added) Note(folder / L"log", "icon removed");
    if (mode == L"async-log") Sleep(INFINITE);
  };
  options.set_start_with_windows = [&](bool) -> Status {
    Note(folder / L"late-work", "command");
    return Ok{};
  };
  options.on_snapshot = [&](const core::Snapshot& snapshot) {
    if (mode.starts_with(L"async")) Note(folder / L"late-work", "snapshot");
    if (mode.starts_with(L"restart") && snapshot.state == core::State::kRunning) core->Reimport();
  };
  options.on_notice = [&](const std::optional<tray::Notice>& notice) {
    if (notice && notice->kind == tray::Notice::Kind::kFileHold) {
      Note(folder / (notice->title == "An installer is waiting" ? L"waiting" : L"released"), notice->text);
    }
  };
  options.restart = [&](core::RestartNeed, bool, const core::RestartFiles&) -> Status {
    const auto due = std::chrono::steady_clock::now() + 5s;
    while (!watch->wanted() && std::chrono::steady_clock::now() < due) Sleep(5);
    if (!watch->wanted()) return Error{"The scratch installer did not start."};
    if (auto prepared = watch->PrepareHandoff(); !prepared) return prepared;
    if (mode.starts_with(L"restart-late")) {
      PostMessageW(window, WM_CLOSE, 0, 0);
      if (!WaitForFile(folder / L"closing")) return Error{"Quit did not start."};
    }
    return StartWithFileOplocks(SelfPath(), std::format(L"--file-update-helper \"{}\"", (folder / L"released").native()),
                                {watch->handle()});
  };
  auto app = tray::TrayApp::Create(std::move(options));
  if (!app) return 4;
  window = (*app)->window();
  if (ready.wait_for(3s) != std::future_status::ready) return 5;
  if (mode.starts_with(L"async")) {
    (*app)->Quit();
    Note(folder / L"quitting", "returned");
    (*app)->Execute(tray::kIdStartWithWindows);
    (*app)->Execute(tray::kIdPauseForObs);
    (*app)->OnSnapshot(core::Snapshot{.state = core::State::kNeedsSetup});
    if (mode == L"async-ui") Sleep(INFINITE);
    (*app)->Run();
  } else if (mode.starts_with(L"end-")) {
    const bool prior = mode.ends_with(L"-prior");
    const LPARAM flags = prior ? 0 : static_cast<LPARAM>(std::stoull(mode.substr(mode.rfind(L'-') + 1)));
    if (prior) {
      (*app)->Quit();
    }
    SendMessageW(window, WM_QUERYENDSESSION, 0, flags);
    SendMessageW(window, WM_ENDSESSION, TRUE, flags);
    Note(folder / L"session-returned", fs::exists(folder / L"clean") ? "core cleaned up" : "returned too soon");
  } else if (mode.starts_with(L"restart") || mode == L"update" || mode.starts_with(L"hold-")) {
    Note(folder / L"ready", "ready");
    if (mode == L"hold-stop") core->Pause();
    (*app)->Run();
  } else {
    (*app)->Quit();
    if (mode == L"clean") (*app)->Run();
  }
  app->reset();
  if (mode == L"clean") Sleep(2200);  // Normal completion disarms the 2 s watchdog.
  CoUninitialize();
  return 0;
}
