// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "util/result.h"

namespace knobs::tray {

// Started before plugins load. At expiry, `before_exit` removes the icon
// and logs why on a worker. A second worker ends the process even if that
// cleanup blocks, after at most another 100 ms.
class ShutdownWatchdog {
 public:
  static Result<std::unique_ptr<ShutdownWatchdog>> Start(std::function<void()> before_exit);
  ~ShutdownWatchdog();
  // The first call sets the deadline. Further calls cannot extend it.
  void Arm(std::chrono::milliseconds wait);
  // False once expiry has won the race with normal completion.
  bool Disarm();

 private:
  struct State;
  explicit ShutdownWatchdog(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

}  // namespace knobs::tray
