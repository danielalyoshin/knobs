// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>

// Whether a download is one of knobs's releases (docs/design.md, Updates).
namespace knobs::update {

// What CheckReleaseSignature found.
struct SignatureCheck {
  enum class Verdict {
    kRelease,  // Signed as knobs's releases are.
    // Windows couldn't finish the check, as when it can't reach the servers
    // that say whether a certificate was revoked, or the file is in use. It
    // may pass later.
    kUnknown,
    kRejected,  // Not signed, changed since, or signed by someone else.
  };

  Verdict verdict = Verdict::kRejected;
  std::string error;  // Why it isn't a release, as a sentence.

  bool ok() const { return verdict == Verdict::kRelease; }
};

// Whether the check asks online whether a certificate was revoked, as Windows
// does before running a download. kOffline skips that, and fetches nothing:
// the unit tests use it.
enum class Revocation { kOnline, kOffline };

// Checks `file`'s Authenticode signature, as Windows checks a program's
// before running it, and that it was signed the way knobs's releases are:
// through Azure Artifact Signing, by Daniel Alyoshin's identity validation.
// Its certificates last three days, so no one certificate can be pinned, but
// each carries an extended key usage unique to that validation, the same on
// every one, and chains to Microsoft's identity verification root. An error
// says which check failed.
SignatureCheck CheckReleaseSignature(const std::filesystem::path& file,
                                     Revocation revocation = Revocation::kOnline);

}  // namespace knobs::update
