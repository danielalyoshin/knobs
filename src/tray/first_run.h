// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/state.h"

// The first run as data (docs/design.md, First run): which page to show for a
// snapshot of the core, what the page says and offers, and what each button
// does. setup_window.cpp shows the pages in a window of its own. Text is
// UTF-8. Content, steps, notes, footers and stage values may hold the markup
// rich_text.h reads: <a href="…">links</a>, <b>names in a UI</b> and
// <code>file names</code>.
namespace knobs::tray {

// In the order the first door goes through them.
enum class FirstRunPage {
  kWelcome,  // The two doors.
  kSteps,    // The second door: setting up a mic in OBS.
  // The first door. These five come up whenever the core's state calls for
  // them, and block the rest.
  kLooking,         // Starting: finding OBS and reading its settings.
  kFindObs,         // No OBS install found.
  kObsUnsupported,  // The install's version isn't supported.
  kObsSettings,     // OBS's settings can't be read.
  kProblem,         // Failed, or a restart is needed.
  // Then, each only when it's needed, or until it's answered.
  kMic,       // Which mic, when OBS has several. Needed while none is picked.
  kFilters,   // The mic has no filters. Needed while OBS has no mic.
  kCable,     // Which cable, when OBS doesn't monitor to one. Needed while
              // none is chosen or the chosen one isn't connected.
  kWarnings,  // Pre-flight warnings, and notes that change what to expect.
  kDone,      // The chain, where to find knobs, and Start with Windows.
};

// "welcome", "cable", ...: for the settings file and the dev tools.
std::string_view PageName(FirstRunPage page);
std::optional<FirstRunPage> PageNamed(std::string_view name);

enum class Door {
  kNone,      // Not chosen yet: the welcome page.
  kObsUser,   // "I set up my mic in OBS".
  kNewToObs,  // "I'm new to OBS".
};

// How far a first run got, saved so that an unfinished one resumes there.
struct FirstRunProgress {
  // Finished once. knobs no longer opens it by itself.
  bool done = false;
  Door door = Door::kNone;
  // The first door's pages from kMic up to this one are answered, and show
  // again only if they're needed.
  FirstRunPage reached = FirstRunPage::kMic;

  friend bool operator==(const FirstRunProgress&, const FirstRunProgress&) = default;
};

// Button and choice IDs. Choices (radio buttons) are numbered from
// kChoiceFirst, in the order the page lists them.
enum FirstRunId : int {
  kButtonObsUser = 100,  // The doors, as command links.
  kButtonNewToObs,
  kButtonBack,
  kButtonNext,
  kButtonDone,
  kButtonOpenObs,
  kButtonChooseObs,          // OBS's install folder.
  kButtonChooseObsSettings,  // OBS's settings folder.
  kButtonTryAgain,
  kButtonRestart,
  kButtonShowSteps,      // To the second door.
  kButtonSkipFilters,    // Go on with a mic that has no filters.
  kButtonKeepDevice,     // Go on with a playback device that isn't a cable.
  kButtonLookForObs,     // Drop the picked install folder and look for OBS.
  kButtonOwnObsSettings,  // Drop the picked settings folder for OBS's own.
  kChoiceFirst = 200,
  // The last page's check boxes.
  kCheckStartWithWindows = 300,
  kCheckForUpdates,
  kCheckInstallUpdates,
};

// The last page's check boxes: Start with Windows, then the update check
// (docs/design.md, Updates).
struct FinishChoices {
  bool start_with_windows = false;
  bool check_for_updates = false;
  // Updating by itself, which needs the check.
  bool install_updates = false;

  friend bool operator==(const FinishChoices&, const FinishChoices&) = default;
};

// The check boxes as the user has them, by ID. One left out is as the page
// has it.
using CheckStates = std::map<int, bool>;

// The stages of the mic's path, in the band across the top of every page:
// the mic, OBS's filters, the virtual cable and the other apps.
enum class Stage { kMic, kFilters, kCable, kApps };
inline constexpr size_t kStageCount = 4;

enum class StageState {
  kUnknown,  // Not known yet.
  kKnown,    // Known, with nothing to mark.
  kSet,      // In place.
  kWarning,  // In place, with a warning that changes what to expect.
  kBlocked,  // Stops knobs, or needs the user before it can run.
};

struct StageView {
  StageState state = StageState::kUnknown;
  std::string value;  // What knobs knows of it: a line or two, "\n" between.

  friend bool operator==(const StageView&, const StageView&) = default;
};

// The band: each stage as the snapshot has it.
std::array<StageView, kStageCount> StageViews(const core::Snapshot& snapshot);

struct PageButton {
  int id = 0;
  // A command link's and a choice's may be "Text\nNote". A button's and a
  // command link's mark their access key with "&", as Windows does: "&Back"
  // is Alt+B ("&&" for an "&").
  std::string text;
  bool enabled = true;

  friend bool operator==(const PageButton&, const PageButton&) = default;
};

// The walkthrough beside a page's steps (walkthrough.h).
struct PageWalkthrough {
  std::string name;  // "obs-filters", "vb-cable-setup".
  std::string alt;   // What it shows, for screen readers.

  friend bool operator==(const PageWalkthrough&, const PageWalkthrough&) = default;
};

// A check box under the page's content.
struct PageCheck {
  int id = 0;
  // Its access key marked as a button's.
  std::string text;
  bool checked = false;
  bool enabled = true;
  // Under the one before it, which it needs: off and disabled while that
  // one is off.
  bool needs_previous = false;

  friend bool operator==(const PageCheck&, const PageCheck&) = default;
};

// A note on the warnings page.
struct PageNote {
  bool warning = false;  // Else information.
  std::string text;

  friend bool operator==(const PageNote&, const PageNote&) = default;
};

struct PageView {
  FirstRunPage page = FirstRunPage::kWelcome;
  // The stage the page is about, which the band points at.
  std::optional<Stage> stage;
  std::array<StageView, kStageCount> stages;
  std::string instruction;  // The heading.
  // Paragraphs, "\n\n" between.
  std::string content;
  // Numbered steps after the content, and the walkthrough beside them.
  std::vector<std::string> steps;
  std::optional<PageWalkthrough> walkthrough;
  std::vector<PageNote> notes;
  // The chain as it will run, set out on the last page.
  std::optional<core::ChainSummary> chain;
  // The page's note, beside its buttons.
  std::string footer;
  // The buttons are command links (the doors), not push buttons.
  bool command_links = false;
  std::vector<PageButton> buttons;
  int default_button = 0;
  std::vector<PageButton> choices;
  int default_choice = 0;
  std::vector<PageCheck> checks;
  // The page waits for something and moves on by itself, and `status` says
  // what for, beside the buttons.
  bool waiting = false;
  std::string status;

  friend bool operator==(const PageView&, const PageView&) = default;
};

// What a click or a change asks of the dialog, besides showing the page
// that's next.
struct FirstRunAction {
  enum class Kind {
    kNone,
    kApply,            // Apply `settings`, and wait for the core to take them.
    kReimport,         // Try again.
    kOpenObs,          // Start obs64.exe.
    kChooseObs,        // Ask for OBS's install folder (Settings::obs_dir).
    kChooseObsSettings,  // Ask for OBS's settings folder (Settings::obs_config).
    kRestart,          // Restart knobs.
    kFinish,           // Done: close, and set Start with Windows.
  };
  Kind kind = Kind::kNone;
  core::Settings settings;  // kApply.
  FinishChoices finish;     // kFinish.
};

// One first run: the progress, and the pages visited, for Back. Snapshots
// passed in carry the settings they were worked out with
// (core::Snapshot::settings); changes are made to those.
class FirstRun {
 public:
  // Starts from `progress`, at `page` if given (the dev tools open any page
  // with it). A page that doesn't apply to the state shows the page that
  // does instead. `now` is how the last page's choices are now. It offers
  // Start with Windows checked on a first run, and as it is when one that
  // was finished is opened again, so that Done changes nothing the user
  // didn't, and the update check as it is: off until the user turns it on.
  // Updating by itself is only offered if `can_install_updates`: the
  // installer installed this copy.
  explicit FirstRun(FirstRunProgress progress, std::optional<FirstRunPage> page = std::nullopt,
                    FinishChoices now = {}, bool can_install_updates = false);

  const FirstRunProgress& progress() const { return progress_; }

  FirstRunPage Page(const core::Snapshot& snapshot) const;
  // The page for `snapshot`, with `choice` selected if the page offers it,
  // and its check boxes as `checks` has them; 0 and none mean the page's
  // defaults.
  PageView View(const core::Snapshot& snapshot, int choice = 0, const CheckStates& checks = {}) const;

  // A button was clicked on the page View showed for `snapshot`.
  FirstRunAction Click(int button, int choice, const CheckStates& checks, const core::Snapshot& snapshot);
  // The core moved on from `before` to `after`: OBS closing on the second
  // door, or a cable appearing on the cable page, moves the first run on.
  FirstRunAction Changed(const core::Snapshot& before, const core::Snapshot& after);

 private:
  void Leave(FirstRunPage page);
  void Back(const core::Snapshot& snapshot);

  FirstRunProgress progress_;
  // The last page's check boxes, as it offers them.
  FinishChoices finish_;
  bool can_install_updates_ = false;
  // A page shown out of turn: one gone Back to, or asked for.
  std::optional<FirstRunPage> focus_;
  std::vector<FirstRunPage> history_;
};

// A button's or check box's text without its access key's "&".
std::string WithoutAccessKey(std::string_view text);

// Whether any of the collection's mics has a filter that's on.
bool AnyFilters(const core::Snapshot& snapshot);

// The playback devices the cable page offers: virtual cables, then the
// device knobs sends to if it's connected and not a cable.
std::vector<audio::AudioDevice> CableChoices(const core::Snapshot& snapshot);

}  // namespace knobs::tray
