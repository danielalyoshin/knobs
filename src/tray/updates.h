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
  // `check`, and only in a copy the installer installed. It's kept while
  // either isn't so, and holds again once both are.
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
    // After a check, a download or an install that failed, as with no
    // network.
    Clock::duration retry = std::chrono::hours(1);
    // Before the first check or download after knobs starts: at sign-in,
    // the network may not be up yet. An install doesn't wait: knobs can
    // restart then, before it sends the mic.
    Clock::duration first = std::chrono::minutes(1);
    // A version whose download or install fails this many times in a way
    // trying again won't help, as one whose installer GitHub doesn't have,
    // or one changed after it downloaded, isn't tried again by itself: the
    // user hears, and can get it from its page. No network, or a signature
    // Windows can't finish checking, is only tried again later.
    int tries = 3;
    // An update that installs by itself and has waited this long, with no
    // moment to install, as for someone who never opens OBS, or without
    // downloading, as with no network, gets a notification.
    Clock::duration remind_after = std::chrono::hours(24);
  };

  enum class Job {
    kNone,
    kCheck,     // Ask GitHub for the latest release.
    kDownload,  // Download latest()'s installer and check its signature.
    kInstall,   // Run the installer downloaded, which quits knobs.
  };

  // How a download went.
  enum class Downloaded {
    kDone,  // Downloaded, and signed as knobs's releases are.
    // It didn't download, as with no network or a server error, or Windows
    // couldn't finish checking its signature: tried again later.
    kFailed,
    // GitHub doesn't have it, or it's too big to be an installer: tried
    // again later, up to Options::tries times.
    kUnusable,
    kRejected,  // It downloaded, but isn't signed as knobs's releases are.
  };

  Updater(Options options, UpdateSettings settings, Clock::time_point started);

  const UpdateSettings& settings() const { return settings_; }
  bool can_install() const { return options_.can_install; }
  // The user turned the check, or updating by itself, on or off. Each is
  // kept as it's given (UpdateSettings::install).
  void Set(bool check, bool install);

  // What's due at `now`. Installing restarts knobs, so it waits until
  // `can_restart`: knobs isn't sending the mic to the cable, and nothing of
  // its own is open.
  Job Due(Clock::time_point now, bool can_restart) const;
  // When something may be due next, unless a restart is all it waits for.
  std::optional<Clock::time_point> NextDeadline(Clock::time_point now) const;

  // The version a download or an install is for.
  const std::optional<update::Version>& latest() const { return settings_.latest; }
  // The version whose installer is worth keeping: newer than this copy, not
  // tried before, in a copy that can install it. Once it's downloaded, the
  // menu offers to install it, even with updating by itself off.
  std::optional<update::Version> installable() const;
  // latest()'s installer is downloaded, and its signature passed the check.
  bool ready() const { return ready_ && ready_ == settings_.latest; }

  // A check got `latest`, or no answer. Returns what to say about it.
  std::optional<Notice> Checked(Clock::time_point now, std::optional<update::Version> latest);
  // The download for latest(). Returns what to say about it.
  std::optional<Notice> DownloadDone(Clock::time_point now, Downloaded outcome);
  // An installer for latest() was downloaded before: it can be installed,
  // once its signature is checked again.
  void Found();
  // The installer for latest() didn't pass the check of its signature just
  // before it ran. `lost`: it was changed, or is gone, and is downloaded
  // again, which counts toward Options::tries; otherwise Windows couldn't
  // finish the check, which is tried again later. Returns what to say.
  std::optional<Notice> NotVerified(Clock::time_point now, bool lost);
  // The installer for latest() is starting, which quits knobs. The copy it
  // starts sees settings().tried.
  void Installing();
  // An update that installs by itself has waited Options::remind_after:
  // ready, with no moment to install, it says so, with a click that
  // installs it now; not downloaded yet, it says the version is out. Once a
  // version.
  std::optional<Notice> Waiting(Clock::time_point now);
  // The version Waiting last said so for.
  const std::optional<update::Version>& reminded() const { return reminded_; }

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
  // Whether the latest release installs by itself: installable(), with
  // updating by itself on.
  bool Installs() const;
  Clock::time_point NextCheck() const;
  Clock::time_point NextDownload() const;
  // A download or an install of latest() failed: it's tried again later,
  // and if `counts`, up to Options::tries times.
  std::optional<Notice> Failed(Clock::time_point now, bool counts);
  // latest() isn't tried again by itself.
  std::optional<Notice> GiveUp();

  Options options_;
  UpdateSettings settings_;
  Clock::time_point started_;
  // The last check that failed.
  std::optional<Clock::time_point> check_failed_;
  // The last download or install of latest() that failed, and how many have
  // in a row.
  std::optional<Clock::time_point> try_failed_;
  int failures_ = 0;
  // The version whose installer is downloaded and checked, since when, and
  // the one Waiting said so for.
  std::optional<update::Version> ready_;
  std::optional<Clock::time_point> ready_since_;
  std::optional<update::Version> reminded_;
  // When latest() was first known, this run.
  std::optional<Clock::time_point> latest_since_;
};

// What the copy an update's installer started says (main.cpp's --updated):
// that it's the version tried, or that the update didn't happen.
std::optional<Notice> UpdatedNotice(const UpdateSettings& settings, const update::Version& current);

}  // namespace knobs::tray
