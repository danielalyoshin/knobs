// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tests for the update check: knobs's releases on GitHub (src/update) and when
// the tray checks, downloads and installs them (tray/updates.h). None of them
// uses the network.

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "app_info.h"
#include "core/state.h"
#include "test_harness.h"
#include "tray/notices.h"
#include "tray/updates.h"
#include "update/release.h"
#include "update/signature.h"

namespace {

using namespace knobs;
using namespace knobs::tray;
using namespace std::chrono_literals;
using update::Version;
using Job = Updater::Job;
using Downloaded = Updater::Downloaded;
using Clock = Updater::Clock;

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// Some day, at the start of an hour.
const Clock::time_point kStart = Clock::time_point(std::chrono::hours(497'712));
constexpr Version kCurrent{0, 2, 0};

Updater Make(UpdateSettings settings, bool can_install = true) {
  return Updater({.current = kCurrent, .can_install = can_install}, std::move(settings), kStart);
}

TEST(ReleaseVersionsParse) {
  CHECK(update::ParseVersion("0.3.0") == (Version{0, 3, 0}));
  CHECK(update::ParseVersion("12.10.7") == (Version{12, 10, 7}));
  for (const std::string_view bad : {"", "0.3", "0.3.0.1", "v0.3.0", "0.3.0-rc1", "01.2.3", " 0.3.0", "0.3.x",
                                     "-1.0.0", "+1.0.0", "4294967296.0.0", "0..0", "0.3.0 "}) {
    CHECK(!update::ParseVersion(bad));
  }
  CHECK((Version{0, 10, 0}) > (Version{0, 9, 9}));
  CHECK((Version{1, 0, 0}) > (Version{0, 99, 99}));
  CHECK((Version{1, 2, 3}).ToString() == "1.2.3");
}

TEST(LatestReleaseIsWhereGitHubRedirects) {
  CHECK(update::VersionFromReleaseUrl("https://github.com/danielalyoshin/knobs/releases/tag/v0.2.0") ==
        (Version{0, 2, 0}));
  // GitHub may give the names in another case.
  CHECK(update::VersionFromReleaseUrl("https://github.com/DanielAlyoshin/Knobs/releases/tag/v0.3.1") ==
        (Version{0, 3, 1}));
  for (const std::string_view other :
       {"https://github.com/danielalyoshin/knobs/releases/tag/0.2.0",
        "https://github.com/danielalyoshin/knobs/releases/tag/v0.2.0-rc1",
        "https://github.com/danielalyoshin/knobs/releases/tag/v0.2.0/",
        "https://github.com/someone/knobs/releases/tag/v0.2.0",
        "https://github.com.example.com/danielalyoshin/knobs/releases/tag/v9.9.9",
        "http://github.com/danielalyoshin/knobs/releases/tag/v0.2.0",
        "https://github.com/danielalyoshin/knobs/releases/latest",
        "/danielalyoshin/knobs/releases/tag/v0.2.0", ""}) {
    CHECK(!update::VersionFromReleaseUrl(other));
  }
  CHECK(update::ReleasePageUrl({0, 3, 0}) == "https://github.com/danielalyoshin/knobs/releases/tag/v0.3.0");
  CHECK(update::InstallerName({0, 3, 0}) == std::format("{}-0.3.0-setup.exe", kDisplayName));
  CHECK(update::InstallerUrl({0, 3, 0}) ==
        std::format("https://github.com/danielalyoshin/knobs/releases/download/v0.3.0/{}-0.3.0-setup.exe",
                    kDisplayName));
}

TEST(OnlyKnobsReleasesAreInstalled) {
  // The tests aren't signed.
  wchar_t self[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
  const Status unsigned_file = update::CheckReleaseSignature(self);
  CHECK(!unsigned_file && Contains(unsigned_file.error(), "isn't signed"));
  // Nor is a file that isn't there.
  CHECK(!update::CheckReleaseSignature(std::filesystem::path(self).parent_path() / L"no-such-installer.exe"));
  // Someone else's signature, such as Microsoft's on Edge, isn't knobs's.
  wchar_t programs[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"ProgramFiles(x86)", programs, MAX_PATH)) {
    const std::filesystem::path edge =
        std::filesystem::path(programs) / L"Microsoft" / L"Edge" / L"Application" / L"msedge.exe";
    if (std::filesystem::exists(edge)) CHECK(!update::CheckReleaseSignature(edge));
  }
}

TEST(UpdatesWaitUntilTurnedOn) {
  Updater updater = Make({});
  CHECK(updater.Due(kStart + 48h, true) == Job::kNone);
  CHECK(!updater.NextDeadline(kStart));
  // What a check found before is still offered, from its page.
  updater = Make({.latest = Version{0, 3, 0}});
  CHECK(updater.offer() == (Updater::Offer{{0, 3, 0}, false}));
  CHECK(updater.Due(kStart + 48h, true) == Job::kNone);
}

TEST(UpdatesCheckOnceADay) {
  Updater updater = Make({.check = true});
  // Not at sign-in, when the network may not be up yet.
  CHECK(updater.Due(kStart, true) == Job::kNone);
  CHECK(updater.NextDeadline(kStart) == kStart + 1min);
  CHECK(updater.Due(kStart + 1min, true) == Job::kCheck);
  CHECK(!updater.Checked(kStart + 1min, kCurrent));
  CHECK(updater.settings().checked == kStart + 1min && updater.settings().latest == kCurrent);
  CHECK(!updater.offer());
  CHECK(updater.Due(kStart + 2h, true) == Job::kNone);
  CHECK(updater.NextDeadline(kStart + 2h) == kStart + 1min + 24h);
  CHECK(updater.Due(kStart + 1min + 24h, true) == Job::kCheck);

  // No answer, as with no network: again an hour later.
  CHECK(!updater.Checked(kStart + 25h, std::nullopt));
  CHECK(updater.settings().checked == kStart + 1min);
  CHECK(updater.Due(kStart + 25h + 59min, true) == Job::kNone);
  CHECK(updater.Due(kStart + 26h, true) == Job::kCheck);

  // A check a day before this start counts.
  updater = Make({.check = true, .checked = kStart - 23h});
  CHECK(updater.Due(kStart + 59min, true) == Job::kNone);
  CHECK(updater.Due(kStart + 1h, true) == Job::kCheck);
  // One "after" now is from a clock set back since: a day from now.
  updater = Make({.check = true, .checked = kStart + 72h});
  CHECK(updater.Due(kStart + 1h, true) == Job::kNone);
  CHECK(updater.Due(kStart + 25h, true) == Job::kCheck);

  // Turned off, nothing's due.
  updater.Set(false, true);
  CHECK(updater.Due(kStart + 48h, true) == Job::kNone && !updater.settings().install);
}

TEST(UpdatesSayANewVersionOnce) {
  Updater updater = Make({.check = true});
  std::optional<Notice> notice = updater.Checked(kStart + 1min, Version{0, 3, 0});
  CHECK(notice && notice->kind == Notice::Kind::kUpdateAvailable);
  CHECK(notice->title == std::format("{} 0.3.0 is out", kDisplayName) && notice->text == "Click to download it.");
  CHECK(notice->url == "https://github.com/danielalyoshin/knobs/releases/tag/v0.3.0" && !notice->problem);
  CHECK(updater.offer() == (Updater::Offer{{0, 3, 0}, false}));
  // Updating by itself is off: nothing to download.
  CHECK(updater.Due(kStart + 2min, true) == Job::kNone);
  // Found again, it's no news.
  CHECK(!updater.Checked(kStart + 25h, Version{0, 3, 0}));
  CHECK(updater.Checked(kStart + 49h, Version{0, 3, 1}));
  // A release taken back: what's latest is older again, and nothing's offered.
  CHECK(!updater.Checked(kStart + 73h, Version{0, 2, 0}) && !updater.offer());

  // A copy that can't update itself never does.
  updater = Make({.check = true, .install = true}, false);
  CHECK(!updater.settings().install);
  CHECK(updater.Checked(kStart + 1min, Version{0, 3, 0}));
  CHECK(updater.Due(kStart + 2min, true) == Job::kNone);
  CHECK(updater.offer() == (Updater::Offer{{0, 3, 0}, false}));
}

TEST(UpdatesInstallWhenKnobsCanRestart) {
  Updater updater = Make({.check = true, .install = true});
  // It installs by itself, and says so once it has.
  CHECK(!updater.Checked(kStart + 1min, Version{0, 3, 0}));
  CHECK(updater.Due(kStart + 1min, false) == Job::kDownload);
  CHECK(!updater.DownloadDone(kStart + 2min, Downloaded::kDone));
  CHECK(updater.offer() == (Updater::Offer{{0, 3, 0}, true}));
  // Not while knobs sends the mic, or anything of its own is open.
  CHECK(updater.Due(kStart + 3min, false) == Job::kNone);
  CHECK(updater.Due(kStart + 3min, true) == Job::kInstall);
  updater.Installing();
  CHECK(updater.settings().tried == (Version{0, 3, 0}));
  // Tried, it isn't again by itself, but its page is still offered.
  CHECK(updater.Due(kStart + 4min, true) == Job::kNone);
  CHECK(updater.offer() == (Updater::Offer{{0, 3, 0}, false}));
  // A newer one is.
  CHECK(!updater.Checked(kStart + 25h, Version{0, 3, 1}));
  CHECK(updater.Due(kStart + 25h, true) == Job::kDownload);

  // A download that failed is tried again an hour later.
  updater = Make({.check = true, .install = true});
  updater.Checked(kStart + 1min, Version{0, 3, 0});
  CHECK(!updater.DownloadDone(kStart + 2min, Downloaded::kFailed));
  CHECK(updater.Due(kStart + 61min, true) == Job::kNone);
  CHECK(updater.NextDeadline(kStart + 61min) == kStart + 62min);
  CHECK(updater.Due(kStart + 62min, true) == Job::kDownload);

  // One that isn't signed as knobs's releases are isn't installed, or
  // downloaded again: the user hears, and can get it from its page.
  const std::optional<Notice> notice = updater.DownloadDone(kStart + 63min, Downloaded::kRejected);
  CHECK(notice && notice->kind == Notice::Kind::kUpdateFailed &&
        notice->title == std::format("{} couldn't update to 0.3.0", kDisplayName));
  CHECK(notice->url == update::ReleasePageUrl({0, 3, 0}));
  CHECK(updater.Due(kStart + 5h, true) == Job::kNone);
  CHECK(updater.offer() == (Updater::Offer{{0, 3, 0}, false}));

  // A newer version found while one waits to install replaces it.
  updater = Make({.check = true, .install = true});
  updater.Checked(kStart + 1min, Version{0, 3, 0});
  updater.DownloadDone(kStart + 2min, Downloaded::kDone);
  updater.Checked(kStart + 25h, Version{0, 3, 1});
  CHECK(updater.Due(kStart + 25h, true) == Job::kDownload);

  // An installer downloaded before this start installs once knobs can
  // restart; one that's lost since is downloaded again.
  updater = Make({.check = true, .install = true, .checked = kStart - 1h, .latest = Version{0, 3, 0}});
  updater.Found();
  CHECK(updater.Due(kStart, false) == Job::kNone && updater.Due(kStart, true) == Job::kInstall);
  updater.Lost(kStart);
  CHECK(updater.Due(kStart + 30min, true) == Job::kNone && updater.Due(kStart + 1h, true) == Job::kDownload);
  // Turning updating by itself off leaves its page.
  updater.Set(true, false);
  CHECK(updater.Due(kStart + 2h, true) == Job::kNone && updater.offer() == (Updater::Offer{{0, 3, 0}, false}));
}

TEST(UpdatedCopySaysHowItWent) {
  const std::optional<Notice> updated = UpdatedNotice({.tried = Version{0, 2, 0}}, kCurrent);
  CHECK(updated && updated->kind == Notice::Kind::kUpdated);
  CHECK(updated->title == std::format("{} was updated to 0.2.0", kDisplayName));
  CHECK(updated->text == "Click to see what's new." && updated->url == update::ReleasePageUrl(kCurrent));
  const std::optional<Notice> failed = UpdatedNotice({.tried = Version{0, 3, 0}}, kCurrent);
  CHECK(failed && failed->kind == Notice::Kind::kUpdateFailed);
  CHECK(!UpdatedNotice({}, kCurrent) && !UpdatedNotice({.tried = Version{0, 1, 1}}, kCurrent));

  // The notifier says it after the first import, unless a problem says more.
  core::Snapshot running;
  running.state = core::State::kRunning;
  Notifier notifier({.updated = updated});
  CHECK(!notifier.Changed(core::Snapshot{}, false, Notifier::Clock::now()).show);
  CHECK(notifier.Changed(running, false, Notifier::Clock::now()).show == updated);
  CHECK(!notifier.Changed(running, false, Notifier::Clock::now()).show);
  core::Snapshot missing;
  missing.state = core::State::kObsMissing;
  Notifier problem({.updated = updated});
  CHECK(problem.Changed(missing, false, Notifier::Clock::now()).show->kind == Notice::Kind::kObsMissing);
  // A new version is said as it's found, unless the first run is.
  CHECK(Notifier().Say(*failed, false).show == failed);
  CHECK(!Notifier().Say(*failed, true).show);
}

}  // namespace
