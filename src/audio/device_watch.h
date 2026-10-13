// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "audio/audio_devices.h"
#include "util/result.h"

// Audio endpoints straight from Windows (MMDevice API), with no libobs
// involved, so knobs can tell what's connected before libobs runs.
namespace knobs::audio {

// The endpoints that are active (enabled and connected), and what tells why a
// virtual cable isn't. Opens none of them.
struct Endpoints {
  std::vector<AudioDevice> mics;     // Recording.
  std::vector<AudioDevice> outputs;  // Playback.
  // Playback devices turned off in Windows' Sound settings.
  std::vector<AudioDevice> disabled_outputs;
  // VB-Cable's driver was installed since Windows last started: it's among
  // Windows' installed apps, and its devices may wait for a restart.
  bool vb_cable_pending = false;
  // The default communications recording device, which win-wasapi records from
  // for a mic set to "default" (docs/design.md, Mic candidates). Empty if
  // there's none.
  std::string default_mic;
  // The default playback device, which plays the PC's sound: Output in
  // Windows' Sound settings. Empty if there's none.
  std::string default_output;
  // Why Windows' list of devices couldn't be read, if it couldn't: then the
  // lists are empty, and say nothing about what's connected.
  std::string error;

  friend bool operator==(const Endpoints&, const Endpoints&) = default;
};

// Call from a thread with COM initialized. Empty lists, and an error, if
// Windows' device enumerator isn't available.
Endpoints ListEndpoints();

// Opens playback device `id` as libobs's monitor does, in shared mode with
// its own format, and closes it again, playing nothing. An Error, naming the
// device by `name`, if that fails, as when another app uses it in exclusive
// mode: libobs's monitor would fail the same way, and say nothing. Call from a
// thread with COM initialized.
Status CheckPlaybackDevice(std::string_view id, std::string_view name);

// Hears from Windows whenever an audio endpoint is added, removed, enabled,
// disabled, plugged in or unplugged, or a default device changes
// (IMMNotificationClient). Property changes, which some drivers send all the
// time, are left out. Also whenever Windows' installed apps change, as when
// VB-Cable's driver installs (Endpoints::vb_cable_pending).
class DeviceWatch {
 public:
  // `on_change` runs on a thread of Windows' audio service, possibly several
  // times per change; it mustn't block. Start and destroy the watch on a
  // thread with COM initialized.
  static Result<std::unique_ptr<DeviceWatch>> Start(std::function<void()> on_change);
  // Once this returns, `on_change` won't run again.
  ~DeviceWatch();
  DeviceWatch(const DeviceWatch&) = delete;
  DeviceWatch& operator=(const DeviceWatch&) = delete;

 private:
  struct State;
  explicit DeviceWatch(std::unique_ptr<State> state);

  std::unique_ptr<State> state_;
};

}  // namespace knobs::audio
