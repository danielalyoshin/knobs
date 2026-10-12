// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/device_watch.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <format>
#include <iterator>
#include <mutex>
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

// The endpoints in `states`, such as DEVICE_STATE_ACTIVE.
std::vector<AudioDevice> Devices(IMMDeviceEnumerator* enumerator, EDataFlow flow, DWORD states) {
  std::vector<AudioDevice> devices;
  ComPtr<IMMDeviceCollection> collection;
  if (FAILED(enumerator->EnumAudioEndpoints(flow, states, &collection))) return devices;
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
  return devices;
}

// Whether VB-Cable's setup has installed its driver: it adds itself to
// Windows' installed apps as "VBCABLE, The Virtual Audio Cable", in the
// 64-bit registry, which every user can read.
bool VbCableInstalled() {
  HKEY uninstall = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                    KEY_READ | KEY_WOW64_64KEY, &uninstall) != ERROR_SUCCESS) {
    return false;
  }
  bool found = false;
  wchar_t name[256];
  for (DWORD i = 0; !found; ++i) {
    DWORD length = static_cast<DWORD>(std::size(name));
    if (RegEnumKeyExW(uninstall, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    wchar_t display[256] = {};
    DWORD size = sizeof(display);
    if (RegGetValueW(uninstall, name, L"DisplayName", RRF_RT_REG_SZ, nullptr, display, &size) == ERROR_SUCCESS) {
      found = AsciiLower(ToUtf8(std::wstring_view(display))).starts_with("vbcable");
    }
  }
  RegCloseKey(uninstall);
  return found;
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
};

Endpoints ListEndpoints() {
  Endpoints endpoints;
  const ComPtr<IMMDeviceEnumerator> enumerator = CreateEnumerator();
  if (!enumerator) return endpoints;
  endpoints.mics = Devices(enumerator.Get(), eCapture, DEVICE_STATE_ACTIVE);
  endpoints.outputs = Devices(enumerator.Get(), eRender, DEVICE_STATE_ACTIVE);
  endpoints.disabled_outputs = Devices(enumerator.Get(), eRender, DEVICE_STATE_DISABLED);
  endpoints.vb_cable_installed = VbCableInstalled();
  ComPtr<IMMDevice> default_mic;
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &default_mic))) {
    endpoints.default_mic = DeviceId(default_mic.Get());
  }
  return endpoints;
}

Result<std::unique_ptr<DeviceWatch>> DeviceWatch::Start(std::function<void()> on_change) {
  auto state = std::make_unique<State>();
  state->enumerator = CreateEnumerator();
  if (!state->enumerator) return Error{"Couldn't reach Windows' list of audio devices."};
  state->client = new NotificationClient(std::move(on_change));
  const HRESULT hr = state->enumerator->RegisterEndpointNotificationCallback(state->client);
  if (FAILED(hr)) {
    state->client->Release();
    return Error{std::format("Couldn't listen for audio devices coming and going (0x{:08X}).", static_cast<uint32_t>(hr))};
  }
  return std::unique_ptr<DeviceWatch>(new DeviceWatch(std::move(state)));
}

DeviceWatch::DeviceWatch(std::unique_ptr<State> state) : state_(std::move(state)) {}

DeviceWatch::~DeviceWatch() {
  state_->enumerator->UnregisterEndpointNotificationCallback(state_->client);
  // A notification already running finishes first: Disconnect waits for it.
  state_->client->Disconnect();
  state_->client->Release();
}

}  // namespace knobs::audio
