// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-update: runs the update check's parts the way knobs does (src/update),
// one at a time: asks GitHub for the latest release, downloads a release's
// installer, and checks an installer's signature. It installs nothing.

#include <windows.h>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "app_info.h"
#include "common/console.h"
#include "update/github.h"
#include "update/release.h"
#include "update/signature.h"
#include "util/win_strings.h"
#include "version.h"

namespace {

using namespace knobs;
using namespace knobs::tools;
namespace fs = std::filesystem;

std::string Usage() {
  return std::format(R"(Usage: knobs-update <what> [options]

Runs the update check's parts the way {0} does, one at a time. It installs
nothing, and changes none of {0}'s settings.

  --latest               Ask GitHub for the latest release, as the check does:
                         one request to {1},
                         which reads only where it redirects to.
  --download <version>   Download that release's installer, as updating by
                         itself does, and check its signature.
  --out <folder>         Where --download puts it. Default: the current folder.
  --verify <file>        Check a file's signature as {0} checks an installer's
                         before it runs it.
)",
                     kDisplayName, kLatestReleaseUrl);
}

struct Options {
  bool latest = false;
  std::optional<update::Version> download;
  fs::path out = fs::current_path();
  std::optional<fs::path> verify;
};

std::optional<Options> ParseArgs(int argc, wchar_t** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    if (arg == L"--latest") {
      options.latest = true;
      continue;
    }
    if (i + 1 >= argc) return std::nullopt;
    const std::wstring_view value = argv[++i];
    if (arg == L"--download") {
      options.download = update::ParseVersion(ToUtf8(value));
      if (!options.download) return std::nullopt;
    } else if (arg == L"--out") {
      options.out = fs::absolute(value);
    } else if (arg == L"--verify") {
      options.verify = fs::absolute(value);
    } else {
      return std::nullopt;
    }
  }
  if (!options.latest && !options.download && !options.verify) return std::nullopt;
  return options;
}

// Reports `file`'s signature check, and whether one that didn't pass might
// later, as knobs tries again then.
void Signature(const fs::path& file) {
  const update::SignatureCheck checked = update::CheckReleaseSignature(file);
  Check(checked.ok(), "signature",
        checked.ok() ? std::format("{}: signed as {}'s releases are", ToUtf8(file.filename()), kDisplayName)
                     : checked.error);
  if (checked.verdict == update::SignatureCheck::Verdict::kUnknown) {
    Report(Outcome::kNote, "signature", std::format("it may pass later, and {} tries again then", kDisplayName));
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == std::wstring_view(L"--help") || argv[i] == std::wstring_view(L"-h")) {
      Print(Usage());
      return kExitPass;
    }
  }
  const auto options = ParseArgs(argc, argv);
  if (!options) {
    Print(Usage());
    return kExitUsage;
  }
  update::Cancel cancel;
  if (options->latest) {
    const auto latest = update::FindLatestRelease(cancel);
    Check(latest.ok(), "latest", latest ? latest->ToString() : latest.error());
    if (latest) {
      const update::Version current{KNOBS_VERSION_MAJOR, KNOBS_VERSION_MINOR, KNOBS_VERSION_PATCH};
      Report(Outcome::kNote, "this",
             std::format("{} {}, {}", kDisplayName, current.ToString(),
                         *latest > current ? "older" : *latest == current ? "the latest" : "newer"));
    }
  }
  if (options->download) {
    const fs::path file = options->out / FromUtf8(update::InstallerName(*options->download));
    std::error_code ec;
    fs::create_directories(options->out, ec);
    const std::string url = update::InstallerUrl(*options->download);
    const Status got = update::Download(url, file, update::kMaxInstallerBytes, cancel);
    Check(got.ok(), "download",
          got ? std::format("{} ({:.1f} MB)", ToUtf8(file), fs::file_size(file, ec) / (1024.0 * 1024.0))
              : got.error());
    if (got) Signature(file);
  }
  if (options->verify) Signature(*options->verify);
  Print(AnyFailed() ? "FAIL\n" : "PASS\n");
  return AnyFailed() ? kExitFail : kExitPass;
}
