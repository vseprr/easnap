#include "ui/timeline_accessibility.h"

#include <UIAutomation.h>
#include <atomic>
#include <cmath>
#include <new>

namespace easnap::ui {
namespace {
struct TimelineState {
    HWND window{};
    std::function<AccessibleTimelineSnapshot()> read;
    std::function<bool(int, double)> set;
    std::function<void(int)> focus;
};

class RangeProvider final : public IRawElementProviderSimple,
                            public IRawElementProviderFragment,
                            public IRawElementProviderFragmentRoot,
                            public IRangeValueProvider {
public:
    RangeProvider(std::shared_ptr<TimelineState> state, int index) : state_(std::move(state)), index_(index) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) { return E_POINTER; }
        *result = nullptr;
        if (id == IID_IUnknown || id == __uuidof(IRawElementProviderSimple)) { *result = static_cast<IRawElementProviderSimple*>(this); }
        else if (id == __uuidof(IRawElementProviderFragment)) { *result = static_cast<IRawElementProviderFragment*>(this); }
        else if (id == __uuidof(IRawElementProviderFragmentRoot) && index_ < 0) { *result = static_cast<IRawElementProviderFragmentRoot*>(this); }
        else if (id == __uuidof(IRangeValueProvider) && index_ >= 0) { *result = static_cast<IRangeValueProvider*>(this); }
        if (!*result) { return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG left = --references_; if (!left) { delete this; } return left; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) override {
        if (!result) { return E_POINTER; }
        *result = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** result) override {
        if (!result) { return E_POINTER; } *result = nullptr;
        if (!Alive()) { return UIA_E_ELEMENTNOTAVAILABLE; }
        if (index_ >= 0 && pattern == UIA_RangeValuePatternId) {
            *result = static_cast<IRangeValueProvider*>(this); AddRef();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* result) override {
        if (!result) { return E_POINTER; } ::VariantInit(result);
        if (!Alive()) { return UIA_E_ELEMENTNOTAVAILABLE; }
        const auto snapshot = state_->read();
        if (property == UIA_NamePropertyId || property == UIA_AutomationIdPropertyId || property == UIA_HelpTextPropertyId) {
            static constexpr const wchar_t* names[]{L"In point", L"Playback position", L"Out point"};
            const wchar_t* text = property == UIA_HelpTextPropertyId
                ? L"Seconds. Arrow keys adjust; Shift adjusts In and Ctrl adjusts Out. Export follows media sample boundaries."
                : (index_ < 0 ? L"Trim timeline" : names[index_]);
            result->vt = VT_BSTR; result->bstrVal = ::SysAllocString(text);
            return result->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        if (property == UIA_ControlTypePropertyId) {
            result->vt = VT_I4; result->lVal = index_ < 0 ? UIA_GroupControlTypeId : UIA_SliderControlTypeId;
        } else if (property == UIA_IsEnabledPropertyId || property == UIA_IsKeyboardFocusablePropertyId ||
                   property == UIA_HasKeyboardFocusPropertyId || property == UIA_IsControlElementPropertyId ||
                   property == UIA_IsContentElementPropertyId || property == UIA_IsOffscreenPropertyId) {
            result->vt = VT_BOOL;
            bool value = true;
            if (property == UIA_IsEnabledPropertyId) { value = snapshot.enabled; }
            if (property == UIA_IsKeyboardFocusablePropertyId) { value = index_ >= 0; }
            if (property == UIA_HasKeyboardFocusPropertyId) { value = index_ == snapshot.focus && ::GetFocus() == state_->window; }
            if (property == UIA_IsOffscreenPropertyId) { value = !::IsWindowVisible(state_->window); }
            result->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override {
        if (!result) { return E_POINTER; } *result = nullptr;
        if (!Alive()) { return UIA_E_ELEMENTNOTAVAILABLE; }
        return index_ < 0 ? ::UiaHostProviderFromHwnd(state_->window, result) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** result) override {
        if (!result) { return E_POINTER; } *result = nullptr;
        if (!Alive()) { return UIA_E_ELEMENTNOTAVAILABLE; }
        int target = -2;
        if (index_ < 0 && direction == NavigateDirection_FirstChild) { target = 0; }
        if (index_ < 0 && direction == NavigateDirection_LastChild) { target = 2; }
        if (index_ >= 0 && direction == NavigateDirection_Parent) { target = -1; }
        if (index_ >= 0 && index_ < 2 && direction == NavigateDirection_NextSibling) { target = index_ + 1; }
        if (index_ > 0 && direction == NavigateDirection_PreviousSibling) { target = index_ - 1; }
        return target == -2 ? S_OK : MakeFragment(target, result);
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override {
        if (!result) { return E_POINTER; } *result = nullptr;
        if (!Alive()) { return UIA_E_ELEMENTNOTAVAILABLE; }
        if (index_ < 0) { return S_OK; }
        *result = ::SafeArrayCreateVector(VT_I4, 0, 2);
        if (!*result) { return E_OUTOFMEMORY; }
        int values[]{UiaAppendRuntimeId, index_ + 1};
        for (LONG i = 0; i != 2; ++i) { ::SafeArrayPutElement(*result, &i, &values[i]); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* result) override {
        if (!result) { return E_POINTER; } *result = {};
        if (!Alive()) { return UIA_E_ELEMENTNOTAVAILABLE; }
        RECT r{};
        if (index_ < 0) { ::GetWindowRect(state_->window, &r); }
        else { r = state_->read().values[index_].bounds; }
        *result = {static_cast<double>(r.left), static_cast<double>(r.top),
            static_cast<double>(r.right-r.left), static_cast<double>(r.bottom-r.top)};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** result) override {
        if (!result) { return E_POINTER; } *result = nullptr; return Alive() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        if (!Alive()) { return UIA_E_ELEMENTNOTAVAILABLE; }
        if (!state_->read().enabled) { return UIA_E_ELEMENTNOTENABLED; }
        state_->focus(index_ < 0 ? 1 : index_); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** result) override {
        if (!result) { return E_POINTER; } *result = nullptr;
        if (!Alive()) { return UIA_E_ELEMENTNOTAVAILABLE; }
        auto* provider = new (std::nothrow) RangeProvider(state_, -1);
        if (!provider) { return E_OUTOFMEMORY; }
        *result = provider; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** result) override {
        if (!result) { return E_POINTER; } *result = nullptr;
        if (!Alive()) { return UIA_E_ELEMENTNOTAVAILABLE; }
        const auto snapshot = state_->read();
        for (int i = 0; i != 3; ++i) {
            const RECT& r = snapshot.values[i].bounds;
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) { return MakeFragment(i, result); }
        }
        return MakeFragment(-1, result);
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) override {
        if (!result) { return E_POINTER; } *result = nullptr;
        if (!Alive()) { return UIA_E_ELEMENTNOTAVAILABLE; }
        return ::GetFocus() == state_->window ? MakeFragment(state_->read().focus, result) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetValue(double value) override {
        if (!Alive() || index_ < 0) { return UIA_E_ELEMENTNOTAVAILABLE; }
        const auto snapshot = state_->read();
        if (!snapshot.enabled) { return UIA_E_ELEMENTNOTENABLED; }
        const auto& range = snapshot.values[index_];
        if (!std::isfinite(value) || value < range.minimum || value > range.maximum) { return E_INVALIDARG; }
        return state_->set(index_, value) ? S_OK : E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE get_Value(double* result) override { return ReadNumber(result, 0); }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* result) override {
        if (!result) { return E_POINTER; }
        if (!Alive()) { return UIA_E_ELEMENTNOTAVAILABLE; }
        *result = !state_->read().enabled; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Maximum(double* result) override { return ReadNumber(result, 2); }
    HRESULT STDMETHODCALLTYPE get_Minimum(double* result) override { return ReadNumber(result, 1); }
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* result) override { if (!result) { return E_POINTER; } *result = 10; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* result) override { if (!result) { return E_POINTER; } *result = 1; return S_OK; }
private:
    bool Alive() const { return state_->window && ::IsWindow(state_->window); }
    HRESULT MakeFragment(int index, IRawElementProviderFragment** result) {
        auto* provider = new (std::nothrow) RangeProvider(state_, index);
        if (!provider) { return E_OUTOFMEMORY; } *result = provider; return S_OK;
    }
    HRESULT ReadNumber(double* result, int field) {
        if (!result) { return E_POINTER; } *result = 0;
        if (!Alive() || index_ < 0) { return UIA_E_ELEMENTNOTAVAILABLE; }
        const auto value = state_->read().values[index_];
        *result = field == 0 ? value.value : field == 1 ? value.minimum : value.maximum;
        return S_OK;
    }
    std::atomic<ULONG> references_{1};
    std::shared_ptr<TimelineState> state_;
    int index_{};
};
}

struct TimelineAccessibility::Impl {
    std::shared_ptr<TimelineState> state;
    platform::ComPtr<IRawElementProviderSimple> root;
    AccessibleTimelineSnapshot previous{};
    bool focused{false};
};
TimelineAccessibility::TimelineAccessibility(HWND window, std::function<AccessibleTimelineSnapshot()> read,
    std::function<bool(int, double)> set, std::function<void(int)> focus) : impl_(std::make_unique<Impl>()) {
    impl_->state = std::make_shared<TimelineState>(TimelineState{window, std::move(read), std::move(set), std::move(focus)});
    impl_->root.Attach(new RangeProvider(impl_->state, -1));
    impl_->previous = impl_->state->read();
}
TimelineAccessibility::~TimelineAccessibility() {
    impl_->state->window = nullptr;
    impl_->state->read = {}; impl_->state->set = {}; impl_->state->focus = {};
    ::UiaDisconnectProvider(impl_->root.Get());
}
LRESULT TimelineAccessibility::HandleGetObject(WPARAM wparam, LPARAM lparam) {
    return ::UiaReturnRawElementProvider(impl_->state->window, wparam, lparam, impl_->root.Get());
}
void TimelineAccessibility::NotifyChanged() {
    const auto current = impl_->state->read();
    const bool focused = ::GetFocus() == impl_->state->window;
    if (::UiaClientsAreListening()) {
        for (int i = 0; i != 3; ++i) {
            platform::ComPtr<IRawElementProviderSimple> provider;
            provider.Attach(new RangeProvider(impl_->state, i));
            if (current.values[i].value != impl_->previous.values[i].value) {
                VARIANT before{}, after{}; before.vt = after.vt = VT_R8;
                before.dblVal = impl_->previous.values[i].value; after.dblVal = current.values[i].value;
                ::UiaRaiseAutomationPropertyChangedEvent(provider.Get(), UIA_RangeValueValuePropertyId, before, after);
            }
            if (current.enabled != impl_->previous.enabled) {
                VARIANT before{}, after{}; before.vt = after.vt = VT_BOOL;
                before.boolVal = impl_->previous.enabled ? VARIANT_TRUE : VARIANT_FALSE;
                after.boolVal = current.enabled ? VARIANT_TRUE : VARIANT_FALSE;
                ::UiaRaiseAutomationPropertyChangedEvent(provider.Get(), UIA_IsEnabledPropertyId, before, after);
            }
            if (i == current.focus && focused && (!impl_->focused || current.focus != impl_->previous.focus)) {
                ::UiaRaiseAutomationEvent(provider.Get(), UIA_AutomationFocusChangedEventId);
            }
        }
    }
    impl_->previous = current;
    impl_->focused = focused;
}
} // namespace easnap::ui
