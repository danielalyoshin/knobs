// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-test-vst: a tiny VST 2.4 effect, built from source, for the tests of
// OBS's VST filter (obs-vst), so they need no third-party plugin.
//
// Stereo in and out, two parameters, one program. Each channel runs
//
//   s += k * (x - s);  out = s * gain;
//
// with gain = 2 * Gain (default 0.5: unity) and k = 1 - 0.99 * Smooth
// (default 0: no smoothing), in float math, so the output is the same on
// every run. It has no chunks, so obs-vst saves its state as base64 of the
// raw float parameters.
//
// It says it has an editor, so the tests can ask obs-vst to open it, but the
// editor opcodes do nothing. If KNOBS_TEST_VST_LOG names a file, the plugin
// appends a line to it for each of these, with the calling thread's id:
//
//   open <tid>                 effOpen
//   close <tid>                effClose, on the thread effOpen ran on
//   close-wrong-thread <tid>   effClose, on another thread
//   editor <tid>               any editor opcode
//
// effOpen also creates a hidden message-only window on the calling thread,
// as many real plugins do, and effClose destroys it, which only works on the
// thread that created it.

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

// The VST 2.4 ABI, as much of it as this plugin uses.

constexpr int32_t FourCC(char a, char b, char c, char d) {
  return (static_cast<int32_t>(a) << 24) | (static_cast<int32_t>(b) << 16) | (static_cast<int32_t>(c) << 8) |
         static_cast<int32_t>(d);
}

struct AEffect;
using HostCallback = intptr_t (*)(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr,
                                  float opt);
using DispatcherFn = intptr_t (*)(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr,
                                  float opt);
using ProcessFn = void (*)(AEffect* effect, float** inputs, float** outputs, int32_t frames);
using ProcessDoubleFn = void (*)(AEffect* effect, double** inputs, double** outputs, int32_t frames);
using SetParameterFn = void (*)(AEffect* effect, int32_t index, float value);
using GetParameterFn = float (*)(AEffect* effect, int32_t index);

// What the plugin hands the host. The host sets `user`; `object` is the
// plugin's own.
struct AEffect {
  int32_t magic;
  DispatcherFn dispatcher;
  ProcessFn process;  // Accumulates into the outputs. Deprecated in 2.4.
  SetParameterFn set_parameter;
  GetParameterFn get_parameter;
  int32_t num_programs;
  int32_t num_params;
  int32_t num_inputs;
  int32_t num_outputs;
  int32_t flags;
  intptr_t reserved1;
  intptr_t reserved2;
  int32_t initial_delay;
  int32_t real_qualities;  // Unused.
  int32_t off_qualities;   // Unused.
  float io_ratio;          // Unused.
  void* object;
  void* user;
  int32_t unique_id;
  int32_t version;
  ProcessFn process_replacing;
  ProcessDoubleFn process_double_replacing;
  char future[56];
};
static_assert(offsetof(AEffect, dispatcher) == 8);
static_assert(offsetof(AEffect, flags) == 56);
static_assert(offsetof(AEffect, object) == 96);
static_assert(offsetof(AEffect, user) == 104);
static_assert(offsetof(AEffect, unique_id) == 112);
static_assert(offsetof(AEffect, process_replacing) == 120);

// The editor's size, for effEditGetRect.
struct ERect {
  int16_t top, left, bottom, right;
};

constexpr int32_t kEffectMagic = FourCC('V', 's', 't', 'P');

constexpr int32_t kFlagsHasEditor = 1 << 0;
constexpr int32_t kFlagsCanReplacing = 1 << 4;

constexpr int32_t kAudioMasterVersion = 1;

constexpr int32_t kEffOpen = 0;
constexpr int32_t kEffClose = 1;
constexpr int32_t kEffSetProgram = 2;
constexpr int32_t kEffGetProgram = 3;
constexpr int32_t kEffGetProgramName = 5;
constexpr int32_t kEffGetParamLabel = 6;
constexpr int32_t kEffGetParamDisplay = 7;
constexpr int32_t kEffGetParamName = 8;
constexpr int32_t kEffSetSampleRate = 10;
constexpr int32_t kEffSetBlockSize = 11;
constexpr int32_t kEffMainsChanged = 12;
constexpr int32_t kEffEditGetRect = 13;
constexpr int32_t kEffEditOpen = 14;
constexpr int32_t kEffEditClose = 15;
constexpr int32_t kEffEditIdle = 19;
constexpr int32_t kEffIdentify = 22;
constexpr int32_t kEffCanBeAutomated = 26;
constexpr int32_t kEffGetProgramNameIndexed = 29;
constexpr int32_t kEffGetPlugCategory = 35;
constexpr int32_t kEffGetEffectName = 45;
constexpr int32_t kEffGetVendorString = 47;
constexpr int32_t kEffGetProductString = 48;
constexpr int32_t kEffGetVendorVersion = 49;
constexpr int32_t kEffGetVstVersion = 58;

constexpr intptr_t kPlugCategEffect = 1;

// Buffer sizes the host gives for strings, terminator included.
constexpr size_t kMaxParamStrLen = 8;
constexpr size_t kMaxProgNameLen = 24;
constexpr size_t kMaxEffectNameLen = 32;
constexpr size_t kMaxVendorStrLen = 64;

// The plugin.

constexpr int kChannels = 2;
constexpr int kGain = 0;
constexpr int kSmooth = 1;
constexpr int kParams = 2;
constexpr float kDefaults[kParams] = {0.5f, 0.0f};
constexpr const char* kParamNames[kParams] = {"Gain", "Smooth"};
constexpr wchar_t kWindowClass[] = L"knobs-test-vst";
constexpr ERect kEditorRect = {0, 0, 200, 320};

struct Plugin {
  AEffect effect{};
  float params[kParams] = {kDefaults[kGain], kDefaults[kSmooth]};
  float state[kChannels] = {};
  bool opened = false;
  DWORD open_thread = 0;
  HWND window = nullptr;
};

std::atomic<int> g_live_instances{0};
std::mutex g_log_mutex;
std::mutex g_window_mutex;

Plugin& Get(AEffect* effect) { return *static_cast<Plugin*>(effect->object); }

// Appends "<event> <thread id>" to KNOBS_TEST_VST_LOG's file, if it's set.
// The file is opened for each line, so it's whole whenever the host stops.
void Log(std::string_view event) {
  wchar_t path[MAX_PATH * 4];
  const DWORD length = GetEnvironmentVariableW(L"KNOBS_TEST_VST_LOG", path, static_cast<DWORD>(std::size(path)));
  if (length == 0 || length >= std::size(path)) return;
  const std::string line = std::format("{} {}\n", event, GetCurrentThreadId());
  std::lock_guard lock(g_log_mutex);
  const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  CloseHandle(file);
}

HINSTANCE Module() { return reinterpret_cast<HINSTANCE>(&__ImageBase); }

// The class's window procedure is DefWindowProcW, in user32, so a window
// that outlives the DLL, such as one effClose couldn't destroy, never calls
// into unloaded code. A class left registered by an earlier load of the DLL
// is as good as a new one.
HWND CreateHiddenWindow() {
  std::lock_guard lock(g_window_mutex);
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = Module();
  window_class.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
  return CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, Module(), nullptr);
}

// Fails on any thread but the one that created the window. The class goes
// with the last window, so the DLL can unload without leaving it behind.
bool DestroyHiddenWindow(HWND window) {
  std::lock_guard lock(g_window_mutex);
  if (!DestroyWindow(window)) return false;
  UnregisterClassW(kWindowClass, Module());  // Fails while other instances have windows.
  return true;
}

void CopyString(void* ptr, std::string_view text, size_t size) {
  if (!ptr) return;
  auto* out = static_cast<char*>(ptr);
  const size_t n = text.size() < size - 1 ? text.size() : size - 1;
  text.copy(out, n);
  out[n] = '\0';
}

void ParamDisplay(const Plugin& plugin, int32_t index, void* ptr) {
  char text[kMaxParamStrLen];
  if (index == kGain) {
    const float gain = 2.0f * plugin.params[kGain];
    if (gain > 0.0f) {
      std::snprintf(text, sizeof(text), "%.1f", 20.0 * std::log10(gain));
    } else {
      std::snprintf(text, sizeof(text), "-inf");
    }
  } else {
    std::snprintf(text, sizeof(text), "%.0f", 100.0 * plugin.params[kSmooth]);
  }
  CopyString(ptr, text, kMaxParamStrLen);
}

bool IsEditorOpcode(int32_t opcode) {
  return opcode == kEffEditGetRect || opcode == kEffEditOpen || opcode == kEffEditClose || opcode == kEffEditIdle;
}

intptr_t Dispatcher(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float /*opt*/) {
  Plugin& plugin = Get(effect);
  if (IsEditorOpcode(opcode)) Log("editor");
  switch (opcode) {
    case kEffOpen:
      if (!plugin.opened) {
        plugin.opened = true;
        plugin.open_thread = GetCurrentThreadId();
        plugin.window = CreateHiddenWindow();
        ++g_live_instances;
        Log("open");
      }
      return 0;
    case kEffClose: {
      // With no window, the thread ids tell what DestroyWindow would have.
      if (plugin.opened) {
        const bool same_thread =
            plugin.window ? DestroyHiddenWindow(plugin.window) : GetCurrentThreadId() == plugin.open_thread;
        --g_live_instances;
        Log(same_thread ? "close" : "close-wrong-thread");
      }
      // A VST 2 plugin deletes itself on effClose.
      delete &plugin;
      return 0;
    }
    case kEffMainsChanged:
      // Turned on: start from silence.
      if (value) {
        for (float& s : plugin.state) s = 0.0f;
      }
      return 0;
    case kEffSetSampleRate:
    case kEffSetBlockSize:
    case kEffSetProgram:
      return 0;  // The DSP doesn't depend on them, and there's one program.
    case kEffGetProgram:
      return 0;
    case kEffGetProgramName:
      CopyString(ptr, "Default", kMaxProgNameLen);
      return 0;
    case kEffGetProgramNameIndexed:
      if (index != 0) return 0;
      CopyString(ptr, "Default", kMaxProgNameLen);
      return 1;
    case kEffGetParamName:
    case kEffGetParamLabel:
    case kEffGetParamDisplay:
      if (index < 0 || index >= kParams) return 0;
      if (opcode == kEffGetParamName) {
        CopyString(ptr, kParamNames[index], kMaxParamStrLen);
      } else if (opcode == kEffGetParamLabel) {
        CopyString(ptr, index == kGain ? "dB" : "%", kMaxParamStrLen);
      } else {
        ParamDisplay(plugin, index, ptr);
      }
      return 0;
    case kEffCanBeAutomated:
      return index >= 0 && index < kParams ? 1 : 0;
    case kEffEditGetRect:
      if (!ptr) return 0;
      *static_cast<const ERect**>(ptr) = &kEditorRect;
      return 1;
    case kEffEditOpen:
    case kEffEditClose:
    case kEffEditIdle:
      return 0;
    case kEffIdentify:
      return FourCC('N', 'v', 'E', 'f');
    case kEffGetPlugCategory:
      return kPlugCategEffect;
    case kEffGetEffectName:
      CopyString(ptr, "knobs test", kMaxEffectNameLen);
      return 1;
    case kEffGetProductString:
      CopyString(ptr, "knobs test", kMaxVendorStrLen);
      return 1;
    case kEffGetVendorString:
      CopyString(ptr, "knobs", kMaxVendorStrLen);
      return 1;
    case kEffGetVendorVersion:
      return 1;
    case kEffGetVstVersion:
      return 2400;
    default:
      return 0;
  }
}

void SetParameter(AEffect* effect, int32_t index, float value) {
  if (index < 0 || index >= kParams) return;
  Get(effect).params[index] = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

float GetParameter(AEffect* effect, int32_t index) {
  return index >= 0 && index < kParams ? Get(effect).params[index] : 0.0f;
}

// Each sample is read before its output is written, so an input buffer may
// also be the output.
template <bool kAccumulate>
void Process(AEffect* effect, float** inputs, float** outputs, int32_t frames) {
  Plugin& plugin = Get(effect);
  const float gain = 2.0f * plugin.params[kGain];
  const float k = 1.0f - 0.99f * plugin.params[kSmooth];
  for (int c = 0; c < kChannels; ++c) {
    const float* in = inputs[c];
    float* out = outputs[c];
    float s = plugin.state[c];
    for (int32_t i = 0; i < frames; ++i) {
      const float x = in[i];
      s += k * (x - s);
      if constexpr (kAccumulate) {
        out[i] += s * gain;
      } else {
        out[i] = s * gain;
      }
    }
    plugin.state[c] = s;
  }
}

}  // namespace

// The entry point obs-vst looks for. Returns a new instance each call.
extern "C" __declspec(dllexport) AEffect* VSTPluginMain(HostCallback host) {
  if (!host || !host(nullptr, kAudioMasterVersion, 0, 0, nullptr, 0.0f)) return nullptr;
  auto* plugin = new Plugin;
  AEffect& effect = plugin->effect;
  effect.magic = kEffectMagic;
  effect.dispatcher = Dispatcher;
  effect.process = Process<true>;
  effect.set_parameter = SetParameter;
  effect.get_parameter = GetParameter;
  effect.num_programs = 1;
  effect.num_params = kParams;
  effect.num_inputs = kChannels;
  effect.num_outputs = kChannels;
  effect.flags = kFlagsCanReplacing | kFlagsHasEditor;
  effect.io_ratio = 1.0f;
  effect.object = plugin;
  effect.unique_id = FourCC('k', 'T', 's', 't');
  effect.version = 1;
  effect.process_replacing = Process<false>;
  return &effect;
}

// Instances opened and not yet closed, for debugging.
extern "C" __declspec(dllexport) int KnobsTestVstLiveInstances() { return g_live_instances.load(); }
