// SPDX-License-Identifier: GPL-2.0-or-later
#include "import/mic_import.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <set>

#include "app_info.h"
#include "runtime/obs_layout.h"
#include "runtime/obs_session.h"
#include "runtime/obs_version.h"
#include "runtime/pe_imports.h"
#include "util/text_file.h"
#include "util/win_strings.h"

namespace knobs::import {
namespace {

namespace fs = std::filesystem;

// win-wasapi's input source.
constexpr std::string_view kMicSourceId = "wasapi_input_capture";
// The global audio devices, in the order OBS loads them (OBSBasic::LoadData).
constexpr std::array<const char*, 6> kGlobalDeviceKeys = {"DesktopAudioDevice1", "DesktopAudioDevice2",
                                                          "AuxAudioDevice1",     "AuxAudioDevice2",
                                                          "AuxAudioDevice3",     "AuxAudioDevice4"};
constexpr std::string_view kVstFilterId = "vst_filter";
constexpr std::string_view kNvidiaFilterId = "nvidia_audiofx_filter";
using runtime::kVstOpenWhenActive;
constexpr std::string_view kCompressorId = "compressor_filter";
constexpr std::string_view kGainId = "gain_filter";
// What ImportedMic::chain_key leaves out of a saved source (obs.c,
// obs_save_source).
constexpr std::array<const char*, 17> kKeysTheCableIgnores = {
    // The monitor ignores these (docs/design.md, Mute and push-to-talk), in OBS
    // too.
    "muted", "push-to-mute", "push-to-mute-delay", "push-to-talk", "push-to-talk-delay", "enabled", "sync",
    // knobs sets these itself.
    "monitoring_type", "monitoring_enabled", "hotkeys",
    // The output mix, video, and OBS's own bookkeeping.
    "mixers", "deinterlace_mode", "deinterlace_field_order", "uuid", "canvas_uuid", "private_settings",
    // Only libobs's log uses it.
    "name"};
// What it leaves out of each filter: the same name and UUID. A filter's
// "enabled" stays, since it turns the filter off.
constexpr std::array<const char*, 2> kFilterKeysTheCableIgnores = {"name", "uuid"};

struct DataReleaser {
  const runtime::ObsApi* api;
  void operator()(obs_data_t* data) const { api->obs_data_release(data); }
};
using DataPtr = std::unique_ptr<obs_data_t, DataReleaser>;

struct ArrayReleaser {
  const runtime::ObsApi* api;
  void operator()(obs_data_array_t* array) const { api->obs_data_array_release(array); }
};
using ArrayPtr = std::unique_ptr<obs_data_array_t, ArrayReleaser>;

DataPtr Own(const runtime::ObsApi& api, obs_data_t* data) { return DataPtr(data, DataReleaser{&api}); }
ArrayPtr Own(const runtime::ObsApi& api, obs_data_array_t* array) { return ArrayPtr(array, ArrayReleaser{&api}); }

std::string String(const runtime::ObsApi& api, obs_data_t* data, const char* name) {
  const char* value = api.obs_data_get_string(data, name);
  return value ? value : "";
}

// libobs creates a source by its versioned type, falling back to its type
// (obs_load_source_type).
std::string VersionedId(const runtime::ObsApi& api, obs_data_t* data) {
  std::string id = String(api, data, "versioned_id");
  return id.empty() ? String(api, data, "id") : id;
}

// A key the loader defaults when it's missing (obs_load_source_type).
bool Bool(const runtime::ObsApi& api, obs_data_t* data, const char* name, bool missing) {
  return api.obs_data_has_user_value(data, name) ? api.obs_data_get_bool(data, name) : missing;
}

// A compressor that ducks under another source: the only filter in
// obs-filters 32.2.2 with a sidechain.
struct Ducking {
  std::string sidechain;   // The source it follows.
  double output_gain = 0;  // In dB, as saved.
};

std::optional<Ducking> AsDucking(const runtime::ObsApi& api, obs_data_t* filter) {
  if (String(api, filter, "id") != kCompressorId) return std::nullopt;
  DataPtr settings = Own(api, api.obs_data_get_obj(filter, "settings"));
  const std::string sidechain = settings ? String(api, settings.get(), "sidechain_source") : "";
  if (sidechain.empty() || sidechain == "none") return std::nullopt;
  return Ducking{sidechain, api.obs_data_get_double(settings.get(), "output_gain")};
}

// What OBS's compressor does while nothing plays on its sidechain: OBS's Gain
// filter at the compressor's output gain, in its place.
//
// With a sidechain, the compressor follows the sidechain's level instead of
// the mic's; without one, the mic's (compressor-filter.c,
// compressor_filter_audio). knobs loads only the mic, so as saved it would
// compress the mic by its own level. While the sidechain is below the
// threshold, silence included, the compressor's gain is exactly 1, and each
// sample is multiplied by db_to_mul((float)output_gain), which is what the
// Gain filter does with its "db" (gain-filter.c): the same audio, bit for bit.
DataPtr GainInsteadOf(const runtime::ObsApi& api, obs_data_t* compressor, double db) {
  // A copy keeps the filter's name, its enabled flag and the rest the loader
  // reads; only its type and settings change.
  const char* json = api.obs_data_get_json(compressor);
  DataPtr gain = Own(api, json ? api.obs_data_create_from_json(json) : nullptr);
  if (!gain) return gain;
  api.obs_data_set_string(gain.get(), "id", kGainId.data());
  api.obs_data_set_string(gain.get(), "versioned_id", kGainId.data());
  DataPtr settings = Own(api, api.obs_data_create());
  api.obs_data_set_double(settings.get(), "db", db);
  api.obs_data_set_obj(gain.get(), "settings", settings.get());
  return gain;
}

// The optional module that registers a filter's type, if any
// (runtime::kOptionalObsModules).
const runtime::OptionalObsModule* OptionalModuleFor(std::string_view id) {
  for (const runtime::OptionalObsModule& module : runtime::kOptionalObsModules) {
    if (module.filter_id == id) return &module;
  }
  return nullptr;
}

// A VST filter's plugin, as obs-vst loads it: the DLL at "plugin_path", or
// none (vst_update, obs-vst.cpp).
std::string VstPluginPath(const runtime::ObsApi& api, obs_data_t* filter) {
  DataPtr settings = Own(api, api.obs_data_get_obj(filter, "settings"));
  return settings ? String(api, settings.get(), "plugin_path") : "";
}

bool VstPluginFound(const std::string& path) {
  std::error_code ec;
  return !path.empty() && fs::is_regular_file(FromUtf8(path), ec);
}

// The folders the loader finds a VST plugin's own DLLs in, as obs-vst loads
// it, with LoadLibraryW and the process's default directories
// (SetDefaultDllDirectories): the program's folder, the runtime copy's
// bin\64bit, which knobs adds (runtime::ObsRuntime::Load) and obs.dll is in,
// and the system's. In OBS, the program's folder is OBS's bin\64bit.
std::vector<fs::path> PluginSearchFolders() {
  std::vector<fs::path> folders;
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD size = static_cast<DWORD>(std::size(path));
  if (GetModuleFileNameW(nullptr, path, size)) folders.push_back(fs::path(path).parent_path());
  if (const HMODULE obs = GetModuleHandleW(runtime::kObsDll.data()); obs && GetModuleFileNameW(obs, path, size)) {
    folders.push_back(fs::path(path).parent_path());
  }
  if (GetSystemDirectoryW(path, size)) folders.push_back(path);
  return folders;
}

// Whether a VST plugin's file is a 64-bit DLL, which obs-vst can load, in
// OBS too. Yes if it can't tell.
bool VstPluginIs64Bit(const std::string& path) {
  const auto is64 = runtime::IsWindows64Binary(FromUtf8(path));
  return !is64 || *is64;
}

// The DLLs a VST plugin imports that knobs's process can't find, so obs-vst
// couldn't load it. Empty if it can't tell.
std::vector<std::string> VstMissingDlls(const std::string& path) {
  auto missing = runtime::UnresolvedDllImports(FromUtf8(path), PluginSearchFolders());
  return missing ? *missing : std::vector<std::string>{};
}

std::string JoinNames(const std::vector<std::string>& names) {
  std::string joined;
  for (size_t i = 0; i < names.size(); ++i) {
    joined += std::format("{}{}", i == 0 ? "" : i + 1 == names.size() ? " and " : ", ", names[i]);
  }
  return joined;
}

// Where nv-filters finds NVIDIA's Audio Effects SDK, which it loads to run
// the filter: the folder NVAFX_SDK_DIR names, from OBS 33 the one AFX_SDK_DIR
// names, or the SDK's own folder in Program Files (nvafx_get_sdk_path,
// nvafx-load.h). Empty if NVAudioEffects.dll isn't there.
fs::path FindNvidiaAudioEffects(const runtime::ObsVersion& obs) {
  const fs::path dir = NvidiaAudioEffectsFolder(obs);
  std::error_code ec;
  return !dir.empty() && fs::is_regular_file(dir / L"NVAudioEffects.dll", ec) ? dir : fs::path();
}

// Whether a filter is part of the chain anyone hears: it's on, libobs has its
// type, and pre-flight keeps it as something that changes the audio. For a
// type it doesn't have, such as a filter from an OBS plugin knobs doesn't
// load, libobs loads a placeholder that passes audio through untouched, and
// so does obs-vst for a plugin it can't load. A compressor with a sidechain
// runs only as its output gain (GainInsteadOf).
//
// An optional module's filter whose module knobs hasn't tried yet counts as
// running if what it needs is there: knobs loads the module once the mic is
// picked, and only then, since nv-filters alone costs hundreds of MB.
// `failed` has the modules that didn't load.
bool Runs(const runtime::ObsApi& api, obs_data_t* filter, const ModuleErrors& failed) {
  if (!Bool(api, filter, "enabled", true)) return false;
  const std::string id = String(api, filter, "id");
  if (api.obs_get_source_output_flags(VersionedId(api, filter).c_str()) == 0) {
    const runtime::OptionalObsModule* module = OptionalModuleFor(id);
    const std::string name = module ? std::string(module->name) : "";
    if (!module || api.obs_get_module(name.c_str()) || failed.contains(name)) return false;
    const runtime::ObsVersion obs = runtime::ObsVersion::FromLibobs(api.obs_get_version());
    if (id == kNvidiaFilterId && FindNvidiaAudioEffects(obs).empty()) return false;
  }
  if (id == kVstFilterId) {
    const std::string path = VstPluginPath(api, filter);
    if (!VstPluginFound(path) || !VstPluginIs64Bit(path) || !VstMissingDlls(path).empty()) return false;
  }
  const auto ducking = AsDucking(api, filter);
  return !ducking || ducking->output_gain != 0;
}

// What obs-vst will load for a VST filter, which its saved settings don't
// say: whether the plugin's file is there, which file it is, and what it
// can't load. For the chain key, so a plugin that appears, is updated or
// goes away restarts the chain.
std::string VstPluginState(const std::string& path) {
  if (!VstPluginFound(path)) return "missing";
  std::error_code ec;
  const fs::path file = FromUtf8(path);
  const uint64_t size = fs::file_size(file, ec);
  const auto written = fs::last_write_time(file, ec).time_since_epoch().count();
  return std::format("{} {} {}", size, written, JoinNames(VstMissingDlls(path)));
}

// Why an optional module's filter isn't one libobs has: the module didn't
// load, or for nv-filters, NVIDIA's runtime isn't there or didn't start, so
// nv-filters registered no filter (load_nvidia_afx, nvidia-audiofx-filter.c).
// A warning, unless OBS can't run it either.
ImportNote WhyNotLoaded(const runtime::ObsApi& api, std::string_view name, std::string_view id,
                        const std::string& plugin, const ModuleErrors& modules) {
  const auto failed = modules.find(id == kVstFilterId ? "obs-vst" : "nv-filters");
  const std::string error = failed == modules.end() ? "" : failed->second;
  if (id == kVstFilterId) {
    return {true, std::format("Filter \"{}\" is a VST plugin ({}), and {} couldn't load OBS's VST module: {} It "
                              "passes audio through untouched.",
                              name, plugin.empty() ? "none chosen" : plugin, kDisplayName,
                              error.empty() ? "obs-vst registered no filter." : error)};
  }
  if (!error.empty()) {
    return {true, std::format("Filter \"{}\" is NVIDIA's noise removal, and {} couldn't load OBS's nv-filters "
                              "module: {} It passes audio through untouched.",
                              name, kDisplayName, error)};
  }
  if (FindNvidiaAudioEffects(runtime::ObsVersion::FromLibobs(api.obs_get_version())).empty()) {
    return {.text = std::format("Filter \"{}\" is NVIDIA's noise removal, which needs NVIDIA's Audio Effects SDK. "
                                "It isn't installed, so the filter passes audio through untouched, as it does in "
                                "OBS without it.",
                                name),
            .changes_expectations = true};
  }
  return {true, std::format("Filter \"{}\" is NVIDIA's noise removal. NVIDIA's Audio Effects SDK is installed but "
                            "didn't start, so the filter passes audio through untouched. It needs an NVIDIA RTX "
                            "GPU, and the log says why it didn't start.",
                            name)};
}

// `file` parsed by libobs as its JSON reader takes it: os_quick_read_utf8_file
// skips a BOM. Null if it doesn't parse.
obs_data_t* ParseJsonFile(const runtime::ObsApi& api, const fs::path& file) {
  auto text = ReadText(file);
  if (!text) return nullptr;
  if (text->starts_with("\xEF\xBB\xBF")) text->erase(0, 3);
  return api.obs_data_create_from_json(text->c_str());
}

// `file` parsed by libobs, else its .bak, as obs_data_create_from_json_file_safe
// would read them. Null if neither parses.
obs_data_t* ParseCollection(const runtime::ObsApi& api, const fs::path& file, bool* from_backup) {
  *from_backup = false;
  if (obs_data_t* data = ParseJsonFile(api, file)) return data;
  obs_data_t* backup = ParseJsonFile(api, fs::path(file) += L".bak");
  *from_backup = backup != nullptr;
  return backup;
}

std::optional<MicCandidate> AsMic(const runtime::ObsApi& api, obs_data_t* source, const ModuleErrors& failed) {
  if (String(api, source, "id") != kMicSourceId) return std::nullopt;
  MicCandidate mic;
  mic.name = String(api, source, "name");
  DataPtr settings = Own(api, api.obs_data_get_obj(source, "settings"));
  mic.device_id = settings ? String(api, settings.get(), "device_id") : "";
  if (mic.device_id.empty()) mic.device_id = "default";  // win-wasapi's default.
  // OBS 33 saves monitoring on or off as its own key (docs/design.md, OBS
  // 33.0).
  mic.monitored = api.obs_data_get_int(source, "monitoring_type") != OBS_MONITORING_TYPE_NONE ||
                  api.obs_data_get_bool(source, "monitoring_enabled");
  ArrayPtr filters = Own(api, api.obs_data_get_array(source, "filters"));
  const size_t count = filters ? api.obs_data_array_count(filters.get()) : 0;
  for (size_t i = 0; i < count; ++i) {
    DataPtr filter = Own(api, api.obs_data_array_item(filters.get(), i));
    if (Runs(api, filter.get(), failed)) mic.filters.push_back(String(api, filter.get(), "name"));
  }
  return mic;
}

std::string FileName(std::string_view path) {
  const size_t slash = path.find_last_of("/\\");
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// One filter's pre-flight. Returns whether to keep it, and sets `instead` to
// a filter to load in its place, if any.
bool CheckFilter(const runtime::ObsApi& api, obs_data_t* filter, const ModuleErrors& modules,
                 std::vector<ImportNote>& notes, DataPtr& instead) {
  const std::string name = String(api, filter, "name");
  const std::string id = String(api, filter, "id");
  const std::string type = VersionedId(api, filter);
  const bool enabled = Bool(api, filter, "enabled", true);
  DataPtr settings = Own(api, api.obs_data_get_obj(filter, "settings"));
  // Keys the note just added by what it's about, so a rename isn't a new note.
  const std::string uuid = String(api, filter, "uuid");
  const auto key = [&](std::string_view kind) {
    notes.back().key = std::format("{} {}", kind, uuid.empty() ? name : uuid);
  };
  const bool optional = OptionalModuleFor(id) != nullptr;
  const bool known = api.obs_get_source_output_flags(type.c_str()) != 0;

  if (!known && !(enabled && optional)) {
    // libobs knows no such type, so it loads a placeholder that does nothing.
    // knobs loads an optional module only for a filter that's on.
    if (!enabled) {
      notes.push_back(optional ? ImportNote{false, std::format("Filter \"{}\" is off in OBS, and stays off.", name)}
                               : ImportNote{false, std::format("Filter \"{}\" ({}) isn't one {} has, but it's off "
                                                               "in OBS anyway.",
                                                               name, type, kDisplayName)});
      key(optional ? "off" : "unknown");
    } else {
      notes.push_back({true, std::format("Filter \"{}\" ({}) isn't one {} has. It passes audio through untouched.",
                                         name, type, kDisplayName)});
      key("unknown");
    }
    return true;
  }
  if (!enabled) {
    notes.push_back({false, std::format("Filter \"{}\" is off in OBS, and stays off.", name)});
    key("off");
    return true;
  }
  if (!known) {
    // An optional module's filter, with the module or what it needs missing.
    const std::string plugin = id == kVstFilterId ? FileName(VstPluginPath(api, filter)) : "";
    notes.push_back(WhyNotLoaded(api, name, id, plugin, modules));
    // Not 0.3.0's keys, "vst" and "unknown", which its own warnings left in
    // the warned list (Notifier::Options::warned) for these filters.
    key(id == kVstFilterId ? "vst-module" : "nvidia-module");
    notes.back().file = plugin;
    return true;
  }
  if (id == kVstFilterId) {
    const std::string path = VstPluginPath(api, filter);
    if (path.empty()) {
      notes.push_back({.text = std::format("VST filter \"{}\" has no plugin chosen, so it passes audio through "
                                           "untouched, in OBS too.",
                                           name),
                       .changes_expectations = true});
      key("vst");
    } else if (!VstPluginFound(path)) {
      notes.push_back({.text = std::format("Filter \"{}\" is a VST plugin ({}) that isn't at {} any more, so it "
                                           "passes audio through untouched, in OBS too.",
                                           name, FileName(path), path),
                       .changes_expectations = true,
                       .file = FileName(path)});
      key("vst");
    } else if (!VstPluginIs64Bit(path)) {
      notes.push_back({.text = std::format("Filter \"{}\" is a VST plugin ({}) that isn't a 64-bit plugin, so it "
                                           "passes audio through untouched, in OBS too.",
                                           name, FileName(path)),
                       .changes_expectations = true,
                       .file = FileName(path)});
      key("vst");
    } else if (const std::vector<std::string> missing = VstMissingDlls(path); !missing.empty()) {
      // In OBS, the program's folder is OBS's bin\64bit, which can have
      // them (PluginSearchFolders).
      notes.push_back({.warning = true,
                       .text = std::format("Filter \"{}\" is a VST plugin ({}) that needs {}, which {} can't find "
                                           "in Windows or in its copy of OBS, so it passes audio through untouched.",
                                           name, FileName(path), JoinNames(missing), kDisplayName),
                       .file = FileName(path)});
      key("vst-dlls");
    }
    // audio::LoadSourceJson turns the window off, and the chain key leaves
    // the setting out.
    if (settings && api.obs_data_get_bool(settings.get(), kVstOpenWhenActive)) {
      notes.push_back({false, std::format("Filter \"{}\" opens its VST plugin's window in OBS. {} shows no windows, "
                                          "so it runs the plugin without it.",
                                          name, kDisplayName)});
      key("window");
    }
    return true;
  }
  if (id == kNvidiaFilterId) {
    // A neural network on the GPU, with a memory: what it outputs depends on
    // what it heard before and on where its 10 ms frames fall, which is when
    // the mic started (nvidia_audio_filter_audio). It also passes audio
    // through untouched until its model has loaded (nvidia_audio_initialize).
    notes.push_back({.text = std::format("Filter \"{}\" is NVIDIA's noise removal, which runs on the GPU. As in OBS, "
                                         "it lets the first moment after the mic starts through untouched, and its "
                                         "output is close to OBS's but not identical, since it depends on what it "
                                         "heard before.",
                                         name),
                     .changes_expectations = true});
    key("nvidia");
    return true;
  }
  if (const auto ducking = AsDucking(api, filter)) {
    // As OBS while nothing plays on the sidechain (GainInsteadOf). At 0 dB
    // that's no change at all.
    const std::string what = std::format("Compressor \"{}\" turns the mic down under \"{}\" in OBS.", name,
                                         ducking->sidechain);
    const std::string then = std::format("and the mic sounds as it does in OBS while nothing plays on \"{}\".",
                                         ducking->sidechain);
    if (ducking->output_gain != 0) instead = GainInsteadOf(api, filter, ducking->output_gain);
    if (instead) {
      notes.push_back({true, std::format("{} {} loads only the mic, so it keeps only this compressor's output gain "
                                         "({:+.1f} dB), {}",
                                         what, kDisplayName, ducking->output_gain, then)});
      key("ducking");
      return true;
    }
    // At 0 dB, or if libobs couldn't copy the filter.
    notes.push_back({true, ducking->output_gain == 0
                               ? std::format("{} {} loads only the mic, so it leaves this compressor out, {}", what,
                                             kDisplayName, then)
                               : std::format("{} {} loads only the mic, so it leaves this compressor out, and its "
                                             "output gain ({:+.1f} dB) with it.",
                                             what, kDisplayName, ducking->output_gain)});
    key("ducking");
    return false;
  }
  return true;
}

// Source-level state the monitor ignores, which a user might expect to
// matter.
void CheckSourceState(const runtime::ObsApi& api, obs_data_t* source, const MicCandidate& mic,
                      std::vector<ImportNote>& notes) {
  std::vector<std::string_view> silencers;
  if (!Bool(api, source, "enabled", true)) silencers.push_back("disabled");
  if (Bool(api, source, "muted", false)) silencers.push_back("muted");
  if (Bool(api, source, "push-to-talk", false)) silencers.push_back("on push-to-talk");
  if (Bool(api, source, "push-to-mute", false)) silencers.push_back("on push-to-mute");
  if (!silencers.empty()) {
    std::string states;
    for (size_t i = 0; i < silencers.size(); ++i) {
      states += std::format("{}{}", i == 0 ? "" : i + 1 == silencers.size() ? " and " : ", ", silencers[i]);
    }
    // obs_source_t's enabled flag counts as muted (obs-source.c). What to
    // do instead: the apps that use the cable have their own.
    std::string keys;
    if (Bool(api, source, "push-to-talk", false)) keys = "push-to-talk";
    if (Bool(api, source, "push-to-mute", false)) keys += keys.empty() ? "push-to-mute" : " and push-to-mute";
    const std::string instead = keys.empty() ? "Mute it in each app that uses the cable instead."
                                             : std::format("Set up {} in each app that uses the cable.", keys);
    notes.push_back({.text = std::format("The mic is {} in OBS, which doesn't silence the cable. {}", states, instead),
                     .changes_expectations = true});
  }
  if (const int64_t sync = api.obs_data_get_int(source, "sync"); sync != 0) {
    notes.push_back({false, std::format("The mic has a sync offset of {} ms. libobs's monitor ignores it for "
                                        "audio-only sources, in OBS too.",
                                        sync / 1'000'000)});
  }
  if (!mic.monitored) {
    notes.push_back(
        {false, std::format("OBS doesn't monitor this mic, so it doesn't send it to a cable. {} will.", kDisplayName)});
  }
}

// Where the collection saves `mic`, or null.
DataPtr SavedMic(const runtime::ObsApi& api, obs_data_t* collection, const MicCandidate& mic) {
  if (mic.origin == MicOrigin::kGlobalDevice) return Own(api, api.obs_data_get_obj(collection, mic.key.c_str()));
  ArrayPtr sources = Own(api, api.obs_data_get_array(collection, mic.key.c_str()));
  if (!sources || mic.index >= api.obs_data_array_count(sources.get())) return DataPtr(nullptr, DataReleaser{&api});
  return Own(api, api.obs_data_array_item(sources.get(), mic.index));
}

// Adds the optional modules whose filters are on in `source`'s chain.
void AddNeededModules(const runtime::ObsApi& api, obs_data_t* source, std::set<std::string>& needed) {
  ArrayPtr filters = Own(api, api.obs_data_get_array(source, "filters"));
  const size_t count = filters ? api.obs_data_array_count(filters.get()) : 0;
  for (size_t i = 0; i < count; ++i) {
    DataPtr filter = Own(api, api.obs_data_array_item(filters.get(), i));
    const runtime::OptionalObsModule* module = OptionalModuleFor(String(api, filter.get(), "id"));
    if (module && Bool(api, filter.get(), "enabled", true)) needed.emplace(module->name);
  }
}

// The mics, numbered, one to a line, for an error that asks to choose one.
std::string MicList(const std::vector<MicCandidate>& mics) {
  std::string list;
  for (size_t i = 0; i < mics.size(); ++i) list += std::format("\n  {}", DescribeMic(mics[i], i + 1));
  return list;
}

// The mics whose name is `name`, ignoring ASCII case.
std::vector<size_t> MicsNamed(const std::vector<MicCandidate>& mics, std::string_view name) {
  const std::string needle = AsciiLower(name);
  std::vector<size_t> matches;
  for (size_t i = 0; i < mics.size(); ++i) {
    if (AsciiLower(mics[i].name) == needle) matches.push_back(i);
  }
  return matches;
}

// What an empty pick picks: the only mic.
Result<size_t> OnlyMic(const std::vector<MicCandidate>& mics) {
  if (mics.empty()) return Error{"The scene collection has no mic (win-wasapi input) sources."};
  if (mics.size() == 1) return size_t{0};
  return Error{std::format("The scene collection has {} mics. Choose one:{}", mics.size(), MicList(mics))};
}

}  // namespace

Result<size_t> PickMic(const std::vector<MicCandidate>& mics, std::string_view query) {
  if (mics.empty() || query.empty()) return OnlyMic(mics);
  size_t number = 0;
  const auto [end, ec] = std::from_chars(query.data(), query.data() + query.size(), number);
  if (ec == std::errc() && end == query.data() + query.size()) {
    if (number >= 1 && number <= mics.size()) return number - 1;
    return Error{std::format("There's no mic {}. Choose one:{}", number, MicList(mics))};
  }
  const std::vector<size_t> matches = MicsNamed(mics, query);
  if (matches.size() == 1) return matches.front();
  return Error{std::format("{} named \"{}\". Choose by number:{}",
                           matches.empty() ? "There's no mic" : "Several mics are", query, MicList(mics))};
}

Result<size_t> PickMicByName(const std::vector<MicCandidate>& mics, std::string_view name) {
  if (mics.empty() || name.empty()) return OnlyMic(mics);
  for (size_t i = 0; i < mics.size(); ++i) {
    if (mics[i].name == name) return i;
  }
  const std::vector<size_t> matches = MicsNamed(mics, name);
  if (matches.size() == 1) return matches.front();
  return Error{std::format("{} named \"{}\". Choose one:{}", matches.empty() ? "There's no mic" : "Several mics are",
                           name, MicList(mics))};
}

std::string DescribeMic(const MicCandidate& mic, size_t number) {
  return std::format("{}. \"{}\" ({}{})", number, mic.name, mic.location, mic.monitored ? ", monitored" : "");
}

size_t ImportedMic::warning_count() const {
  return static_cast<size_t>(std::count_if(notes.begin(), notes.end(), [](const ImportNote& n) { return n.warning; }));
}

Result<std::unique_ptr<SceneCollection>> SceneCollection::Read(const runtime::ObsApi& api, const fs::path& file) {
  bool from_backup = false;
  obs_data_t* data = ParseCollection(api, file, &from_backup);
  if (!data) return Error{std::format("Couldn't read the scene collection {}, or its backup.", ToUtf8(file))};
  std::unique_ptr<SceneCollection> collection(new SceneCollection(api, data));
  collection->file_ = from_backup ? fs::path(file) += L".bak" : file;
  collection->from_backup_ = from_backup;
  return collection;
}

SceneCollection::~SceneCollection() { api_.obs_data_release(data_); }

std::string SceneCollection::name() const { return String(api_, data_, "name"); }

std::vector<MicCandidate> SceneCollection::Mics(const ModuleErrors& modules) const {
  std::vector<MicCandidate> mics;
  for (const char* key : kGlobalDeviceKeys) {
    DataPtr source = Own(api_, api_.obs_data_get_obj(data_, key));
    if (!source) continue;
    if (auto mic = AsMic(api_, source.get(), modules)) {
      mic->location = key;
      mic->origin = MicOrigin::kGlobalDevice;
      mic->key = key;
      mics.push_back(std::move(*mic));
    }
  }
  ArrayPtr sources = Own(api_, api_.obs_data_get_array(data_, "sources"));
  const size_t count = sources ? api_.obs_data_array_count(sources.get()) : 0;
  for (size_t i = 0; i < count; ++i) {
    DataPtr source = Own(api_, api_.obs_data_array_item(sources.get(), i));
    if (auto mic = AsMic(api_, source.get(), modules)) {
      mic->location = std::format("sources[{}]", i);
      mic->origin = MicOrigin::kSource;
      mic->key = "sources";
      mic->index = i;
      mics.push_back(std::move(*mic));
    }
  }
  return mics;
}

std::vector<std::string> SceneCollection::NeededModules(const MicCandidate& mic) const {
  std::set<std::string> needed;
  DataPtr saved = SavedMic(api_, data_, mic);
  if (saved && String(api_, saved.get(), "id") == kMicSourceId) AddNeededModules(api_, saved.get(), needed);
  return {needed.begin(), needed.end()};
}

Result<ImportedMic> SceneCollection::Import(const MicCandidate& mic, const ModuleErrors& modules) const {
  DataPtr saved = SavedMic(api_, data_, mic);
  if (!saved || !AsMic(api_, saved.get(), modules)) {
    return Error{std::format("The scene collection has no mic at {}.", mic.location)};
  }
  // A copy, so the collection stays as read.
  const char* saved_json = api_.obs_data_get_json(saved.get());
  DataPtr source = Own(api_, saved_json ? api_.obs_data_create_from_json(saved_json) : nullptr);
  if (!source) return Error{std::format("libobs couldn't copy the mic at {}.", mic.location)};

  ImportedMic imported;
  imported.mic = mic;
  ArrayPtr filters = Own(api_, api_.obs_data_get_array(source.get(), "filters"));
  if (filters) {
    ArrayPtr kept = Own(api_, api_.obs_data_array_create());
    const size_t count = api_.obs_data_array_count(filters.get());
    bool changed = false;
    for (size_t i = 0; i < count; ++i) {
      DataPtr filter = Own(api_, api_.obs_data_array_item(filters.get(), i));
      DataPtr instead;
      const bool keep = CheckFilter(api_, filter.get(), modules, imported.notes, instead);
      changed |= !keep || instead;
      if (!keep) continue;
      api_.obs_data_array_push_back(kept.get(), instead ? instead.get() : filter.get());
      if (Runs(api_, filter.get(), modules)) imported.filters.push_back(String(api_, filter.get(), "name"));
    }
    if (changed) api_.obs_data_set_array(source.get(), "filters", kept.get());
  }
  CheckSourceState(api_, source.get(), mic, imported.notes);
  const char* json = api_.obs_data_get_json(source.get());
  if (!json) return Error{std::format("libobs couldn't save the mic at {}.", mic.location)};
  imported.source_json = json;
  for (const char* key : kKeysTheCableIgnores) api_.obs_data_erase(source.get(), key);
  ArrayPtr key_filters = Own(api_, api_.obs_data_get_array(source.get(), "filters"));
  const size_t key_filter_count = key_filters ? api_.obs_data_array_count(key_filters.get()) : 0;
  for (size_t i = 0; i < key_filter_count; ++i) {
    DataPtr filter = Own(api_, api_.obs_data_array_item(key_filters.get(), i));
    for (const char* key : kFilterKeysTheCableIgnores) api_.obs_data_erase(filter.get(), key);
    if (String(api_, filter.get(), "id") == kVstFilterId) {
      const std::string plugin = VstPluginPath(api_, filter.get());
      if (!plugin.empty()) imported.vst_plugins.emplace_back(FromUtf8(plugin));
      // Every VST filter loads with its window off (audio::LoadSourceJson).
      DataPtr settings = Own(api_, api_.obs_data_get_obj(filter.get(), "settings"));
      if (settings) api_.obs_data_erase(settings.get(), kVstOpenWhenActive);
      api_.obs_data_set_string(filter.get(), "knobs_plugin", VstPluginState(VstPluginPath(api_, filter.get())).c_str());
    }
  }
  const char* key_json = api_.obs_data_get_json(source.get());
  if (!key_json) return Error{std::format("libobs couldn't save the mic at {}.", mic.location)};
  imported.chain_key = key_json;
  return imported;
}

std::vector<std::string> NeededModulesForSource(const runtime::ObsApi& api, std::string_view source_json) {
  const std::string json(source_json);
  DataPtr source = Own(api, api.obs_data_create_from_json(json.c_str()));
  std::set<std::string> needed;
  if (source) AddNeededModules(api, source.get(), needed);
  return {needed.begin(), needed.end()};
}

bool NvidiaAudioEffectsInstalled(const runtime::ObsApi& api) {
  return !FindNvidiaAudioEffects(runtime::ObsVersion::FromLibobs(api.obs_get_version())).empty();
}

fs::path NvidiaAudioEffectsFolder(const runtime::ObsVersion& obs) {
  // As nv-filters reads them (nvafx_get_sdk_path): in the system's code page,
  // into MAX_PATH bytes, so a name it can't hold is one it doesn't find.
  const auto variable = [](const char* name) {
    char value[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 1) return fs::path();
    wchar_t wide[MAX_PATH] = {};
    const int converted = MultiByteToWideChar(CP_ACP, 0, value, static_cast<int>(length), wide, MAX_PATH);
    return converted > 0 ? fs::path(std::wstring(wide, converted)) : fs::path();
  };
  fs::path dir = variable("NVAFX_SDK_DIR");
  if (dir.empty() && obs >= runtime::ObsVersion{33, 0, 0}) dir = variable("AFX_SDK_DIR");
  if (dir.empty()) {
    const fs::path program_files = variable("ProgramFiles");
    if (!program_files.empty()) dir = program_files / L"NVIDIA Corporation" / L"NVIDIA Audio Effects SDK";
  }
  return dir;
}

ModuleErrors LoadModules(runtime::ObsSession& session, const std::vector<std::string>& modules) {
  ModuleErrors errors;
  for (const std::string& module : modules) {
    if (const Status loaded = session.LoadOptionalModule(module); !loaded) errors.emplace(module, loaded.error());
  }
  return errors;
}

Result<std::unique_ptr<SceneCollection>> ReadActiveCollection(const runtime::ObsApi& api,
                                                              const ActiveObsConfig& config) {
  // The search stops at the match, so the last file parsed is the one it
  // returns, unless that matched by its file name without parsing.
  std::unique_ptr<SceneCollection> collection;
  fs::path parsed;
  auto file = FindSceneCollectionFile(config, [&](const fs::path& path) -> std::string {
    auto read = SceneCollection::Read(api, path);
    parsed = path;
    collection = read ? std::move(*read) : nullptr;
    return collection ? collection->name() : "";
  });
  if (!file) return Error{file.error()};
  if (collection && parsed == *file) return collection;
  return SceneCollection::Read(api, *file);
}

}  // namespace knobs::import
