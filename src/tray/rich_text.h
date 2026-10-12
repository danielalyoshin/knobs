// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The setup's text, with the little markup its pages use (first_run.h):
// <a href="url">a link</a>, <b>a name in the UI</b> and <code>a file
// name</code>. Anything else that looks like a tag is text, so a name from
// OBS can't turn into markup.
namespace knobs::tray {

struct TextSpan {
  enum class Kind { kLink, kBold, kCode };
  Kind kind = Kind::kBold;
  // In UTF-16 code units of RichText::text, as DirectWrite counts them.
  uint32_t start = 0;
  uint32_t length = 0;
  std::string url;  // kLink.

  friend bool operator==(const TextSpan&, const TextSpan&) = default;
};

struct RichText {
  std::wstring text;
  std::vector<TextSpan> spans;  // In the order they open.

  friend bool operator==(const RichText&, const RichText&) = default;
};

// Parses UTF-8 text with markup. An unclosed tag runs to the end.
RichText ParseRichText(std::string_view markup);

// The text alone, in UTF-8: for screen readers and logs.
std::string PlainText(std::string_view markup);

}  // namespace knobs::tray
