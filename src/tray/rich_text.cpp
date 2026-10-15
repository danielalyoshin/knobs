// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/rich_text.h"

#include <algorithm>
#include <optional>

#include "util/win_strings.h"

namespace knobs::tray {
namespace {

struct Tag {
  TextSpan::Kind kind = TextSpan::Kind::kBold;
  bool closing = false;
  std::string url;
  size_t length = 0;  // Of the tag, in bytes.
};

// The tag at the start of `rest`, if it's one of the markup's.
std::optional<Tag> TagAt(std::string_view rest) {
  constexpr std::string_view kLinkOpen = "<a href=\"";
  for (const auto& [text, kind, closing] : {std::tuple{std::string_view("<b>"), TextSpan::Kind::kBold, false},
                                            std::tuple{std::string_view("</b>"), TextSpan::Kind::kBold, true},
                                            std::tuple{std::string_view("<code>"), TextSpan::Kind::kCode, false},
                                            std::tuple{std::string_view("</code>"), TextSpan::Kind::kCode, true},
                                            std::tuple{std::string_view("</a>"), TextSpan::Kind::kLink, true}}) {
    if (rest.starts_with(text)) return Tag{kind, closing, {}, text.size()};
  }
  if (rest.starts_with(kLinkOpen)) {
    const size_t end = rest.find("\">", kLinkOpen.size());
    if (end == std::string_view::npos) return std::nullopt;
    return Tag{TextSpan::Kind::kLink, false, std::string(rest.substr(kLinkOpen.size(), end - kLinkOpen.size())),
               end + 2};
  }
  return std::nullopt;
}

}  // namespace

RichText ParseRichText(std::string_view markup) {
  RichText rich;
  // Spans still open, by index in rich.spans.
  std::vector<size_t> open;
  size_t done = 0;  // Bytes of `markup` turned into text or tags.
  // Text between tags, with its "<" and "&" escapes undone.
  const auto flush = [&](size_t to) {
    std::string_view part = markup.substr(done, to > done ? to - done : 0);
    std::string text;
    while (!part.empty()) {
      if (part.starts_with("&lt;")) {
        text += '<';
        part.remove_prefix(4);
      } else if (part.starts_with("&amp;")) {
        text += '&';
        part.remove_prefix(5);
      } else {
        text += part.front();
        part.remove_prefix(1);
      }
    }
    rich.text += FromUtf8(text);
    done = std::max(done, to);
  };
  for (size_t at = markup.find('<'); at != std::string_view::npos; at = markup.find('<', at + 1)) {
    const std::optional<Tag> tag = TagAt(markup.substr(at));
    if (!tag) continue;
    flush(at);
    done = at + tag->length;
    const auto position = static_cast<uint32_t>(rich.text.size());
    if (!tag->closing) {
      open.push_back(rich.spans.size());
      rich.spans.push_back({tag->kind, position, 0, tag->url});
    } else {
      // Closes the innermost open span of its kind.
      for (auto it = open.rbegin(); it != open.rend(); ++it) {
        TextSpan& span = rich.spans[*it];
        if (span.kind != tag->kind) continue;
        span.length = position - span.start;
        open.erase(std::next(it).base());
        break;
      }
    }
    at = done - 1;
  }
  flush(markup.size());
  for (const size_t index : open) {
    rich.spans[index].length = static_cast<uint32_t>(rich.text.size()) - rich.spans[index].start;
  }
  return rich;
}

std::string PlainText(std::string_view markup) { return ToUtf8(ParseRichText(markup).text); }

std::string EscapeMarkup(std::string_view text) {
  std::string escaped;
  for (const char c : text) {
    if (c == '<') {
      escaped += "&lt;";
    } else if (c == '&') {
      escaped += "&amp;";
    } else {
      escaped += c;
    }
  }
  return escaped;
}

}  // namespace knobs::tray
