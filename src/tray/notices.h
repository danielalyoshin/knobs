// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/state.h"
#include "tray/badge.h"
#include "tray/first_run.h"

// The tray's notifications as data (docs/design.md, Notifications and badges):
// which changes of the core's state get one, what it says, and what a click on
// it opens. Each says what happened, then what knobs does or what to do. There
// are none for a normal start, or when OBS opens or closes without changing the
// mic. The icon's badge comes from here too, since Do Not Disturb can hide the
// notifications. tray_app.cpp shows them as Shell_NotifyIcon balloons. Text is
// UTF-8.
namespace knobs::tray {

struct Notice {
  enum class Kind {
    kMicMissing,      // For Notifier::Options::device_grace.
    kCableMissing,    // Likewise.
    kObsUnsupported,  // OBS was updated to a version knobs doesn't support.
    kObsMissing,
    kNeedsSetup,      // Something changed in OBS that only the user can settle.
    kFailed,
    kRestartNeeded,   // An SDK installer was released because a restart can't happen yet.
    kChainChanged,    // The mic or its chain changed in OBS.
    kFilterWarnings,  // New pre-flight warnings: filters knobs can't run.
    kRestarted,       // The copy before this one restarted to follow OBS.
    kOtherObs,        // OBS opened in another Windows session.
    kSoundIntoCable,  // Windows plays the PC's sound into a virtual cable.
    kUpdateAvailable,  // A new version is out (updates.h).
    kUpdated,          // This copy is the version an update installed.
    kUpdateFailed,     // An update didn't install by itself.
    kUpdateReady,      // One that installs by itself has waited: a click installs it.
  };

  Kind kind = Kind::kChainChanged;
  std::string title;  // What happened.
  std::string text;   // What knobs does, or what to do.
  // The cable gets no mic. Shown with the attention badge.
  bool problem = false;
  // Played with a sound: a problem, or a filter knobs can't run.
  bool sound = false;
  // What a click opens: the first run at this page, a web page in the
  // browser, or else the tray menu.
  std::optional<FirstRunPage> page;
  std::string url;

  friend bool operator==(const Notice&, const Notice&) = default;
};

// "obs-updated", "audio-changed": how a restart says why, on the new copy's
// command line.
std::string_view RestartNeedName(core::RestartNeed need);
std::optional<core::RestartNeed> RestartNeedNamed(std::string_view name);

// Follows the core's snapshots, and time, and says when to show or take down
// a notification. The rules:
// - Problems, the states where the cable gets no mic, get one as they begin:
//   a mic or cable missing once it has been for Options::device_grace, the
//   others at once. A restart that's needed gets none while the tray can
//   restart by itself; a delayed SDK restart says its installer was released.
//   While paused, nothing is missing.
// - A problem's notification is taken down when it ends.
// - The mic or its chain changing in OBS gets one after the first import,
//   and new pre-flight warnings do even at it, if Options::warned says which
//   are known. A change knobs's own settings made gets none.
// - After a restart to follow OBS, the new copy says why, and after an
//   update, what it did, after anything shown at the same time.
// - OBS opening in another Windows session, and Windows starting to play the
//   PC's sound into a virtual cable, get one, unless one of the above is
//   shown at the same time, and it's taken down when it ends.
// - Nothing shows while `quiet`: the setup window is open, and shows the
//   state itself. A problem that began meanwhile shows once it isn't quiet,
//   if it's still there, and so do what an update did and what Say was
//   given. News of the chain and its warnings doesn't: the window showed it.
class Notifier {
 public:
  using Clock = std::chrono::steady_clock;

  struct Options {
    // A mic or cable missing this long gets a notification, and the
    // attention badge. Shorter, and an audio interface's power cycle, or
    // Windows' audio service restarting, would get one.
    Clock::duration device_grace = std::chrono::seconds(5);
    // A mic that stalled is back once its chain has run this long. The
    // core's watchdog takes up to 4 s to see that a rebuilt chain stalled
    // again (core::Controller::Timing), so a mic that keeps stalling stays
    // missing. A mic whose device came back is back at once.
    Clock::duration mic_back_after = std::chrono::seconds(5);
    // Why the copy before this one restarted it, if it did.
    core::RestartNeed restarted_for = core::RestartNeed::kNone;
    // What an update did, said as the restart is, by the first copy that
    // runs after it (UpdatedNotice).
    std::optional<Notice> updated;
    // The pre-flight warnings already said, by what each is about, as
    // warnings() last had them, in this copy or one before: the first
    // import says the others. Unknown, it says none.
    std::optional<std::vector<std::string>> warned;
    // Notices held while quiet, or behind another, show one at a time, this
    // far apart.
    Clock::duration held_apart = std::chrono::seconds(8);
  };

  struct Update {
    std::optional<Notice> show;
    // Take down the notification showing: it no longer holds.
    bool hide = false;
  };

  Notifier() : Notifier(Options{}) {}
  explicit Notifier(Options options);

  // A new snapshot.
  Update Changed(const core::Snapshot& snapshot, bool quiet, Clock::time_point now);
  // Something to say that isn't about the core's state, such as a new
  // version (updates.h). While `quiet`, it waits.
  Update Say(Notice notice, bool quiet);
  // Does what's due by `now` (NextDeadline), and shows what Say held back
  // if it's no longer quiet.
  Update Tick(bool quiet, Clock::time_point now);
  std::optional<Clock::time_point> NextDeadline() const;

  Badge badge() const;
  // The notification last shown, until it's taken down or clicked: what a
  // click on it opens.
  const std::optional<Notice>& shown() const { return shown_; }
  // It was clicked, and Windows took it down.
  void Clicked() { shown_.reset(); }
  // The warnings said, for Options::warned, once an import has found the
  // mic.
  const std::optional<std::vector<std::string>>& warnings() const { return warned_; }

 private:
  // A mic or cable missing, from the first snapshot that said so.
  struct Missing {
    Clock::time_point since;
    // Past the grace: the icon says so.
    bool due = false;
    // The notification was shown, or isn't due while quiet.
    bool notified = false;
    // A mic only: its device is connected, and the chain stalled.
    bool stalled = false;
    // A stalled mic only: running again since, but not for long enough to
    // be back.
    std::optional<Clock::time_point> running_since;
  };

  // `stalled` is whether a missing mic's device is connected.
  void Follow(std::optional<Missing>& missing, bool now_missing, bool stalled, bool running,
              Clock::time_point now) const;
  // Shows what `missing` is due to say, once, when it isn't quiet.
  void Due(std::optional<Missing>& missing, Notice::Kind kind, bool quiet, Clock::time_point now, Update& update);
  // Shows the problem's notification, if it waits and it isn't quiet.
  void ShowProblem(bool quiet, Update& update);
  // Shows `notice` now, or after what's quiet or shown now.
  void ShowOrHold(Notice notice, bool quiet, Update& update);
  void Show(Notice notice, bool quiet, Update& update);
  // Takes down the notification showing if it's a `kind`.
  void Hide(Notice::Kind kind, Update& update);
  // Holds a notice while quiet: one of its kind replaces it.
  void Hold(Notice notice);
  // Shows the first of what was held, unless something else shows.
  void ShowHeld(bool quiet, Clock::time_point now, Update& update);

  Options options_;
  core::Snapshot snapshot_;
  bool started_ = false;
  std::optional<Missing> mic_;
  std::optional<Missing> cable_;
  // The problem other than a missing device, as its state and setup need,
  // and its notification while it waits for quiet to end.
  std::optional<std::pair<core::State, core::SetupNeed>> problem_;
  std::optional<Notice> problem_waiting_;
  // What the last import that found the mic found.
  uint32_t chain_revision_ = 0;
  // Its warnings, by what each is about (WarningKey), once known.
  std::optional<std::vector<std::string>> warned_;
  std::string mic_name_;
  std::vector<core::OtherObs> other_obs_;
  // The cable Windows plays the PC's sound into, if any.
  std::string sound_into_;
  runtime::ObsVersion obs_version_;
  std::optional<Notice> shown_;
  // What Say got while quiet, and the update's notice: the latest of each
  // kind, in turn.
  std::vector<Notice> held_;
  // When the next of held_ may show.
  std::optional<Clock::time_point> held_after_;
};

}  // namespace knobs::tray
