// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "core/state.h"
#include "tray/first_run.h"
#include "tray/menu_theme.h"
#include "tray/setup_scene.h"
#include "tray/setup_theme.h"
#include "tray/setup_uia.h"
#include "util/result.h"

// The setup window (docs/design.md, The setup window): a window of knobs's
// own that shows FirstRun's pages, drawn with Direct2D and DirectWrite, and
// moves between them as the user clicks and as the core's state changes.
namespace knobs::tray {

// What the first run needs from the tray. Called on the tray's thread.
class FirstRunHost {
 public:
  virtual ~FirstRunHost() = default;
  // The core's latest state.
  virtual const core::Snapshot& snapshot() const = 0;
  virtual void ApplySettings(const core::Settings& settings) = 0;
  virtual void Reimport() = 0;
  // Starts a new copy and quits. Returns false if it couldn't, once it has
  // said why: the tray runs on.
  virtual bool Restart() = 0;
  // Starts OBS from its install (OpenObs).
  virtual Status StartObs(const std::filesystem::path& install_root) = 0;
  // Done was clicked, with the last page's check boxes as `finish`.
  virtual void FinishFirstRun(const FinishChoices& finish) = 0;
  virtual void SaveFirstRun(const FirstRunProgress& progress) = 0;
  // A page waits for Windows to restart: open knobs again after it. Off: it
  // no longer does, as when the cable appeared without one.
  virtual void ResumeAfterRestart(bool on) = 0;
};

// The window's class, which the dev tools find it by.
inline constexpr wchar_t kSetupWindowClass[] = L"knobs.Setup";
// Clicks what has this id, as the dev tools do: a button, door, choice or
// check box (a FirstRunId), in wParam.
inline constexpr UINT kSetupClickMessage = WM_APP + 0x40;

class SetupWindow final : private SetupUiaHost {
 public:
  // `theme` is the tray's: kSystem follows Windows' app mode. For the dev
  // tools' pictures, `zoom` takes the place of Windows' text size, and
  // `walk_at` holds a walkthrough still at that moment, in seconds.
  SetupWindow(FirstRunHost& host, HINSTANCE instance, FirstRun first_run, MenuTheme theme,
              std::optional<float> zoom = std::nullopt, std::optional<float> walk_at = std::nullopt);
  ~SetupWindow() override;
  SetupWindow(const SetupWindow&) = delete;
  SetupWindow& operator=(const SetupWindow&) = delete;

  // Shows the window, and returns once it's closed. Snapshots keep arriving
  // meanwhile: the tray's messages are pumped by the window's loop.
  void Show();
  // The host's snapshot has moved on from `before`.
  void Changed(const core::Snapshot& before);
  // Null until it's shown, and after.
  HWND window() const { return window_; }

 private:
  using Clock = std::chrono::steady_clock;

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
  bool Create();
  void ApplyTheme();
  bool MakeTarget();
  // Draws what `dirty` covers, in client pixels.
  void Paint(const RECT& dirty);
  // Asks for the whole window to be drawn again, while it's there.
  void Redraw();

  // Shows the page for the host's snapshot, if it's changed.
  void Render();
  void ShowView(PageView view);
  void Layout();
  // Something the user did to an element.
  void Activate(int element);
  // A button or door, as FirstRunDialog's buttons were.
  void Click(int id);
  void Apply(const core::Settings& settings);
  void Save();
  void ShowError(const std::string& text);
  void Close();
  // The close button, Esc or Alt+F4: on the last page, as Done.
  void CloseAsked();

  // Keyboard and focus.
  void OnKey(WPARAM key, bool repeat);
  // A letter typed, with Alt or without: clicks what has it as its access
  // key. Returns whether anything did.
  bool OnAccessKey(wchar_t letter);
  // Moves the focus to `element`, and scrolls it into view unless the
  // mouse is on it.
  void FocusOn(int element, bool scroll = true);
  void MoveFocus(int step);
  void MoveChoice(int step);
  void ScrollTo(float scroll);
  void ScrollIntoView(int element);
  D2D1_POINT_2F ToDips(LPARAM lparam) const;

  // Motion.
  void StartMotion(bool first, bool new_page);
  void Step();
  // A page's walkthrough: from the start, playing unless Windows' animations
  // are off; a frame of it; play or pause; the moment it shows; its
  // screenshots, decoded before it needs them; and whether the user can
  // see it.
  void StartWalk();
  void WalkStep();
  void SetWalkPlaying(bool playing);
  // The play and pause's state and name, as walk_playing_ has it.
  void ShowWalkPlaying();
  void SetWalkTime(float time);
  void PreloadWalk();
  bool WalkSeen() const;
  // Asks for `box`, in client DIPs, and `margin` DIPs round it, to be drawn
  // again.
  void InvalidateDips(const D2D1_RECT_F& box, float margin);

  ID2D1Bitmap* Picture(const std::string& name);

  // SetupUiaHost.
  HWND UiaWindow() const override { return window_; }
  const Scene& UiaScene() const override { return scene_; }
  float UiaScroll() const override { return scroll_; }
  int UiaFocus() const override { return focus_; }
  void UiaFocusOn(int element) override;
  // After the provider's call has returned: a click may open a dialog.
  void UiaActivate(int element) override;
  RECT UiaScreenRect(const D2D1_RECT_F& box) const override;
  D2D1_POINT_2F UiaClientPoint(POINT screen) const override;

  FirstRunHost& host_;
  HINSTANCE instance_;
  FirstRun first_run_;
  FirstRunProgress saved_;
  MenuTheme theme_;
  std::optional<float> zoom_asked_;
  std::optional<float> walk_at_;
  HWND window_ = nullptr;
  HICON icon_large_ = nullptr;
  HICON icon_small_ = nullptr;
  // Which ground each icon was loaded for: 1 dark, 0 light, -1 not loaded.
  int icon_large_dark_ = -1;
  int icon_small_dark_ = -1;
  std::optional<WPARAM> quit_;  // WM_QUIT came while the window was open.

  // What's shown.
  PageView view_;
  int choice_ = 0;
  CheckStates checks_;
  bool more_open_ = false;  // The page's dropdown of more choices.
  Scene scene_;
  // Goes up when a layout changes the scene's parts, so an activation posted
  // for one layout can't land on another's.
  uintptr_t layout_generation_ = 0;
  SetupPalette palette_;
  // Settings applied, until the core's snapshots carry them.
  std::optional<core::Settings> pending_;
  // In a button's handler, which may run a dialog of its own: changes wait
  // until it returns.
  bool in_button_ = false;
  bool render_due_ = false;
  std::optional<core::Snapshot> changed_from_;

  // Drawing.
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
  ScenePainter painter_;
  std::map<std::string, Microsoft::WRL::ComPtr<ID2D1Bitmap>> pictures_;
  UINT dpi_ = 96;
  float zoom_ = 1;  // Windows' text size.
  float Scale() const { return static_cast<float>(dpi_) / 96 * zoom_; }

  // Using it.
  int hover_ = -1;
  int pressed_ = -1;
  int focus_ = -1;
  bool show_focus_ = false;
  // The control Space went down on, which it clicks when it comes up.
  int space_down_ = -1;
  // Access keys show while Alt is down, until the mouse is used, or always if
  // Windows is set to underline them.
  bool show_access_keys_ = false;
  bool tracking_mouse_ = false;
  float scroll_ = 0;

  // Motion.
  bool animate_ = true;
  Clock::time_point motion_start_;
  Clock::time_point breath_start_;  // The waiting dot's breath counts from here.
  bool moving_ = false;
  float caret_from_ = 0;
  std::optional<float> caret_to_;
  std::array<float, kStageCount - 1> wire_from_{}, wire_to_{};
  float arrive_ = 1;
  SceneState shown_;
  float walk_time_ = 0;  // Seconds into the page's walkthrough.
  std::optional<WalkFrame> walk_frame_;  // What it shows then.
  bool walk_playing_ = false;
  bool walk_unseen_ = false;  // Playing, but waiting for the user to see it.
  Clock::time_point walk_tick_;

  std::unique_ptr<SetupUia> uia_;
};

}  // namespace knobs::tray
