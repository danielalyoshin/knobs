// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/obs_session.h"

#include <windows.h>

#include <algorithm>
#include <cassert>
#include <format>

#include "app_info.h"
#include "runtime/obs_layout.h"
#include "util/win_strings.h"

namespace knobs::runtime {
namespace {

// Only affects module display strings, which knobs doesn't show.
constexpr char kLocale[] = "en-US";

std::string_view DescribeModuleError(int code) {
  switch (code) {
    case MODULE_FAILED_TO_OPEN:
      return "its DLL couldn't be loaded";
    case MODULE_MISSING_EXPORTS:
      return "it doesn't export the OBS module functions";
    case MODULE_INCOMPATIBLE_VER:
      return "it was built for a newer libobs";
    case MODULE_HARDCODED_SKIP:
      return "libobs refuses to load it";
    default:
      return "unknown error";
  }
}

}  // namespace

Result<std::unique_ptr<ObsSession>> ObsSession::Start(const ObsRuntime& runtime,
                                                      const SessionOptions& options) {
  const ObsApi& api = runtime.api();
  std::error_code ec;
  std::filesystem::create_directories(options.module_config_dir, ec);
  if (ec) {
    return Error{std::format("Couldn't create {}.", ToUtf8(options.module_config_dir))};
  }

  std::unique_ptr<ObsSession> session(new ObsSession(runtime, options));
  if (!api.obs_startup(kLocale, ToObsPath(options.module_config_dir).c_str(), nullptr)) {
    return Error{"libobs failed to start (obs_startup). The log has details."};
  }
  session->running_ = true;

  const obs_audio_info audio = {options.samples_per_sec, options.speakers};
  if (!api.obs_reset_audio(&audio)) {
    return Error{std::format("libobs rejected the audio format ({} Hz, speaker layout {}).",
                             audio.samples_per_sec, static_cast<int>(audio.speakers))};
  }
  session->audio_running_ = true;

  if (options.video == VideoMode::kDummy) {
    const std::string graphics_module(kGraphicsModule);
    obs_video_info video = {};
    video.graphics_module = graphics_module.c_str();
    video.fps_num = 1;
    video.fps_den = 1;
    video.base_width = video.output_width = 8;
    video.base_height = video.output_height = 8;
    video.output_format = VIDEO_FORMAT_NV12;
    video.gpu_conversion = true;
    video.colorspace = VIDEO_CS_709;
    video.range = VIDEO_RANGE_PARTIAL;
    video.scale_type = OBS_SCALE_BICUBIC;
    const int result = api.obs_reset_video(&video);
    if (result != OBS_VIDEO_SUCCESS) {
      return Error{std::format("Couldn't start the dummy video canvas (obs_reset_video error {}).",
                               result)};
    }
    session->video_running_ = true;
  }

  for (std::string_view name : kObsModules) {
    const std::string binary = ToObsPath(PluginDll(runtime.root(), name, runtime.install_version()));
    const std::string data = ToObsPath(PluginDataDir(runtime.root(), name, runtime.install_version()));
    obs_module_t* module = nullptr;
    const int code = api.obs_open_module(&module, binary.c_str(), data.c_str());
    if (code != MODULE_SUCCESS) {
      return Error{std::format("Couldn't open the OBS module {}: {}.", name, DescribeModuleError(code))};
    }
    if (!api.obs_init_module(module)) {
      return Error{std::format("The OBS module {} failed to initialize. The log has details.", name)};
    }
    session->modules_.emplace_back(name, module);
  }
  api.obs_post_load_modules();
  return session;
}

ObsSession::ObsSession(const ObsRuntime& runtime, const SessionOptions& options)
    : runtime_(runtime), options_(options), thread_id_(GetCurrentThreadId()) {}

ObsSession::~ObsSession() { Shutdown(); }

obs_module_t* ObsSession::module(std::string_view name) const {
  for (const auto& [module_name, module] : modules_) {
    if (module_name == name) return module;
  }
  return nullptr;
}

Status ObsSession::LoadOptionalModule(std::string_view name) {
  assert(GetCurrentThreadId() == thread_id_ && "modules load on the obs_startup() thread");
  if (module(name)) return Ok{};
  if (const auto failed = module_errors_.find(name); failed != module_errors_.end()) return Error{failed->second};
  if (!running_) return Error{"libobs isn't running."};
  const auto known = std::find_if(kOptionalObsModules.begin(), kOptionalObsModules.end(),
                                  [&](const OptionalObsModule& module) { return module.name == name; });
  if (known == kOptionalObsModules.end()) {
    return Error{std::format("{} doesn't load the OBS module {}.", kDisplayName, name)};
  }

  const ObsApi& api = runtime_.api();
  const auto open = [&]() -> Status {
    const std::filesystem::path dll = PluginDll(runtime_.root(), name, runtime_.install_version());
    std::error_code ec;
    if (!std::filesystem::is_regular_file(dll, ec)) {
      // The install lacks it, or its files couldn't be read when the copy
      // was made (RuntimeFileSet::skipped), which the log says.
      return Error{std::format("{}'s copy of OBS has no {} module.", kDisplayName, name)};
    }
    const std::string binary = ToObsPath(dll);
    const std::string data = ToObsPath(PluginDataDir(runtime_.root(), name, runtime_.install_version()));
    obs_module_t* module = nullptr;
    const int code = api.obs_open_module(&module, binary.c_str(), data.c_str());
    if (code != MODULE_SUCCESS) {
      return Error{std::format("Couldn't open the OBS module {}: {}.", name, DescribeModuleError(code))};
    }
    // Before any of its filters exist, so their objects belong to this
    // thread's application.
    if (name == "obs-vst" && !qt_) {
      auto qt = QtApp::Start();
      if (!qt) return Error{std::format("Couldn't start the Qt application obs-vst needs: {}", qt.error())};
      qt_ = std::move(*qt);
    }
    if (!api.obs_init_module(module)) {
      return Error{std::format("The OBS module {} failed to initialize. The log has details.", name)};
    }
    // Neither module has a post_load callback, which obs_post_load_modules()
    // would run, for every module again.
    modules_.emplace_back(name, module);
    return Ok{};
  };
  Status opened = open();
  if (!opened) module_errors_.emplace(name, opened.error());
  return opened;
}

void ObsSession::DrainDestroyQueue() {
  const ObsApi& api = runtime_.api();
  constexpr obs_task_t kNoop = [](void*) {};
  // Each thread runs queued tasks after releasing that tick's source
  // references (obs-video.c, obs-audio.c). Only wait on threads that exist, or
  // the wait never ends.
  if (video_running_) api.obs_queue_task(OBS_TASK_GRAPHICS, kNoop, nullptr, true);
  if (audio_running_) api.obs_queue_task(OBS_TASK_AUDIO, kNoop, nullptr, true);
  // Runs after every destroy task queued before it.
  api.obs_queue_task(OBS_TASK_DESTROY, kNoop, nullptr, true);
  if (qt_) qt_->DeliverPostedEvents();
}

long ObsSession::Shutdown() {
  const ObsApi& api = runtime_.api();
  if (running_) {
    assert(GetCurrentThreadId() == thread_id_ && "obs_shutdown() must run on the obs_startup() thread");
    DrainDestroyQueue();
    api.obs_shutdown();
    running_ = false;
    audio_running_ = false;
    video_running_ = false;
    modules_.clear();
    // What obs_shutdown() destroyed may have posted deletes too, which free
    // libobs allocations.
    qt_.reset();
  }
  return api.bnum_allocs();
}

}  // namespace knobs::runtime
