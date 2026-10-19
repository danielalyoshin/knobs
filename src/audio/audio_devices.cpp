// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/audio_devices.h"

#include <algorithm>
#include <format>

#include "util/win_strings.h"

namespace knobs::audio {
namespace {

// win-wasapi's input source, and the name of its device list property.
constexpr char kMicSourceId[] = "wasapi_input_capture";
constexpr char kDeviceIdProperty[] = "device_id";

}  // namespace

bool SameId(std::string_view a, std::string_view b) { return AsciiLower(a) == AsciiLower(b); }

const AudioDevice* FindById(const std::vector<AudioDevice>& devices, std::string_view id) {
  const auto found =
      std::find_if(devices.begin(), devices.end(), [&](const AudioDevice& device) { return SameId(device.id, id); });
  return found == devices.end() ? nullptr : &*found;
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

bool IsVirtualCable(std::string_view name) {
  const std::string lower = AsciiLower(name);
  return lower.find("vb-audio") != std::string::npos || lower.find("virtual audio cable") != std::string::npos;
}

bool IsVbCable(std::string_view name) { return AsciiLower(name).find("(vb-audio virtual cable)") != std::string::npos; }

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

bool SameCable(const AudioDevice& a, const AudioDevice& b) {
  if (a.id.empty() || b.id.empty()) return false;
  if (SameId(a.id, b.id)) return true;
  const std::string side = CableRecordingSide(a.name);
  return !side.empty() && side == CableRecordingSide(b.name);
}

std::vector<AudioDevice> ListMicDevices(const runtime::ObsApi& api) {
  std::vector<AudioDevice> devices;
  obs_properties_t* properties = api.obs_get_source_properties(kMicSourceId);
  if (!properties) return devices;
  if (obs_property_t* list = api.obs_properties_get(properties, kDeviceIdProperty)) {
    const size_t count = api.obs_property_list_item_count(list);
    for (size_t i = 0; i < count; ++i) {
      const char* name = api.obs_property_list_item_name(list, i);
      const char* id = api.obs_property_list_item_string(list, i);
      if (id) devices.push_back({name ? name : id, id});
    }
  }
  api.obs_properties_destroy(properties);
  return devices;
}

std::vector<AudioDevice> ListMonitoringDevices(const runtime::ObsApi& api) {
  // libobs lists the endpoints; OBS's settings dialog adds "Default" itself.
  std::vector<AudioDevice> devices = {{"Default", "default"}};
  api.obs_enum_audio_monitoring_devices(
      [](void* param, const char* name, const char* id) {
        static_cast<std::vector<AudioDevice>*>(param)->push_back({name, id});
        return true;
      },
      &devices);
  return devices;
}

Result<AudioDevice> FindDevice(const std::vector<AudioDevice>& devices, std::string_view query,
                               std::string_view kind) {
  if (const AudioDevice* device = FindById(devices, query)) return *device;
  const std::string needle = AsciiLower(query);
  std::vector<const AudioDevice*> matches;
  for (const AudioDevice& device : devices) {
    const std::string name = AsciiLower(device.name);
    if (name == needle) return device;
    if (name.find(needle) != std::string::npos) matches.push_back(&device);
  }
  if (matches.size() == 1) return *matches.front();
  if (matches.empty()) return Error{std::format("No {} matches “{}”.", kind, query)};
  std::string names;
  for (const AudioDevice* device : matches) {
    names += std::format("{}“{}”", names.empty() ? "" : ", ", device->name);
  }
  return Error{std::format("Several {}s match “{}”: {}.", kind, query, names)};
}

Status SetMonitoringDevice(const runtime::ObsApi& api, const AudioDevice& device) {
  if (!api.obs_set_audio_monitoring_device(device.name.c_str(), device.id.c_str())) {
    return Error{std::format("OBS’s audio monitoring couldn’t use “{}”.", device.name)};
  }
  return Ok{};
}

}  // namespace knobs::audio
