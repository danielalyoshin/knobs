// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>
#include <string_view>
#include <vector>

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
// at 1.5 DIPs with SharpStroke, cut straight as the brand's icons are: the
// stage icons in a 21-DIP box, the rest in a 16-DIP one, both at the origin.
enum class Icon { kMic, kFilters, kCable, kApps, kChevron, kBoxCheck, kChevronDown, kChevronUp };
// How many there are: one past the last.
inline constexpr size_t kIconCount = static_cast<size_t>(Icon::kChevronUp) + 1;
ComPtr<ID2D1Geometry> IconGeometry(Icon icon);
// The stage icons redrawn for 1-DIP strokes on whole pixels at 100%, where
// a 1.5-DIP stroke can't be sharp; null for the others.
ComPtr<ID2D1Geometry> CrispIconGeometry(Icon icon);
// The box an icon is drawn in.
float IconSize(Icon icon);

// A path in SVG's syntax (M, L, H, V, A and Z, absolute or relative), for
// outlines to stroke, or with `filled`, shapes to fill.
ComPtr<ID2D1PathGeometry> PathGeometry(std::string_view svg, bool filled = false);

// The state marks' shapes (knobs-brand's status-shape tokens), to fill, in a
// 16-unit box at the origin. A mark is a ground with a glyph over it.
enum class MarkShape { kDisc, kTriangle, kCheck, kExclamation, kInformation, kPause, kTriangleExclamation };
ID2D1Geometry* MarkGeometry(MarkShape shape);
// The shape as rectangles, in its 16-unit box, if it's only those, as the
// marks' bars and dots are, so they can be drawn on whole pixels; else empty.
const std::vector<D2D1_RECT_F>& MarkRects(MarkShape shape);

// A stroke style with round ends and joins, or with mitred joins and flat
// ends (`sharp`), as the brand's icons, the wire and its arrowheads have.
ID2D1StrokeStyle* RoundStroke();
ID2D1StrokeStyle* SharpStroke();

// A picture in the module's resources (RCDATA named `name`), a PNG or a
// JPEG, decoded for `target`, or null if there's none. One bigger than
// `target` can hold, as on older graphics hardware, is scaled down to fit.
ComPtr<ID2D1Bitmap> LoadPictureResource(ID2D1RenderTarget* target, HINSTANCE instance, std::wstring_view name);

}  // namespace knobs::tray
