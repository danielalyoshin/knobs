// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#include "util/result.h"

namespace knobs {

// A file check which may wait on Windows' network timeout. The owner only
// polls. The worker owns its inputs and result, never the owner or libobs,
// so abandoning a check and quitting don't wait for an unavailable drive.
// At most four checks of a given type run at once, including abandoned ones.
template <typename T>
class AsyncProbe {
 public:
  using Clock = std::chrono::steady_clock;
  explicit AsyncProbe(std::function<Result<T>()> work, Clock::duration timeout = std::chrono::seconds(2))
      : work_(std::move(work)), due_(Clock::now() + timeout) {}

  // A result stays valid until this probe is destroyed. Null means pending.
  const Result<T>* Poll() {
    if (expired_) return &*expired_;
    if (state_ && state_->done.load(std::memory_order_acquire)) return &*state_->result;
    if (Clock::now() >= due_) {
      expired_ = Error{"The file check took too long."};
      work_ = {};
      return &*expired_;
    }
    if (!state_) {
      unsigned active = active_.load();
      while (active < 4) {
        if (!active_.compare_exchange_weak(active, active + 1)) continue;
        state_ = std::make_shared<State>();
        try {
          std::thread([state = state_, work = std::move(work_)] {
            try {
              state->result = work();
            } catch (const std::exception& error) {
              state->result = Error{error.what()};
            } catch (...) {
              state->result = Error{"The file check failed."};
            }
            state->done.store(true, std::memory_order_release);
            --active_;
          }).detach();
        } catch (const std::exception& error) {
          --active_;
          expired_ = Error{error.what()};
          return &*expired_;
        }
        break;
      }
    }
    return nullptr;
  }

 private:
  struct State {
    std::optional<Result<T>> result;
    std::atomic<bool> done = false;
  };
  inline static std::atomic<unsigned> active_ = 0;
  std::function<Result<T>()> work_;
  Clock::time_point due_;
  std::shared_ptr<State> state_;
  std::optional<Result<T>> expired_;
};

}  // namespace knobs
