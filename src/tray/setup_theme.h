// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <d2d1.h>
#include <windows.h>

#include "knobs_tokens.h"
#include "tray/menu_theme.h"

// The setup window's colours (docs/design.md, The setup window): the brand's
// scheme for Windows' light or dark app mode, from knobs-brand's tokens
// (brand/dist/knobs_tokens.h), or the user's own colours under high contrast.
namespace knobs::tray {

enum class SetupMode { kLight, kDark, kHighContrast };

// The mode to draw in: high contrast if it's on, else `theme`, where kSystem
// follows Windows' app mode (Settings > Personalization > Colors).
SetupMode CurrentSetupMode(MenuTheme theme);

// A token's colour for Direct2D.
constexpr D2D1_COLOR_F TokenColor(tokens::Argb argb) {
  return {tokens::Red(argb), tokens::Green(argb), tokens::Blue(argb), tokens::Alpha(argb)};
}

// The brand's roles, as the window uses them.
struct SetupPalette {
  SetupMode mode = SetupMode::kLight;
  D2D1_COLOR_F ground;       // The window's ground, under the panel and the foot.
  D2D1_COLOR_F ground_deep;  // The band.
  D2D1_COLOR_F raised;       // The panel, and the controls on it.
  D2D1_COLOR_F raised_2;     // A door or a choice under the pointer, or pressed.
  D2D1_COLOR_F sunken;       // The chain's well.
  D2D1_COLOR_F line;         // Hairlines: rules, the panel's edge, the well's.
  D2D1_COLOR_F line_strong;  // Control edges.
  D2D1_COLOR_F text;         // Text, names, the stage the page is about, the wire as far as the signal has got.
  D2D1_COLOR_F text2;        // Secondary text.
  D2D1_COLOR_F text3;        // Labels, the foot's note, idle icons and wires.
  // Cap red: what can be pressed.
  D2D1_COLOR_F action;           // The default button, a secondary button's edge, what's selected.
  D2D1_COLOR_F action_hover;     // The default button under the pointer.
  D2D1_COLOR_F action_pressed;   // A button pressed.
  D2D1_COLOR_F action_text;      // A secondary button's label.
  D2D1_COLOR_F on_action;        // Text and glyphs on an action fill.
  D2D1_COLOR_F on_action_hover;  // On action_hover or action_pressed.
  D2D1_COLOR_F hover;            // A control's edge or chevron under the pointer.
  D2D1_COLOR_F link_underline;
  D2D1_COLOR_F link_hover;  // A link's words under the pointer.
  D2D1_COLOR_F focus;       // The focus ring.
  D2D1_COLOR_F control_edge;  // An idle radio's or check box's edge.
  D2D1_COLOR_F disabled;      // Disabled text.
  D2D1_COLOR_F here;          // The stage the page is about.
  // The state marks, the tray badges' colours, the same in both modes but
  // for the info disc.
  D2D1_COLOR_F mark_ok, mark_stop, mark_warn, mark_info;
  D2D1_COLOR_F on_mark;  // The glyph on the ok and stop discs.
  D2D1_COLOR_F on_info;  // The glyph on the info disc, which turns over on a dark ground.
  D2D1_COLOR_F on_warn;  // The glyph on the warning triangle.
  // The title bar, for DWMWA_CAPTION_COLOR, or DWMWA_COLOR_DEFAULT.
  COLORREF caption = 0xFFFFFFFF;
};

SetupPalette MakeSetupPalette(SetupMode mode);

// Windows' "Show animations" setting, off for reduced motion.
bool AnimationsOn();

}  // namespace knobs::tray
