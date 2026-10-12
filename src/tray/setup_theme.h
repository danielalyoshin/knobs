// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <d2d1.h>
#include <windows.h>

#include "tray/menu_theme.h"

// The setup window's colours (docs/design.md, The setup window): getknobs.app's
// graphite in Windows' light or dark app mode, or the user's own colours under
// high contrast.
namespace knobs::tray {

enum class SetupMode { kLight, kDark, kHighContrast };

// The mode to draw in: high contrast if it's on, else `theme`, where kSystem
// follows Windows' app mode (Settings > Personalization > Colors).
SetupMode CurrentSetupMode(MenuTheme theme);

struct SetupPalette {
  SetupMode mode = SetupMode::kLight;
  D2D1_COLOR_F paper;         // The window's ground.
  D2D1_COLOR_F paper_subtle;  // The band.
  D2D1_COLOR_F surface;       // The panel, buttons, choices.
  D2D1_COLOR_F sunken;        // Wells: the chain, the screenshots' mat.
  D2D1_COLOR_F line;          // Rules inside a container.
  D2D1_COLOR_F line_strong;   // Container borders.
  D2D1_COLOR_F ink;           // Text, the primary button, what's selected.
  D2D1_COLOR_F ink_hover;
  D2D1_COLOR_F on_ink;        // Text on ink.
  D2D1_COLOR_F text2;         // Secondary text.
  D2D1_COLOR_F text3;         // Tertiary text, idle icons.
  D2D1_COLOR_F control_edge;  // Radio and check box rings.
  D2D1_COLOR_F link;
  D2D1_COLOR_F disabled;
  D2D1_COLOR_F here;          // The stage the page is about.
  // The badges, the tray icon's colours in both modes (badge.h): a disc with
  // a glyph on it.
  D2D1_COLOR_F badge_ok, badge_stop, badge_warning, badge_info;
  D2D1_COLOR_F badge_glyph;       // On the discs.
  D2D1_COLOR_F badge_glyph_dark;  // On the amber triangle.
  float lift_opacity = 0;         // The panel's one-pixel shadow.
  // The title bar, for DWMWA_CAPTION_COLOR, or DWMWA_COLOR_DEFAULT.
  COLORREF caption = 0xFFFFFFFF;
};

SetupPalette MakeSetupPalette(SetupMode mode);

// Windows' "Show animations" setting, off for reduced motion.
bool AnimationsOn();

}  // namespace knobs::tray
