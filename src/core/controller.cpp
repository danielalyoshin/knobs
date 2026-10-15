// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/controller.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "app_info.h"

namespace knobs::core {
namespace {

using audio::FindById;
using audio::kDefaultDevice;
using audio::SameId;

std::string Seconds(Controller::Clock::duration duration) {
  return std::format("{:.3g} s", std::chrono::duration<double>(duration).count());
}

std::string Minutes(Controller::Clock::duration duration) {
  if (duration < std::chrono::minutes(2)) return Seconds(duration);
  return std::format("{:.0f} min", std::chrono::duration<double, std::ratio<60>>(duration).count());
}

// When the chain's output counts as silent, for restarting the monitor
// (Timing::monitor_restart_every), by its typical level (ChainLevel). At or
// below -50 dBFS, whatever the mic: a gate or an expander gets there between
// words.
constexpr float kSilentPeak = 0.0031623f;  // -50 dBFS
// A mic without one never does, so within 6 dB of its noise floor counts
// too: nobody is talking...
constexpr float kFloorMargin = 1.9953f;  // +6 dB
// ...unless the floor is that loud. A mic that never gets down to -30 dBFS
// isn't restarted, and its delay grows until the monitor's buffer overflows,
// which restarts it too (docs/design.md, Keeping the delay down).
constexpr float kQuietCeiling = 0.031623f;  // -30 dBFS

std::string Dbfs(float peak) {
  return peak > 0 ? std::format("{:.0f} dBFS", 20 * std::log10(peak)) : std::string("-inf dBFS");
}

}  // namespace

Controller::Controller(Backend& backend, Settings settings, Publish publish, Timing timing)
    : backend_(backend), settings_(std::move(settings)), publish_(std::move(publish)), timing_(std::move(timing)) {
  if (timing_.retry_delays.empty()) timing_.retry_delays.push_back(timing_.stall_timeout);
  snapshot_.settings = settings_;
}

void Controller::Start(bool obs_running, Clock::time_point now, bool paused, bool wait_for_nvidia,
                       std::vector<std::filesystem::path> wait_folders) {
  obs_running_ = obs_running;
  paused_by_user_ = paused;
  wait_for_nvidia_ = wait_for_nvidia;
  snapshot_.obs_running = obs_running;
  snapshot_.paused_by_user = paused;
  publish_(snapshot_);
  devices_ = backend_.ListDevices();
  if (!wait_folders.empty()) WaitForFiles(std::move(wait_folders), wait_for_nvidia, now);
  Refresh(now);
  Reconcile(now);
}

void Controller::SetObsRunning(bool running, Clock::time_point now) {
  if (stopped_ || running == obs_running_) return;
  obs_running_ = running;
  if (running) {
    backend_.Log("OBS started.");
  } else {
    // OBS saves its settings as it exits, so they're complete by now.
    backend_.Log("OBS exited. Importing its settings again.");
    ClearFailures();
    Refresh(now);
  }
  Reconcile(now);
}

void Controller::SetOtherObs(std::vector<OtherObs> others, Clock::time_point now) {
  if (stopped_ || others == other_obs_) return;
  other_obs_ = std::move(others);
  backend_.Log(other_obs_.empty() ? std::string("OBS isn't running in another Windows session any more.")
                                  : std::format("OBS is running in {} other Windows session(s).", other_obs_.size()));
  Reconcile(now);
}

void Controller::DevicesChanged(Clock::time_point now) {
  if (stopped_) return;
  if (!devices_first_) devices_first_ = now;
  devices_due_ = std::min(now + timing_.device_settle, *devices_first_ + timing_.device_settle_max);
}

void Controller::PollDevices(Clock::time_point now) {
  if (stopped_) return;
  audio::Endpoints devices = backend_.ListDevices();
  if (devices == devices_) return;
  devices_ = std::move(devices);
  backend_.Log("Audio devices changed.");
  chain_error_.reset();
  retry_at_.reset();
  Reconcile(now);
}

void Controller::Pause(Clock::time_point now) {
  if (stopped_) return;
  paused_by_user_ = true;
  Reconcile(now);
}

void Controller::Resume(Clock::time_point now) {
  if (stopped_) return;
  paused_by_user_ = false;
  CheckFileUpdates(now);
  ClearFailures();
  Reconcile(now);
}

void Controller::Reimport(Clock::time_point now) {
  if (stopped_) return;
  ClearFailures();
  // Devices too: without Windows' notifications, as when they couldn't be
  // listened for, this is how a device that came back is seen.
  devices_ = backend_.ListDevices();
  Refresh(now);
  Reconcile(now);
}

void Controller::Apply(const Settings& settings, Clock::time_point now) {
  if (stopped_) return;
  // Only these are imported. Importing again for the cable or pausing would
  // read OBS's settings, maybe while it's open, and a change made there would
  // come with the new settings as if they had made it.
  const bool reimport = settings.obs_dir != settings_.obs_dir || settings.obs_config != settings_.obs_config ||
                        settings.mic != settings_.mic;
  settings_ = settings;
  ClearFailures();
  if (reimport) {
    Refresh(now);
  } else if (import_ && import_->mic) {
    PlanChain();
  }
  Reconcile(now);
}

Status Controller::Restart(const RestartHook& start, Clock::time_point now) {
  ReleaseIfDue(now);
  const RestartFiles files = backend_.UpdateFileHandles();
  const Status started = start(files);
  // Successful duplicates keep their prepared breaks through our exit.
  // Failure lets the installer through even when it still finds DLLs loaded.
  backend_.ReleaseUpdateFiles();
  if (file_restart_ != FileRestart::kNone) {
    file_restart_ = started ? FileRestart::kHandedOff : FileRestart::kReleased;
  }
  if (started) stopped_ = true;
  else Reconcile(now);
  return started;
}

void Controller::PollFiles(Clock::time_point now) {
  if (stopped_) return;
  const FileRestart before = file_restart_;
  const bool waiting_before = !updating_folders_.empty();
  const bool installed_before = nvidia_installed_;
  bool changed = ReleaseIfDue(now);
  if (file_restart_ == FileRestart::kNone) backend_.PollFileWatches();
  CheckFileUpdates(now);
  // WaitForFiles already took the first fingerprint. Never scan twice on
  // the tick which first notices a writer.
  if (waiting_before && !updating_folders_.empty() && file_restart_ == FileRestart::kNone) {
    const std::string fingerprint = backend_.FingerprintFolders(updating_folders_);
    if (fingerprint != folder_fingerprint_) {
      folder_fingerprint_ = fingerprint;
      files_quiet_since_ = now;
    }
    const bool settled = now - files_quiet_since_ >= timing_.file_settle;
    if (settled || now - file_wait_since_ >= timing_.file_wait_max) {
      updating_folders_.clear();
      wait_for_nvidia_ = false;
      backend_.Log(settled ? std::string("The updated files stopped changing. Importing OBS's settings again.")
                           : std::format("The updated files kept changing for {}. Importing OBS's settings again anyway.",
                                         Minutes(now - file_wait_since_)));
      ClearFailures();
      devices_ = backend_.ListDevices();
      Refresh(now);
      changed = true;
    }
  }
  if (import_pending_ && updating_folders_.empty() && file_restart_ == FileRestart::kNone) {
    if (auto finished = backend_.PollImport()) {
      if (*finished) AcceptImport(std::move(**finished), now);
      else {
        import_pending_ = false;
        import_.reset();
        plan_.reset();
        problem_ = Problem{State::kFailed, finished->error()};
      }
      changed = true;
    }
  }
  if (!nvidia_installed_ && backend_.NvidiaSdkArrived()) {
    nvidia_installed_ = true;
    backend_.Log("NVIDIA's Audio Effects SDK was installed since OBS's NVIDIA filters loaded without it.");
  }
  changed |= file_restart_ != before || updating_folders_.empty() == waiting_before ||
             nvidia_installed_ != installed_before;
  if (changed) Reconcile(now);
}

void Controller::Tick(Clock::time_point now) {
  if (stopped_) return;
  bool changed = ReleaseIfDue(now);
  if (devices_due_ && now >= *devices_due_) {
    devices_due_.reset();
    devices_first_.reset();
    audio::Endpoints devices = backend_.ListDevices();
    if (devices != devices_) {
      devices_ = std::move(devices);
      backend_.Log("Audio devices changed.");
    }
    // A device that came back may be what a stalled or failed chain lacked.
    chain_error_.reset();
    retry_at_.reset();
    changed = true;
  }
  if (loaded_ && now >= next_check_) {
    next_check_ = now + timing_.watchdog_interval;
    const uint64_t packets = backend_.ChainPackets();
    const ChainLevel level = backend_.TakeChainLevel();
    if (packets != last_packets_) {
      last_packets_ = packets;
      last_audio_ = now;
      if (now - running_since_ >= timing_.healthy_after) stalls_ = 0;
      FollowLevel(level, now);
    } else if (now - last_audio_ >= timing_.stall_timeout) {
      const Clock::duration delay = timing_.retry_delays[std::min(stalls_, timing_.retry_delays.size() - 1)];
      ++stalls_;
      retry_at_ = now + delay;
      stall_detail_ = std::format("\"{}\" stopped sending audio. {} tries again in {}.", import_->mic->mic.name,
                                  kDisplayName, Seconds(delay));
      backend_.Log(std::format("No audio from the mic for {}.", Seconds(now - last_audio_)));
      changed = true;
    }
  }
  if (retry_at_ && now >= *retry_at_) {
    retry_at_.reset();
    changed = true;
  }
  if (changed) Reconcile(now);
}

std::optional<Controller::Clock::time_point> Controller::NextDeadline() const {
  if (stopped_) return std::nullopt;
  std::optional<Clock::time_point> next;
  const auto consider = [&next](std::optional<Clock::time_point> time) {
    if (time && (!next || *time < *next)) next = time;
  };
  consider(devices_due_);
  if (file_restart_ == FileRestart::kHolding) consider(file_restart_due_);
  if (loaded_) consider(next_check_);
  consider(retry_at_);
  return next;
}

void Controller::Stop() {
  StopChain();
  stopped_ = true;
}

void Controller::CheckFileUpdates(Clock::time_point now) {
  if (file_restart_ != FileRestart::kNone) return;
  const FileUpdate update = backend_.FilesWanted();
  if (update.restart) {
    // The new process inherits the broken oplocks and releases them after
    // this process has exited. Stopping the chain doesn't unload the SDK.
    file_restart_ = FileRestart::kHolding;
    file_restart_nvidia_ = update.nvidia;
    // From now, not `now`: an import that took seconds may be how long ago
    // this check began.
    file_restart_due_ = std::max(now, timing_.clock()) + timing_.file_restart_wait;
    snapshot_.files_held = true;
    publish_(snapshot_);
  } else if (!update.folders.empty() && updating_folders_.empty()) {
    // Publish before unloading: a plugin can take too long to close. The
    // tray's own timer can explain the hold while the core is still busy.
    snapshot_.files_held = true;
    publish_(snapshot_);
    StopChain();
  }
}

void Controller::WaitForFiles(std::vector<std::filesystem::path> folders, bool nvidia, Clock::time_point now) {
  updating_folders_ = std::move(folders);
  updating_nvidia_ = nvidia;
  folder_fingerprint_ = backend_.FingerprintFolders(updating_folders_);
  files_quiet_since_ = std::max(now, timing_.clock());
  file_wait_since_ = files_quiet_since_;
  ClearFailures();
}

void Controller::ReleaseUpdateFiles() {
  if (file_restart_ != FileRestart::kHolding) return;
  backend_.ReleaseUpdateFiles();
  file_restart_ = FileRestart::kReleased;
}

bool Controller::ReleaseIfDue(Clock::time_point now) {
  if (file_restart_ != FileRestart::kHolding || now < file_restart_due_) return false;
  ReleaseUpdateFiles();
  return true;
}

void Controller::Refresh(Clock::time_point now) {
  CheckFileUpdates(now);
  // A re-import, OBS closing or a settings change cannot load files halfway
  // through an update. The eventual refresh uses the latest settings.
  if (file_restart_ != FileRestart::kNone || !updating_folders_.empty()) return;
  problem_.reset();
  auto previous_import = std::exchange(import_, std::nullopt);
  auto previous_plan = std::exchange(plan_, std::nullopt);
  import_pending_ = false;
  obs_install_ = {};
  obs_writes_config_ = false;
  profile_cable_id_.clear();
  profile_cable_name_.clear();
  const auto fail = [this](State state, std::string detail, SetupNeed setup = SetupNeed::kNone,
                           RestartNeed restart = RestartNeed::kNone) {
    problem_ = Problem{state, std::move(detail), setup, restart};
  };

  const ObsCheck obs = backend_.CheckObs(settings_);
  if (obs.found == ObsFound::kMissing) return fail(State::kObsMissing, obs.message);
  obs_install_ = obs.install;
  obs_writes_config_ = obs.config && obs.config->obs_writes_here;
  if (obs.found == ObsFound::kUnsupported) return fail(State::kObsUnsupported, obs.message);
  if (wait_for_nvidia_) {
    // Nothing has loaded in this process yet.
    wait_for_nvidia_ = false;
    auto folders = backend_.NvidiaUpdateFolders(obs.install.version);
    if (!folders.empty()) {
      WaitForFiles(std::move(folders), true, now);
      return;
    }
  }
  // libobs stays loaded until the process ends (runtime::ObsRuntime).
  if (libobs_ && obs.install.version != libobs_->version) {
    // Newer where the old one was is an update. Older, or another folder
    // chosen since, isn't.
    if (obs.install.version > libobs_->version && obs.install.root == libobs_->root) {
      return fail(State::kRestartNeeded,
                  std::format("OBS was updated from {} to {}. {} has to restart to use it.",
                              libobs_->version.ToString(), obs.install.version.ToString(), kDisplayName),
                  SetupNeed::kNone, RestartNeed::kObsUpdated);
    }
    return fail(State::kRestartNeeded,
                std::format("{} runs OBS {}, and has to restart to use OBS {}.", kDisplayName,
                            libobs_->version.ToString(), obs.install.version.ToString()),
                SetupNeed::kNone, RestartNeed::kObsChanged);
  }
  if (!obs.config) return fail(State::kNeedsSetup, obs.config.error(), SetupNeed::kObsSettings);
  auto config = backend_.ReadObsConfig(*obs.config);
  if (!config) return fail(State::kNeedsSetup, config.error(), SetupNeed::kObsSettings);
  const import::ProfileAudio& audio = config->audio;
  profile_cable_id_ = audio.monitoring_device_id;
  profile_cable_name_ = audio.monitoring_device_name;
  if (!libobs_) {
    if (libobs_spent_) {
      // Only a new process can try again, and the tray starts one by itself.
      return fail(State::kRestartNeeded,
                  std::format("OBS's core (libobs) failed to start, and can't start again while {0} runs. {0} has "
                              "to restart to try again.",
                              kDisplayName));
    }
    const Status started = backend_.StartLibobs(obs.install, audio);
    if (!started) {
      libobs_spent_ = !backend_.LibobsCanRetry();
      return fail(State::kFailed, started.error());
    }
    libobs_ = Libobs{obs.install.version, obs.install.root, audio.sample_rate, audio.speakers, audio.channel_setup};
  } else if (audio.sample_rate != libobs_->sample_rate || audio.speakers != libobs_->speakers) {
    // OBS restarts for this too (OBSBasic::GetRestartRequirements).
    return fail(State::kRestartNeeded,
                std::format("The OBS profile's audio changed from {} Hz {} to {} Hz {}. {} has to restart to follow "
                            "it, as OBS does.",
                            libobs_->sample_rate, libobs_->channel_setup, audio.sample_rate, audio.channel_setup,
                            kDisplayName),
                SetupNeed::kNone, RestartNeed::kAudioChanged);
  }

  auto imported = backend_.ImportMic(*config, settings_.mic);
  if (!imported) return fail(State::kFailed, imported.error());
  if (imported->pending && loaded_ && imported->restart == RestartNeed::kNone) {
    // Keep the current chain and its description until the checks finish.
    // An unchanged re-import must not interrupt audio just to check a file.
    import_ = std::move(previous_import);
    plan_ = std::move(previous_plan);
  }
  AcceptImport(std::move(*imported), now);
}

void Controller::AcceptImport(MicImport imported, Clock::time_point now) {
  const auto fail = [this](State state, std::string detail, SetupNeed setup = SetupNeed::kNone,
                           RestartNeed restart = RestartNeed::kNone) {
    problem_ = Problem{state, std::move(detail), setup, restart};
  };
  import_pending_ = imported.pending;
  CheckFileUpdates(std::max(now, timing_.clock()));
  if (import_pending_ && loaded_ && imported.restart == RestartNeed::kNone) return;
  import_ = std::move(imported);
  if (import_->restart == RestartNeed::kNvidiaInstalled) {
    // Kept apart from problem_, so that a new cable or another import can't
    // lose it.
    if (!nvidia_installed_) backend_.Log("NVIDIA's Audio Effects SDK was installed since OBS's NVIDIA filters loaded.");
    nvidia_installed_ = true;
    return;
  }
  if (import_->restart == RestartNeed::kNvidiaUnused) {
    return fail(State::kRestartNeeded,
                std::format("NVIDIA's noise removal is no longer needed. {} has to restart to free its memory.",
                            kDisplayName), SetupNeed::kNone, RestartNeed::kNvidiaUnused);
  }
  if (import_->pending) return;
  if (!import_->mic) {
    return fail(State::kNeedsSetup, import_->pick_error,
                import_->mics.empty() ? SetupNeed::kNoMic : SetupNeed::kPickMic);
  }
  const import::ImportedMic& mic = *import_->mic;
  if (mic.chain_key != chain_key_) {
    if (chain_revision_ > 0) backend_.Log("The mic or its chain changed in OBS.");
    chain_key_ = mic.chain_key;
    ++chain_revision_;
  } else if (mic.plugins_key != plugins_key_) {
    // Not a change in OBS: an installer updated, added or removed a plugin.
    backend_.Log("A VST plugin's file changed.");
  }
  plugins_key_ = mic.plugins_key;
  PlanChain();
}

void Controller::PlanChain() {
  // After an import that found the mic, the cable is the only problem left.
  problem_.reset();
  plan_.reset();
  const std::string cable = settings_.cable.empty() ? profile_cable_id_ : settings_.cable;
  if (SameId(cable, kDefaultDevice)) {
    problem_ = Problem{State::kNeedsSetup,
                       "OBS's Monitoring Device is Default, which is usually your speakers. Choose the cable to send "
                       "the mic to.",
                       SetupNeed::kCable};
    return;
  }
  const import::ImportedMic& mic = *import_->mic;
  plan_ = ChainPlan{mic.source_json, mic.chain_key, mic.load_callbacks(), {"", cable}, mic.vst_plugins,
                    mic.plugins_key};
}

void Controller::Reconcile(Clock::time_point now) {
  if (stopped_) return;
  Snapshot next = Decide(now);
  const bool waiting = !updating_folders_.empty();
  const auto restart_before = file_restart_;
  if (next.state != State::kRunning) {
    StopChain();
  } else if (!loaded_ || !loaded_->SameAs(*plan_) || (MicIsDefault() && loaded_default_mic_ != devices_.default_mic)) {
    // A new chain, or the same one rebuilt: libobs's loader runs again, and
    // win-wasapi opens the device afresh.
    StopChain();
  }
  if ((!waiting && !updating_folders_.empty()) || restart_before != file_restart_ ||
      (import_pending_ && !loaded_)) {
    // Stopping found an installer, or checks must finish before a new load.
    next = Decide(now);
  } else if (next.state == State::kRunning && !loaded_) {
    ChainPlan plan = *plan_;
    plan.cable.name = CableName();
    const Status started = backend_.StartChain(plan);
    if (started) {
      // Not `now`: the import or the chain's start may have taken seconds.
      const Clock::time_point started_at = timing_.clock();
      loaded_ = std::move(plan);
      loaded_default_mic_ = devices_.default_mic;
      running_since_ = started_at;
      last_audio_ = started_at;
      last_packets_ = 0;
      next_check_ = started_at + timing_.watchdog_interval;
      // The chain's monitor is new too.
      monitor_since_ = started_at;
      monitor_restarted_ = false;
      last_sound_ = started_at;
      levels_.clear();
      backend_.Log(std::format("Loaded the chain: {}", FormatChain(*next.chain, false)));
    } else {
      chain_error_ = started.error();
      backend_.Log(std::format("The chain didn't start: {}", started.error()));
      next = Decide(now);
    }
  }
  if (next == snapshot_) return;
  snapshot_ = std::move(next);
  backend_.Log(DescribeSnapshot(snapshot_));
  publish_(snapshot_);
}

Snapshot Controller::Decide(Clock::time_point now) const {
  Snapshot next;
  next.obs_running = obs_running_;
  next.other_obs = other_obs_;
  next.paused_by_user = paused_by_user_;
  next.chain_revision = chain_revision_;
  next.obs_cable = {profile_cable_name_, profile_cable_id_};
  next.outputs = devices_.outputs;
  next.default_output = devices_.default_output;
  next.disabled_outputs = devices_.disabled_outputs;
  next.vb_cable_pending = devices_.vb_cable_pending;
  next.inputs = devices_.mics;
  next.default_input = devices_.default_mic;
  next.obs = obs_install_;
  next.obs_writes_config = obs_writes_config_;
  next.settings = settings_;
  next.files_waiting = wait_for_nvidia_;
  if (import_) {
    next.mics = import_->mics;
    next.picked_mic = import_->picked;
    if (import_->mic) {
      next.notes = import_->mic->notes;
      next.chain = ChainSummary{import_->mic->mic.name, import_->mic->filters, CableName()};
    }
  }
  const auto set = [&next](State state, std::string detail = {}) {
    next.state = state;
    next.detail = std::move(detail);
  };
  if (file_restart_ != FileRestart::kNone) {
    next.state = State::kRestartNeeded;
    next.restart = file_restart_nvidia_ ? RestartNeed::kNvidiaUpdating : RestartNeed::kPluginUpdating;
    next.restart_delayed = file_restart_ == FileRestart::kReleased;
    next.files_held = file_restart_ == FileRestart::kHolding;
    if (next.restart_delayed) {
      next.detail = std::format("{} couldn't restart yet, so it let the installer continue. "
                                "If the installer says a file is in use, quit {}, then retry the installer.",
                                kDisplayName, kDisplayName);
    } else if (file_restart_nvidia_) {
      next.detail = std::format("An installer is updating NVIDIA's Audio Effects SDK. {} has to restart to let it finish.",
                                kDisplayName);
    } else {
      next.detail = std::format("An installer is updating a plugin {0} is using. {0} has to restart to let it finish.",
                                kDisplayName);
    }
  } else if (!updating_folders_.empty()) {
    set(libobs_ ? State::kPausedForUpdate : State::kStarting, "Waiting for the updated files to stop changing.");
    next.files_waiting = true;
    next.nvidia_update = updating_nvidia_;
  } else if (nvidia_installed_) {
    set(State::kRestartNeeded,
        std::format("NVIDIA's Audio Effects SDK was installed after {0} loaded OBS's NVIDIA filters. {0} has to "
                    "restart to use it.",
                    kDisplayName));
    next.restart = RestartNeed::kNvidiaInstalled;
  } else if (problem_) {
    set(problem_->state, problem_->detail);
    next.setup = problem_->setup;
    next.restart = problem_->restart;
  } else if (import_pending_ && !loaded_) {
    set(paused_by_user_ ? State::kPausedByUser : State::kStarting, "Checking your mic's VST plugins…");
  } else if (chain_error_) {
    set(State::kFailed, *chain_error_);
  } else if (!devices_.error.empty()) {
    // Without the lists, the cable and the mic would read as unplugged.
    set(State::kFailed, devices_.error);
  } else if (paused_by_user_) {
    set(State::kPausedByUser);
  } else if (obs_running_ && settings_.pause_for_obs) {
    set(State::kPausedForObs);
  } else if (!CablePresent()) {
    set(State::kCableMissing, std::format("\"{}\" isn't connected.", CableName()));
  } else if (!MicPresent()) {
    set(State::kMicMissing, MicIsDefault() ? std::string("There's no recording device.")
                                           : std::format("The recording device for \"{}\" isn't connected.",
                                                         import_->mic->mic.name));
  } else if (retry_at_ && now < *retry_at_) {
    set(State::kMicMissing, stall_detail_);
  } else {
    set(State::kRunning);
  }
  return next;
}

void Controller::StopChain() {
  if (!loaded_) return;
  backend_.StopChain();
  // The backend checks for late writers after unload, before releasing
  // watches, and retains that result for the controller.
  if (updating_folders_.empty() && file_restart_ == FileRestart::kNone) {
    FileUpdate update = backend_.FilesWanted();
    if (update.restart) {
      file_restart_ = FileRestart::kHolding;
      file_restart_nvidia_ = update.nvidia;
      file_restart_due_ = timing_.clock() + timing_.file_restart_wait;
    } else if (!update.folders.empty()) {
      WaitForFiles(std::move(update.folders), false, timing_.clock());
    }
  }
  loaded_.reset();
}

void Controller::ClearFailures() {
  chain_error_.reset();
  retry_at_.reset();
  stalls_ = 0;
}

void Controller::FollowLevel(const ChainLevel& level, Clock::time_point now) {
  levels_.push_back(level.typical);
  const auto window = std::max<Clock::rep>(1, timing_.noise_floor_window / timing_.watchdog_interval);
  while (levels_.size() > static_cast<size_t>(window)) levels_.pop_front();
  const float floor = *std::min_element(levels_.begin(), levels_.end());
  const float silent = std::max(kSilentPeak, std::min(floor * kFloorMargin, kQuietCeiling));
  if (level.typical > silent) {
    last_sound_ = now;
    return;
  }
  if (now - monitor_since_ < timing_.monitor_restart_every || now - last_sound_ < timing_.monitor_restart_silence) {
    return;
  }
  // A sound at the very end may be a word starting, which a restart would
  // cut into. It doesn't spoil the silence before it, so the next check may
  // restart.
  if (level.latest > silent) return;
  backend_.RestartMonitor();
  backend_.Log(std::format("Restarted the monitor {} after {}, {} into silence (at or below {}{}), so the delay to "
                           "the cable starts again from the least.",
                           Minutes(now - monitor_since_), monitor_restarted_ ? "its last restart" : "the chain started",
                           Seconds(now - last_sound_), Dbfs(silent),
                           silent > kSilentPeak ? ", within 6 dB of the noise floor" : ""));
  monitor_since_ = now;
  monitor_restarted_ = true;
}

bool Controller::MicIsDefault() const {
  return import_ && import_->mic && SameId(import_->mic->mic.device_id, kDefaultDevice);
}

bool Controller::MicPresent() const {
  if (!import_ || !import_->mic) return false;
  if (MicIsDefault()) return !devices_.default_mic.empty();
  return FindById(devices_.mics, import_->mic->mic.device_id) != nullptr;
}

bool Controller::CablePresent() const { return plan_ && FindById(devices_.outputs, plan_->cable.id) != nullptr; }

std::string Controller::CableName() const {
  if (!plan_) return "";
  const std::string& id = plan_->cable.id;
  if (const audio::AudioDevice* device = FindById(devices_.outputs, id)) return device->name;
  if (SameId(id, profile_cable_id_) && !profile_cable_name_.empty()) return profile_cable_name_;
  if (SameId(id, settings_.cable) && !settings_.cable_name.empty()) return settings_.cable_name;
  return id;
}

}  // namespace knobs::core
