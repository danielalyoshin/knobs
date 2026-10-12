// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string_view>

#include "util/result.h"

namespace knobs {

// Starts `exe`, with `arguments` after its quoted path, in `folder`, and
// doesn't wait for it. On a failure, the Error is Windows' reason, for the
// caller to say what it was starting.
Status StartProcess(const std::filesystem::path& exe, std::wstring_view arguments,
                    const std::filesystem::path& folder);

}  // namespace knobs
