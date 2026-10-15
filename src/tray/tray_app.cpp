// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/tray_app.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <semaphore>
#include <system_error>
#include <utility>
#include <vector>

#include "app_info.h"
#include "tray/message_dialog.h"
#include "tray/open_obs.h"
#include "tray/resource.h"
#include "tray/settings_file.h"
#include "util/win_strings.h"
#include "version.h"

namespace knobs::tray {
namespace {

constexpr UINT kIconId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;      // From the icon (NOTIFYICON_VERSION_4).
constexpr UINT kSnapshotMessage = WM_APP + 2;  // OnSnapshot left one in pending_.
// Another copy was started (ActivateTray): wParam 1 after a restart the
// first run asked for.
constexpr UINT kActivateMessage = WM_APP + 3;
// Open the unfinished first run, or (wParam 1) the first run after a restart
// it asked for.
constexpr UINT kFirstRunMessage = WM_APP + 4;
// The update check: a job is done (wParam 1), or it's time to look.
constexpr UINT kUpdateMessage = WM_APP + 5;
constexpr UINT kErrorMessage = WM_APP + 6;  // Show later_error_.
// The installer this copy started ended without closing it, with its exit
// code in wParam.
constexpr UINT kInstallerMessage = WM_APP + 7;
constexpr UINT kFileHoldMessage = WM_APP + 8;
constexpr UINT_PTR kNoticeTimer = 1;   // Notifier::NextDeadline.
constexpr UINT_PTR kRestartTimer = 2;  // RestartByItself, while a window is open.
constexpr UINT_PTR kUpdateTimer = 3;   // Updater::NextDeadline.
constexpr UINT_PTR kCoreTimer = 4;     // ReleaseCore, if an update at start takes too long.
constexpr UINT_PTR kQuitTimer = 5;     // FinishQuit, without blocking window messages.
// How long an update at start can hold back the mic: its installer's
// signature is checked again first, which may wait on servers.
constexpr UINT kCoreHoldMs = 5000;
// The longest the update timer waits: a computer that sleeps doesn't count
// the time asleep.
constexpr long long kUpdateTimerMaxMs = 60 * 60 * 1000;
// How long quitting waits for an update job. A request stops at once, but a
// signature check can't be stopped, and may wait a while on servers it can't
// reach: knobs quits without it.
constexpr std::chrono::seconds kUpdateStopWait{1};
constexpr UINT kRestartRetryMs = 1000;

using Clock = Notifier::Clock;

constexpr Badge kBadges[] = {Badge::kNone, Badge::kPaused, Badge::kAttention};

// Creates the native menu for `items`.
HMENU CreateNativeMenu(const std::vector<MenuItem>& items) {
  const HMENU menu = CreatePopupMenu();
  UINT position = 0;
  for (const MenuItem& item : items) {
    MENUITEMINFOW info = {sizeof(info)};
    std::wstring text = FromUtf8(item.text);
    if (item.kind == MenuItem::Kind::kSeparator) {
      info.fMask = MIIM_FTYPE;
      info.fType = MFT_SEPARATOR;
    } else {
      info.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
      info.fType = item.radio ? MFT_RADIOCHECK : MFT_STRING;
      info.fState = (item.enabled ? MFS_ENABLED : MFS_DISABLED) | (item.checked ? MFS_CHECKED : 0u) |
                    (item.bold ? MFS_DEFAULT : 0u);
      info.wID = item.id;
      info.dwTypeData = text.data();
      if (item.kind == MenuItem::Kind::kSubmenu) {
        info.fMask |= MIIM_SUBMENU;
        info.hSubMenu = CreateNativeMenu(item.items);
      }
    }
    InsertMenuItemW(menu, position++, TRUE, &info);
  }
  return menu;
}

// Copies `text` into a NOTIFYICONDATAW field, shortened with "…" to fit.
template <size_t N>
void CopyField(wchar_t (&field)[N], std::string_view text) {
  std::wstring wide = FromUtf8(text);
  if (wide.size() >= N) {
    wide.resize(N - 2);
    wide += L'…';
  }
  wide.copy(field, wide.size());
  field[wide.size()] = L'\0';
}

// When `file` was last written, by the system clock. From its FILETIME, not
// std::chrono's clock_cast, which goes through leap seconds and so ICU, which
// Windows 10 before 1903 doesn't have.
std::optional<std::chrono::system_clock::time_point> LastWritten(const std::filesystem::path& file) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) return std::nullopt;
  const uint64_t ticks =
      (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
  // FILETIME counts 100 ns from 1601, and the system clock from 1970.
  constexpr uint64_t kTicksTo1970 = 116'444'736'000'000'000;
  if (ticks < kTicksTo1970) return std::nullopt;
  using Ticks = std::chrono::duration<uint64_t, std::ratio<1, 10'000'000>>;
  return std::chrono::system_clock::time_point(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(Ticks(ticks - kTicksTo1970)));
}

Updater MakeUpdater(const UpdateHooks& hooks, const UpdateSettings& settings) {
  return Updater(Updater::Options{.current = hooks.current,
                                  .can_install = hooks.can_install,
                                  .first = hooks.first_check,
                                  .remind_after = hooks.remind_after},
                 settings, Updater::Clock::now());
}

}  // namespace

// What an update job found or did, between its thread and the tray's.
struct TrayApp::UpdateWork {
  Updater::Job job = Updater::Job::kNone;
  bool asked = false;
  // The version a download or an install is for.
  std::optional<update::Version> version;
  update::Cancel cancel;
  // Written by the job's thread before it posts kUpdateMessage.
  std::optional<update::Version> latest;
  Updater::Downloaded downloaded = Updater::Downloaded::kFailed;
  update::SignatureCheck verified;
  std::string error;
  // Released as the job's thread ends.
  std::binary_semaphore done{0};
};

Result<std::unique_ptr<TrayApp>> TrayApp::Create(TrayOptions options) {
  std::unique_ptr<TrayApp> app(new TrayApp(std::move(options)));
  const TrayOptions& opts = app->options_;
  std::string settings_error;
  if (!opts.settings_file.empty()) {
    if (auto loaded = LoadSettings(opts.settings_file)) {
      app->settings_ = std::move(loaded->core);
      app->first_run_ = loaded->first_run;
      app->updater_ = MakeUpdater(opts.updates, loaded->updates);
      app->warned_ = std::move(loaded->warned);
    } else {
      settings_error = loaded.error();
    }
  }
  if (app->HasUpdateCheck()) app->PrepareUpdates();
  // An installer was started, by this copy or the one before: whether it
  // installed is said by the first copy that runs after it, whichever that is.
  const std::optional<Notice> updated = UpdatedNotice(app->updater_.settings(), opts.updates.current);
  if (app->updater_.settings().installing) {
    app->updater_.Reported();
    app->Save();
  }
  app->notifier_ = Notifier({.restarted_for = opts.restarted_for, .handoff_failed = opts.handoff_failed, .updated = updated, .warned = app->warned_});

  WNDCLASSEXW window_class = {sizeof(window_class)};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = opts.instance;
  window_class.lpszClassName = opts.window_class.c_str();
  if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return Error{std::format("Couldn't register the tray's window class: {}", DescribeWinError(GetLastError()))};
  }
  // A top-level window, not a message-only one: only those hear
  // TaskbarCreated when Explorer restarts.
  if (!CreateWindowExW(0, opts.window_class.c_str(), std::wstring(kDisplayNameW).c_str(), WS_OVERLAPPED, 0, 0, 0, 0,
                       nullptr, nullptr, opts.instance, app.get())) {
    return Error{std::format("Couldn't create the tray's window: {}", DescribeWinError(GetLastError()))};
  }
  app->taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
  if (app->taskbar_created_) ChangeWindowMessageFilterEx(app->window_, app->taskbar_created_, MSGFLT_ALLOW, nullptr);
  app->LoadIcons();
  ApplyMenuTheme(opts.theme);
  app->AddIcon();
  WatchFileHolds(reinterpret_cast<uintptr_t>(app->window_), kFileHoldMessage);
  auto watchdog = ShutdownWatchdog::Start([window = app->window_, log = opts.log, notify_icon = opts.notify_icon] {
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = window;
    data.uID = kIconId;
    notify_icon(NIM_DELETE, data);
    constexpr char reason[] = "Shutdown didn't finish before its deadline. Ending the process to release its files.";
    if (log) log(reason);
    else OutputDebugStringA(reason);
  });
  if (!watchdog) return Error{watchdog.error()};
  app->shutdown_watchdog_ = std::move(*watchdog);

  // An update downloaded before installs now, before knobs sends the mic:
  // once knobs runs, it waits for a moment the mic isn't sent anyway.
  if (app->HasUpdateCheck() && app->updater_.Due(Updater::Clock::now(), app->CanRestart()) == Updater::Job::kInstall) {
    app->holding_core_ = true;
    app->StartUpdateJob(Updater::Job::kInstall);
    SetTimer(app->window_, kCoreTimer, kCoreHoldMs, nullptr);
  } else if (const Status started = app->StartCore(); !started) {
    return Error{started.error()};
  }
  app->KeepReopening();
  if (!settings_error.empty()) {
    app->ShowError(std::format("Couldn't read the settings, so {} uses its defaults until they're changed.\n\n{}",
                               kDisplayName, settings_error));
  }
  // Once Run pumps messages: the dialog's loop needs Run's to end with it.
  // Meanwhile, what it will show isn't notified.
  if (opts.resume_first_run || (opts.open_first_run && !app->first_run_.done)) {
    app->setup_due_ = true;
    PostMessageW(app->window_, kFirstRunMessage, opts.resume_first_run, 0);
  }
  if (app->HasUpdateCheck()) PostMessageW(app->window_, kUpdateMessage, 0, 0);
  return app;
}

TrayApp::TrayApp(TrayOptions options)
    : options_(std::move(options)),
      settings_(options_.settings),
      first_run_(options_.first_run),
      updater_(MakeUpdater(options_.updates, options_.update_settings)) {}

TrayApp::~TrayApp() {
  Quit();
  WaitForQuit();
  StopWatchingFileHolds(reinterpret_cast<uintptr_t>(window_));
  StopWaitingForInstaller();
  DestroyIcons();
}

Status TrayApp::Run() {
  if (quit_finished_) return failed_.empty() ? Status(Ok{}) : Status(Error{failed_});
  running_ = true;
  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (!failed_.empty()) return Error{failed_};
  return Ok{};
}

void TrayApp::Quit() {
  if (quitting_) return;
  quitting_ = true;
  if (shutdown_watchdog_) shutdown_watchdog_->Arm(options_.core_stop_wait);
  update_stop_due_ = Clock::now() + kUpdateStopWait;
  if (window_) SetTimer(window_, kQuitTimer, 20, nullptr);
  if (in_menu_) EndMenu();
  if (setup_window_) DestroyWindow(setup_window_->window());
  setup_due_ = false;
  if (core_) core_->RequestStop();
  if (installer_stop_) SetEvent(installer_stop_);
  Notify(notifier_.Quitting());
  UpdateTip();
  if (update_work_) update_work_->cancel.Stop();
  FinishQuit();
}

void TrayApp::FinishQuit() {
  if (!quitting_ || quit_finished_) return;
  if (core_ && !core_->stopped()) return;
  core_.reset();
  if (!StopUpdates()) return;
  if (installer_thread_.joinable() && WaitForSingleObject(installer_thread_.native_handle(), 0) != WAIT_OBJECT_0) return;
  StopWaitingForInstaller();
  KillTimer(window_, kQuitTimer);
  StopWatchingFileHolds(reinterpret_cast<uintptr_t>(window_));
  RemoveIcon();
  if (window_) DestroyWindow(window_);
  if (shutdown_watchdog_ && !shutdown_watchdog_->Disarm()) return;
  quit_finished_ = true;
  shutdown_watchdog_.reset();
}

void TrayApp::WaitForQuit() {
  // Windows can end the process as soon as WM_ENDSESSION returns. The
  // independent watchdog also runs during this deliberately synchronous wait.
  while (!quit_finished_) {
    FinishQuit();
    if (quit_finished_) break;
    FollowFileHolds();
    if (const auto due = notifier_.NextDeadline(); due && Clock::now() >= *due) {
      Notify(notifier_.Tick(Quiet(), Clock::now()));
    }
    Sleep(10);
  }
}

void TrayApp::RemoveIcon() {
  if (icon_added_) {
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = window_;
    data.uID = kIconId;
    options_.notify_icon(NIM_DELETE, data);
    icon_added_ = false;
  }
}

void TrayApp::OnSnapshot(const core::Snapshot& snapshot) {
  {
    std::lock_guard lock(pending_mutex_);
    pending_ = snapshot;
  }
  PostMessageW(window_, kSnapshotMessage, 0, 0);
}

LRESULT CALLBACK TrayApp::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto* app = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    app->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  auto* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!app) return DefWindowProcW(window, message, wparam, lparam);
  if (message == WM_NCDESTROY) {
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    app->window_ = nullptr;
    return DefWindowProcW(window, message, wparam, lparam);
  }
  return app->HandleMessage(message, wparam, lparam);
}

LRESULT TrayApp::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  if (taskbar_created_ && message == taskbar_created_) {
    if (quitting_) return 0;
    // Explorer restarted, and the icon went with it, or the taskbar's DPI
    // changed, which may leave it there at the old size.
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = window_;
    data.uID = kIconId;
    options_.notify_icon(NIM_DELETE, data);
    icon_added_ = false;
    LoadIcons();
    AddIcon();
    return 0;
  }
  switch (message) {
    case kFileHoldMessage:
      FollowFileHolds();
      return 0;
    case kTrayMessage:
      switch (LOWORD(lparam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_CONTEXTMENU:
          if (const HWND dialog = OpenDialog()) {
            SetForegroundWindow(dialog);
          } else {
            ShowMenu({GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)});
          }
          break;
        case NIN_BALLOONUSERCLICK:
          OpenNotice();
          break;
        default:
          break;
      }
      return 0;
    case kSnapshotMessage: {
      std::optional<core::Snapshot> snapshot;
      {
        std::lock_guard lock(pending_mutex_);
        snapshot.swap(pending_);
      }
      if (quitting_) return 0;
      if (snapshot) {
        const core::Snapshot before = std::exchange(snapshot_, std::move(*snapshot));
        UpdateTip();
        KeepReopening();
        Notify(notifier_.Changed(snapshot_, Quiet(), Clock::now()));
        if (RestartByItself()) return 0;
        if (setup_window_) setup_window_->Changed(before);
        SaveWarned();
        KeepCable();
        if (options_.on_snapshot) options_.on_snapshot(snapshot_);
        RunUpdates();
      }
      return 0;
    }
    case kUpdateMessage:
      if (quitting_) {
        FinishQuit();
        return 0;
      }
      if (wparam == 1) {
        FinishUpdateJob();
      } else {
        RunUpdates();
      }
      return 0;
    case WM_TIMER:
      if (wparam == kQuitTimer) {
        FinishQuit();
        return 0;
      }
      if (quitting_ && wparam != kNoticeTimer) return 0;
      if (wparam == kUpdateTimer) {
        KillTimer(window_, kUpdateTimer);
        RunUpdates();
        return 0;
      }
      if (wparam == kRestartTimer) {
        KillTimer(window_, kRestartTimer);
        if (restart_result_) FinishRestart();
        else RestartByItself();
        return 0;
      }
      if (wparam == kCoreTimer) {
        ReleaseCore();
        return 0;
      }
      if (wparam != kNoticeTimer) break;
      KillTimer(window_, kNoticeTimer);
      Notify(notifier_.Tick(Quiet(), Clock::now()));
      return 0;
    case kActivateMessage:
      if (quitting_) {
        ShowMenu(IconPoint());
        return 0;
      }
      if (const HWND dialog = OpenDialog()) {
        SetForegroundWindow(dialog);
      } else if (!first_run_.done || wparam) {
        // Started again before setup was finished, or after a restart it
        // asked for: that's what's wanted.
        ShowFirstRun(false, wparam && first_run_.done ? std::optional(FirstRunPage::kCable) : std::nullopt);
      } else {
        ShowMenu(IconPoint());
      }
      return 0;
    case kFirstRunMessage:
      // After a restart for VB-Cable, or once the installer installed it, a
      // finished first run opens at the cable, to choose it.
      setup_due_ = false;
      if (!first_run_.done || wparam) {
        ShowFirstRun(false, wparam && first_run_.done ? std::optional(FirstRunPage::kCable) : std::nullopt);
      } else {
        Notify(notifier_.Tick(Quiet(), Clock::now()));
      }
      return 0;
    case kErrorMessage:
      if (quitting_) return 0;
      if (!later_error_.empty()) ShowError(std::exchange(later_error_, {}));
      return 0;
    case kInstallerMessage:
      InstallerEnded(static_cast<DWORD>(wparam));
      return 0;
    case WM_SETTINGCHANGE:
      if (wparam == SPI_SETHIGHCONTRAST ||
          (lparam && std::wstring_view(reinterpret_cast<const wchar_t*>(lparam)) == L"ImmersiveColorSet")) {
        ApplyMenuTheme(options_.theme);
        // The tray's and the notifications' marks follow the taskbar, as the
        // menu does.
        LoadIcons();
        NOTIFYICONDATAW data = {sizeof(data)};
        data.uFlags = NIF_ICON;
        data.hIcon = icons_[static_cast<size_t>(badge_)];
        ModifyIcon(data);
      }
      break;
    case WM_QUERYENDSESSION:
      // An installer closing knobs to replace a file it has open asks with
      // ENDSESSION_CLOSEAPP, and Windows reopens knobs after it
      // (KeepReopening). A shutdown that restarts apps, as Windows' "restart
      // apps" setting has it do, asks the same way, but then the session is
      // ending. At a shutdown or sign-out, the Run entry starts knobs at the
      // next sign-in if the user wants that, and Windows shouldn't.
      if (!(lparam & ENDSESSION_CLOSEAPP) || (lparam & ENDSESSION_LOGOFF) || GetSystemMetrics(SM_SHUTTINGDOWN)) {
        session_ending_ = true;
        if (reopen_paused_ && options_.reopen_after_installer) {
          if (const Status off = options_.reopen_after_installer(false, false); !off) Log(off.error());
          reopen_paused_.reset();
        }
      }
      return TRUE;
    case WM_ENDSESSION:
      // The session or knobs ends, and Windows ends the process after this
      // returns: let go of the mic and the cable first.
      if (wparam) {
        Quit();
        WaitForQuit();
      } else {
        // It doesn't end after all: another program said no.
        session_ending_ = false;
        KeepReopening();
      }
      return 0;
    case WM_CLOSE:
      Quit();
      return 0;
    case WM_DESTROY:
      // Only Run's loop is waiting for it. Before that, a WM_QUIT would close
      // the next dialog instead.
      if (running_) PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

void TrayApp::LoadIcons() {
  DestroyIcons();
  // The one-colour mark, as Windows' own tray icons are: white on a dark
  // taskbar, black on a light one.
  const bool dark = TaskbarIsDark(options_.theme);
  HICON small_icon = nullptr;
  if (FAILED(LoadIconMetric(options_.instance, MAKEINTRESOURCEW(dark ? IDI_TRAY_WHITE : IDI_TRAY_BLACK), LIM_SMALL,
                            &small_icon))) {
    small_icon = nullptr;
  }
  // Windows 11 draws a notification's icon about 50 px wide at 100%, and
  // resamples whatever it's given. Twice that is sharpest: an icon that size
  // or a little bigger blurs, and a much bigger one comes out jagged.
  const int size = MulDiv(2 * 48, static_cast<int>(GetDpiForSystem()), 96);
  HICON notification = nullptr;
  // The colour mark for the notification's ground, which is the taskbar's.
  if (FAILED(LoadIconWithScaleDown(options_.instance, MAKEINTRESOURCEW(dark ? IDI_KNOBS_ON_DARK : IDI_KNOBS), size,
                                   size, &notification))) {
    notification = nullptr;
  }
  for (const Badge badge : kBadges) {
    icons_[static_cast<size_t>(badge)] = BadgedIcon(small_icon, badge, dark);
    notification_icons_[static_cast<size_t>(badge)] = BadgedIcon(notification, badge, dark);
  }
  if (small_icon) DestroyIcon(small_icon);
  if (notification) DestroyIcon(notification);
}

void TrayApp::DestroyIcons() {
  for (std::array<HICON, 3>* icons : {&icons_, &notification_icons_}) {
    for (HICON& icon : *icons) {
      if (icon) DestroyIcon(icon);
      icon = nullptr;
    }
  }
}

void TrayApp::AddIcon() {
  NOTIFYICONDATAW data = {sizeof(data)};
  data.hWnd = window_;
  data.uID = kIconId;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  data.uCallbackMessage = kTrayMessage;
  data.hIcon = icons_[static_cast<size_t>(badge_)];
  // Fails while Explorer is starting, at sign-in; TaskbarCreated comes once
  // it's up.
  if (!options_.notify_icon(NIM_ADD, data)) return;
  data.uVersion = NOTIFYICON_VERSION_4;
  options_.notify_icon(NIM_SETVERSION, data);
  icon_added_ = true;
  UpdateTip();
}

void TrayApp::ModifyIcon(NOTIFYICONDATAW& data) {
  if (!icon_added_) return;
  data.hWnd = window_;
  data.uID = kIconId;
  // With the tooltip every time: Windows 11 names the icon in the hidden
  // icons' flyout, for screen readers, from the change it got last, and a
  // change without one leaves the icon nameless.
  data.uFlags |= NIF_TIP | NIF_SHOWTIP;
  std::wstring tip = std::format(L"{}\n{}", kDisplayNameW, quitting_ ? L"Closing…" : FromUtf8(StatusLine(snapshot_)));
  if (tip.size() >= std::size(data.szTip)) {
    tip.resize(std::size(data.szTip) - 2);
    tip += L'…';
  }
  tip.copy(data.szTip, tip.size());
  data.szTip[tip.size()] = L'\0';
  options_.notify_icon(NIM_MODIFY, data);
}

void TrayApp::UpdateTip() {
  NOTIFYICONDATAW data = {sizeof(data)};
  ModifyIcon(data);
}

void TrayApp::UpdateBadge() {
  const Badge badge = notifier_.badge();
  if (badge == badge_) return;
  badge_ = badge;
  NOTIFYICONDATAW data = {sizeof(data)};
  data.uFlags = NIF_ICON;
  data.hIcon = icons_[static_cast<size_t>(badge_)];
  ModifyIcon(data);
  if (options_.on_badge) options_.on_badge(badge_);
}

void TrayApp::Notify(const Notifier::Update& update) {
  if (update.show) {
    ShowBalloon(*update.show);
  } else if (update.hide) {
    HideBalloon();
  }
  if ((update.show || update.hide) && options_.on_notice) options_.on_notice(update.show);
  UpdateBadge();
  KillTimer(window_, kNoticeTimer);
  if (const auto next = notifier_.NextDeadline()) {
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(*next - Clock::now()).count();
    SetTimer(window_, kNoticeTimer, static_cast<UINT>(std::clamp<long long>(wait, USER_TIMER_MINIMUM, 60'000)),
             nullptr);
  }
}

void TrayApp::ShowBalloon(const Notice& notice) {
  NOTIFYICONDATAW data = {sizeof(data)};
  data.uFlags = NIF_INFO;
  CopyField(data.szInfoTitle, notice.title);
  CopyField(data.szInfo, notice.text);
  // The knob, with the badge for a problem, rather than one of Windows'
  // icons.
  data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | (notice.sound ? 0u : NIIF_NOSOUND);
  data.hBalloonIcon = notification_icons_[static_cast<size_t>(notice.problem ? Badge::kAttention : Badge::kNone)];
  ModifyIcon(data);
}

void TrayApp::HideBalloon() {
  // An empty text takes it down.
  NOTIFYICONDATAW data = {sizeof(data)};
  data.uFlags = NIF_INFO;
  ModifyIcon(data);
}

void TrayApp::FollowFileHolds() {
  const FileHoldStatus holds = FileHolds();
  if (holds == file_holds_) return;
  Notify(notifier_.FileHold(holds.waiting, !holds.released.empty()));
  for (const auto& file : holds.released) {
    if (std::find(file_holds_.released.begin(), file_holds_.released.end(), file) == file_holds_.released.end()) {
      Log(std::format("The file hold expired for {}. The installer may need to retry after {} closes.", ToUtf8(file),
                      kDisplayName));
    }
  }
  for (const auto& error : holds.errors) {
    if (std::find(file_holds_.errors.begin(), file_holds_.errors.end(), error) == file_holds_.errors.end()) Log(error);
  }
  file_holds_ = holds;
}

bool TrayApp::Quiet() const { return setup_window_ != nullptr || setup_due_; }

void TrayApp::KeepCable() {
  // Only once the first run is finished, and not while it's open or about to
  // open, where the cable page asks.
  if (!first_run_.done || Quiet() || !settings_.cable.empty() || snapshot_.state == core::State::kStarting) return;
  const audio::AudioDevice& obs = snapshot_.obs_cable;
  if (obs.id.empty() || audio::SameId(obs.id, audio::kDefaultDevice)) return;
  core::Settings settings = settings_;
  settings.cable = obs.id;
  const audio::AudioDevice* connected = audio::FindById(snapshot_.outputs, obs.id);
  settings.cable_name = connected ? connected->name : obs.name;
  Log(std::format("Keeping {} as the cable: OBS monitoring to another device no longer moves it.",
                  settings.cable_name.empty() ? settings.cable : settings.cable_name));
  ApplySettings(settings);
}

void TrayApp::SaveWarned() {
  if (notifier_.warnings() == warned_) return;
  warned_ = notifier_.warnings();
  Save();
}

bool TrayApp::RestartByItself() {
  if (snapshot_.state != core::State::kRestartNeeded) {
    restart_tried_.reset();
    return false;
  }
  // Once for each reason, so that one for NVIDIA's SDK still happens after a
  // restart for OBS failed.
  if (restart_tried_ == snapshot_.restart || quitting_) return false;
  // An SDK installer waits for this restart only briefly, so it goes ahead
  // over the first run and any dialog, which close as they do at sign-out,
  // and the menu closes for it. Other restarts wait for them: the menu and
  // the first run try again as they close.
  const bool urgent = (snapshot_.restart == core::RestartNeed::kNvidiaUpdating ||
                       snapshot_.restart == core::RestartNeed::kPluginUpdating) && !snapshot_.restart_delayed;
  if (in_menu_) {
    if (urgent) {
      EndMenu();
    } else {
      SayRestartWaits(RestartWait::kMenu);
    }
    return false;
  }
  if (!urgent && setup_window_) {
    SayRestartWaits(RestartWait::kSetup);
    return false;
  }
  if (!urgent && OpenDialog()) {
    // Other windows, such as an error or a folder picker, don't say when
    // they close.
    SetTimer(window_, kRestartTimer, kRestartRetryMs, nullptr);
    SayRestartWaits(RestartWait::kWindow);
    return false;
  }
  return Restart();
}

void TrayApp::SayRestartWaits(RestartWait window) {
  // NVIDIA's SDK only: the notes on its filter said that knobs restarts by
  // itself to run it, and the cable gets no mic until it does.
  if (snapshot_.restart != core::RestartNeed::kNvidiaInstalled) return;
  if (const Notifier::Update update = notifier_.RestartWaits(window, Clock::now()); update.show) Notify(update);
}

POINT TrayApp::IconPoint() const {
  NOTIFYICONIDENTIFIER icon = {sizeof(icon)};
  icon.hWnd = window_;
  icon.uID = kIconId;
  RECT rect = {};
  if (SUCCEEDED(Shell_NotifyIconGetRect(&icon, &rect))) {
    return {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
  }
  POINT cursor = {};
  GetCursorPos(&cursor);
  return cursor;
}

void TrayApp::ShowMenu(POINT point) {
  if (in_menu_) return;
  in_menu_ = true;
  const bool starts = !quitting_ && options_.starts_with_windows && options_.starts_with_windows();
  const Menu menu = quitting_ ? Menu{.items = {{.id = kIdStatus, .text = "Closing…", .enabled = false}}}
                             : BuildMenu(snapshot_, settings_, starts, MenuUpdates());
  const HMENU native = CreateNativeMenu(menu.items);
  // Without this, the menu wouldn't close when the user clicks elsewhere.
  SetForegroundWindow(window_);
  UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
  flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
  const UINT id = TrackPopupMenuEx(native, flags, point.x, point.y, window_, nullptr);
  PostMessageW(window_, WM_NULL, 0, 0);
  DestroyMenu(native);
  in_menu_ = false;
  if (id != 0) Execute(id, menu);
  if (!RestartByItself()) RunUpdates();
}

void TrayApp::Execute(unsigned id) {
  if (quitting_) return;
  const bool starts = options_.starts_with_windows && options_.starts_with_windows();
  Execute(id, BuildMenu(snapshot_, settings_, starts, MenuUpdates()));
}

void TrayApp::OpenNotice() {
  if (quitting_) {
    ShowMenu(IconPoint());
    return;
  }
  const std::optional<Notice> notice = notifier_.shown();
  notifier_.Clicked();
  if (notice && notice->kind == Notice::Kind::kUpdateReady) {
    if (const auto version = updater_.reminded()) InstallUpdate(*version);
  } else if (notice && !notice->url.empty()) {
    OpenUrl(notice->url);
  } else if (notice && notice->page) {
    ShowFirstRun(false, notice->page);
  } else if (const HWND dialog = OpenDialog()) {
    SetForegroundWindow(dialog);
  } else {
    ShowMenu(IconPoint());
  }
}

void TrayApp::Execute(unsigned id, const Menu& menu) {
  if (quitting_) return;
  if (const auto picked = SettingsForPick(menu, id, settings_)) {
    ApplySettings(*picked);
    return;
  }
  switch (id) {
    case kIdSoundIntoCable:
      OpenUrl("ms-settings:sound");
      break;
    case kIdPause:
      if (core_) core_->Pause();
      break;
    case kIdResume:
      if (core_) core_->Resume();
      break;
    case kIdReimport:
    case kIdTryAgain:
      Reimport();
      break;
    case kIdPauseForObs: {
      core::Settings settings = settings_;
      settings.pause_for_obs = !settings.pause_for_obs;
      ApplySettings(settings);
      break;
    }
    case kIdStartWithWindows:
      ToggleStartWithWindows();
      break;
    case kIdSetup:
      ShowFirstRun(true);
      break;
    case kIdFinishSetup:
    case kIdChooseMic:
    case kIdChooseCable:
      // Opens at the page for it, even before the first run's doors.
      ShowFirstRun(false, PageFor(snapshot_));
      break;
    case kIdFindObs:
      FindObs();
      break;
    case kIdOpenLogs:
      OpenLogFolder();
      break;
    case kIdAbout:
      ShowAbout();
      break;
    case kIdRestart:
      Restart();
      break;
    case kIdGetUpdate:
      if (menu.update) OpenUrl(update::ReleasePageUrl(*menu.update));
      break;
    case kIdInstallUpdate:
      if (menu.update) InstallUpdate(*menu.update);
      break;
    case kIdCheckForUpdates:
      SetUpdates(!updater_.settings().check, updater_.settings().install);
      break;
    case kIdInstallUpdates:
      SetUpdates(updater_.settings().check, !updater_.settings().install);
      break;
    case kIdQuit:
      Quit();
      break;
    default:
      break;
  }
}

void TrayApp::ShowFirstRun(bool from_start, std::optional<FirstRunPage> page) {
  if (quitting_) return;
  if (setup_window_) {
    if (const HWND window = setup_window_->window()) SetForegroundWindow(window);
    return;
  }
  FirstRunProgress progress = first_run_;
  if (progress.done) {
    // Every page again, or only what the state calls for, then the last.
    progress = from_start ? FirstRunProgress{.done = true}
                          : FirstRunProgress{.done = true, .door = Door::kObsUser, .reached = FirstRunPage::kDone};
  }
  const bool starts = options_.starts_with_windows && options_.starts_with_windows();
  const UpdateSettings& updates = updater_.settings();
  const FinishChoices now{
      .start_with_windows = starts, .check_for_updates = updates.check, .install_updates = updates.install};
  SetupWindow dialog(*this, options_.instance,
                     FirstRun(progress, page, now, HasUpdateCheck() && updater_.can_install(), options_.windows11),
                     options_.theme, options_.setup_zoom, options_.setup_walk_at);
  setup_window_ = &dialog;
  dialog.Show();
  setup_window_ = nullptr;
  if (quitting_) return;
  if (RestartByItself()) return;
  // What waited for it to close.
  Notify(notifier_.Tick(Quiet(), Clock::now()));
  KeepCable();
  RunUpdates();
}

void TrayApp::ApplySettings(const core::Settings& settings) {
  if (quitting_) return;
  settings_ = settings;
  if (core_) core_->Apply(settings_);
  Save();
}

void TrayApp::Reimport() {
  if (quitting_) return;
  if (core_) core_->Reimport();
}

Status TrayApp::StartObs(const std::filesystem::path& install_root) {
  if (quitting_) return Error{std::format("{} is closing.", kDisplayName)};
  return options_.open_obs ? options_.open_obs(install_root) : OpenObs(install_root);
}

void TrayApp::FinishFirstRun(const FinishChoices& finish) {
  if (quitting_) return;
  const bool starts = options_.starts_with_windows && options_.starts_with_windows();
  if (options_.set_start_with_windows && finish.start_with_windows != starts) {
    if (const Status set = options_.set_start_with_windows(finish.start_with_windows); !set) ShowError(set.error());
  }
  first_run_.done = true;
  // Nothing to carry on with after a restart, as when the installer asked
  // for one and the cable appeared without it.
  if (options_.resume_after_restart) {
    if (const Status off = options_.resume_after_restart(false); !off) Log(off.error());
  }
  SetUpdates(finish.check_for_updates, finish.install_updates);
}

void TrayApp::SaveFirstRun(const FirstRunProgress& progress) {
  if (quitting_) return;
  first_run_ = progress;
  Save();
}

void TrayApp::ResumeAfterRestart(bool on) {
  if (quitting_) return;
  if (!options_.resume_after_restart) return;
  const Status set = options_.resume_after_restart(on);
  if (set) return;
  Log(set.error());
  // The page says knobs opens after the restart. It's in the middle of
  // showing that, so the error waits for it.
  if (on) {
    later_error_ = std::format("{} can't open by itself after the restart, so open it once Windows has restarted."
                               "\n\n{}",
                               kDisplayName, set.error());
    PostMessageW(window_, kErrorMessage, 0, 0);
  }
}

void TrayApp::Save() {
  if (options_.settings_file.empty()) return;
  if (const Status saved = SaveSettings(options_.settings_file, {settings_, first_run_, updater_.settings(), warned_});
      !saved) {
    ShowError(std::format("Couldn't save the change, so it lasts only until {} quits.\n\n{}", kDisplayName,
                          saved.error()));
  }
}

void TrayApp::ToggleStartWithWindows() {
  if (!options_.set_start_with_windows) return;
  const bool on = !(options_.starts_with_windows && options_.starts_with_windows());
  if (const Status set = options_.set_start_with_windows(on); !set) ShowError(set.error());
}

void TrayApp::FindObs() {
  // The first run's page, not a bare folder picker: it says how to get OBS,
  // offers Choose folder…, and Look for OBS when a folder was picked before.
  ShowFirstRun(false, FirstRunPage::kFindObs);
}

void TrayApp::OpenLogFolder() {
  std::error_code ec;
  std::filesystem::create_directories(options_.log_folder, ec);
  const auto opened = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", options_.log_folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (opened <= 32) ShowError(std::format("Couldn't open the log folder, {}.", ToUtf8(options_.log_folder)));
}

bool TrayApp::Restart() {
  if (quitting_) return false;
  if (restart_result_) return true;
  // Once by itself: if it fails, the menu and the first run offer it.
  restart_tried_ = snapshot_.restart;
  if (!options_.restart) return false;
  // Paused, the new copy is too: a restart doesn't put the mic back live.
  const auto start = [restart = options_.restart, why = snapshot_.restart, paused = Paused()](const auto& files) {
    return restart(why, paused, files);
  };
  if (core_) {
    restart_result_ = core_->Restart(start);
  } else {
    std::promise<Status> result;
    result.set_value(start(core::RestartFiles{}));
    restart_result_ = result.get_future();
  }
  FinishRestart();
  return true;
}

void TrayApp::FinishRestart() {
  if (!restart_result_) return;
  if (restart_result_->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    SetTimer(window_, kRestartTimer, 50, nullptr);
    return;
  }
  const Status started = restart_result_->get();
  restart_result_.reset();
  if (!started) {
    const bool updating = snapshot_.restart == core::RestartNeed::kNvidiaUpdating ||
                          snapshot_.restart == core::RestartNeed::kPluginUpdating;
    const std::string advice = updating
        ? std::format("{0} let the installer continue. If it says a file is in use, quit {0} and retry it.",
                      kDisplayName)
        : std::format("Quit {}, then start it again.", kDisplayName);
    ShowError(std::format("{}\n\n{}", started.error(), advice));
    return;
  }
  Quit();
}

void TrayApp::ShowAbout() {
  const HICON icon = LoadAppIcon(options_.instance, LIM_LARGE);
  ShowMessageDialog(options_.instance,
                    {.custom_icon = icon,
                     .instruction = std::format(L"{} {}", kDisplayNameW, L"" KNOBS_VERSION),
                     .content = std::format(L"Your OBS mic chain, in every app. {0} runs the filters from your own "
                                            L"OBS Studio install and sends your mic to a virtual cable.\n\n{0} "
                                            L"isn't affiliated with or endorsed by the OBS Project.",
                                            kDisplayNameW),
                     .footer = L"Free software under the GNU General Public License, version 2 or later."});
  if (icon) DestroyIcon(icon);
}

void TrayApp::ShowError(std::string_view text) {
  if (quitting_) return;
  // Over the first run while it's open, which keeps its buttons from being
  // clicked again while the error is up: a button's handler may be why.
  const HWND owner = setup_window_ ? setup_window_->window() : nullptr;
  ShowMessageDialog(options_.instance, {.owner = owner, .icon = TD_WARNING_ICON, .content = FromUtf8(text)});
}

void TrayApp::OpenUrl(const std::string& url) {
  const auto opened = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", FromUtf8(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (opened <= 32) ShowError(std::format("Couldn't open {} in your browser.", url));
}

void TrayApp::Log(std::string line) {
  if (options_.log) {
    options_.log(line);
  } else if (core_) {
    core_->Log(std::move(line));
  }
}

Status TrayApp::StartCore() {
  auto core = options_.start_core(*this, settings_, options_.start_paused);
  if (!core) return Error{core.error()};
  core_ = std::move(*core);
  return Ok{};
}

void TrayApp::ReleaseCore() {
  if (!holding_core_ || quitting_) return;
  KillTimer(window_, kCoreTimer);
  // Not while an installer runs, which closes this copy: the mic would only
  // start to stop again. One that ends first releases it (InstallerEnded).
  if (installer_) return;
  holding_core_ = false;
  if (const Status started = StartCore(); !started) {
    failed_ = started.error();
    Quit();
  }
}

bool TrayApp::Paused() const {
  // Before the core says otherwise, as this copy was started: an update at
  // start comes before it.
  return snapshot_.paused_by_user || (snapshot_.state == core::State::kStarting && options_.start_paused);
}

void TrayApp::KeepReopening() {
  if (!options_.reopen_after_installer || quitting_ || session_ending_) return;
  const bool paused = Paused();
  if (reopen_paused_ == paused) return;
  if (const Status on = options_.reopen_after_installer(true, paused); !on) {
    Log(on.error());
    return;
  }
  reopen_paused_ = paused;
}

bool TrayApp::FilesUpdating() const {
  return snapshot_.files_waiting || snapshot_.state == core::State::kPausedForUpdate ||
         snapshot_.restart == core::RestartNeed::kNvidiaUpdating || snapshot_.restart == core::RestartNeed::kPluginUpdating ||
         (snapshot_.state == core::State::kStarting && (options_.wait_for_nvidia || !options_.wait_folders.empty()));
}

bool TrayApp::CanRestart() const {
  // A self-update starts a new copy without the plugin wait. Let that wait
  // finish first, including the new SDK copy's time before its first snapshot.
  if (FilesUpdating()) return false;
  return snapshot_.state != core::State::kRunning && !Paused() && !Quiet() && !in_menu_ && !OpenDialog();
}

// --- Updates ------------------------------------------------------------------------

std::filesystem::path TrayApp::InstallerFile(const update::Version& version) const {
  return options_.updates.folder / FromUtf8(update::InstallerName(version));
}

void TrayApp::PrepareUpdates() {
  if (!TidyUpdates()) return;
  // Waiting since it downloaded, for the day before a notification offers it.
  updater_.Found(LastWritten(InstallerFile(*updater_.installable())).value_or(Updater::Clock::now()));
}

std::filesystem::path TrayApp::UnverifiedFile(const update::Version& version) const {
  return options_.updates.folder / L"unverified" / FromUtf8(update::InstallerName(version));
}

bool TrayApp::TidyUpdates() {
  // Another copy, as a build or an unzipped copy that shares the folder,
  // leaves the installed copy's installer alone.
  if (!updater_.can_install()) return false;
  // The installer for the version that's installable is kept, or one
  // downloaded whose signature Windows couldn't finish checking, and
  // nothing else: the update it was for is done, given up on, or another
  // came out.
  std::filesystem::path keep, unverified;
  if (const auto version = updater_.installable()) {
    keep = InstallerFile(*version);
    unverified = UnverifiedFile(*version);
  }
  std::error_code ec;
  std::vector<std::filesystem::path> old;
  for (const std::filesystem::path& folder : {options_.updates.folder, options_.updates.folder / L"unverified"}) {
    for (auto it = std::filesystem::directory_iterator(folder, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
      if (it->path() != keep && it->path() != unverified && it->path() != unverified.parent_path()) {
        old.push_back(it->path());
      }
    }
    ec.clear();
  }
  // One in use stays until the next start.
  for (const std::filesystem::path& path : old) std::filesystem::remove_all(path, ec);
  return !keep.empty() && std::filesystem::is_regular_file(keep, ec);
}

void TrayApp::RunUpdates() {
  if (!HasUpdateCheck() || quitting_) return;
  KillTimer(window_, kUpdateTimer);
  // A job is running: it says when it's done.
  if (update_thread_.joinable()) return;
  // The menu asked to update while one ran, or while files were being
  // updated.
  if (const auto asked = FilesUpdating() ? std::nullopt : std::exchange(install_asked_, std::nullopt)) {
    const auto offer = updater_.offer();
    if (offer && offer->install && offer->version == *asked) {
      StartUpdateJob(Updater::Job::kInstall, true);
      return;
    }
    NotOffered(*asked);
  }
  const Updater::Clock::time_point now = Updater::Clock::now();
  if (auto waiting = updater_.Waiting(now)) {
    // Once a version: the next start knows it was said.
    Save();
    Notify(notifier_.Say(std::move(*waiting), Quiet()));
  }
  if (const Updater::Job job = updater_.Due(now, CanRestart()); job != Updater::Job::kNone) {
    StartUpdateJob(job);
    return;
  }
  if (const auto next = updater_.NextDeadline(now)) {
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(*next - now).count();
    SetTimer(window_, kUpdateTimer, static_cast<UINT>(std::clamp<long long>(wait, USER_TIMER_MINIMUM, kUpdateTimerMaxMs)),
             nullptr);
  }
}

void TrayApp::StartUpdateJob(Updater::Job job, bool asked) {
  const std::optional<update::Version> latest = updater_.latest();
  if (job != Updater::Job::kCheck && !latest) return;
  auto work = std::make_shared<UpdateWork>();
  work->job = job;
  work->asked = asked;
  work->version = latest;
  update_work_ = work;
  const std::filesystem::path file = latest ? InstallerFile(*latest) : std::filesystem::path();
  const std::filesystem::path unverified = latest ? UnverifiedFile(*latest) : std::filesystem::path();
  std::error_code ec;
  // One downloaded before whose signature Windows couldn't finish checking
  // is checked again, not downloaded again.
  const bool kept = job == Updater::Job::kDownload && std::filesystem::is_regular_file(unverified, ec);
  switch (job) {
    case Updater::Job::kCheck:
      Log("Asking GitHub for the latest release.");
      break;
    case Updater::Job::kDownload:
      Log(kept ? std::format("Checking the signature of {} again.", ToUtf8(unverified))
               : std::format("Downloading {}.", update::InstallerUrl(*latest)));
      break;
    default:
      break;
  }
  update_thread_ = std::thread([work, hooks = options_.updates, file, unverified, kept, window = window_] {
    if (work->job == Updater::Job::kCheck) {
      if (auto found = hooks.latest(work->cancel)) {
        work->latest = *found;
      } else {
        work->error = found.error();
      }
    } else if (work->job == Updater::Job::kDownload) {
      // Into a folder of its own, until its signature has been checked.
      std::error_code ec;
      const std::filesystem::path download = file.parent_path() / L"download";
      const std::filesystem::path part = kept ? unverified : download / file.filename();
      std::optional<update::DownloadResult> got;
      if (!kept) {
        std::filesystem::create_directories(download, ec);
        got = hooks.download(*work->version, part, work->cancel);
      }
      if (got && !got->ok()) {
        if (got->outcome == update::DownloadResult::Outcome::kUnusable) {
          work->downloaded = Updater::Downloaded::kUnusable;
        }
        work->error = got->error;
      } else if (const update::SignatureCheck verified = hooks.verify(part); !verified.ok()) {
        work->error = verified.error;
        if (verified.verdict == update::SignatureCheck::Verdict::kRejected) {
          work->downloaded = Updater::Downloaded::kRejected;
          std::filesystem::remove(part, ec);
        } else if (!kept) {
          // Windows couldn't finish the check: kept for the next one, which
          // doesn't download it again.
          std::filesystem::create_directories(unverified.parent_path(), ec);
          std::filesystem::rename(part, unverified, ec);
        }
      } else {
        std::filesystem::rename(part, file, ec);
        if (ec) {
          work->error = std::format("Couldn't move the installer into {}: {}", ToUtf8(file.parent_path()),
                                    ec.message());
        } else {
          work->downloaded = Updater::Downloaded::kDone;
        }
      }
      std::filesystem::remove_all(download, ec);
    } else if (work->job == Updater::Job::kInstall) {
      // Again, just before it runs.
      work->verified = hooks.verify(file);
    }
    PostMessageW(window, kUpdateMessage, 1, 0);
    work->done.release();
  });
}

void TrayApp::FinishUpdateJob() {
  if (update_thread_.joinable()) update_thread_.join();
  const std::shared_ptr<UpdateWork> work = std::move(update_work_);
  if (!work || quitting_) return;
  const Updater::Clock::time_point now = Updater::Clock::now();
  std::optional<Notice> notice;
  // For an install the user asked for, and waits on.
  std::string error;
  switch (work->job) {
    case Updater::Job::kCheck:
      // It was turned off while the check ran: what it found is dropped.
      if (!updater_.settings().check) {
        Log("The update check was turned off, so its answer is dropped.");
        break;
      }
      if (work->latest) {
        Log(std::format("The latest release is {} {}.", kDisplayName, work->latest->ToString()));
      } else {
        Log(std::format("Couldn't check for a new version: {}", work->error));
      }
      notice = updater_.Checked(now, work->latest);
      break;
    case Updater::Job::kDownload:
      // As is a download, with the installer it got.
      if (!updater_.settings().check) {
        Log("The update check was turned off, so the download is dropped.");
        if (work->downloaded == Updater::Downloaded::kDone) {
          std::error_code ec;
          std::filesystem::remove(InstallerFile(*work->version), ec);
        }
        break;
      }
      if (work->downloaded == Updater::Downloaded::kDone) {
        Log(std::format("Downloaded {}, signed as {}'s releases are.", ToUtf8(InstallerFile(*updater_.latest())),
                        kDisplayName));
      } else {
        Log(std::format("Couldn't download the new version: {}", work->error));
      }
      notice = updater_.DownloadDone(now, work->downloaded);
      break;
    case Updater::Job::kInstall: {
      const update::Version version = *work->version;
      const std::filesystem::path file = InstallerFile(version);
      const bool asked = work->asked || install_asked_ == version;
      install_asked_.reset();
      if (!work->verified.ok()) {
        Log(std::format("Didn't install {} {}: {}", kDisplayName, version.ToString(), work->verified.error));
        // One Windows couldn't finish checking is kept, and checked again
        // later.
        const bool lost = work->verified.verdict == update::SignatureCheck::Verdict::kRejected;
        if (lost) {
          std::error_code ec;
          std::filesystem::remove(file, ec);
        }
        notice = updater_.NotVerified(now, lost);
        if (asked) {
          // Offered still, or not: then the menu offers its page, if it
          // offers the version at all, as a link here would be cut short.
          const auto offer = updater_.offer();
          std::string next = "Try again later.";
          if (!offer || offer->version != version) {
            next = std::format("{} {} isn't the latest version any more.", kDisplayName, version.ToString());
          } else if (!offer->install) {
            next = std::format("Get {} {}… in the menu opens its page, to download it yourself.", kDisplayName,
                               version.ToString());
          }
          error = std::format("Couldn't update to {} {}. {}\n\n{}", kDisplayName, version.ToString(), next,
                              work->verified.error);
          // The error says it.
          notice.reset();
        }
        break;
      }
      // What the user asked for happens now. By itself, it waits for a moment
      // when knobs can restart, which may have passed while the signature was
      // checked, and only while it still installs by itself.
      if (!asked && updater_.Due(now, CanRestart()) != Updater::Job::kInstall) break;
      // Asked or not, not while files are being updated: it installs once
      // they've settled (RunUpdates).
      if (FilesUpdating()) {
        Log(std::format("Updating to {} {} once the files being updated have settled.", kDisplayName,
                        version.ToString()));
        install_asked_ = version;
        break;
      }
      Log(std::format("Updating to {} {}.", kDisplayName, version.ToString()));
      updater_.Installing();
      Save();
      // Paused, the new copy is too.
      auto started = options_.updates.install(file, Paused());
      if (!started) {
        Log(std::format("Couldn't start the installer: {}", started.error()));
        notice = UpdatedNotice(updater_.settings(), options_.updates.current);
        updater_.Reported();
        break;
      }
      if (!*started) {
        // Nothing to wait on: the installer closes this copy, once it's
        // ready to, and going now spares it the wait.
        Quit();
        return;
      }
      // The installer closes this copy as it's ready to install. Until then
      // knobs runs on, so one that ends first, as an antivirus can make it,
      // doesn't leave it closed.
      WaitForInstaller(*started, version);
      break;
    }
    case Updater::Job::kNone:
      break;
  }
  Save();
  // An installer for a version given up on, or one a newer version replaced.
  TidyUpdates();
  if (notice) Notify(notifier_.Say(std::move(*notice), Quiet()));
  // An update at start that didn't install: knobs starts as usual.
  ReleaseCore();
  if (quitting_) return;
  if (!error.empty()) ShowError(error);
  RunUpdates();
}

void TrayApp::SetUpdates(bool check, bool install) {
  const UpdateSettings before = updater_.settings();
  updater_.Set(check, install);
  const UpdateSettings& after = updater_.settings();
  // A check or a download running stops: no network once it's off. An
  // installer's check doesn't use it.
  if (!after.check && update_work_ && update_work_->job != Updater::Job::kInstall) update_work_->cancel.Stop();
  if (after.check != before.check) {
    Log(after.check ? "Checking for new versions once a day." : "No longer checking for new versions.");
  }
  if (after.install != before.install) {
    Log(after.install ? "Installing new versions by itself." : "No longer installing new versions by itself.");
  }
  Save();
  RunUpdates();
}

void TrayApp::InstallUpdate(const update::Version& asked) {
  if (update_thread_.joinable() || FilesUpdating()) {
    install_asked_ = asked;
    return;
  }
  const auto offer = updater_.offer();
  if (offer && offer->install && offer->version == asked) {
    StartUpdateJob(Updater::Job::kInstall, true);
  } else {
    NotOffered(asked);
  }
}

void TrayApp::NotOffered(const update::Version& asked) {
  // A check that ended while the menu or the notification was up found a
  // newer version, which isn't downloaded yet, or none newer than this copy,
  // as when a release is taken back, or the installer was lost.
  const auto offer = updater_.offer();
  if (offer && offer->version != asked) {
    ShowError(std::format("{} {} is out now, so the menu offers that instead.", kDisplayName,
                          offer->version.ToString()));
  } else if (!offer) {
    ShowError(std::format("{} {} isn't the latest version any more, so it isn't installed.", kDisplayName,
                          asked.ToString()));
  } else {
    ShowError(std::format("{} {} isn't ready to install any more. Get {} {}… in the menu opens its page, to download "
                          "it yourself.",
                          kDisplayName, asked.ToString(), kDisplayName, asked.ToString()));
  }
}

void TrayApp::WaitForInstaller(HANDLE process, const update::Version& version) {
  StopWaitingForInstaller();
  installer_ = process;
  installer_version_ = version;
  installer_stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  installer_thread_ = std::thread([process, stop = installer_stop_, window = window_] {
    const HANDLE handles[] = {stop, process};
    if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return;
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    PostMessageW(window, kInstallerMessage, code, 0);
  });
}

void TrayApp::InstallerEnded(DWORD exit_code) {
  if (!installer_ || quitting_) return;
  const update::Version version = installer_version_;
  StopWaitingForInstaller();
  // It never closed this copy, so it didn't install. It isn't tried again
  // by itself: its page is where to get it.
  Log(std::format("The installer for {} {} ended without updating it (exit code {}).", kDisplayName,
                  version.ToString(), exit_code));
  updater_.Reported();
  Save();
  Notify(notifier_.Say(NotUpdatedNotice(version), Quiet()));
  // An update at start held the mic back for it.
  ReleaseCore();
  if (quitting_) return;
  RunUpdates();
}

void TrayApp::StopWaitingForInstaller() {
  if (installer_thread_.joinable()) {
    SetEvent(installer_stop_);
    installer_thread_.join();
  }
  if (installer_stop_) CloseHandle(installer_stop_);
  if (installer_) CloseHandle(installer_);
  installer_stop_ = nullptr;
  installer_ = nullptr;
}

bool TrayApp::StopUpdates() {
  KillTimer(window_, kUpdateTimer);
  if (!update_thread_.joinable()) return true;
  if (WaitForSingleObject(update_thread_.native_handle(), 0) == WAIT_OBJECT_0) {
    update_thread_.join();
  } else if (Clock::now() >= update_stop_due_) {
    // It keeps its own share of the work, and its post to a window that's
    // gone does nothing.
    update_thread_.detach();
  } else {
    return false;
  }
  update_work_.reset();
  return true;
}

UpdateMenu TrayApp::MenuUpdates() const {
  if (!HasUpdateCheck()) return {};
  const UpdateSettings& updates = updater_.settings();
  UpdateMenu menu{.offered = true,
                  .check = updates.check,
                  .install = updates.install,
                  .can_install = updater_.can_install()};
  if (const auto offer = updater_.offer()) {
    menu.version = offer->version;
    menu.install_now = offer->install;
  }
  return menu;
}

HWND TrayApp::OpenDialog() const {
  struct Search {
    HWND tray;
    HWND found = nullptr;
  } search{window_};
  EnumThreadWindows(
      GetCurrentThreadId(),
      [](HWND window, LPARAM param) -> BOOL {
        auto* search = reinterpret_cast<Search*>(param);
        if (window == search->tray || !IsWindowVisible(window)) return TRUE;
        search->found = window;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.found;
}

bool ActivateTray(const std::wstring& window_class, bool resume_setup) {
  const HWND window = FindWindowW(window_class.c_str(), nullptr);
  if (!window) return false;
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  // Lets it bring its menu to the front.
  AllowSetForegroundWindow(process);
  return PostMessageW(window, kActivateMessage, resume_setup, 0) != FALSE;
}

void ShowStartupError(std::string_view text) {
  ShowMessageDialog(GetModuleHandleW(nullptr), {.icon = TD_ERROR_ICON, .content = FromUtf8(text)});
}

}  // namespace knobs::tray
