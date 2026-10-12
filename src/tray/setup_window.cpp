// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/setup_window.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <numbers>
#include <tuple>
#include <utility>
#include <vector>

#include "app_info.h"
#include "import/obs_config.h"
#include "runtime/obs_install.h"
#include "tray/message_dialog.h"
#include "tray/setup_graphics.h"
#include "util/pick_folder.h"
#include "util/win_strings.h"

namespace knobs::tray {
namespace {

constexpr UINT_PTR kRenderTimer = 1;  // Catches a render put off by a button's handler.
// An activation from UI Automation, carried out once its call has returned:
// wParam is the element, lParam the layout's generation.
constexpr UINT kActivateMessage = WM_APP + 0x41;
constexpr UINT_PTR kMotionTimer = 2;
constexpr UINT_PTR kWalkTimer = 3;  // A walkthrough's frames.
constexpr UINT kWalkUnseenMs = 250;  // How often an unseen walkthrough looks again.
constexpr UINT kRenderMs = 200;
constexpr UINT kFrameMs = 16;
// The motion (docs/design.md, The setup window): 320 ms for structure,
// 200 ms for a page coming in, and the waiting dot's breath.
constexpr float kStructureMs = 320;
constexpr float kArriveMs = 200;
constexpr float kBreathMs = 1600;
constexpr UINT kBreathFrameMs = 50;  // The dot needs no more.
// DWM's attributes, by number: older SDKs lack the names.
constexpr DWORD kDarkMode = 20;      // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kCaptionColor = 35;  // DWMWA_CAPTION_COLOR, Windows 11
constexpr DWORD kCloaked = 14;       // DWMWA_CLOAKED: on another desktop

// cubic-bezier(0.2, 0, 0, 1), the site's one curve, solved for x.
float Ease(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const auto bezier = [](float u, float p1, float p2) {
    const float v = 1 - u;
    return 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u;
  };
  float lo = 0, hi = 1, u = t;
  for (int i = 0; i < 24; ++i) {
    u = (lo + hi) / 2;
    if (bezier(u, 0.2f, 0.0f) < t) lo = u; else hi = u;
  }
  return bezier(u, 0.0f, 1.0f);
}

// Windows' text size (Settings > Accessibility > Text size), from 1 to 2.25.
float TextScale() {
  DWORD percent = 100;
  DWORD size = sizeof(percent);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD,
                   nullptr, &percent, &size) != ERROR_SUCCESS) {
    return 1;
  }
  return std::clamp(static_cast<float>(percent) / 100, 1.0f, 2.25f);
}

// A picture's resource name: "obs-main" is OBS_MAIN.
std::wstring ResourceName(const std::string& name) {
  std::wstring resource = FromUtf8(name);
  for (wchar_t& c : resource) c = c == L'-' ? L'_' : static_cast<wchar_t>(towupper(c));
  return resource;
}

// The parts of a scene, as screen readers know them.
std::vector<std::tuple<Role, int, int, std::wstring>> Parts(const Scene& scene) {
  std::vector<std::tuple<Role, int, int, std::wstring>> parts;
  for (const Element& element : scene.elements) parts.emplace_back(element.role, element.id, element.parent, element.name);
  return parts;
}

// Links open in the browser, and nothing else does.
bool IsWebLink(std::string_view url) { return url.starts_with("https://") || url.starts_with("http://"); }

bool IsControl(Role role) {
  return role == Role::kButton || role == Role::kDoor || role == Role::kRadio || role == Role::kCheckBox ||
         role == Role::kToggle || role == Role::kLink;
}

}  // namespace

SetupWindow::SetupWindow(FirstRunHost& host, HINSTANCE instance, FirstRun first_run, MenuTheme theme,
                         std::optional<float> zoom, std::optional<float> walk_at)
    : host_(host),
      instance_(instance),
      first_run_(std::move(first_run)),
      saved_(first_run_.progress()),
      theme_(theme),
      zoom_asked_(zoom),
      walk_at_(walk_at) {
  icon_large_ = LoadAppIcon(instance_, LIM_LARGE);
  icon_small_ = LoadAppIcon(instance_, LIM_SMALL);
  uia_ = std::make_unique<SetupUia>(static_cast<SetupUiaHost&>(*this));
}

SetupWindow::~SetupWindow() {
  if (window_) DestroyWindow(window_);
  uia_->Detach();
  if (icon_large_) DestroyIcon(icon_large_);
  if (icon_small_) DestroyIcon(icon_small_);
}

void SetupWindow::Show() {
  // What the first run starts from, such as a page asked for.
  host_.SaveFirstRun(saved_);
  if (!Create()) {
    ShowError("The setup window couldn't open: Direct2D or DirectWrite isn't working on this PC.");
    return;
  }
  // Snapshots that came before the window did.
  Render();
  ShowWindow(window_, SW_SHOWNORMAL);
  SetForegroundWindow(window_);
  MSG message;
  while (window_) {
    const BOOL got = GetMessageW(&message, nullptr, 0, 0);
    if (got == -1) break;
    if (got == 0) {
      // The tray quits: so does the window, and the tray's loop hears it too.
      quit_ = message.wParam;
      break;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (window_) DestroyWindow(window_);
  if (quit_) PostQuitMessage(static_cast<int>(*quit_));
}

void SetupWindow::Changed(const core::Snapshot& before) {
  if (in_button_) {
    if (!changed_from_) changed_from_ = before;
    render_due_ = true;
    return;
  }
  const FirstRunAction action = first_run_.Changed(before, host_.snapshot());
  Save();
  if (action.kind == FirstRunAction::Kind::kApply) Apply(action.settings);
  Render();
}

// --- The window ----------------------------------------------------------------------

bool SetupWindow::Create() {
  if (!D2DFactory() || !DWriteFactory()) return false;
  static const bool registered = [this] {
    WNDCLASSEXW window_class = {sizeof(window_class)};
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Icons of the class's own, for as long as the process lasts: each
    // window's are destroyed with it.
    window_class.hIcon = LoadAppIcon(instance_, LIM_LARGE);
    window_class.hIconSm = LoadAppIcon(instance_, LIM_SMALL);
    window_class.lpszClassName = kSetupWindowClass;
    return RegisterClassExW(&window_class) != 0;
  }();
  if (!registered) return false;

  // On the monitor the pointer is on, as the tray icon or a notification
  // that opened it is, at that monitor's scale and Windows' text size.
  POINT cursor = {};
  GetCursorPos(&cursor);
  const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info = {sizeof(info)};
  GetMonitorInfoW(monitor, &info);
  UINT dpi_x = 96, dpi_y = 96;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) dpi_x = 96;
  dpi_ = dpi_x;
  constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  const auto frame = [&](float zoom) {
    const float scale = static_cast<float>(dpi_) / 96 * zoom;
    RECT rect = {0, 0, static_cast<LONG>(std::lround(kSetupWidth * scale)),
                 static_cast<LONG>(std::lround(kSetupHeight * scale))};
    AdjustWindowRectExForDpi(&rect, kStyle, FALSE, 0, dpi_);
    return rect;
  };
  // The text size grows the whole window, as far as the screen allows.
  const RECT work = info.rcWork;
  zoom_ = zoom_asked_.value_or(TextScale());
  RECT rect = frame(zoom_);
  while (zoom_ > 1 && (rect.right - rect.left > work.right - work.left || rect.bottom - rect.top > work.bottom - work.top)) {
    zoom_ = std::max(1.0f, zoom_ - 0.05f);
    rect = frame(zoom_);
  }
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  const int x = work.left + std::max(0L, (work.right - work.left - width) / 2);
  const int y = work.top + std::max(0L, (work.bottom - work.top - height) / 2);
  const std::wstring title = std::format(L"{} setup", kDisplayNameW);
  window_ = CreateWindowExW(0, kSetupWindowClass, title.c_str(), kStyle, x, y, width, height, nullptr, nullptr,
                            instance_, this);
  if (!window_) return false;
  // Created where it'll show, it has that monitor's scale already.
  dpi_ = GetDpiForWindow(window_);
  SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_large_));
  SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small_));
  ApplyTheme();
  if (!MakeTarget()) {
    DestroyWindow(window_);
    return false;
  }
  SetTimer(window_, kRenderTimer, kRenderMs, nullptr);
  breath_start_ = Clock::now();
  return true;
}

void SetupWindow::ApplyTheme() {
  const bool animated = animate_;
  animate_ = AnimationsOn();
  // Turned off, they stop the walkthrough too, once: it can be played again,
  // and keeps playing through other changes of setting.
  if (animated && !animate_) SetWalkPlaying(false);
  palette_ = MakeSetupPalette(CurrentSetupMode(theme_));
  if (!window_) return;
  const BOOL dark = palette_.mode == SetupMode::kDark;
  DwmSetWindowAttribute(window_, kDarkMode, &dark, sizeof(dark));
  // Windows 11 colours the title bar in the window's own ground; earlier
  // versions keep theirs.
  const COLORREF caption = palette_.caption;
  DwmSetWindowAttribute(window_, kCaptionColor, &caption, sizeof(caption));
  Redraw();
}

bool SetupWindow::MakeTarget() {
  if (target_) return true;
  RECT client = {};
  GetClientRect(window_, &client);
  const float dpi = 96 * Scale();
  const HRESULT made = D2DFactory()->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi),
      D2D1::HwndRenderTargetProperties(window_,
                                       D2D1::SizeU(static_cast<UINT32>(client.right), static_cast<UINT32>(client.bottom)),
                                       D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS),
      &target_);
  if (FAILED(made)) return false;
  painter_.Reset();
  pictures_.clear();
  PreloadWalk();
  return true;
}

ID2D1Bitmap* SetupWindow::Picture(const std::string& name) {
  if (!target_) return nullptr;
  auto found = pictures_.find(name);
  if (found == pictures_.end()) {
    found = pictures_.emplace(name, LoadPictureResource(target_.Get(), instance_, ResourceName(name))).first;
  }
  return found->second.Get();
}

void SetupWindow::Redraw() {
  if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void SetupWindow::Paint(const RECT& dirty) {
  if (!MakeTarget()) return;
  shown_.hover = hover_;
  shown_.pressed = pressed_;
  shown_.focus = focus_;
  shown_.show_focus = show_focus_ && GetFocus() == window_;
  BOOL always = FALSE;
  SystemParametersInfoW(SPI_GETKEYBOARDCUES, 0, &always, 0);
  shown_.show_access_keys = always || show_access_keys_;
  shown_.scroll = scroll_;
  shown_.walk = walk_frame_;
  shown_.walk_playing = walk_playing_;
  target_->BeginDraw();
  // Only what needs it: the waiting dot asks for no more than its own box.
  const float scale = Scale();
  target_->PushAxisAlignedClip({dirty.left / scale, dirty.top / scale, dirty.right / scale, dirty.bottom / scale},
                               D2D1_ANTIALIAS_MODE_ALIASED);
  painter_.Paint(target_.Get(), scene_, palette_, shown_, 1 / scale,
                 [this](const std::string& name) { return Picture(name); });
  target_->PopAxisAlignedClip();
  if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
    target_.Reset();
    painter_.Reset();
    pictures_.clear();
    Redraw();
  }
}

LRESULT CALLBACK SetupWindow::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto* self = static_cast<SetupWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->window_ = window;
  }
  auto* self = reinterpret_cast<SetupWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!self) return DefWindowProcW(window, message, wparam, lparam);
  if (message == WM_NCDESTROY) {
    self->window_ = nullptr;
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return DefWindowProcW(window, message, wparam, lparam);
  }
  return self->HandleMessage(message, wparam, lparam);
}

LRESULT SetupWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT paint;
      BeginPaint(window_, &paint);
      Paint(paint.rcPaint);
      EndPaint(window_, &paint);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (target_) target_->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
      return 0;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wparam);
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                   suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      if (target_) target_->SetDpi(96 * Scale(), 96 * Scale());
      Redraw();
      return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
      ApplyTheme();
      break;
    case WM_MOUSEMOVE: {
      if (!tracking_mouse_) {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window_, 0};
        tracking_mouse_ = TrackMouseEvent(&track) != FALSE;
      }
      const int hover = scene_.HitTest(ToDips(lparam), scroll_);
      if (hover != hover_) {
        hover_ = hover;
        Redraw();
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking_mouse_ = false;
      if (hover_ >= 0) {
        hover_ = -1;
        Redraw();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        const bool link = hover_ >= 0 && scene_.elements[static_cast<size_t>(hover_)].role == Role::kLink;
        SetCursor(LoadCursorW(nullptr, link ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_LBUTTONDOWN: {
      const int hit = scene_.HitTest(ToDips(lparam), scroll_);
      show_focus_ = false;
      show_access_keys_ = false;
      if (hit >= 0 && scene_.elements[static_cast<size_t>(hit)].enabled) {
        pressed_ = hit;
        SetCapture(window_);
        FocusOn(hit, false);
      }
      Redraw();
      return 0;
    }
    case WM_LBUTTONUP: {
      const int pressed = pressed_;
      pressed_ = -1;
      if (GetCapture() == window_) ReleaseCapture();
      if (pressed >= 0 && scene_.HitTest(ToDips(lparam), scroll_) == pressed) Activate(pressed);
      Redraw();
      return 0;
    }
    case WM_CAPTURECHANGED:
      pressed_ = -1;
      Redraw();
      return 0;
    case WM_MOUSEWHEEL:
      ScrollTo(scroll_ - static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 48);
      return 0;
    case WM_KEYDOWN:
      OnKey(wparam, (lparam & (1 << 30)) != 0);
      return 0;
    case WM_KEYUP:
      // Space clicks as it comes up, on the control it went down on, as a
      // Windows button does.
      if (wparam == VK_SPACE && space_down_ >= 0) {
        const int down = space_down_;
        space_down_ = -1;
        pressed_ = -1;
        if (down == focus_) Activate(down);
        Redraw();
      }
      return 0;
    case WM_SYSKEYDOWN:
      if (wparam == VK_MENU && !show_access_keys_) {
        show_access_keys_ = true;
        Redraw();
      }
      break;
    case WM_SYSCHAR:
      // Alt and a letter, once per press. Anything else, such as Alt+Space,
      // is Windows'.
      if ((lparam & (1 << 30)) != 0 && iswalpha(static_cast<wint_t>(wparam))) return 0;
      if (OnAccessKey(static_cast<wchar_t>(wparam))) return 0;
      break;
    case WM_CHAR:
      // A letter alone does it too, as in Windows' dialogs: nothing here
      // takes typing.
      if ((lparam & (1 << 30)) == 0 && iswalpha(static_cast<wint_t>(wparam))) {
        OnAccessKey(static_cast<wchar_t>(wparam));
      }
      return 0;
    case WM_SETFOCUS:
      uia_->FocusChanged(focus_);
      Redraw();
      return 0;
    case WM_KILLFOCUS:
      Redraw();
      return 0;
    case WM_GETOBJECT:
      if (const LRESULT result = uia_->OnGetObject(wparam, lparam)) return result;
      break;
    case WM_TIMER:
      if (wparam == kRenderTimer && render_due_ && !in_button_) Render();
      if (wparam == kMotionTimer) Step();
      if (wparam == kWalkTimer) WalkStep();
      return 0;
    case WM_CLOSE:
      // The close button, Esc and Alt+F4: knobs stays in the tray, and the
      // first run resumes here next time.
      if (!in_button_) Close();
      return 0;
    case WM_DESTROY:
      KillTimer(window_, kRenderTimer);
      KillTimer(window_, kMotionTimer);
      KillTimer(window_, kWalkTimer);
      uia_->Detach();
      target_.Reset();
      painter_.Reset();
      pictures_.clear();
      return 0;
    case kActivateMessage:
      if (static_cast<uintptr_t>(lparam) == layout_generation_) Activate(static_cast<int>(wparam));
      return 0;
    case kSetupClickMessage:
      for (size_t i = 0; i < scene_.elements.size(); ++i) {
        const Element& element = scene_.elements[i];
        if (element.id == static_cast<int>(wparam) && IsControl(element.role) && element.enabled) {
          Activate(static_cast<int>(i));
          break;
        }
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

D2D1_POINT_2F SetupWindow::ToDips(LPARAM lparam) const {
  return {static_cast<float>(GET_X_LPARAM(lparam)) / Scale(), static_cast<float>(GET_Y_LPARAM(lparam)) / Scale()};
}

RECT SetupWindow::UiaScreenRect(const D2D1_RECT_F& box) const {
  POINT origin = {0, 0};
  ClientToScreen(window_, &origin);
  return {origin.x + static_cast<LONG>(std::floor(box.left * Scale())),
          origin.y + static_cast<LONG>(std::floor(box.top * Scale())),
          origin.x + static_cast<LONG>(std::ceil(box.right * Scale())),
          origin.y + static_cast<LONG>(std::ceil(box.bottom * Scale()))};
}

D2D1_POINT_2F SetupWindow::UiaClientPoint(POINT screen) const {
  ScreenToClient(window_, &screen);
  return {static_cast<float>(screen.x) / Scale(), static_cast<float>(screen.y) / Scale()};
}

void SetupWindow::UiaActivate(int element) {
  if (window_) PostMessageW(window_, kActivateMessage, static_cast<WPARAM>(element), layout_generation_);
}

void SetupWindow::UiaFocusOn(int element) {
  show_focus_ = true;
  FocusOn(element);
}

// --- Pages ---------------------------------------------------------------------------

void SetupWindow::Render() {
  if (!window_ || in_button_) {
    render_due_ = true;
    return;
  }
  render_due_ = false;
  const core::Snapshot& snapshot = host_.snapshot();
  PageView view;
  if (pending_ && snapshot.settings != *pending_) {
    // Until the core has the new settings, the page is as it was, with
    // nothing to click, and says so.
    view = view_;
    for (PageButton& button : view.buttons) button.enabled = false;
    for (PageButton& choice : view.choices) choice.enabled = false;
    view.waiting = true;
    if (view.status.empty()) view.status = "Applying your choice…";
  } else {
    pending_.reset();
    view = first_run_.View(snapshot, 0);
    if (view.page == view_.page) view = first_run_.View(snapshot, choice_, checked_);
  }
  if (view == view_ && !scene_.elements.empty()) return;
  ShowView(std::move(view));
}

void SetupWindow::ShowView(PageView view) {
  const bool new_page = scene_.elements.empty() || view.page != view_.page;
  // What has the focus, to find it again on the same page.
  std::optional<std::pair<Role, int>> focused;
  std::wstring focused_name;
  if (focus_ >= 0 && static_cast<size_t>(focus_) < scene_.elements.size()) {
    const Element& element = scene_.elements[static_cast<size_t>(focus_)];
    focused = {element.role, element.id};
    focused_name = element.name;
  }
  const std::wstring status_before =
      scene_.status >= 0 ? scene_.elements[static_cast<size_t>(scene_.status)].name : L"";
  const bool first = scene_.elements.empty();
  const auto parts_before = Parts(scene_);
  const Walkthrough* walk_before = scene_.walkthrough;
  view_ = std::move(view);
  choice_ = view_.default_choice;
  if (!view_.check.empty()) checked_ = view_.checked;
  Layout();
  const bool rebuilt = Parts(scene_) != parts_before;
  if (rebuilt) ++layout_generation_;
  if (new_page) scroll_ = 0;
  scroll_ = std::clamp(scroll_, 0.0f, scene_.MaxScroll());
  hover_ = -1;
  pressed_ = -1;
  focus_ = -1;
  if (!new_page && focused) {
    for (size_t i = 0; i < scene_.elements.size(); ++i) {
      const Element& element = scene_.elements[i];
      if (element.role == focused->first && element.id == focused->second &&
          (element.id != 0 || element.name == focused_name)) {
        focus_ = static_cast<int>(i);
        break;
      }
    }
  }
  if (focus_ < 0 || !scene_.elements[static_cast<size_t>(focus_)].enabled) focus_ = scene_.default_element;
  if (focus_ < 0) {
    for (size_t i = 0; i < scene_.elements.size(); ++i) {
      if (scene_.elements[i].focusable && scene_.elements[i].enabled) {
        focus_ = static_cast<int>(i);
        break;
      }
    }
  }
  StartMotion(first, new_page);
  uia_->SceneChanged(new_page, rebuilt, FromUtf8(view_.instruction));
  // After the scene's providers are made: the walkthrough's toggle is one.
  if (new_page || scene_.walkthrough != walk_before) StartWalk();
  if (GetFocus() == window_) uia_->FocusChanged(focus_);
  if (scene_.status >= 0 && scene_.elements[static_cast<size_t>(scene_.status)].name != status_before) {
    uia_->StatusChanged(scene_.status);
  }
  Redraw();
}

void SetupWindow::Layout() {
  scene_ = LayoutScene(view_, choice_, checked_);
  if (scene_.walk_toggle >= 0) scene_.elements[static_cast<size_t>(scene_.walk_toggle)].selected = walk_playing_;
}

void SetupWindow::Activate(int index) {
  if (index < 0 || static_cast<size_t>(index) >= scene_.elements.size()) return;
  const Element element = scene_.elements[static_cast<size_t>(index)];
  if (!element.enabled) return;
  switch (element.role) {
    case Role::kButton:
    case Role::kDoor:
      Click(element.id);
      break;
    case Role::kRadio:
      if (!element.selected) {
        choice_ = element.id;
        Render();
        // The chosen radio has the focus, as Windows' radios do.
        for (size_t i = 0; i < scene_.elements.size(); ++i) {
          if (scene_.elements[i].role == Role::kRadio && scene_.elements[i].id == element.id) {
            FocusOn(static_cast<int>(i));
            uia_->Selected(static_cast<int>(i));
          }
        }
      }
      break;
    case Role::kCheckBox:
      checked_ = !checked_;
      Render();
      for (size_t i = 0; i < scene_.elements.size(); ++i) {
        if (scene_.elements[i].role == Role::kCheckBox) uia_->Toggled(static_cast<int>(i), !checked_);
      }
      break;
    case Role::kToggle:
      SetWalkPlaying(!walk_playing_);
      break;
    case Role::kLink:
      if (IsWebLink(element.url)) {
        ShellExecuteW(window_, L"open", FromUtf8(element.url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
      break;
    default:
      break;
  }
  Redraw();
}

void SetupWindow::Click(int id) {
  // One click at a time: a handler's own dialog (an error, a folder picker)
  // runs a message loop that could hand the window another.
  if (in_button_) return;
  in_button_ = true;
  const core::Snapshot snapshot = host_.snapshot();
  const FirstRunAction action = first_run_.Click(id, choice_, checked_, snapshot);
  Save();
  bool close = false;
  switch (action.kind) {
    case FirstRunAction::Kind::kNone:
      break;
    case FirstRunAction::Kind::kApply:
      Apply(action.settings);
      break;
    case FirstRunAction::Kind::kReimport:
      host_.Reimport();
      break;
    case FirstRunAction::Kind::kOpenObs:
      if (const Status opened = host_.StartObs(snapshot.obs.root); !opened) ShowError(opened.error());
      break;
    case FirstRunAction::Kind::kChooseObs:
      if (const auto folder = runtime::PickObsInstallFolder(window_)) {
        core::Settings settings = snapshot.settings;
        settings.obs_dir = *folder;
        Apply(settings);
      }
      break;
    case FirstRunAction::Kind::kChooseObsSettings:
      if (const auto folder = PickFolder(window_, L"Choose OBS's settings folder")) {
        core::Settings settings = snapshot.settings;
        settings.obs_config = import::SettingsFolderFor(*folder);
        Apply(settings);
      }
      break;
    case FirstRunAction::Kind::kRestart:
      // The tray quits, which ends the window's loop too. If the new copy
      // couldn't start, the tray said so, and the page stays.
      close = host_.Restart();
      break;
    case FirstRunAction::Kind::kFinish:
      host_.FinishFirstRun(action.start_with_windows);
      close = true;
      break;
  }
  in_button_ = false;
  if (close) {
    Close();
    return;
  }
  if (changed_from_) {
    const core::Snapshot before = std::move(*changed_from_);
    changed_from_.reset();
    Changed(before);
  } else {
    Render();
  }
}

void SetupWindow::Apply(const core::Settings& settings) {
  host_.ApplySettings(settings);
  pending_ = settings;
}

void SetupWindow::Save() {
  if (first_run_.progress() == saved_) return;
  saved_ = first_run_.progress();
  host_.SaveFirstRun(saved_);
}

void SetupWindow::ShowError(const std::string& text) {
  // Over the window, which can't be clicked meanwhile.
  ShowMessageDialog(instance_, {.owner = window_,
                                .title = std::format(L"{} setup", kDisplayNameW),
                                .icon = TD_WARNING_ICON,
                                .content = FromUtf8(text)});
}

void SetupWindow::Close() {
  if (window_) DestroyWindow(window_);
}

// --- Keyboard and focus ------------------------------------------------------------------

void SetupWindow::OnKey(WPARAM key, bool repeat) {
  const bool shift = GetKeyState(VK_SHIFT) < 0;
  const Element* focused =
      focus_ >= 0 && static_cast<size_t>(focus_) < scene_.elements.size() ? &scene_.elements[static_cast<size_t>(focus_)]
                                                                         : nullptr;
  switch (key) {
    case VK_TAB:
      show_focus_ = true;
      MoveFocus(shift ? -1 : 1);
      break;
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
      show_focus_ = true;
      if (focused && focused->role == Role::kRadio) {
        MoveChoice(key == VK_LEFT || key == VK_UP ? -1 : 1);
      } else if (key == VK_UP || key == VK_DOWN) {
        ScrollTo(scroll_ + (key == VK_UP ? -40.0f : 40.0f));
      }
      break;
    case VK_PRIOR:
    case VK_NEXT:
      ScrollTo(scroll_ + (key == VK_PRIOR ? -1 : 1) * (scene_.content.bottom - scene_.content.top - 40));
      break;
    case VK_SPACE:
      // Down: the control looks pressed. It clicks when Space comes up.
      show_focus_ = true;
      if (!repeat && focused && focused->enabled) {
        space_down_ = focus_;
        pressed_ = focus_;
        hover_ = focus_;
      }
      break;
    case VK_RETURN:
      // Once per press: held down, it doesn't click on through the pages.
      if (repeat) return;
      show_focus_ = true;
      // The control with the focus, or else the page's default button.
      if (focused && (focused->role == Role::kButton || focused->role == Role::kDoor || focused->role == Role::kLink ||
                      focused->role == Role::kToggle)) {
        Activate(focus_);
      } else if (scene_.default_element >= 0) {
        Activate(scene_.default_element);
      }
      break;
    case VK_ESCAPE:
      Close();
      return;
    default:
      return;
  }
  Redraw();
}

bool SetupWindow::OnAccessKey(wchar_t letter) {
  const wchar_t key = static_cast<wchar_t>(towupper(letter));
  for (size_t i = 0; i < scene_.elements.size(); ++i) {
    const Element& element = scene_.elements[i];
    if (element.access_key != key || !element.enabled) continue;
    FocusOn(static_cast<int>(i));
    Activate(static_cast<int>(i));
    return true;
  }
  return false;
}

void SetupWindow::FocusOn(int element, bool scroll) {
  if (element == focus_) return;
  focus_ = element;
  if (scroll) ScrollIntoView(element);
  if (GetFocus() == window_) uia_->FocusChanged(element);
  Redraw();
}

void SetupWindow::MoveFocus(int step) {
  // Every control in reading order, but only the selected radio of a group.
  std::vector<int> order;
  for (size_t i = 0; i < scene_.elements.size(); ++i) {
    const Element& element = scene_.elements[i];
    if (!element.focusable || !element.enabled) continue;
    if (element.role == Role::kRadio && !element.selected) {
      const bool any_selected = std::any_of(scene_.elements.begin(), scene_.elements.end(), [&](const Element& e) {
        return e.role == Role::kRadio && e.parent == element.parent && e.selected;
      });
      if (any_selected || (!order.empty() && scene_.elements[static_cast<size_t>(order.back())].parent == element.parent)) {
        continue;
      }
    }
    order.push_back(static_cast<int>(i));
  }
  if (order.empty()) return;
  const auto at = std::find(order.begin(), order.end(), focus_);
  int next = 0;
  if (at == order.end()) {
    next = step > 0 ? order.front() : order.back();
  } else {
    const auto size = static_cast<int>(order.size());
    next = order[static_cast<size_t>(((at - order.begin()) + step + size) % size)];
  }
  FocusOn(next);
}

void SetupWindow::MoveChoice(int step) {
  if (focus_ < 0) return;
  const int group = scene_.elements[static_cast<size_t>(focus_)].parent;
  std::vector<int> radios;
  for (size_t i = 0; i < scene_.elements.size(); ++i) {
    const Element& element = scene_.elements[i];
    if (element.role == Role::kRadio && element.parent == group && element.enabled) radios.push_back(static_cast<int>(i));
  }
  const auto at = std::find(radios.begin(), radios.end(), focus_);
  if (radios.empty() || at == radios.end()) return;
  const auto size = static_cast<int>(radios.size());
  Activate(radios[static_cast<size_t>(((at - radios.begin()) + step + size) % size)]);
}

void SetupWindow::ScrollTo(float scroll) {
  const float clamped = std::clamp(scroll, 0.0f, scene_.MaxScroll());
  if (clamped == scroll_) return;
  scroll_ = clamped;
  Redraw();
}

void SetupWindow::ScrollIntoView(int element) {
  if (element < 0) return;
  const Element& e = scene_.elements[static_cast<size_t>(element)];
  if (!e.in_panel) return;
  const float top = e.bounds.top - scene_.content.top;
  const float bottom = e.bounds.bottom - scene_.content.top;
  const float height = scene_.content.bottom - scene_.content.top;
  if (top - 8 < scroll_) {
    ScrollTo(top - 8);
  } else if (bottom + 8 > scroll_ + height) {
    ScrollTo(bottom + 8 - height);
  }
}

// --- Motion ------------------------------------------------------------------------------

void SetupWindow::StartMotion(bool first, bool new_page) {
  // Where everything is headed.
  caret_to_.reset();
  if (scene_.here) caret_to_ = StageCentre(static_cast<size_t>(*scene_.here));
  for (size_t i = 0; i + 1 < kStageCount; ++i) {
    wire_from_[i] = first ? (WireLive(scene_, i) ? 1.0f : 0.0f) : shown_.wire[i];
    wire_to_[i] = WireLive(scene_, i) ? 1.0f : 0.0f;
  }
  caret_from_ = shown_.caret_x && caret_to_ ? *shown_.caret_x : caret_to_.value_or(0);
  if (!animate_ || first) {
    shown_.caret_x = caret_to_;
    shown_.wire = wire_to_;
    shown_.arrive = 1;
    moving_ = false;
    if (scene_.waiting && animate_) SetTimer(window_, kMotionTimer, kBreathFrameMs, nullptr);
    return;
  }
  arrive_ = new_page ? 0.0f : 1.0f;
  shown_.arrive = arrive_;
  motion_start_ = Clock::now();
  moving_ = true;
  SetTimer(window_, kMotionTimer, kFrameMs, nullptr);
  Step();
}

void SetupWindow::StartWalk() {
  if (window_) KillTimer(window_, kWalkTimer);
  walk_playing_ = false;
  walk_unseen_ = false;
  walk_frame_.reset();
  if (!scene_.walkthrough) return;
  PreloadWalk();
  // Held for a picture, or still with Windows' animations off, until played.
  // The new page has told screen readers what it shows, so playing from the
  // start needs no word of its own.
  walk_playing_ = animate_ && !walk_at_;
  SetWalkTime(walk_at_ ? *walk_at_ : walk_playing_ ? 0 : scene_.walkthrough->still);
  if (scene_.walk_toggle >= 0) scene_.elements[static_cast<size_t>(scene_.walk_toggle)].selected = walk_playing_;
  if (walk_playing_ && window_) {
    walk_tick_ = Clock::now();
    SetTimer(window_, kWalkTimer, kFrameMs, nullptr);
  }
}

void SetupWindow::SetWalkPlaying(bool playing) {
  if (!scene_.walkthrough || playing == walk_playing_) return;
  walk_playing_ = playing;
  walk_unseen_ = false;
  if (scene_.walk_toggle >= 0) {
    scene_.elements[static_cast<size_t>(scene_.walk_toggle)].selected = playing;
    uia_->Toggled(scene_.walk_toggle, !playing);
  }
  if (!window_) return;
  if (playing) {
    walk_tick_ = Clock::now();
    SetTimer(window_, kWalkTimer, kFrameMs, nullptr);
  } else {
    KillTimer(window_, kWalkTimer);
  }
  Redraw();
}

void SetupWindow::SetWalkTime(float time) {
  walk_time_ = time;
  walk_frame_.reset();
  if (scene_.walkthrough) walk_frame_ = WalkAt(*scene_.walkthrough, time, kWalkFrame);
}

void SetupWindow::PreloadWalk() {
  if (!scene_.walkthrough || !target_) return;
  for (const Walkthrough::Shot& shot : scene_.walkthrough->shots) Picture(shot.picture);
}

bool SetupWindow::WalkSeen() const {
  if (!window_ || !IsWindowVisible(window_) || IsIconic(window_)) return false;
  // On another desktop, or with the screen locked or off.
  BOOL cloaked = FALSE;
  if (SUCCEEDED(DwmGetWindowAttribute(window_, kCloaked, &cloaked, sizeof(cloaked))) && cloaked) return false;
  if (target_ && (target_->CheckWindowState() & D2D1_WINDOW_STATE_OCCLUDED)) return false;
  // Behind the window the user is using, such as OBS, over all of the frame.
  const HWND front = GetForegroundWindow();
  if (!front || front == window_ || IsIconic(front)) return true;
  RECT cover = {};
  if (FAILED(DwmGetWindowAttribute(front, DWMWA_EXTENDED_FRAME_BOUNDS, &cover, sizeof(cover)))) {
    GetWindowRect(front, &cover);
  }
  const float scale = Scale();
  POINT corners[2] = {{std::lround(scene_.walk.left * scale), std::lround((scene_.walk.top - scroll_) * scale)},
                      {std::lround(scene_.walk.right * scale), std::lround((scene_.walk.bottom - scroll_) * scale)}};
  MapWindowPoints(window_, nullptr, corners, 2);
  const RECT frame = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
  RECT both = {};
  IntersectRect(&both, &frame, &cover);
  if (!EqualRect(&both, &frame)) return true;
  // Over it, not under it, as the desktop is.
  for (HWND above = GetWindow(window_, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
    if (above == front) return false;
  }
  return true;
}

void SetupWindow::InvalidateDips(const D2D1_RECT_F& box, float margin) {
  if (!window_ || box.right <= box.left || box.bottom <= box.top) return;
  const float scale = Scale();
  const RECT dirty = {static_cast<LONG>(std::floor((box.left - margin) * scale)),
                      static_cast<LONG>(std::floor((box.top - margin) * scale)),
                      static_cast<LONG>(std::ceil((box.right + margin) * scale)),
                      static_cast<LONG>(std::ceil((box.bottom + margin) * scale))};
  InvalidateRect(window_, &dirty, FALSE);
}

void SetupWindow::WalkStep() {
  const Clock::time_point now = Clock::now();
  // A long gap, such as a dialog's, doesn't skip it on.
  const float elapsed = std::min(std::chrono::duration<float>(now - walk_tick_).count(), 0.1f);
  walk_tick_ = now;
  if (!scene_.walkthrough || !walk_playing_ || !window_) return;
  // Unseen, it waits where it is, drawing nothing and looking a few times a
  // second for the user to come back.
  const bool unseen = !WalkSeen();
  if (unseen != walk_unseen_) {
    walk_unseen_ = unseen;
    SetTimer(window_, kWalkTimer, unseen ? kWalkUnseenMs : kFrameMs, nullptr);
  }
  if (unseen) return;
  const int before = walk_frame_ ? walk_frame_->step : 0;
  SetWalkTime(std::fmod(walk_time_ + elapsed, scene_.walkthrough->length));
  if (moving_ || !walk_frame_ || walk_frame_->step != before) {
    // The step lights up in the list too.
    Redraw();
    return;
  }
  // Only the frame moves: only the part of it the panel shows is drawn again.
  const D2D1_RECT_F& frame = scene_.walk;
  InvalidateDips({std::max(frame.left, scene_.panel.left + 1), std::max(frame.top - scroll_, scene_.panel.top + 1),
                  std::min(frame.right, scene_.panel.right - 1), std::min(frame.bottom - scroll_, scene_.panel.bottom - 1)},
                 1);
}

void SetupWindow::Step() {
  const float ms = std::chrono::duration<float, std::milli>(Clock::now() - motion_start_).count();
  if (moving_) {
    const float t = Ease(ms / kStructureMs);
    if (caret_to_) shown_.caret_x = caret_from_ + (*caret_to_ - caret_from_) * t;
    else shown_.caret_x.reset();
    for (size_t i = 0; i + 1 < kStageCount; ++i) shown_.wire[i] = wire_from_[i] + (wire_to_[i] - wire_from_[i]) * t;
    shown_.arrive = arrive_ + (1 - arrive_) * Ease(ms / kArriveMs);
    if (ms >= kStructureMs) moving_ = false;
  }
  // The waiting dot breathes for as long as the page waits.
  if (scene_.waiting && animate_) {
    const double breath = std::chrono::duration<double, std::milli>(Clock::now() - breath_start_).count();
    const auto phase = static_cast<float>(std::fmod(breath, kBreathMs) / kBreathMs);
    shown_.pulse = 0.5f + 0.5f * std::cos(phase * 2 * std::numbers::pi_v<float>);
  } else {
    shown_.pulse = 1;
  }
  if (!moving_ && !(scene_.waiting && animate_)) {
    KillTimer(window_, kMotionTimer);
  } else if (!moving_) {
    SetTimer(window_, kMotionTimer, kBreathFrameMs, nullptr);
  }
  if (moving_ || !scene_.waiting) {
    Redraw();
  } else if (window_ && !IsIconic(window_)) {
    // Only the dot moves: only its box is drawn again.
    InvalidateDips(scene_.dot, 2);
  }
}

}  // namespace knobs::tray
