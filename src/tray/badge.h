// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <vector>

// The tray icon's badges (docs/design.md, Notifications and badges): the knob
// alone while knobs runs, a pause badge while it's paused, and a "!" while the
// cable gets no mic and that needs the user. Notifications can be hidden (Do
// Not Disturb), so the icon shows the state too. They're the brand's state
// marks (knobs-brand's status tokens): a white "!" on the stop disc, and
// white pause bars on the info disc, which turns over on a dark taskbar:
// graphite bars on a white disc.
// The badge is drawn over the icon's bottom-right corner, 7/16 of its size,
// with a clear ring that lets the taskbar show between the two. The mark
// itself isn't recolored.
namespace knobs::tray {

enum class Badge { kNone, kPaused, kAttention };

// An icon's pixels: BGRA, top-down, straight alpha, as icons hold them.
struct IconPixels {
  int size = 0;  // Width and height.
  std::vector<uint32_t> bgra;
};

// `icon` with `badge`, for a taskbar that's dark if `on_dark`, where the pause
// disc turns over.
IconPixels AddBadge(IconPixels icon, Badge badge, bool on_dark = false);

// AddBadge on a Windows icon. Returns a new icon for the caller to destroy,
// or null.
HICON BadgedIcon(HICON icon, Badge badge, bool on_dark = false);

// A square icon's pixels, or nullopt.
std::optional<IconPixels> ReadIcon(HICON icon);
// An icon from pixels, for the caller to destroy, or null.
HICON CreateIconFrom(const IconPixels& pixels);

}  // namespace knobs::tray
