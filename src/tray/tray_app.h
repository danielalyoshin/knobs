// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "core/core.h"
#include "core/state.h"
#include "tray/badge.h"
#include "tray/first_run.h"
#include "tray/setup_window.h"
#include "tray/menu.h"
#include "tray/menu_theme.h"
#include "tray/notices.h"
#include "tray/updates.h"
#include "update/github.h"
#include "update/release.h"
#include "update/signature.h"
#include "util/result.h"

// The tray icon and its menu (docs/design.md, The tray app). It runs the core
// and shows its state: the icon's tooltip and the menu's first line are the
// status line, the icon's badge says whether knobs is paused or needs the user,
// and the menu offers the fix for whatever needs the user. Notifications
// (Notifier) say what changed, and a click on one opens its fix.
namespace knobs::tray {

// What the update check runs (docs/design.md, Updates), so that knobs-tray
// can fake it. Without `latest`, knobs has no update check.
struct UpdateHooks {
  // Asks GitHub for the latest release. On a thread of its own, as is
  // download, and `cancel` stops either.
  std::function<Result<update::Version>(update::Cancel& cancel)> latest;
  // Downloads `version`'s installer into `file`.
  std::function<Status(const update::Version& version, const std::filesystem::path& file, update::Cancel& cancel)>
      download;
  // Checks an installer's signature (update::CheckReleaseSignature): once
  // it's downloaded, and again before it runs. On the same thread.
  std::function<update::SignatureCheck(const std::filesystem::path& file)> verify;
  // Starts an installer that updates this copy: it closes this copy and
  // starts the new one with --updated.
  std::function<Status(const std::filesystem::path& file)> install;
  // Where installers are downloaded to.
  std::filesystem::path folder;
  update::Version current;  // This copy's version.
  // How long after knobs starts the first check waits (Updater::Options).
  std::chrono::system_clock::duration first_check = std::chrono::minutes(1);
  // The installer installed this copy (Updater::Options::can_install).
  bool can_install = false;
};

struct TrayOptions {
  HINSTANCE instance = nullptr;  // Holds the icon (IDI_KNOBS).
  // Names the hidden window's class, so another copy can find it
  // (ActivateTray).
  std::wstring window_class;
  // Starts the core, which reports to `observer`. A null core leaves the
  // tray starting for good (knobs-tray does that to show the state).
  std::function<Result<std::unique_ptr<core::Core>>(core::Observer& observer, const core::Settings& settings)>
      start_core;
  // Where the settings are kept. Empty: `settings` and `first_run` are used
  // and nothing is saved.
  std::filesystem::path settings_file;
  core::Settings settings;
  FirstRunProgress first_run;
  UpdateSettings update_settings;
  // Opens the first run once the tray runs, unless it was finished before.
  bool open_first_run = false;
  // Opens it even if it was: Windows started this copy after a restart the
  // first run asked for (resume_after_restart).
  bool resume_first_run = false;
  // Start with Windows.
  std::function<bool()> starts_with_windows;
  std::function<Status(bool on)> set_start_with_windows;
  // Has Windows open knobs at the next sign-in, to carry on with the first
  // run after a restart it asked for, or not.
  std::function<Status(bool on)> resume_after_restart;
  // Starts a new copy, which waits for this one to quit, and says why. The
  // tray restarts by itself when the core needs it to follow OBS.
  std::function<Status(core::RestartNeed why)> restart;
  // Why the copy before this one restarted it, if it did.
  core::RestartNeed restarted_for = core::RestartNeed::kNone;
  UpdateHooks updates;
  // An update's installer started this copy: say whether it updated.
  bool updated = false;
  // Starts OBS from its install, for the first run. Default: OpenObs.
  std::function<Status(const std::filesystem::path& install_root)> open_obs;
  std::filesystem::path log_folder;
  MenuTheme theme = MenuTheme::kSystem;
  // The setup window's zoom in place of Windows' text size, and the moment
  // its walkthroughs hold still at, for pictures (knobs-tray's --zoom and
  // --walk-at).
  std::optional<float> setup_zoom;
  std::optional<float> setup_walk_at;
  // Hears each snapshot on the tray's thread, after the tray has.
  std::function<void(const core::Snapshot& snapshot)> on_snapshot;
  // Hears each notification as it's shown, and nullopt as one is taken
  // down, on the tray's thread.
  std::function<void(const std::optional<Notice>& notice)> on_notice;
  // Hears the icon's badge change.
  std::function<void(Badge badge)> on_badge;
};

class TrayApp final : public core::Observer, private FirstRunHost {
 public:
  // Creates the hidden window and the icon, and starts the core. Call from
  // a thread that is a COM single-threaded apartment and pumps messages
  // (Run).
  static Result<std::unique_ptr<TrayApp>> Create(TrayOptions options);
  ~TrayApp() override;
  TrayApp(const TrayApp&) = delete;
  TrayApp& operator=(const TrayApp&) = delete;

  // Pumps messages until Quit. Returns the exit code.
  int Run();
  // Stops the core, removes the icon and ends Run.
  void Quit();

  // Opens the menu at `point`, in screen coordinates, as a click on the icon
  // does, and carries out what's picked. Returns once the menu has closed.
  void ShowMenu(POINT point);
  // Carries out a menu command as if it had been picked.
  void Execute(unsigned id);
  // Opens what the notification showing offers, as a click on it does.
  void OpenNotice();
  // Opens the first run, or brings it forward, and returns once it's
  // closed. Unfinished, it resumes where it stopped; finished, it starts
  // over if `from_start`, and otherwise shows only what the state needs.
  // `page` opens that page instead (the dev tools show any page with it).
  void ShowFirstRun(bool from_start, std::optional<FirstRunPage> page = std::nullopt);

  HWND window() const { return window_; }
  const core::Snapshot& snapshot() const override { return snapshot_; }
  const core::Settings& settings() const { return settings_; }
  const FirstRunProgress& first_run() const { return first_run_; }

  // core::Observer, on the core's thread.
  void OnSnapshot(const core::Snapshot& snapshot) override;

 private:
  explicit TrayApp(TrayOptions options);
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  // The icons with each badge, for the tray and for notifications.
  void LoadIcons();
  void DestroyIcons();
  void AddIcon();
  // Changes the icon as `data` says, with the tooltip.
  void ModifyIcon(NOTIFYICONDATAW& data);
  void UpdateTip();
  void UpdateBadge();
  // Shows or takes down a notification, then updates the badge and the
  // timer for what's due next.
  void Notify(const Notifier::Update& update);
  void ShowBalloon(const Notice& notice);
  void HideBalloon();
  // The first run shows the state while it's unfinished or open.
  bool Quiet() const;
  // Restarts to follow OBS, when the core needs it and nothing's open, or
  // once what's open closes. Returns whether this copy is quitting.
  bool RestartByItself();
  POINT IconPoint() const;
  void Execute(unsigned id, const Menu& menu);
  void Save();
  void ToggleStartWithWindows();
  void FindObs();
  void OpenLogFolder();
  void OpenUrl(const std::string& url);
  // A line for knobs's log.
  void Log(std::string line);
  // An update restarts knobs, so it waits until knobs isn't sending the mic
  // to the cable and the user hasn't paused it, which a new copy wouldn't
  // be, and nothing of knobs's is open.
  bool CanRestart() const;

  // The update check: does what's due, or sets the timer for when it is.
  void RunUpdates();
  // Clears out installers done with, and finds one downloaded before.
  void PrepareUpdates();
  // Starts a check, a download, or the check of an installer's signature
  // before it runs, on a thread of its own: FinishUpdateJob hears how it
  // went. `asked`: the user asked for the install, so it doesn't wait.
  void StartUpdateJob(Updater::Job job, bool asked = false);
  void FinishUpdateJob();
  void SetUpdates(bool check, bool install);
  // Update to the version offered, as the menu asked: now, or once the job
  // running is done.
  void InstallUpdate();
  // Stops the job running, and waits a moment for it.
  void StopUpdates();
  std::filesystem::path InstallerFile(const update::Version& version) const;
  UpdateMenu MenuUpdates() const;
  void ShowAbout();
  void ShowError(std::string_view text);

  // FirstRunHost.
  void ApplySettings(const core::Settings& settings) override;
  void Reimport() override;
  bool Restart() override;
  Status StartObs(const std::filesystem::path& install_root) override;
  void FinishFirstRun(const FinishChoices& finish) override;
  void SaveFirstRun(const FirstRunProgress& progress) override;
  void ResumeAfterRestart(bool on) override;

  // A window of ours that's open (a dialog), to bring forward rather than
  // open the menu over it.
  HWND OpenDialog() const;

  TrayOptions options_;
  HWND window_ = nullptr;
  // By Badge: small for the tray, and at notifications' size.
  std::array<HICON, 3> icons_ = {};
  std::array<HICON, 3> notification_icons_ = {};
  Badge badge_ = Badge::kNone;
  UINT taskbar_created_ = 0;
  bool icon_added_ = false;
  bool running_ = false;  // In Run.
  bool in_menu_ = false;
  bool quitting_ = false;
  core::Settings settings_;
  FirstRunProgress first_run_;
  core::Snapshot snapshot_;
  std::unique_ptr<core::Core> core_;
  SetupWindow* setup_window_ = nullptr;  // While it's open.
  Notifier notifier_;
  bool restart_tried_ = false;  // Since the core said it needs a restart.
  // The update check, if there's one, and the settings it was loaded with
  // otherwise.
  std::optional<Updater> updater_;
  UpdateSettings update_settings_;
  struct UpdateWork;
  std::shared_ptr<UpdateWork> update_work_;
  std::thread update_thread_;
  // The version the menu's update asked for while a job ran.
  std::optional<update::Version> install_asked_;
  // An error to show once the code that hit it is done (kErrorMessage).
  std::string later_error_;

  std::mutex pending_mutex_;  // Guards pending_.
  std::optional<core::Snapshot> pending_;
};

// Tells a running copy, through its window, that another copy was started:
// it opens its menu, so the user sees where knobs is, or its unfinished first
// run. `resume_setup`: Windows started it after a restart the first run asked
// for, so the first run opens even if it was finished. Returns false if no
// copy's window was found.
bool ActivateTray(const std::wstring& window_class, bool resume_setup = false);

// A dialog for an error before there's a tray to show it.
void ShowStartupError(std::string_view text);

}  // namespace knobs::tray
