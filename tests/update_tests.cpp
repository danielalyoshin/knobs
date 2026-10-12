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
using Verdict = update::SignatureCheck::Verdict;

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
  // Offline: Windows doesn't ask whether a certificate was revoked.
  constexpr update::Revocation kOffline = update::Revocation::kOffline;
  // The tests aren't signed.
  wchar_t self[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
  const update::SignatureCheck unsigned_file = update::CheckReleaseSignature(self, kOffline);
  CHECK(unsigned_file.verdict == Verdict::kRejected && Contains(unsigned_file.error, "isn't signed"));
  // Nor is a file that isn't there.
  const std::filesystem::path missing = std::filesystem::path(self).parent_path() / L"no-such-installer.exe";
  CHECK(update::CheckReleaseSignature(missing, kOffline).verdict == Verdict::kRejected);
  // Someone else's signature, such as Microsoft's on Edge, isn't knobs's.
  wchar_t programs[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"ProgramFiles(x86)", programs, MAX_PATH)) {
    const std::filesystem::path edge =
        std::filesystem::path(programs) / L"Microsoft" / L"Edge" / L"Application" / L"msedge.exe";
    if (std::filesystem::exists(edge)) {
      const update::SignatureCheck microsoft = update::CheckReleaseSignature(edge, kOffline);
      CHECK(microsoft.verdict == Verdict::kRejected && Contains(microsoft.error, "signed by Microsoft Corporation"));
    }
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
  CHECK(updater.Due(kStart + 48h, true) == Job::kNone && !updater.NextDeadline(kStart + 48h));
}

TEST(UpdatesKeepUpdatingByItselfWithoutTheCheck) {
  // Turning the check off and on again leaves updating by itself as it was.
  Updater updater = Make({.check = true, .install = true});
  updater.Set(false, true);
  CHECK(!updater.settings().check && updater.settings().install);
  updater.Set(true, true);
  CHECK(!updater.Checked(kStart + 1min, Version{0, 3, 0}));
  CHECK(updater.Due(kStart + 2min, true) == Job::kDownload);
  // A copy that can't update itself keeps it too, as another copy may share
  // the settings, but doesn't act on it.
  CHECK(Make({.check = true, .install = true}, false).settings().install);
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
  CHECK(updater.Checked(kStart + 1min, Version{0, 3, 0}));
  CHECK(!updater.installable());
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
  std::optional<Notice> notice = updater.DownloadDone(kStart + 63min, Downloaded::kRejected);
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
  // restart, at once; one that's lost since is downloaded again.
  updater = Make({.check = true, .install = true, .checked = kStart - 1h, .latest = Version{0, 3, 0}});
  CHECK(updater.installable() == (Version{0, 3, 0}));
  updater.Found();
  CHECK(updater.Due(kStart, false) == Job::kNone && updater.Due(kStart, true) == Job::kInstall);
  CHECK(!updater.NotVerified(kStart, true));
  CHECK(updater.Due(kStart + 30min, true) == Job::kNone && updater.Due(kStart + 1h, true) == Job::kDownload);
  // Turning updating by itself off leaves its page.
  updater.Set(true, false);
  CHECK(updater.Due(kStart + 2h, true) == Job::kNone && updater.offer() == (Updater::Offer{{0, 3, 0}, false}));
  // Tried, it isn't worth keeping.
  updater.Installing();
  CHECK(!updater.installable());
}

TEST(UpdatesWaitForTheNetworkAfterSignIn) {
  // A download waits as the check does: at sign-in, the network may not be
  // up yet.
  Updater updater = Make({.check = true, .install = true, .checked = kStart - 1h, .latest = Version{0, 3, 0}});
  CHECK(updater.Due(kStart, true) == Job::kNone && updater.NextDeadline(kStart) == kStart + 1min);
  CHECK(updater.Due(kStart + 1min, true) == Job::kDownload);
  // A check that failed doesn't hold up a download, nor the other way round.
  updater = Make({.check = true, .install = true, .checked = kStart - 25h, .latest = Version{0, 3, 0}});
  CHECK(updater.Due(kStart + 1min, true) == Job::kCheck);
  CHECK(!updater.Checked(kStart + 1min, std::nullopt));
  CHECK(updater.Due(kStart + 2min, true) == Job::kDownload);
  CHECK(!updater.DownloadDone(kStart + 3min, Downloaded::kFailed));
  CHECK(updater.Due(kStart + 61min, true) == Job::kCheck);
}

TEST(UpdatesThatKeepFailingGiveUp) {
  // A download that fails three times in a row, as for a release whose
  // installer GitHub doesn't have, isn't tried again by itself: the user
  // hears, once, and can get it from its page.
  Updater updater = Make({.check = true, .install = true});
  updater.Checked(kStart + 1min, Version{0, 3, 0});
  CHECK(!updater.DownloadDone(kStart + 2min, Downloaded::kFailed));
  CHECK(!updater.DownloadDone(kStart + 62min, Downloaded::kFailed));
  const std::optional<Notice> notice = updater.DownloadDone(kStart + 122min, Downloaded::kFailed);
  CHECK(notice && notice->kind == Notice::Kind::kUpdateFailed && notice->url == update::ReleasePageUrl({0, 3, 0}));
  CHECK(updater.settings().tried == (Version{0, 3, 0}) && !updater.installable());
  CHECK(updater.Due(kStart + 5h, true) == Job::kNone && updater.offer() == (Updater::Offer{{0, 3, 0}, false}));
  // A newer one is tried afresh.
  CHECK(!updater.Checked(kStart + 25h, Version{0, 3, 1}));
  CHECK(updater.Due(kStart + 25h, true) == Job::kDownload);
  CHECK(!updater.DownloadDone(kStart + 25h, Downloaded::kFailed));
  CHECK(!updater.DownloadDone(kStart + 26h, Downloaded::kDone));
  // Done, the count starts over.
  CHECK(!updater.NotVerified(kStart + 27h, false) && !updater.NotVerified(kStart + 28h, false));

  // An installer Windows couldn't finish checking, as offline at sign-in, is
  // kept, and checked again an hour later, then only when knobs can restart.
  updater = Make({.check = true, .install = true, .checked = kStart - 1h, .latest = Version{0, 3, 0}});
  updater.Found();
  CHECK(updater.Due(kStart, true) == Job::kInstall);
  CHECK(!updater.NotVerified(kStart, false));
  CHECK(updater.ready() && updater.offer() == (Updater::Offer{{0, 3, 0}, true}));
  CHECK(updater.Due(kStart + 59min, true) == Job::kNone && updater.NextDeadline(kStart + 59min) == kStart + 1h);
  CHECK(updater.Due(kStart + 1h, false) == Job::kNone && updater.NextDeadline(kStart + 1h) == kStart - 1h + 24h);
  CHECK(updater.Due(kStart + 1h, true) == Job::kInstall);
  CHECK(!updater.NotVerified(kStart + 1h, false));
  CHECK(updater.NotVerified(kStart + 2h, false)->kind == Notice::Kind::kUpdateFailed);
  CHECK(!updater.ready() && updater.Due(kStart + 3h, true) == Job::kNone);
}

TEST(UpdatesThatWaitADaySaySo) {
  // Updating by itself waits for a moment knobs can restart. After a day
  // without one, as for someone who never opens OBS, the user hears, once,
  // and a click installs it.
  Updater updater = Make({.check = true, .install = true});
  updater.Checked(kStart + 1min, Version{0, 3, 0});
  CHECK(!updater.DownloadDone(kStart + 2min, Downloaded::kDone));
  CHECK(!updater.Waiting(kStart + 2min + 23h));
  const std::optional<Notice> notice = updater.Waiting(kStart + 2min + 24h);
  CHECK(notice && notice->kind == Notice::Kind::kUpdateReady);
  CHECK(notice->title == std::format("{} 0.3.0 is ready to install", kDisplayName));
  CHECK(Contains(notice->text, "Click to update now") && notice->url.empty() && !notice->page);
  CHECK(!updater.Waiting(kStart + 72h));
  // A newer version is said again, once it has waited a day.
  updater.Checked(kStart + 49h, Version{0, 3, 1});
  CHECK(!updater.DownloadDone(kStart + 49h, Downloaded::kDone));
  CHECK(!updater.Waiting(kStart + 72h) && updater.Waiting(kStart + 73h));

  // One downloaded before this start counts from the start.
  updater = Make({.check = true, .install = true, .checked = kStart, .latest = Version{0, 3, 0}});
  updater.Found();
  CHECK(updater.NextDeadline(kStart + 1h) == kStart + 24h);
  CHECK(!updater.Waiting(kStart + 23h) && updater.Waiting(kStart + 24h));
  // Not with updating by itself off, or once it's installing.
  updater = Make({.check = true, .install = true, .checked = kStart, .latest = Version{0, 3, 0}});
  updater.Found();
  updater.Set(true, false);
  CHECK(!updater.Waiting(kStart + 25h));
  updater.Set(true, true);
  updater.Installing();
  CHECK(!updater.Waiting(kStart + 25h));
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
  // A new version is said as it's found. While the first run is open, it
  // waits for it to close.
  CHECK(Notifier().Say(*failed, false).show == failed);
  Notifier quiet;
  CHECK(!quiet.Say(*failed, true).show);
  CHECK(!quiet.Tick(true, Notifier::Clock::now()).show);
  CHECK(quiet.Tick(false, Notifier::Clock::now()).show == failed);
  CHECK(!quiet.Tick(false, Notifier::Clock::now()).show);
  // So does what an update did.
  Notifier first_run({.updated = updated});
  CHECK(!first_run.Changed(running, true, Notifier::Clock::now()).show);
  CHECK(first_run.Changed(running, false, Notifier::Clock::now()).show == updated);
}

}  // namespace
