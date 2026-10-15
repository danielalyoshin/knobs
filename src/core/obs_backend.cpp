// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/obs_backend.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <utility>
#include <vector>

#include "app_info.h"
#include "audio/audio_devices.h"
#include "audio/device_watch.h"
#include "runtime/obs_layout.h"
#include "runtime/obs_version.h"
#include "runtime/runtime_copy.h"
#include "util/win_strings.h"

namespace knobs::core {
namespace {

namespace fs = std::filesystem;

// OBS's install where its installer puts it.
Result<runtime::ObsInstall> FindInstalledObs() {
  const std::vector<fs::path> candidates = runtime::ObsInstallCandidates();
  if (candidates.empty()) {
    return Error{std::format("OBS Studio isn't installed. {} needs {}.", kDisplayName,
                             runtime::DescribeSupportedObsVersions())};
  }
  return runtime::FindObsInstall(candidates);
}

// ChainLevel::latest: 200 ms of win-wasapi's 10 ms packets.
constexpr size_t kLatestPackets = 20;
// Enough for a minute of packets, in case the core's thread stops asking.
constexpr size_t kMaxPackets = 6000;

// What pruning did, for the log. Nothing when there was nothing to prune.
void LogPruned(const runtime::PrunedCopies& pruned, runtime::ObsLog& log) {
  if (pruned.busy) {
    log.Write(LOG_INFO,
              "Another program was making or loading an OBS runtime copy, so old copies stay until next time.");
    return;
  }
  if (!pruned.removed.empty()) {
    std::string names;
    for (const fs::path& folder : pruned.removed) {
      names += std::format("{}{}", names.empty() ? "" : ", ", ToUtf8(folder.filename()));
    }
    log.Write(LOG_INFO, std::format("Removed OBS runtime copies no longer needed ({:.1f} MB): {}",
                                    static_cast<double>(pruned.removed_bytes) / (1024 * 1024), names));
  }
  for (const runtime::PrunedCopies::Left& left : pruned.left) {
    log.Write(LOG_INFO, std::format("Left {} for next time: {}", ToUtf8(left.folder), left.why));
  }
  if (pruned.stopped) {
    log.Write(LOG_INFO, "Stopped removing old OBS runtime copies to shut down, so the rest stay until next time.");
  }
}

}  // namespace

ChainLevel SummarizeChainLevel(std::vector<float> packet_peaks) {
  ChainLevel level;
  if (packet_peaks.empty()) return level;
  const size_t latest = std::min(packet_peaks.size(), kLatestPackets);
  level.latest = *std::max_element(packet_peaks.end() - latest, packet_peaks.end());
  // One packet in ten, rounded down, is left out.
  const auto typical = packet_peaks.end() - 1 - packet_peaks.size() / 10;
  std::nth_element(packet_peaks.begin(), typical, packet_peaks.end());
  level.typical = *typical;
  return level;
}

ObsBackend::ObsBackend(ObsBackendOptions options) : options_(std::move(options)) {}

ObsBackend::~ObsBackend() {
  // It writes to host_'s log. Asked to stop, it stops once it's done with the
  // file it's on, so quitting doesn't wait for the rest.
  pruner_.request_stop();
  if (pruner_.joinable()) pruner_.join();
  StopChain();
  importing_.reset();  // libobs data dies before its session. Workers own only paths.
  // A successful SDK handoff already completed its oplock requests, leaving
  // breaks that the new copy acknowledges only once our DLLs are unmapped.
  if (!host_) return;
  const long leaks = host_->Shutdown();
  if (leaks != 0) host_->log().Write(LOG_WARNING, std::format("libobs leaked {} allocation(s).", leaks));
  if (options_.on_shutdown) options_.on_shutdown(leaks, host_->log());
}

ObsCheck ObsBackend::CheckObs(const Settings& settings) {
  ObsCheck check;
  // A picked folder OBS has gone from, or that holds an OBS knobs doesn't
  // support, stays the user's to change: the first run offers to pick
  // another or to look for OBS as with no pick.
  auto install = settings.obs_dir ? runtime::InspectObsInstall(*settings.obs_dir) : FindInstalledObs();
  if (!install) {
    check.message = install.error();
    return check;
  }
  check.install = *install;
  // The same for a picked settings folder. Opening OBS fills only the folder
  // it keeps its settings in, so the errors suggest it only for that one.
  check.config = import::ObsConfigRootFor(install->root, settings.obs_config);
  if (!runtime::IsSupportedObsVersion(install->version)) {
    check.found = ObsFound::kUnsupported;
    check.message = runtime::UnsupportedObsMessage(install->version.ToString());
    return check;
  }
  check.found = ObsFound::kYes;
  return check;
}

Result<import::ActiveObsConfig> ObsBackend::ReadObsConfig(const import::ObsConfigRoot& root) {
  return import::FindActiveObsConfig(root);
}

Status ObsBackend::StartLibobs(const runtime::ObsInstall& install, const import::ProfileAudio& audio) {
  if (host_) return Error{"libobs is already running."};
  baseline_modules_ = ListLoadedFiles();
  runtime::HostOptions options;
  options.install = install;
  options.log = options_.log;
  options.log_prefix = options_.log_prefix;
  options.verbose = options_.verbose;
  options.samples_per_sec = audio.sample_rate;
  options.speakers = audio.speakers;
  auto host = runtime::ObsHost::Start(options);
  if (!host) return Error{host.error()};
  host_ = std::move(*host);
  if (options_.prune_runtime) {
    // Now the copy in use is known and loaded. Off the core's thread, so the
    // chain doesn't wait for 50 MB of deletes.
    pruner_ = std::jthread([base = host_->app_dirs().RuntimeBase(), keep = host_->copy().root,
                            &log = host_->log()](std::stop_token stop) {
      const runtime::PruneOptions options{.stop_requested = [&stop] { return stop.stop_requested(); }};
      LogPruned(runtime::PruneRuntimeCopies(base, keep, options), log);
    });
  }
  return Ok{};
}

bool ObsBackend::LibobsCanRetry() {
  // A failed start frees obs.dll, which unloads it unless a module libobs
  // opened keeps it loaded. Then libobs's state, and the modules', stayed
  // with it, and starting again would run on them.
  return GetModuleHandleW(runtime::kObsDll.data()) == nullptr;
}

Result<MicImport> ObsBackend::ImportMic(const import::ActiveObsConfig& config, std::string_view pick) {
  if (!host_) return Error{"libobs isn't running."};
  auto collection = import::ReadActiveCollection(host_->api(), config);
  if (!collection) return Error{collection.error()};
  if ((*collection)->from_backup()) {
    Log(std::format("The scene collection doesn't parse, so this is its backup: {}", ToUtf8((*collection)->file())));
  }
  importing_ = std::move(*collection);
  importing_->ProbePluginsOnWorkers();
  importing_pick_ = pick;
  auto result = FinishImport(*importing_, pick);
  if (!result || !result->pending) importing_.reset();
  return result;
}

std::optional<Result<MicImport>> ObsBackend::PollImport() {
  if (!importing_) return std::nullopt;
  auto result = FinishImport(*importing_, importing_pick_);
  if (result && result->pending) return std::nullopt;
  importing_.reset();
  return result;
}

Result<MicImport> ObsBackend::FinishImport(import::SceneCollection& collection, std::string_view pick) {
  const runtime::ObsApi& api = host_->api();
  runtime::ObsSession& session = host_->session();
  MicImport result;
  result.mics = collection.Mics(session.module_errors(), false);
  chain_needs_nvidia_ = false;
  auto picked =
      options_.pick_by_number ? import::PickMic(result.mics, pick) : import::PickMicByName(result.mics, pick);
  if (!picked) {
    result.mics = collection.Mics(session.module_errors());
    result.pending = collection.plugins_pending();
    result.pick_error = picked.error();
    if (session.module("nv-filters")) result.restart = RestartNeed::kNvidiaUnused;
    return result;
  }
  // The optional modules the picked mic needs, before its pre-flight tells
  // the filters libobs has from the rest. A module never unloads, so one a
  // chain needed stays for the rest of the session.
  const std::vector<std::string> needed = collection.NeededModules(result.mics[*picked]);
  chain_needs_nvidia_ = std::find(needed.begin(), needed.end(), "nv-filters") != needed.end();
  if (!chain_needs_nvidia_ && session.module("nv-filters")) {
    result.restart = RestartNeed::kNvidiaUnused;
    return result;
  }
  if (chain_needs_nvidia_ && !nvidia_sdk_at_load_) {
    nvidia_sdk_at_load_ = import::NvidiaAudioEffectsInstalled(api);
    nvidia_folder_ = import::NvidiaAudioEffectsFolder(runtime::ObsVersion::FromLibobs(api.obs_get_version()));
  }
  const bool sdk_arrived = NvidiaSdkArrived();
  const bool nvidia_loaded = session.module("nv-filters") != nullptr;
  auto imported = collection.LoadAndImport(session, result.mics[*picked], sdk_arrived);
  if (!nvidia_loaded && session.module("nv-filters")) WatchNvidiaFiles(nvidia_folder_);
  if (!imported) return Error{imported.error()};
  result.mics = std::move(imported->mics);
  result.picked = *picked;
  result.mic = std::move(imported->mic);
  result.pending = collection.plugins_pending();
  if (sdk_arrived) result.restart = RestartNeed::kNvidiaInstalled;
  return result;
}

audio::Endpoints ObsBackend::ListDevices() { return audio::ListEndpoints(); }

Status ObsBackend::StartChain(const ChainPlan& plan) {
  if (!host_) return Error{"libobs isn't running."};
  StopChain();
  const runtime::ObsApi& api = host_->api();
  // libobs makes no monitor for a device it can't open, and says nothing, so
  // the cable would get silence while knobs ran (docs/design.md, Monitoring
  // into the cable).
  if (const Status opens = audio::CheckPlaybackDevice(plan.cable.id, plan.cable.name); !opens) return opens;
  // First: the monitor opens its device as soon as monitoring starts.
  const Status routed = audio::SetMonitoringDevice(api, plan.cable);
  if (!routed) return routed;
  auto chain = audio::LiveChain::Start(api, host_->session(), plan.source_json, {.load_callbacks = plan.load_callbacks});
  if (!chain) return Error{chain.error()};
  chain_ = std::move(*chain);
  packets_ = 0;
  {
    std::lock_guard lock(level_mutex_);
    packet_peaks_.clear();
  }
  volume_ = api.obs_source_get_volume(chain_->source());
  api.obs_source_add_audio_capture_callback(chain_->source(), OnChainAudio, this);
  WatchChainFiles(plan);
  return Ok{};
}

void ObsBackend::StopChain() {
  if (!chain_) return;
  host_->api().obs_source_remove_audio_capture_callback(chain_->source(), OnChainAudio, this);
  chain_.reset();
  ReleaseChainFiles();
}

void ObsBackend::WatchFile(const fs::path& file, std::vector<std::unique_ptr<FileOplock>>& watches,
                           std::string_view advice) {
  if (std::any_of(watches.begin(), watches.end(), [&](const auto& watch) { return SamePath(watch->file(), file); })) {
    return;
  }
  auto watch = FileOplock::Take(file);
  if (watch) {
    watches.push_back(std::move(*watch));
  } else if (std::none_of(unwatchable_.begin(), unwatchable_.end(),
                          [&](const fs::path& path) { return SamePath(path, file); })) {
    // Once a file: every chain start would say it again, as on a drive
    // without oplocks.
    unwatchable_.push_back(file);
    Log(std::format("Couldn't watch for an update: {} {}", watch.error(), advice));
  }
}

void ObsBackend::WatchNvidiaFiles(const fs::path& folder) {
  nvidia_folder_ = folder;
  WatchLoadedFiles(watch_plugins_);
}

void ObsBackend::WatchChainFiles(const ChainPlan& plan) {
  watch_plugins_ = !plan.vst_plugins.empty();
  stopped_update_ = {};
  WatchLoadedFiles(watch_plugins_);
}

void ObsBackend::WatchLoadedFiles(bool plugins) {
  if (!plugins && nvidia_folder_.empty()) return;
  const auto loaded = ListLoadedFiles();
  wchar_t windows[MAX_PATH] = {};
  GetWindowsDirectoryW(windows, MAX_PATH);
  wchar_t exe[32768] = {};
  GetModuleFileNameW(nullptr, exe, 32768);
  const fs::path app = fs::path(exe).parent_path();
  for (const fs::path& file : loaded) {
    if (InFolder(file, nvidia_folder_)) {
      WatchFile(file, persistent_files_, std::format("Quit {} before replacing this file.", kDisplayName));
    } else if (!InFolder(file, windows) && !InFolder(file, app) &&
               !(host_ && InFolder(file, host_->copy().root)) &&
               std::none_of(baseline_modules_.begin(), baseline_modules_.end(), [&](const fs::path& baseline) {
                 return SamePath(file, baseline);
               })) {
      // Wrapper plugins and NVIDIA's SDK can load processing DLLs elsewhere.
      // Without a VST chain, these belong to the persistent SDK runtime.
      if (std::none_of(persistent_files_.begin(), persistent_files_.end(), [&](const auto& watch) {
            return SamePath(watch->file(), file);
          })) {
        if (plugins) {
          WatchFile(file, vst_files_, std::format("Pause {} before replacing this file.", kDisplayName));
        } else {
          WatchFile(file, persistent_files_, std::format("Quit {} before replacing this file.", kDisplayName));
        }
      }
    }
  }
}

void ObsBackend::ReleaseChainFiles() {
  watch_plugins_ = false;
  if (vst_files_.empty()) return;
  // The source and Qt's posted deletes have finished. A wrapper may keep a
  // dependency loaded; keep its watch until a restart can free it.
  FileUpdate update = FilesWanted();
  if (update.restart || !update.folders.empty()) {
    // An installer can update the SDK and several plugins together. Keep
    // the unloaded plugins' folders too if another DLL needs a restart.
    for (const auto& file : vst_files_) {
      const fs::path folder = file->file().parent_path();
      if (std::none_of(update.folders.begin(), update.folders.end(), [&](const auto& path) {
            return SamePath(path, folder);
          })) update.folders.push_back(folder);
    }
  }
  bool complete = false;
  const auto loaded = ListLoadedFiles(&complete);
  for (auto& watch : vst_files_) {
    if (!complete || std::any_of(loaded.begin(), loaded.end(), [&](const fs::path& file) {
          return SamePath(file, watch->file());
        })) {
      persistent_files_.push_back(std::move(watch));
    }
  }
  vst_files_.clear();
  if (const FileUpdate persistent = FilesWanted(); persistent.restart) {
    update.restart = true;
    update.nvidia = persistent.nvidia;
  }
  if (update.restart) {
    for (const fs::path& folder : update.folders) {
      if (std::none_of(restart_folders_.begin(), restart_folders_.end(), [&](const auto& path) {
            return SamePath(path, folder);
          })) restart_folders_.push_back(folder);
    }
  }
  stopped_update_ = std::move(update);
}

void ObsBackend::PollFileWatches() { WatchLoadedFiles(watch_plugins_); }

std::vector<fs::path> ObsBackend::ListLoadedFiles(bool* complete) {
  return LoadedModules([this](std::string_view error) { Log(error); }, complete);
}

FileUpdate ObsBackend::FilesWanted() {
  const auto wanted = [](const auto& files) {
    bool found = false;
    // Poll every watch, even after a writer, to let readers through and
    // rearm their watches on the core thread.
    for (const auto& file : files) found = file->wanted() || found;
    return found;
  };
  bool persistent = false, sdk = false;
  for (const auto& file : persistent_files_) {
    if (file->wanted()) {
      persistent = true;
      sdk |= InFolder(file->file(), nvidia_folder_);
    }
  }
  const bool vst = wanted(vst_files_);
  if (persistent) return {.restart = true, .nvidia = sdk};
  FileUpdate update = std::exchange(stopped_update_, {});
  if (vst) {
    // One installer may update several plugins. Releasing the chain releases
    // all of them, so wait for all their folders, not just the first file.
    for (const auto& file : vst_files_) {
      const fs::path folder = file->file().parent_path();
      if (std::none_of(update.folders.begin(), update.folders.end(), [&](const fs::path& path) {
            return SamePath(path, folder);
          })) {
        update.folders.push_back(folder);
      }
    }
  }
  return update;
}

std::string ObsBackend::FingerprintFolders(const std::vector<fs::path>& folders) {
  return FolderFingerprint(folders);
}

std::vector<fs::path> ObsBackend::NvidiaUpdateFolders(const runtime::ObsVersion& obs) {
  const fs::path folder = import::NvidiaAudioEffectsFolder(obs);
  return folder.empty() ? std::vector<fs::path>{} : std::vector<fs::path>{folder};
}

bool ObsBackend::NvidiaSdkArrived() {
  // nv-filters registers its filter only if NVIDIA's SDK was there as it
  // loaded (load_nvidia_afx), and a module never loads again.
  if (!host_ || !chain_needs_nvidia_ || nvidia_sdk_at_load_.value_or(true)) return false;
  const runtime::ObsApi& api = host_->api();
  return host_->session().module("nv-filters") && api.obs_get_source_output_flags("nvidia_audiofx_filter") == 0 &&
         import::NvidiaAudioEffectsInstalled(api);
}

RestartFiles ObsBackend::UpdateFileHandles() {
  RestartFiles result;
  bool wanted = false;
  // Poll them all: an exclusive reader must be released even if a writer
  // has already been found on another DLL.
  for (const auto& file : persistent_files_) wanted = file->wanted() || wanted;
  if (!wanted) {
    result.folders = restart_folders_;  // A delayed retry still waits.
    return result;
  }
  for (auto& file : persistent_files_) {
    const fs::path folder = file->file().parent_path();
    if (std::none_of(restart_folders_.begin(), restart_folders_.end(), [&](const auto& path) {
          return SamePath(path, folder);
        })) restart_folders_.push_back(folder);
    if (const Status prepared = PrepareFileHandoff(*file); !prepared) {
      Log(std::format("{} Restarting anyway; the installer may need to retry this file.", prepared.error()));
      result.incomplete = true;
      file.reset();
    } else if (const uintptr_t handle = file->handle()) {
      result.handles.push_back(handle);
    }
  }
  std::erase(persistent_files_, nullptr);
  result.folders = restart_folders_;
  return result;
}

void ObsBackend::ReleaseUpdateFiles() {
  for (const auto& file : persistent_files_) {
    const fs::path folder = file->file().parent_path();
    if (std::none_of(restart_folders_.begin(), restart_folders_.end(), [&](const auto& path) {
          return SamePath(path, folder);
        })) restart_folders_.push_back(folder);
  }
  persistent_files_.clear();
}

uint64_t ObsBackend::ChainPackets() { return packets_.load(std::memory_order_relaxed); }

ChainLevel ObsBackend::TakeChainLevel() {
  taken_peaks_.clear();
  {
    std::lock_guard lock(level_mutex_);
    taken_peaks_.swap(packet_peaks_);
  }
  ChainLevel level = SummarizeChainLevel(taken_peaks_);
  level.typical *= volume_;
  level.latest *= volume_;
  return level;
}

void ObsBackend::RestartMonitor() {
  if (chain_) chain_->RestartMonitor();
}

void ObsBackend::Log(std::string_view line) {
  if (options_.log) {
    options_.log->Write(LOG_INFO, line);
  } else if (host_) {
    host_->log().Write(LOG_INFO, line);
  }
}

void ObsBackend::OnChainAudio(void* param, obs_source_t*, const audio_data* audio, bool) {
  auto* backend = static_cast<ObsBackend*>(param);
  backend->packets_.fetch_add(1, std::memory_order_relaxed);
  // Planar float, one plane per channel, as libobs mixes; the planes past
  // the last channel are null.
  float peak = 0;
  for (size_t c = 0; c < MAX_AV_PLANES && audio->data[c]; ++c) {
    const float* samples = reinterpret_cast<const float*>(audio->data[c]);
    for (uint32_t i = 0; i < audio->frames; ++i) peak = std::max(peak, std::fabs(samples[i]));
  }
  std::lock_guard lock(backend->level_mutex_);
  std::vector<float>& peaks = backend->packet_peaks_;
  if (peaks.size() >= kMaxPackets) peaks.erase(peaks.begin(), peaks.begin() + kMaxPackets / 2);
  peaks.push_back(peak);
}

}  // namespace knobs::core
