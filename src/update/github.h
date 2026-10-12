// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "update/release.h"
#include "util/result.h"

// The update check's requests to GitHub (docs/design.md, Updates), over
// WinHTTP, which ships with Windows, through the system's proxy settings. They
// block: run them off the tray's thread, which can stop them with a Cancel.
namespace knobs::update {

// Stops a request from another thread: its WinHTTP handles are closed, which
// makes the call in flight fail at once.
class Cancel {
 public:
  // Stops the request in flight, and fails any made after.
  void Stop();
  bool stopped() const;

  // For the requests: a handle to close on Stop. False once stopped, and
  // then the caller closes it.
  bool Track(void* handle);
  // Whether the caller still has to close `handle`, which Stop hasn't.
  bool Untrack(void* handle);

 private:
  mutable std::mutex mutex_;
  bool stopped_ = false;
  std::vector<void*> handles_;
};

// Asks GitHub which release is the latest, by where kLatestReleaseUrl
// redirects to: one request, which follows no redirect, and reads only the
// status and the Location header. GitHub's latest release is never a draft
// or a pre-release.
Result<Version> FindLatestRelease(Cancel& cancel);

// The most an installer of knobs's can be: Download stops beyond it.
inline constexpr uint64_t kMaxInstallerBytes = 64 * 1024 * 1024;

// How a download went.
struct DownloadResult {
  enum class Outcome {
    kDone,
    // No network, an answer such as a server error, or it was stopped: it
    // may work later.
    kFailed,
    // GitHub doesn't have it, or it's bigger than an installer can be.
    kUnusable,
  };

  Outcome outcome = Outcome::kFailed;
  std::string error;  // Why it didn't download, as a sentence.

  bool ok() const { return outcome == Outcome::kDone; }
};

// Downloads `url` into `file`, following redirects, as long as it's no bigger
// than `max_bytes`. Only HTTPS.
DownloadResult Download(std::string_view url, const std::filesystem::path& file, uint64_t max_bytes, Cancel& cancel);

}  // namespace knobs::update
