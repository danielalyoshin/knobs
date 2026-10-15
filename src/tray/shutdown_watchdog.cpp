// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/shutdown_watchdog.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <latch>
#include <system_error>
#include <thread>
#include <utility>

#include "util/win_strings.h"

namespace knobs::tray {

struct ShutdownWatchdog::State {
  static constexpr uint64_t kDisarmed = UINT64_MAX;
  static constexpr uint64_t kExpired = UINT64_MAX - 1;
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE expired = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::atomic<uint64_t> due = 0;
  std::function<void()> before_exit;

  ~State() {
    if (stop) CloseHandle(stop);
    if (wake) CloseHandle(wake);
    if (expired) CloseHandle(expired);
  }

  void Wait() {
    const HANDLE events[] = {stop, wake};
    for (;;) {
      uint64_t deadline = due.load();
      if (deadline == kDisarmed) return;
      const uint64_t now = GetTickCount64();
      if (deadline && now >= deadline) {
        if (!due.compare_exchange_strong(deadline, kExpired)) continue;
        SetEvent(expired);
        // Cleanup has a small allowance, but never controls termination.
        WaitForSingleObject(stop, 100);
        TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
        return;
      }
      const DWORD delay = deadline ? static_cast<DWORD>(std::min<uint64_t>(deadline - now, INFINITE - 1)) : INFINITE;
      if (WaitForMultipleObjects(2, events, FALSE, delay) == WAIT_OBJECT_0) return;
    }
  }

  void Cleanup() {
    const HANDLE events[] = {stop, expired};
    if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return;
    before_exit();
    TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
  }
};

ShutdownWatchdog::ShutdownWatchdog(std::shared_ptr<State> state) : state_(std::move(state)) {}

Result<std::unique_ptr<ShutdownWatchdog>> ShutdownWatchdog::Start(std::function<void()> before_exit) {
  auto state = std::make_shared<State>();
  if (!state->stop || !state->wake || !state->expired) {
    return Error{std::format("Couldn't prepare the shutdown watchdog: {}", DescribeWinError(GetLastError()))};
  }
  state->before_exit = std::move(before_exit);
  auto ready = std::make_shared<std::latch>(2);
  // Each worker owns its state until it returns. Destruction never joins a
  // thread that a plugin could stall in DLL_THREAD_DETACH.
  try {
    std::thread([state, ready] { ready->count_down(); state->Wait(); }).detach();
    std::thread([state, ready] { ready->count_down(); state->Cleanup(); }).detach();
  } catch (const std::system_error& error) {
    state->due = State::kDisarmed;
    SetEvent(state->stop);
    return Error{std::format("Couldn't start the shutdown watchdog: {}", error.what())};
  }
  ready->wait();
  return std::unique_ptr<ShutdownWatchdog>(new ShutdownWatchdog(std::move(state)));
}

ShutdownWatchdog::~ShutdownWatchdog() {
  // Once expiry starts, keep the callback's captures alive until termination.
  while (!Disarm()) Sleep(1);
}

bool ShutdownWatchdog::Disarm() {
  uint64_t deadline = state_->due.load();
  do {
    if (deadline == State::kExpired) return false;
  } while (!state_->due.compare_exchange_weak(deadline, State::kDisarmed));
  SetEvent(state_->stop);
  return true;
}

void ShutdownWatchdog::Arm(std::chrono::milliseconds wait) {
  uint64_t unset = 0;
  if (state_->due.compare_exchange_strong(unset, GetTickCount64() + std::max<int64_t>(1, wait.count()))) {
    SetEvent(state_->wake);
  }
}

}  // namespace knobs::tray
