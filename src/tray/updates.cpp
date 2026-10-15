// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/updates.h"

#include <algorithm>
#include <format>
#include <utility>

#include "app_info.h"

namespace knobs::tray {
namespace {

using Kind = Notice::Kind;

Notice Available(const update::Version& version) {
  return {.kind = Kind::kUpdateAvailable,
          .title = std::format("{} {} is out", kDisplayName, version.ToString()),
          // The click opens the release's page.
          .text = "Click to see what’s new and download it.",
          .url = update::ReleasePageUrl(version)};
}

Notice Ready(const update::Version& version) {
  return {.kind = Kind::kUpdateReady,
          .title = std::format("{} {} is ready to install", kDisplayName, version.ToString()),
          .text = std::format("Click to update now. Your mic stops for a few seconds while {} restarts.", kDisplayName)};
}

Notice NotUpdated(const update::Version& version) {
  return {.kind = Kind::kUpdateFailed,
          .title = std::format("{} couldn’t update to {}", kDisplayName, version.ToString()),
          .text = "Click to download it yourself.",
          .url = update::ReleasePageUrl(version)};
}

}  // namespace

Updater::Updater(Options options, UpdateSettings settings, Clock::time_point started)
    : options_(std::move(options)), settings_(std::move(settings)), started_(started) {
  // Times "after" now were saved before the clock was set back: they count
  // as now.
  for (std::optional<Clock::time_point>* time : {&settings_.checked, &settings_.seen, &settings_.failed}) {
    if (*time && **time > started_) *time = started_;
  }
  // Found by a copy that didn't keep when.
  if (settings_.latest && !settings_.seen) settings_.seen = started_;
}

void Updater::Set(bool check, bool install) {
  settings_.check = check;
  settings_.install = install;
}

std::optional<update::Version> Updater::installable() const {
  if (!options_.can_install || !settings_.latest || *settings_.latest <= options_.current ||
      settings_.tried == settings_.latest) {
    return std::nullopt;
  }
  return settings_.latest;
}

bool Updater::Installs() const { return settings_.check && settings_.install && installable(); }

Updater::Clock::time_point Updater::NextCheck() const {
  Clock::time_point next = started_ + options_.first;
  if (settings_.checked) next = std::max(next, *settings_.checked + options_.every);
  if (check_failed_) next = std::max(next, *check_failed_ + options_.retry);
  return next;
}

Updater::Clock::time_point Updater::NextDownload() const {
  Clock::time_point next = started_ + options_.first;
  if (settings_.failed) next = std::max(next, *settings_.failed + options_.retry);
  return next;
}

Updater::Job Updater::Due(Clock::time_point now, bool can_restart) const {
  if (!settings_.check) return Job::kNone;
  if (Installs() && ready() && can_restart && (!settings_.failed || now >= *settings_.failed + options_.retry)) {
    return Job::kInstall;
  }
  if (now >= NextCheck()) return Job::kCheck;
  if (Installs() && !ready() && now >= NextDownload()) return Job::kDownload;
  return Job::kNone;
}

std::optional<Updater::Clock::time_point> Updater::NextDeadline(Clock::time_point now) const {
  if (!settings_.check) return std::nullopt;
  Clock::time_point next = NextCheck();
  if (Installs() && !ready()) next = std::min(next, NextDownload());
  // An install that failed waits to be tried again, then only for a restart.
  if (Installs() && ready() && settings_.failed && *settings_.failed + options_.retry > now) {
    next = std::min(next, *settings_.failed + options_.retry);
  }
  // Waiting's notification.
  const std::optional<Clock::time_point> since = ready() ? ready_since_ : settings_.seen;
  if (Installs() && since && settings_.reminded != settings_.latest && *since + options_.remind_after > now) {
    next = std::min(next, *since + options_.remind_after);
  }
  return next;
}

std::optional<Notice> Updater::Checked(Clock::time_point now, std::optional<update::Version> latest) {
  if (!latest) {
    check_failed_ = now;
    return std::nullopt;
  }
  check_failed_.reset();
  settings_.checked = now;
  const bool news = *latest > options_.current && (!settings_.latest || *latest > *settings_.latest);
  if (latest != settings_.latest) {
    ready_.reset();
    settings_.failed.reset();
    settings_.failures = 0;
    settings_.seen = now;
  }
  settings_.latest = latest;
  // An update that installs by itself says so once it has, or if it can't.
  if (!news || Installs()) return std::nullopt;
  return Available(*latest);
}

std::optional<Notice> Updater::DownloadDone(Clock::time_point now, Downloaded outcome) {
  switch (outcome) {
    case Downloaded::kDone:
      settings_.failed.reset();
      ready_ = settings_.latest;
      ready_since_ = now;
      return std::nullopt;
    case Downloaded::kFailed:
      return Failed(now, false);
    case Downloaded::kUnusable:
      return Failed(now, true);
    case Downloaded::kRejected:
      return GiveUp();
  }
  return std::nullopt;
}

std::optional<Notice> Updater::NotVerified(Clock::time_point now, bool lost) {
  if (lost) ready_.reset();
  return Failed(now, lost);
}

std::optional<Notice> Updater::Failed(Clock::time_point now, bool counts) {
  settings_.failed = now;
  if (!counts || ++settings_.failures < options_.tries) return std::nullopt;
  return GiveUp();
}

std::optional<Notice> Updater::GiveUp() {
  ready_.reset();
  settings_.failed.reset();
  settings_.failures = 0;
  settings_.tried = settings_.latest;
  if (settings_.latest) return NotUpdated(*settings_.latest);
  return std::nullopt;
}

void Updater::Found(Clock::time_point downloaded) {
  ready_ = settings_.latest;
  // Not later than now, whatever the file says.
  ready_since_ = std::min(downloaded, started_);
}

void Updater::Installing() {
  ready_.reset();
  settings_.tried = settings_.latest;
  settings_.installing = settings_.latest;
}

std::optional<Notice> Updater::Waiting(Clock::time_point now) {
  const std::optional<Clock::time_point> since = ready() ? ready_since_ : settings_.seen;
  if (!Installs() || !since || settings_.reminded == settings_.latest || now < *since + options_.remind_after) {
    return std::nullopt;
  }
  settings_.reminded = settings_.latest;
  return ready() ? Ready(*settings_.latest) : Available(*settings_.latest);
}

std::optional<Updater::Offer> Updater::offer() const {
  if (!settings_.latest || *settings_.latest <= options_.current) return std::nullopt;
  return Offer{*settings_.latest, options_.can_install && ready()};
}

std::optional<Notice> UpdatedNotice(const UpdateSettings& settings, const update::Version& current) {
  if (!settings.installing) return std::nullopt;
  if (*settings.installing == current) {
    return Notice{.kind = Kind::kUpdated,
                  .title = std::format("{} was updated to {}", kDisplayName, current.ToString()),
                  .text = "Click to see what’s new.",
                  .url = update::ReleasePageUrl(current)};
  }
  if (*settings.installing > current) return NotUpdated(*settings.installing);
  return std::nullopt;
}

Notice NotUpdatedNotice(const update::Version& version) { return NotUpdated(version); }

}  // namespace knobs::tray
