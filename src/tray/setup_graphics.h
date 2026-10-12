// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>
#include <string_view>

// What the setup window draws with: Direct2D and DirectWrite, which ship
// with Windows, its fonts, and the icons and badges, drawn from the same
// path data as getknobs.app's.
namespace knobs::tray {

using Microsoft::WRL::ComPtr;

// The process's factories, made on first use. Null if Direct2D or
// DirectWrite isn't there.
ID2D1Factory1* D2DFactory();
IDWriteFactory3* DWriteFactory();

// Windows 11's UI fonts, or Windows 10's.
struct SetupFonts {
  std::wstring text;     // "Segoe UI Variable Text", or "Segoe UI".
  std::wstring display;  // "Segoe UI Variable Display", or "Segoe UI".
  std::wstring mono;     // "Consolas".
  // Windows' icon font: "Segoe Fluent Icons", or "Segoe MDL2 Assets", or
  // empty without either.
  std::wstring icons;
};
const SetupFonts& Fonts();

struct TextStyle {
  const std::wstring* family = nullptr;
  float size = 14;
  float line_height = 21;
  DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
};

// A text format with lines `line_height` apart and the font's ascent and
// descent centred in each, as CSS sets them.
ComPtr<IDWriteTextFormat> MakeTextFormat(const TextStyle& style);

// The icons, from the site's sprite and signal path, as outlines to stroke
// at 1.5 DIPs with round ends: the stage icons in a 21-DIP box, the rest
// in a 16-DIP one, both at the origin.
enum class Icon { kMic, kFilters, kCable, kApps, kChevron, kBoxCheck };
ComPtr<ID2D1Geometry> IconGeometry(Icon icon);
// The stage icons redrawn for 1-DIP strokes on whole pixels at 100%, where
// a 1.5-DIP stroke can't be sharp; null for the others.
ComPtr<ID2D1Geometry> CrispIconGeometry(Icon icon);
// The box an icon is drawn in.
float IconSize(Icon icon);

// A path in SVG's syntax (M, L, H, V, A and Z, absolute or relative), for
// outlines to stroke, or with `filled`, shapes to fill.
ComPtr<ID2D1PathGeometry> PathGeometry(std::string_view svg, bool filled = false);

// A stroke style with round ends and joins, or with mitred joins and flat
// ends (`sharp`), as the wire's arrowheads have.
ID2D1StrokeStyle* RoundStroke();
ID2D1StrokeStyle* SharpStroke();

// A picture in the module's resources (RCDATA named `name`), a PNG or a
// JPEG, decoded for `target`, or null if there's none. One bigger than
// `target` can hold, as on older graphics hardware, is scaled down to fit.
ComPtr<ID2D1Bitmap> LoadPictureResource(ID2D1RenderTarget* target, HINSTANCE instance, std::wstring_view name);

}  // namespace knobs::tray
