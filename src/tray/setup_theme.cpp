// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/setup_theme.h"

namespace knobs::tray {
namespace {

D2D1_COLOR_F System(int index) {
  const COLORREF color = GetSysColor(index);
  return {GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, 1.0f};
}

bool HighContrastOn() {
  HIGHCONTRASTW contrast = {sizeof(contrast)};
  return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
         (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool AppsModeIsDark() {
  DWORD light = 1;
  DWORD size = sizeof(light);
  const LSTATUS status =
      RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
  return status == ERROR_SUCCESS && light == 0;
}

COLORREF ToColorRef(const D2D1_COLOR_F& color) {
  return RGB(static_cast<BYTE>(color.r * 255 + 0.5f), static_cast<BYTE>(color.g * 255 + 0.5f),
             static_cast<BYTE>(color.b * 255 + 0.5f));
}

}  // namespace

SetupMode CurrentSetupMode(MenuTheme theme) {
  if (HighContrastOn()) return SetupMode::kHighContrast;
  switch (theme) {
    case MenuTheme::kLight:
      return SetupMode::kLight;
    case MenuTheme::kDark:
      return SetupMode::kDark;
    case MenuTheme::kSystem:
      break;
  }
  return AppsModeIsDark() ? SetupMode::kDark : SetupMode::kLight;
}

SetupPalette MakeSetupPalette(SetupMode mode) {
  SetupPalette p;
  p.mode = mode;
  p.mark_ok = TokenColor(tokens::status::kOk);
  p.mark_stop = TokenColor(tokens::status::kStop);
  p.mark_warn = TokenColor(tokens::status::kWarn);
  p.mark_info = TokenColor(tokens::status::kInfo);
  p.on_mark = TokenColor(tokens::status::kOnStatus);
  p.on_warn = TokenColor(tokens::status::kOnWarn);
  p.on_info = p.on_mark;
  if (mode == SetupMode::kDark) {
    // The graphite disc barely shows on the dark ground, so it turns over.
    p.mark_info = TokenColor(tokens::status::kInfoOnDark);
    p.on_info = TokenColor(tokens::status::kOnInfoOnDark);
  }
  if (mode == SetupMode::kHighContrast) {
    // The user's colours only: shapes and text carry the meaning.
    const D2D1_COLOR_F window = System(COLOR_WINDOW);
    const D2D1_COLOR_F text = System(COLOR_WINDOWTEXT);
    p.ground = p.ground_deep = p.raised = p.raised_2 = p.sunken = window;
    p.line = p.line_strong = p.text = p.text2 = p.text3 = p.control_edge = text;
    p.action = p.action_text = p.focus = text;
    p.on_action = window;
    // Under the pointer, the user's highlight, as Windows' own controls have.
    p.hover = p.action_hover = p.action_pressed = System(COLOR_HIGHLIGHT);
    p.on_action_hover = System(COLOR_HIGHLIGHTTEXT);
    p.link_underline = p.link_hover = System(COLOR_HOTLIGHT);
    p.disabled = System(COLOR_GRAYTEXT);
    p.here = System(COLOR_HIGHLIGHT);
    p.mark_ok = p.mark_stop = p.mark_warn = p.mark_info = text;
    p.on_mark = p.on_warn = p.on_info = window;
    p.caption = 0xFFFFFFFF;
    return p;
  }
  const tokens::Scheme& s = mode == SetupMode::kDark ? tokens::kDark : tokens::kLight;
  p.ground = TokenColor(s.ground);
  p.ground_deep = TokenColor(s.ground_deep);
  p.raised = TokenColor(s.raised);
  p.raised_2 = TokenColor(s.raised_2);
  p.sunken = TokenColor(s.sunken);
  p.line = TokenColor(s.line);
  p.line_strong = TokenColor(s.line_strong);
  p.text = TokenColor(s.text);
  p.text2 = TokenColor(s.text_2);
  p.text3 = TokenColor(s.text_3);
  p.action = TokenColor(s.action);
  p.action_hover = TokenColor(s.action_hover);
  p.action_text = TokenColor(s.action_text);
  p.on_action = TokenColor(s.on_action);
  p.link_underline = TokenColor(s.link_underline);
  p.link_hover = TokenColor(s.link_hover);
  p.focus = TokenColor(s.focus);
  // Pressed goes a step deeper than under the pointer: the light scheme's red
  // words' tone, which is deeper than its hover. The dark scheme lights its
  // buttons under the pointer, so pressed goes back to cap red.
  p.action_pressed = mode == SetupMode::kDark ? p.action : p.action_text;
  p.on_action_hover = p.on_action;
  p.hover = p.action;
  // An idle control's edge is an idle icon's colour, which holds 3:1 against
  // the panel, as line_strong doesn't.
  p.control_edge = p.text3;
  p.disabled = p.text3;
  p.here = p.text;
  p.caption = ToColorRef(p.ground);
  return p;
}

bool AnimationsOn() {
  BOOL on = TRUE;
  return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
}

}  // namespace knobs::tray
