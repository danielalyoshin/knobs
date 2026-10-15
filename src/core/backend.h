// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/audio_devices.h"
#include "audio/device_watch.h"
#include "core/state.h"
#include "import/mic_import.h"
#include "import/obs_config.h"
#include "runtime/obs_install.h"
#include "util/result.h"

namespace knobs::core {

enum class ObsFound { kYes, kMissing, kUnsupported };

struct ObsCheck {
  ObsFound found = ObsFound::kMissing;
  runtime::ObsInstall install;  // Unless missing.
  std::string message;          // Why not, unless found.
  // The settings folder to read for that install, unless missing: the one
  // picked (Settings::obs_config), else where that OBS keeps them
  // (import::ObsConfigRootFor).
  Result<import::ObsConfigRoot> config = Error{};
};

struct MicImport {
  // Every mic in the scene collection.
  std::vector<import::MicCandidate> mics;
  // The picked one, ready to load. Empty when the pick (Settings::mic)
  // matches none; pick_error says why.
  std::optional<size_t> picked;
  std::optional<import::ImportedMic> mic;
  std::string pick_error;
  // Set when the import found that only a new process can run the chain.
  RestartNeed restart = RestartNeed::kNone;
  bool pending = false;
};

// What the chain runs: a mic's source object, loaded by libobs's own loader
// and monitored to a cable.
struct ChainPlan {
  std::string source_json;  // import::ImportedMic::source_json
  std::string chain_key;    // import::ImportedMic::chain_key
  bool load_callbacks = false;
  audio::AudioDevice cable;
  std::vector<std::filesystem::path> vst_plugins;
  std::string plugins_key;  // import::ImportedMic::plugins_key

  // The same chain, with the same plugin files, to the same cable
  // (audio::SameId). The rest of source_json, and the cable's name, don't
  // change what reaches the cable.
  bool SameAs(const ChainPlan& other) const {
    return chain_key == other.chain_key && plugins_key == other.plugins_key &&
           load_callbacks == other.load_callbacks && audio::SameId(cable.id, other.cable.id);
  }
};

// An installer is waiting to replace loaded files in these folders. NVIDIA's
// DLLs stay loaded until exit; VST plugins unload when the chain stops.
struct FileUpdate {
  std::vector<std::filesystem::path> folders;
  bool restart = false;
  bool nvidia = true;
};

// Borrowed handles prepared for a replacement, and the folders it must let
// settle before loading anything. A partial handoff still restarts.
struct RestartFiles {
  std::vector<uintptr_t> handles;
  std::vector<std::filesystem::path> folders;
  bool incomplete = false;
};

// How loud a running chain's output has been since the last look, from the
// peak of each packet it filtered (10 ms each from win-wasapi), as the monitor
// plays it: after the source's volume. 1 is full scale.
struct ChainLevel {
  // What the packets stayed at or below, leaving out the loudest tenth of
  // them, so a click or a short breath doesn't count.
  float typical = 0;
  // The loudest of the last few packets: what's playing now.
  float latest = 0;
};

// Everything the Controller needs from the world: OBS's install and
// settings, libobs, and the audio devices. Unit tests fake it. Every call,
// destruction included, comes from one thread, and libobs runs on it.
class Backend {
 public:
  virtual ~Backend() = default;

  // Finds the OBS install and its settings folder, and checks its version.
  virtual ObsCheck CheckObs(const Settings& settings) = 0;
  // The active OBS profile and scene collection in `root` (ObsCheck::config).
  virtual Result<import::ActiveObsConfig> ReadObsConfig(const import::ObsConfigRoot& root) = 0;
  // Starts libobs from `install`'s runtime copy, at the profile's sample rate
  // and channels. Once, before anything below. After a failure, again only
  // if LibobsCanRetry says so.
  virtual Status StartLibobs(const runtime::ObsInstall& install, const import::ProfileAudio& audio) = 0;
  // After StartLibobs failed: whether it can run again in this process. Not
  // once libobs had opened a module: libobs never unloads module DLLs, and
  // they keep obs.dll and its state loaded (runtime::ObsRuntime).
  virtual bool LibobsCanRetry() = 0;
  // Reads the scene collection, picks the mic and runs the pre-flight checks.
  virtual Result<MicImport> ImportMic(const import::ActiveObsConfig& config, std::string_view pick) = 0;
  // Completes a pending import without waiting for its file workers.
  virtual std::optional<Result<MicImport>> PollImport() { return std::nullopt; }
  // Doesn't need libobs.
  virtual audio::Endpoints ListDevices() = 0;
  // Loads the chain and monitors it to its cable. Only one chain runs at a
  // time: the Controller stops one before starting the next.
  virtual Status StartChain(const ChainPlan& plan) = 0;
  virtual void StopChain() = 0;
  // The loaded files an installer wants to replace, since the last watch.
  // A VST plugin needs the chain stopped. DLLs still loaded after that,
  // including NVIDIA's SDK, need a restart.
  virtual FileUpdate FilesWanted() = 0;
  // Refresh loaded DLLs on the existing OBS tick; no work without plugins.
  virtual void PollFileWatches() {}
  // Names, sizes and write times in these folders and their subfolders, for
  // noticing writes while an installer works. Doesn't need libobs.
  virtual std::string FingerprintFolders(const std::vector<std::filesystem::path>& folders) = 0;
  // Where this OBS version looks for NVIDIA's SDK, even while it is missing
  // during an update. Used by the new process before loading nv-filters.
  virtual std::vector<std::filesystem::path> NvidiaUpdateFolders(const runtime::ObsVersion& obs) = 0;
  // Whether NVIDIA's SDK is now where nv-filters looks for it, though
  // nv-filters loaded without it for a chain that still needs it. nv-filters
  // looks only as it loads, so only a new process can run the SDK. Cheap
  // enough to ask every second.
  virtual bool NvidiaSdkArrived() = 0;
  // Prepares borrowed handles for an installer waiting on DLLs that stay
  // loaded until exit. The caller starts the replacement, then releases the
  // originals. A failed preparation is logged and marks an incomplete handoff.
  virtual RestartFiles UpdateFileHandles() = 0;
  // Releases the persistent watches after a handoff or a delayed/failed restart.
  // The installer proceeds as without them; loaded DLLs can still block it.
  virtual void ReleaseUpdateFiles() = 0;
  // Audio packets the running chain has filtered since it started. Silence
  // counts: win-wasapi passes silent packets on as zeros.
  virtual uint64_t ChainPackets() = 0;
  // How loud the running chain's output has been since the last call, or
  // since it started.
  virtual ChainLevel TakeChainLevel() = 0;
  // Opens the running chain's monitor stream afresh, dropping the audio it had
  // queued, so the delay to the cable starts again from the least. The chain
  // keeps running; the packets that come meanwhile are lost (docs/design.md,
  // Keeping the delay down).
  virtual void RestartMonitor() = 0;
  // A line for knobs's log, once there is one.
  virtual void Log(std::string_view line) = 0;
};

}  // namespace knobs::core
