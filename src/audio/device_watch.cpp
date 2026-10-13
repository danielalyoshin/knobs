// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/device_watch.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <format>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>

#include "util/win_strings.h"

namespace knobs::audio {
namespace {

using Microsoft::WRL::ComPtr;

// PKEY_Device_FriendlyName, spelled out so this file needn't instantiate
// GUIDs with initguid.h.
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
                                       14};

ComPtr<IMMDeviceEnumerator> CreateEnumerator() {
  ComPtr<IMMDeviceEnumerator> enumerator;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  return enumerator;
}

std::string DeviceId(IMMDevice* device) {
  LPWSTR id = nullptr;
  if (FAILED(device->GetId(&id)) || !id) return "";
  std::string text = ToUtf8(std::wstring_view(id));
  CoTaskMemFree(id);
  return text;
}

std::string FriendlyName(IMMDevice* device) {
  ComPtr<IPropertyStore> properties;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return "";
  PROPVARIANT value;
  PropVariantInit(&value);
  std::string name;
  if (SUCCEEDED(properties->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
    name = ToUtf8(std::wstring_view(value.pwszVal));
  }
  PropVariantClear(&value);
  return name;
}

// The endpoints in `states`, such as DEVICE_STATE_ACTIVE, into `devices`.
// The failure if Windows couldn't list them.
HRESULT Devices(IMMDeviceEnumerator* enumerator, EDataFlow flow, DWORD states, std::vector<AudioDevice>& devices) {
  devices.clear();
  ComPtr<IMMDeviceCollection> collection;
  if (const HRESULT hr = enumerator->EnumAudioEndpoints(flow, states, &collection); FAILED(hr)) return hr;
  UINT count = 0;
  collection->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(i, &device))) continue;
    std::string id = DeviceId(device.Get());
    if (id.empty()) continue;
    std::string name = FriendlyName(device.Get());
    devices.push_back({name.empty() ? id : std::move(name), std::move(id)});
  }
  return S_OK;
}

std::string DefaultDevice(IMMDeviceEnumerator* enumerator, EDataFlow flow, ERole role) {
  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, &device))) return "";
  return DeviceId(device.Get());
}

constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

uint64_t Ticks(const FILETIME& time) {
  return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// Whether VB-Cable's setup has installed its driver since Windows last
// started, when its devices may wait for a restart. It adds itself to
// Windows' installed apps as "VBCABLE, The Virtual Audio Cable", in the
// 64-bit registry, which every user can read. Installed before, a restart
// won't bring its devices back.
bool VbCablePending() {
  HKEY uninstall = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_READ | KEY_WOW64_64KEY, &uninstall) != ERROR_SUCCESS) {
    return false;
  }
  FILETIME now = {};
  GetSystemTimeAsFileTime(&now);
  // In FILETIME's 100 ns.
  const uint64_t started = Ticks(now) - GetTickCount64() * 10'000;
  bool pending = false;
  wchar_t name[256];
  for (DWORD i = 0; !pending; ++i) {
    DWORD length = static_cast<DWORD>(std::size(name));
    if (RegEnumKeyExW(uninstall, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    wchar_t display[256] = {};
    DWORD size = sizeof(display);
    if (RegGetValueW(uninstall, name, L"DisplayName", RRF_RT_REG_SZ, nullptr, display, &size) != ERROR_SUCCESS ||
        !AsciiLower(ToUtf8(std::wstring_view(display))).starts_with("vbcable")) {
      continue;
    }
    HKEY entry = nullptr;
    if (RegOpenKeyExW(uninstall, name, 0, KEY_READ, &entry) != ERROR_SUCCESS) continue;
    FILETIME written = {};
    if (RegQueryInfoKeyW(entry, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr, &written) == ERROR_SUCCESS) {
      pending = Ticks(written) > started;
    }
    RegCloseKey(entry);
  }
  RegCloseKey(uninstall);
  return pending;
}

// Forwards every notification to a callback, until disconnected. Windows
// holds a reference while it's registered, so the object outlives the watch
// if a notification is in flight when the watch ends.
class NotificationClient final : public IMMNotificationClient {
 public:
  explicit NotificationClient(std::function<void()> on_change) : on_change_(std::move(on_change)) {}

  void Disconnect() {
    std::lock_guard lock(mutex_);
    on_change_ = nullptr;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG references = InterlockedDecrement(&references_);
    if (references == 0) delete this;
    return references;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *object = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return Notify(); }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return Notify(); }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return Notify(); }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return Notify(); }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

 private:
  ~NotificationClient() = default;

  HRESULT Notify() {
    std::lock_guard lock(mutex_);
    if (on_change_) on_change_();
    return S_OK;
  }

  LONG references_ = 1;
  std::mutex mutex_;  // Guards on_change_.
  std::function<void()> on_change_;
};

}  // namespace

struct DeviceWatch::State {
  ComPtr<IMMDeviceEnumerator> enumerator;
  NotificationClient* client = nullptr;  // One reference, ours.
  // Installed apps changing, for VB-Cable's driver, whose devices may not
  // appear until a restart (VbCablePending).
  HKEY uninstall = nullptr;
  HANDLE changed = nullptr;
  HANDLE stop = nullptr;
  std::thread apps;
};

namespace {

// Calls `on_change` whenever an installed app is added or changed, until
// `stop` is set.
void WatchInstalledApps(HKEY uninstall, HANDLE changed, HANDLE stop, const std::function<void()>& on_change) {
  for (;;) {
    if (RegNotifyChangeKeyValue(uninstall, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE) !=
        ERROR_SUCCESS) {
      return;
    }
    const HANDLE handles[] = {stop, changed};
    if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return;
    on_change();
  }
}

}  // namespace

Endpoints ListEndpoints() {
  Endpoints endpoints;
  const ComPtr<IMMDeviceEnumerator> enumerator = CreateEnumerator();
  if (!enumerator) {
    endpoints.error = "Couldn't reach Windows' list of audio devices.";
    return endpoints;
  }
  HRESULT hr = Devices(enumerator.Get(), eCapture, DEVICE_STATE_ACTIVE, endpoints.mics);
  if (SUCCEEDED(hr)) hr = Devices(enumerator.Get(), eRender, DEVICE_STATE_ACTIVE, endpoints.outputs);
  if (SUCCEEDED(hr)) hr = Devices(enumerator.Get(), eRender, DEVICE_STATE_DISABLED, endpoints.disabled_outputs);
  if (FAILED(hr)) {
    // Lists that say nothing would read as every device unplugged.
    return {.error = std::format("Couldn't read Windows' list of audio devices (0x{:08X}).", static_cast<uint32_t>(hr))};
  }
  endpoints.vb_cable_pending = VbCablePending();
  endpoints.default_mic = DefaultDevice(enumerator.Get(), eCapture, eCommunications);
  // Sound settings' Output sets the console and multimedia roles together.
  endpoints.default_output = DefaultDevice(enumerator.Get(), eRender, eConsole);
  return endpoints;
}

Status CheckPlaybackDevice(std::string_view id, std::string_view name) {
  const auto failed = [name](HRESULT hr) {
    return Error{hr == AUDCLNT_E_DEVICE_IN_USE
                     ? std::format("\"{}\" is in use by another app on its own (exclusive mode), so nothing else can "
                                   "play into it.",
                                   name)
                     : std::format("\"{}\" can't be opened (0x{:08X}).", name, static_cast<uint32_t>(hr))};
  };
  const ComPtr<IMMDeviceEnumerator> enumerator = CreateEnumerator();
  if (!enumerator) return Error{"Couldn't reach Windows' list of audio devices."};
  ComPtr<IMMDevice> device;
  HRESULT hr = enumerator->GetDevice(FromUtf8(id).c_str(), &device);
  if (FAILED(hr)) return failed(hr);
  ComPtr<IAudioClient> client;
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
  if (FAILED(hr)) return failed(hr);
  WAVEFORMATEX* format = nullptr;
  hr = client->GetMixFormat(&format);
  if (FAILED(hr)) return failed(hr);
  // As wasapi-output.c opens it: shared, with a 1 s buffer, in 100 ns units.
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10'000'000, 0, format, nullptr);
  CoTaskMemFree(format);
  if (FAILED(hr)) return failed(hr);
  return Ok{};
}

Result<std::unique_ptr<DeviceWatch>> DeviceWatch::Start(std::function<void()> on_change) {
  auto state = std::make_unique<State>();
  state->enumerator = CreateEnumerator();
  if (!state->enumerator) return Error{"Couldn't reach Windows' list of audio devices."};
  state->client = new NotificationClient(on_change);
  const HRESULT hr = state->enumerator->RegisterEndpointNotificationCallback(state->client);
  if (FAILED(hr)) {
    state->client->Release();
    return Error{std::format("Couldn't listen for audio devices coming and going (0x{:08X}).", static_cast<uint32_t>(hr))};
  }
  // Without it, VB-Cable installed by hand while knobs runs is only seen
  // with the next change of devices.
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_NOTIFY | KEY_WOW64_64KEY, &state->uninstall) ==
      ERROR_SUCCESS) {
    state->changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    state->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state->changed && state->stop) {
      state->apps = std::thread(WatchInstalledApps, state->uninstall, state->changed, state->stop, std::move(on_change));
    }
  }
  return std::unique_ptr<DeviceWatch>(new DeviceWatch(std::move(state)));
}

DeviceWatch::DeviceWatch(std::unique_ptr<State> state) : state_(std::move(state)) {}

DeviceWatch::~DeviceWatch() {
  state_->enumerator->UnregisterEndpointNotificationCallback(state_->client);
  // A notification already running finishes first: Disconnect waits for it.
  state_->client->Disconnect();
  state_->client->Release();
  if (state_->apps.joinable()) {
    SetEvent(state_->stop);
    state_->apps.join();
  }
  if (state_->stop) CloseHandle(state_->stop);
  if (state_->changed) CloseHandle(state_->changed);
  if (state_->uninstall) RegCloseKey(state_->uninstall);
}

}  // namespace knobs::audio
