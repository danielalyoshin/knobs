// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/badge.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "knobs_tokens.h"
#include "tray/setup_graphics.h"

namespace knobs::tray {
namespace {

struct Color {
  float r, g, b;
};

constexpr Color Rgb(uint32_t rgb) {
  return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f};
}

// The state marks' colours (knobs-brand's status tokens): stop for "!" and
// info for pause, both with a white mark. On a dark taskbar the info disc
// turns over, white with graphite bars, since the graphite disc barely shows
// there.
struct Palette {
  Color fill;
  Color mark;
};

Palette Colors(Badge badge, bool on_dark) {
  if (badge == Badge::kAttention) return {Rgb(tokens::status::kStop), Rgb(tokens::status::kOnStatus)};
  if (on_dark) return {Rgb(tokens::status::kInfoOnDark), Rgb(tokens::status::kOnInfoOnDark)};
  return {Rgb(tokens::status::kInfo), Rgb(tokens::status::kOnStatus)};
}

constexpr float kBadge = 0.4375f;  // Of the icon's size: 7 px at 16.

// How much of a pixel a shape covers, from the signed distance of the
// pixel's center to the shape's edge, in pixels (negative inside).
float Coverage(float distance) { return std::clamp(0.5f - distance, 0.0f, 1.0f); }

Color Mix(Color a, Color b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

uint8_t Byte(float value) { return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); }

// The nearest whole number to `value` that's odd if `odd`, else even: a
// stroke that size centered on a badge of that parity has whole-pixel edges.
int WithParity(float value, bool odd) {
  int whole = static_cast<int>(std::lround(value));
  if ((whole % 2 != 0) != odd) whole += value > whole ? 1 : -1;
  return std::max(odd ? 1 : 2, whole);
}

// The least whole number from `least` up that's odd if `odd`, else even.
int AtLeastWithParity(int least, bool odd) { return least + (((least % 2 != 0) != odd) ? 1 : 0); }

// The state marks' "!" and pause bars, in units of the disc's 16, as the
// brand's shapes have them (MarkRects): a bar's width, the "!"'s height and
// the gap above its dot, and the pause bars' height and the gap between them.
struct Proportions {
  float bar = 2, mark = 9, mark_gap = 1.5f, pause = 7, pause_gap = 1.5f;
};

Proportions MarkProportions() {
  Proportions p;
  const std::vector<D2D1_RECT_F>& mark = MarkRects(MarkShape::kExclamation);
  if (mark.size() == 2) {
    p.bar = mark[0].right - mark[0].left;
    p.mark = mark[1].bottom - mark[0].top;
    p.mark_gap = mark[1].top - mark[0].bottom;
  }
  const std::vector<D2D1_RECT_F>& pause = MarkRects(MarkShape::kPause);
  if (pause.size() == 2) {
    p.pause = pause[0].bottom - pause[0].top;
    p.pause_gap = pause[1].left - pause[0].right;
  }
  return p;
}

// The badge's mark, as coverage of each of the icon's pixels: the state
// marks' "!" and pause bars in their proportions to the disc, cut square and
// fitted to whole pixels so they stay crisp at tray sizes.
std::vector<float> Mark(Badge badge, int size, int diameter) {
  std::vector<float> mark(static_cast<size_t>(size) * size, 0.0f);
  const bool odd = diameter % 2 != 0;
  const float d = static_cast<float>(diameter);
  // The badge's top-left corner.
  const int corner = size - diameter;
  const auto fill = [&](int left, int top, int width, int height) {
    for (int y = top; y < top + height; ++y) {
      for (int x = left; x < left + width; ++x) {
        if (x >= 0 && y >= 0 && x < size && y < size) mark[static_cast<size_t>(y) * size + x] = 1.0f;
      }
    }
  };
  static const Proportions p = MarkProportions();
  // Bars and gaps a pixel each up to 24 px, then more.
  const int stroke = WithParity(d * p.bar / 16, odd);
  if (badge == Badge::kAttention) {
    // A bar over a square dot. The bar stays longer than the dot at any
    // size, and the mark inside the disc.
    const int gap = std::max(1, static_cast<int>(std::lround(d * p.mark_gap / 16)));
    const int height = std::min(
        std::max(WithParity(d * p.mark / 16, odd), AtLeastWithParity(2 * stroke + gap + 1, odd)), diameter - 2);
    const int left = corner + (diameter - stroke) / 2;
    const int top = corner + (diameter - height) / 2;
    fill(left, top, stroke, height - stroke - gap);
    fill(left, top + height - stroke, stroke, stroke);
  } else {
    // Two bars.
    const int gap = std::max(1, static_cast<int>(std::lround(d * p.pause_gap / 16)));
    const int bar_gap = AtLeastWithParity(gap + 2 * stroke, odd) - 2 * stroke;
    const int height = WithParity(d * p.pause / 16, odd);
    const int left = corner + (diameter - (2 * stroke + bar_gap)) / 2;
    const int top = corner + (diameter - height) / 2;
    fill(left, top, stroke, height);
    fill(left + stroke + bar_gap, top, stroke, height);
  }
  return mark;
}

// A pixel three quarters opaque or more is solid.
constexpr float kSolid = 0.75f;

// Numbers each solid pixel in `alpha` (`size` square) by its group, from 1,
// in `group` (0 for the rest), and returns each group's size by its number.
// Pixels group through their edges only, so a sliver of soft pixels doesn't
// join two pieces.
std::vector<int> Groups(const std::vector<float>& alpha, int size, std::vector<int>& group) {
  group.assign(alpha.size(), 0);
  std::vector<int> sizes = {0};
  std::vector<int> pending;
  for (int start = 0; start < size * size; ++start) {
    if (alpha[static_cast<size_t>(start)] < kSolid || group[static_cast<size_t>(start)] != 0) continue;
    const int id = static_cast<int>(sizes.size());
    sizes.push_back(0);
    group[static_cast<size_t>(start)] = id;
    pending.push_back(start);
    while (!pending.empty()) {
      const int at = pending.back();
      pending.pop_back();
      ++sizes[static_cast<size_t>(id)];
      constexpr int kSteps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& step : kSteps) {
        const int x = at % size + step[0];
        const int y = at / size + step[1];
        if (x < 0 || y < 0 || x >= size || y >= size) continue;
        const auto next = static_cast<size_t>(y * size + x);
        if (alpha[next] < kSolid || group[next] != 0) continue;
        group[next] = id;
        pending.push_back(y * size + x);
      }
    }
  }
  return sizes;
}

}  // namespace

IconPixels AddBadge(IconPixels icon, Badge badge, bool on_dark) {
  if (badge == Badge::kNone || icon.size <= 0) return icon;
  const int n = icon.size;
  const float size = static_cast<float>(n);
  // Whole pixels, flush with the corner.
  const int whole_diameter = static_cast<int>(std::lround(size * kBadge));
  const float diameter = static_cast<float>(whole_diameter);
  const float radius = diameter / 2;
  const float center = size - radius;
  // The clear ring: a pixel at 16 px, two at 32.
  const float gap = std::max(1.0f, std::round(size / 16));
  const Palette colors = Colors(badge, on_dark);
  const std::vector<float> mark = Mark(badge, n, whole_diameter);
  const auto distance = [&](int x, int y) { return std::hypot(x + 0.5f - center, y + 0.5f - center); };

  // The mark, cut clear inside the badge and its ring.
  std::vector<float> alpha(icon.bgra.size());
  for (size_t i = 0; i < alpha.size(); ++i) alpha[i] = (icon.bgra[i] >> 24) / 255.0f;
  std::vector<int> group_before, group_after;
  const std::vector<int> before = Groups(alpha, n, group_before);
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      alpha[static_cast<size_t>(y) * n + x] *= 1 - Coverage(distance(x, y) - (radius + gap));
    }
  }
  // A piece the cut leaves loose, a small part of what it was cut from, goes
  // too, with its soft edge: at tray sizes it reads as a speck, not as the
  // mark.
  const std::vector<int> after = Groups(alpha, n, group_after);
  std::vector<bool> loose(after.size(), false);
  for (size_t i = 0; i < alpha.size(); ++i) {
    if (group_after[i] == 0) continue;
    loose[static_cast<size_t>(group_after[i])] =
        after[static_cast<size_t>(group_after[i])] * 5 < before[static_cast<size_t>(group_before[i])];
  }
  std::vector<bool> cleared(alpha.size(), false);
  for (size_t i = 0; i < alpha.size(); ++i) {
    if (group_after[i] != 0 && loose[static_cast<size_t>(group_after[i])]) cleared[i] = true;
  }
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const size_t i = static_cast<size_t>(y) * n + x;
      if (cleared[i] || alpha[i] <= 0 || alpha[i] >= kSolid) continue;
      bool near_loose = false, near_kept = false;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (x + dx < 0 || y + dy < 0 || x + dx >= n || y + dy >= n) continue;
          const size_t j = static_cast<size_t>(y + dy) * n + (x + dx);
          if (cleared[j]) near_loose = true;
          if (group_after[j] != 0 && !cleared[j]) near_kept = true;
        }
      }
      if (near_loose && !near_kept) alpha[i] = 0;
    }
  }
  for (size_t i = 0; i < alpha.size(); ++i) {
    if (cleared[i]) alpha[i] = 0;
  }

  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const size_t index = static_cast<size_t>(y) * n + x;
      uint32_t& pixel = icon.bgra[index];
      const float disc = Coverage(distance(x, y) - radius);
      const float base_alpha = alpha[index];
      const Color base = Rgb(pixel & 0xffffff);
      if (disc <= 0) {
        pixel = (uint32_t{Byte(base_alpha)} << 24) | (pixel & 0xffffff);
        continue;
      }
      const Color color = Mix(colors.fill, colors.mark, mark[index]);
      const float out_alpha = disc + base_alpha * (1 - disc);
      Color out = color;
      if (out_alpha > 0) {
        const float weight = base_alpha * (1 - disc) / out_alpha;
        out = Mix(color, base, weight);
      }
      pixel = (uint32_t{Byte(out_alpha)} << 24) | (uint32_t{Byte(out.r)} << 16) | (uint32_t{Byte(out.g)} << 8) |
              Byte(out.b);
    }
  }
  return icon;
}

std::optional<IconPixels> ReadIcon(HICON icon) {
  ICONINFO info = {};
  if (!icon || !GetIconInfo(icon, &info)) return std::nullopt;
  // Deletes the icon's bitmaps on the way out.
  struct Bitmaps {
    ICONINFO& info;
    ~Bitmaps() {
      if (info.hbmColor) DeleteObject(info.hbmColor);
      if (info.hbmMask) DeleteObject(info.hbmMask);
    }
  } bitmaps{info};
  BITMAP bitmap = {};
  if (!info.hbmColor || !GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) || bitmap.bmWidth <= 0 ||
      bitmap.bmWidth != bitmap.bmHeight) {
    return std::nullopt;
  }

  IconPixels pixels;
  pixels.size = bitmap.bmWidth;
  const int size = pixels.size;
  pixels.bgra.resize(static_cast<size_t>(size) * size);
  BITMAPINFO format = {};
  format.bmiHeader.biSize = sizeof(format.bmiHeader);
  format.bmiHeader.biWidth = size;
  format.bmiHeader.biHeight = -size;  // Top-down.
  format.bmiHeader.biPlanes = 1;
  format.bmiHeader.biBitCount = 32;
  format.bmiHeader.biCompression = BI_RGB;
  const HDC screen = GetDC(nullptr);
  const bool read = GetDIBits(screen, info.hbmColor, 0, size, pixels.bgra.data(), &format, DIB_RGB_COLORS) == size;
  // An icon without alpha takes it from its mask, where a set bit is clear.
  bool has_alpha = std::any_of(pixels.bgra.begin(), pixels.bgra.end(), [](uint32_t p) { return (p >> 24) != 0; });
  if (read && !has_alpha) {
    std::vector<uint32_t> mask(pixels.bgra.size());
    format.bmiHeader.biHeight = -size;
    if (GetDIBits(screen, info.hbmMask, 0, size, mask.data(), &format, DIB_RGB_COLORS) == size) {
      for (size_t i = 0; i < mask.size(); ++i) {
        if ((mask[i] & 0xffffff) == 0) pixels.bgra[i] |= 0xff000000;
      }
    }
  }
  ReleaseDC(nullptr, screen);
  if (!read) return std::nullopt;
  return pixels;
}

HICON CreateIconFrom(const IconPixels& pixels) {
  const int size = pixels.size;
  if (size <= 0 || pixels.bgra.size() != static_cast<size_t>(size) * size) return nullptr;
  BITMAPINFO format = {};
  format.bmiHeader.biSize = sizeof(format.bmiHeader);
  format.bmiHeader.biWidth = size;
  format.bmiHeader.biHeight = -size;  // Top-down.
  format.bmiHeader.biPlanes = 1;
  format.bmiHeader.biBitCount = 32;
  format.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const HBITMAP color = CreateDIBSection(nullptr, &format, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!color) return nullptr;
  std::copy(pixels.bgra.begin(), pixels.bgra.end(), static_cast<uint32_t*>(bits));
  // All zero: the color bitmap's alpha decides what shows.
  const std::vector<uint8_t> clear(static_cast<size_t>((size + 15) / 16 * 2) * size, 0);
  const HBITMAP mask = CreateBitmap(size, size, 1, 1, clear.data());
  ICONINFO badged = {TRUE, 0, 0, mask, color};
  const HICON result = mask ? CreateIconIndirect(&badged) : nullptr;
  DeleteObject(color);
  if (mask) DeleteObject(mask);
  return result;
}

HICON BadgedIcon(HICON icon, Badge badge, bool on_dark) {
  if (!icon) return nullptr;
  if (badge == Badge::kNone) return CopyIcon(icon);
  auto pixels = ReadIcon(icon);
  return pixels ? CreateIconFrom(AddBadge(std::move(*pixels), badge, on_dark)) : nullptr;
}

}  // namespace knobs::tray
