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
constexpr UINT_PTR kNoticeTimer = 1;   // Notifier::NextDeadline.
constexpr UINT_PTR kRestartTimer = 2;  // RestartByItself, while a window is open.
constexpr UINT_PTR kUpdateTimer = 3;   // Updater::NextDeadline.
constexpr UINT_PTR kCoreTimer = 4;     // ReleaseCore, if an update at start takes too long.
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
      app->update_settings_ = loaded->updates;
    } else {
      settings_error = loaded.error();
    }
  }
  if (opts.updates.latest) {
    app->updater_.emplace(Updater::Options{.current = opts.updates.current,
                                           .can_install = opts.updates.can_install,
                                           .first = opts.updates.first_check,
                                           .remind_after = opts.updates.remind_after},
                          app->update_settings_, Updater::Clock::now());
    app->PrepareUpdates();
  }
  if (opts.updated) {
    app->notifier_ = Notifier({.restarted_for = opts.restarted_for,
                               .updated = UpdatedNotice(app->update_settings_, opts.updates.current)});
  }

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

  // An update downloaded before installs now, before knobs sends the mic:
  // once knobs runs, it waits for a moment the mic isn't sent anyway.
  if (app->updater_ && app->updater_->Due(Updater::Clock::now(), app->CanRestart()) == Updater::Job::kInstall) {
    app->holding_core_ = true;
    app->StartUpdateJob(Updater::Job::kInstall);
    SetTimer(app->window_, kCoreTimer, kCoreHoldMs, nullptr);
  } else if (const Status started = app->StartCore(); !started) {
    return Error{started.error()};
  }
  if (!settings_error.empty()) {
    app->ShowError(std::format("Couldn't read the settings, so {} uses its defaults until they're changed.\n\n{}",
                               kDisplayName, settings_error));
  }
  // Once Run pumps messages: the dialog's loop needs Run's to end with it.
  if (opts.resume_first_run || (opts.open_first_run && !app->first_run_.done)) {
    PostMessageW(app->window_, kFirstRunMessage, opts.resume_first_run, 0);
  }
  if (app->updater_) PostMessageW(app->window_, kUpdateMessage, 0, 0);
  return app;
}

TrayApp::TrayApp(TrayOptions options)
    : options_(std::move(options)),
      settings_(options_.settings),
      first_run_(options_.first_run),
      update_settings_(options_.update_settings),
      notifier_({.restarted_for = options_.restarted_for}) {}

TrayApp::~TrayApp() {
  Quit();
  DestroyIcons();
}

int TrayApp::Run() {
  running_ = true;
  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}

void TrayApp::Quit() {
  if (quitting_) return;
  quitting_ = true;
  // An update job stops while the core does.
  if (update_work_) update_work_->cancel.Stop();
  core_.reset();  // Stops the chain and libobs, which releases the mic and the cable.
  StopUpdates();
  if (icon_added_) {
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = window_;
    data.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    icon_added_ = false;
  }
  if (window_) DestroyWindow(window_);
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
    // Explorer restarted, and the icon went with it, or the taskbar's DPI
    // changed, which may leave it there at the old size.
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = window_;
    data.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    icon_added_ = false;
    LoadIcons();
    AddIcon();
    return 0;
  }
  switch (message) {
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
      if (snapshot) {
        const core::Snapshot before = std::exchange(snapshot_, std::move(*snapshot));
        UpdateTip();
        if (RestartByItself()) return 0;
        if (setup_window_) setup_window_->Changed(before);
        Notify(notifier_.Changed(snapshot_, Quiet(), Clock::now()));
        if (options_.on_snapshot) options_.on_snapshot(snapshot_);
        RunUpdates();
      }
      return 0;
    }
    case kUpdateMessage:
      if (wparam == 1) {
        FinishUpdateJob();
      } else {
        RunUpdates();
      }
      return 0;
    case WM_TIMER:
      if (wparam == kUpdateTimer) {
        KillTimer(window_, kUpdateTimer);
        RunUpdates();
        return 0;
      }
      if (wparam == kRestartTimer) {
        KillTimer(window_, kRestartTimer);
        RestartByItself();
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
      if (const HWND dialog = OpenDialog()) {
        SetForegroundWindow(dialog);
      } else if (!first_run_.done || wparam) {
        // Started again before setup was finished, or after a restart it
        // asked for: that's what's wanted.
        ShowFirstRun(false);
      } else {
        ShowMenu(IconPoint());
      }
      return 0;
    case kFirstRunMessage:
      if (!first_run_.done || wparam) ShowFirstRun(false);
      return 0;
    case kErrorMessage:
      if (!later_error_.empty()) ShowError(std::exchange(later_error_, {}));
      return 0;
    case WM_SETTINGCHANGE:
      if (wparam == SPI_SETHIGHCONTRAST ||
          (lparam && std::wstring_view(reinterpret_cast<const wchar_t*>(lparam)) == L"ImmersiveColorSet")) {
        ApplyMenuTheme(options_.theme);
      }
      break;
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      // Windows is shutting down or signing out, and ends the process after
      // this returns: let go of the mic and the cable first.
      if (wparam) Quit();
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
  const HICON small_icon = LoadAppIcon(options_.instance, LIM_SMALL);
  // Windows 11 draws a notification's icon about 50 px wide at 100%, and
  // resamples whatever it's given. Twice that is sharpest: an icon that size
  // or a little bigger blurs, and a much bigger one comes out jagged.
  const int size = MulDiv(2 * 48, static_cast<int>(GetDpiForSystem()), 96);
  HICON notification = nullptr;
  if (FAILED(LoadIconWithScaleDown(options_.instance, MAKEINTRESOURCEW(IDI_KNOBS), size, size, &notification))) {
    notification = nullptr;
  }
  for (const Badge badge : kBadges) {
    icons_[static_cast<size_t>(badge)] = BadgedIcon(small_icon, badge);
    notification_icons_[static_cast<size_t>(badge)] = BadgedIcon(notification, badge);
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
  if (!Shell_NotifyIconW(NIM_ADD, &data)) return;
  data.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &data);
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
  std::wstring tip = std::format(L"{}\n{}", kDisplayNameW, FromUtf8(StatusLine(snapshot_)));
  if (tip.size() >= std::size(data.szTip)) {
    tip.resize(std::size(data.szTip) - 2);
    tip += L'…';
  }
  tip.copy(data.szTip, tip.size());
  data.szTip[tip.size()] = L'\0';
  Shell_NotifyIconW(NIM_MODIFY, &data);
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

bool TrayApp::Quiet() const { return !first_run_.done || setup_window_ != nullptr; }

bool TrayApp::RestartByItself() {
  if (snapshot_.state != core::State::kRestartNeeded) {
    restart_tried_ = false;
    return false;
  }
  if (restart_tried_ || quitting_) return false;
  // The menu and the first run try again as they close.
  if (in_menu_ || setup_window_) return false;
  if (OpenDialog()) {
    // Other windows, such as an error or a folder picker, don't say when
    // they close.
    SetTimer(window_, kRestartTimer, kRestartRetryMs, nullptr);
    return false;
  }
  return Restart();
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
  if (in_menu_ || quitting_) return;
  in_menu_ = true;
  const bool starts = options_.starts_with_windows && options_.starts_with_windows();
  const Menu menu = BuildMenu(snapshot_, settings_, starts, MenuUpdates());
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
  const bool starts = options_.starts_with_windows && options_.starts_with_windows();
  Execute(id, BuildMenu(snapshot_, settings_, starts, MenuUpdates()));
}

void TrayApp::OpenNotice() {
  const std::optional<Notice> notice = notifier_.shown();
  notifier_.Clicked();
  if (notice && notice->kind == Notice::Kind::kUpdateReady) {
    InstallUpdate();
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
  if (const auto picked = SettingsForPick(menu, id, settings_)) {
    ApplySettings(*picked);
    return;
  }
  switch (id) {
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
      // Opens at the page for it: the state calls for it.
      ShowFirstRun(false);
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
      if (updater_ && updater_->latest()) OpenUrl(update::ReleasePageUrl(*updater_->latest()));
      break;
    case kIdInstallUpdate:
      InstallUpdate();
      break;
    case kIdCheckForUpdates:
      if (updater_) SetUpdates(!updater_->settings().check, updater_->settings().install);
      break;
    case kIdInstallUpdates:
      if (updater_) SetUpdates(updater_->settings().check, !updater_->settings().install);
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
  const UpdateSettings& updates = updater_ ? updater_->settings() : update_settings_;
  const FinishChoices now{
      .start_with_windows = starts, .check_for_updates = updates.check, .install_updates = updates.install};
  SetupWindow dialog(*this, options_.instance, FirstRun(progress, page, now, updater_ && updater_->can_install()),
                     options_.theme, options_.setup_zoom, options_.setup_walk_at);
  setup_window_ = &dialog;
  dialog.Show();
  setup_window_ = nullptr;
  if (RestartByItself()) return;
  // What waited for it to close.
  Notify(notifier_.Tick(Quiet(), Clock::now()));
  RunUpdates();
}

void TrayApp::ApplySettings(const core::Settings& settings) {
  settings_ = settings;
  if (core_) core_->Apply(settings_);
  Save();
}

void TrayApp::Reimport() {
  if (core_) core_->Reimport();
}

Status TrayApp::StartObs(const std::filesystem::path& install_root) {
  return options_.open_obs ? options_.open_obs(install_root) : OpenObs(install_root);
}

void TrayApp::FinishFirstRun(const FinishChoices& finish) {
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
  if (updater_) {
    SetUpdates(finish.check_for_updates, finish.install_updates);
  } else {
    Save();
  }
}

void TrayApp::SaveFirstRun(const FirstRunProgress& progress) {
  first_run_ = progress;
  Save();
}

void TrayApp::ResumeAfterRestart(bool on) {
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
  const UpdateSettings& updates = updater_ ? updater_->settings() : update_settings_;
  if (const Status saved = SaveSettings(options_.settings_file, {settings_, first_run_, updates}); !saved) {
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
  // Once by itself: if it fails, the menu and the first run offer it.
  restart_tried_ = true;
  if (!options_.restart) return false;
  if (const Status started = options_.restart(snapshot_.restart); !started) {
    ShowError(std::format("{}\n\nQuit {}, then start it again.", started.error(), kDisplayName));
    return false;
  }
  Quit();
  return true;
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
  auto core = options_.start_core(*this, settings_);
  if (!core) return Error{core.error()};
  core_ = std::move(*core);
  return Ok{};
}

void TrayApp::ReleaseCore() {
  if (!holding_core_ || quitting_) return;
  holding_core_ = false;
  KillTimer(window_, kCoreTimer);
  if (const Status started = StartCore(); !started) {
    ShowStartupError(started.error());
    Quit();
  }
}

bool TrayApp::CanRestart() const {
  return snapshot_.state != core::State::kRunning && !snapshot_.paused_by_user && !Quiet() && !in_menu_ &&
         !OpenDialog();
}

// --- Updates ------------------------------------------------------------------------

std::filesystem::path TrayApp::InstallerFile(const update::Version& version) const {
  return options_.updates.folder / FromUtf8(update::InstallerName(version));
}

void TrayApp::PrepareUpdates() {
  // The installer for the version that's installable is kept, and nothing
  // else: the update it was for is done, or another came out.
  std::filesystem::path keep;
  if (const auto version = updater_->installable()) keep = InstallerFile(*version);
  std::error_code ec;
  std::vector<std::filesystem::path> old;
  for (auto it = std::filesystem::directory_iterator(options_.updates.folder, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (it->path() != keep) old.push_back(it->path());
  }
  // One in use stays until the next start.
  for (const std::filesystem::path& path : old) std::filesystem::remove_all(path, ec);
  if (!keep.empty() && std::filesystem::is_regular_file(keep, ec)) updater_->Found();
}

void TrayApp::RunUpdates() {
  if (!updater_ || quitting_) return;
  KillTimer(window_, kUpdateTimer);
  // A job is running: it says when it's done.
  if (update_thread_.joinable()) return;
  // The menu asked to update while one ran, and it's still offered.
  if (const auto asked = std::exchange(install_asked_, std::nullopt)) {
    const auto offer = updater_->offer();
    if (offer && offer->install && offer->version == *asked) {
      StartUpdateJob(Updater::Job::kInstall, true);
      return;
    }
  }
  const Updater::Clock::time_point now = Updater::Clock::now();
  if (auto waiting = updater_->Waiting(now)) Notify(notifier_.Say(std::move(*waiting), Quiet()));
  if (const Updater::Job job = updater_->Due(now, CanRestart()); job != Updater::Job::kNone) {
    StartUpdateJob(job);
    return;
  }
  if (const auto next = updater_->NextDeadline(now)) {
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(*next - now).count();
    SetTimer(window_, kUpdateTimer, static_cast<UINT>(std::clamp<long long>(wait, USER_TIMER_MINIMUM, kUpdateTimerMaxMs)),
             nullptr);
  }
}

void TrayApp::StartUpdateJob(Updater::Job job, bool asked) {
  const std::optional<update::Version> latest = updater_->latest();
  if (job != Updater::Job::kCheck && !latest) return;
  auto work = std::make_shared<UpdateWork>();
  work->job = job;
  work->asked = asked;
  work->version = latest;
  update_work_ = work;
  const std::filesystem::path file = latest ? InstallerFile(*latest) : std::filesystem::path();
  switch (job) {
    case Updater::Job::kCheck:
      Log("Asking GitHub for the latest release.");
      break;
    case Updater::Job::kDownload:
      Log(std::format("Downloading {}.", update::InstallerUrl(*latest)));
      break;
    default:
      break;
  }
  update_thread_ = std::thread([work, hooks = options_.updates, file, window = window_] {
    if (work->job == Updater::Job::kCheck) {
      if (auto found = hooks.latest(work->cancel)) {
        work->latest = *found;
      } else {
        work->error = found.error();
      }
    } else if (work->job == Updater::Job::kDownload) {
      // Into a folder of its own, until its signature has been checked.
      std::error_code ec;
      const std::filesystem::path part = file.parent_path() / L"download" / file.filename();
      std::filesystem::create_directories(part.parent_path(), ec);
      if (const Status got = hooks.download(*work->version, part, work->cancel); !got) {
        work->error = got.error();
      } else if (const update::SignatureCheck verified = hooks.verify(part); !verified.ok()) {
        // One Windows couldn't finish checking is downloaded again later.
        if (verified.verdict == update::SignatureCheck::Verdict::kRejected) {
          work->downloaded = Updater::Downloaded::kRejected;
        }
        work->error = verified.error;
      } else {
        std::filesystem::rename(part, file, ec);
        if (ec) {
          work->error = std::format("Couldn't move the installer into {}: {}", ToUtf8(file.parent_path()),
                                    ec.message());
        } else {
          work->downloaded = Updater::Downloaded::kDone;
        }
      }
      std::filesystem::remove_all(part.parent_path(), ec);
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
  if (!work || !updater_ || quitting_) return;
  const Updater::Clock::time_point now = Updater::Clock::now();
  std::optional<Notice> notice;
  // For an install the user asked for, and waits on.
  std::string error;
  switch (work->job) {
    case Updater::Job::kCheck:
      if (work->latest) {
        Log(std::format("The latest release is {} {}.", kDisplayName, work->latest->ToString()));
      } else {
        Log(std::format("Couldn't check for a new version: {}", work->error));
      }
      notice = updater_->Checked(now, work->latest);
      break;
    case Updater::Job::kDownload:
      if (work->downloaded == Updater::Downloaded::kDone) {
        Log(std::format("Downloaded {}, signed as {}'s releases are.", ToUtf8(InstallerFile(*updater_->latest())),
                        kDisplayName));
      } else {
        Log(std::format("Couldn't download the new version: {}", work->error));
      }
      notice = updater_->DownloadDone(now, work->downloaded);
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
        notice = updater_->NotVerified(now, lost);
        if (asked) {
          error = std::format("Couldn't update to {} {}. {}\n\n{}", kDisplayName, version.ToString(),
                              // The menu offers its page then, as a link
                              // here would be cut short.
                              lost ? std::format("Get {} {}… in the menu opens its page, to download it yourself.",
                                                 kDisplayName, version.ToString())
                                   : std::string("Try again later."),
                              work->verified.error);
          // The error says it.
          notice.reset();
        }
        break;
      }
      // What the user asked for happens now. By itself, it waits for a moment
      // when knobs can restart, which may have passed while the signature was
      // checked, and only while it still installs by itself.
      if (!asked && updater_->Due(now, CanRestart()) != Updater::Job::kInstall) break;
      Log(std::format("Updating to {} {}.", kDisplayName, version.ToString()));
      updater_->Installing();
      Save();
      if (const Status started = options_.updates.install(file); !started) {
        Log(std::format("Couldn't start the installer: {}", started.error()));
        notice = UpdatedNotice(updater_->settings(), options_.updates.current);
        break;
      }
      // The installer closes this copy too, once it's ready to: going now
      // spares it the wait.
      Quit();
      return;
    }
    case Updater::Job::kNone:
      break;
  }
  Save();
  if (notice) Notify(notifier_.Say(std::move(*notice), Quiet()));
  // An update at start that didn't install: knobs starts as usual.
  ReleaseCore();
  if (!error.empty()) ShowError(error);
  RunUpdates();
}

void TrayApp::SetUpdates(bool check, bool install) {
  const UpdateSettings before = updater_->settings();
  updater_->Set(check, install);
  const UpdateSettings& after = updater_->settings();
  if (after.check != before.check) {
    Log(after.check ? "Checking for new versions once a day." : "No longer checking for new versions.");
  }
  if (after.install != before.install) {
    Log(after.install ? "Installing new versions by itself." : "No longer installing new versions by itself.");
  }
  Save();
  RunUpdates();
}

void TrayApp::InstallUpdate() {
  // A check that ended while the menu was open may have found a newer
  // version, which isn't downloaded yet: the menu offers that one now.
  const auto offer = updater_ ? updater_->offer() : std::nullopt;
  if (!offer || !offer->install) return;
  if (update_thread_.joinable()) {
    install_asked_ = offer->version;
  } else {
    StartUpdateJob(Updater::Job::kInstall, true);
  }
}

void TrayApp::StopUpdates() {
  KillTimer(window_, kUpdateTimer);
  if (!update_thread_.joinable()) return;
  update_work_->cancel.Stop();
  if (update_work_->done.try_acquire_for(kUpdateStopWait)) {
    update_thread_.join();
  } else {
    // It keeps its own share of the work, and its post to a window that's
    // gone does nothing.
    update_thread_.detach();
  }
}

UpdateMenu TrayApp::MenuUpdates() const {
  if (!updater_) return {};
  const UpdateSettings& updates = updater_->settings();
  UpdateMenu menu{.offered = true,
                  .check = updates.check,
                  .install = updates.install,
                  .can_install = updater_->can_install()};
  if (const auto offer = updater_->offer()) {
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
