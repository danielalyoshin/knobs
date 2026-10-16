// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/state.h"
#include "tray/first_run.h"
#include "tray/updates.h"
#include "util/result.h"

// knobs's settings, kept in %AppData%\knobs\settings.ini, or in the data
// folder of a portable copy (AppDirs):
//
//   [OBS]
//   Install=D:\\Games\\obs-studio
//   Settings=D:\\Games\\obs-studio\\config
//   WhileOpen=pause
//   [Audio]
//   Mic=Mic/Aux
//   Cable={0.0.0.00000000}.{…}
//   CableName=CABLE In 16ch (VB-Audio Virtual Cable)
//   [Setup]
//   Done=false
//   Door=obs
//   Page=cable
//   [Updates]
//   Check=true
//   Install=true
//   Checked=1791763200
//   Latest=0.3.1
//   Seen=1791763200
//   Reminded=0.3.1
//   Failures=1
//   Failed=1791766800
//   Tried=0.3.0
//   Installing=0.3.0
//   [Notices]
//   Warned=vst {…}\nunknown {…}
//
// WhileOpen is pause or run, once the user has chosen whether knobs pauses
// while OBS is open (core::Settings::pause_for_obs). Before 1.0.1 that was
// PauseWhileOpen, true or false, always saved: true was the default, and is
// read as no choice. Checked, Seen and Failed are times in seconds since 1970
// (UTC): when a check last got an answer, when it first found Latest, and
// when getting Latest last failed (UpdateSettings). Warned is the pre-flight
// warnings already said, and whether the pause for OBS playing the mic into
// the cable was, one a line (Notifier::Options::warned). Keys left out keep
// their defaults (core::Settings, FirstRunProgress). Values are escaped as
// libobs's INI parser unescapes them (\\, \n, \r), and read with that parser
// (import::ObsIni).
namespace knobs::tray {

struct SavedSettings {
  core::Settings core;
  // How far the first run got: Door and Page while it's unfinished.
  FirstRunProgress first_run;
  UpdateSettings updates;
  std::optional<std::vector<std::string>> warned;

  friend bool operator==(const SavedSettings&, const SavedSettings&) = default;
};

std::string FormatSettings(const SavedSettings& settings);
SavedSettings ParseSettings(std::string_view text);

// Defaults if `file` doesn't exist; an error if it can't be read.
Result<SavedSettings> LoadSettings(const std::filesystem::path& file);
Status SaveSettings(const std::filesystem::path& file, const SavedSettings& settings);

}  // namespace knobs::tray
