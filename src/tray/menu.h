// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/audio_devices.h"
#include "core/backend.h"
#include "core/state.h"
#include "update/release.h"

// The tray menu as data (docs/design.md, Status line and menu): what it says
// and offers for a snapshot of the core. tray_app.cpp turns it into a native
// menu. Text is UTF-8 menu text: "&" marks the key that picks an item, so a
// name's own "&" is doubled.
namespace knobs::tray {

// Menu item IDs. Mic and cable picks are numbered from kMicFirst and
// kCableFirst, in the order of Menu::mics and Menu::cables.
enum MenuId : unsigned {
  kIdStatus = 1,
  kIdPause,
  kIdResume,
  kIdMicMenu,
  kIdMicSameAsObs,
  kIdCableMenu,
  kIdOtherDevicesMenu,
  kIdReimport,
  kIdPauseForObs,
  kIdStartWithWindows,
  kIdSetup,
  kIdOpenLogs,
  kIdAbout,
  kIdQuit,
  // Fixes, shown in bold after the status line.
  kIdFinishSetup,
  kIdChooseMic,
  kIdChooseCable,
  kIdFindObs,
  kIdRestart,
  kIdTryAgain,
  kIdOtherObs,  // A line under the status: OBS open in another account.
  // Another: Windows plays the PC's sound into a cable. Opens Sound settings.
  kIdSoundIntoCable,
  // The update check (updates.h).
  kIdGetUpdate,     // Opens a new version's page.
  kIdInstallUpdate,  // Installs the new version downloaded.
  kIdCheckForUpdates,
  kIdInstallUpdates,
  kIdMicFirst = 100,
  kIdCableFirst = 200,
  kIdCableLast = 299,
};

struct MenuItem {
  enum class Kind { kCommand, kSeparator, kSubmenu };

  Kind kind = Kind::kCommand;
  unsigned id = 0;  // Submenus have one too, to find them by.
  std::string text;
  bool enabled = true;
  bool checked = false;
  bool radio = false;  // Checked with a bullet: one of several choices.
  bool bold = false;   // The fix for what needs the user.
  std::vector<MenuItem> items;  // A submenu's.
};

struct Menu {
  std::vector<MenuItem> items;
  // What each pick sets: a mic's name for Settings::mic, and a playback
  // device for Settings::cable, which then stays whatever OBS monitors to.
  std::vector<std::string> mics;
  std::vector<audio::AudioDevice> cables;
  // The version Get… and Update to are for, as the menu showed it.
  std::optional<update::Version> update;
};

// The single line that says what knobs is doing, in the chain's short form:
// "Mic/Aux › 4 filters › CABLE In 16ch", "Paused: …", "Can't find OBS
// Studio". The menu's first line and the tooltip.
std::string StatusLine(const core::Snapshot& snapshot);

// Why `version` doesn't run: "knobs doesn't support OBS 34.0.0 yet", or "OBS
// 31.1.4 is too old for knobs". The status line, the notification and the
// setup page say it alike.
std::string ObsUnsupportedLine(const runtime::ObsVersion& version);

// Device names as the tray says them (audio_devices.h).
using audio::CableRecordingSide;
using audio::IsVbCable;
using audio::IsVirtualCable;
using audio::ShortDeviceName;

// The chain as the tray says it, with the cable's short name: in full, or in
// the short form the menu and tooltip use.
std::string ChainText(const core::ChainSummary& chain, bool short_form);

// The cable the mic goes to, by its short name, or "" until one is known.
std::string ShortCableName(const core::Snapshot& snapshot);

// "OBS has 2 mics", or "1 mic".
std::string MicCount(const core::Snapshot& snapshot);

// The short name of Windows' default playback device if it's the cable knobs
// sends to, or another input of it, else "": Windows plays the PC's sound
// into the cable, as installing one can make it do. Then the user hears
// nothing, and the cable's apps hear it with the mic.
std::string DefaultOutputCable(const core::Snapshot& snapshot);

// The update check's part of the menu (updates.h).
struct UpdateMenu {
  // knobs has an update check, which the menu turns on and off.
  bool offered = false;
  bool check = false;
  bool install = false;
  // The installer installed this copy, so it can update itself.
  bool can_install = false;
  // A version newer than this one, to get from its page, or to install now
  // once it's downloaded.
  std::optional<update::Version> version;
  bool install_now = false;
};

Menu BuildMenu(const core::Snapshot& snapshot, const core::Settings& settings, bool start_with_windows,
               const UpdateMenu& updates = {});

// The menu item with `id`, anywhere in `items`, or null.
const MenuItem* FindItem(const std::vector<MenuItem>& items, unsigned id);

// The settings a mic or cable pick asks for, or nullopt if `id` isn't one.
std::optional<core::Settings> SettingsForPick(const Menu& menu, unsigned id, const core::Settings& settings);

// The settings a click on Pause while OBS is open asks for: the other way
// from what the menu shows, kept as a choice only while it isn't what knobs
// does by default (core::Settings::pause_for_obs).
core::Settings SettingsForPauseForObs(const core::Snapshot& snapshot, const core::Settings& settings);

}  // namespace knobs::tray
