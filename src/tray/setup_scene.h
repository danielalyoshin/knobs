// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <d2d1_1.h>
#include <dwrite_3.h>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tray/first_run.h"
#include "tray/setup_graphics.h"
#include "tray/setup_theme.h"
#include "tray/walkthrough.h"

// The setup window's page, laid out and drawn (docs/design.md, The setup
// window): the band with the mic's path, the panel with the page, and the
// foot with its note and buttons. Everything is in DIPs, in a client area
// kSetupWidth by kSetupHeight.
namespace knobs::tray {

inline constexpr float kSetupWidth = 760;
inline constexpr float kSetupHeight = 520;

// The check box's id, beside the buttons' and choices' (FirstRunId).
inline constexpr int kCheckBoxId = 300;
// The walkthrough's play and pause.
inline constexpr int kWalkToggleId = 301;
// The frame a guide's walkthrough plays in.
inline constexpr D2D1_SIZE_F kWalkFrame = {300, 188};

// What a part of the page is, for the keyboard, the mouse and screen
// readers.
enum class Role {
  kStageList,  // The band.
  kStage,
  kHeading,
  kText,
  kLink,
  kDoor,  // A command link.
  kChoiceGroup,
  kRadio,
  kStepList,
  kStep,
  kPicture,
  kNoteList,
  kNote,
  kChain,
  kCheckBox,
  kToggle,  // A toggle button: the walkthrough's play and pause.
  kStatus,  // What a waiting page waits for.
  kButton,
};

// A colour from the palette.
enum class Ink { kInk, kText2, kText3, kOnInk };

struct Element {
  Role role = Role::kText;
  int id = 0;       // A button's, door's or choice's (FirstRunId), or kCheckBoxId.
  int parent = -1;  // Its list or group, by index; -1 for the window.
  // In client DIPs, as laid out: the panel's parts move up as it scrolls.
  D2D1_RECT_F bounds{};
  bool in_panel = false;
  std::wstring name, help;
  std::wstring item_status;  // A stage's: "Set", "Needs you".
  std::string url;           // A link's.
  bool focusable = false;
  bool enabled = true;
  bool selected = false;  // A radio's, or the check box's.
  bool is_default = false;
  bool warning = false;   // A note's.
  // A link: the boxes of its words and its glyph, one per line, and its
  // underlines, as lines from left to right at top.
  std::vector<D2D1_RECT_F> rects;
  std::vector<D2D1_RECT_F> underlines;
  // What's written on a control.
  ComPtr<IDWriteTextLayout> label, note;
  D2D1_POINT_2F label_at{}, note_at{};
  // A button's, door's or check box's access key, upper case, and the line
  // under it in the label, shown while Windows shows access keys.
  wchar_t access_key = 0;
  std::optional<D2D1_RECT_F> access_line;
};

// Something drawn that isn't a control, in paint order.
struct Item {
  enum class Kind {
    kText,     // `text` at the rect's top left, in `ink`.
    kGroup,    // A box of choices: a surface with a strong border.
    kWell,     // A sunken box with a hairline border.
    kRule,     // A hairline along the rect's top.
    kIcon,     // `icon`, scaled to the rect, in `ink`.
    kMark,     // A badge, the rect's width across.
    kNumeral,  // Step `step`'s number: on an ink disc while the walkthrough shows it.
    kShownDisc,  // The number of the step the walkthrough shows, on an ink disc.
    kWalkthrough,  // The scene's walkthrough, framed.
    kDot,      // The waiting dot.
  };
  Kind kind = Kind::kText;
  D2D1_RECT_F rect{};
  Ink ink = Ink::kInk;
  ComPtr<IDWriteTextLayout> text;
  // Ranges of `text` in ink although the rest isn't: names in a UI.
  std::vector<DWRITE_TEXT_RANGE> ink_ranges;
  std::vector<DWRITE_TEXT_RANGE> link_ranges;
  Icon icon = Icon::kChevron;
  // kSet, kWarning or kBlocked, or kKnown for information.
  StageState mark = StageState::kSet;
  bool ring = false;  // A disc with a ring of the panel around it.
  int step = 0;       // A numeral's.
  bool in_panel = false;
};

struct StageBox {
  float x = 0;  // The icon's left.
  StageState state = StageState::kUnknown;
  ComPtr<IDWriteTextLayout> name, value;
  std::vector<DWRITE_TEXT_RANGE> value_ink;
};

struct Scene {
  std::array<StageBox, kStageCount> stages;
  std::optional<Stage> here;
  D2D1_RECT_F panel{};    // The panel's outside.
  D2D1_RECT_F content{};  // Where its content shows.
  float content_height = 0;
  std::vector<Item> items;
  std::vector<Element> elements;
  int default_element = -1;  // What Enter clicks.
  int heading = -1;
  int status = -1;
  bool waiting = false;
  D2D1_RECT_F dot{};  // The waiting dot, which moves by itself.
  // The steps' numbers for their discs, by step.
  std::vector<ComPtr<IDWriteTextLayout>> digits;
  // A guide's walkthrough, its frame in the panel, and its play and pause.
  const Walkthrough* walkthrough = nullptr;
  D2D1_RECT_F walk{};
  int walk_toggle = -1;

  float MaxScroll() const;
  // The control at `point` (client DIPs), or -1.
  int HitTest(D2D1_POINT_2F point, float scroll) const;
  // An element's box as shown, after `scroll`.
  D2D1_RECT_F Shown(const Element& element, float scroll) const;
  // Whether the panel shows it at `scroll`.
  bool Visible(const Element& element, float scroll) const;
};

// Lays out `view`, with the choice and the check box as the user has them.
Scene LayoutScene(const PageView& view, int choice, bool checked);

// What changes as the window is used, for the painter.
struct SceneState {
  int hover = -1;
  int pressed = -1;
  int focus = -1;
  bool show_focus = false;
  bool show_access_keys = false;
  float scroll = 0;
  // In motion: the caret's centre, how far the signal has got along each
  // stretch of wire (0 to 1), the page coming in (0 to 1), and the waiting
  // dot's strength.
  std::optional<float> caret_x;
  std::array<float, kStageCount - 1> wire{};
  float arrive = 1;
  float pulse = 1;
  // The moment of its walkthrough the page shows, and whether it's playing.
  std::optional<WalkFrame> walk;
  bool walk_playing = false;
};

class ScenePainter {
 public:
  // `pixel` is a device pixel in DIPs, for hairlines that stay sharp.
  // `picture` gives a walkthrough's screenshot, decoded for `target`.
  void Paint(ID2D1RenderTarget* target, const Scene& scene, const SetupPalette& palette, const SceneState& state,
             float pixel, const std::function<ID2D1Bitmap*(const std::string&)>& picture);
  // Brushes belong to a render target: forget them with it.
  void Reset();

 private:
  ComPtr<ID2D1RenderTarget> target_;
  ComPtr<ID2D1SolidColorBrush> brush_, ink_, link_;
  // The walkthrough's clip, kept from frame to frame: its layer belongs to
  // the target, and its shape to the frame's box.
  ComPtr<ID2D1Layer> walk_layer_;
  ComPtr<ID2D1RoundedRectangleGeometry> walk_clip_;
  D2D1_RECT_F walk_clip_box_{};
};

// The centre of a stage's icon, where the caret points.
float StageCentre(size_t stage);
// Whether the signal has got along the wire from stage `i` to the next:
// stage i is set, and the next is set or is what the page is about.
bool WireLive(const Scene& scene, size_t i);

}  // namespace knobs::tray
