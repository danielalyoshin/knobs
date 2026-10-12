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
          .text = "Click to download it.",
          .url = update::ReleasePageUrl(version)};
}

Notice NotUpdated(const update::Version& version) {
  return {.kind = Kind::kUpdateFailed,
          .title = std::format("{} couldn't update to {}", kDisplayName, version.ToString()),
          .text = "Click to download it yourself.",
          .url = update::ReleasePageUrl(version)};
}

}  // namespace

Updater::Updater(Options options, UpdateSettings settings, Clock::time_point started)
    : options_(std::move(options)), settings_(std::move(settings)), started_(started) {
  if (!options_.can_install) settings_.install = false;
  // A check "after" now was saved before the clock was set back: it counts
  // as now.
  if (settings_.checked && *settings_.checked > started_) settings_.checked = started_;
}

void Updater::Set(bool check, bool install) {
  settings_.check = check;
  settings_.install = check && install && options_.can_install;
}

bool Updater::Installs() const {
  return settings_.check && settings_.install && options_.can_install && settings_.latest &&
         *settings_.latest > options_.current && settings_.tried != settings_.latest;
}

Updater::Clock::time_point Updater::NextCheck() const {
  Clock::time_point next = started_ + options_.first;
  if (settings_.checked) next = std::max(next, *settings_.checked + options_.every);
  if (failed_) next = std::max(next, *failed_ + options_.retry);
  return next;
}

Updater::Job Updater::Due(Clock::time_point now, bool can_restart) const {
  if (!settings_.check) return Job::kNone;
  const bool ready = ready_ && ready_ == settings_.latest;
  if (Installs() && ready && can_restart) return Job::kInstall;
  if (now >= NextCheck()) return Job::kCheck;
  if (Installs() && !ready && (!failed_ || now >= *failed_ + options_.retry)) return Job::kDownload;
  return Job::kNone;
}

std::optional<Updater::Clock::time_point> Updater::NextDeadline(Clock::time_point now) const {
  if (!settings_.check) return std::nullopt;
  Clock::time_point next = NextCheck();
  if (Installs() && !(ready_ && ready_ == settings_.latest)) {
    next = std::min(next, failed_ ? *failed_ + options_.retry : now);
  }
  return next;
}

std::optional<Notice> Updater::Checked(Clock::time_point now, std::optional<update::Version> latest) {
  if (!latest) {
    failed_ = now;
    return std::nullopt;
  }
  failed_.reset();
  settings_.checked = now;
  const bool news = *latest > options_.current && (!settings_.latest || *latest > *settings_.latest);
  settings_.latest = latest;
  if (ready_ != latest) ready_.reset();
  // An update that installs by itself says so once it has.
  if (!news || Installs()) return std::nullopt;
  return Available(*latest);
}

std::optional<Notice> Updater::DownloadDone(Clock::time_point now, Downloaded outcome) {
  switch (outcome) {
    case Downloaded::kDone:
      failed_.reset();
      ready_ = settings_.latest;
      return std::nullopt;
    case Downloaded::kFailed:
      failed_ = now;
      return std::nullopt;
    case Downloaded::kRejected:
      ready_.reset();
      settings_.tried = settings_.latest;
      if (settings_.latest) return NotUpdated(*settings_.latest);
      return std::nullopt;
  }
  return std::nullopt;
}

void Updater::Lost(Clock::time_point now) {
  ready_.reset();
  failed_ = now;
}

void Updater::Installing() {
  ready_.reset();
  settings_.tried = settings_.latest;
}

std::optional<Updater::Offer> Updater::offer() const {
  if (!settings_.latest || *settings_.latest <= options_.current) return std::nullopt;
  return Offer{*settings_.latest, options_.can_install && ready_ && ready_ == settings_.latest};
}

std::optional<Notice> UpdatedNotice(const UpdateSettings& settings, const update::Version& current) {
  if (!settings.tried) return std::nullopt;
  if (*settings.tried == current) {
    return Notice{.kind = Kind::kUpdated,
                  .title = std::format("{} was updated to {}", kDisplayName, current.ToString()),
                  .text = "Click to see what's new.",
                  .url = update::ReleasePageUrl(current)};
  }
  if (*settings.tried > current) return NotUpdated(*settings.tried);
  return std::nullopt;
}

}  // namespace knobs::tray
