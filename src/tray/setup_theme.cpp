// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/setup_theme.h"

#include <cstdint>

namespace knobs::tray {
namespace {

constexpr D2D1_COLOR_F Hex(uint32_t rgb) {
  return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f};
}

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
  // The tray icon's badge colours (badge.cpp), and Windows' amber.
  p.badge_ok = Hex(0x23875A);
  p.badge_stop = Hex(0xE5484D);
  p.badge_warning = Hex(0xF0A33C);
  p.badge_info = Hex(0x4A4C52);
  p.badge_glyph = Hex(0xFFFFFF);
  p.badge_glyph_dark = Hex(0x17181B);
  switch (mode) {
    case SetupMode::kLight:
      p.paper = Hex(0xF7F8FA);
      p.paper_subtle = Hex(0xEEF0F3);
      p.surface = Hex(0xFFFFFF);
      p.sunken = Hex(0xF0F1F4);
      p.line = Hex(0xE1E3E8);
      p.line_strong = Hex(0xC8CCD3);
      p.ink = Hex(0x17181B);
      p.ink_hover = Hex(0x34363C);
      p.on_ink = Hex(0xFFFFFF);
      p.text2 = Hex(0x4A4E57);
      p.text3 = Hex(0x686D77);
      p.lift_opacity = 0.10f;
      break;
    case SetupMode::kDark:
      p.paper = Hex(0x0F1012);
      p.paper_subtle = Hex(0x141518);
      p.surface = Hex(0x17181B);
      p.sunken = Hex(0x121316);
      p.line = Hex(0x26282D);
      p.line_strong = Hex(0x363940);
      p.ink = Hex(0xE8EAEE);
      p.ink_hover = Hex(0xFFFFFF);
      p.on_ink = Hex(0x17181B);
      p.text2 = Hex(0xB0B4BC);
      p.text3 = Hex(0x8B8F98);
      p.lift_opacity = 0.5f;
      break;
    case SetupMode::kHighContrast: {
      // The user's colours only: shapes and text carry the meaning.
      const D2D1_COLOR_F window = System(COLOR_WINDOW);
      const D2D1_COLOR_F text = System(COLOR_WINDOWTEXT);
      p.paper = p.paper_subtle = p.surface = p.sunken = window;
      p.line = p.line_strong = p.text2 = p.text3 = p.control_edge = text;
      p.ink = p.ink_hover = text;
      p.on_ink = window;
      p.link = System(COLOR_HOTLIGHT);
      p.disabled = System(COLOR_GRAYTEXT);
      p.here = System(COLOR_HIGHLIGHT);
      p.badge_ok = p.badge_stop = p.badge_warning = p.badge_info = text;
      p.badge_glyph = p.badge_glyph_dark = window;
      p.lift_opacity = 0;
      p.caption = 0xFFFFFFFF;
      return p;
    }
  }
  p.control_edge = p.text3;
  p.link = p.ink;
  p.disabled = p.text3;
  p.here = p.ink;
  p.caption = ToColorRef(p.paper);
  return p;
}

bool AnimationsOn() {
  BOOL on = TRUE;
  return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
}

}  // namespace knobs::tray
