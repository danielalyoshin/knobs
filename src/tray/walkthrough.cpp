// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/walkthrough.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace knobs::tray {
namespace {

// How far behind the pointer the camera runs, as a hand-held one would.
constexpr float kCameraLag = 0.12f;
// How long the camera takes to turn to what it looks at: as long as it
// takes to zoom out with it, so the two keep step.
constexpr float kLookTurn = 0.8f;
// From this close the camera follows the pointer, or looks where it's told;
// further out it eases back to the whole screen.
constexpr float kFollowScale = 0.2f;
constexpr float kClickRing = 0.5f;
constexpr float kClickPress = 0.14f;

// Places in OBS 32.2.2 at twice its size, on a 2560 by 1392 screen
// (src/tray/pictures/README.md).
constexpr Walkthrough::Point kObsStart = {0, 1950, 1180};
constexpr float kMixerX = 440, kMixerY = 906;      // Mic/Aux in the Audio Mixer.
constexpr float kFiltersX = 470, kFiltersY = 794;  // Filters in its menu.
constexpr float kPlusX = 636, kPlusY = 1124;       // + under Audio Filters.

// Setting up a mic in OBS (FirstRun's ObsSteps): open OBS, click Mic/Aux in
// the Audio Mixer and choose Filters, then click + under Audio Filters. The
// pointer rests after each click, so the user sees what it opened.
Walkthrough ObsFilters() {
  Walkthrough walk;
  walk.screen = {2560, 1392};
  walk.shots = {
      {0, "obs-main", {0, 0, 2560, 1392}},
      {3.4f, "obs-mixer-menu", {67, 554, 843, 1146}},
      {5.3f, "obs-mixer-filters", {96, 554, 844, 1036}},
      {5.9f, "obs-filters-window", {97, 554, 1137, 1250}},
      {8.9f, "obs-add-filter", {218, 451, 1362, 1250}},
      {12.5f, "obs-main", {0, 0, 2560, 1392}, 0.3f},
  };
  walk.pointer = {
      {0, kObsStart.x, kObsStart.y},
      {1.6f, kObsStart.x, kObsStart.y},
      {3.0f, kMixerX, kMixerY},
      {4.6f, kMixerX, kMixerY},
      {5.3f, kFiltersX, kFiltersY},
      {7.1f, kFiltersX, kFiltersY},
      {8.5f, kPlusX, kPlusY},
      {12.8f, kPlusX, kPlusY},
      {13.4f, kObsStart.x, kObsStart.y},
  };
  // In once, close enough to read, and it stays in until it pulls back to
  // show the whole + menu, as it turns to it.
  walk.zoom = {{0, 0}, {1.6f, 0}, {3.0f, 0.42f}, {8.9f, 0.42f}, {9.7f, 0.27f}, {12.8f, 0.27f}, {13.4f, 0}};
  walk.clicks = {3.25f, 5.75f, 8.75f};
  walk.steps = {{0, 1}, {1.6f, 2}, {5.9f, 3}, {12.5f, 1}};
  // The Filters window's list and the buttons under it, then straight on to
  // the + menu and + under it.
  walk.looks = {{5.95f, 8.9f, 760, 1010}, {8.9f, 13.6f, 790, 815}};
  walk.length = 13.6f;
  // The pointer on +, close to OBS's own size.
  walk.still = 8.6f;
  walk.ground = D2D1::ColorF(0x111317);
  return walk;
}

// Ease in and out, gently: at its fastest, half as fast again as its
// average, so the camera never whips.
float Ease(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return 0.5f - 0.5f * std::cos(x * std::numbers::pi_v<float>);
}

float Lerp(float a, float b, float f) { return a + (b - a) * f; }

D2D1_POINT_2F PointerAt(const Walkthrough& walk, float time) {
  const auto& keys = walk.pointer;
  if (keys.empty()) return {walk.screen.width / 2, walk.screen.height / 2};
  if (time <= keys.front().at) return {keys.front().x, keys.front().y};
  for (size_t i = 1; i < keys.size(); ++i) {
    if (time > keys[i].at) continue;
    const Walkthrough::Point& a = keys[i - 1];
    const Walkthrough::Point& b = keys[i];
    const float f = b.at > a.at ? Ease((time - a.at) / (b.at - a.at)) : 1;
    return {Lerp(a.x, b.x, f), Lerp(a.y, b.y, f)};
  }
  return {keys.back().x, keys.back().y};
}

float ScaleAt(const Walkthrough& walk, float time, float wide) {
  const auto& keys = walk.zoom;
  const auto value = [wide](const Walkthrough::Zoom& key) { return key.scale > 0 ? key.scale : wide; };
  if (keys.empty()) return wide;
  if (time <= keys.front().at) return value(keys.front());
  for (size_t i = 1; i < keys.size(); ++i) {
    if (time > keys[i].at) continue;
    const float f = keys[i].at > keys[i - 1].at ? Ease((time - keys[i - 1].at) / (keys[i].at - keys[i - 1].at)) : 1;
    return Lerp(value(keys[i - 1]), value(keys[i]), f);
  }
  return value(keys.back());
}

}  // namespace

const Walkthrough* WalkthroughNamed(std::string_view name) {
  static const Walkthrough obs_filters = ObsFilters();
  if (name == "obs-filters") return &obs_filters;
  return nullptr;
}

WalkFrame WalkAt(const Walkthrough& walk, float time, D2D1_SIZE_F frame) {
  WalkFrame shown;
  if (walk.length > 0) time = std::fmod(std::max(time, 0.0f), walk.length);

  for (size_t i = 0; i < walk.shots.size(); ++i) {
    if (walk.shots[i].at <= time) shown.shot = static_cast<int>(i);
  }
  if (!walk.shots.empty()) {
    const Walkthrough::Shot& shot = walk.shots[static_cast<size_t>(shown.shot)];
    if (shown.shot > 0 && shot.fade > 0 && time < shot.at + shot.fade) {
      shown.under = shown.shot - 1;
      shown.fade = (time - shot.at) / shot.fade;
    }
  }

  shown.pointer = PointerAt(walk, time);
  for (const float click : walk.clicks) {
    const float since = time - click;
    if (since < 0 || since >= kClickRing) continue;
    shown.ring = since / kClickRing;
    if (since < kClickPress) shown.press = 0.88f;
  }

  const float wide = std::min(frame.width / walk.screen.width, frame.height / walk.screen.height);
  shown.scale = ScaleAt(walk, time, wide);
  D2D1_POINT_2F target = PointerAt(walk, std::max(0.0f, time - kCameraLag));
  for (const Walkthrough::Look& look : walk.looks) {
    if (time < look.from) continue;
    // Where another look takes over, the camera turns straight to it, not
    // back by way of the pointer.
    const bool handed_on = std::any_of(walk.looks.begin(), walk.looks.end(), [&look](const Walkthrough::Look& next) {
      return std::abs(next.from - look.until) < 1e-3f;
    });
    const float in = Ease((time - look.from) / kLookTurn);
    const float out = time > look.until && !handed_on ? Ease((time - look.until) / kLookTurn) : 0;
    const float f = in * (1 - out);
    target = {Lerp(target.x, look.x, f), Lerp(target.y, look.y, f)};
  }
  const float follow = std::clamp((shown.scale - wide) / (kFollowScale - wide), 0.0f, 1.0f);
  const D2D1_POINT_2F middle = {walk.screen.width / 2, walk.screen.height / 2};
  const auto fit = [](float centre, float half, float whole) {
    return half * 2 >= whole ? whole / 2 : std::clamp(centre, half, whole - half);
  };
  shown.centre = {fit(Lerp(middle.x, target.x, follow), frame.width / 2 / shown.scale, walk.screen.width),
                  fit(Lerp(middle.y, target.y, follow), frame.height / 2 / shown.scale, walk.screen.height)};

  for (const Walkthrough::Step& step : walk.steps) {
    if (step.at <= time) shown.step = step.step;
  }
  return shown;
}

D2D1_RECT_F WalkView(const WalkFrame& frame, D2D1_SIZE_F size) {
  const float hw = size.width / 2 / frame.scale;
  const float hh = size.height / 2 / frame.scale;
  return {frame.centre.x - hw, frame.centre.y - hh, frame.centre.x + hw, frame.centre.y + hh};
}

}  // namespace knobs::tray
