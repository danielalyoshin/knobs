// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <optional>

#include "tray/notices.h"
#include "update/release.h"

// The update check as data (docs/design.md, Updates): when to ask GitHub for
// the latest release, when to download its installer and install it, what the
// menu offers, and what the notifications say. It's off until the user turns
// it on. It has no threads or clock of its own: tray_app.cpp runs what it asks
// for (src/update) on a thread of its own, and tells it how that went.
namespace knobs::tray {

// What the user chose, and what the check has found, kept in settings.ini.
struct UpdateSettings {
  // Automatically check for new versions.
  bool check = false;
  // Automatically update when a new version is available: only with
  // `check`, and only in a copy the installer installed.
  bool install = false;
  // When a check last got an answer.
  std::optional<std::chrono::system_clock::time_point> checked;
  // The latest release it found.
  std::optional<update::Version> latest;
  // The version an update last tried to install. An update doesn't try it
  // again by itself.
  std::optional<update::Version> tried;

  friend bool operator==(const UpdateSettings&, const UpdateSettings&) = default;
};

class Updater {
 public:
  using Clock = std::chrono::system_clock;

  struct Options {
    update::Version current;  // This copy's version.
    // The installer installed this copy, so it can run the next one. A
    // portable copy, or a build, only says where to get it.
    bool can_install = false;
    // A check at most this often.
    Clock::duration every = std::chrono::hours(24);
    // After a check or download that failed, as with no network.
    Clock::duration retry = std::chrono::hours(1);
    // After knobs starts: at sign-in, the network may not be up yet.
    Clock::duration first = std::chrono::minutes(1);
  };

  enum class Job {
    kNone,
    kCheck,     // Ask GitHub for the latest release.
    kDownload,  // Download latest()'s installer and check its signature.
    kInstall,   // Run the installer downloaded, which quits knobs.
  };

  // How a download went.
  enum class Downloaded {
    kDone,      // Downloaded, and signed as knobs's releases are.
    kFailed,    // It didn't download, as with no network: tried again later.
    kRejected,  // It downloaded, but isn't signed as knobs's releases are.
  };

  Updater(Options options, UpdateSettings settings, Clock::time_point started);

  const UpdateSettings& settings() const { return settings_; }
  bool can_install() const { return options_.can_install; }
  // The user turned the check, or updating by itself, on or off.
  void Set(bool check, bool install);

  // What's due at `now`. Installing restarts knobs, so it waits until
  // `can_restart`: knobs isn't sending the mic to the cable, and nothing of
  // its own is open.
  Job Due(Clock::time_point now, bool can_restart) const;
  // When something may be due next, unless a restart is all it waits for.
  std::optional<Clock::time_point> NextDeadline(Clock::time_point now) const;

  // The version a download or an install is for.
  const std::optional<update::Version>& latest() const { return settings_.latest; }
  // A check got `latest`, or no answer. Returns what to say about it.
  std::optional<Notice> Checked(Clock::time_point now, std::optional<update::Version> latest);
  // The download for latest(). Returns what to say about it.
  std::optional<Notice> DownloadDone(Clock::time_point now, Downloaded outcome);
  // An installer for latest() was downloaded before: it can be installed,
  // once its signature is checked again.
  void Found() { ready_ = settings_.latest; }
  // The one downloaded didn't pass a check again, or is gone: download it
  // again later.
  void Lost(Clock::time_point now);
  // The installer for latest() is starting, which quits knobs. The copy it
  // starts sees settings().tried.
  void Installing();

  // What the menu offers when a version newer than this one is out: to
  // install it now, once it's downloaded and installs by itself, else to
  // get it from its page.
  struct Offer {
    update::Version version;
    bool install = false;

    friend bool operator==(const Offer&, const Offer&) = default;
  };
  std::optional<Offer> offer() const;

 private:
  // Whether the latest release installs by itself: a newer version, not
  // tried before, with updating by itself on in a copy that can.
  bool Installs() const;
  Clock::time_point NextCheck() const;

  Options options_;
  UpdateSettings settings_;
  Clock::time_point started_;
  // The last check or download that failed.
  std::optional<Clock::time_point> failed_;
  // The version whose installer is downloaded and checked.
  std::optional<update::Version> ready_;
};

// What the copy an update's installer started says (main.cpp's --updated):
// that it's the version tried, or that the update didn't happen.
std::optional<Notice> UpdatedNotice(const UpdateSettings& settings, const update::Version& current);

}  // namespace knobs::tray
