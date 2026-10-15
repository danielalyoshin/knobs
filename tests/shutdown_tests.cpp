// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <shellapi.h>

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
    if (mode_ == L"destroy" || mode_ == L"restart") Sleep(INFINITE);
    if (mode_ == L"thread") {
      const HMODULE dll = LoadLibraryW((SelfPath().parent_path() / L"knobs-test-stuck-thread.dll").c_str());
      const auto block = dll ? reinterpret_cast<void (*)()>(GetProcAddress(dll, "BlockThisThreadOnExit")) : nullptr;
      if (block) block();
    }
    Note(folder_ / L"clean", "destroyed");
  }

  Result<core::MicImport> ImportMic(const import::ActiveObsConfig& config, std::string_view pick) override {
    if (mode_ == L"busy" || mode_ == L"hold-busy") {
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
      CHECK(text.find("The core did not stop within 0.1 s. Ending the process to release its files.") != std::string::npos);
      CHECK(text.find("icon removed") != std::string::npos);
    }
    fs::remove_all(folder);
  }
}

TEST(TrayForcedQuitPreservesTheInstallersHandoff) {
  const fs::path folder = fs::temp_directory_path() / std::format(L"knobs-tests-{}-quit-handoff", GetCurrentProcessId());
  fs::create_directories(folder);
  fs::path dependency;
  for (const auto& entry : fs::recursive_directory_iterator(SelfPath().parent_path().parent_path() / L"test-dep")) {
    if (entry.path().filename() == L"knobs-test-dep.dll") dependency = entry.path();
  }
  CHECK(!dependency.empty());
  if (dependency.empty()) return;
  const fs::path file = folder / L"loaded.dll";
  fs::copy_file(dependency, file, fs::copy_options::overwrite_existing);
  auto child = StartProcessToWait(SelfPath(), std::format(L"--tray-shutdown restart \"{}\"", folder.native()), folder);
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
  fs::remove_all(folder);
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
    const bool released = writer.wait_for(3s) == std::future_status::ready;
    CHECK(released);
    CHECK(WaitForFile(folder / L"waiting"));
    CHECK(WaitForFile(folder / L"released"));
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
    fs::remove_all(folder);
  }
}

}  // namespace

int knobs::test::RunShutdownHelper(const wchar_t* mode_arg, const wchar_t* folder_arg) {
  const std::wstring mode = mode_arg;
  const fs::path folder = folder_arg;
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
  std::unique_ptr<FileOplock> watch;
  if (mode == L"restart" || mode.starts_with(L"hold-")) {
    const fs::path file = folder / L"loaded.dll";
    if (!LoadLibraryW(file.c_str())) return 2;
    auto taken = mode == L"restart" ? FileOplock::Take(file) : FileOplock::Take(file, 500ms, 50ms);
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
  tray::TrayOptions options;
  options.instance = GetModuleHandleW(nullptr);
  options.window_class = std::format(L"knobs-tests-{}-shutdown", GetCurrentProcessId());
  options.core_stop_wait = 100ms;
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
    NOTIFYICONDATAW icon = {sizeof(icon)};
    icon.hWnd = window;
    icon.uID = 1;
    icon.uFlags = NIF_TIP;
    if (!Shell_NotifyIconW(NIM_MODIFY, &icon)) Note(folder / L"log", "icon removed");
  };
  options.on_snapshot = [&](const core::Snapshot& snapshot) {
    if (mode == L"restart" && snapshot.state == core::State::kRunning) core->Reimport();
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
    return StartWithFileOplocks(SelfPath(), std::format(L"--file-update-helper \"{}\"", (folder / L"released").native()),
                                {watch->handle()});
  };
  auto app = tray::TrayApp::Create(std::move(options));
  if (!app) return 4;
  window = (*app)->window();
  if (ready.wait_for(3s) != std::future_status::ready) return 5;
  if (mode == L"restart" || mode.starts_with(L"hold-")) {
    Note(folder / L"ready", "ready");
    if (mode == L"hold-stop") core->Pause();
    (*app)->Run();
  } else {
    (*app)->Quit();
  }
  app->reset();
  CoUninitialize();
  return 0;
}
