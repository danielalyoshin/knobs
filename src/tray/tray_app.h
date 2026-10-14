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
#include <vector>

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
  std::function<update::DownloadResult(const update::Version& version, const std::filesystem::path& file,
                                       update::Cancel& cancel)>
      download;
  // Checks an installer's signature (update::CheckReleaseSignature): once
  // it's downloaded, and again before it runs. On the same thread.
  std::function<update::SignatureCheck(const std::filesystem::path& file)> verify;
  // Starts an installer that updates this copy: it closes this copy and
  // starts the new one with --updated, and --paused if `paused`. Returns the
  // installer's process, which the tray waits on and closes: one that ends
  // before it has closed this copy didn't update it. Null: there's none to
  // wait on, and the tray quits.
  std::function<Result<HANDLE>(const std::filesystem::path& file, bool paused)> install;
  // Where installers are downloaded to.
  std::filesystem::path folder;
  update::Version current;  // This copy's version.
  // How long after knobs starts the first check waits, and how long an
  // update waits for a moment to install before knobs says so
  // (Updater::Options).
  std::chrono::system_clock::duration first_check = std::chrono::minutes(1);
  std::chrono::system_clock::duration remind_after = std::chrono::hours(24);
  // The installer installed this copy (Updater::Options::can_install).
  bool can_install = false;
};

struct TrayOptions {
  HINSTANCE instance = nullptr;  // Holds the icon (IDI_KNOBS).
  // Names the hidden window's class, so another copy can find it
  // (ActivateTray).
  std::wstring window_class;
  // Starts the core, which reports to `observer`, paused if `paused`. A null
  // core leaves the tray starting for good (knobs-tray does that to show the
  // state).
  std::function<Result<std::unique_ptr<core::Core>>(core::Observer& observer, const core::Settings& settings,
                                                    bool paused)>
      start_core;
  // The copy before this one was paused, and restarted: the core starts
  // paused.
  bool start_paused = false;
  // Where the settings are kept. Empty: `settings` and `first_run` are used
  // and nothing is saved.
  std::filesystem::path settings_file;
  core::Settings settings;
  FirstRunProgress first_run;
  UpdateSettings update_settings;
  // Opens the first run once the tray runs, unless it was finished before.
  bool open_first_run = false;
  // Opens it even if it was, at the cable page then: Windows started this
  // copy after a restart the first run or the installer asked for
  // (resume_after_restart), or the installer did, having installed VB-Cable.
  bool resume_first_run = false;
  // Windows 11, whose Sound settings use other words than Windows 10's.
  bool windows11 = true;
  // Start with Windows.
  std::function<bool()> starts_with_windows;
  std::function<Status(bool on)> set_start_with_windows;
  // Has Windows open knobs at the next sign-in, to carry on with the first
  // run after a restart it asked for, or not.
  std::function<Status(bool on)> resume_after_restart;
  // Starts a new copy, which waits for this one to quit, says why, and starts
  // paused if `paused`. The tray restarts by itself when the core needs it
  // to follow OBS.
  // Called on the core's thread, through Backend::Restart. `files` must be
  // inherited before returning success, then released by the new copy only
  // after this process exits (StartWithFileOplocks). Must return promptly.
  std::function<Status(core::RestartNeed why, bool paused, const std::vector<uintptr_t>& files)> restart;
  // Has Windows start knobs again, paused if `paused`, after an installer
  // closes it to replace a file it has open (Restart Manager), or not, if
  // `on` is false. The tray keeps it on, and in step with pausing, except
  // while Windows shuts down or signs out.
  std::function<Status(bool on, bool paused)> reopen_after_installer;
  // Why the copy before this one restarted it, if it did.
  core::RestartNeed restarted_for = core::RestartNeed::kNone;
  UpdateHooks updates;
  // Starts OBS from its install, for the first run. Default: OpenObs.
  std::function<Status(const std::filesystem::path& install_root)> open_obs;
  std::filesystem::path log_folder;
  // Writes a line to knobs's log. Default: the core's log, once it runs.
  std::function<void(std::string_view line)> log;
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

  // Pumps messages until Quit. An Error if knobs quit by itself because its
  // core couldn't start once an update at start had held it back: what
  // Create returns otherwise.
  Status Run();
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
  // The setup window shows the state while it's open, or about to open.
  bool Quiet() const;
  // Restarts to follow OBS, when the core needs it and nothing's open, or
  // once what's open closes, paused if the user paused this copy. Returns
  // whether this copy is quitting.
  bool RestartByItself();
  // A finished first run with no cable picked, as one from before knobs
  // asked for it: what OBS monitors to becomes the pick, so OBS monitoring
  // elsewhere later doesn't move it.
  void KeepCable();
  // Saves the warnings the notifications have said, if they changed.
  void SaveWarned();
  POINT IconPoint() const;
  void Execute(unsigned id, const Menu& menu);
  void Save();
  void ToggleStartWithWindows();
  void FindObs();
  void OpenLogFolder();
  void OpenUrl(const std::string& url);
  // A line for knobs's log.
  void Log(std::string line);
  // The user paused knobs, or the copy before this one, if this one restarted.
  bool Paused() const;
  // An update by itself restarts knobs, so it waits until knobs isn't
  // sending the mic to the cable, the user hasn't paused it, and nothing of
  // knobs's is open.
  bool CanRestart() const;
  // Turns on reopening after an installer (TrayOptions::reopen_after_installer),
  // paused or not as knobs is now, unless it's on that way already.
  void KeepReopening();
  // Starts the core, which sends the mic.
  Status StartCore();
  // Starts the core held back for an update at start (Create), once that's
  // decided, or has taken too long. On a failure, quits, and Run returns it.
  void ReleaseCore();
  // Whether the update check is built in: knobs-tray's tests may leave it
  // out (UpdateHooks).
  bool HasUpdateCheck() const { return static_cast<bool>(options_.updates.latest); }

  // The update check: does what's due, or sets the timer for when it is.
  void RunUpdates();
  // Clears out installers done with, and finds one downloaded before.
  void PrepareUpdates();
  // Removes every installer but the one for the version that's installable,
  // in a copy that installs updates. Returns whether that one is there.
  bool TidyUpdates();
  // Starts a check, a download, or the check of an installer's signature
  // before it runs, on a thread of its own: FinishUpdateJob hears how it
  // went. `asked`: the user asked for the install, so it doesn't wait.
  void StartUpdateJob(Updater::Job job, bool asked = false);
  void FinishUpdateJob();
  void SetUpdates(bool check, bool install);
  // Updates to `asked`, the version the menu or a notification offered: now,
  // or once the job running is done. If it's no longer offered, says why.
  void InstallUpdate(const update::Version& asked);
  void NotOffered(const update::Version& asked);
  // Stops the job running, and waits a moment for it.
  void StopUpdates();
  // The installer started for `version`: knobs runs on until it closes this
  // copy. One that ends first didn't update it, which InstallerEnded says.
  void WaitForInstaller(HANDLE process, const update::Version& version);
  void InstallerEnded(DWORD exit_code);
  void StopWaitingForInstaller();
  std::filesystem::path InstallerFile(const update::Version& version) const;
  // Where a download is kept whose signature Windows couldn't finish
  // checking, to check again without downloading it again.
  std::filesystem::path UnverifiedFile(const update::Version& version) const;
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
  // Why knobs quit by itself, for Run to return.
  std::string failed_;
  core::Settings settings_;
  FirstRunProgress first_run_;
  core::Snapshot snapshot_;
  std::unique_ptr<core::Core> core_;
  SetupWindow* setup_window_ = nullptr;  // While it's open.
  bool setup_due_ = false;               // Asked to open, by kFirstRunMessage.
  Notifier notifier_;
  // The warnings the notifications have said, as last saved.
  std::optional<std::vector<std::string>> warned_;
  bool restart_tried_ = false;  // Since the core said it needs a restart.
  // How Windows reopens knobs after an installer: paused or not, or not at
  // all (TrayOptions::reopen_after_installer).
  std::optional<bool> reopen_paused_;
  // The update check's plan, and its settings, which are kept even without
  // the check (HasUpdateCheck).
  Updater updater_;
  struct UpdateWork;
  std::shared_ptr<UpdateWork> update_work_;
  std::thread update_thread_;
  // The version the menu's update asked for while a job ran.
  std::optional<update::Version> install_asked_;
  // An error to show once the code that hit it is done (kErrorMessage).
  std::string later_error_;
  // The core waits for an update downloaded before to install, or not.
  bool holding_core_ = false;
  // An installer that's running, for the version, until it closes this copy
  // or ends: a thread waits on it, and on the event that stops the wait.
  update::Version installer_version_;
  HANDLE installer_ = nullptr;
  HANDLE installer_stop_ = nullptr;
  std::thread installer_thread_;

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
