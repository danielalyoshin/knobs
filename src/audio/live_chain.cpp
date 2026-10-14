// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/live_chain.h"

#include <format>

#include "runtime/obs_layout.h"
#include "util/json.h"

namespace knobs::audio {
namespace {

std::string OrEmpty(const char* text) { return text ? text : ""; }

// OBS 33 only turns monitoring on or off, and warns in the log on every call
// to obs_source_set_monitoring_type. 32.2 has no
// obs_source_set_monitoring_enabled, and its Monitor Only keeps the source
// out of libobs's output buffers (docs/design.md, OBS 33.0).
void SetMonitoring(const runtime::ObsApi& api, obs_source_t* source, bool on) {
  if (api.obs_source_set_monitoring_enabled) {
    api.obs_source_set_monitoring_enabled(source, on);
  } else {
    api.obs_source_set_monitoring_type(source, on ? OBS_MONITORING_TYPE_MONITOR_ONLY : OBS_MONITORING_TYPE_NONE);
  }
}

// Turns obs-vst's "Open interface when active" off for every VST filter in
// a source object, which would otherwise abort the process as it loads
// (runtime::kVstOpenWhenActive). A VST filter is one whose id without its
// version is obs-vst's, as pre-flight tells them, whichever version of it the
// loader loads (obs_load_source_type). The loader loads a filter's own
// filters too. Nested objects are shared, so this changes `source` itself.
void CloseVstWindows(const runtime::ObsApi& api, obs_data_t* source) {
  obs_data_array_t* filters = api.obs_data_get_array(source, "filters");
  const size_t count = filters ? api.obs_data_array_count(filters) : 0;
  for (size_t i = 0; i < count; ++i) {
    obs_data_t* filter = api.obs_data_array_item(filters, i);
    const std::string id = OrEmpty(api.obs_data_get_string(filter, "id"));
    const std::string versioned_id = OrEmpty(api.obs_data_get_string(filter, "versioned_id"));
    if (id == "vst_filter" || versioned_id.starts_with("vst_filter")) {
      obs_data_t* settings = api.obs_data_get_obj(filter, "settings");
      if (settings) {
        api.obs_data_set_bool(settings, runtime::kVstOpenWhenActive, false);
        api.obs_data_release(settings);
      }
    }
    CloseVstWindows(api, filter);
    api.obs_data_release(filter);
  }
  if (filters) api.obs_data_array_release(filters);
}

}  // namespace

std::string MicWithGainSourceJson(std::string_view device_id, double gain_db) {
  return std::format(
      R"({{"id": "wasapi_input_capture", "versioned_id": "wasapi_input_capture", "name": "Mic", )"
      R"("settings": {{"device_id": {}}}, )"
      R"("filters": [{{"id": "gain_filter", "versioned_id": "gain_filter", "name": "Gain", )"
      R"("settings": {{"db": {}}}}}]}})",
      JsonQuote(device_id), gain_db);
}

Result<obs_source_t*> LoadSourceJson(const runtime::ObsApi& api, std::string_view json,
                                     const LoadOptions& options) {
  obs_data_t* data = api.obs_data_create_from_json(std::string(json).c_str());
  if (!data) return Error{"The source isn't valid JSON."};
  if (!options.type_id.empty()) {
    const std::string id(options.type_id);
    api.obs_data_set_string(data, "id", id.c_str());
    api.obs_data_set_string(data, "versioned_id", id.c_str());
  }
  // Scene collections save the libobs version that wrote each source, and the
  // loader migrates older ones. Built-in JSON has none, so it loads as
  // current instead of as version 0.
  api.obs_data_set_default_int(data, "prev_ver", api.obs_get_version());
  // A source saved as monitored would otherwise open the current monitoring
  // device during the load. The caller decides where audio goes. OBS 33's
  // loader reads monitoring_enabled instead for sources it saved; 32.2 ignores
  // that key (docs/design.md, OBS 33.0).
  api.obs_data_set_int(data, "monitoring_type", OBS_MONITORING_TYPE_NONE);
  api.obs_data_set_bool(data, "monitoring_enabled", false);
  CloseVstWindows(api, data);
  obs_source_t* source = api.obs_load_private_source(data);
  api.obs_data_release(data);
  if (!source) return Error{"libobs couldn't load the source."};
  // The loader still turns monitoring on for a type that monitors by default
  // if the source was saved before libobs 23.2.2 (obs_load_source_type).
  // Neither win-wasapi's inputs nor the push source do, but turn it off
  // regardless.
  SetMonitoring(api, source, false);

  // An unknown type still gets a source, with no callbacks and no flags.
  if (!(api.obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
    const std::string id = OrEmpty(api.obs_source_get_id(source));
    api.obs_source_release(source);
    return Error{std::format("libobs has no audio source type \"{}\".", id)};
  }
  if (options.load_callbacks) api.obs_source_load2(source);
  return source;
}

ChainInfo DescribeChain(const runtime::ObsApi& api, obs_source_t* source) {
  ChainInfo info;
  info.type = OrEmpty(api.obs_source_get_id(source));
  info.balance = api.obs_source_get_balance_value(source);
  info.mono = (api.obs_source_get_flags(source) & OBS_SOURCE_FLAG_FORCE_MONO) != 0;
  info.volume = api.obs_source_get_volume(source);
  struct Context {
    const runtime::ObsApi& api;
    ChainInfo& info;
  } context{api, info};
  // Enumerates in processing order (obs-source.c, filter_async_audio).
  api.obs_source_enum_filters(
      source,
      [](obs_source_t*, obs_source_t* filter, void* param) {
        auto& [api, info] = *static_cast<Context*>(param);
        // A type libobs doesn't know loads with no flags at all.
        const uint32_t flags = api.obs_source_get_output_flags(filter);
        info.filters.push_back({OrEmpty(api.obs_source_get_id(filter)),
                                OrEmpty(api.obs_source_get_name(filter)), api.obs_source_enabled(filter),
                                flags != 0, (flags & OBS_SOURCE_AUDIO) != 0});
      },
      &context);
  return info;
}

Result<std::unique_ptr<LiveChain>> LiveChain::Start(const runtime::ObsApi& api,
                                                    runtime::ObsSession& session,
                                                    std::string_view source_json,
                                                    const LoadOptions& options) {
  auto source = LoadSourceJson(api, source_json, options);
  if (!source) return Error{source.error()};
  std::unique_ptr<LiveChain> chain(new LiveChain(api, session, *source));
  SetMonitoring(api, *source, true);
  api.obs_source_inc_active(*source);
  return chain;
}

void LiveChain::RestartMonitor() { api_.obs_reset_audio_monitoring(); }

LiveChain::~LiveChain() {
  api_.obs_source_dec_active(source_);
  SetMonitoring(api_, source_, false);
  api_.obs_source_release(source_);
  session_.DrainDestroyQueue();
}

}  // namespace knobs::audio
