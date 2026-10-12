// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for the guides' walkthroughs (src/tray/walkthrough.h): the
// pointer rests on what it clicks, the steps light up in order, it starts
// again where it began, and the camera never looks past the part of a
// screenshot that knobs keeps.

#include <algorithm>
#include <cmath>
#include <vector>

#include "test_harness.h"
#include "tray/walkthrough.h"

namespace {

using namespace knobs::tray;

// The frame the setup window plays it in (setup_scene.cpp).
constexpr D2D1_SIZE_F kFrame = {300, 188};
constexpr float kTick = 0.005f;

const Walkthrough& Obs() { return *WalkthroughNamed("obs-filters"); }

bool Near(D2D1_POINT_2F a, D2D1_POINT_2F b, float within = 0.5f) {
  return std::abs(a.x - b.x) <= within && std::abs(a.y - b.y) <= within;
}

TEST(WalkthroughsAreFoundByName) {
  CHECK(WalkthroughNamed("obs-filters") != nullptr);
  CHECK(WalkthroughNamed("vb-cable-setup") == nullptr);
}

TEST(WalkthroughPointerRestsOnWhatItClicks) {
  const Walkthrough& walk = Obs();
  // Mic/Aux in the Audio Mixer, Filters in its menu, then +.
  const std::vector<D2D1_POINT_2F> targets = {{440, 906}, {470, 794}, {636, 1124}};
  CHECK(walk.clicks.size() == targets.size());
  for (size_t i = 0; i < walk.clicks.size() && i < targets.size(); ++i) {
    // Still from before the click until its ring has gone.
    for (float t = walk.clicks[i] - 0.2f; t < walk.clicks[i] + 0.5f; t += kTick) {
      CHECK(Near(WalkAt(walk, t, kFrame).pointer, targets[i]));
    }
  }
  // What it opens shows after the click, and the pointer rests there for
  // more than a second before it moves on.
  for (size_t i = 0; i < walk.clicks.size(); ++i) {
    const float click = walk.clicks[i];
    const int before = WalkAt(walk, click - 0.01f, kFrame).shot;
    CHECK(WalkAt(walk, click + 0.3f, kFrame).shot != before);
    const D2D1_POINT_2F at = WalkAt(walk, click, kFrame).pointer;
    CHECK(Near(WalkAt(walk, click + 1.0f, kFrame).pointer, at) || i + 1 == walk.clicks.size());
  }
}

TEST(WalkthroughStepsLightUpInOrder) {
  const Walkthrough& walk = Obs();
  std::vector<int> seen;
  for (float t = 0; t < walk.length; t += kTick) {
    const int step = WalkAt(walk, t, kFrame).step;
    if (seen.empty() || seen.back() != step) seen.push_back(step);
  }
  CHECK((seen == std::vector<int>{1, 2, 3, 1}));
  // Each click is in the step it belongs to: Mic/Aux and Filters in step 2,
  // + in step 3.
  CHECK(WalkAt(walk, walk.clicks[0], kFrame).step == 2);
  CHECK(WalkAt(walk, walk.clicks[1], kFrame).step == 2);
  CHECK(WalkAt(walk, walk.clicks[2], kFrame).step == 3);
  // Held still, it shows the pointer on +, close to OBS's own size.
  const WalkFrame still = WalkAt(walk, walk.still, kFrame);
  CHECK(still.step == 3);
  CHECK(walk.shots[static_cast<size_t>(still.shot)].picture == "obs-filters-window");
  CHECK(Near(still.pointer, {636, 1124}));
  CHECK(still.scale >= 0.4f && still.scale <= 0.5f);
}

TEST(WalkthroughStartsAgainWhereItBegan) {
  const Walkthrough& walk = Obs();
  const WalkFrame first = WalkAt(walk, 0, kFrame);
  const WalkFrame last = WalkAt(walk, walk.length - kTick, kFrame);
  CHECK(walk.shots[static_cast<size_t>(last.shot)].picture == walk.shots[static_cast<size_t>(first.shot)].picture);
  CHECK(last.under < 0);
  CHECK(std::abs(last.scale - first.scale) < 1e-3f);
  CHECK(Near(last.centre, first.centre));
  CHECK(Near(last.pointer, first.pointer));
  // At first it shows the whole screen.
  const D2D1_RECT_F view = WalkView(first, kFrame);
  CHECK(view.left <= 0 && view.top <= 0 && view.right >= walk.screen.width && view.bottom >= walk.screen.height);
  // And it goes round: a time past its length is the same as that much less.
  CHECK(Near(WalkAt(walk, walk.length + 4, kFrame).pointer, WalkAt(walk, 4, kFrame).pointer));
}

TEST(WalkthroughCameraStaysInsideTheShots) {
  const Walkthrough& walk = Obs();
  for (float t = 0; t < walk.length; t += kTick) {
    const WalkFrame frame = WalkAt(walk, t, kFrame);
    const D2D1_RECT_F view = WalkView(frame, kFrame);
    // What it sees of the screen; past its edge is the frame's ground.
    const D2D1_RECT_F seen = {std::max(view.left, 0.0f), std::max(view.top, 0.0f),
                              std::min(view.right, walk.screen.width), std::min(view.bottom, walk.screen.height)};
    for (const int shot : {frame.shot, frame.under}) {
      if (shot < 0) continue;
      const D2D1_RECT_F& crop = walk.shots[static_cast<size_t>(shot)].crop;
      const bool inside = seen.left >= crop.left - 0.5f && seen.top >= crop.top - 0.5f &&
                          seen.right <= crop.right + 0.5f && seen.bottom <= crop.bottom + 0.5f;
      CHECK(inside);
      if (!inside) return;
    }
  }
}

TEST(WalkthroughClicksInView) {
  const Walkthrough& walk = Obs();
  for (const float click : walk.clicks) {
    for (float t = click - 0.1f; t < click + 0.5f; t += kTick) {
      const WalkFrame frame = WalkAt(walk, t, kFrame);
      const D2D1_RECT_F view = WalkView(frame, kFrame);
      // The pointer, and its ring, a little in from the frame's edge.
      const float margin = 12 / frame.scale;
      CHECK(frame.pointer.x >= view.left + margin && frame.pointer.x <= view.right - margin &&
            frame.pointer.y >= view.top + margin && frame.pointer.y <= view.bottom - margin);
    }
  }
  // After Filters, the camera shows the window it opened: its + is in view
  // while the pointer is still on the menu.
  const WalkFrame opened = WalkAt(walk, walk.clicks[1] + 1.0f, kFrame);
  const D2D1_RECT_F view = WalkView(opened, kFrame);
  CHECK(view.left < 636 && view.right > 636 && view.top < 1124 && view.bottom > 1124);
}

TEST(WalkthroughOnlyZoomsOutAtTheEnd) {
  const Walkthrough& walk = Obs();
  // Once it's in, it stays in until + has opened its menu: it pulls back
  // only to show the whole menu, and then to start again.
  float before = WalkAt(walk, walk.pointer[2].at, kFrame).scale;
  for (float t = walk.pointer[2].at; t < walk.clicks.back() + 0.15f; t += kTick) {
    const float now = WalkAt(walk, t, kFrame).scale;
    CHECK(now >= before - 1e-4f);
    if (now < before - 1e-4f) return;
    before = now;
  }
}

TEST(WalkthroughTurnsStraightToTheMenu) {
  const Walkthrough& walk = Obs();
  // From the Filters window to the + menu, the camera's centre moves along
  // a straight line, one way, as it zooms out: no curve, no turning back.
  const float from = walk.clicks.back();
  const D2D1_POINT_2F a = WalkAt(walk, from, kFrame).centre;
  const D2D1_POINT_2F b = WalkAt(walk, from + 2, kFrame).centre;
  const float length = std::hypot(b.x - a.x, b.y - a.y);
  float along_before = 0;
  for (float t = from; t < from + 2; t += kTick) {
    const D2D1_POINT_2F c = WalkAt(walk, t, kFrame).centre;
    const float along = ((c.x - a.x) * (b.x - a.x) + (c.y - a.y) * (b.y - a.y)) / length;
    const float off = std::abs((c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x)) / length;
    CHECK(off < 1);  // Pixels off the line.
    CHECK(along >= along_before - 1e-3f);
    if (off >= 1 || along < along_before - 1e-3f) return;
    along_before = along;
  }
}

TEST(WalkthroughCameraMovesSmoothly) {
  const Walkthrough& walk = Obs();
  // From one frame to the next at 60 a second, the view moves and zooms by
  // a little: no jumps, even where the camera turns or starts again.
  WalkFrame before = WalkAt(walk, 0, kFrame);
  for (float t = 1 / 60.0f; t < walk.length * 2; t += 1 / 60.0f) {
    const WalkFrame now = WalkAt(walk, t, kFrame);
    const float moved = std::hypot((now.centre.x - before.centre.x) * now.scale,
                                   (now.centre.y - before.centre.y) * now.scale);
    CHECK(moved < 12);  // DIPs.
    CHECK(std::abs(now.scale / before.scale - 1) < 0.06f);
    if (moved >= 12) return;
    before = now;
  }
}

}  // namespace
