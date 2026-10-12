// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>

#include "util/result.h"

// Whether a download is one of knobs's releases (docs/design.md, Updates).
namespace knobs::update {

// Checks `file`'s Authenticode signature, as Windows checks a program's
// before running it, and that it was signed the way knobs's releases are:
// through Azure Artifact Signing, by Daniel Alyoshin's identity validation.
// Its certificates last three days, so no one certificate can be pinned, but
// each carries an extended key usage unique to that validation, the same on
// every one, and chains to Microsoft's identity verification root. An error
// says which check failed.
Status CheckReleaseSignature(const std::filesystem::path& file);

}  // namespace knobs::update
