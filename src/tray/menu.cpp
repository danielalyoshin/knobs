// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/menu.h"

#include <format>
#include <utility>

#include "app_info.h"
#include "util/win_strings.h"

namespace knobs::tray {
namespace {

using core::SetupNeed;
using core::State;

// Picks past these would run into the next range of IDs.
constexpr size_t kMaxMics = kIdCableFirst - kIdMicFirst;
constexpr size_t kMaxCables = kIdCableLast - kIdCableFirst + 1;

using audio::FindById;
using audio::kDefaultDevice;
using audio::SameId;

// Menu text for a name: '&' marks a mnemonic, so a literal one is doubled.
std::string Escape(std::string_view text) {
  std::string escaped;
  for (const char c : text) {
    if (c == '&') escaped += '&';
    escaped += c;
  }
  return escaped;
}

MenuItem Command(unsigned id, std::string text) { return {.id = id, .text = std::move(text)}; }

MenuItem Separator() { return {.kind = MenuItem::Kind::kSeparator}; }

MenuItem Toggle(unsigned id, std::string text, bool checked) {
  return {.id = id, .text = std::move(text), .checked = checked};
}

MenuItem Choice(unsigned id, std::string text, bool checked) {
  return {.id = id, .text = std::move(text), .checked = checked, .radio = true};
}

MenuItem Submenu(unsigned id, std::string text) {
  return {.kind = MenuItem::Kind::kSubmenu, .id = id, .text = std::move(text)};
}

// The fix for what needs the user, if there's one in the menu.
std::optional<MenuItem> Fix(const core::Snapshot& snapshot) {
  MenuItem fix;
  switch (snapshot.state) {
    case State::kObsMissing:
      fix = Command(kIdFindObs, "&Find OBS…");
      break;
    case State::kObsUnsupported:
      // The first run's page for it, as Finish setup… opens the page the
      // state calls for.
      fix = Command(kIdFinishSetup, "&Find a supported OBS…");
      break;
    case State::kNeedsSetup:
      if (snapshot.setup == SetupNeed::kPickMic) {
        fix = Command(kIdChooseMic, "C&hoose a mic…");
      } else if (snapshot.setup == SetupNeed::kCable) {
        fix = Command(kIdChooseCable, "C&hoose a cable…");
      } else {
        fix = Command(kIdFinishSetup, "&Finish setup…");
      }
      break;
    case State::kCableMissing:
      fix = Command(kIdChooseCable, "C&hoose a cable…");
      break;
    case State::kRestartNeeded:
      fix = Command(kIdRestart, std::format("Res&tart {}", kDisplayName));
      break;
    case State::kFailed:
      fix = Command(kIdTryAgain, "&Try again");
      break;
    default:
      return std::nullopt;
  }
  fix.bold = true;
  return fix;
}

MenuItem MicMenu(const core::Snapshot& snapshot, const core::Settings& settings, Menu& menu) {
  MenuItem submenu = Submenu(kIdMicMenu, "&Mic");
  if (snapshot.state == State::kStarting) {
    submenu.enabled = false;
    return submenu;
  }
  // An empty pick takes the only mic, so it follows OBS when that mic is
  // renamed or replaced.
  if (snapshot.mics.size() == 1) {
    submenu.items.push_back(Choice(kIdMicSameAsObs, std::format("&Same as OBS ({})", Escape(snapshot.mics[0].name)),
                                   settings.mic.empty()));
    submenu.items.push_back(Separator());
  }
  for (size_t i = 0; i < snapshot.mics.size() && i < kMaxMics; ++i) {
    const bool picked = !settings.mic.empty() && snapshot.picked_mic == i;
    menu.mics.push_back(snapshot.mics[i].name);
    submenu.items.push_back(Choice(kIdMicFirst + static_cast<unsigned>(i), Escape(snapshot.mics[i].name), picked));
  }
  // A pick the collection doesn't have, or that can't be checked yet.
  if (!settings.mic.empty() && !snapshot.picked_mic) {
    MenuItem missing = Choice(0, Escape(snapshot.mics.empty() ? settings.mic
                                                             : std::format("{} (not in OBS)", settings.mic)),
                              true);
    missing.enabled = false;
    submenu.items.push_back(std::move(missing));
  }
  submenu.enabled = !submenu.items.empty();
  return submenu;
}

MenuItem CableMenu(const core::Snapshot& snapshot, const core::Settings& settings, Menu& menu) {
  MenuItem submenu = Submenu(kIdCableMenu, "&Cable");
  if (snapshot.state == State::kStarting) {
    submenu.enabled = false;
    return submenu;
  }
  // Where knobs sends the mic: the user's pick, or until there's one, what
  // OBS monitors to. A pick stays whatever OBS does, so there's no "Same as
  // OBS" to pick.
  audio::AudioDevice sends_to{settings.cable_name, settings.cable};
  if (settings.cable.empty() && !SameId(snapshot.obs_cable.id, kDefaultDevice)) sends_to = snapshot.obs_cable;
  bool picked_connected = false;
  const auto pick = [&](const audio::AudioDevice& device) {
    const bool picked = !sends_to.id.empty() && SameId(device.id, sends_to.id);
    picked_connected |= picked;
    menu.cables.push_back(device);
    return Choice(kIdCableFirst + static_cast<unsigned>(menu.cables.size() - 1), Escape(device.name), picked);
  };
  MenuItem others = Submenu(kIdOtherDevicesMenu, "&Other devices");
  size_t cables = 0;
  for (const audio::AudioDevice& device : snapshot.outputs) {
    if (menu.cables.size() >= kMaxCables) break;
    if (IsVirtualCable(device.name)) {
      submenu.items.push_back(pick(device));
      ++cables;
    }
  }
  for (const audio::AudioDevice& device : snapshot.outputs) {
    if (menu.cables.size() >= kMaxCables) break;
    if (!IsVirtualCable(device.name)) others.items.push_back(pick(device));
  }
  if (!sends_to.id.empty() && !picked_connected) {
    const std::string& name = sends_to.name.empty() ? sends_to.id : sends_to.name;
    MenuItem missing = Choice(0, Escape(std::format("{} (not connected)", name)), true);
    missing.enabled = false;
    submenu.items.push_back(std::move(missing));
  } else if (cables == 0) {
    MenuItem none = Command(0, "No virtual cable found");
    none.enabled = false;
    submenu.items.push_back(std::move(none));
  }
  if (!others.items.empty()) {
    submenu.items.push_back(Separator());
    submenu.items.push_back(std::move(others));
  }
  return submenu;
}

}  // namespace

std::string StatusLine(const core::Snapshot& snapshot) {
  const std::string chain = snapshot.chain ? ChainText(*snapshot.chain, true) : "";
  const auto with_chain = [&chain](std::string_view state) {
    return chain.empty() ? std::string(state) : std::format("{}: {}", state, chain);
  };
  switch (snapshot.state) {
    case State::kStarting:
      return snapshot.detail.empty() ? "Starting…" : snapshot.detail;
    case State::kRunning:
      return chain;
    case State::kPausedByUser:
      return with_chain("Paused");
    case State::kPausedForObs:
      return with_chain("Paused while OBS is open");
    case State::kPausedForUpdate:
      return with_chain(snapshot.nvidia_update ? "Paused for NVIDIA's SDK update" : "Paused for a plugin update");
    case State::kMicMissing:
      return with_chain("Mic missing");
    case State::kCableMissing:
      return with_chain("Cable missing");
    case State::kNeedsSetup:
      switch (snapshot.setup) {
        case SetupNeed::kObsSettings:
          return "Can't read OBS's settings";
        case SetupNeed::kNoMic:
          return "No mic in OBS yet";
        case SetupNeed::kPickMic:
          return MicCount(snapshot);
        case SetupNeed::kCable:
          return with_chain("No cable chosen");
        case SetupNeed::kNone:
          break;
      }
      return "Needs setup";
    case State::kObsMissing:
      return "Can't find OBS Studio";
    case State::kObsUnsupported:
      return "This version of OBS isn't supported";
    case State::kRestartNeeded:
      return std::format("{} needs to restart", kDisplayName);
    case State::kFailed:
      return "Stopped after an error";
  }
  return "";
}

std::string ShortDeviceName(std::string_view name) {
  while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
  if (name.empty() || name.back() != ')') return std::string(name);
  // The opening bracket that matches the last one: "Speakers (Realtek(R)
  // Audio)" is "Speakers".
  int depth = 0;
  for (size_t i = name.size(); i-- > 0;) {
    if (name[i] == ')') {
      ++depth;
    } else if (name[i] == '(' && --depth == 0) {
      std::string_view rest = name.substr(0, i);
      while (!rest.empty() && rest.back() == ' ') rest.remove_suffix(1);
      return std::string(rest.empty() ? name : rest);
    }
  }
  return std::string(name);
}

std::string ChainText(const core::ChainSummary& chain, bool short_form) {
  core::ChainSummary short_cable = chain;
  short_cable.cable = ShortDeviceName(chain.cable);
  return core::FormatChain(short_cable, short_form);
}

std::string ShortCableName(const core::Snapshot& snapshot) {
  return snapshot.chain ? ShortDeviceName(snapshot.chain->cable) : "";
}

std::string MicCount(const core::Snapshot& snapshot) {
  const size_t count = snapshot.mics.size();
  return std::format("OBS has {} mic{}", count, count == 1 ? "" : "s");
}

bool IsVirtualCable(std::string_view name) {
  const std::string lower = AsciiLower(name);
  return lower.find("vb-audio") != std::string::npos || lower.find("virtual audio cable") != std::string::npos;
}

std::string DefaultOutputCable(const core::Snapshot& snapshot) {
  if (snapshot.default_output.empty()) return "";
  const audio::AudioDevice* output = FindById(snapshot.outputs, snapshot.default_output);
  if (!output || !IsVirtualCable(output->name)) return "";
  // Only the cable knobs sends to, by either of its inputs: another, such as
  // VoiceMeeter's, may be Output on purpose, and its apps don't get the mic.
  audio::AudioDevice cable{snapshot.settings.cable_name, snapshot.settings.cable};
  if (cable.id.empty()) cable = snapshot.obs_cable;
  if (cable.id.empty() || SameId(cable.id, kDefaultDevice)) return "";
  if (const audio::AudioDevice* connected = FindById(snapshot.outputs, cable.id)) cable.name = connected->name;
  const std::string side = CableRecordingSide(output->name);
  if (!SameId(output->id, cable.id) && (side.empty() || side != CableRecordingSide(cable.name))) return "";
  return ShortDeviceName(output->name);
}

std::string CableRecordingSide(std::string_view name) {
  if (!IsVirtualCable(name)) return "";
  const std::string short_name = ShortDeviceName(name);
  for (const std::string_view input : {" In 16ch", " Input"}) {
    if (short_name.size() > input.size() && short_name.ends_with(input)) {
      return short_name.substr(0, short_name.size() - input.size()) + " Output";
    }
  }
  return "";
}

Menu BuildMenu(const core::Snapshot& snapshot, const core::Settings& settings, bool start_with_windows,
               const UpdateMenu& updates) {
  Menu menu;
  std::vector<MenuItem>& items = menu.items;
  MenuItem status = Command(kIdStatus, Escape(StatusLine(snapshot)));
  status.enabled = false;
  items.push_back(std::move(status));
  if (auto fix = Fix(snapshot)) items.push_back(std::move(*fix));
  if (!snapshot.other_obs.empty()) {
    const std::string cable = ShortCableName(snapshot);
    MenuItem other = Command(kIdOtherObs, Escape(std::format("OBS in {} may also send audio to {}",
                                                             core::DescribeOtherObs(snapshot.other_obs),
                                                             cable.empty() ? "your cable" : cable)));
    other.enabled = false;
    items.push_back(std::move(other));
  }
  // A click opens Sound settings, where Output is set back.
  if (const std::string cable = DefaultOutputCable(snapshot); !cable.empty()) {
    items.push_back(Command(kIdSoundIntoCable, Escape(std::format("Windows plays your PC's sound into {}…", cable))));
  }
  if (updates.version) {
    menu.update = updates.version;
    const std::string version = updates.version->ToString();
    items.push_back(updates.install_now
                        ? Command(kIdInstallUpdate, std::format("&Update to {} {}", kDisplayName, version))
                        : Command(kIdGetUpdate, std::format("&Get {} {}…", kDisplayName, version)));
  }
  items.push_back(Separator());
  items.push_back(snapshot.paused_by_user ? Command(kIdResume, "&Resume") : Command(kIdPause, "&Pause"));
  items.push_back(MicMenu(snapshot, settings, menu));
  items.push_back(CableMenu(snapshot, settings, menu));
  items.push_back(Command(kIdReimport, "Re-&import from OBS"));
  items.push_back(Separator());
  items.push_back(Toggle(kIdPauseForObs, "Pause while &OBS is open", settings.pause_for_obs));
  items.push_back(Toggle(kIdStartWithWindows, "Start with &Windows", start_with_windows));
  if (updates.offered) {
    items.push_back(Toggle(kIdCheckForUpdates, "Automatically check for &new versions", updates.check));
    if (updates.can_install) {
      // Kept while the check is off, which disables it.
      MenuItem install = Toggle(kIdInstallUpdates, "Automatically up&date", updates.install);
      install.enabled = updates.check;
      items.push_back(std::move(install));
    }
  }
  items.push_back(Separator());
  items.push_back(Command(kIdSetup, "&Setup…"));
  items.push_back(Command(kIdOpenLogs, "Open &log folder"));
  items.push_back(Command(kIdAbout, "&About"));
  items.push_back(Separator());
  items.push_back(Command(kIdQuit, "&Quit"));
  return menu;
}

const MenuItem* FindItem(const std::vector<MenuItem>& items, unsigned id) {
  for (const MenuItem& item : items) {
    if (item.kind != MenuItem::Kind::kSeparator && item.id == id) return &item;
    if (const MenuItem* found = FindItem(item.items, id)) return found;
  }
  return nullptr;
}

std::optional<core::Settings> SettingsForPick(const Menu& menu, unsigned id, const core::Settings& settings) {
  core::Settings picked = settings;
  if (id == kIdMicSameAsObs) {
    picked.mic.clear();
  } else if (id >= kIdMicFirst && id - kIdMicFirst < menu.mics.size()) {
    picked.mic = menu.mics[id - kIdMicFirst];
  } else if (id >= kIdCableFirst && id - kIdCableFirst < menu.cables.size()) {
    picked.cable = menu.cables[id - kIdCableFirst].id;
    picked.cable_name = menu.cables[id - kIdCableFirst].name;
  } else {
    return std::nullopt;
  }
  return picked;
}

}  // namespace knobs::tray
