// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/setup_uia.h"

#include <oleauto.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <vector>

namespace knobs::tray {

// What the providers share with the window, which outlives neither.
struct UiaBridge {
  SetupUiaHost* host = nullptr;
  // Goes up with each layout: providers made for an older one are stale.
  uint32_t generation = 1;
  IRawElementProviderSimple* root = nullptr;
};

namespace {

constexpr int kRoot = -1;

long ControlType(Role role) {
  switch (role) {
    case Role::kStageList:
    case Role::kStepList:
    case Role::kNoteList:
      return UIA_ListControlTypeId;
    case Role::kStage:
    case Role::kStep:
    case Role::kNote:
      return UIA_ListItemControlTypeId;
    case Role::kLink:
      return UIA_HyperlinkControlTypeId;
    case Role::kDoor:
    case Role::kButton:
      return UIA_ButtonControlTypeId;
    case Role::kChoiceGroup:
      return UIA_GroupControlTypeId;
    case Role::kRadio:
      return UIA_RadioButtonControlTypeId;
    case Role::kPicture:
      return UIA_ImageControlTypeId;
    case Role::kCheckBox:
      return UIA_CheckBoxControlTypeId;
    case Role::kHeading:
    case Role::kText:
    case Role::kChain:
    case Role::kStatus:
      return UIA_TextControlTypeId;
  }
  return UIA_CustomControlTypeId;
}

bool Invokable(Role role) { return role == Role::kLink || role == Role::kDoor || role == Role::kButton; }

void SetString(VARIANT* value, const std::wstring& text) {
  if (text.empty()) return;
  value->vt = VT_BSTR;
  value->bstrVal = SysAllocString(text.c_str());
}

void SetBool(VARIANT* value, bool on) {
  value->vt = VT_BOOL;
  value->boolVal = on ? VARIANT_TRUE : VARIANT_FALSE;
}

void SetInt(VARIANT* value, long number) {
  value->vt = VT_I4;
  value->lVal = number;
}

class Provider final : public IRawElementProviderSimple,
                       public IRawElementProviderFragment,
                       public IRawElementProviderFragmentRoot,
                       public IInvokeProvider,
                       public IToggleProvider,
                       public ISelectionItemProvider {
 public:
  Provider(std::shared_ptr<UiaBridge> bridge, int index)
      : bridge_(std::move(bridge)), index_(index), generation_(bridge_->generation) {}

  // A new provider for `index`, with one reference, or the root's.
  static IRawElementProviderSimple* For(const std::shared_ptr<UiaBridge>& bridge, int index) {
    if (index == kRoot) {
      if (bridge->root) bridge->root->AddRef();
      return bridge->root;
    }
    return new Provider(bridge, index);
  }

  // IUnknown.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    *out = nullptr;
    const Element* element = index_ == kRoot ? nullptr : Live();
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple)) {
      *out = static_cast<IRawElementProviderSimple*>(this);
    } else if (iid == __uuidof(IRawElementProviderFragment)) {
      *out = static_cast<IRawElementProviderFragment*>(this);
    } else if (iid == __uuidof(IRawElementProviderFragmentRoot) && index_ == kRoot) {
      *out = static_cast<IRawElementProviderFragmentRoot*>(this);
    } else if (iid == __uuidof(IInvokeProvider) && element && Invokable(element->role)) {
      *out = static_cast<IInvokeProvider*>(this);
    } else if (iid == __uuidof(IToggleProvider) && element && element->role == Role::kCheckBox) {
      *out = static_cast<IToggleProvider*>(this);
    } else if (iid == __uuidof(ISelectionItemProvider) && element && element->role == Role::kRadio) {
      *out = static_cast<ISelectionItemProvider*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  // IRawElementProviderSimple.
  HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override {
    *options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** out) override {
    *out = nullptr;
    const Element* element = Live();
    if (index_ == kRoot) return S_OK;
    if (!element) return UIA_E_ELEMENTNOTAVAILABLE;
    if ((pattern == UIA_InvokePatternId && Invokable(element->role)) ||
        (pattern == UIA_TogglePatternId && element->role == Role::kCheckBox) ||
        (pattern == UIA_SelectionItemPatternId && element->role == Role::kRadio)) {
      *out = static_cast<IRawElementProviderSimple*>(this);
      AddRef();
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* value) override {
    value->vt = VT_EMPTY;
    if (index_ == kRoot) return S_OK;  // The window's own provider answers.
    const Element* element = Live();
    if (!element) return UIA_E_ELEMENTNOTAVAILABLE;
    SetupUiaHost& host = *bridge_->host;
    switch (property) {
      case UIA_ControlTypePropertyId:
        SetInt(value, ControlType(element->role));
        break;
      case UIA_NamePropertyId:
        SetString(value, element->warning ? L"Warning: " + element->name : element->name);
        break;
      case UIA_HelpTextPropertyId:
        SetString(value, element->help);
        break;
      case UIA_ItemStatusPropertyId:
        SetString(value, element->item_status);
        break;
      case UIA_AccessKeyPropertyId:
        if (element->access_key) SetString(value, std::wstring(L"Alt+") + element->access_key);
        break;
      case UIA_AutomationIdPropertyId:
        if (element->id != 0) SetString(value, std::to_wstring(element->id));
        break;
      case UIA_FrameworkIdPropertyId:
        SetString(value, L"knobs");
        break;
      case UIA_IsEnabledPropertyId:
        SetBool(value, element->enabled);
        break;
      case UIA_IsKeyboardFocusablePropertyId:
        SetBool(value, element->focusable && element->enabled);
        break;
      case UIA_HasKeyboardFocusPropertyId:
        SetBool(value, host.UiaFocus() == index_ && ::GetFocus() == host.UiaWindow());
        break;
      case UIA_IsOffscreenPropertyId:
        SetBool(value, !host.UiaScene().Visible(*element, host.UiaScroll()));
        break;
      case UIA_HeadingLevelPropertyId:
        if (element->role == Role::kHeading) SetInt(value, HeadingLevel1);
        break;
      case UIA_LiveSettingPropertyId:
        if (element->role == Role::kStatus) SetInt(value, Polite);
        break;
      case UIA_IsControlElementPropertyId:
      case UIA_IsContentElementPropertyId:
        SetBool(value, true);
        break;
      default:
        break;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** host) override {
    *host = nullptr;
    if (index_ == kRoot && bridge_->host) return UiaHostProviderFromHwnd(bridge_->host->UiaWindow(), host);
    return S_OK;
  }

  // IRawElementProviderFragment.
  HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override {
    *out = nullptr;
    if (!Alive()) return UIA_E_ELEMENTNOTAVAILABLE;
    const std::vector<Element>& elements = bridge_->host->UiaScene().elements;
    const int parent = index_ == kRoot ? kRoot : elements[static_cast<size_t>(index_)].parent;
    const auto children = [&elements](int of) {
      std::vector<int> found;
      for (size_t i = 0; i < elements.size(); ++i) {
        if (elements[i].parent == of) found.push_back(static_cast<int>(i));
      }
      return found;
    };
    int target = -2;
    switch (direction) {
      case NavigateDirection_Parent:
        if (index_ != kRoot) target = parent;
        break;
      case NavigateDirection_FirstChild:
      case NavigateDirection_LastChild: {
        const std::vector<int> mine = children(index_);
        if (!mine.empty()) target = direction == NavigateDirection_FirstChild ? mine.front() : mine.back();
        break;
      }
      case NavigateDirection_NextSibling:
      case NavigateDirection_PreviousSibling: {
        if (index_ == kRoot) break;
        const std::vector<int> siblings = children(parent);
        const auto at = std::find(siblings.begin(), siblings.end(), index_);
        if (direction == NavigateDirection_NextSibling && at != siblings.end() && at + 1 != siblings.end()) {
          target = *(at + 1);
        } else if (direction == NavigateDirection_PreviousSibling && at != siblings.begin() && at != siblings.end()) {
          target = *(at - 1);
        }
        break;
      }
    }
    if (target == -2) return S_OK;
    IRawElementProviderSimple* simple = For(bridge_, target);
    if (!simple) return S_OK;
    const HRESULT result = simple->QueryInterface(IID_PPV_ARGS(out));
    simple->Release();
    return result;
  }

  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** id) override {
    *id = nullptr;
    if (index_ == kRoot) return S_OK;  // The window's.
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    const int parts[] = {UiaAppendRuntimeId, static_cast<int>(generation_), index_};
    *id = SafeArrayCreateVector(VT_I4, 0, 3);
    if (!*id) return E_OUTOFMEMORY;
    for (LONG i = 0; i < 3; ++i) SafeArrayPutElement(*id, &i, const_cast<int*>(&parts[i]));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* rect) override {
    *rect = {};
    if (index_ == kRoot) return S_OK;  // The window's.
    const Element* element = Live();
    if (!element) return UIA_E_ELEMENTNOTAVAILABLE;
    SetupUiaHost& host = *bridge_->host;
    const RECT box = host.UiaScreenRect(host.UiaScene().Shown(*element, host.UiaScroll()));
    *rect = {static_cast<double>(box.left), static_cast<double>(box.top), static_cast<double>(box.right - box.left),
             static_cast<double>(box.bottom - box.top)};
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** roots) override {
    *roots = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetFocus() override {
    const Element* element = Live();
    if (!element) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!element->focusable || !element->enabled) return UIA_E_INVALIDOPERATION;
    bridge_->host->UiaFocusOn(index_);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** root) override {
    *root = nullptr;
    if (!bridge_->root) return S_OK;
    return bridge_->root->QueryInterface(IID_PPV_ARGS(root));
  }

  // IRawElementProviderFragmentRoot.
  HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override {
    *out = nullptr;
    if (!Alive()) return UIA_E_ELEMENTNOTAVAILABLE;
    SetupUiaHost& host = *bridge_->host;
    const D2D1_POINT_2F point = host.UiaClientPoint({static_cast<LONG>(x), static_cast<LONG>(y)});
    const Scene& scene = host.UiaScene();
    // The smallest element there: the most specific.
    int found = -1;
    float smallest = 0;
    for (size_t i = 0; i < scene.elements.size(); ++i) {
      const D2D1_RECT_F box = scene.Shown(scene.elements[i], host.UiaScroll());
      if (point.x < box.left || point.x >= box.right || point.y < box.top || point.y >= box.bottom) continue;
      if (!scene.Visible(scene.elements[i], host.UiaScroll())) continue;
      const float area = (box.right - box.left) * (box.bottom - box.top);
      if (found < 0 || area <= smallest) {
        found = static_cast<int>(i);
        smallest = area;
      }
    }
    if (found < 0) return S_OK;
    IRawElementProviderSimple* simple = For(bridge_, found);
    const HRESULT result = simple->QueryInterface(IID_PPV_ARGS(out));
    simple->Release();
    return result;
  }

  HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override {
    *out = nullptr;
    if (!Alive()) return UIA_E_ELEMENTNOTAVAILABLE;
    const int focus = bridge_->host->UiaFocus();
    if (focus < 0) return S_OK;
    IRawElementProviderSimple* simple = For(bridge_, focus);
    const HRESULT result = simple->QueryInterface(IID_PPV_ARGS(out));
    simple->Release();
    return result;
  }

  // IInvokeProvider.
  HRESULT STDMETHODCALLTYPE Invoke() override { return Activate(); }

  // IToggleProvider.
  HRESULT STDMETHODCALLTYPE Toggle() override { return Activate(); }
  HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* state) override {
    const Element* element = Live();
    if (!element) return UIA_E_ELEMENTNOTAVAILABLE;
    *state = element->selected ? ToggleState_On : ToggleState_Off;
    return S_OK;
  }

  // ISelectionItemProvider.
  HRESULT STDMETHODCALLTYPE Select() override {
    const Element* element = Live();
    if (!element) return UIA_E_ELEMENTNOTAVAILABLE;
    return element->selected ? S_OK : Activate();
  }
  HRESULT STDMETHODCALLTYPE AddToSelection() override { return Select(); }
  HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
  HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* selected) override {
    const Element* element = Live();
    if (!element) return UIA_E_ELEMENTNOTAVAILABLE;
    *selected = element->selected;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** container) override {
    *container = nullptr;
    const Element* element = Live();
    if (!element) return UIA_E_ELEMENTNOTAVAILABLE;
    if (element->parent >= 0) *container = For(bridge_, element->parent);
    return S_OK;
  }

 private:
  bool Alive() const { return bridge_->host && (index_ == kRoot || generation_ == bridge_->generation); }

  const Element* Live() const {
    if (!Alive() || index_ == kRoot) return nullptr;
    const std::vector<Element>& elements = bridge_->host->UiaScene().elements;
    return static_cast<size_t>(index_) < elements.size() ? &elements[static_cast<size_t>(index_)] : nullptr;
  }

  HRESULT Activate() {
    const Element* element = Live();
    if (!element) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!element->enabled) return UIA_E_ELEMENTNOTENABLED;
    bridge_->host->UiaActivate(index_);
    return S_OK;
  }

  std::atomic<ULONG> refs_ = 1;
  std::shared_ptr<UiaBridge> bridge_;
  int index_;
  uint32_t generation_;
};

// Raises `event` from `element`'s provider, if anything is listening.
void Raise(const std::shared_ptr<UiaBridge>& bridge, int element, EVENTID event) {
  if (!bridge->host || element < 0 || !UiaClientsAreListening()) return;
  IRawElementProviderSimple* provider = Provider::For(bridge, element);
  UiaRaiseAutomationEvent(provider, event);
  provider->Release();
}

}  // namespace

SetupUia::SetupUia(SetupUiaHost& host) : bridge_(std::make_shared<UiaBridge>()) {
  bridge_->host = &host;
  bridge_->root = new Provider(bridge_, kRoot);
}

SetupUia::~SetupUia() { Detach(); }

LRESULT SetupUia::OnGetObject(WPARAM wparam, LPARAM lparam) {
  if (!bridge_->host || !bridge_->root || static_cast<long>(lparam) != UiaRootObjectId) return 0;
  return UiaReturnRawElementProvider(bridge_->host->UiaWindow(), wparam, lparam, bridge_->root);
}

void SetupUia::SceneChanged(bool new_page, bool rebuilt, const std::wstring& heading) {
  if (rebuilt) ++bridge_->generation;
  if (!new_page || !bridge_->root || !UiaClientsAreListening()) return;
  const BSTR said = SysAllocString(heading.c_str());
  const BSTR activity = SysAllocString(L"knobs.setup.page");
  UiaRaiseNotificationEvent(bridge_->root, NotificationKind_ActionCompleted,
                            NotificationProcessing_ImportantMostRecent, said, activity);
  SysFreeString(said);
  SysFreeString(activity);
}

void SetupUia::FocusChanged(int element) { Raise(bridge_, element, UIA_AutomationFocusChangedEventId); }

void SetupUia::Toggled(int element, bool was_on) {
  if (!bridge_->host || element < 0 || !UiaClientsAreListening()) return;
  IRawElementProviderSimple* provider = Provider::For(bridge_, element);
  VARIANT before = {}, after = {};
  before.vt = after.vt = VT_I4;
  before.lVal = was_on ? ToggleState_On : ToggleState_Off;
  after.lVal = was_on ? ToggleState_Off : ToggleState_On;
  UiaRaiseAutomationPropertyChangedEvent(provider, UIA_ToggleToggleStatePropertyId, before, after);
  provider->Release();
}

void SetupUia::Selected(int element) { Raise(bridge_, element, UIA_SelectionItem_ElementSelectedEventId); }

void SetupUia::StatusChanged(int element) { Raise(bridge_, element, UIA_LiveRegionChangedEventId); }

void SetupUia::Detach() {
  if (!bridge_->host) return;
  const HWND window = bridge_->host->UiaWindow();
  bridge_->host = nullptr;
  if (window) UiaReturnRawElementProvider(window, 0, 0, nullptr);
  if (bridge_->root) {
    UiaDisconnectProvider(bridge_->root);
    bridge_->root->Release();
    bridge_->root = nullptr;
  }
}

}  // namespace knobs::tray
