// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The setup's text, with the little markup its pages use (first_run.h):
// <a href="url">a link</a>, <b>a name in the UI</b> and <code>a file
// name</code>, with &lt; and &amp; for "<" and "&". Names from OBS and
// Windows go in through EscapeMarkup, so none can turn into markup.
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

// `text` as markup that shows it as it is.
std::string EscapeMarkup(std::string_view text);

// Typographic quotes for the straight ones in `text`, one for one, so spans
// stay where they are: apostrophes, and double quotes that open after a space
// or a bracket and close otherwise. The brand asks for them in everything a
// reader sees, whatever the copy's source uses. Quotes in a path or an
// address, such as C:\Users\O'Brien, are data and stay straight.
void CurlQuotes(std::wstring& text);
std::wstring CurlQuotes(std::wstring_view text);

}  // namespace knobs::tray
