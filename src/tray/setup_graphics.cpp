// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/setup_graphics.h"

#include <wincodec.h>

#include <cctype>
#include <charconv>
#include <vector>

namespace knobs::tray {
namespace {

bool HasFamily(IDWriteFontCollection* collection, const wchar_t* name) {
  UINT32 index = 0;
  BOOL exists = FALSE;
  return collection && SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists;
}

// Reads SVG's path syntax into a geometry sink.
class PathReader {
 public:
  PathReader(std::string_view svg, ID2D1GeometrySink* sink, bool filled) : svg_(svg), sink_(sink), filled_(filled) {}

  bool Read() {
    char command = 0;
    while (true) {
      SkipSeparators();
      if (at_ >= svg_.size()) break;
      if (std::isalpha(static_cast<unsigned char>(svg_[at_]))) {
        command = svg_[at_++];
      } else if (command == 0) {
        return false;
      }
      const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
      const D2D1_POINT_2F from = relative ? point_ : D2D1_POINT_2F{0, 0};
      switch (std::tolower(static_cast<unsigned char>(command))) {
        case 'm': {
          float x = 0, y = 0;
          if (!Number(x) || !Number(y)) return false;
          EndOpenFigure();
          point_ = start_ = {from.x + x, from.y + y};
          sink_->BeginFigure(point_, filled_ ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
          open_ = true;
          // Pairs after a moveto are linetos.
          command = relative ? 'l' : 'L';
          break;
        }
        case 'l': {
          float x = 0, y = 0;
          if (!Number(x) || !Number(y)) return false;
          LineTo({from.x + x, from.y + y});
          break;
        }
        case 'h': {
          float x = 0;
          if (!Number(x)) return false;
          LineTo({from.x + x, point_.y});
          break;
        }
        case 'v': {
          float y = 0;
          if (!Number(y)) return false;
          LineTo({point_.x, from.y + y});
          break;
        }
        case 'a': {
          float rx = 0, ry = 0, rotation = 0, large = 0, sweep = 0, x = 0, y = 0;
          if (!Number(rx) || !Number(ry) || !Number(rotation) || !Number(large) || !Number(sweep) || !Number(x) ||
              !Number(y)) {
            return false;
          }
          point_ = {from.x + x, from.y + y};
          // Screen coordinates run down, as SVG's do: a positive sweep is
          // clockwise in both.
          sink_->AddArc({point_, {rx, ry}, rotation,
                         sweep != 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                         large != 0 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL});
          break;
        }
        case 'z':
          if (open_) sink_->EndFigure(D2D1_FIGURE_END_CLOSED);
          open_ = false;
          point_ = start_;
          command = 0;
          break;
        default:
          return false;
      }
    }
    EndOpenFigure();
    return true;
  }

 private:
  void SkipSeparators() {
    while (at_ < svg_.size() && (svg_[at_] == ' ' || svg_[at_] == ',' || svg_[at_] == '\n')) ++at_;
  }

  bool Number(float& value) {
    SkipSeparators();
    const char* begin = svg_.data() + at_;
    const auto [end, error] = std::from_chars(begin, svg_.data() + svg_.size(), value);
    if (error != std::errc{}) return false;
    at_ += static_cast<size_t>(end - begin);
    return true;
  }

  void LineTo(D2D1_POINT_2F point) {
    point_ = point;
    sink_->AddLine(point_);
  }

  void EndOpenFigure() {
    if (open_) sink_->EndFigure(D2D1_FIGURE_END_OPEN);
    open_ = false;
  }

  std::string_view svg_;
  ID2D1GeometrySink* sink_;
  bool filled_;
  size_t at_ = 0;
  D2D1_POINT_2F point_{}, start_{};
  bool open_ = false;
};

ComPtr<ID2D1Geometry> Group(std::vector<ComPtr<ID2D1Geometry>> parts, float offset) {
  ID2D1Factory1* factory = D2DFactory();
  std::vector<ID2D1Geometry*> raw;
  for (const auto& part : parts) {
    if (part) raw.push_back(part.Get());
  }
  ComPtr<ID2D1GeometryGroup> group;
  if (!factory || FAILED(factory->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(),
                                                      static_cast<UINT32>(raw.size()), &group))) {
    return nullptr;
  }
  if (offset == 0) return group;
  ComPtr<ID2D1TransformedGeometry> moved;
  factory->CreateTransformedGeometry(group.Get(), D2D1::Matrix3x2F::Translation(offset, offset), &moved);
  return moved;
}

ComPtr<ID2D1Geometry> Circle(float x, float y, float r) {
  ComPtr<ID2D1EllipseGeometry> circle;
  if (ID2D1Factory1* factory = D2DFactory()) factory->CreateEllipseGeometry(D2D1::Ellipse({x, y}, r, r), &circle);
  return circle;
}

ComPtr<ID2D1Geometry> RoundRect(float x, float y, float w, float h, float r) {
  ComPtr<ID2D1RoundedRectangleGeometry> rect;
  if (ID2D1Factory1* factory = D2DFactory()) {
    factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), &rect);
  }
  return rect;
}

ComPtr<ID2D1StrokeStyle> MakeStroke(bool round) {
  ComPtr<ID2D1StrokeStyle> style;
  const D2D1_CAP_STYLE cap = round ? D2D1_CAP_STYLE_ROUND : D2D1_CAP_STYLE_FLAT;
  if (ID2D1Factory1* factory = D2DFactory()) {
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(cap, cap, cap,
                                                           round ? D2D1_LINE_JOIN_ROUND : D2D1_LINE_JOIN_MITER),
                               nullptr, 0, &style);
  }
  return style;
}

}  // namespace

ID2D1Factory1* D2DFactory() {
  static const ComPtr<ID2D1Factory1> factory = [] {
    ComPtr<ID2D1Factory1> made;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, made.GetAddressOf());
    return made;
  }();
  return factory.Get();
}

IDWriteFactory3* DWriteFactory() {
  static const ComPtr<IDWriteFactory3> factory = [] {
    ComPtr<IDWriteFactory3> made;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                        reinterpret_cast<IUnknown**>(made.GetAddressOf()));
    return made;
  }();
  return factory.Get();
}

const SetupFonts& Fonts() {
  static const SetupFonts fonts = [] {
    ComPtr<IDWriteFontCollection> system;
    if (IDWriteFactory3* factory = DWriteFactory()) factory->GetSystemFontCollection(&system, FALSE);
    SetupFonts found;
    // Segoe UI Variable's optical sizes are families of their own.
    const bool variable = HasFamily(system.Get(), L"Segoe UI Variable Text");
    found.text = variable ? L"Segoe UI Variable Text" : L"Segoe UI";
    found.display = variable && HasFamily(system.Get(), L"Segoe UI Variable Display") ? L"Segoe UI Variable Display"
                                                                                      : found.text;
    found.mono = L"Consolas";
    if (HasFamily(system.Get(), L"Segoe Fluent Icons")) {
      found.icons = L"Segoe Fluent Icons";
    } else if (HasFamily(system.Get(), L"Segoe MDL2 Assets")) {
      found.icons = L"Segoe MDL2 Assets";
    }
    return found;
  }();
  return fonts;
}

ComPtr<IDWriteTextFormat> MakeTextFormat(const TextStyle& style) {
  IDWriteFactory3* factory = DWriteFactory();
  if (!factory || !style.family) return nullptr;
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(factory->CreateTextFormat(style.family->c_str(), nullptr, style.weight, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, style.size, L"en-us", &format))) {
    return nullptr;
  }
  // The font's ascent and descent, centred in the line.
  float ascent = style.size * 0.75f;
  float descent = style.size * 0.25f;
  ComPtr<IDWriteFontCollection> system;
  factory->GetSystemFontCollection(&system, FALSE);
  UINT32 index = 0;
  BOOL exists = FALSE;
  ComPtr<IDWriteFontFamily> family;
  ComPtr<IDWriteFont> font;
  if (system && SUCCEEDED(system->FindFamilyName(style.family->c_str(), &index, &exists)) && exists &&
      SUCCEEDED(system->GetFontFamily(index, &family)) &&
      SUCCEEDED(family->GetFirstMatchingFont(style.weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                             &font))) {
    DWRITE_FONT_METRICS metrics = {};
    font->GetMetrics(&metrics);
    ascent = style.size * metrics.ascent / metrics.designUnitsPerEm;
    descent = style.size * metrics.descent / metrics.designUnitsPerEm;
  }
  format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, style.line_height,
                         (style.line_height - ascent - descent) / 2 + ascent);
  return format;
}

ComPtr<ID2D1PathGeometry> PathGeometry(std::string_view svg, bool filled) {
  ID2D1Factory1* factory = D2DFactory();
  ComPtr<ID2D1PathGeometry> path;
  ComPtr<ID2D1GeometrySink> sink;
  if (!factory || FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
  sink->SetFillMode(D2D1_FILL_MODE_WINDING);
  const bool read = PathReader(svg, sink.Get(), filled).Read();
  if (FAILED(sink->Close()) || !read) return nullptr;
  return path;
}

ComPtr<ID2D1Geometry> IconGeometry(Icon icon) {
  // The site's glyphs: the signal path's in a 20-unit square, drawn half a
  // unit in so their 1.5-unit strokes sit in a 21-DIP box; the sprite's in
  // a 16-unit one.
  switch (icon) {
    case Icon::kMic:
      return Group({RoundRect(7, 2.25f, 6, 9.5f, 3), PathGeometry("M4.25 9.25a5.75 5.75 0 0 0 11.5 0M10 15v2.75M7 17.75h6")},
                   0.5f);
    case Icon::kFilters:
      return Group({PathGeometry("M2.75 4.25h6M11.25 10h6M2.75 15.75H6.5m9 0h1.75"), Circle(13.25f, 4.25f, 1.75f),
                    Circle(6.75f, 10, 1.75f), Circle(11, 15.75f, 1.75f)},
                   0.5f);
    case Icon::kCable:
      return Group({PathGeometry("M7.75 2.25v3.5m4.5-3.5v3.5M5 5.75h10v2.5a5 5 0 0 1-10 0zm5 7.5v4.5")}, 0.5f);
    case Icon::kApps:
      return Group({RoundRect(3, 3, 5.25f, 5.25f, 1.25f), RoundRect(11.75f, 3, 5.25f, 5.25f, 1.25f),
                    RoundRect(3, 11.75f, 5.25f, 5.25f, 1.25f), RoundRect(11.75f, 11.75f, 5.25f, 5.25f, 1.25f)},
                   0.5f);
    case Icon::kChevron:
      return PathGeometry("m5.875 3.75 4.25 4.25-4.25 4.25");
    case Icon::kBoxCheck:
      return PathGeometry("m3.5 8.25 3 3 6-6.5");
  }
  return nullptr;
}

ComPtr<ID2D1Geometry> CrispIconGeometry(Icon icon) {
  // The same glyphs in the 21-DIP box, their straight strokes on pixel
  // centres.
  switch (icon) {
    case Icon::kMic:
      return Group({RoundRect(7.5f, 2.5f, 6, 10, 3), PathGeometry("M4.5 9.5a6 6 0 0 0 12 0M10.5 15.5v3M7.5 18.5h6")}, 0);
    case Icon::kFilters:
      return Group({PathGeometry("M3.5 4.5h6M11.5 10.5h6M3.5 16.5h4m8 0h2"), Circle(13.5f, 4.5f, 2),
                    Circle(7.5f, 10.5f, 2), Circle(11.5f, 16.5f, 2)},
                   0);
    case Icon::kCable:
      return Group({PathGeometry("M8.5 2.5v4m4-4v4M5.5 6.5h10v2.5a5 5 0 0 1-10 0zM10.5 14v4.5")}, 0);
    case Icon::kApps:
      return Group({RoundRect(3.5f, 3.5f, 5, 5, 1.25f), RoundRect(12.5f, 3.5f, 5, 5, 1.25f),
                    RoundRect(3.5f, 12.5f, 5, 5, 1.25f), RoundRect(12.5f, 12.5f, 5, 5, 1.25f)},
                   0);
    default:
      return nullptr;
  }
}

float IconSize(Icon icon) {
  switch (icon) {
    case Icon::kMic:
    case Icon::kFilters:
    case Icon::kCable:
    case Icon::kApps:
      return 21;
    default:
      return 16;
  }
}

ID2D1StrokeStyle* RoundStroke() {
  static const ComPtr<ID2D1StrokeStyle> style = MakeStroke(true);
  return style.Get();
}

ID2D1StrokeStyle* SharpStroke() {
  static const ComPtr<ID2D1StrokeStyle> style = MakeStroke(false);
  return style.Get();
}

ComPtr<ID2D1Bitmap> LoadPictureResource(ID2D1RenderTarget* target, HINSTANCE instance, std::wstring_view name) {
  const std::wstring id(name);
  const HRSRC resource = FindResourceW(instance, id.c_str(), RT_RCDATA);
  if (!target || !resource) return nullptr;
  const HGLOBAL loaded = LoadResource(instance, resource);
  const void* bytes = loaded ? LockResource(loaded) : nullptr;
  const DWORD size = SizeofResource(instance, resource);
  if (!bytes || size == 0) return nullptr;
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> converter;
  ComPtr<ID2D1Bitmap> bitmap;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
      FAILED(wic->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(bytes)), size)) ||
      FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame))) {
    return nullptr;
  }
  ComPtr<IWICBitmapSource> source = frame;
  UINT width = 0, height = 0;
  frame->GetSize(&width, &height);
  if (const UINT32 most = target->GetMaximumBitmapSize(); most > 0 && (width > most || height > most)) {
    const double fit = std::min(static_cast<double>(most) / width, static_cast<double>(most) / height);
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), std::max(1u, static_cast<UINT>(width * fit)),
                                  std::max(1u, static_cast<UINT>(height * fit)), WICBitmapInterpolationModeFant))) {
      return nullptr;
    }
    source = scaler;
  }
  if (FAILED(wic->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeMedianCut)) ||
      FAILED(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap))) {
    return nullptr;
  }
  return bitmap;
}

}  // namespace knobs::tray
