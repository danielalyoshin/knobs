// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "tray/setup_scene.h"

// UI Automation for the setup window, which draws its own controls: a
// provider for the window, and one for each element of the page's scene,
// with the patterns screen readers use to work its controls.
namespace knobs::tray {

// What the providers ask the window. Called on the window's thread: the
// providers use COM's threading, and the thread is a single-threaded
// apartment.
class SetupUiaHost {
 public:
  virtual ~SetupUiaHost() = default;
  virtual HWND UiaWindow() const = 0;
  virtual const Scene& UiaScene() const = 0;
  virtual float UiaScroll() const = 0;
  // The element with the keyboard focus, or -1.
  virtual int UiaFocus() const = 0;
  virtual void UiaFocusOn(int element) = 0;
  // Clicks a button, door or link, selects a radio, toggles the check box, or
  // opens or closes a dropdown.
  virtual void UiaActivate(int element) = 0;
  // A box in client DIPs, in screen pixels.
  virtual RECT UiaScreenRect(const D2D1_RECT_F& box) const = 0;
  // A point in screen pixels, in client DIPs.
  virtual D2D1_POINT_2F UiaClientPoint(POINT screen) const = 0;
};

struct UiaBridge;

class SetupUia {
 public:
  explicit SetupUia(SetupUiaHost& host);
  ~SetupUia();
  SetupUia(const SetupUia&) = delete;
  SetupUia& operator=(const SetupUia&) = delete;

  // WM_GETOBJECT.
  LRESULT OnGetObject(WPARAM wparam, LPARAM lparam);
  // The scene was laid out again. Providers go stale only if its parts did
  // (`rebuilt`), so a screen reader keeps its place on the same page. A new
  // page also says its heading.
  void SceneChanged(bool new_page, bool rebuilt, const std::wstring& heading);
  void FocusChanged(int element);
  void Toggled(int element, bool was_on);
  void Selected(int element);
  // A dropdown opened, or closed.
  void Expanded(int element, bool open);
  // What a waiting page waits for changed.
  void StatusChanged(int element);
  // The window is going: providers stop answering.
  void Detach();

 private:
  std::shared_ptr<UiaBridge> bridge_;
};

}  // namespace knobs::tray
