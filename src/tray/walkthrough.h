// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <d2d1.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A guide's walkthrough (docs/design.md, The setup window): real screenshots
// of another app, with a camera that follows a pointer through the steps,
// clicking where the user will. The setup window plays it beside the steps,
// and the step it's showing lights up there. Times are in seconds, places in
// the screenshots' pixels: they're taken at twice the app's size, so half a
// DIP to a pixel shows it at its own size.
namespace knobs::tray {

struct Walkthrough {
  // A screenshot, shown from `at`: a picture resource, PNG or JPEG
  // (pictures.rc2), cropped to `crop` of the whole screen, which is as much
  // as the camera sees of it.
  struct Shot {
    float at = 0;
    std::string picture;
    D2D1_RECT_F crop{};
    float fade = 0.08f;  // How long it takes to come in.
  };
  struct Point {
    float at = 0;
    float x = 0, y = 0;
  };
  // How close the camera is, in DIPs to a pixel. 0 is far enough for the
  // whole screen.
  struct Zoom {
    float at = 0;
    float scale = 0;
  };
  struct Step {
    float at = 0;
    int step = 1;  // From 1.
  };
  // From `from` until `until`, the camera turns from the pointer to look at
  // (x, y): what a click opened.
  struct Look {
    float from = 0, until = 0;
    float x = 0, y = 0;
  };

  D2D1_SIZE_F screen{};  // The whole screen, which the first shot shows.
  std::vector<Shot> shots;
  std::vector<Point> pointer;  // Where it is at each time, eased between.
  std::vector<Zoom> zoom;
  std::vector<float> clicks;
  std::vector<Step> steps;
  std::vector<Look> looks;
  float length = 0;  // Then it starts again.
  float still = 0;   // What it shows with Windows' animations off.
  D2D1_COLOR_F ground{};  // Round the screen, where the frame shows more.
};

// The walkthrough of that name, or null.
const Walkthrough* WalkthroughNamed(std::string_view name);

// What a walkthrough shows at a moment.
struct WalkFrame {
  int shot = 0;
  int under = -1;  // The shot `shot` is coming in over, or -1.
  float fade = 1;  // How far it's come in.
  D2D1_POINT_2F pointer{};
  float press = 1;            // The pointer's size, smaller as it clicks.
  std::optional<float> ring;  // How far a click's ring has spread, 0 to 1.
  D2D1_POINT_2F centre{};     // What the camera looks at.
  float scale = 0;            // DIPs to a pixel.
  int step = 1;
};

// The walkthrough at `time` (it starts again after its length), in a frame
// `frame` DIPs big.
WalkFrame WalkAt(const Walkthrough& walk, float time, D2D1_SIZE_F frame);

// What the camera sees at that moment, in the screenshots' pixels.
D2D1_RECT_F WalkView(const WalkFrame& frame, D2D1_SIZE_F size);

// When step `step` first shows, once a screenshot coming in with it is in:
// what a click on it jumps to. None if the walkthrough doesn't show it.
std::optional<float> StepStart(const Walkthrough& walk, int step);

}  // namespace knobs::tray
