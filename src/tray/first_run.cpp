// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/first_run.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <utility>

#include "app_info.h"
#include "runtime/obs_version.h"
#include "tray/rich_text.h"
#include "tray/menu.h"
#include "util/win_strings.h"

namespace knobs::tray {
namespace {

using core::SetupNeed;
using core::State;
using Page = FirstRunPage;

constexpr std::string_view kObsDownloadUrl = "https://obsproject.com/download";
constexpr std::string_view kObsReleasesUrl = "https://github.com/obsproject/obs-studio/releases";
constexpr std::string_view kVbCableUrl = "https://vb-audio.com/Cable/";

using audio::FindById;
using audio::kDefaultDevice;
using audio::SameId;

constexpr std::array<std::pair<Page, std::string_view>, 12> kPageNames = {{
    {Page::kWelcome, "welcome"},
    {Page::kSteps, "steps"},
    {Page::kLooking, "looking"},
    {Page::kFindObs, "find-obs"},
    {Page::kObsUnsupported, "obs-unsupported"},
    {Page::kObsSettings, "obs-settings"},
    {Page::kProblem, "problem"},
    {Page::kMic, "mic"},
    {Page::kFilters, "filters"},
    {Page::kCable, "cable"},
    {Page::kWarnings, "warnings"},
    {Page::kDone, "done"},
}};

std::string Count(size_t count, std::string_view thing) {
  return std::format("{} {}{}", count, thing, count == 1 ? "" : "s");
}

// "a", "a and b", "a, b and c".
std::string ListNames(const std::vector<std::string>& names) {
  std::string text;
  for (size_t i = 0; i < names.size(); ++i) {
    text += std::format("{}{}", i == 0 ? "" : i + 1 == names.size() ? " and " : ", ", names[i]);
  }
  return text;
}

// The device knobs sends the mic to: its own pick, else OBS's monitoring
// device. The name is the connected device's, when it's connected.
audio::AudioDevice SendsTo(const core::Snapshot& snapshot) {
  audio::AudioDevice device = snapshot.settings.cable.empty()
                                  ? snapshot.obs_cable
                                  : audio::AudioDevice{snapshot.settings.cable_name, snapshot.settings.cable};
  if (const audio::AudioDevice* connected = FindById(snapshot.outputs, device.id)) device.name = connected->name;
  if (device.name.empty()) device.name = device.id;
  return device;
}

std::vector<audio::AudioDevice> VirtualCables(const core::Snapshot& snapshot) {
  std::vector<audio::AudioDevice> cables;
  for (const audio::AudioDevice& device : snapshot.outputs) {
    if (IsVirtualCable(device.name)) cables.push_back(device);
  }
  return cables;
}

// The core can't run without a cable being chosen or connected.
bool NeedsCable(const core::Snapshot& snapshot) {
  return (snapshot.state == State::kNeedsSetup && snapshot.setup == SetupNeed::kCable) ||
         snapshot.state == State::kCableMissing;
}

// OBS monitors to a device that isn't a cable, and knobs follows it.
bool CableUnconfirmed(const core::Snapshot& snapshot) {
  if (!snapshot.settings.cable.empty() || snapshot.obs_cable.id.empty() ||
      SameId(snapshot.obs_cable.id, kDefaultDevice)) {
    return false;
  }
  return !IsVirtualCable(SendsTo(snapshot).name);
}

// Warnings, and notes that change what to expect.
std::vector<import::ImportNote> HeadsUp(const core::Snapshot& snapshot) {
  std::vector<import::ImportNote> notes;
  for (const import::ImportNote& note : snapshot.notes) {
    if (note.warning || note.changes_expectations) notes.push_back(note);
  }
  return notes;
}

// A note as markup, with the file it names, such as a VST plugin's
// "ReaComp-standalone.dll", as <code>, as the pages set the files they name.
std::string NoteMarkup(const import::ImportNote& note) {
  std::string text = EscapeMarkup(note.text);
  const std::string file = EscapeMarkup(note.file);
  if (const size_t at = file.empty() ? std::string::npos : text.find(file); at != std::string::npos) {
    text.replace(at, file.size(), "<code>" + file + "</code>");
  }
  return text;
}

// The pages the state calls for, which block the rest.
std::optional<Page> Blocking(const core::Snapshot& snapshot) {
  switch (snapshot.state) {
    case State::kStarting:
      return Page::kLooking;
    case State::kObsMissing:
      return Page::kFindObs;
    case State::kObsUnsupported:
      return Page::kObsUnsupported;
    case State::kRestartNeeded:
    case State::kFailed:
      return Page::kProblem;
    case State::kNeedsSetup:
      switch (snapshot.setup) {
        case SetupNeed::kObsSettings:
          return Page::kObsSettings;
        case SetupNeed::kNoMic:
          return Page::kFilters;
        case SetupNeed::kPickMic:
          return Page::kMic;
        default:
          return std::nullopt;
      }
    default:
      return std::nullopt;
  }
}

// Whether the first door's `page` has anything to show for `snapshot`.
bool Applies(Page page, const core::Snapshot& snapshot) {
  switch (page) {
    case Page::kMic:
      return !snapshot.mics.empty();
    case Page::kFilters:
      return snapshot.chain && snapshot.chain->filters.empty();
    case Page::kCable:
    case Page::kDone:
      return true;
    case Page::kWarnings:
      return !HeadsUp(snapshot).empty();
    default:
      return Blocking(snapshot) == page;
  }
}

// The mic the steps and pages talk about: the picked one, or the only one.
const import::MicCandidate* TheMic(const core::Snapshot& snapshot) {
  if (snapshot.picked_mic && *snapshot.picked_mic < snapshot.mics.size()) return &snapshot.mics[*snapshot.picked_mic];
  if (snapshot.mics.size() == 1) return &snapshot.mics.front();
  return nullptr;
}

std::string MicName(const core::Snapshot& snapshot) {
  if (snapshot.chain) return snapshot.chain->mic;
  const import::MicCandidate* mic = TheMic(snapshot);
  return mic ? mic->name : "";
}

// The recording device a mic records from, if it's connected: for the
// default device, the default communications device.
std::string InputName(const std::string& device_id, const core::Snapshot& snapshot) {
  const audio::AudioDevice* device =
      FindById(snapshot.inputs, SameId(device_id, kDefaultDevice) ? snapshot.default_input : device_id);
  return device ? device->name : "";
}

// --- The band ------------------------------------------------------------------------

StageView MicStage(const core::Snapshot& snapshot) {
  if (snapshot.state == State::kNeedsSetup && snapshot.setup == SetupNeed::kNoMic) {
    return {StageState::kBlocked, "No mic in OBS"};
  }
  const import::MicCandidate* mic = TheMic(snapshot);
  if (!mic) {
    if (snapshot.mics.size() > 1) return {StageState::kKnown, std::format("{} mics in OBS", snapshot.mics.size())};
    return {StageState::kUnknown, "Not read yet"};
  }
  const std::string device = InputName(mic->device_id, snapshot);
  if (snapshot.state == State::kMicMissing || device.empty()) {
    return {StageState::kBlocked, EscapeMarkup(mic->name) + "\nNot connected"};
  }
  return {StageState::kSet, EscapeMarkup(mic->name) + "\n" + EscapeMarkup(device)};
}

StageView FiltersStage(const core::Snapshot& snapshot) {
  switch (snapshot.state) {
    case State::kStarting:
      return {StageState::kUnknown, "Looking for OBS"};
    case State::kObsMissing:
      return {StageState::kBlocked, "OBS not found"};
    case State::kObsUnsupported:
      return {StageState::kBlocked, std::format("Can't run OBS {}", snapshot.obs.version.ToString())};
    case State::kFailed:
      return {StageState::kBlocked, "Stopped after an error"};
    case State::kNeedsSetup:
      if (snapshot.setup == SetupNeed::kObsSettings) return {StageState::kBlocked, "Can't read OBS's settings"};
      break;
    default:
      break;
  }
  std::optional<size_t> count;
  if (snapshot.chain) {
    count = snapshot.chain->filters.size();
  } else if (const import::MicCandidate* mic = TheMic(snapshot)) {
    count = mic->filters.size();
  }
  if (!count) return {StageState::kUnknown, "Not read yet"};
  if (*count == 0) return {StageState::kUnknown, "No filters yet"};
  const std::vector<import::ImportNote> notes = HeadsUp(snapshot);
  const bool warned = std::any_of(notes.begin(), notes.end(), [](const import::ImportNote& n) { return n.warning; });
  return {warned ? StageState::kWarning : StageState::kSet, Count(*count, "filter")};
}

StageView CableStage(const core::Snapshot& snapshot) {
  const audio::AudioDevice sends_to = SendsTo(snapshot);
  const std::string name = EscapeMarkup(ShortDeviceName(sends_to.name));
  if (snapshot.state == State::kCableMissing) return {StageState::kBlocked, name + "\nNot connected"};
  if (NeedsCable(snapshot)) {
    return {StageState::kUnknown, VirtualCables(snapshot).empty() ? "No cable yet" : "Not chosen yet"};
  }
  if (sends_to.id.empty()) return {StageState::kUnknown, "Not read yet"};
  if (SameId(sends_to.id, kDefaultDevice)) return {StageState::kUnknown, "Not chosen yet"};
  if (!FindById(snapshot.outputs, sends_to.id)) return {StageState::kBlocked, name + "\nNot connected"};
  if (!IsVirtualCable(sends_to.name)) return {StageState::kKnown, name + "\nNot a virtual cable"};
  return {StageState::kSet, name};
}

StageView AppsStage(const core::Snapshot& snapshot, const StageView& cable) {
  if (cable.state != StageState::kSet) return {StageState::kUnknown, "Use the cable as their mic"};
  const std::string side = CableRecordingSide(SendsTo(snapshot).name);
  if (side.empty()) return {StageState::kKnown, "Use the cable as their mic"};
  return {StageState::kKnown, std::format("Choose <b>{}</b>", EscapeMarkup(side))};
}

// The stage a page is about.
std::optional<Stage> StageOf(Page page, const core::Snapshot& snapshot) {
  switch (page) {
    case Page::kWelcome:
    case Page::kSteps:
    case Page::kFindObs:
    case Page::kObsUnsupported:
    case Page::kObsSettings:
    case Page::kWarnings:
      return Stage::kFilters;
    case Page::kProblem:
      if (snapshot.state == State::kFailed) return Stage::kFilters;
      return std::nullopt;
    case Page::kLooking:
      return std::nullopt;
    case Page::kMic:
      return Stage::kMic;
    case Page::kFilters:
      return snapshot.chain ? Stage::kFilters : Stage::kMic;
    case Page::kCable:
      return Stage::kCable;
    case Page::kDone:
      return Stage::kApps;
  }
  return std::nullopt;
}

bool ObsInstalled(const core::Snapshot& snapshot) {
  return !snapshot.obs.root.empty() && snapshot.state != State::kObsMissing;
}

// --- Copy ---------------------------------------------------------------------------

std::string ObsUserNote(const core::Snapshot& snapshot) {
  switch (snapshot.state) {
    case State::kStarting:
      return "Looking for OBS…";
    case State::kObsMissing:
      return std::format("Can't find OBS Studio. If it's installed somewhere else, you can show {} where.",
                         kDisplayName);
    case State::kObsUnsupported:
      if (!runtime::IsNewerThanSupportedObs(snapshot.obs.version)) {
        return std::format("OBS {} is installed, which is too old for {}.", snapshot.obs.version.ToString(),
                           kDisplayName);
      }
      return std::format("OBS {} is installed, and {} doesn't support it yet.", snapshot.obs.version.ToString(),
                         kDisplayName);
    case State::kNeedsSetup:
      if (snapshot.setup == SetupNeed::kObsSettings) return "Can't read OBS's settings.";
      if (snapshot.setup == SetupNeed::kNoMic) return "OBS has no mic yet.";
      break;
    default:
      break;
  }
  if (snapshot.mics.size() > 1 && !snapshot.chain) {
    std::vector<std::string> names;
    for (const import::MicCandidate& mic : snapshot.mics) names.push_back(mic.name);
    return std::format("Found {}: {}.", Count(names.size(), "mic"), ListNames(names));
  }
  if (snapshot.chain) {
    if (snapshot.chain->filters.empty()) return std::format("{} has no filters yet.", snapshot.chain->mic);
    return std::format("Found {} with {}.", snapshot.chain->mic, Count(snapshot.chain->filters.size(), "filter"));
  }
  return "Import your mic and its filters from OBS.";
}

std::string NewToObsNote(const core::Snapshot& snapshot) {
  if (snapshot.state == State::kObsMissing) {
    return "OBS Studio isn't installed. Get it and set up your mic, step by step.";
  }
  return "Set up your mic's filters in OBS, step by step.";
}

bool ObsUserFits(const core::Snapshot& snapshot) {
  switch (snapshot.state) {
    case State::kObsMissing:
      return false;
    case State::kNeedsSetup:
      if (snapshot.setup == SetupNeed::kObsSettings || snapshot.setup == SetupNeed::kNoMic) return false;
      break;
    case State::kStarting:
    case State::kObsUnsupported:
      return true;
    default:
      break;
  }
  return AnyFilters(snapshot) || snapshot.state == State::kFailed || snapshot.state == State::kRestartNeeded;
}

// A line for pages whose answer OBS can change: OBS saves what it's told as
// it closes.
std::string ObsOpenLine(const core::Snapshot& snapshot) {
  if (!snapshot.obs_running) return "";
  return std::format("OBS is open. {} reads its settings again when OBS closes.", kDisplayName);
}

PageButton Button(int id, std::string text, bool enabled = true) { return {id, std::move(text), enabled}; }

// A name in a button's text: its "&"s are text, not an access key.
std::string EscapeAccessKey(std::string_view name) {
  std::string escaped;
  for (const char c : name) escaped += c == '&' ? "&&" : std::string(1, c);
  return escaped;
}

PageView Welcome(const core::Snapshot& snapshot) {
  PageView view;
  view.instruction = "Have you set up your mic in OBS?";
  view.content = std::format("{} runs your mic through the filters you set up in OBS Studio and sends it to a "
                             "virtual cable, with OBS closed. Other apps use the cable as their mic.",
                             kDisplayName);
  view.footer = std::format("{} isn't affiliated with or endorsed by the OBS Project.", kDisplayName);
  view.command_links = true;
  view.buttons = {Button(kButtonObsUser, "I &set up my mic in OBS\n" + ObsUserNote(snapshot)),
                  Button(kButtonNewToObs, "I'm &new to OBS\n" + NewToObsNote(snapshot))};
  view.default_button = ObsUserFits(snapshot) ? kButtonObsUser : kButtonNewToObs;
  return view;
}

PageView Steps(const core::Snapshot& snapshot) {
  PageView view;
  view.instruction = "Set up your mic in OBS";
  const bool installed = ObsInstalled(snapshot);
  std::string open;
  if (!installed) {
    open = std::format("Install {} from <a href=\"{}\">obsproject.com</a>, and open it.",
                       runtime::DescribeSupportedObsVersions(), kObsDownloadUrl);
  } else if (snapshot.state == State::kObsUnsupported) {
    const std::string why = runtime::IsNewerThanSupportedObs(snapshot.obs.version)
                                ? std::format("{} doesn't support OBS {} yet.", kDisplayName,
                                              snapshot.obs.version.ToString())
                                : std::format("OBS {} is too old for {}.", snapshot.obs.version.ToString(),
                                              kDisplayName);
    open = std::format("Install {} from <a href=\"{}\">OBS's releases on GitHub</a>, and open it. {}",
                       runtime::DescribeSupportedObsVersions(), kObsReleasesUrl, why);
  } else {
    open = "Open OBS.";
  }
  const std::string mic = MicName(snapshot);
  std::string filters;
  if (snapshot.mics.empty()) {
    // The names in OBS's en-US locale, the same in 32.2.2 and 33.0:
    // Basic.Settings.Audio, Basic.Settings.Audio.AuxDevice, Mixer,
    // Basic.AuxDevice1 and Filters.
    filters = "In <b>Settings › Audio</b>, set <b>Mic/Auxiliary Audio</b> to your mic. Then, in the Audio Mixer, "
              "click <b>Mic/Aux</b> and choose <b>Filters</b>.";
  } else {
    // In OBS's mixer, a source's name opens its menu (VolumeControl.cpp).
    filters = std::format("In the Audio Mixer, click {} and choose <b>Filters</b>.",
                          mic.empty() ? "your mic" : std::format("<b>{}</b>", EscapeMarkup(mic)));
  }
  // An audio-only source's list is Basic.Filters.AudioFilters. knobs
  // suggests no filters and no settings.
  view.content =
      std::format("OBS is where you tune your mic. {} then runs the same filters with OBS closed.", kDisplayName);
  view.steps = {open, filters, "Under Audio Filters, click <b>+</b> and add filters as needed.",
                "Click each filter and adjust it by ear, starting from OBS's defaults.",
                std::format("Close OBS. {} then reads your filters and carries on here.", kDisplayName)};
  view.picture = Picture{"obs-filters", 0,
                         "A walkthrough in OBS: Mic/Aux clicked in the Audio Mixer, Filters chosen from its menu, then "
                         "+ clicked under Audio Filters"};
  view.footer = std::format("<a href=\"{}\">The setup guide</a> has more on each step.", kSetupGuideUrl);
  if (snapshot.obs_running) {
    view.waiting = true;
    view.status = "OBS is open. Close it when you're done.";
  }
  view.buttons.push_back(Button(kButtonBack, "&Back"));
  if (installed) view.buttons.push_back(Button(kButtonOpenObs, "&Open OBS", !snapshot.obs_running));
  view.buttons.push_back(Button(kButtonNext, "&Next", !snapshot.obs_running && AnyFilters(snapshot)));
  view.default_button = installed && !snapshot.obs_running ? kButtonOpenObs : kButtonNext;
  return view;
}

PageView Looking() {
  PageView view;
  view.instruction = "Looking for OBS…";
  view.content = "Copying the parts of OBS needed to run, this may take a few seconds the first time.";
  view.waiting = true;
  view.buttons = {Button(kButtonBack, "&Back")};
  return view;
}

PageView FindObs(const core::Snapshot& snapshot) {
  PageView view;
  view.instruction = "Can't find OBS Studio";
  const std::string_view gap = snapshot.detail.empty() ? "" : "\n\n";
  view.buttons = {Button(kButtonBack, "&Back"), Button(kButtonTryAgain, "&Try again")};
  if (snapshot.settings.obs_dir) {
    // A folder picked before: it stays picked until the user says otherwise.
    view.content = std::format("{}{}OBS isn't in the folder chosen for it. Choose the folder it's in now, or let {} "
                               "look for it where OBS's installer puts it.",
                               EscapeMarkup(snapshot.detail), gap, kDisplayName);
    view.buttons.push_back(Button(kButtonLookForObs, "&Look for OBS"));
  } else {
    view.content = std::format("{}{}If OBS is installed somewhere else, such as by Steam or in a folder of its own, "
                               "choose that folder. Otherwise, <a href=\"{}\">get OBS Studio</a>, install it, and "
                               "try again.",
                               EscapeMarkup(snapshot.detail), gap, kObsDownloadUrl);
  }
  view.buttons.push_back(Button(kButtonChooseObs, "&Choose folder…"));
  view.default_button = kButtonChooseObs;
  return view;
}

PageView ObsUnsupported(const core::Snapshot& snapshot) {
  PageView view;
  view.instruction = "This version of OBS isn't supported";
  const std::string_view gap = snapshot.detail.empty() ? "" : "\n\n";
  // Only an OBS newer than knobs supports can be waited for. Without an
  // update check, the link is how people find the update.
  const std::string wait = runtime::IsNewerThanSupportedObs(snapshot.obs.version)
                               ? std::format(" Or wait for a {0} update that supports OBS {1}, from <a href=\"{2}\">{0}'s "
                                             "latest release</a>.",
                                             kDisplayName, snapshot.obs.version.ToString(), kLatestReleaseUrl)
                               : "";
  view.buttons = {Button(kButtonBack, "&Back"), Button(kButtonTryAgain, "&Try again")};
  if (snapshot.settings.obs_dir) {
    // A folder picked before: it stays picked until the user says otherwise.
    view.content = std::format("{}{}This is the OBS in the folder chosen for it, {}. Choose the folder of a "
                               "supported OBS, or let {} look for OBS where OBS's installer puts it.{}",
                               EscapeMarkup(snapshot.detail), gap, EscapeMarkup(ToUtf8(snapshot.obs.root)),
                               kDisplayName, wait);
    view.buttons.push_back(Button(kButtonLookForObs, "&Look for OBS"));
    view.default_button = kButtonChooseObs;
  } else {
    view.content = std::format("{}{}Install a supported version from <a href=\"{}\">OBS's releases on GitHub</a>, "
                               "and try again. If a supported OBS is installed somewhere else, choose that folder.{}",
                               EscapeMarkup(snapshot.detail), gap, kObsReleasesUrl, wait);
    view.default_button = kButtonTryAgain;
  }
  view.buttons.push_back(Button(kButtonChooseObs, "&Choose folder…"));
  return view;
}

PageView ObsSettings(const core::Snapshot& snapshot) {
  PageView view;
  view.instruction = "Can't read OBS's settings";
  const std::string_view gap = snapshot.detail.empty() ? "" : "\n\n";
  // Try again reads the same folder again: on a drive plugged back in, or
  // with settings put there some other way.
  view.buttons = {Button(kButtonBack, "&Back"), Button(kButtonTryAgain, "&Try again")};
  // A folder picked before, other than the one OBS keeps its settings in,
  // stays picked until the user says otherwise. Opening OBS wouldn't fill
  // it, so the page doesn't suggest that. OBS's own folder picked is as good
  // as no pick (import::ObsConfigRootFor).
  if (snapshot.settings.obs_config && !snapshot.obs_writes_config) {
    view.content = std::format("{}{}OBS's settings aren't in the folder chosen for them. Choose the folder that holds "
                               "obs-studio now, or use the one OBS keeps them in.",
                               EscapeMarkup(snapshot.detail), gap);
    view.buttons.push_back(Button(kButtonOwnObsSettings, "&Use OBS's own folder"));
    view.buttons.push_back(Button(kButtonChooseObsSettings, "&Choose folder…"));
    view.default_button = kButtonChooseObsSettings;
    return view;
  }
  view.content = std::format("{}{}If your OBS keeps its settings in a folder of its own, as a portable OBS does, "
                             "choose that folder: the one that holds obs-studio.",
                             EscapeMarkup(snapshot.detail), gap);
  view.footer = ObsOpenLine(snapshot);
  view.buttons.push_back(Button(kButtonChooseObsSettings, "&Choose folder…"));
  if (ObsInstalled(snapshot)) view.buttons.push_back(Button(kButtonOpenObs, "&Open OBS", !snapshot.obs_running));
  view.default_button = ObsInstalled(snapshot) && !snapshot.obs_running ? kButtonOpenObs : kButtonChooseObsSettings;
  return view;
}

PageView Problem(const core::Snapshot& snapshot) {
  PageView view;
  view.content = EscapeMarkup(snapshot.detail);
  view.buttons = {Button(kButtonBack, "&Back")};
  if (snapshot.state == State::kRestartNeeded) {
      view.instruction = std::format("{} needs to restart", kDisplayName);
    view.buttons.push_back(Button(kButtonRestart, std::format("&Restart {}", kDisplayName)));
    view.default_button = kButtonRestart;
  } else {
    view.instruction = "Stopped after an error";
    view.buttons.push_back(Button(kButtonTryAgain, "&Try again"));
    view.default_button = kButtonTryAgain;
  }
  return view;
}

// "Mic/Aux: Microphone (Audient iD4), 4 filters".
std::string MicChoice(const import::MicCandidate& mic, const core::Snapshot& snapshot) {
  std::string device;
  if (SameId(mic.device_id, kDefaultDevice)) {
    const audio::AudioDevice* current = FindById(snapshot.inputs, snapshot.default_input);
    device = std::format("Default communications device ({})", current ? current->name : "none connected");
  } else if (const audio::AudioDevice* connected = FindById(snapshot.inputs, mic.device_id)) {
    device = connected->name;
  } else {
    device = "device not connected";
  }
  return std::format("{}\n{}, {}", mic.name, device,
                     mic.filters.empty() ? "no filters" : Count(mic.filters.size(), "filter"));
}

PageView Mic(const core::Snapshot& snapshot) {
  PageView view;
  view.instruction = std::format("Which mic should {} run?", kDisplayName);
  view.content = snapshot.mics.size() == 1
                     ? std::format("OBS has one mic. {} runs it, with its filters.", kDisplayName)
                     : std::format("OBS has {} mics. {} runs one of them, with its filters.", snapshot.mics.size(),
                                   kDisplayName);
  if (snapshot.setup == SetupNeed::kPickMic && !snapshot.settings.mic.empty()) {
    view.content += std::format(" The one chosen before, \"{}\", is no longer in OBS.",
                                EscapeMarkup(snapshot.settings.mic));
  }
  // The pick, else the first mic with filters, preferring one OBS monitors.
  std::optional<size_t> preferred = snapshot.picked_mic;
  for (const bool monitored : {true, false}) {
    for (size_t i = 0; i < snapshot.mics.size() && !preferred; ++i) {
      if (!snapshot.mics[i].filters.empty() && (snapshot.mics[i].monitored || !monitored)) preferred = i;
    }
  }
  for (size_t i = 0; i < snapshot.mics.size(); ++i) {
    view.choices.push_back(Button(kChoiceFirst + static_cast<int>(i), MicChoice(snapshot.mics[i], snapshot)));
  }
  view.default_choice = kChoiceFirst + static_cast<int>(preferred.value_or(0));
  view.footer = ObsOpenLine(snapshot);
  view.buttons = {Button(kButtonBack, "&Back"), Button(kButtonNext, "&Next")};
  view.default_button = kButtonNext;
  return view;
}

PageView Filters(const core::Snapshot& snapshot) {
  PageView view;
  view.footer = ObsOpenLine(snapshot);
  view.buttons = {Button(kButtonBack, "&Back")};
  if (!snapshot.chain) {
    view.instruction = "OBS has no mic yet";
    view.content = std::format("{0} runs a mic from OBS. To set one up, open OBS's Settings › Audio, set "
                               "Mic/Auxiliary Audio to your mic, and close OBS. {0} carries on when OBS closes.",
                               kDisplayName);
    if (ObsInstalled(snapshot)) view.buttons.push_back(Button(kButtonOpenObs, "&Open OBS", !snapshot.obs_running));
  } else {
    view.instruction = std::format("{} has no filters yet", snapshot.chain->mic);
    view.content = std::format("{} would send it to the cable unprocessed. To set up its filters, follow the steps "
                               "for setting up a mic in OBS.",
                               kDisplayName);
    view.buttons.push_back(Button(kButtonSkipFilters, "Continue &without filters"));
  }
  view.buttons.push_back(Button(kButtonShowSteps, "&Show the steps"));
  view.default_button = kButtonShowSteps;
  return view;
}

PageView Cable(const core::Snapshot& snapshot) {
  PageView view;
  const audio::AudioDevice sends_to = SendsTo(snapshot);
  const std::vector<audio::AudioDevice> choices = CableChoices(snapshot);
  if (VirtualCables(snapshot).empty()) {
    view.instruction = "Install a virtual cable";
    view.content = "There's no virtual cable on this PC yet. To install VB-Cable:";
    // VB-Audio's driver pack (VBCABLE_Driver_Pack45.zip) and its setup.
    view.steps = {
        std::format("Download its driver pack from <a href=\"{}\"><code>vb-audio.com/Cable</code></a>.", kVbCableUrl),
        "Right-click the zip and choose <b>Extract All</b>.",
        "In the extracted folder, right-click <code>VBCABLE_Setup_x64.exe</code> and choose <b>Run as "
        "administrator</b>.",
        "Click <b>Install Driver</b>.",
        std::format("Restart Windows, then open {} again.", kDisplayName),
        "After the restart, open <b>Settings › System › Sound</b> and check that Output is still your speakers or "
        "headphones, not CABLE Input."};
    view.picture = Picture{"vb-cable-setup", 4, "VB-Cable's setup window, with Install Driver"};
    view.waiting = true;
    view.status = std::format("Waiting for a virtual cable. {} moves on as soon as one appears.", kDisplayName);
    view.buttons = {Button(kButtonBack, "&Back")};
    if (!choices.empty()) {
      view.buttons.push_back(Button(kButtonKeepDevice, std::format("&Keep {}", EscapeAccessKey(ShortDeviceName(choices[0].name)))));
    }
    return view;
  }
  view.instruction = std::format("Where should {} send your mic?", kDisplayName);
  if (snapshot.state == State::kNeedsSetup && snapshot.setup == SetupNeed::kCable) {
    view.content = "OBS monitors your mic to its default playback device, which is usually your speakers. Choose "
                   "a virtual cable instead.";
  } else if (snapshot.state == State::kCableMissing) {
    view.content =
        std::format("{} isn't connected. Choose another cable.", EscapeMarkup(ShortDeviceName(sends_to.name)));
  } else if (CableUnconfirmed(snapshot)) {
    view.content = std::format("OBS monitors your mic to {}, which isn't a virtual cable. Choose a cable, or keep "
                               "{}.",
                               EscapeMarkup(ShortDeviceName(sends_to.name)),
                               EscapeMarkup(ShortDeviceName(sends_to.name)));
  } else {
    view.content =
        std::format("{} sends your mic to {}.", kDisplayName, EscapeMarkup(ShortDeviceName(sends_to.name)));
  }
  view.footer = "Other playback devices are under Cable in the tray menu.";
  for (size_t i = 0; i < choices.size(); ++i) {
    std::string text = ShortDeviceName(choices[i].name);
    if (SameId(choices[i].id, snapshot.obs_cable.id)) text += "\nOBS's monitoring device";
    view.choices.push_back(Button(kChoiceFirst + static_cast<int>(i), std::move(text)));
  }
  const auto current = std::find_if(choices.begin(), choices.end(),
                                    [&](const audio::AudioDevice& device) { return SameId(device.id, sends_to.id); });
  view.default_choice = kChoiceFirst + static_cast<int>(current == choices.end() ? 0 : current - choices.begin());
  view.buttons = {Button(kButtonBack, "&Back"), Button(kButtonNext, "&Next")};
  view.default_button = kButtonNext;
  return view;
}

PageView Warnings(const core::Snapshot& snapshot) {
  PageView view;
  view.instruction = "Before you start";
  for (const import::ImportNote& note : HeadsUp(snapshot)) {
    view.notes.push_back({note.warning, NoteMarkup(note)});
  }
  view.buttons = {Button(kButtonBack, "&Back"), Button(kButtonNext, "&Next")};
  view.default_button = kButtonNext;
  return view;
}

PageView Done(const core::Snapshot& snapshot, bool start_checked) {
  PageView view;
  view.instruction = "Your mic is set up";
  core::ChainSummary chain = snapshot.chain.value_or(core::ChainSummary{});
  const std::string side = CableRecordingSide(chain.cable);
  chain.cable = ShortDeviceName(chain.cable);
  if (snapshot.chain) view.chain = chain;
  view.content = std::format(
      "In your other apps, choose {0} as your mic.\n\n"
      "{1} runs in the tray, next to the clock. If you don't see its icon, click ^ (Show hidden icons). {2}",
      side.empty() ? std::format("the recording side of {}", EscapeMarkup(chain.cable))
                   : std::format("<b>{}</b>", EscapeMarkup(side)),
      kDisplayName,
      snapshot.settings.pause_for_obs ? "It pauses while OBS is open, and reads your filters again each time OBS "
                                        "closes."
                                      : "It reads your filters again each time OBS closes.");
  switch (snapshot.state) {
    case State::kPausedForObs:
      view.content += std::format("\n\nOBS is open now, so {} starts when you close it.", kDisplayName);
      break;
    case State::kPausedByUser:
      view.content += "\n\nIt's paused now. Resume it from the tray menu.";
      break;
    case State::kMicMissing:
      view.content +=
          std::format("\n\n{} {} starts when the mic is back.", EscapeMarkup(snapshot.detail), kDisplayName);
      break;
    default:
      break;
  }
  view.check = std::format("Start {} with &Windows", kDisplayName);
  view.checked = start_checked;
  view.buttons = {Button(kButtonBack, "&Back"), Button(kButtonDone, "&Done")};
  view.default_button = kButtonDone;
  return view;
}

}  // namespace

std::string_view PageName(FirstRunPage page) {
  for (const auto& [named, name] : kPageNames) {
    if (named == page) return name;
  }
  return "";
}

std::optional<FirstRunPage> PageNamed(std::string_view name) {
  for (const auto& [page, page_name] : kPageNames) {
    if (page_name == name) return page;
  }
  return std::nullopt;
}

std::string WithoutAccessKey(std::string_view text) {
  std::string plain;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '&' && i + 1 < text.size()) ++i;
    plain += text[i];
  }
  return plain;
}

std::array<StageView, kStageCount> StageViews(const core::Snapshot& snapshot) {
  const StageView cable = CableStage(snapshot);
  return {MicStage(snapshot), FiltersStage(snapshot), cable, AppsStage(snapshot, cable)};
}

bool AnyFilters(const core::Snapshot& snapshot) {
  if (snapshot.chain && !snapshot.chain->filters.empty()) return true;
  return std::any_of(snapshot.mics.begin(), snapshot.mics.end(),
                     [](const import::MicCandidate& mic) { return !mic.filters.empty(); });
}

std::vector<audio::AudioDevice> CableChoices(const core::Snapshot& snapshot) {
  std::vector<audio::AudioDevice> choices = VirtualCables(snapshot);
  const audio::AudioDevice sends_to = SendsTo(snapshot);
  if (const audio::AudioDevice* connected = FindById(snapshot.outputs, sends_to.id);
      connected && !IsVirtualCable(connected->name)) {
    choices.push_back(*connected);
  }
  return choices;
}

FirstRun::FirstRun(FirstRunProgress progress, std::optional<FirstRunPage> page, bool start_with_windows)
    : progress_(progress), start_checked_(!progress.done || start_with_windows) {
  if (!page) return;
  switch (*page) {
    case Page::kWelcome:
      progress_.door = Door::kNone;
      break;
    case Page::kSteps:
      progress_.door = Door::kNewToObs;
      break;
    default:
      progress_.door = Door::kObsUser;
      if (*page >= Page::kMic) progress_.reached = std::min(progress_.reached, *page);
      focus_ = *page;
      break;
  }
}

FirstRunPage FirstRun::Page(const core::Snapshot& snapshot) const {
  if (progress_.door == Door::kNone) return Page::kWelcome;
  if (progress_.door == Door::kNewToObs) return Page::kSteps;
  if (const auto blocking = Blocking(snapshot)) return *blocking;
  if (focus_ && Applies(*focus_, snapshot)) return *focus_;
  const auto unanswered = [this](FirstRunPage page) { return progress_.reached <= page; };
  if (unanswered(Page::kMic) && snapshot.mics.size() > 1) return Page::kMic;
  if (unanswered(Page::kFilters) && snapshot.chain && snapshot.chain->filters.empty()) return Page::kFilters;
  if (NeedsCable(snapshot) || (unanswered(Page::kCable) && CableUnconfirmed(snapshot))) return Page::kCable;
  if (unanswered(Page::kWarnings) && !HeadsUp(snapshot).empty()) return Page::kWarnings;
  return Page::kDone;
}

PageView FirstRun::View(const core::Snapshot& snapshot, int choice, std::optional<bool> checked) const {
  const FirstRunPage page = Page(snapshot);
  PageView view;
  switch (page) {
    case Page::kWelcome:
      view = Welcome(snapshot);
      break;
    case Page::kSteps:
      view = Steps(snapshot);
      break;
    case Page::kLooking:
      view = Looking();
      break;
    case Page::kFindObs:
      view = FindObs(snapshot);
      break;
    case Page::kObsUnsupported:
      view = ObsUnsupported(snapshot);
      break;
    case Page::kObsSettings:
      view = ObsSettings(snapshot);
      break;
    case Page::kProblem:
      view = Problem(snapshot);
      break;
    case Page::kMic:
      view = Mic(snapshot);
      break;
    case Page::kFilters:
      view = Filters(snapshot);
      break;
    case Page::kCable:
      view = Cable(snapshot);
      break;
    case Page::kWarnings:
      view = Warnings(snapshot);
      break;
    case Page::kDone:
      view = Done(snapshot, start_checked_);
      break;
  }
  view.page = page;
  view.stage = StageOf(page, snapshot);
  view.stages = StageViews(snapshot);
  if (std::any_of(view.choices.begin(), view.choices.end(), [choice](const PageButton& c) { return c.id == choice; })) {
    view.default_choice = choice;
  }
  if (checked && !view.check.empty()) view.checked = *checked;
  return view;
}

FirstRunAction FirstRun::Click(int button, int choice, bool checked, const core::Snapshot& snapshot) {
  const FirstRunPage page = Page(snapshot);
  core::Settings settings = snapshot.settings;
  const auto apply = [&settings, &snapshot]() {
    if (settings == snapshot.settings) return FirstRunAction{};
    return FirstRunAction{.kind = FirstRunAction::Kind::kApply, .settings = settings};
  };
  const size_t index = choice >= kChoiceFirst ? static_cast<size_t>(choice - kChoiceFirst) : SIZE_MAX;
  switch (button) {
    case kButtonObsUser:
      Leave(page);
      progress_.door = Door::kObsUser;
      progress_.reached = Page::kMic;
      return {};
    case kButtonNewToObs:
    case kButtonShowSteps:
      Leave(page);
      progress_.door = Door::kNewToObs;
      return {};
    case kButtonBack:
      Back(snapshot);
      return {};
    case kButtonOpenObs:
      return {.kind = FirstRunAction::Kind::kOpenObs};
    case kButtonChooseObs:
      return {.kind = FirstRunAction::Kind::kChooseObs};
    case kButtonChooseObsSettings:
      return {.kind = FirstRunAction::Kind::kChooseObsSettings};
    case kButtonLookForObs:
      settings.obs_dir.reset();
      return apply();
    case kButtonOwnObsSettings:
      settings.obs_config.reset();
      return apply();
    case kButtonTryAgain:
      return {.kind = FirstRunAction::Kind::kReimport};
    case kButtonRestart:
      return {.kind = FirstRunAction::Kind::kRestart};
    case kButtonSkipFilters:
      Leave(page);
      progress_.reached = std::max(progress_.reached, Page::kCable);
      return {};
    case kButtonKeepDevice:
      Leave(page);
      progress_.reached = std::max(progress_.reached, Page::kWarnings);
      return {};
    case kButtonDone:
      progress_.done = true;
      return {.kind = FirstRunAction::Kind::kFinish, .start_with_windows = checked};
    case kButtonNext:
      break;
    default:
      return {};
  }
  switch (page) {
    case Page::kSteps:
      Leave(page);
      progress_.door = Door::kObsUser;
      progress_.reached = Page::kMic;
      return {};
    case Page::kMic:
      Leave(page);
      progress_.reached = std::max(progress_.reached, Page::kFilters);
      // An empty pick takes the only mic, and follows it in OBS.
      if (index < snapshot.mics.size()) settings.mic = snapshot.mics.size() == 1 ? "" : snapshot.mics[index].name;
      return apply();
    case Page::kCable: {
      Leave(page);
      progress_.reached = std::max(progress_.reached, Page::kWarnings);
      const std::vector<audio::AudioDevice> choices = CableChoices(snapshot);
      if (index < choices.size()) {
        // OBS's own device is no pick: knobs follows OBS's choice.
        const bool obs = SameId(choices[index].id, snapshot.obs_cable.id);
        settings.cable = obs ? "" : choices[index].id;
        settings.cable_name = obs ? "" : choices[index].name;
      }
      return apply();
    }
    case Page::kWarnings:
      Leave(page);
      progress_.reached = std::max(progress_.reached, Page::kDone);
      return {};
    default:
      return {};
  }
}

FirstRunAction FirstRun::Changed(const core::Snapshot& before, const core::Snapshot& after) {
  if (progress_.door == Door::kNewToObs) {
    // OBS saved the user's work as it closed, and the core imported it.
    if (before.obs_running && !after.obs_running && AnyFilters(after)) {
      Leave(Page::kSteps);
      progress_.door = Door::kObsUser;
      progress_.reached = Page::kMic;
    }
    return {};
  }
  if (progress_.door != Door::kObsUser || Page(before) != Page::kCable || !VirtualCables(before).empty()) return {};
  // Waiting for a cable: one has appeared. If it's the only one, it's the
  // one, and the first run moves on with it.
  const std::vector<audio::AudioDevice> cables = VirtualCables(after);
  if (cables.size() != 1 || Page(after) != Page::kCable) return {};
  Leave(Page::kCable);
  progress_.reached = std::max(progress_.reached, Page::kWarnings);
  core::Settings settings = after.settings;
  const bool obs = SameId(cables[0].id, after.obs_cable.id);
  settings.cable = obs ? "" : cables[0].id;
  settings.cable_name = obs ? "" : cables[0].name;
  if (settings == after.settings) return {};
  return {.kind = FirstRunAction::Kind::kApply, .settings = settings};
}

void FirstRun::Leave(FirstRunPage page) {
  history_.push_back(page);
  focus_.reset();
}

void FirstRun::Back(const core::Snapshot& snapshot) {
  focus_.reset();
  while (!history_.empty()) {
    const FirstRunPage page = history_.back();
    history_.pop_back();
    if (page == Page::kWelcome) break;
    if (page == Page::kSteps) {
      progress_.door = Door::kNewToObs;
      return;
    }
    if (Applies(page, snapshot)) {
      progress_.door = Door::kObsUser;
      if (page >= Page::kMic) progress_.reached = std::min(progress_.reached, page);
      focus_ = page;
      return;
    }
  }
  progress_.door = Door::kNone;
}

}  // namespace knobs::tray
