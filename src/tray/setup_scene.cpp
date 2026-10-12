// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/setup_scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <format>
#include <utility>

#include "tray/rich_text.h"
#include "util/win_strings.h"

namespace knobs::tray {
namespace {

// --- Measures (docs/design.md, The setup window) -------------------------------------

constexpr std::array<float, kStageCount> kStageX = {48, 220, 392, 564};
constexpr float kIcon = 21;
constexpr float kIconTop = 16;
constexpr float kWireY = kIconTop + kIcon / 2;  // Through the icons' centres.
constexpr float kNameTop = 43;
constexpr float kValueTop = 64;
constexpr float kValueWidth = 152;
constexpr float kBandHeight = 104;
constexpr D2D1_RECT_F kPanel = {24, 118, 736, 452};
// The panel's content: inside its border, 23 DIPs in at the sides, 19 at
// the top and 16 at the foot, so it lines up with the band's first icon.
constexpr D2D1_RECT_F kContent = {48, 138, 712, 435};
constexpr float kFootTop = 468;
constexpr float kButtonHeight = 32;
constexpr float kControlRadius = 4;
constexpr float kPanelRadius = 8;
// After each link: the icon fonts' "open in new window" glyph, the one
// Windows' own links to the web use.
constexpr wchar_t kOpenGlyph = L'\xE8A7';
constexpr float kOpenGlyphSize = 0.8f;  // Of the text's size.

float Width(const D2D1_RECT_F& r) { return r.right - r.left; }
float Height(const D2D1_RECT_F& r) { return r.bottom - r.top; }
D2D1_RECT_F Rect(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
D2D1_RECT_F Moved(D2D1_RECT_F r, float dx, float dy) { return {r.left + dx, r.top + dy, r.right + dx, r.bottom + dy}; }
D2D1_RECT_F Grown(D2D1_RECT_F r, float by) { return {r.left - by, r.top - by, r.right + by, r.bottom + by}; }
D2D1_RECT_F Union(D2D1_RECT_F a, D2D1_RECT_F b) {
  return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}
bool Inside(const D2D1_RECT_F& r, D2D1_POINT_2F p) {
  return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

// --- Text ---------------------------------------------------------------------------

struct Text {
  ComPtr<IDWriteTextLayout> layout;
  std::wstring text;  // With kOpenGlyph after each link.
  std::vector<TextSpan> spans;
  std::vector<uint32_t> glyphs;  // Where those are.
  float width = 0;
  float height = 0;
  float baseline = 0;  // From the top of each line.
};

TextStyle Style(float size, float line, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
  return {&Fonts().text, size, line, weight};
}

// Lays out `text`, markup unless `markup` is false, `width` DIPs wide.
// Unwrapped, each line is cut short with an ellipsis to fit.
Text MakeText(std::string_view text, const TextStyle& style, float width, bool wrap = true, bool markup = true) {
  Text made;
  RichText rich = markup ? ParseRichText(text) : RichText{FromUtf8(text), {}};
  // Typographic quotes for the copy's straight ones, one for one, so the
  // spans stay where they are: apostrophes, and double quotes that open
  // after a space or a bracket and close otherwise.
  for (size_t i = 0; i < rich.text.size(); ++i) {
    wchar_t& c = rich.text[i];
    if (c == L'\'') {
      c = L'\x2019';
    } else if (c == L'"') {
      const bool opening = i == 0 || iswspace(rich.text[i - 1]) || rich.text[i - 1] == L'(';
      c = opening ? L'\x201C' : L'\x201D';
    }
  }
  // The glyph after each link, if Windows has its icon font: the text after
  // it moves on by one.
  std::vector<uint32_t> inserts;
  for (const TextSpan& span : rich.spans) {
    if (span.kind == TextSpan::Kind::kLink && !Fonts().icons.empty()) inserts.push_back(span.start + span.length);
  }
  std::sort(inserts.begin(), inserts.end());
  const auto inserted = [&inserts](uint32_t at, bool at_too) {
    const auto end = at_too ? std::upper_bound(inserts.begin(), inserts.end(), at)
                            : std::lower_bound(inserts.begin(), inserts.end(), at);
    return static_cast<uint32_t>(end - inserts.begin());
  };
  size_t next = 0;
  for (size_t i = 0; i <= rich.text.size(); ++i) {
    while (next < inserts.size() && inserts[next] == i) {
      made.glyphs.push_back(static_cast<uint32_t>(made.text.size()));
      made.text += kOpenGlyph;
      ++next;
    }
    if (i < rich.text.size()) made.text += rich.text[i];
  }
  for (TextSpan span : rich.spans) {
    const uint32_t end = span.start + span.length;
    span.start += inserted(span.start, true);
    // A link takes its own glyph.
    const uint32_t moved_end = end + inserted(end, span.kind == TextSpan::Kind::kLink);
    span.length = moved_end - span.start;
    made.spans.push_back(std::move(span));
  }

  const ComPtr<IDWriteTextFormat> format = MakeTextFormat(style);
  IDWriteFactory3* factory = DWriteFactory();
  if (!format || !factory) return made;
  if (!wrap) {
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteInlineObject> ellipsis;
    factory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
    const DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    format->SetTrimming(&trimming, ellipsis.Get());
  }
  if (FAILED(factory->CreateTextLayout(made.text.c_str(), static_cast<UINT32>(made.text.size()), format.Get(), width,
                                       10000, &made.layout))) {
    return made;
  }
  for (const TextSpan& span : made.spans) {
    const DWRITE_TEXT_RANGE range = {span.start, span.length};
    if (span.kind == TextSpan::Kind::kBold) made.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
    if (span.kind == TextSpan::Kind::kCode) made.layout->SetFontFamilyName(Fonts().mono.c_str(), range);
  }
  // The glyph, a little smaller than the text and a little after it.
  ComPtr<IDWriteTextLayout1> spacing;
  made.layout.As(&spacing);
  for (const uint32_t at : made.glyphs) {
    made.layout->SetFontFamilyName(Fonts().icons.c_str(), {at, 1});
    made.layout->SetFontSize(style.size * kOpenGlyphSize, {at, 1});
    if (spacing) spacing->SetCharacterSpacing(2, 0, 0, {at, 1});
  }
  DWRITE_TEXT_METRICS metrics = {};
  made.layout->GetMetrics(&metrics);
  made.width = metrics.widthIncludingTrailingWhitespace;
  made.height = metrics.height;
  // Every line has the same baseline, with uniform spacing; asked for
  // fewer lines than there are, DirectWrite gives none.
  UINT32 count = 0;
  made.layout->GetLineMetrics(nullptr, 0, &count);
  std::vector<DWRITE_LINE_METRICS> lines(std::max(count, 1u));
  if (SUCCEEDED(made.layout->GetLineMetrics(lines.data(), static_cast<UINT32>(lines.size()), &count))) {
    made.baseline = lines.front().baseline;
  }
  return made;
}

std::wstring Plain(std::wstring text) {
  std::erase(text, kOpenGlyph);
  return text;
}

std::vector<DWRITE_TEXT_RANGE> Ranges(const Text& text, TextSpan::Kind kind) {
  std::vector<DWRITE_TEXT_RANGE> ranges;
  for (const TextSpan& span : text.spans) {
    if (span.kind == kind) ranges.push_back({span.start, span.length});
  }
  return ranges;
}

// A control's label, its access key marked with "&" as Windows writes it,
// laid out, with the line under the key for when Windows shows them.
struct Label {
  Text text;
  std::wstring name;  // Without the mark.
  wchar_t key = 0;    // Upper case.
  std::optional<D2D1_RECT_F> line;  // From the label's top left.
};

Label MakeLabel(std::string_view marked, const TextStyle& style, float width, bool wrap = true) {
  std::string plain;
  std::optional<size_t> key_at;
  for (size_t i = 0; i < marked.size(); ++i) {
    if (marked[i] == '&' && i + 1 < marked.size()) {
      ++i;
      if (marked[i] != '&' && !key_at) key_at = plain.size();
    }
    plain += marked[i];
  }
  Label label;
  label.text = MakeText(plain, style, width, wrap, false);
  label.name = Plain(label.text.text);
  if (key_at && static_cast<unsigned char>(plain[*key_at]) < 0x80 && label.text.layout) {
    label.key = static_cast<wchar_t>(towupper(static_cast<wint_t>(plain[*key_at])));
    // MakeText keeps the text's length, so the key is where it was.
    const auto at = static_cast<uint32_t>(FromUtf8(plain.substr(0, *key_at)).size());
    FLOAT x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS hit = {};
    label.text.layout->HitTestTextPosition(at, FALSE, &x, &y, &hit);
    label.line = Rect(hit.left, hit.top + label.text.baseline + 2, hit.width, 0);
  }
  return label;
}

// The label's access key on its control, at `at`.
void SetAccessKey(Element& element, const Label& label, D2D1_POINT_2F at) {
  element.access_key = label.key;
  if (label.line) element.access_line = Moved(*label.line, at.x, at.y);
}

// "Text\nNote": a door's or a choice's two lines.
std::pair<std::string, std::string> Lines(const std::string& text) {
  const size_t newline = text.find('\n');
  if (newline == std::string::npos) return {text, ""};
  return {text.substr(0, newline), text.substr(newline + 1)};
}

std::vector<std::string> Paragraphs(const std::string& content) {
  std::vector<std::string> paragraphs;
  size_t start = 0;
  while (start <= content.size() && !content.empty()) {
    const size_t end = content.find("\n\n", start);
    paragraphs.push_back(content.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 2;
  }
  return paragraphs;
}

// --- Building -----------------------------------------------------------------------

class Builder {
 public:
  explicit Builder(Scene& scene) : scene_(scene) {}

  int Add(Element element) {
    scene_.elements.push_back(std::move(element));
    return static_cast<int>(scene_.elements.size()) - 1;
  }
  Element& At(int index) { return scene_.elements[static_cast<size_t>(index)]; }

  size_t AddItem(Item item) {
    scene_.items.push_back(std::move(item));
    return scene_.items.size() - 1;
  }
  Item& ItemAt(size_t index) { return scene_.items[index]; }

  // A paragraph at (x, y), with an element for it and, after it, one for
  // each of its links. Returns the paragraph's element.
  int Paragraph(const Text& text, float x, float y, float width, Ink ink, bool panel, Role role = Role::kText,
                int parent = -1) {
    Item item;
    item.kind = Item::Kind::kText;
    item.rect = Rect(x, y, width, text.height);
    item.ink = ink;
    item.text = text.layout;
    item.in_panel = panel;
    if (ink != Ink::kInk) item.ink_ranges = Ranges(text, TextSpan::Kind::kBold);
    item.link_ranges = Ranges(text, TextSpan::Kind::kLink);
    AddItem(std::move(item));
    Element element;
    element.role = role;
    element.parent = parent;
    element.bounds = Rect(x, y, std::min(width, std::max(text.width, 1.0f)), text.height);
    element.in_panel = panel;
    element.name = Plain(text.text);
    const int index = Add(std::move(element));
    // A step's or a note's links are inside it, for screen readers.
    Links(text, x, y, panel, role == Role::kStep || role == Role::kNote ? index : parent);
    return index;
  }

 private:
  void Links(const Text& text, float x, float y, bool panel, int parent) {
    for (const TextSpan& span : text.spans) {
      if (span.kind != TextSpan::Kind::kLink) continue;
      Element link;
      link.role = Role::kLink;
      link.parent = parent;
      link.in_panel = panel;
      link.focusable = true;
      link.url = span.url;
      link.name = Plain(text.text.substr(span.start, span.length));
      const auto boxes = [&](uint32_t length) {
        UINT32 count = 0;
        text.layout->HitTestTextRange(span.start, length, x, y, nullptr, 0, &count);
        std::vector<DWRITE_HIT_TEST_METRICS> hits(count);
        text.layout->HitTestTextRange(span.start, length, x, y, hits.data(), count, &count);
        hits.resize(count);
        return hits;
      };
      // The glyph is part of the link, but only its words are underlined.
      const bool glyph = std::find(text.glyphs.begin(), text.glyphs.end(), span.start + span.length - 1) !=
                         text.glyphs.end();
      for (const DWRITE_HIT_TEST_METRICS& hit : boxes(span.length)) {
        link.rects.push_back(Rect(hit.left, hit.top, hit.width, hit.height));
      }
      for (DWRITE_HIT_TEST_METRICS hit : boxes(span.length - (glyph ? 1 : 0))) {
        // A line that wraps ends in a space, which isn't underlined.
        UINT32 length = hit.length;
        while (length > 0 && iswspace(text.text[hit.textPosition + length - 1])) --length;
        if (length == 0) continue;
        if (length < hit.length) {
          UINT32 count = 0;
          text.layout->HitTestTextRange(hit.textPosition, length, x, y, &hit, 1, &count);
        }
        link.underlines.push_back(Rect(hit.left, hit.top + text.baseline + 3, hit.width, 0));
      }
      if (link.rects.empty()) continue;
      link.bounds = link.rects.front();
      for (const D2D1_RECT_F& r : link.rects) link.bounds = Union(link.bounds, r);
      Add(std::move(link));
    }
  }

  Scene& scene_;
};

std::wstring StageName(size_t stage) {
  switch (static_cast<Stage>(stage)) {
    case Stage::kMic:
      return L"Your mic";
    case Stage::kFilters:
      return L"OBS's filters";
    case Stage::kCable:
      return L"Virtual cable";
    case Stage::kApps:
      return L"Your other apps";
  }
  return L"";
}

std::wstring StageStatus(StageState state, bool here) {
  std::wstring status;
  switch (state) {
    case StageState::kSet:
      status = L"Set";
      break;
    case StageState::kWarning:
      status = L"Set, with a warning";
      break;
    case StageState::kBlocked:
      status = L"Needs you";
      break;
    default:
      break;
  }
  if (here) status += status.empty() ? L"This page" : L"; this page";
  return status;
}

void LayoutBand(const PageView& view, Scene& scene, Builder& build) {
  scene.here = view.stage;
  Element list;
  list.role = Role::kStageList;
  list.bounds = {0, 0, kSetupWidth, kBandHeight};
  list.name = L"Your mic's path";
  const int list_index = build.Add(std::move(list));
  for (size_t i = 0; i < kStageCount; ++i) {
    StageBox& box = scene.stages[i];
    box.x = kStageX[i];
    box.state = view.stages[i].state;
    const std::wstring name = StageName(i);
    const Text title = MakeText(ToUtf8(name), Style(14, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD), kValueWidth + 8, false);
    const Text value = MakeText(view.stages[i].value, Style(12, 17), kValueWidth, false);
    box.name = title.layout;
    box.value = value.layout;
    box.value_ink = Ranges(value, TextSpan::Kind::kBold);
    Element stage;
    stage.role = Role::kStage;
    stage.parent = list_index;
    stage.bounds = Rect(box.x, kIconTop, kValueWidth, kValueTop + 34 - kIconTop);
    std::wstring said = Plain(value.text);
    for (size_t at = said.find(L'\n'); at != std::wstring::npos; at = said.find(L'\n', at)) said.replace(at, 1, L", ");
    stage.name = name + L": " + said;
    stage.item_status = StageStatus(box.state, view.stage && static_cast<size_t>(*view.stage) == i);
    build.Add(std::move(stage));
  }
}

float LayoutDoors(const PageView& view, Builder& build, Scene& scene, float y) {
  const float x = kContent.left;
  const float width = Width(kContent);
  for (const PageButton& button : view.buttons) {
    const auto [title, note] = Lines(button.text);
    const Label label = MakeLabel(title, Style(14, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD), width - 64);
    const Text& t = label.text;
    const Text n = MakeText(note, Style(13, 18), width - 64, true, false);
    const float height = std::max(62.0f, 11 + t.height + 2 + n.height + 11);
    Element door;
    door.role = Role::kDoor;
    door.id = button.id;
    door.bounds = Rect(x, y, width, height);
    door.in_panel = true;
    door.focusable = true;
    door.enabled = button.enabled;
    door.is_default = button.id == view.default_button;
    door.name = label.name;
    door.help = FromUtf8(note);
    door.label = t.layout;
    door.note = n.layout;
    const float text_top = y + (height - t.height - 2 - n.height) / 2;
    door.label_at = {x + 16, text_top};
    SetAccessKey(door, label, door.label_at);
    door.note_at = {x + 16, text_top + t.height + 2};
    const int index = build.Add(std::move(door));
    if (button.id == view.default_button && button.enabled) scene.default_element = index;
    y += height + 8;
  }
  return y - 8;
}

float LayoutChoices(const PageView& view, int choice, Builder& build, float y) {
  const float x = kContent.left;
  const float width = Width(kContent);
  // The group's box and the rules between its rows, under the radios.
  Item box;
  box.kind = Item::Kind::kGroup;
  box.in_panel = true;
  const size_t box_index = build.AddItem(box);
  Element group;
  group.role = Role::kChoiceGroup;
  group.in_panel = true;
  group.name = FromUtf8(view.instruction);
  const int group_index = build.Add(std::move(group));
  const float top = y;
  for (const PageButton& option : view.choices) {
    const auto [title, note] = Lines(option.text);
    const Text t = MakeText(title, Style(14, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD), width - 58, true, false);
    const Text n = MakeText(note, Style(13, 18), width - 58, true, false);
    const float height = 10 + t.height + (note.empty() ? 0 : n.height) + 10;
    if (y > top) {
      Item rule;
      rule.kind = Item::Kind::kRule;
      rule.rect = Rect(x + 1, y, width - 2, 1);
      rule.in_panel = true;
      build.AddItem(rule);
    }
    Element radio;
    radio.role = Role::kRadio;
    radio.id = option.id;
    radio.parent = group_index;
    radio.bounds = Rect(x + 1, y + 1, width - 2, height - 1);
    radio.in_panel = true;
    radio.focusable = true;
    radio.enabled = option.enabled;
    radio.selected = option.id == choice;
    radio.name = FromUtf8(title);
    radio.help = FromUtf8(note);
    radio.label = t.layout;
    radio.label_at = {x + 44, y + 10};
    if (!note.empty()) {
      radio.note = n.layout;
      radio.note_at = {x + 44, y + 10 + t.height};
    }
    build.Add(std::move(radio));
    y += height;
  }
  build.ItemAt(box_index).rect = Rect(x, top, width, y - top);
  build.At(group_index).bounds = Rect(x, top, width, y - top);
  return y;
}

// The steps, with the page's walkthrough of them beside them. The step it
// shows has its number on an ink disc, and so has the walkthrough's corner.
float LayoutGuide(const PageView& view, Builder& build, Scene& scene, float y) {
  const float x = kContent.left;
  const float width = Width(kContent);
  const Walkthrough* walk = view.walkthrough ? WalkthroughNamed(view.walkthrough->name) : nullptr;
  scene.walkthrough = walk;
  const float steps_width = walk ? width - 28 - kWalkFrame.width : width;
  Element list;
  list.role = Role::kStepList;
  list.in_panel = true;
  list.name = L"Steps";
  const int list_index = build.Add(std::move(list));
  const float top = y;
  scene.digits.assign(view.steps.size() + 1, nullptr);
  for (size_t i = 0; i < view.steps.size(); ++i) {
    const int number = static_cast<int>(i) + 1;
    // Plain, and on its disc.
    scene.digits[i + 1] = MakeText(std::to_string(number), Style(12, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD), 20).layout;
    Text digits = MakeText(std::to_string(number), Style(14, 21, DWRITE_FONT_WEIGHT_SEMI_BOLD), 20);
    if (digits.layout) digits.layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    build.AddItem({.kind = Item::Kind::kNumeral, .rect = Rect(x, y, 20, 21), .ink = Ink::kText3, .text = digits.layout,
                   .step = number, .in_panel = true});
    const Text text = MakeText(view.steps[i], Style(14, 21), steps_width - 30);
    const int step = build.Paragraph(text, x + 30, y, steps_width - 30, Ink::kInk, true, Role::kStep, list_index);
    build.At(step).name = std::format(L"{}. {}", number, build.At(step).name);
    y += text.height + 5;
  }
  y -= 5;
  build.At(list_index).bounds = Rect(x, top, steps_width, y - top);
  if (walk) {
    const D2D1_RECT_F at = Rect(x + width - kWalkFrame.width, top, kWalkFrame.width, kWalkFrame.height);
    scene.walk = at;
    build.AddItem({.kind = Item::Kind::kWalkthrough, .rect = at, .in_panel = true});
    build.AddItem({.kind = Item::Kind::kShownDisc, .rect = Rect(at.left - 7, at.top - 7, 20, 20), .ring = true,
                   .in_panel = true});
    Element image;
    image.role = Role::kPicture;
    image.bounds = at;
    image.in_panel = true;
    image.name = FromUtf8(view.walkthrough->alt);
    build.Add(std::move(image));
    // The whole frame plays and pauses it, as its corner shows.
    Element toggle;
    toggle.role = Role::kToggle;
    toggle.id = kWalkToggleId;
    toggle.bounds = at;
    toggle.in_panel = true;
    toggle.focusable = true;
    toggle.name = L"Play the walkthrough";
    toggle.access_key = L'P';
    scene.walk_toggle = build.Add(std::move(toggle));
    y = std::max(y, at.bottom);
  }
  return y;
}

float LayoutNotes(const PageView& view, Builder& build, float y) {
  const float x = kContent.left;
  Element list;
  list.role = Role::kNoteList;
  list.in_panel = true;
  list.name = FromUtf8(view.instruction);
  const int list_index = build.Add(std::move(list));
  const float top = y;
  for (const PageNote& note : view.notes) {
    Item mark;
    mark.kind = Item::Kind::kMark;
    mark.rect = Rect(x, y + 2.5f, 16, 16);
    mark.mark = note.warning ? StageState::kWarning : StageState::kKnown;
    mark.in_panel = true;
    build.AddItem(std::move(mark));
    const Text text = MakeText(note.text, Style(14, 21), Width(kContent) - 28);
    const int element = build.Paragraph(text, x + 28, y, Width(kContent) - 28, Ink::kInk, true, Role::kNote,
                                        list_index);
    build.At(element).warning = note.warning;
    y += text.height + 12;
  }
  y -= 12;
  build.At(list_index).bounds = Rect(x, top, Width(kContent), y - top);
  return y;
}

// The chain as it will run, in a well: the mic and the cable with their
// icons, the filters between them, wrapping as they need.
float LayoutChain(const core::ChainSummary& chain, Builder& build, float y) {
  const float x = kContent.left;
  const float width = Width(kContent);
  const size_t well = build.AddItem({.kind = Item::Kind::kWell, .in_panel = true});
  struct Piece {
    std::optional<Icon> icon;
    Text text;
    bool end = false;
  };
  std::vector<Piece> pieces;
  const TextStyle end = Style(13, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  pieces.push_back({Icon::kMic, MakeText(chain.mic, end, width, false, false), true});
  for (const std::string& filter : chain.filters) {
    pieces.push_back({std::nullopt, MakeText(filter, Style(13, 20), width, false, false)});
  }
  if (!chain.cable.empty()) pieces.push_back({Icon::kCable, MakeText(chain.cable, end, width, false, false), true});
  const float left = x + 12;
  const float right = x + width - 12;
  float px = left;
  float py = y + 7;
  std::wstring said;
  for (size_t i = 0; i < pieces.size(); ++i) {
    Piece& piece = pieces[i];
    const float piece_width = (piece.icon ? 22.0f : 0.0f) + piece.text.width;
    const float separator = i == 0 ? 0.0f : 6 + 12 + 6;
    if (i > 0 && px + separator + piece_width > right) {
      px = left;
      py += 22;
    } else if (i > 0) {
      build.AddItem({.kind = Item::Kind::kIcon, .rect = Rect(px + 6, py + 4, 12, 12), .ink = Ink::kText3,
                     .icon = Icon::kChevron, .in_panel = true});
      px += separator;
    }
    if (piece.icon) {
      build.AddItem({.kind = Item::Kind::kIcon, .rect = Rect(px, py + 2, 16, 16), .ink = Ink::kInk,
                     .icon = *piece.icon, .in_panel = true});
      px += 22;
    }
    piece.text.layout->SetMaxWidth(piece.text.width + 1);
    build.AddItem({.kind = Item::Kind::kText, .rect = Rect(px, py, piece.text.width + 1, 20),
                   .ink = piece.end ? Ink::kInk : Ink::kText2, .text = piece.text.layout, .in_panel = true});
    px += piece.text.width;
    said += (said.empty() ? L"" : L", then ") + Plain(piece.text.text);
  }
  const float bottom = py + 20 + 7;
  build.ItemAt(well).rect = Rect(x, y, width, bottom - y);
  Element element;
  element.role = Role::kChain;
  element.bounds = Rect(x, y, width, bottom - y);
  element.in_panel = true;
  element.name = said;
  build.Add(std::move(element));
  return bottom;
}

float LayoutCheck(const PageView& view, bool checked, Builder& build, float y) {
  const Label label = MakeLabel(view.check, Style(14, 20), Width(kContent) - 28);
  Element check;
  check.role = Role::kCheckBox;
  check.id = kCheckBoxId;
  check.bounds = Rect(kContent.left, y, 28 + label.text.width, std::max(20.0f, label.text.height));
  check.in_panel = true;
  check.focusable = true;
  check.selected = checked;
  check.name = label.name;
  check.label = label.text.layout;
  check.label_at = {kContent.left + 28, y};
  SetAccessKey(check, label, check.label_at);
  build.Add(std::move(check));
  return y + std::max(20.0f, label.text.height);
}

void LayoutFoot(const PageView& view, Builder& build, Scene& scene) {
  // The buttons, right-aligned, in the order the page lists them.
  float right = kPanel.right;
  std::vector<Element> buttons;
  for (auto it = view.buttons.rbegin(); it != view.buttons.rend(); ++it) {
    const bool primary = it->id == view.default_button && it->enabled;
    const DWRITE_FONT_WEIGHT weight = primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    const Label marked = MakeLabel(it->text, Style(14, 20, weight), 400, false);
    const Text& label = marked.text;
    const float width = std::max(84.0f, std::ceil(label.width) + 32);
    Element button;
    button.role = Role::kButton;
    button.id = it->id;
    button.bounds = Rect(right - width, kFootTop, width, kButtonHeight);
    button.focusable = true;
    button.enabled = it->enabled;
    button.is_default = primary;
    button.name = marked.name;
    button.label = label.layout;
    button.label_at = {right - width + (width - label.width) / 2, kFootTop + (kButtonHeight - label.height) / 2};
    SetAccessKey(button, marked, button.label_at);
    buttons.push_back(std::move(button));
    right -= width + 8;
  }
  // The note and what the page waits for, beside them, centred on them. A
  // page that waits shows the dot, with what it waits for if it says.
  const float note_left = kPanel.left;
  const float note_width = right + 8 - 24 - note_left;
  const bool status = view.waiting && !view.status.empty();
  const Text waiting = MakeText(status ? view.status : "", Style(13, 18), note_width - 15, true, false);
  const Text footer = MakeText(view.footer, Style(12, 17), note_width);
  const float status_height = status ? waiting.height : view.waiting ? 18.0f : 0.0f;
  const float height = status_height + (status_height > 0 && !view.footer.empty() ? 2 : 0) +
                       (view.footer.empty() ? 0 : footer.height);
  float y = kFootTop + (kButtonHeight - height) / 2;
  if (view.waiting) {
    build.AddItem({.kind = Item::Kind::kDot, .rect = Rect(note_left, y + 5.5f, 7, 7)});
    scene.waiting = true;
    scene.dot = Rect(note_left, y + 5.5f, 7, 7);
    if (status) {
      scene.status = build.Paragraph(waiting, note_left + 15, y, note_width - 15, Ink::kText2, false, Role::kStatus);
    }
    y += status_height + 2;
  }
  if (!view.footer.empty()) build.Paragraph(footer, note_left, y, note_width, Ink::kText3, false);
  for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
    const bool primary = it->is_default;
    const int index = build.Add(std::move(*it));
    if (primary) scene.default_element = index;
  }
}

// --- Painting -------------------------------------------------------------------------

D2D1_COLOR_F Color(const SetupPalette& palette, Ink ink) {
  switch (ink) {
    case Ink::kInk:
      return palette.ink;
    case Ink::kText2:
      return palette.text2;
    case Ink::kText3:
      return palette.text3;
    case Ink::kOnInk:
      return palette.on_ink;
  }
  return palette.ink;
}

ID2D1Geometry* CachedIcon(Icon icon, bool crisp) {
  static std::array<ComPtr<ID2D1Geometry>, 14> cache;
  ComPtr<ID2D1Geometry>& slot = cache[static_cast<size_t>(icon) * 2 + (crisp ? 1 : 0)];
  if (!slot) slot = crisp ? CrispIconGeometry(icon) : IconGeometry(icon);
  return slot.Get();
}

ID2D1Geometry* WarningTriangle() {
  static const ComPtr<ID2D1PathGeometry> triangle = PathGeometry("M0 -5.6 5.8 4.6H-5.8z", true);
  return triangle.Get();
}

// Windows' arrow pointer, 19 units tall, its tip at the origin.
ID2D1Geometry* PointerArrow() {
  static const ComPtr<ID2D1PathGeometry> arrow = PathGeometry("M0 0V16L4 12.4 6.6 18.6 8.8 17.6 6.3 11.7H11.4z", true);
  return arrow.Get();
}

// Play, centred on the origin.
ID2D1Geometry* PlayTriangle() {
  static const ComPtr<ID2D1PathGeometry> triangle = PathGeometry("M-3.5 -5.5 6 0-3.5 5.5z", true);
  return triangle.Get();
}

class Painter {
 public:
  Painter(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, const SetupPalette& palette, float pixel)
      : target_(target), brush_(brush), palette_(palette), pixel_(pixel) {
    // Hairlines: a whole number of device pixels, about a DIP.
    hair_ = pixel * std::max(1.0f, std::floor(1 / pixel + 0.01f));
  }

  ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& color, float opacity = 1) {
    brush_->SetColor(color);
    brush_->SetOpacity(opacity);
    return brush_;
  }

  // An edge, on the device pixel grid.
  float Snap(float v) const { return std::round(v / pixel_) * pixel_; }

  // A hairline along y, from x0 to x1.
  void HLine(float x0, float x1, float y, const D2D1_COLOR_F& color) {
    const float top = Snap(y);
    target_->FillRectangle({Snap(x0), top, Snap(x1), top + hair_}, Brush(color));
  }

  void Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& color, float radius = 0, float opacity = 1) {
    if (radius > 0) {
      target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(color, opacity));
    } else {
      target_->FillRectangle(r, Brush(color, opacity));
    }
  }

  // A hairline border inside `r`'s outer edge, which sits on the pixel grid.
  void Border(const D2D1_RECT_F& r, const D2D1_COLOR_F& color, float radius) {
    const float half = hair_ / 2;
    const D2D1_RECT_F edge = {Snap(r.left) + half, Snap(r.top) + half, Snap(r.right) - half, Snap(r.bottom) - half};
    target_->DrawRoundedRectangle(D2D1::RoundedRect(edge, radius - half, radius - half), Brush(color), hair_);
  }

  void DrawIcon(Icon icon, const D2D1_RECT_F& box, const D2D1_COLOR_F& color) {
    const float scale = Width(box) / IconSize(icon);
    // At 100%, at full size on whole pixels, a stage icon is redrawn for a
    // sharp 1-pixel stroke, as dark as the wire.
    if (std::abs(pixel_ - 1) < 0.01f && scale == 1 && box.left == std::floor(box.left) &&
        box.top == std::floor(box.top)) {
      if (ID2D1Geometry* crisp = CachedIcon(icon, true)) {
        D2D1_MATRIX_3X2_F before;
        target_->GetTransform(&before);
        target_->SetTransform(D2D1::Matrix3x2F::Translation(box.left, box.top) * before);
        target_->DrawGeometry(crisp, Brush(color), 1, RoundStroke());
        target_->SetTransform(before);
        return;
      }
    }
    ID2D1Geometry* geometry = CachedIcon(icon, false);
    if (!geometry) return;
    D2D1_MATRIX_3X2_F before;
    target_->GetTransform(&before);
    target_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(box.left, box.top) *
                          before);
    target_->DrawGeometry(geometry, Brush(color), 1.5f / scale, RoundStroke());
    target_->SetTransform(before);
  }

  // The tray icon's badges: `diameter` across at `centre`, with a ring of
  // `ground` round them if given.
  void DrawBadge(StageState kind, D2D1_POINT_2F c, float diameter, const D2D1_COLOR_F* ground) {
    const float s = diameter / 11;
    const auto at = [&](float x, float y) { return D2D1::Point2F(c.x + x * s, c.y + y * s); };
    const auto dot = [&](float x, float y, float r, const D2D1_COLOR_F& color) {
      target_->FillEllipse(D2D1::Ellipse(at(x, y), r * s, r * s), Brush(color));
    };
    const auto line = [&](float x0, float y0, float x1, float y1, const D2D1_COLOR_F& color) {
      target_->DrawLine(at(x0, y0), at(x1, y1), Brush(color), 1.4f * s, RoundStroke());
    };
    if (kind == StageState::kWarning) {
      D2D1_MATRIX_3X2_F before;
      target_->GetTransform(&before);
      target_->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(c.x, c.y) * before);
      // A clear ring of a DIP beyond its stroke, as round the discs.
      if (ground) target_->DrawGeometry(WarningTriangle(), Brush(*ground), 1.5f + 2 / s, RoundStroke());
      target_->FillGeometry(WarningTriangle(), Brush(palette_.badge_warning));
      target_->DrawGeometry(WarningTriangle(), Brush(palette_.badge_warning), 1.5f, RoundStroke());
      target_->SetTransform(before);
      line(0, -1.5f, 0, 1.1f, palette_.badge_glyph_dark);
      dot(0, 3.1f, 0.8f, palette_.badge_glyph_dark);
      return;
    }
    // The tray badge's clear ring: a DIP round the disc.
    if (ground) target_->FillEllipse(D2D1::Ellipse(c, 5.5f * s + 1, 5.5f * s + 1), Brush(*ground));
    switch (kind) {
      case StageState::kSet:
        dot(0, 0, 5.5f, palette_.badge_ok);
        line(-2.5f, 0.2f, -0.8f, 1.9f, palette_.badge_glyph);
        line(-0.8f, 1.9f, 2.3f, -1.5f, palette_.badge_glyph);
        break;
      case StageState::kBlocked:
        dot(0, 0, 5.5f, palette_.badge_stop);
        line(0, -2.8f, 0, 0.3f, palette_.badge_glyph);
        dot(0, 2.6f, 0.85f, palette_.badge_glyph);
        break;
      default:  // Information.
        dot(0, 0, 5.5f, palette_.badge_info);
        dot(0, -2.35f, 0.75f, palette_.badge_glyph);
        line(0, -0.45f, 0, 2.75f, palette_.badge_glyph);
        break;
    }
  }

  // An ink disc with a number on it, the number's glyphs centred on it.
  void Disc(const D2D1_RECT_F& box, IDWriteTextLayout* numeral, bool ring) {
    const D2D1_POINT_2F c = {(box.left + box.right) / 2, (box.top + box.bottom) / 2};
    const float r = Width(box) / 2;
    if (ring) target_->FillEllipse(D2D1::Ellipse(c, r + 2, r + 2), Brush(palette_.surface));
    target_->FillEllipse(D2D1::Ellipse(c, r, r), Brush(palette_.ink));
    if (!numeral) return;
    DWRITE_OVERHANG_METRICS over = {};
    numeral->GetOverhangMetrics(&over);
    const float ink_left = -over.left;
    const float ink_right = numeral->GetMaxWidth() + over.right;
    const float ink_top = -over.top;
    const float ink_bottom = numeral->GetMaxHeight() + over.bottom;
    target_->DrawTextLayout({c.x - (ink_left + ink_right) / 2, c.y - (ink_top + ink_bottom) / 2}, numeral,
                            Brush(palette_.on_ink));
  }

  // A walkthrough at a moment, in `box`, clipped to `clip` in `layer`: its
  // shots through the camera, then the pointer and its click on top.
  // `picture` gives the shots.
  void Walkthrough(const knobs::tray::Walkthrough& walk, const WalkFrame& frame, const D2D1_RECT_F& box,
                   const std::function<ID2D1Bitmap*(const std::string&)>& picture, ID2D1Layer* layer,
                   ID2D1Geometry* clip) {
    if (!layer || !clip) return;
    const bool contrast = palette_.mode == SetupMode::kHighContrast;
    target_->PushLayer(D2D1::LayerParameters(box, clip), layer);
    target_->FillRectangle(box, Brush(walk.ground));
    D2D1_MATRIX_3X2_F before;
    target_->GetTransform(&before);
    const D2D1_POINT_2F mid = {(box.left + box.right) / 2, (box.top + box.bottom) / 2};
    target_->SetTransform(D2D1::Matrix3x2F::Translation(-frame.centre.x, -frame.centre.y) *
                          D2D1::Matrix3x2F::Scale(frame.scale, frame.scale) *
                          D2D1::Matrix3x2F::Translation(mid.x, mid.y) * before);
    // Smooth as it shrinks: the shots are drawn at a fifth to a half of
    // their pixels.
    ComPtr<ID2D1DeviceContext> context;
    target_->QueryInterface(IID_PPV_ARGS(&context));
    for (const int index : {frame.under, frame.shot}) {
      if (index < 0 || static_cast<size_t>(index) >= walk.shots.size()) continue;
      const knobs::tray::Walkthrough::Shot& shot = walk.shots[static_cast<size_t>(index)];
      ID2D1Bitmap* bitmap = picture ? picture(shot.picture) : nullptr;
      if (!bitmap) continue;
      const float opacity = index == frame.shot ? frame.fade : 1;
      if (context) {
        context->DrawBitmap(bitmap, &shot.crop, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
      } else {
        target_->DrawBitmap(bitmap, shot.crop, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      }
    }
    target_->SetTransform(before);
    const D2D1_POINT_2F tip = {mid.x + (frame.pointer.x - frame.centre.x) * frame.scale,
                               mid.y + (frame.pointer.y - frame.centre.y) * frame.scale};
    // In high contrast, the click's ring is the user's highlight, and the
    // pointer their window's colours.
    if (frame.ring) {
      const float spread = 1 - (1 - *frame.ring) * (1 - *frame.ring);
      const float r = 4 + 16 * spread;
      target_->DrawEllipse(D2D1::Ellipse(tip, r, r),
                           Brush(contrast ? palette_.here : D2D1::ColorF(D2D1::ColorF::White), 0.75f * (1 - *frame.ring)),
                           2);
    }
    // As big as Windows' pointer on the app at its own size, and never
    // smaller than most of that.
    const float u = std::max(38 * frame.scale, 15.0f) / 19 * frame.press;
    if (ID2D1Geometry* arrow = PointerArrow()) {
      target_->SetTransform(D2D1::Matrix3x2F::Scale(u, u) * D2D1::Matrix3x2F::Translation(tip.x, tip.y) * before);
      target_->FillGeometry(arrow, Brush(contrast ? palette_.surface : D2D1::ColorF(D2D1::ColorF::White)));
      target_->DrawGeometry(arrow, Brush(contrast ? palette_.ink : D2D1::ColorF(D2D1::ColorF::Black)), 1, RoundStroke());
      target_->SetTransform(before);
    }
    target_->PopLayer();
  }

  // The walkthrough's play or pause, on a dark disc in its frame's corner.
  // In high contrast, the disc is the user's window colour, ringed and marked
  // with their text colour, and their highlight under the pointer.
  void WalkToggle(const D2D1_RECT_F& frame, bool playing, bool hover) {
    const D2D1_POINT_2F c = {frame.right - 19, frame.bottom - 19};
    D2D1_COLOR_F glyph = D2D1::ColorF(D2D1::ColorF::White);
    if (palette_.mode == SetupMode::kHighContrast) {
      target_->FillEllipse(D2D1::Ellipse(c, 13, 13), Brush(hover ? palette_.here : palette_.surface));
      target_->DrawEllipse(D2D1::Ellipse(c, 12.5f, 12.5f), Brush(palette_.ink), 1);
      glyph = hover ? palette_.on_ink : palette_.ink;
    } else {
      target_->FillEllipse(D2D1::Ellipse(c, 13, 13), Brush(D2D1::ColorF(0x111317), hover ? 0.92f : 0.72f));
    }
    if (playing) {
      ID2D1SolidColorBrush* bars = Brush(glyph);
      target_->FillRectangle(Rect(c.x - 4.5f, c.y - 5, 3, 10), bars);
      target_->FillRectangle(Rect(c.x + 1.5f, c.y - 5, 3, 10), bars);
    } else if (ID2D1Geometry* play = PlayTriangle()) {
      D2D1_MATRIX_3X2_F before;
      target_->GetTransform(&before);
      target_->SetTransform(D2D1::Matrix3x2F::Translation(c.x, c.y) * before);
      target_->FillGeometry(play, Brush(glyph));
      target_->SetTransform(before);
    }
  }

  void FocusRing(const D2D1_RECT_F& r, float radius) {
    target_->DrawRoundedRectangle(D2D1::RoundedRect(Grown(r, 3), radius + 3, radius + 3), Brush(palette_.ink), 2);
  }

  ID2D1RenderTarget* target() const { return target_; }
  float hair() const { return hair_; }
  const SetupPalette& palette() const { return palette_; }

 private:
  ID2D1RenderTarget* target_;
  ID2D1SolidColorBrush* brush_;
  const SetupPalette& palette_;
  float pixel_;
  float hair_ = 1;
};

}  // namespace

// --- Scene ----------------------------------------------------------------------------

float Scene::MaxScroll() const { return std::max(0.0f, content_height - Height(content)); }

D2D1_RECT_F Scene::Shown(const Element& element, float scroll) const {
  return element.in_panel ? Moved(element.bounds, 0, -scroll) : element.bounds;
}

bool Scene::Visible(const Element& element, float scroll) const {
  if (!element.in_panel) return true;
  const D2D1_RECT_F shown = Shown(element, scroll);
  return shown.bottom > content.top - 19 && shown.top < content.bottom + 16;
}

int Scene::HitTest(D2D1_POINT_2F point, float scroll) const {
  const D2D1_RECT_F inside_panel = Grown(panel, -1);
  for (int i = static_cast<int>(elements.size()) - 1; i >= 0; --i) {
    const Element& element = elements[static_cast<size_t>(i)];
    if (!element.focusable) continue;
    if (element.in_panel && !Inside(inside_panel, point)) continue;
    const float dy = element.in_panel ? -scroll : 0;
    if (element.role == Role::kLink) {
      for (const D2D1_RECT_F& r : element.rects) {
        if (Inside(Moved(r, 0, dy), point)) return i;
      }
    } else if (Inside(Moved(element.bounds, 0, dy), point)) {
      return i;
    }
  }
  return -1;
}

Scene LayoutScene(const PageView& view, int choice, bool checked) {
  Scene scene;
  scene.panel = kPanel;
  scene.content = kContent;
  Builder build(scene);
  LayoutBand(view, scene, build);

  const float x = kContent.left;
  const float width = Width(kContent);
  float y = kContent.top;
  const Text heading =
      MakeText(view.instruction, {&Fonts().display, 22, 28, DWRITE_FONT_WEIGHT_SEMI_BOLD}, width, true, false);
  scene.heading = build.Paragraph(heading, x, y, width, Ink::kInk, true, Role::kHeading);
  y += heading.height;
  if (view.chain) y = LayoutChain(*view.chain, build, y + 12);
  // The first paragraph is the page's lead, in secondary text, and the rest
  // say what to do. Under the chain it's the other way round: the first
  // says what to do, and the rest are notes.
  const std::vector<std::string> paragraphs = Paragraphs(view.content);
  for (size_t i = 0; i < paragraphs.size(); ++i) {
    const bool lead = view.chain ? i > 0 : i == 0;
    y += i == 0 ? (view.chain ? 12 : 6) : (lead ? 6 : 12);
    const Text text = MakeText(paragraphs[i], Style(14, 21), width);
    build.Paragraph(text, x, y, width, lead ? Ink::kText2 : Ink::kInk, true);
    y += text.height;
  }
  if (view.command_links) y = LayoutDoors(view, build, scene, y + 20);
  if (!view.choices.empty()) y = LayoutChoices(view, choice, build, y + 16);
  if (!view.steps.empty()) y = LayoutGuide(view, build, scene, y + 14);
  if (!view.notes.empty()) y = LayoutNotes(view, build, y + 16);
  if (!view.check.empty()) y = LayoutCheck(view, checked, build, y + 18);
  scene.content_height = y - kContent.top;
  if (!view.command_links) LayoutFoot(view, build, scene);
  else if (!view.footer.empty()) {
    const Text footer = MakeText(view.footer, Style(12, 17), Width(kPanel));
    build.Paragraph(footer, kPanel.left, kFootTop + (kButtonHeight - footer.height) / 2, Width(kPanel), Ink::kText3,
                    false);
  }
  return scene;
}

float StageCentre(size_t stage) { return kStageX[stage] + kIcon / 2; }

bool WireLive(const Scene& scene, size_t i) {
  const auto reached = [&scene](size_t s) {
    return scene.stages[s].state == StageState::kSet || scene.stages[s].state == StageState::kWarning;
  };
  return reached(i) && (reached(i + 1) || (scene.here && static_cast<size_t>(*scene.here) == i + 1));
}

void ScenePainter::Reset() {
  target_.Reset();
  brush_.Reset();
  ink_.Reset();
  link_.Reset();
  walk_layer_.Reset();
}

void ScenePainter::Paint(ID2D1RenderTarget* target, const Scene& scene, const SetupPalette& palette,
                         const SceneState& state, float pixel,
                         const std::function<ID2D1Bitmap*(const std::string&)>& picture) {
  if (target_.Get() != target) {
    Reset();
    target_ = target;
  }
  if (!brush_) target->CreateSolidColorBrush(palette.ink, &brush_);
  if (!ink_) target->CreateSolidColorBrush(palette.ink, &ink_);
  if (!link_) target->CreateSolidColorBrush(palette.link, &link_);
  if (!brush_ || !ink_ || !link_) return;
  ink_->SetColor(palette.ink);
  link_->SetColor(palette.link);
  Painter p(target, brush_.Get(), palette, pixel);
  target->Clear(palette.paper);
  const std::optional<WalkFrame> walk = scene.walkthrough ? state.walk : std::nullopt;
  const int shown_step = walk ? walk->step : 0;
  if (walk) {
    if (!walk_layer_) target->CreateLayer(&walk_layer_);
    if (!walk_clip_ || std::memcmp(&walk_clip_box_, &scene.walk, sizeof(D2D1_RECT_F)) != 0) {
      walk_clip_.Reset();
      if (D2DFactory()) {
        D2DFactory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(scene.walk, kControlRadius, kControlRadius),
                                                     &walk_clip_);
      }
      walk_clip_box_ = scene.walk;
    }
  }
  const auto digits = [&scene](int step) {
    return step > 0 && static_cast<size_t>(step) < scene.digits.size() ? scene.digits[static_cast<size_t>(step)].Get()
                                                                       : nullptr;
  };

  // The band.
  p.Fill({0, 0, kSetupWidth, kBandHeight}, palette.paper_subtle);
  p.HLine(0, kSetupWidth, kBandHeight, palette.line);
  for (size_t i = 0; i + 1 < kStageCount; ++i) {
    const float from = kStageX[i] + kIcon + 6;
    const float to = kStageX[i + 1] - 7;
    const float tip = kStageX[i + 1] - 6.76f;
    const float lit = from + (to - from) * std::clamp(state.wire[i], 0.0f, 1.0f);
    const float y = kWireY - p.hair() / 2;
    if (lit > from) p.HLine(from, lit, y, palette.ink);
    if (lit < to) p.HLine(lit, to, y, palette.text3);
    ID2D1SolidColorBrush* head = p.Brush(state.wire[i] >= 1 ? palette.ink : palette.text3);
    target->DrawLine({tip - 4.6f, kWireY - 4.6f}, {tip, kWireY}, head, 1, SharpStroke());
    target->DrawLine({tip, kWireY}, {tip - 4.6f, kWireY + 4.6f}, head, 1, SharpStroke());
  }
  for (size_t i = 0; i < kStageCount; ++i) {
    const StageBox& box = scene.stages[i];
    const bool here = scene.here && static_cast<size_t>(*scene.here) == i;
    const D2D1_COLOR_F icon = here ? palette.here : box.state == StageState::kUnknown ? palette.text3 : palette.text2;
    p.DrawIcon(static_cast<Icon>(i), Rect(box.x, kIconTop, kIcon, kIcon), icon);
    if (box.state == StageState::kSet || box.state == StageState::kWarning || box.state == StageState::kBlocked) {
      // As on the tray icon: 7/16 of the icon, flush with its corner.
      p.DrawBadge(box.state, {box.x + kIcon - 4.5f, kIconTop + kIcon - 4.5f}, 9, &palette.paper_subtle);
    }
    if (box.name) {
      target->DrawTextLayout({box.x, kNameTop}, box.name.Get(), p.Brush(here ? palette.here : palette.text2));
    }
    if (box.value) {
      for (const DWRITE_TEXT_RANGE& range : box.value_ink) box.value->SetDrawingEffect(ink_.Get(), range);
      target->DrawTextLayout({box.x, kValueTop}, box.value.Get(),
                             p.Brush(box.state == StageState::kUnknown ? palette.text3 : palette.text2));
    }
  }

  // The panel, with its one-pixel lift.
  if (palette.lift_opacity > 0) p.Fill(Moved(kPanel, 0, 1), {0, 0, 0, 1}, kPanelRadius, palette.lift_opacity);
  p.Fill(kPanel, palette.surface, kPanelRadius);
  p.Border(kPanel, palette.line_strong, kPanelRadius);
  // The caret: the panel's edge rising to a point under the stage.
  if (state.caret_x) {
    const float cx = *state.caret_x;
    const float edge = p.Snap(kPanel.top) + p.hair() / 2;
    ComPtr<ID2D1PathGeometry> caret;
    ComPtr<ID2D1GeometrySink> sink;
    if (D2DFactory() && SUCCEEDED(D2DFactory()->CreatePathGeometry(&caret)) && SUCCEEDED(caret->Open(&sink))) {
      sink->BeginFigure({cx - 6.5f, edge + p.hair()}, D2D1_FIGURE_BEGIN_FILLED);
      sink->AddLine({cx, edge - 6.5f});
      sink->AddLine({cx + 6.5f, edge + p.hair()});
      sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      sink->Close();
      target->FillGeometry(caret.Get(), p.Brush(palette.surface));
    }
    target->DrawLine({cx - 6, edge}, {cx, edge - 6}, p.Brush(palette.ink), 1, SharpStroke());
    target->DrawLine({cx, edge - 6}, {cx + 6, edge}, p.Brush(palette.ink), 1, SharpStroke());
  }

  const auto paint_item = [&](const Item& item) {
    switch (item.kind) {
      case Item::Kind::kText:
        if (!item.text) break;
        for (const DWRITE_TEXT_RANGE& range : item.ink_ranges) item.text->SetDrawingEffect(ink_.Get(), range);
        for (const DWRITE_TEXT_RANGE& range : item.link_ranges) item.text->SetDrawingEffect(link_.Get(), range);
        target->DrawTextLayout({item.rect.left, item.rect.top}, item.text.Get(), p.Brush(Color(palette, item.ink)));
        break;
      case Item::Kind::kGroup:
        p.Fill(item.rect, palette.surface, kControlRadius);
        p.Border(item.rect, palette.line_strong, kControlRadius);
        break;
      case Item::Kind::kWell:
        p.Fill(item.rect, palette.sunken, kControlRadius);
        p.Border(item.rect, palette.line, kControlRadius);
        break;
      case Item::Kind::kRule:
        p.HLine(item.rect.left, item.rect.right, item.rect.top, palette.line);
        break;
      case Item::Kind::kIcon:
        p.DrawIcon(item.icon, item.rect, Color(palette, item.ink));
        break;
      case Item::Kind::kMark:
        p.DrawBadge(item.mark, {(item.rect.left + item.rect.right) / 2, (item.rect.top + item.rect.bottom) / 2},
                Width(item.rect) * 14.5f / 16, nullptr);
        break;
      case Item::Kind::kNumeral:
        if (item.step == shown_step) {
          p.Disc(Rect(item.rect.left, item.rect.top + 0.5f, 20, 20), digits(item.step), false);
        } else if (item.text) {
          target->DrawTextLayout({item.rect.left, item.rect.top}, item.text.Get(), p.Brush(Color(palette, item.ink)));
        }
        break;
      case Item::Kind::kShownDisc:
        if (shown_step > 0) p.Disc(item.rect, digits(shown_step), item.ring);
        break;
      case Item::Kind::kWalkthrough:
        if (walk) p.Walkthrough(*scene.walkthrough, *walk, item.rect, picture, walk_layer_.Get(), walk_clip_.Get());
        p.Border(item.rect, palette.line_strong, kControlRadius);
        break;
      case Item::Kind::kDot:
        target->FillEllipse(D2D1::Ellipse({(item.rect.left + item.rect.right) / 2, (item.rect.top + item.rect.bottom) / 2},
                                          3.5f, 3.5f),
                            p.Brush(palette.text3, 0.35f + 0.65f * state.pulse));
        break;
    }
  };

  // An access key's line, while Windows shows them.
  const auto access_line = [&](const Element& e, const D2D1_COLOR_F& color) {
    if (state.show_access_keys && e.access_line) {
      p.HLine(e.access_line->left, e.access_line->right, e.access_line->top, color);
    }
  };

  const auto paint_element = [&](int index) {
    const Element& e = scene.elements[static_cast<size_t>(index)];
    const bool hover = state.hover == index && e.enabled;
    const bool pressed = state.pressed == index && hover;
    const bool focused = state.show_focus && state.focus == index;
    const D2D1_RECT_F r = e.bounds;
    switch (e.role) {
      case Role::kButton: {
        D2D1_COLOR_F fill = palette.surface, edge = palette.line_strong, text = palette.ink;
        if (!e.enabled) {
          fill = palette.paper;
          edge = palette.line;
          text = palette.disabled;
        } else if (e.is_default) {
          fill = pressed ? palette.text2 : hover ? palette.ink_hover : palette.ink;
          edge = fill;
          text = palette.on_ink;
        } else if (pressed) {
          fill = palette.sunken;
        } else if (hover) {
          fill = palette.paper_subtle;
          edge = palette.text3;
        }
        p.Fill(r, fill, kControlRadius);
        p.Border(r, edge, kControlRadius);
        if (e.label) target->DrawTextLayout(e.label_at, e.label.Get(), p.Brush(text));
        access_line(e, text);
        if (focused) p.FocusRing(r, kControlRadius);
        break;
      }
      case Role::kDoor: {
        p.Fill(r, pressed ? palette.sunken : hover ? palette.paper_subtle : palette.surface, kControlRadius);
        p.Border(r, e.is_default || hover ? palette.ink : palette.line_strong, kControlRadius);
        if (e.label) target->DrawTextLayout(e.label_at, e.label.Get(), p.Brush(palette.ink));
        access_line(e, palette.ink);
        if (e.note) target->DrawTextLayout(e.note_at, e.note.Get(), p.Brush(palette.text2));
        p.DrawIcon(Icon::kChevron, Rect(r.right - 30, (r.top + r.bottom) / 2 - 8, 16, 16),
               e.is_default ? palette.ink : palette.text3);
        if (focused) p.FocusRing(r, kControlRadius);
        break;
      }
      case Role::kRadio: {
        const D2D1_POINT_2F c = {r.left + 13 + 9, e.label_at.y + 1 + 9};
        if (e.selected) {
          target->FillEllipse(D2D1::Ellipse(c, 9, 9), p.Brush(e.enabled ? palette.ink : palette.disabled));
          target->FillEllipse(D2D1::Ellipse(c, 3, 3), p.Brush(palette.on_ink));
        } else {
          target->FillEllipse(D2D1::Ellipse(c, 9, 9), p.Brush(palette.surface));
          target->DrawEllipse(D2D1::Ellipse(c, 8.5f, 8.5f),
                              p.Brush(!e.enabled ? palette.line_strong : hover ? palette.ink : palette.control_edge), 1);
        }
        if (e.label) target->DrawTextLayout(e.label_at, e.label.Get(), p.Brush(e.enabled ? palette.ink : palette.disabled));
        if (e.note) target->DrawTextLayout(e.note_at, e.note.Get(), p.Brush(palette.text2));
        if (focused) p.FocusRing(Grown(r, -3), kControlRadius);
        break;
      }
      case Role::kCheckBox: {
        const D2D1_RECT_F box = Rect(r.left, r.top + 1, 18, 18);
        if (e.selected) {
          p.Fill(box, palette.ink, kControlRadius);
          p.DrawIcon(Icon::kBoxCheck, Rect(box.left + 2, box.top + 2, 14, 14), palette.on_ink);
        } else {
          p.Fill(box, palette.surface, kControlRadius);
          p.Border(box, hover ? palette.ink : palette.control_edge, kControlRadius);
        }
        if (e.label) target->DrawTextLayout(e.label_at, e.label.Get(), p.Brush(palette.ink));
        access_line(e, palette.ink);
        if (focused) p.FocusRing(Grown(r, 1), kControlRadius);
        break;
      }
      case Role::kToggle:
        p.WalkToggle(r, state.walk_playing, state.hover == index);
        if (focused) p.FocusRing(r, kControlRadius);
        break;
      case Role::kLink: {
        const D2D1_COLOR_F underline =
            palette.mode == SetupMode::kHighContrast ? palette.link : hover ? palette.ink : palette.text3;
        for (const D2D1_RECT_F& line : e.underlines) p.HLine(line.left, line.right, line.top, underline);
        if (focused) {
          for (const D2D1_RECT_F& box : e.rects) p.FocusRing(Grown(box, -1), 2);
        }
        break;
      }
      default:
        break;
    }
  };

  // The panel's content, scrolled and clipped to it, coming in as a page
  // arrives.
  const D2D1_RECT_F inside = Grown(kPanel, -1);
  target->PushAxisAlignedClip(inside, D2D1_ANTIALIAS_MODE_ALIASED);
  ComPtr<ID2D1Layer> fade;
  const bool fading = state.arrive < 1 && SUCCEEDED(target->CreateLayer(&fade));
  if (fading) {
    target->PushLayer(D2D1::LayerParameters(inside, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), state.arrive),
                      fade.Get());
  }
  D2D1_MATRIX_3X2_F identity;
  target->GetTransform(&identity);
  target->SetTransform(D2D1::Matrix3x2F::Translation(0, -state.scroll + (1 - state.arrive) * 4) * identity);
  for (const Item& item : scene.items) {
    if (item.in_panel) paint_item(item);
  }
  for (size_t i = 0; i < scene.elements.size(); ++i) {
    if (scene.elements[i].in_panel) paint_element(static_cast<int>(i));
  }
  target->SetTransform(identity);
  if (fading) target->PopLayer();
  target->PopAxisAlignedClip();
  // A sign that there's more: a hairline where the content is cut off.
  if (state.scroll > 0) p.HLine(inside.left, inside.right, kContent.top - 19, palette.line);
  if (state.scroll < scene.MaxScroll()) p.HLine(inside.left, inside.right, kPanel.bottom - 2, palette.line);

  // The foot.
  for (const Item& item : scene.items) {
    if (!item.in_panel) paint_item(item);
  }
  for (size_t i = 0; i < scene.elements.size(); ++i) {
    if (!scene.elements[i].in_panel) paint_element(static_cast<int>(i));
  }
}

}  // namespace knobs::tray
