// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for the first run as data (src/tray/first_run.h): which page
// each state calls for on each door, what the pages say and offer, and how
// clicks and changes of state move the first run on.

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app_info.h"
#include "core/state.h"
#include "test_harness.h"
#include "tray/first_run.h"
#include "tray/rich_text.h"
#include "tray/setup_scene.h"

namespace {

using namespace knobs;
using namespace knobs::tray;
using core::SetupNeed;
using core::State;
using Kind = FirstRunAction::Kind;
using Page = FirstRunPage;

constexpr char kMicId[] = "{0.0.1.00000000}.{audient-id4}";
constexpr char kHeadsetId[] = "{0.0.1.00000000}.{headset}";
constexpr char kCable16Id[] = "{0.0.0.00000000}.{cable-16ch}";
constexpr char kCable16[] = "CABLE In 16ch (VB-Audio Virtual Cable)";
constexpr char kCableInputId[] = "{0.0.0.00000000}.{cable-input}";
constexpr char kCableInput[] = "CABLE Input (VB-Audio Virtual Cable)";
constexpr char kSpeakersId[] = "{0.0.0.00000000}.{speakers}";
constexpr char kSpeakers[] = "Speakers (Realtek(R) Audio)";
constexpr char kHeadphonesId[] = "{0.0.0.00000000}.{headphones}";
constexpr char kHeadphones[] = "Headphones (Audient iD4)";

const std::vector<std::string> kFourFilters = {"Noise Suppression", "Noise Gate", "Compressor", "Limiter"};

import::MicCandidate Mic(std::string name, std::string device_id, std::vector<std::string> filters,
                         bool monitored = true) {
  import::MicCandidate mic;
  mic.name = std::move(name);
  mic.device_id = std::move(device_id);
  mic.filters = std::move(filters);
  mic.monitored = monitored;
  return mic;
}

// Running: Mic/Aux with four filters into CABLE In 16ch, as OBS monitors it.
core::Snapshot Running() {
  core::Snapshot snapshot;
  snapshot.state = State::kRunning;
  snapshot.chain = core::ChainSummary{"Mic/Aux", kFourFilters, kCable16};
  snapshot.mics = {Mic("Mic/Aux", kMicId, kFourFilters)};
  snapshot.picked_mic = 0;
  snapshot.chain_revision = 1;
  snapshot.obs_cable = {kCable16, kCable16Id};
  snapshot.outputs = {{kSpeakers, kSpeakersId}, {kCableInput, kCableInputId}, {kCable16, kCable16Id}};
  snapshot.inputs = {{"Microphone (Audient iD4)", kMicId}, {"Headset Microphone (USB)", kHeadsetId}};
  snapshot.default_input = kHeadsetId;
  snapshot.obs = {"C:\\Program Files\\obs-studio", {32, 2, 2}};
  return snapshot;
}

// Three mics, none picked.
core::Snapshot PickMic() {
  core::Snapshot snapshot = Running();
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kPickMic;
  snapshot.chain.reset();
  snapshot.picked_mic.reset();
  snapshot.mics = {Mic("Desk Mic", "default", {}), Mic("Mic/Aux", kMicId, kFourFilters, false),
                   Mic("Podcast Mic", "{0.0.1.00000000}.{gone}", {"Gain"})};
  return snapshot;
}

// The first door's pages, from the welcome page.
FirstRun ObsUser(const core::Snapshot& snapshot) {
  FirstRun first_run({});
  first_run.Click(kButtonObsUser, 0, {}, snapshot);
  return first_run;
}

std::vector<std::string> Texts(const std::vector<PageButton>& buttons) {
  std::vector<std::string> texts;
  for (const PageButton& button : buttons) texts.push_back(WithoutAccessKey(button.text));
  return texts;
}

const PageButton* FindButton(const PageView& view, int id) {
  const auto found = std::find_if(view.buttons.begin(), view.buttons.end(),
                                  [id](const PageButton& button) { return button.id == id; });
  return found == view.buttons.end() ? nullptr : &*found;
}

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// --- The welcome page ---------------------------------------------------------------

TEST(WelcomeSaysWhatWasFound) {
  const auto notes = [](const core::Snapshot& snapshot) {
    const PageView view = FirstRun({}).View(snapshot);
    CHECK(view.page == Page::kWelcome && view.command_links && view.buttons.size() == 2);
    return std::format("{} | {} | {}", WithoutAccessKey(view.buttons[0].text),
                       WithoutAccessKey(view.buttons[1].text), view.default_button == kButtonObsUser ? "obs" : "new");
  };
  core::Snapshot snapshot = Running();
  CHECK(notes(snapshot) ==
        "I set up my mic in OBS\nFound Mic/Aux with 4 filters. | I'm new to OBS\nSet up your mic's filters in OBS, "
        "step by step. | obs");
  snapshot.chain->filters.clear();
  snapshot.mics[0].filters.clear();
  CHECK(notes(snapshot).starts_with("I set up my mic in OBS\nMic/Aux has no filters yet. |"));
  CHECK(notes(snapshot).ends_with("| new"));
  CHECK(notes(PickMic()).starts_with("I set up my mic in OBS\nFound 3 mics: Desk Mic, Mic/Aux and Podcast Mic. |"));
  CHECK(notes(PickMic()).ends_with("| obs"));

  // A copy restarted for NVIDIA's SDK waits for it before its first import:
  // as when starting, the door for OBS users.
  core::Snapshot waiting;
  waiting.state = State::kStarting;
  waiting.files_waiting = true;
  waiting.detail = "Waiting for the updated files to stop changing.";
  waiting.nvidia_update = true;
  CHECK(notes(waiting).starts_with("I set up my mic in OBS\nWaiting for the updated files to stop changing."));
  CHECK(notes(waiting).ends_with("| obs"));

  core::Snapshot missing;
  missing.state = State::kObsMissing;
  CHECK(Contains(notes(missing), "I'm new to OBS\nOBS Studio isn't installed."));
  CHECK(notes(missing).ends_with("| new"));
  core::Snapshot unsupported = missing;
  unsupported.state = State::kObsUnsupported;
  unsupported.obs = {"C:\\Program Files\\obs-studio", {34, 0, 0}};
  CHECK(Contains(notes(unsupported), "\nOBS 34.0.0 is installed, and "));
  CHECK(notes(unsupported).ends_with("| obs"));
  // Too old: no update will fix it, so not "yet".
  core::Snapshot too_old = unsupported;
  too_old.obs.version = {31, 1, 4};
  CHECK(Contains(notes(too_old), std::format("\nOBS 31.1.4 is installed, which is too old for {}.", kDisplayName)));
  CHECK(!Contains(notes(too_old), "yet"));
  core::Snapshot settings = missing;
  settings.state = State::kNeedsSetup;
  settings.setup = SetupNeed::kObsSettings;
  CHECK(Contains(notes(settings), "\nCan't read OBS's settings.") && notes(settings).ends_with("| new"));
  settings.setup = SetupNeed::kNoMic;
  CHECK(Contains(notes(settings), "\nOBS has no mic yet.") && notes(settings).ends_with("| new"));
  CHECK(Contains(notes(core::Snapshot{}), "\nLooking for OBS…"));

  const PageView view = FirstRun({}).View(Running());
  CHECK(view.stage == Stage::kFilters);
  CHECK(view.footer == std::format("{} isn't affiliated with or endorsed by the OBS Project.", kDisplayName));
}

// --- The first door -----------------------------------------------------------------

TEST(FirstDoorShowsOnlyWhatsNeeded) {
  // Everything's in place: the cable, then the last page. The cable is asked
  // even though OBS monitors to one, and the answer is the user's own pick,
  // even of OBS's device.
  FirstRun first_run = ObsUser(Running());
  CHECK(first_run.progress().door == Door::kObsUser && first_run.Page(Running()) == Page::kCable);
  const PageView cable = first_run.View(Running());
  CHECK(cable.instruction == std::format("Where should {} send your mic?", kDisplayName));
  CHECK(cable.content == std::format("Choose the virtual cable {} sends your mic to. Your other apps then use it as "
                                     "their mic.",
                                     kDisplayName));
  CHECK(Texts(cable.choices) == (std::vector<std::string>{"CABLE Input", "CABLE In 16ch\nOBS's monitoring device"}));
  CHECK(Texts(cable.more_choices) == (std::vector<std::string>{"Speakers"}));
  CHECK(cable.more_label == "Other playback devices" && cable.footer.empty());
  CHECK(cable.default_choice == kChoiceFirst + 1 && cable.default_button == kButtonNext);
  const FirstRunAction picked = first_run.Click(kButtonNext, kChoiceFirst + 1, {}, Running());
  CHECK(picked.kind == Kind::kApply && picked.settings.cable == kCable16Id && picked.settings.cable_name == kCable16);
  CHECK(first_run.Page(Running()) == Page::kDone);
  const PageView done = first_run.View(Running());
  CHECK(done.instruction == "Your mic is set up");
  CHECK((done.chain == core::ChainSummary{"Mic/Aux", kFourFilters, "CABLE In 16ch"}));
  CHECK(done.content.starts_with("In your other apps, choose <b>CABLE Output</b> as your mic."));
  CHECK(done.stage == Stage::kApps);
  CHECK(Contains(done.content, "click ^ (Show hidden icons)"));
  CHECK(Contains(done.content, "It pauses while OBS is open"));
  CHECK(done.checks.size() == 2 && done.checks[0].id == kCheckStartWithWindows &&
        done.checks[0].text == std::format("Start {} with &Windows", kDisplayName) && done.checks[0].checked);
  CHECK(done.default_button == kButtonDone);
  CHECK(!first_run.View(Running(), 0, {{kCheckStartWithWindows, false}}).checks[0].checked);

  const FirstRunAction finish = first_run.Click(kButtonDone, 0, {{kCheckStartWithWindows, true}}, Running());
  CHECK(finish.kind == Kind::kFinish && finish.finish.start_with_windows && first_run.progress().done);
  CHECK(!first_run.Click(kButtonDone, 0, {{kCheckStartWithWindows, false}}, Running()).finish.start_with_windows);

  // A first run offers Start with Windows checked, even while it's off. One
  // that was finished, opened again, offers it as it is, so that Done
  // doesn't turn it back on.
  const PageView first = FirstRun({.door = Door::kObsUser, .reached = Page::kDone}, std::nullopt, {}).View(Running());
  CHECK(first.page == Page::kDone && first.checks[0].checked);

  // Also on the way from a page a notification opened.
  const FirstRunProgress finished = {.done = true, .door = Door::kObsUser, .reached = Page::kDone};
  core::Snapshot warned = Running();
  warned.notes = {{true, "Filter \"ReaComp\" is a VST plugin."}};
  for (const bool on : {false, true}) {
    FirstRun again(finished, std::nullopt, {.start_with_windows = on});
    PageView last = again.View(Running());
    CHECK(last.page == Page::kDone && last.checks[0].checked == on);
    CHECK(again.Click(kButtonDone, 0, {}, Running()).finish.start_with_windows == on);

    FirstRun warnings(finished, Page::kWarnings, {.start_with_windows = on});
    CHECK(warnings.Page(warned) == Page::kWarnings);
    warnings.Click(kButtonNext, 0, {}, warned);
    last = warnings.View(warned);
    CHECK(last.page == Page::kDone && last.checks[0].checked == on);
  }

  // The last page says why the mic isn't running yet.
  core::Snapshot paused = Running();
  paused.state = State::kPausedForObs;
  paused.obs_running = true;
  CHECK(Contains(first_run.View(paused).content, "OBS is open now, so "));
  paused.settings.pause_for_obs = false;
  CHECK(!Contains(first_run.View(paused).content, "It pauses while OBS is open"));
  core::Snapshot missing = Running();
  missing.state = State::kMicMissing;
  missing.detail = "The recording device for \"Mic/Aux\" isn't connected.";
  CHECK(Contains(first_run.View(missing).content, "isn't connected. "));
  core::Snapshot updating = Running();
  updating.state = State::kPausedForUpdate;
  CHECK(Contains(first_run.View(updating).content, "paused while a plugin is being updated"));
  updating.nvidia_update = true;
  CHECK(Contains(first_run.View(updating).content, "paused while NVIDIA's Audio Effects SDK is being updated"));
  updating.chain.reset();  // The new NVIDIA copy has not imported yet.
  updating.state = State::kStarting;
  updating.files_waiting = true;
  updating.detail = "Waiting for the updated files to stop changing.";
  CHECK(first_run.View(updating).page == Page::kLooking);
  CHECK(first_run.View(updating).instruction == "Waiting for NVIDIA's SDK update…");
  updating.nvidia_update = false;
  CHECK(first_run.View(updating).instruction == "Waiting for a plugin update…");
}

TEST(SeveralMicsAskWhichOne) {
  core::Snapshot snapshot = PickMic();
  FirstRun first_run = ObsUser(snapshot);
  const PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kMic);
  CHECK(view.instruction == std::format("Which mic should {} run?", kDisplayName));
  CHECK(Texts(view.choices) ==
        (std::vector<std::string>{"Desk Mic\nDefault communications device (Headset Microphone (USB)), no filters",
                                  "Mic/Aux\nMicrophone (Audient iD4), 4 filters",
                                  "Podcast Mic\ndevice not connected, 1 filter"}));
  CHECK(view.stage == Stage::kMic);
  // One with filters that OBS monitors, so sends to a cable.
  CHECK(view.default_choice == kChoiceFirst + 2);
  snapshot.mics[2].monitored = false;
  CHECK(ObsUser(snapshot).View(snapshot).default_choice == kChoiceFirst + 1);

  const FirstRunAction picked = first_run.Click(kButtonNext, kChoiceFirst + 2, {}, snapshot);
  CHECK(picked.kind == Kind::kApply && picked.settings.mic == "Podcast Mic");
  CHECK(first_run.progress().reached == Page::kFilters);
  // Until the core has it, the state still asks for a mic.
  CHECK(first_run.Page(snapshot) == Page::kMic);

  // Picked, the page is a question answered: on to the cable.
  snapshot = Running();
  snapshot.mics = PickMic().mics;
  snapshot.picked_mic = 2;
  snapshot.chain = core::ChainSummary{"Podcast Mic", {"Gain"}, kCable16};
  snapshot.settings.mic = "Podcast Mic";
  CHECK(first_run.Page(snapshot) == Page::kCable);
  CHECK(ObsUser(snapshot).Page(snapshot) == Page::kMic);
  CHECK(ObsUser(snapshot).View(snapshot).default_choice == kChoiceFirst + 2);
  FirstRun again = ObsUser(snapshot);
  CHECK(again.Click(kButtonNext, kChoiceFirst + 2, {}, snapshot).kind == Kind::kNone);

  // A pick that's gone from OBS.
  snapshot = PickMic();
  snapshot.settings.mic = "Old Mic";
  CHECK(Contains(ObsUser(snapshot).View(snapshot).content, "The one chosen before, \"Old Mic\", is no longer"));
  // There's no default device.
  snapshot.default_input.clear();
  CHECK(ObsUser(snapshot).View(snapshot).choices[0].text.starts_with(
      "Desk Mic\nDefault communications device (none connected)"));
}

TEST(NoFiltersOffersTheSteps) {
  core::Snapshot snapshot = Running();
  snapshot.chain->filters.clear();
  snapshot.mics[0].filters.clear();
  FirstRun first_run = ObsUser(snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kFilters && view.instruction == "Mic/Aux has no filters yet");
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Continue without filters", "Show the steps"}));
  CHECK(view.default_button == kButtonShowSteps);

  CHECK(first_run.Click(kButtonShowSteps, 0, {}, snapshot).kind == Kind::kNone);
  CHECK(first_run.Page(snapshot) == Page::kSteps);
  first_run.Click(kButtonBack, 0, {}, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kFilters);
  first_run.Click(kButtonSkipFilters, 0, {}, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kCable);
  first_run.Click(kButtonNext, kChoiceFirst + 1, {}, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kDone);
  CHECK((first_run.View(snapshot).chain == core::ChainSummary{"Mic/Aux", {}, "CABLE In 16ch"}));

  // With OBS open, what it saved may be behind.
  snapshot.obs_running = true;
  CHECK(Contains(ObsUser(snapshot).View(snapshot).footer, "reads its settings again when OBS closes"));

  // No mic at all: OBS has to have one first.
  core::Snapshot none = Running();
  none.state = State::kNeedsSetup;
  none.setup = SetupNeed::kNoMic;
  none.chain.reset();
  none.mics.clear();
  none.picked_mic.reset();
  first_run = FirstRun({.door = Door::kObsUser, .reached = Page::kDone});
  view = first_run.View(none);
  CHECK(view.page == Page::kFilters && view.instruction == "OBS has no mic yet");
  CHECK(Contains(view.content, "open OBS's Settings › Audio, set Mic/Auxiliary Audio to your mic"));
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Open OBS", "Show the steps"}));
}

TEST(CableWhenOBSMonitorsToTheDefaultDevice) {
  core::Snapshot snapshot = Running();
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kCable;
  snapshot.chain->cable.clear();
  snapshot.obs_cable = {"", "default"};
  FirstRun first_run = ObsUser(snapshot);
  const PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kCable && !view.waiting);
  CHECK(Contains(view.content, "doesn't send it where OBS monitors it, your default playback device"));
  CHECK(Texts(view.choices) == (std::vector<std::string>{"CABLE Input", "CABLE In 16ch"}));
  CHECK(view.default_choice == kChoiceFirst);
  const FirstRunAction picked = first_run.Click(kButtonNext, kChoiceFirst + 1, {}, snapshot);
  CHECK(picked.kind == Kind::kApply && picked.settings.cable == kCable16Id && picked.settings.cable_name == kCable16);
  CHECK(first_run.progress().reached == Page::kWarnings);
  // Still needed until the core has the cable.
  CHECK(first_run.Page(snapshot) == Page::kCable);
  core::Snapshot running = Running();
  running.settings = picked.settings;
  CHECK(first_run.Page(running) == Page::kDone);
}

TEST(CableAppearsAndTheUserChoosesIt) {
  core::Snapshot none = Running();
  none.state = State::kNeedsSetup;
  none.setup = SetupNeed::kCable;
  none.chain->cable.clear();
  none.obs_cable = {"", "default"};
  none.outputs = {{kSpeakers, kSpeakersId}};
  FirstRun first_run = ObsUser(none);
  PageView view = first_run.View(none);
  CHECK(view.page == Page::kCable && view.waiting && view.instruction == "Install a virtual cable");
  // The guide to installing VB-Cable, with a walkthrough of it. Its devices
  // don't wait for a restart: when they do, the page says so.
  CHECK(view.content == "There's no virtual cable on this PC yet. To install VB-Cable:");
  CHECK(view.steps.size() == 4);
  CHECK(view.steps[0] == "Download its driver pack from <a href=\"https://vb-audio.com/Cable/\"><code>vb-audio.com/"
                         "Cable</code></a>.");
  CHECK(Contains(view.steps[2], "right-click <code>VBCABLE_Setup_x64.exe</code>, choose <b>Run as administrator</b>, "
                                "then <b>Yes</b>"));
  CHECK(view.steps[3] == "Click <b>Install Driver</b>.");
  CHECK(view.walkthrough && view.walkthrough->name == "vb-cable-setup");
  CHECK(view.status ==
        std::format("Waiting for a virtual cable. {} shows it here as soon as one appears.", kDisplayName));
  CHECK(view.stage == Stage::kCable);
  // Or another playback device, from a dropdown, such as a cable knobs
  // doesn't know by name: Next goes on with the one chosen.
  CHECK(view.choices.empty() && Texts(view.more_choices) == (std::vector<std::string>{"Speakers"}));
  CHECK(view.more_label == "Use another playback device" && view.default_choice == 0);
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Next"}) && !FindButton(view, kButtonNext)->enabled);
  CHECK(FindButton(first_run.View(none, kChoiceFirst), kButtonNext)->enabled);

  // VB-Cable installs: its one input appears, and the page turns into the
  // choice, with it chosen, for the user to go on with. Speakers keeps its
  // choice: one stays with its device as devices come and go.
  core::Snapshot installed = none;
  installed.outputs.push_back({kCableInput, kCableInputId});
  CHECK(first_run.Changed(none, installed).kind == Kind::kNone);
  view = first_run.View(installed, kChoiceFirst);
  CHECK(view.page == Page::kCable && !view.waiting && Texts(view.choices) == (std::vector<std::string>{"CABLE Input"}));
  CHECK(Texts(view.more_choices) == (std::vector<std::string>{"Speakers"}) && view.more_choices[0].id == kChoiceFirst);
  CHECK(view.default_choice == kChoiceFirst && view.choices[0].id == kChoiceFirst + 1);
  CHECK(first_run.View(installed).default_choice == kChoiceFirst + 1);
  const FirstRunAction picked = first_run.Click(kButtonNext, kChoiceFirst + 1, {}, installed);
  CHECK(picked.kind == Kind::kApply && picked.settings.cable == kCableInputId);
  installed.settings = picked.settings;
  installed.state = State::kRunning;
  CHECK(first_run.Page(installed) == Page::kDone);
  // Back goes to it.
  first_run.Click(kButtonBack, 0, {}, installed);
  CHECK(first_run.Page(installed) == Page::kCable);

  // Another playback device, from the dropdown.
  first_run = ObsUser(none);
  const FirstRunAction speakers = first_run.Click(kButtonNext, kChoiceFirst, {}, none);
  CHECK(speakers.kind == Kind::kApply && speakers.settings.cable == kSpeakersId);

  // A choice of a device that's gone is as none: the page's own choice, and
  // Next goes on with that, not with nothing.
  first_run = ObsUser(installed);
  CHECK(first_run.View(installed, kChoiceFirst + 1).default_choice == kChoiceFirst + 1);
  // CABLE Input, gone again.
  view = first_run.View(none, kChoiceFirst);
  CHECK(view.default_choice == 0 && !FindButton(view, kButtonNext)->enabled);
  CHECK(first_run.Click(kButtonNext, kChoiceFirst, {}, none).kind == Kind::kNone);
  view = first_run.View(installed, kChoiceFirst + 7);
  CHECK(view.default_choice == kChoiceFirst && FindButton(view, kButtonNext)->enabled);
}

TEST(CableThatsOffOrWaitsForARestart) {
  core::Snapshot none = Running();
  none.state = State::kNeedsSetup;
  none.setup = SetupNeed::kCable;
  none.chain->cable.clear();
  none.obs_cable = {"", "default"};
  none.outputs = {{kSpeakers, kSpeakersId}};

  // Installed, but turned off in Windows' Sound settings: how to turn it on,
  // in Windows 11's words or Windows 10's.
  core::Snapshot off = none;
  off.disabled_outputs = {{kSpeakers, kSpeakersId}, {kCableInput, kCableInputId}};
  FirstRun first_run = ObsUser(off);
  PageView view = first_run.View(off);
  CHECK(view.page == Page::kCable && view.waiting && view.instruction == "Turn on your virtual cable");
  CHECK(view.content == "CABLE Input is installed, but it's turned off in Windows' Sound settings.");
  CHECK((view.steps == std::vector<std::string>{"Click <b>Open Sound settings</b>, below.",
                                                 "Under Output devices, choose <b>CABLE Input</b>.",
                                                 "Next to Audio, click <b>Allow</b>."}));
  const PageView windows10 = FirstRun({.door = Door::kObsUser}, std::nullopt, {}, false, false).View(off);
  CHECK((windows10.steps == std::vector<std::string>{"Click <b>Open Sound settings</b>, below.",
                                                      "Under Output devices, click <b>CABLE Input</b>.",
                                                      "Click <b>Enable</b>."}));
  CHECK(!view.walkthrough && !view.resume_after_restart);
  CHECK(view.default_button == kButtonOpenSoundSettings);
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Open Sound settings", "Next"}));
  CHECK(first_run.Click(kButtonOpenSoundSettings, 0, {}, off).kind == Kind::kOpenSoundSettings);
  CHECK(view.stages[static_cast<size_t>(Stage::kCable)].value == "CABLE Input\nTurned off");
  // Turned on, it's the choice, and it stays the user's to make: a pick
  // that's missing isn't replaced by itself.
  core::Snapshot on = off;
  on.disabled_outputs = {{kSpeakers, kSpeakersId}};
  on.outputs.push_back({kCableInput, kCableInputId});
  CHECK(first_run.Changed(off, on).kind == Kind::kNone);
  CHECK(first_run.View(on).default_choice == kChoiceFirst + 1 && !first_run.View(on).waiting);

  // VB-Cable is installed, but its devices wait for Windows to restart, as
  // they may after its setup or the installer's: knobs opens again after it.
  core::Snapshot restart = none;
  restart.vb_cable_pending = true;
  first_run = ObsUser(restart);
  view = first_run.View(restart);
  CHECK(view.page == Page::kCable && view.waiting);
  CHECK(view.instruction == "Restart Windows to finish installing VB-Cable");
  CHECK(Contains(view.content, std::format("{} opens again after the restart", kDisplayName)));
  CHECK(view.resume_after_restart && view.steps.empty());
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Next"}));
  CHECK(view.stages[static_cast<size_t>(Stage::kCable)].value == "VB-Cable\nWaits for a restart");
  core::Snapshot started = restart;
  started.outputs.push_back({kCableInput, kCableInputId});
  CHECK(first_run.Changed(restart, started).kind == Kind::kNone && first_run.Page(started) == Page::kCable);

  // The device knobs sends to is chosen in the dropdown, to go on with.
  core::Snapshot keep = restart;
  keep.obs_cable = {kHeadphones, kHeadphonesId};
  keep.outputs = {{kSpeakers, kSpeakersId}, keep.obs_cable};
  keep.state = State::kRunning;
  view = ObsUser(keep).View(keep);
  CHECK(Texts(view.more_choices) == (std::vector<std::string>{"Speakers", "Headphones\nOBS's monitoring device"}));
  CHECK(view.default_choice == kChoiceFirst + 1 && FindButton(view, kButtonNext)->enabled);
}

TEST(LastPageAsksAboutUpdates) {
  // Both off until the user turns them on, and updating by itself only in
  // a copy the installer installed.
  const FirstRunProgress progress{.door = Door::kObsUser, .reached = Page::kDone};
  PageView view = FirstRun(progress, std::nullopt, {}, true).View(Running());
  CHECK(view.checks.size() == 3);
  CHECK(view.checks[1].id == kCheckForUpdates && view.checks[1].text == "Automatically &check for new versions" &&
        !view.checks[1].checked);
  CHECK(view.checks[2].id == kCheckInstallUpdates &&
        view.checks[2].text == "Automatically &update when a new version is available" && !view.checks[2].checked);
  // Updating by itself needs the check: disabled without it.
  CHECK(view.checks[2].needs_previous && !view.checks[2].enabled);
  CHECK(FirstRun(progress).View(Running()).checks.size() == 2);

  FirstRun first_run(progress, std::nullopt, {}, true);
  view = first_run.View(Running(), 0, {{kCheckForUpdates, true}});
  CHECK(view.checks[1].checked && view.checks[2].enabled && !view.checks[2].checked);
  // Turning the check off keeps updating by itself as it was, for when it's
  // on again.
  view = first_run.View(Running(), 0, {{kCheckForUpdates, false}, {kCheckInstallUpdates, true}});
  CHECK(!view.checks[2].enabled && view.checks[2].checked);
  FirstRunAction done = first_run.Click(kButtonDone, 0, {{kCheckForUpdates, true}, {kCheckInstallUpdates, true}},
                                        Running());
  CHECK(done.kind == Kind::kFinish && done.finish.check_for_updates && done.finish.install_updates &&
        done.finish.start_with_windows);
  first_run = FirstRun(progress, std::nullopt, {}, true);
  done = first_run.Click(kButtonDone, 0, {{kCheckInstallUpdates, true}}, Running());
  CHECK(!done.finish.check_for_updates && done.finish.install_updates);
  // A copy that can't install doesn't ask, and keeps what was chosen in
  // one that can.
  first_run = FirstRun(progress, std::nullopt, {.check_for_updates = true, .install_updates = true});
  done = first_run.Click(kButtonDone, 0, {{kCheckInstallUpdates, false}}, Running());
  CHECK(done.finish.check_for_updates && done.finish.install_updates);

  // Opened again, it shows them as they are.
  view = FirstRun({.done = true, .door = Door::kObsUser, .reached = Page::kDone}, std::nullopt,
                  {.check_for_updates = true, .install_updates = true}, true)
             .View(Running());
  CHECK(view.checks[1].checked && view.checks[2].checked && view.checks[2].enabled);
}

TEST(CableThatIsntACable) {
  core::Snapshot snapshot = Running();
  snapshot.obs_cable = {kHeadphones, kHeadphonesId};
  snapshot.outputs.push_back(snapshot.obs_cable);
  snapshot.chain->cable = kHeadphones;
  FirstRun first_run = ObsUser(snapshot);
  const PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kCable);
  CHECK(Contains(view.content, "OBS monitors your mic to Headphones, which isn't a virtual cable."));
  CHECK(Texts(view.choices) == (std::vector<std::string>{"CABLE Input", "CABLE In 16ch"}));
  CHECK(Texts(view.more_choices) == (std::vector<std::string>{"Speakers", "Headphones\nOBS's monitoring device"}));
  CHECK(view.default_choice == kChoiceFirst + 3);
  // Keeping it is the user's pick too: OBS monitoring elsewhere later
  // doesn't move it.
  const FirstRunAction kept = first_run.Click(kButtonNext, kChoiceFirst + 3, {}, snapshot);
  CHECK(kept.kind == Kind::kApply && kept.settings.cable == kHeadphonesId);
  snapshot.settings = kept.settings;
  CHECK(first_run.Page(snapshot) == Page::kDone);
  CHECK(first_run.View(snapshot).content.starts_with(
      "In your other apps, choose the recording side of Headphones as your mic."));

  // A cable picked before is asked about as any other, chosen.
  snapshot.settings = {.cable = kCable16Id, .cable_name = kCable16};
  CHECK(ObsUser(snapshot).Page(snapshot) == Page::kCable);
  CHECK(ObsUser(snapshot).View(snapshot).default_choice == kChoiceFirst + 1);
}

TEST(CableMissingAlwaysAsks) {
  core::Snapshot snapshot = Running();
  snapshot.state = State::kCableMissing;
  snapshot.outputs = {{kSpeakers, kSpeakersId}, {kCableInput, kCableInputId}};
  FirstRun first_run({.door = Door::kObsUser, .reached = Page::kDone});
  const PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kCable && view.content.starts_with("CABLE In 16ch isn't connected. Choose another cable."));
  CHECK(Texts(view.choices) == (std::vector<std::string>{"CABLE Input"}));
  const FirstRunAction picked = first_run.Click(kButtonNext, kChoiceFirst, {}, snapshot);
  CHECK(picked.kind == Kind::kApply && picked.settings.cable == kCableInputId);
  // OBS's own device is a pick of the user's too.
  snapshot.settings = picked.settings;
  snapshot.outputs.push_back({kCable16, kCable16Id});
  first_run = FirstRun({.door = Door::kObsUser, .reached = Page::kDone}, Page::kCable);
  const PageView again = first_run.View(snapshot);
  CHECK(again.choices.back().text == "CABLE In 16ch\nOBS's monitoring device");
  const FirstRunAction obs = first_run.Click(kButtonNext, kChoiceFirst + 1, {}, snapshot);
  CHECK(obs.kind == Kind::kApply && obs.settings.cable == kCable16Id && obs.settings.cable_name == kCable16);
}

TEST(TheDropdownShowsTheOtherDevices) {
  // Laid out closed, it's one row after the cables, saying which of its
  // devices is chosen, if one is; open, its devices follow it as radios.
  core::Snapshot snapshot = Running();
  snapshot.obs_cable = {kHeadphones, kHeadphonesId};
  snapshot.outputs.push_back(snapshot.obs_cable);
  const PageView view = ObsUser(snapshot).View(snapshot);
  const auto roles = [](const Scene& scene, Role role) {
    std::vector<std::wstring> names;
    for (const Element& element : scene.elements) {
      if (element.role == role) names.push_back(element.name);
    }
    return names;
  };
  Scene closed = LayoutScene(view, kChoiceFirst, false);
  CHECK((roles(closed, Role::kRadio) == std::vector<std::wstring>{L"CABLE Input", L"CABLE In 16ch"}));
  CHECK((roles(closed, Role::kDropdown) == std::vector<std::wstring>{L"Other playback devices"}));
  const auto dropdown = [](const Scene& scene) {
    return *std::find_if(scene.elements.begin(), scene.elements.end(),
                         [](const Element& element) { return element.role == Role::kDropdown; });
  };
  CHECK(dropdown(closed).id == kMoreChoicesId && !dropdown(closed).selected && dropdown(closed).help.empty());
  closed = LayoutScene(view, view.default_choice, false);
  CHECK(dropdown(closed).help == L"Headphones");
  const Scene open = LayoutScene(view, view.default_choice, true);
  CHECK((roles(open, Role::kRadio) ==
         std::vector<std::wstring>{L"CABLE Input", L"CABLE In 16ch", L"Speakers", L"Headphones"}));
  CHECK(dropdown(open).selected && dropdown(open).help.empty());
  // Its radios are in the same group, and the one chosen is selected.
  const auto headphones = std::find_if(open.elements.begin(), open.elements.end(),
                                       [](const Element& element) { return element.name == L"Headphones"; });
  CHECK(headphones->selected && headphones->parent == dropdown(open).parent);
}

TEST(SoundIntoACableIsNoted) {
  // Installing a cable can make it Windows' Output: the user hears nothing,
  // and the cable's apps hear the PC with the mic.
  core::Snapshot snapshot = Running();
  snapshot.default_output = kCableInputId;
  const std::string note = "Windows plays your PC's sound into CABLE Input, so you can't hear it, and your other apps "
                           "hear it with your mic. In Sound settings, set Output to your speakers or headphones.";
  for (const Page page : {Page::kCable, Page::kDone}) {
    const PageView view = FirstRun({.door = Door::kObsUser, .reached = Page::kDone}, page).View(snapshot);
    CHECK(view.page == page && view.notes.size() == 1 && view.notes[0].warning && view.notes[0].text == note);
    CHECK(Texts(view.buttons)[1] == "Open Sound settings");
  }
  snapshot.default_output = kSpeakersId;
  CHECK(FirstRun({.door = Door::kObsUser, .reached = Page::kDone}).View(snapshot).notes.empty());
}

TEST(WarningsShowWhatChangesExpectations) {
  core::Snapshot snapshot = Running();
  snapshot.notes = {{true, "Filter \"VST\" is a VST plugin."},
                    {false, "Filter \"Gate\" is off in OBS, and stays off."},
                    {false, "The mic is on push-to-talk in OBS.", true}};
  // After the cable.
  FirstRun first_run = ObsUser(snapshot);
  first_run.Click(kButtonNext, kChoiceFirst + 1, {}, snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kWarnings && view.instruction == "Before you start");
  CHECK((view.notes == std::vector<PageNote>{{true, "Filter \"VST\" is a VST plugin."},
                                             {false, "The mic is on push-to-talk in OBS."}}));
  CHECK(view.stages[static_cast<size_t>(Stage::kFilters)].state == StageState::kWarning);
  first_run.Click(kButtonNext, 0, {}, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kDone && first_run.progress().reached == Page::kDone);

  // File names are set as the pages set them.
  core::Snapshot named = snapshot;
  named.notes = {{.warning = true,
                  .text = "Filter \"VST <3\" is a VST plugin (Pro-Q 3.dll).",
                  .file = "Pro-Q 3.dll"}};
  const FirstRun past_cable({.door = Door::kObsUser, .reached = Page::kWarnings});
  CHECK(past_cable.View(named).notes[0].text == "Filter \"VST &lt;3\" is a VST plugin (<code>Pro-Q 3.dll</code>).");

  snapshot.notes.erase(snapshot.notes.begin());
  view = past_cable.View(snapshot);
  CHECK(view.page == Page::kWarnings && view.notes.size() == 1 && !view.notes[0].warning);
  snapshot.notes.pop_back();
  CHECK(past_cable.Page(snapshot) == Page::kDone);
}

TEST(ProblemsWithObsComeFirst) {
  core::Snapshot snapshot;
  CHECK(ObsUser(snapshot).Page(snapshot) == Page::kLooking && ObsUser(snapshot).View(snapshot).waiting);
  CHECK(ObsUser(snapshot).View(snapshot).content ==
        "Copying the parts of OBS needed to run, this may take a few seconds the first time.");

  snapshot.state = State::kObsMissing;
  snapshot.detail = "OBS Studio isn't installed.";
  FirstRun first_run = ObsUser(snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kFindObs && view.content.starts_with("OBS Studio isn't installed.\n\nIf OBS is installed"));
  CHECK(Contains(view.content, "<a href=\"https://obsproject.com/download\">get OBS Studio</a>"));
  CHECK(view.default_button == kButtonChooseObs);
  CHECK(first_run.Click(kButtonChooseObs, 0, {}, snapshot).kind == Kind::kChooseObs);
  CHECK(first_run.Click(kButtonTryAgain, 0, {}, snapshot).kind == Kind::kReimport);

  snapshot.state = State::kObsUnsupported;
  snapshot.obs = {"C:\\Program Files\\obs-studio", {34, 0, 0}};
  view = ObsUser(snapshot).View(snapshot);
  CHECK(view.page == Page::kObsUnsupported && Contains(view.content, "supports OBS 34.0.0, from <a "));

  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kObsSettings;
  snapshot.obs = {"C:\\Program Files\\obs-studio", {32, 2, 2}};
  first_run = ObsUser(snapshot);
  view = first_run.View(snapshot);
  CHECK(view.page == Page::kObsSettings);
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Try again", "Choose folder…", "Open OBS"}));
  CHECK(view.default_button == kButtonOpenObs);
  CHECK(first_run.Click(kButtonOpenObs, 0, {}, snapshot).kind == Kind::kOpenObs);
  CHECK(first_run.Click(kButtonChooseObsSettings, 0, {}, snapshot).kind == Kind::kChooseObsSettings);
  // Settings put there some other way, or a folder that's back.
  CHECK(first_run.Click(kButtonTryAgain, 0, {}, snapshot).kind == Kind::kReimport);
  snapshot.obs_running = true;
  view = first_run.View(snapshot);
  CHECK(!FindButton(view, kButtonOpenObs)->enabled && view.default_button == kButtonChooseObsSettings);

  snapshot = Running();
  snapshot.state = State::kRestartNeeded;
  first_run = ObsUser(snapshot);
  view = first_run.View(snapshot);
  CHECK(view.page == Page::kProblem && view.instruction == std::format("{} needs to restart", kDisplayName));
  CHECK(first_run.Click(kButtonRestart, 0, {}, snapshot).kind == Kind::kRestart);
  snapshot.state = State::kFailed;
  CHECK(ObsUser(snapshot).View(snapshot).instruction == "Stopped after an error");
  // Even a page that was answered waits for them.
  CHECK(FirstRun({.door = Door::kObsUser, .reached = Page::kDone}, Page::kCable).Page(snapshot) == Page::kProblem);
}

TEST(APickedFolderThatStoppedWorkingIsTheUsersToChange) {
  // OBS gone from the folder picked for it: pick another, or look for OBS
  // as with no pick. Nothing is dropped until the user says.
  core::Snapshot snapshot;
  snapshot.state = State::kObsMissing;
  snapshot.detail = "D:\\OBS isn't a complete OBS Studio install: bin\\64bit\\obs.dll is missing.";
  snapshot.settings.obs_dir = "D:\\OBS";
  FirstRun first_run = ObsUser(snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kFindObs && Contains(view.content, "OBS isn't in the folder chosen for it."));
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Try again", "Look for OBS", "Choose folder…"}));
  CHECK(view.default_button == kButtonChooseObs);
  FirstRunAction action = first_run.Click(kButtonLookForObs, 0, {}, snapshot);
  CHECK(action.kind == Kind::kApply && !action.settings.obs_dir);
  snapshot.settings.obs_dir.reset();
  view = ObsUser(snapshot).View(snapshot);
  CHECK(!FindButton(view, kButtonLookForObs));

  // The same for OBS's settings: pick another folder, or use OBS's own.
  // Opening OBS wouldn't fill a folder elsewhere, so it isn't offered.
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kObsSettings;
  snapshot.obs = {"C:\\Program Files\\obs-studio", {32, 2, 2}};
  snapshot.detail = "OBS has no settings in E:\\Portable\\config\\obs-studio.";
  snapshot.settings.obs_config = "E:\\Portable\\config";
  snapshot.obs_writes_config = false;
  first_run = ObsUser(snapshot);
  view = first_run.View(snapshot);
  CHECK(view.page == Page::kObsSettings && Contains(view.content, "aren't in the folder chosen for them."));
  CHECK(Texts(view.buttons) ==
        (std::vector<std::string>{"Back", "Try again", "Use OBS's own folder", "Choose folder…"}));
  CHECK(view.default_button == kButtonChooseObsSettings && view.footer.empty());
  // The same folder again, such as on a drive plugged back in.
  CHECK(first_run.Click(kButtonTryAgain, 0, {}, snapshot).kind == Kind::kReimport);
  action = first_run.Click(kButtonOwnObsSettings, 0, {}, snapshot);
  CHECK(action.kind == Kind::kApply && !action.settings.obs_config);
  // OBS's own folder picked, however it's spelled (the core says so): as
  // with no pick, where opening OBS once does fill it.
  snapshot.settings.obs_config = "c:\\users\\YOU\\appdata\\roaming\\";
  snapshot.obs_writes_config = true;
  view = ObsUser(snapshot).View(snapshot);
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Try again", "Choose folder…", "Open OBS"}));
  CHECK(view.default_button == kButtonOpenObs && Contains(view.content, "If your OBS keeps its settings"));
  snapshot.settings.obs_config.reset();
  view = ObsUser(snapshot).View(snapshot);
  CHECK(!FindButton(view, kButtonOwnObsSettings) && FindButton(view, kButtonOpenObs));
}

TEST(AnUnsupportedObsOffersAnotherFolder) {
  // OBS updated past what knobs supports: install a supported one, choose
  // one installed elsewhere, or wait for knobs.
  core::Snapshot snapshot;
  snapshot.state = State::kObsUnsupported;
  snapshot.obs = {"C:\\Program Files\\obs-studio", {34, 0, 0}};
  snapshot.detail = "OBS 34.0.0 isn't supported.";
  FirstRun first_run = ObsUser(snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kObsUnsupported);
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Try again", "Choose folder…"}));
  CHECK(view.default_button == kButtonTryAgain);
  CHECK(Contains(view.content, "If a supported OBS is installed somewhere else, choose that folder."));
  CHECK(view.content.ends_with(std::format("Or wait for a {0} update that supports OBS 34.0.0, from <a "
                                           "href=\"https://github.com/danielalyoshin/knobs/releases/latest\">{0}'s "
                                           "latest release</a>.",
                                           kDisplayName)));
  CHECK(first_run.Click(kButtonChooseObs, 0, {}, snapshot).kind == Kind::kChooseObs);
  CHECK(first_run.Click(kButtonTryAgain, 0, {}, snapshot).kind == Kind::kReimport);

  // A folder picked for OBS that holds an older one stays picked, with the
  // ways back. No knobs update supports an older OBS.
  snapshot.settings.obs_dir = "D:\\OBS 31";
  snapshot.obs = {"D:\\OBS 31", {31, 1, 4}};
  snapshot.detail = "OBS 31.1.4 isn't supported.";
  first_run = ObsUser(snapshot);
  view = first_run.View(snapshot);
  CHECK(view.page == Page::kObsUnsupported && Contains(view.content, "the folder chosen for it, D:\\OBS 31."));
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Try again", "Look for OBS", "Choose folder…"}));
  CHECK(view.default_button == kButtonChooseObs && !Contains(view.content, "wait"));
  const FirstRunAction action = first_run.Click(kButtonLookForObs, 0, {}, snapshot);
  CHECK(action.kind == Kind::kApply && !action.settings.obs_dir);
  CHECK(first_run.Click(kButtonChooseObs, 0, {}, snapshot).kind == Kind::kChooseObs);
}

// --- The second door ----------------------------------------------------------------

TEST(SecondDoorListsTheStepsInOBSWords) {
  core::Snapshot snapshot = Running();
  snapshot.chain->filters.clear();
  snapshot.mics[0].filters.clear();
  FirstRun first_run({});
  first_run.Click(kButtonNewToObs, 0, {}, snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kSteps && view.instruction == "Set up your mic in OBS");
  // As OBS 32.2.2 names them (docs/design.md, First run).
  CHECK((view.steps == std::vector<std::string>{
             "Open OBS.", "In the Audio Mixer, click <b>Mic/Aux</b> and choose <b>Filters</b>.",
             "Under Audio Filters, click <b>+</b> and add filters as needed.",
             "Click each filter and adjust it by ear, starting from OBS's defaults.",
             std::format("Close OBS. {} then reads your filters and carries on here.", kDisplayName)}));
  CHECK(view.walkthrough && view.walkthrough->name == "obs-filters");
  CHECK(Contains(view.footer, std::format("<a href=\"{}\">", kSetupGuideUrl)));
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Open OBS", "Next"}));
  CHECK(FindButton(view, kButtonOpenObs)->enabled && !FindButton(view, kButtonNext)->enabled);
  CHECK(view.default_button == kButtonOpenObs);
  CHECK(first_run.Click(kButtonOpenObs, 0, {}, snapshot).kind == Kind::kOpenObs);

  // OBS open: knobs waits for it to close.
  core::Snapshot open = snapshot;
  open.state = State::kPausedForObs;
  open.obs_running = true;
  view = first_run.View(open);
  CHECK(!FindButton(view, kButtonOpenObs)->enabled && view.waiting && view.status.starts_with("OBS is open."));
  // Closed with no filters added: still here.
  CHECK(first_run.Changed(open, snapshot).kind == Kind::kNone && first_run.Page(snapshot) == Page::kSteps);
  // Closed with filters: on to the first door's pages.
  core::Snapshot tuned = Running();
  first_run.Changed(open, tuned);
  CHECK(first_run.progress().door == Door::kObsUser && first_run.Page(tuned) == Page::kCable);
  first_run.Click(kButtonBack, 0, {}, tuned);
  CHECK(first_run.Page(tuned) == Page::kSteps);
  // Filters there already, and OBS closed: Next goes on.
  view = first_run.View(tuned);
  CHECK(FindButton(view, kButtonNext)->enabled);
  first_run.Click(kButtonNext, 0, {}, tuned);
  CHECK(first_run.Page(tuned) == Page::kCable);
  first_run.Click(kButtonBack, 0, {}, tuned);
  first_run.Click(kButtonBack, 0, {}, tuned);
  CHECK(first_run.Page(tuned) == Page::kWelcome);
}

TEST(SecondDoorWithoutOBSOrAMic) {
  core::Snapshot missing;
  missing.state = State::kObsMissing;
  PageView view = FirstRun({}, Page::kSteps).View(missing);
  CHECK(view.steps[0] == "Install OBS Studio 32.2 to 33.x from <a href=\"https://obsproject.com/download\">"
                         "obsproject.com</a>, and open it.");
  CHECK(view.steps[1] == "In <b>Settings › Audio</b>, set <b>Mic/Auxiliary Audio</b> to your mic. Then, in the "
                         "Audio Mixer, click <b>Mic/Aux</b> and choose <b>Filters</b>.");
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Next"}));

  core::Snapshot unsupported = missing;
  unsupported.state = State::kObsUnsupported;
  unsupported.obs = {"C:\\Program Files\\obs-studio", {34, 0, 0}};
  view = FirstRun({}, Page::kSteps).View(unsupported);
  CHECK(Contains(view.steps[0], "OBS's releases on GitHub</a>, and open it."));
  CHECK(Contains(view.steps[0], " doesn't support OBS 34.0.0 yet."));
  unsupported.obs.version = {31, 1, 4};
  view = FirstRun({}, Page::kSteps).View(unsupported);
  CHECK(Contains(view.steps[0], std::format("and open it. OBS 31.1.4 is too old for {}.", kDisplayName)));
  CHECK(!Contains(view.steps[0], "yet"));

  // Several mics, none picked: whichever one they mean.
  CHECK(FirstRun({}, Page::kSteps).View(PickMic()).steps[1] ==
        "In the Audio Mixer, click your mic and choose <b>Filters</b>.");
}

// --- The band ----------------------------------------------------------------------

std::string Band(const core::Snapshot& snapshot) {
  std::string band;
  for (const StageView& stage : StageViews(snapshot)) {
    constexpr const char* kStates[] = {"?", "known", "set", "warning", "blocked"};
    band += std::format("{}[{}: {}]", band.empty() ? "" : " ", kStates[static_cast<int>(stage.state)], stage.value);
  }
  return band;
}

TEST(TheBandFollowsTheMicsPath) {
  CHECK(Band(Running()) ==
        "[set: Mic/Aux\nMicrophone (Audient iD4)] [set: 4 filters] [set: CABLE In 16ch] "
        "[known: Choose <b>CABLE Output</b>]");
  CHECK(Band(PickMic()) == "[known: 3 mics in OBS] [?: Not read yet] [set: CABLE In 16ch] [known: Choose <b>CABLE Output</b>]");
  core::Snapshot looking;
  CHECK(Band(looking) == "[?: Not read yet] [?: Looking for OBS] [?: Not read yet] [?: Use the cable as their mic]");
  core::Snapshot missing;
  missing.state = State::kObsMissing;
  CHECK(Contains(Band(missing), "[blocked: OBS not found]"));
  missing.state = State::kObsUnsupported;
  missing.obs = {"C:\\Program Files\\obs-studio", {34, 0, 0}};
  CHECK(Contains(Band(missing), "[blocked: Can't run OBS 34.0.0]"));

  core::Snapshot snapshot = Running();
  snapshot.chain->filters.clear();
  snapshot.mics[0].filters.clear();
  CHECK(Contains(Band(snapshot), "[?: No filters yet]"));
  snapshot = Running();
  snapshot.notes = {{true, "Filter \"VST\" is a VST plugin."}};
  CHECK(Contains(Band(snapshot), "[warning: 4 filters]"));
  snapshot = Running();
  snapshot.state = State::kMicMissing;
  snapshot.inputs.clear();
  CHECK(Band(snapshot).starts_with("[blocked: Mic/Aux\nNot connected]"));

  // The cable: none, one not chosen, one gone, and a device that isn't one.
  snapshot = Running();
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kCable;
  snapshot.obs_cable = {"", "default"};
  CHECK(Contains(Band(snapshot), "[?: Not chosen yet] [?: Use the cable as their mic]"));
  snapshot.outputs = {{kSpeakers, kSpeakersId}};
  CHECK(Contains(Band(snapshot), "[?: No cable yet]"));
  snapshot = Running();
  snapshot.state = State::kCableMissing;
  snapshot.outputs = {{kSpeakers, kSpeakersId}};
  CHECK(Contains(Band(snapshot), "[blocked: CABLE In 16ch\nNot connected] [?: Use the cable as their mic]"));
  snapshot = Running();
  snapshot.obs_cable = {kHeadphones, kHeadphonesId};
  snapshot.outputs.push_back(snapshot.obs_cable);
  CHECK(Contains(Band(snapshot), "[known: Headphones\nNot a virtual cable]"));

  // Each page points at the stage it's about.
  CHECK(FirstRun({}, Page::kWelcome).View(Running()).stage == Stage::kFilters);
  CHECK(FirstRun({}, Page::kCable).View(Running()).stage == Stage::kCable);
  CHECK(!ObsUser(looking).View(looking).stage);
}

TEST(EveryButtonHasItsOwnAccessKey) {
  // Each button, door and check box on a page has an access key, and no two
  // share one.
  const auto keys_ok = [](const PageView& view) {
    std::vector<char> keys;
    std::vector<std::string> texts;
    for (const PageButton& button : view.buttons) texts.push_back(button.text);
    for (const PageCheck& check : view.checks) texts.push_back(check.text);
    for (const std::string& text : texts) {
      const size_t at = text.find('&');
      if (at == std::string::npos || at + 1 >= text.size()) return false;
      const char key = static_cast<char>(std::tolower(static_cast<unsigned char>(text[at + 1])));
      if (std::find(keys.begin(), keys.end(), key) != keys.end()) return false;
      keys.push_back(key);
    }
    return true;
  };
  core::Snapshot missing;
  missing.state = State::kObsMissing;
  missing.settings.obs_dir = "D:\\OBS";
  core::Snapshot settings;
  settings.state = State::kNeedsSetup;
  settings.setup = SetupNeed::kObsSettings;
  settings.obs = {"C:\\Program Files\\obs-studio", {32, 2, 2}};
  settings.settings.obs_config = "E:\\Portable\\config";
  core::Snapshot no_filters = Running();
  no_filters.chain->filters.clear();
  no_filters.mics[0].filters.clear();
  core::Snapshot keep = Running();
  keep.obs_cable = {kHeadphones, kHeadphonesId};
  keep.outputs = {{kSpeakers, kSpeakersId}, keep.obs_cable};
  core::Snapshot restart = Running();
  restart.state = State::kRestartNeeded;
  for (const Page page : {Page::kWelcome, Page::kSteps, Page::kLooking, Page::kMic, Page::kCable, Page::kWarnings,
                          Page::kDone}) {
    CHECK(keys_ok(FirstRun({}, page).View(PickMic())));
  }
  for (const core::Snapshot& snapshot : {missing, settings, no_filters, keep, restart}) {
    CHECK(keys_ok(ObsUser(snapshot).View(snapshot)));
  }
  // The page that notes the PC's sound going into the cable.
  core::Snapshot sound = Running();
  sound.default_output = kCableInputId;
  for (const Page page : {Page::kCable, Page::kDone}) CHECK(keys_ok(FirstRun({}, page, {}, true).View(sound)));
  // A choice's name keeps its "&"s: a choice has no access key.
  keep.obs_cable = {"Speakers & Headphones", kHeadphonesId};
  keep.outputs = {{kSpeakers, kSpeakersId}, keep.obs_cable};
  CHECK(ObsUser(keep).View(keep).more_choices.back().text == "Speakers & Headphones\nOBS's monitoring device");
}

TEST(RichTextReadsThePagesMarkup) {
  RichText rich = ParseRichText("Click <b>Install Driver</b>.");
  CHECK(rich.text == L"Click Install Driver." &&
        (rich.spans == std::vector<TextSpan>{{TextSpan::Kind::kBold, 6, 14, ""}}));
  rich = ParseRichText("Get <a href=\"https://vb-audio.com/Cable/\"><code>vb-audio.com</code></a> now.");
  CHECK(rich.text == L"Get vb-audio.com now.");
  CHECK((rich.spans == std::vector<TextSpan>{{TextSpan::Kind::kLink, 4, 12, "https://vb-audio.com/Cable/"},
                                            {TextSpan::Kind::kCode, 4, 12, ""}}));
  // Only the markup's own tags: a name from OBS stays as it is.
  CHECK(ParseRichText("Mic <3 <i>EQ</i>").text == L"Mic <3 <i>EQ</i>");
  CHECK(ParseRichText("Mic <3 <i>EQ</i>").spans.empty());
  // An unclosed tag runs to the end.
  rich = ParseRichText("<b>Settings › Sound");
  CHECK(rich.text == L"Settings › Sound" && rich.spans.size() == 1 && rich.spans[0].length == 16);
  CHECK(PlainText("In <b>Settings</b>, <a href=\"x\">this</a>.") == "In Settings, this.");
  // A name from OBS stays text, whatever it holds.
  const std::string name = "<a href=\"ms-settings:\">Mic</a> & <b>";
  rich = ParseRichText("Found " + EscapeMarkup(name) + ".");
  CHECK(rich.spans.empty() && rich.text == L"Found <a href=\"ms-settings:\">Mic</a> & <b>.");
}

// --- Resuming, Back and pages asked for -----------------------------------------

TEST(ResumesWhereItStopped) {
  core::Snapshot snapshot = Running();
  snapshot.mics = PickMic().mics;
  snapshot.picked_mic = 1;
  snapshot.settings.mic = "Mic/Aux";
  snapshot.notes = {{false, "The mic is muted in OBS.", true}};
  // The mic was answered; the warnings weren't.
  CHECK(FirstRun({.door = Door::kObsUser, .reached = Page::kWarnings}).Page(snapshot) == Page::kWarnings);
  CHECK(FirstRun({.door = Door::kObsUser, .reached = Page::kMic}).Page(snapshot) == Page::kMic);
  CHECK(FirstRun({.door = Door::kNewToObs}).Page(snapshot) == Page::kSteps);
  // An answered page the state calls for again.
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kPickMic;
  CHECK(FirstRun({.door = Door::kObsUser, .reached = Page::kDone}).Page(snapshot) == Page::kMic);
  // From the start.
  CHECK(FirstRun({.done = true}).Page(snapshot) == Page::kWelcome);
}

TEST(BackRetracesThePages) {
  core::Snapshot snapshot = PickMic();
  snapshot.obs_cable = {"", "default"};
  FirstRun first_run = ObsUser(snapshot);
  CHECK(first_run.Page(snapshot) == Page::kMic);
  first_run.Click(kButtonNext, kChoiceFirst + 1, {}, snapshot);
  // The core picks Mic/Aux, and needs a cable.
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kCable;
  snapshot.picked_mic = 1;
  snapshot.chain = core::ChainSummary{"Mic/Aux", kFourFilters, ""};
  snapshot.settings.mic = "Mic/Aux";
  CHECK(first_run.Page(snapshot) == Page::kCable);
  first_run.Click(kButtonBack, 0, {}, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kMic && first_run.View(snapshot).default_choice == kChoiceFirst + 1);
  first_run.Click(kButtonBack, 0, {}, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kWelcome && first_run.progress().door == Door::kNone);
  // With nothing to go back to, Back is the welcome page.
  first_run = FirstRun({.door = Door::kObsUser, .reached = Page::kCable});
  first_run.Click(kButtonBack, 0, {}, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kWelcome);
}

TEST(AnyPageCanBeAskedFor) {
  for (const Page page : {Page::kWelcome, Page::kSteps, Page::kLooking, Page::kFindObs, Page::kObsUnsupported,
                          Page::kObsSettings, Page::kProblem, Page::kMic, Page::kFilters, Page::kCable,
                          Page::kWarnings, Page::kDone}) {
    CHECK(PageNamed(PageName(page)) == page);
  }
  CHECK(!PageNamed("elsewhere"));
  // A page the state doesn't call for is still shown when asked for.
  FirstRun first_run({.done = true, .door = Door::kObsUser, .reached = Page::kDone}, Page::kCable);
  CHECK(first_run.Page(Running()) == Page::kCable);
  CHECK(first_run.View(Running()).default_choice == kChoiceFirst + 1);
  first_run.Click(kButtonNext, kChoiceFirst + 1, {}, Running());
  CHECK(first_run.Page(Running()) == Page::kDone);
  // A menu's fix opens the page for what's needed, even before the doors.
  core::Snapshot needs = Running();
  needs.state = State::kCableMissing;
  CHECK(PageFor(needs) == Page::kCable && FirstRun({}, PageFor(needs)).Page(needs) == Page::kCable);
  needs = PickMic();
  CHECK(PageFor(needs) == Page::kMic && FirstRun({.door = Door::kNewToObs}, PageFor(needs)).Page(needs) == Page::kMic);
  needs.state = State::kObsUnsupported;
  CHECK(PageFor(needs) == Page::kObsUnsupported);
  CHECK(!PageFor(Running()));
  CHECK(FirstRun({}, Page::kMic).Page(Running()) == Page::kMic);
  // One that doesn't apply gives way, to the cable until it's answered.
  CHECK(FirstRun({}, Page::kWarnings).Page(Running()) == Page::kCable);
  const FirstRunProgress answered{.door = Door::kObsUser, .reached = Page::kDone};
  CHECK(FirstRun(answered, Page::kWarnings).Page(Running()) == Page::kDone);
  CHECK(FirstRun(answered, Page::kFindObs).Page(Running()) == Page::kDone);
}

}  // namespace
