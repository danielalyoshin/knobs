// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio/live_chain.h"
#include "core/backend.h"
#include "runtime/obs_host.h"
#include "util/files_in_use.h"

namespace knobs::core {

struct ObsBackendOptions {
  // The program's log, which the core's lines go to from the start, and
  // libobs's once it starts. Without one, libobs's start opens a log named
  // with log_prefix in %LocalAppData%\knobs\logs (runtime::HostOptions),
  // and the core's lines before then are dropped.
  std::shared_ptr<runtime::ObsLog> log;
  std::wstring log_prefix;
  // Also print the log to stderr, debug lines included.
  bool verbose = false;
  // Called once libobs has shut down, with the libobs allocations still live
  // (anything but 0 is a leak) and the log. For the dev tools.
  std::function<void(long leaks, const runtime::ObsLog& log)> on_shutdown;
  // Read Settings::mic as the tools' --pick: a number or a name
  // (import::PickMic). Otherwise it's a name, as the tray saves it.
  bool pick_by_number = false;
  // Once libobs has started, remove the runtime copies and leftovers no
  // process uses, on a thread of its own (runtime::PruneRuntimeCopies). The
  // app turns it on. Off, tests and tools leave %LocalAppData%\knobs\runtime
  // as it is.
  bool prune_runtime = false;
};

// Sums up the peaks of the packets a chain filtered, oldest first. The
// typical level leaves out the loudest tenth; the latest is the loudest of
// the last 20 (200 ms of win-wasapi's).
ChainLevel SummarizeChainLevel(std::vector<float> packet_peaks);

// The real thing: the user's OBS install and settings, libobs from its
// runtime copy (runtime::ObsHost), and the chain (audio::LiveChain). libobs
// starts on the thread that calls StartLibobs, and the destructor, which
// shuts it down, has to run on that thread too.
class ObsBackend : public Backend {
 public:
  explicit ObsBackend(ObsBackendOptions options);
  // Stops pruning, stops the chain and shuts libobs down.
  ~ObsBackend() override;

  ObsCheck CheckObs(const Settings& settings) override;
  Result<import::ActiveObsConfig> ReadObsConfig(const import::ObsConfigRoot& root) override;
  Status StartLibobs(const runtime::ObsInstall& install, const import::ProfileAudio& audio) override;
  bool LibobsCanRetry() override;
  Result<MicImport> ImportMic(const import::ActiveObsConfig& config, std::string_view pick) override;
  std::optional<Result<MicImport>> PollImport() override;
  audio::Endpoints ListDevices() override;
  Status StartChain(const ChainPlan& plan) override;
  void StopChain() override;
  FileUpdate FilesWanted() override;
  void PollFileWatches() override;
  std::string FingerprintFolders(const std::vector<std::filesystem::path>& folders) override;
  std::vector<std::filesystem::path> NvidiaUpdateFolders(const runtime::ObsVersion& obs) override;
  bool NvidiaSdkArrived() override;
  RestartFiles UpdateFileHandles() override;
  void ReleaseUpdateFiles() override;
  uint64_t ChainPackets() override;
  ChainLevel TakeChainLevel() override;
  void RestartMonitor() override;
  void Log(std::string_view line) override;

 protected:
  // Null until StartLibobs succeeds.
  runtime::ObsHost* host() { return host_.get(); }
  // The dry backend uses the same watch lifecycle around its stand-in source.
  void WatchChainFiles(const ChainPlan& plan);
  void ReleaseChainFiles();
  // Watch loaded SDK files even without a chain. Also a seam for a scratch
  // SDK in tests: no NVIDIA hardware or libobs is needed to test handoffs.
  void WatchNvidiaFiles(const std::filesystem::path& folder);
  virtual Status PrepareFileHandoff(FileOplock& file) { return file.PrepareHandoff(); }
  virtual std::vector<std::filesystem::path> ListLoadedFiles(bool* complete = nullptr);

 private:
  // A capture callback on the chain: counts what it filters and keeps each
  // packet's peak. It only reads the samples.
  static void OnChainAudio(void* param, obs_source_t* source, const audio_data* audio, bool muted);
  void WatchLoadedFiles(bool plugins);
  Result<MicImport> FinishImport(import::SceneCollection& collection, std::string_view pick);
  // `advice` says what to do instead, for the log, if Windows can't watch it.
  void WatchFile(const std::filesystem::path& file, std::vector<std::unique_ptr<FileOplock>>& watches,
                 std::string_view advice);

  ObsBackendOptions options_;
  std::unique_ptr<runtime::ObsHost> host_;
  std::unique_ptr<audio::LiveChain> chain_;
  std::unique_ptr<import::SceneCollection> importing_;
  std::string importing_pick_;
  // Whether NVIDIA's Audio Effects SDK was there when nv-filters loaded, once
  // it has.
  std::optional<bool> nvidia_sdk_at_load_;
  // Whether the last import's chain needs nv-filters.
  bool chain_needs_nvidia_ = false;
  std::filesystem::path nvidia_folder_;
  std::vector<std::unique_ptr<FileOplock>> persistent_files_;
  std::vector<std::unique_ptr<FileOplock>> vst_files_;
  // A writer noticed while stopping a chain, retained after its watches go.
  FileUpdate stopped_update_;
  std::vector<std::filesystem::path> restart_folders_;
  bool watch_plugins_ = false;
  std::vector<std::filesystem::path> baseline_modules_;
  // Files Windows wouldn't watch, which the log has said.
  std::vector<std::filesystem::path> unwatchable_;
  std::atomic<uint64_t> packets_ = 0;
  std::mutex level_mutex_;
  // Each packet's peak since the last TakeChainLevel, oldest first, before
  // the volume. Guarded by level_mutex_.
  std::vector<float> packet_peaks_;
  // TakeChainLevel's, swapped with packet_peaks_ so the callback doesn't
  // allocate once both have grown.
  std::vector<float> taken_peaks_;
  // The chain's volume, which the monitor applies after the capture
  // callbacks. knobs never changes it while the chain runs.
  float volume_ = 1;
  // Prunes runtime copies, writing to host_'s log. Only the file system and
  // the log, which is thread-safe. Stopped and joined first thing in the
  // destructor.
  std::jthread pruner_;
};

}  // namespace knobs::core
