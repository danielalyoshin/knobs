// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "util/result.h"

namespace knobs::runtime {

// Names of the DLLs a 64-bit PE file imports, normal imports first, then
// delay-loaded ones unless `delay_loaded` is false, as spelled in the file.
// Reads the file through a read-only mapping; nothing is loaded or executed.
Result<std::vector<std::string>> ReadDllImports(const std::filesystem::path& file, bool delay_loaded = true);

// Whether `file` is a 64-bit Windows binary, which a 64-bit process can load,
// from its headers. Fails if it can't be read.
Result<bool> IsWindows64Binary(const std::filesystem::path& file);

// The DLLs `file` imports, not delay-loaded, that loading it in this process
// wouldn't find: neither an API set nor already loaded, nor in `folders`, nor
// in a side-by-side assembly `file`'s own manifest names. Pass the folders
// this process's loader searches, such as its own and the system's.
Result<std::vector<std::string>> UnresolvedDllImports(const std::filesystem::path& file,
                                                      const std::vector<std::filesystem::path>& folders);

}  // namespace knobs::runtime
