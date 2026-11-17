#include "ui/native_controls.h"

#include <commctrl.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <algorithm>
#include <array>
#include <new>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <initguid.h>
#include <oleacc.h>

#include "app/app_settings.h"
#include "platform/files.h"
#include "platform/platform_utils.h"
#include "strings/strings.h"

namespace easnap::ui {
namespace {
constexpr wchar_t kScreenContentProperty[] = L"Easnap.ScreenContent";
struct ButtonData {
    ButtonStyle style;
    bool hot{false};
    float depth{1}, from{1}, target{1}, hover{0};
    ULONGLONG started{0};
    float duration{theme::kButtonSettleMs};
};
void Advance(ButtonData& data) {
    if (!data.started) { return; }
    float t = std::clamp(static_cast<float>(::GetTickCount64() - data.started) / data.duration, 0.0f, 1.0f);
    t = t * t * (3 - 2 * t);
    data.depth = data.from + (data.target - data.from) * t;
}
void Retarget(HWND hwnd, ButtonData& data) {
    Advance(data);
    const bool pressed = (::SendMessageW(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
    const bool checked = ::SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const float target = !::IsWindowEnabled(hwnd) && !data.style.preserve_disabled_visual ? 0.0f : pressed ? -1.0f
                                                                                               : checked   ? -1.0f
                                                                                               : data.hot  ? 1.25f
                                                                                                           : 1.0f;
    if (target == data.target) { return; }
    data.from = data.depth;
    data.target = target;
    data.started = ::GetTickCount64();
    data.duration = pressed ? theme::kButtonPressMs : checked ? theme::kButtonCheckMs
                                                              : theme::kButtonHoverMs;
    if (theme::AnimationsEnabled() && ::IsWindowVisible(hwnd)) {
        ::SetTimer(hwnd, kButtonSubclass, theme::kAnimationTickMs, nullptr);
    } else {
        data.depth = target;
        data.started = 0;
        ::KillTimer(hwnd, kButtonSubclass);
    }
    ::InvalidateRect(hwnd, nullptr, FALSE);
}
unsigned popup_depth = 0;
std::array<bool, 256> swallowed_keys{};
bool swallowed_left = false;
bool swallowed_right = false;

void RefreshTooltip(HWND window) {
    ::SendMessageW(window, TTM_SETTIPBKCOLOR, theme::Current().tooltip, 0);
    ::SendMessageW(window, TTM_SETTIPTEXTCOLOR, theme::Current().text, 0);
    ::InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK TooltipProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                             UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) { ::RemoveWindowSubclass(window, TooltipProc, kTooltipSubclass); }
    if (message == theme::kChangedMessage) {
        RefreshTooltip(window);
        return 0;
    }
    if (message == WM_WINDOWPOSCHANGING && (reinterpret_cast<const WINDOWPOS*>(lparam)->flags & SWP_SHOWWINDOW) != 0) {
        // A tooltip is a layered window that has not been composed until it is
        // first shown, and Windows refuses its exclusion before that (measured:
        // the affinity reads 0 afterwards). Exclude it as each showing begins.
        DWORD affinity = 0;
        if (!::GetWindowDisplayAffinity(window, &affinity) || affinity != 0x11) {
            static_cast<void>(::SetWindowDisplayAffinity(window, 0x11));
        }
    }
    return ::DefSubclassProc(window, message, wparam, lparam);
}

void PaintButton(HWND hwnd, HDC dc, ButtonData& data) {
    Advance(data);
    ButtonPaintState state;
    state.style = data.style;
    state.scale = static_cast<float>(::GetDpiForWindow(hwnd)) / 96.0f;
    ::GetClientRect(hwnd, &state.client);
    state.window_enabled = ::IsWindowEnabled(hwnd) != FALSE;
    state.pressed = (::SendMessageW(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
    state.checked = ::SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state.focused = ::GetFocus() == hwnd;
    state.focus_cues_hidden = state.focused && (::SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
    state.hot = data.hot;
    state.depth = data.depth;
    state.type = ::GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK;
    wchar_t text[256]{};
    ::GetWindowTextW(hwnd, text, ARRAYSIZE(text));
    state.text = text;
    state.font = reinterpret_cast<HFONT>(::SendMessageW(hwnd, WM_GETFONT, 0, 0));
    PaintButtonFace(dc, state);
}

LRESULT CALLBACK ButtonProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR reference) {
    if (message == WM_PRINT) { return ::DefWindowProcW(hwnd, message, wparam, lparam); }
    auto* data = reinterpret_cast<ButtonData*>(reference);
    if (message == WM_SETTEXT && lparam) {
        const int length = ::GetWindowTextLengthW(hwnd);
        std::wstring current(static_cast<size_t>(length) + 1, L'\0');
        ::GetWindowTextW(hwnd, current.data(), length + 1);
        if (::lstrcmpW(current.c_str(), reinterpret_cast<const wchar_t*>(lparam)) == 0) { return TRUE; }
    }
    if (message == WM_NCDESTROY) {
        ::KillTimer(hwnd, kButtonSubclass);
        ::RemoveWindowSubclass(hwnd, ButtonProc, kButtonSubclass);
        delete data;
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }
    if (message == WM_TIMER && wparam == kButtonSubclass) {
        Advance(*data);
        if (::GetTickCount64() - data->started >= static_cast<ULONGLONG>(data->duration)) {
            data->depth = data->target;
            data->started = 0;
            ::KillTimer(hwnd, kButtonSubclass);
        }
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (message == WM_PAINT || message == WM_PRINTCLIENT || message == WM_ERASEBKGND) {
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? ::BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wparam);
        RECT r{};
        ::GetClientRect(hwnd, &r);
        // USER32 can request erase after WINDOWPOSCHANGED's paint, with the
        // final newly exposed region. Publish the complete control to that DC.
        platform::PaintBuffer buffer(dc, r);
        PaintButton(hwnd, buffer.dc(), *data);
        buffer.Present();
        if (message == WM_PAINT) { ::EndPaint(hwnd, &paint); }
        return message == WM_ERASEBKGND ? 1 : 0;
    }
    if (message == WM_MOUSEMOVE && !data->hot) {
        data->hot = true;
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
        Retarget(hwnd, *data);
        ::InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (message == WM_MOUSELEAVE) {
        data->hot = false;
        Retarget(hwnd, *data);
        ::InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (message == WM_SHOWWINDOW) { NativeRedrawGuard::VisibilityChanged(hwnd, wparam != 0); }
    // Input dispatch must remain visible: automatic radio groups enumerate
    // visible siblings during activation. Their nested state messages own paint.
    const bool changes_pixels = message == BM_SETCHECK || message == BM_SETSTATE || message == BM_SETSTYLE ||
                                message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE || message == WM_SETTEXT;
    const LRESULT result = [&] {
        NativeRedrawGuard redraw(hwnd, changes_pixels);
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }();
    if (!::IsWindow(hwnd)) { return result; }
    switch (message) {
        case BM_SETCHECK:
        case BM_SETSTATE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
        case WM_SETTEXT:
        case WM_THEMECHANGED:
        case theme::kChangedMessage:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            Retarget(hwnd, *data);
            ::InvalidateRect(hwnd, nullptr, FALSE);
            break;
        default: break;
    }
    return result;
}

LRESULT CALLBACK MenuHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0) {
        const auto* call = reinterpret_cast<CWPSTRUCT*>(lparam);
        if (call->message == WM_WINDOWPOSCHANGED || call->message == WM_SHOWWINDOW) {
            wchar_t name[32]{};
            ::GetClassNameW(call->hwnd, name, ARRAYSIZE(name));
            // Menus are the system's own windows, with their own animation and
            // shadow, so they are not made layered; they keep the plain
            // exclusion (ADR-008).
            if (::lstrcmpW(name, L"#32768") == 0) { ::SetWindowDisplayAffinity(call->hwnd, 0x11); }
        }
    }
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
}
}  // namespace

namespace {
constexpr wchar_t kRedrawGuard[] = L"Easnap.NativeRedrawGuard";
}  // namespace
void TypingPointer::Typed(HWND timer_owner) {
    if (hidden_ || timer_owner == nullptr) { return; }
    BOOL vanish = FALSE;
    if (!::SystemParametersInfoW(SPI_GETMOUSEVANISH, 0, &vanish, 0) || !vanish) { return; }
    if (!::GetCursorPos(&origin_) || ::SetTimer(timer_owner, kTypingPointerTimer, 30, nullptr) == 0) { return; }
    timer_owner_ = timer_owner;
    hidden_ = true;
    // The display count belongs to this thread: the pointer hides over its
    // windows only, and every other application still shows it.
    ::ShowCursor(FALSE);
}
void TypingPointer::Poll() {
    POINT now{};
    if (hidden_ && ::GetCursorPos(&now) && (now.x != origin_.x || now.y != origin_.y)) { Restore(); }
}
void TypingPointer::Restore() {
    if (!hidden_) { return; }
    hidden_ = false;
    if (timer_owner_ != nullptr && ::IsWindow(timer_owner_)) { ::KillTimer(timer_owner_, kTypingPointerTimer); }
    timer_owner_ = nullptr;
    ::ShowCursor(TRUE);
}
NativeRedrawGuard::NativeRedrawGuard(HWND window, bool enabled) {
    if (!enabled || !window || ::GetPropW(window, kRedrawGuard) || !(::GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE)) { return; }
    if (!::SetPropW(window, kRedrawGuard, this)) { return; }
    window_ = window;
    ::SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
}
NativeRedrawGuard::~NativeRedrawGuard() {
    if (!window_ || !::IsWindow(window_) || ::GetPropW(window_, kRedrawGuard) != this) { return; }
    ::RemovePropW(window_, kRedrawGuard);
    ::SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
    if (!visible_) { ::ShowWindow(window_, SW_HIDE); }
    ::InvalidateRect(window_, nullptr, FALSE);
}
void NativeRedrawGuard::VisibilityChanged(HWND window, bool visible) {
    if (auto* guard = static_cast<NativeRedrawGuard*>(::GetPropW(window, kRedrawGuard))) { guard->visible_ = visible; }
}
void PaintButtonFace(HDC dc, const ButtonPaintState& state) {
    const ButtonStyle& style = state.style;
    const auto& p = theme::Current();
    const float scale = state.scale;
    const RECT& client = state.client;
    theme::FillSolid(dc, client, p.panel);
    RECT face = client;
    ::InflateRect(&face, -theme::Scaled(theme::kButtonFaceInset, scale), -theme::Scaled(theme::kButtonFaceInset, scale));
    const bool enabled = state.window_enabled || style.preserve_disabled_visual;
    const bool pressed = state.pressed;
    const bool checked = state.checked;
    COLORREF fill = checked || pressed ? p.pressed : (state.hot ? p.hover : p.raised);
    COLORREF ink = enabled ? p.text : p.disabled;
    if (checked && enabled) { ink = p.focus; }
    if (enabled && style.role == ButtonRole::Primary) { ink = p.accent; }
    if (enabled && style.role == ButtonRole::Destructive) { ink = p.error; }
    if (enabled && style.role == ButtonRole::Success) { ink = p.success; }
    if (p.high_contrast && pressed) {
        fill = p.accent;
        ink = p.on_accent;
    }
    const float face_radius = style.circular ? std::min(face.right - face.left, face.bottom - face.top) / (2 * scale) : theme::kControlRadius;
    theme::DrawMaterial(dc, face, face_radius, scale, fill, state.depth, style.circular);
    if (state.focused && !style.hide_focus_line && !state.focus_cues_hidden) {
        // Keyboard focus is a neutral underline, independent of latched depth.
        const LONG inset = theme::Scaled(theme::kButtonFocusInset, scale);
        theme::DrawSolidLine(dc, {face.left + inset, face.bottom + 2}, {face.right - inset, face.bottom + 2}, p.text, 1);
    }
    const LONG size = theme::Scaled(style.circular ? theme::kButtonCircularIconSize : theme::kButtonIconSize, scale);
    const LONG cy = (face.top + face.bottom) / 2;
    RECT icon{face.left + theme::Scaled(theme::kButtonIconInset, scale), cy - size / 2, 0, cy - size / 2 + size};
    if (style.icon_only || style.swatch) { icon.left = (face.left + face.right - size) / 2; }
    if (style.icon_only && style.dropdown) { icon.left -= theme::Scaled(theme::kButtonDropdownShift, scale); }
    icon.right = icon.left + size;
    if (style.sample != 0) {
        icon.left = (face.left + face.right - size) / 2;
        icon.right = icon.left + size;
        if (style.sample == 1) {
            theme::DrawSolidLine(dc, {icon.left, cy}, {icon.right, cy}, ink, theme::Scaled(style.sample_size, scale));
        } else {
            const LONG radius = theme::Scaled(2 + style.sample_size / 10.0f, scale);
            const LONG cx = (face.left + face.right) / 2;
            theme::FillRounded(dc, {cx - radius, cy - radius, cx + radius, cy + radius}, radius, ink);
        }
    } else if (style.swatch) {
        theme::FillRoundedBordered(dc, icon, theme::Scaled(theme::kSwatchRadius, scale), style.content_color, p.control_border, 1);
        if (checked) { DrawIcon(dc, icon, Icon::Check, GetRValue(style.content_color) + GetGValue(style.content_color) + GetBValue(style.content_color) > theme::kSwatchCheckThreshold ? theme::kSwatchCheckDark : theme::kSwatchCheckLight); }
    } else if (style.icon != Icon::None) {
        DrawIcon(dc, icon, style.icon, ink);
    }
    if (style.dropdown) {
        const LONG chevron = theme::Scaled(theme::kButtonChevronSize, scale);
        const LONG right = face.right - theme::Scaled(theme::kButtonChevronInset, scale);
        DrawIcon(dc, {right - chevron, cy - chevron / 2, right, cy + chevron / 2}, Icon::ChevronDown, ink);
    }
    if (!style.icon_only && !style.swatch) {
        RECT label = face;
        ::InflateRect(&label, -theme::Scaled(theme::kButtonLabelInset, scale), 0);
        UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
        if (style.icon != Icon::None) {
            label.left = icon.right + theme::Scaled(theme::kButtonIconLabelGap, scale);
            format = DT_LEFT | DT_VCENTER | DT_SINGLELINE;
        }
        const auto type = state.type;
        if ((type == BS_AUTOCHECKBOX || type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON) && style.icon == Icon::None && !style.segmented) {
            RECT check{face.left + theme::Scaled(theme::kButtonCheckInset, scale), cy - theme::Scaled(theme::kButtonCheckSize / 2, scale),
                       face.left + theme::Scaled(theme::kButtonCheckInset + theme::kButtonCheckSize, scale), cy + theme::Scaled(theme::kButtonCheckSize / 2, scale)};
            if (checked && type == BS_AUTOCHECKBOX) {
                DrawIcon(dc, check, Icon::Check, ink);
            } else if (checked) {
                theme::FillRounded(dc, check, theme::Scaled(theme::kButtonCheckSize / 2, scale), ink);
            }
            label.left = check.right + theme::Scaled(theme::kButtonCheckLabelGap, scale);
            format = DT_LEFT | DT_VCENTER | DT_SINGLELINE;
        }
        theme::DrawLabel(dc, label, state.text, ink, state.font, format);
    }
}

bool StyleButton(HWND button, ButtonStyle style) {
    if (!button) { return false; }
    auto* data = new (std::nothrow) ButtonData{style};
    if (!data) { return false; }
    if (!::SetWindowSubclass(button, ButtonProc, kButtonSubclass, reinterpret_cast<DWORD_PTR>(data))) {
        delete data;
        return false;
    }
    return true;
}
void UpdateButtonStyle(HWND button, ButtonStyle style) {
    DWORD_PTR data{};
    if (::GetWindowSubclass(button, ButtonProc, kButtonSubclass, &data)) {
        reinterpret_cast<ButtonData*>(data)->style = style;
        ::InvalidateRect(button, nullptr, FALSE);
    }
}
int ButtonContentWidth(HWND button) {
    DWORD_PTR reference{};
    if (!button || !::GetWindowSubclass(button, ButtonProc, kButtonSubclass, &reference)) { return 0; }
    const ButtonStyle& style = reinterpret_cast<ButtonData*>(reference)->style;
    if (style.icon_only || style.swatch) { return 0; }
    wchar_t text[256]{};
    const int length = ::GetWindowTextW(button, text, ARRAYSIZE(text));
    if (length == 0) { return 0; }
    const float scale = static_cast<float>(::GetDpiForWindow(button)) / 96.0f;
    SIZE extent{};
    if (HDC dc = ::CreateCompatibleDC(nullptr)) {
        const HGDIOBJ previous = ::SelectObject(dc, reinterpret_cast<HFONT>(::SendMessageW(button, WM_GETFONT, 0, 0)));
        ::GetTextExtentPoint32W(dc, text, length, &extent);
        ::SelectObject(dc, previous);
        ::DeleteDC(dc);
    }
    // The same insets PaintButton applies: 6 around the face, 6 beside the
    // caption, and what an icon or a check mark takes on the left.
    int lead = theme::Scaled(theme::kButtonLabelInset, scale);
    const auto type = ::GetWindowLongPtrW(button, GWL_STYLE) & BS_TYPEMASK;
    if (style.icon != Icon::None) {
        lead = theme::Scaled(theme::kButtonIconInset + theme::kButtonIconSize + theme::kButtonIconLabelGap, scale);
    } else if ((type == BS_AUTOCHECKBOX || type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON) && !style.segmented) {
        lead = theme::Scaled(theme::kButtonCheckInset + theme::kButtonCheckSize + theme::kButtonCheckLabelGap, scale);
    }
    return 2 * theme::Scaled(theme::kButtonFaceInset, scale) + lead + static_cast<int>(extent.cx) + theme::Scaled(theme::kButtonLabelInset, scale);
}
HWND CreateStyledButton(HWND parent, const StyledButtonRow& row, DWORD extra_style, ButtonPlacement placement, bool alternate) {
    HWND button = ::CreateWindowExW(0, L"BUTTON", strings::Get(RowCaption(row, alternate)), WS_CHILD | extra_style | row.window_style,
                                    placement.x, placement.y, placement.width, placement.height, parent,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(row.id)), ::GetModuleHandleW(nullptr), nullptr);
    if (button && !StyleButton(button, alternate ? AlternateStyle(row) : row.style)) {
        ::DestroyWindow(button);
        return nullptr;
    }
    return button;
}
void AddButtonTooltip(HWND tooltip, HWND owner, HWND button, const StyledButtonRow& row) {
    if (tooltip == nullptr || button == nullptr || row.tooltip == 0) { return; }
    TOOLINFOW info{sizeof(info)};
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = owner;
    info.uId = reinterpret_cast<UINT_PTR>(button);
    info.lpszText = const_cast<wchar_t*>(strings::Get(row.tooltip));
    ::SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}
namespace {
/// BufferedTextProc's reference data for an EDIT; a label has 0.
constexpr DWORD_PTR kBufferedEdit = 1;
LRESULT CALLBACK BufferedTextProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR reference) {
    if (message == WM_PRINT) { return ::DefWindowProcW(hwnd, message, wparam, lparam); }
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, BufferedTextProc, id);
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }
    if (message == WM_SHOWWINDOW) { NativeRedrawGuard::VisibilityChanged(hwnd, wparam != 0); }
    if (message == WM_PAINT || message == WM_PRINTCLIENT || message == WM_ERASEBKGND) {
        PAINTSTRUCT ps{};
        HDC target = message == WM_PAINT ? ::BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wparam);
        RECT r{};
        ::GetClientRect(hwnd, &r);
        const bool caret = ::HideCaret(hwnd) != FALSE;
        {
            platform::PaintBuffer buffer(target, r);
            theme::FillSolid(buffer.dc(), r, theme::Current().panel);
            ::DefSubclassProc(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(buffer.dc()), PRF_CLIENT);
        }
        if (caret) { ::ShowCaret(hwnd); }
        if (message == WM_PAINT) { ::EndPaint(hwnd, &ps); }
        return message == WM_ERASEBKGND ? 1 : 0;
    }
    // An EDIT's typing is not suspended. Suspending redraw takes WS_VISIBLE
    // away and gives it back, which Windows takes for pointer movement: the
    // pointer it hides while the user types came back at every key. EDIT
    // draws its text on the same background as the buffered pass.
    const bool typing = message >= WM_KEYFIRST && message <= WM_KEYLAST;
    const bool update = (typing && reference != kBufferedEdit) || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
                        message == WM_SETTEXT || message == WM_SETFONT || message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
                        message == EM_REPLACESEL || message == EM_SETSEL || message == WM_CUT || message == WM_PASTE || message == WM_CLEAR ||
                        message == WM_UNDO || message == EM_UNDO || message == WM_IME_COMPOSITION || message == WM_SIZE;
    const bool visible = (::GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
    NativeRedrawGuard guard(hwnd, update);
    // WM_SETREDRAW also took WS_VISIBLE away. An EDIT keeps its own no-redraw
    // flag, but one that is not visible neither scrolls its text to the caret
    // nor places the caret: once the text was wider than the field, the caret
    // stayed on one letter while the rest went out of sight. The text editor
    // does the same (ScreenshotEditor::TextEditProc).
    if (update && visible && reference == kBufferedEdit) {
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, ::GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_VISIBLE);
    }
    if (message == theme::kChangedMessage) { ::InvalidateRect(hwnd, nullptr, FALSE); }
    return ::DefSubclassProc(hwnd, message, wparam, lparam);
}
LRESULT CALLBACK SliderProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY) { ::RemoveWindowSubclass(hwnd, SliderProc, id); }
    if (message == WM_ERASEBKGND) { return 1; }
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? ::BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wparam);
        RECT r{}, thumb{}, track{};
        ::GetClientRect(hwnd, &r);
        HDC destination = dc, buffer = ::CreateCompatibleDC(dc);
        HBITMAP bitmap = ::CreateCompatibleBitmap(dc, r.right, r.bottom);
        HGDIOBJ old = nullptr;
        if (buffer && bitmap) {
            old = ::SelectObject(buffer, bitmap);
            dc = buffer;
        }
        ::SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        ::SendMessageW(hwnd, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&track));
        const float scale = ::GetDpiForWindow(hwnd) / 96.0f;
        const auto& p = theme::Current();
        theme::FillSolid(dc, r, p.panel);
        const LONG cy = (r.top + r.bottom) / 2;
        track.top = cy - theme::Scaled(3, scale);
        track.bottom = cy + theme::Scaled(3, scale);
        theme::DrawMaterial(dc, track, 3, scale, p.recessed, -1);
        const LONG x = (thumb.left + thumb.right) / 2;
        RECT fill = track;
        fill.right = x;
        ::InflateRect(&fill, 0, -theme::Scaled(1, scale));
        theme::FillRounded(dc, fill, theme::Scaled(2, scale), p.accent);
        const LONG radius = theme::Scaled(6, scale);
        theme::DrawMaterial(dc, {x - radius, cy - radius, x + radius, cy + radius}, 6, scale, p.raised, 0.9f);
        theme::FillRounded(dc, {x - radius / 2, cy - radius / 2, x + radius / 2, cy + radius / 2}, radius / 2, p.accent);
        if (old) {
            ::BitBlt(destination, 0, 0, r.right, r.bottom, buffer, 0, 0, SRCCOPY);
            ::SelectObject(buffer, old);
        }
        if (bitmap) { ::DeleteObject(bitmap); }
        if (buffer) { ::DeleteDC(buffer); }
        if (message == WM_PAINT) { ::EndPaint(hwnd, &ps); }
        return 0;
    }
    const bool input = message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_KEYDOWN || message == TBM_SETPOS;
    const bool suspend = input && (::GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
    if (suspend) { ::DefSubclassProc(hwnd, WM_SETREDRAW, FALSE, 0); }
    const LRESULT result = ::DefSubclassProc(hwnd, message, wparam, lparam);
    if (suspend) { ::DefSubclassProc(hwnd, WM_SETREDRAW, TRUE, 0); }
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_KEYDOWN ||
        message == WM_SETFOCUS || message == WM_KILLFOCUS || message == TBM_SETPOS || message == theme::kChangedMessage) { ::InvalidateRect(hwnd, nullptr, FALSE); }
    return result;
}
LRESULT CALLBACK InputProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY) { ::RemoveWindowSubclass(hwnd, InputProc, id); }
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) { return ControlBackground(wparam, theme::Current().text); }
    wchar_t class_name[32]{};
    ::GetClassNameW(hwnd, class_name, 32);
    const bool combo = ::lstrcmpiW(class_name, L"COMBOBOX") == 0;
    if (combo && message == WM_PRINT) { return ::DefWindowProcW(hwnd, message, wparam, lparam); }
    if (combo && (message == WM_NCPAINT || message == WM_ERASEBKGND)) { return 1; }
    if (combo && (message == WM_PAINT || message == WM_PRINTCLIENT)) {
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? ::BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wparam);
        RECT r{};
        ::GetClientRect(hwnd, &r);
        const float scale = ::GetDpiForWindow(hwnd) / 96.0f;
        platform::PaintBuffer buffer(dc, r);
        dc = buffer.dc();
        theme::FillSolid(dc, r, theme::Current().panel);
        RECT face = r;
        ::InflateRect(&face, -1, -1);
        theme::DrawMaterial(dc, face, 5, scale, theme::Current().panel, -1);
        COMBOBOXINFO info{sizeof(info)};
        ::GetComboBoxInfo(hwnd, &info);
        wchar_t value[32]{};
        ::GetWindowTextW(hwnd, value, 32);
        RECT label = r;
        label.left += theme::Scaled(7, scale);
        label.right = info.rcButton.left - theme::Scaled(3, scale);
        if (!info.hwndItem || info.hwndItem == hwnd) {
            theme::DrawLabel(dc, label, value, theme::Current().text, reinterpret_cast<HFONT>(::SendMessageW(hwnd, WM_GETFONT, 0, 0)), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        const LONG x = (info.rcButton.left + info.rcButton.right) / 2, y = (r.top + r.bottom) / 2, d = theme::Scaled(3, scale);
        theme::DrawSolidLine(dc, {x - d, y - 1}, {x, y + d - 1}, theme::Current().text, theme::Scaled(1, scale));
        theme::DrawSolidLine(dc, {x, y + d - 1}, {x + d, y - 1}, theme::Current().text, theme::Scaled(1, scale));

        buffer.Present();
        if (message == WM_PAINT) { ::EndPaint(hwnd, &ps); }
        return 0;
    }
    if (message == WM_NCCALCSIZE) {
        const auto result = ::DefSubclassProc(hwnd, message, wparam, lparam);
        wchar_t name[32]{};
        ::GetClassNameW(hwnd, name, 32);
        if (::lstrcmpW(name, L"Edit") == 0 && lparam) {
            auto* rect = wparam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0] : reinterpret_cast<RECT*>(lparam);
            ::InflateRect(rect, -theme::Scaled(5, ::GetDpiForWindow(hwnd) / 96.0f), -theme::Scaled(3, ::GetDpiForWindow(hwnd) / 96.0f));
        }
        return result;
    }
    const LRESULT result = ::DefSubclassProc(hwnd, message, wparam, lparam);
    if (message == WM_NCPAINT || message == WM_PAINT || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == theme::kChangedMessage) {
        HDC dc = ::GetWindowDC(hwnd);
        RECT r{}, client{};
        ::GetWindowRect(hwnd, &r);
        ::GetClientRect(hwnd, &client);
        ::MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
        ::OffsetRect(&client, -r.left, -r.top);
        ::OffsetRect(&r, -r.left, -r.top);
        const auto saved = ::SaveDC(dc);
        ::ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
        RECT face = r;
        ::InflateRect(&face, -1, -1);
        theme::DrawMaterial(dc, face, 4, ::GetDpiForWindow(hwnd) / 96.0f, theme::Current().panel, -1);

        ::RestoreDC(dc, saved);
        ::ReleaseDC(hwnd, dc);
    }
    return result;
}
LRESULT CALLBACK ProgressProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY) { ::RemoveWindowSubclass(hwnd, ProgressProc, id); }
    if (message == WM_PAINT || message == WM_PRINTCLIENT || message == WM_ERASEBKGND) {
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? ::BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wparam);
        RECT r{};
        ::GetClientRect(hwnd, &r);
        const float scale = ::GetDpiForWindow(hwnd) / 96.0f;
        platform::PaintBuffer buffer(dc, r);
        dc = buffer.dc();
        theme::FillSolid(dc, r, theme::Current().panel);
        ::InflateRect(&r, -theme::Scaled(4, scale), -theme::Scaled(4, scale));
        const float radius = std::max(1.0f, (r.bottom - r.top) / (2 * scale));
        theme::DrawMaterial(dc, r, radius, scale, theme::Current().recessed, -1);
        const auto low = ::SendMessageW(hwnd, PBM_GETRANGE, TRUE, 0), high = ::SendMessageW(hwnd, PBM_GETRANGE, FALSE, 0), value = ::SendMessageW(hwnd, PBM_GETPOS, 0, 0);
        ::InflateRect(&r, -theme::Scaled(3, scale), -theme::Scaled(3, scale));
        if (high > low && value > low) {
            r.right = r.left + static_cast<LONG>((r.right - r.left) * std::clamp(static_cast<double>(value - low) / (high - low), 0.0, 1.0));
            theme::FillRounded(dc, r, std::max(1L, (r.bottom - r.top) / 2), theme::Current().accent);
        }
        buffer.Present();
        if (message == WM_PAINT) { ::EndPaint(hwnd, &ps); }
        return message == WM_ERASEBKGND ? 1 : 0;
    }
    const auto result = [&] { NativeRedrawGuard redraw(hwnd,message==PBM_SETPOS || message==PBM_SETRANGE32); return ::DefSubclassProc(hwnd,message,wparam,lparam); }();
    if (message == PBM_SETPOS || message == theme::kChangedMessage) { ::InvalidateRect(hwnd, nullptr, FALSE); }
    return result;
}
}  // namespace
namespace {
LRESULT CALLBACK AccessibleNameProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        platform::ComPtr<IAccPropServices> properties;
        if (SUCCEEDED(::CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&properties)))) {
            const MSAAPROPID property = PROPID_ACC_NAME;
            properties->ClearHwndProps(hwnd, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, &property, 1);
        }
        ::RemoveWindowSubclass(hwnd, AccessibleNameProc, id);
    }
    return ::DefSubclassProc(hwnd, message, wparam, lparam);
}
}  // namespace
bool CaptureExclusionSupported() {
    // EXCLUDEFROMCAPTURE was introduced in Windows 10 build 19041. Older
    // systems interpret it as a black rectangle.
    static const bool supported = [] {
        using VersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        const auto version = reinterpret_cast<VersionFn>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        return version && version(&info) == 0 && (info.dwMajorVersion > 10 || (info.dwMajorVersion == 10 && info.dwBuildNumber >= 19041));
    }();
    return supported;
}
bool CanCaptureVisibleWindows() {
    // Where exclusion is missing, or one visible window lacks it, the caller
    // retains its hide fallback. Screen content is meant to be captured.
    if (!CaptureExclusionSupported()) { return false; }
    bool excluded = true;
    const bool enumerated = ::EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        DWORD process{};
        ::GetWindowThreadProcessId(hwnd, &process);
        if (process == ::GetCurrentProcessId() && ::IsWindowVisible(hwnd) && !IsScreenContent(hwnd)) {
            DWORD affinity{};
            if (!::GetWindowDisplayAffinity(hwnd, &affinity) || affinity != 0x11) {
                *reinterpret_cast<bool*>(param) = false;
                return FALSE;
            }
        }
        return TRUE;
    },
                                          reinterpret_cast<LPARAM>(&excluded));
    return enumerated && excluded;
}
void MarkAsScreenContent(HWND window) {
    if (window && !::SetPropW(window, kScreenContentProperty, reinterpret_cast<HANDLE>(1))) {
        platform::LogMessage(L"A window could not be marked as screen content (error %lu)", ::GetLastError());
    }
}
void RemoveScreenContentMark(HWND window) noexcept {
    if (window) { static_cast<void>(::RemovePropW(window, kScreenContentProperty)); }
}
bool IsScreenContent(HWND window) noexcept { return window && ::GetPropW(window, kScreenContentProperty) != nullptr; }
bool ExcludeFromCapture(HWND window) {
    if (!window) { return false; }
    const LONG_PTR style = ::GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((style & WS_EX_LAYERED) == 0) {
        // Opaque layered: the window looks and paints as before, but Windows
        // composes it through the path that keeps exclusion intact.
        ::SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_LAYERED);
        if (!::SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA)) {
            ::SetWindowLongPtrW(window, GWL_EXSTYLE, style);
            platform::LogMessage(L"A window could not be made layered before its capture exclusion (error %lu)", ::GetLastError());
        }
    }
    return ::SetWindowDisplayAffinity(window, 0x11) != FALSE;
}
void RoundWindow(HWND window, float radius) {
    if (!window) { return; }
    RECT r{};
    ::GetWindowRect(window, &r);
    const int d = theme::Scaled(radius * 2, ::GetDpiForWindow(window) / 96.0f);
    HRGN region = ::CreateRoundRectRgn(0, 0, r.right - r.left + 1, r.bottom - r.top + 1, d, d);
    HRGN current = ::CreateRectRgn(0, 0, 0, 0);
    const bool same = current && ::GetWindowRgn(window, current) != ERROR && ::EqualRgn(current, region);
    if (current) { ::DeleteObject(current); }
    if (region && (same || !::SetWindowRgn(window, region, TRUE))) { ::DeleteObject(region); }
}
void DrawSeparator(HDC dc, const RECT& r, float scale) {
    const LONG x = (r.left + r.right) / 2;
    theme::DrawSolidLine(dc, {x, r.top}, {x, r.bottom}, theme::Current().separator, theme::Scaled(1, scale));
}
bool DrawComboItem(const DRAWITEMSTRUCT& item) {
    if (item.CtlType != ODT_COMBOBOX) { return false; }
    const float scale = ::GetDpiForWindow(item.hwndItem) / 96.0f;
    const auto& p = theme::Current();
    theme::FillSolid(item.hDC, item.rcItem, p.panel);
    if (item.itemID == static_cast<UINT>(-1)) { return true; }
    wchar_t text[64]{};
    ::SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(text));
    RECT r = item.rcItem;
    ::InflateRect(&r, -theme::Scaled(5, scale), -theme::Scaled(3, scale));
    if (item.itemState & ODS_SELECTED) { theme::DrawMaterial(item.hDC, r, 6, scale, p.pressed, -1); }
    r.left += theme::Scaled(9, scale);
    theme::DrawLabel(item.hDC, r, text, item.itemState & ODS_SELECTED ? p.focus : p.text,
                     reinterpret_cast<HFONT>(::SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    return true;
}
void SetAccessibleName(HWND control, const wchar_t* name) {
    platform::ComPtr<IAccPropServices> properties;
    if (control && SUCCEEDED(::CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&properties))) &&
        ::SetWindowSubclass(control, AccessibleNameProc, kAccessibleNameSubclass, 0)) {
        properties->SetHwndPropStr(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_NAME, name);
    }
}

bool StyleProgress(HWND progress) {
    if (!progress) { return false; }
    ::SetWindowTheme(progress, L"", L"");
    ::SetWindowLongPtrW(progress, GWL_STYLE, ::GetWindowLongPtrW(progress, GWL_STYLE) & ~WS_BORDER);
    ::SetWindowLongPtrW(progress, GWL_EXSTYLE, ::GetWindowLongPtrW(progress, GWL_EXSTYLE) & ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
    ::SetWindowPos(progress, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return ::SetWindowSubclass(progress, ProgressProc, kProgressSubclass, 0) != FALSE;
}
bool StyleSlider(HWND slider) { return slider && ::SetWindowSubclass(slider, SliderProc, kSliderSubclass, 0); }
bool StyleLabel(HWND label) { return label && ::SetWindowSubclass(label, BufferedTextProc, kBufferedTextSubclass, 0); }
bool StyleInput(HWND input) {
    if (!input) { return false; }
    ::SetWindowTheme(input, L"", L"");
    ::SetWindowLongPtrW(input, GWL_STYLE, ::GetWindowLongPtrW(input, GWL_STYLE) & ~WS_BORDER);
    ::SetWindowLongPtrW(input, GWL_EXSTYLE, ::GetWindowLongPtrW(input, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    if (!::SetWindowSubclass(input, InputProc, kInputSubclass, 0)) { return false; }
    wchar_t cls[32]{};
    ::GetClassNameW(input, cls, 32);
    if (::lstrcmpiW(cls, L"EDIT") == 0) {
        if (!::SetWindowSubclass(input, BufferedTextProc, kBufferedTextSubclass, kBufferedEdit)) { return false; }
    } else {
        ::SetWindowLongPtrW(input, GWL_STYLE, ::GetWindowLongPtrW(input, GWL_STYLE) | WS_CLIPCHILDREN);
        COMBOBOXINFO info{sizeof(info)};
        if (::GetComboBoxInfo(input, &info) && info.hwndItem && info.hwndItem != input) {
            ::SetWindowTheme(info.hwndItem, L"", L"");
            if (!::SetWindowSubclass(info.hwndItem, BufferedTextProc, kBufferedTextSubclass, kBufferedEdit)) { return false; }
        }
    }
    ::SetWindowPos(input, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
    return true;
}

void StyleTooltip(HWND tooltip) {
    if (!tooltip) { return; }
    // Native tooltip colors are ignored while visual styles are active. Keep
    // its native behavior/provider, with application-owned accessible colors.
    ::SetWindowTheme(tooltip, L"", L"");
    ::SetWindowSubclass(tooltip, TooltipProc, kTooltipSubclass, 0);
    ::SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0,
                   theme::Scaled(theme::kTooltipMaxWidth, static_cast<float>(::GetDpiForWindow(tooltip)) / 96.0f));
    static_cast<void>(ExcludeFromCapture(tooltip));
    RefreshTooltip(tooltip);
}
void PaintNativePanel(HWND window, float scale) {
    PAINTSTRUCT paint{};
    HDC dc = ::BeginPaint(window, &paint);
    RECT client{};
    ::GetClientRect(window, &client);
    platform::PaintBuffer buffer(dc, client);
    theme::DrawPanel(buffer.dc(), client, scale);
    buffer.Present();
    ::EndPaint(window, &paint);
}
LRESULT ControlBackground(WPARAM dc, COLORREF text) {
    auto target = reinterpret_cast<HDC>(dc);
    ::SetTextColor(target, text);
    ::SetBkColor(target, theme::Current().panel);
    ::SetDCBrushColor(target, theme::Current().panel);
    return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
}
bool ActivateFocusedButton(MSG& message, HWND parent) {
    if (message.message != WM_KEYDOWN || message.wParam != VK_RETURN || !::IsChild(parent, message.hwnd)) { return false; }
    wchar_t name[16]{};
    ::GetClassNameW(message.hwnd, name, ARRAYSIZE(name));
    if (::lstrcmpiW(name, L"BUTTON") || !::IsWindowEnabled(message.hwnd)) { return false; }
    ::SendMessageW(message.hwnd, BM_CLICK, 0, 0);
    return true;
}
HMENU CreateAppearanceMenu(bool follows_system) {
    HMENU menu = ::CreatePopupMenu();
    if (menu) {
        ::AppendMenuW(menu, MF_STRING, kAppearanceSystem, strings::Get(IDS_SETTINGS_SAME_AS_WINDOWS));
        ::AppendMenuW(menu, MF_STRING, kAppearanceLight, strings::Get(IDS_SHELL_APPEARANCE_LIGHT));
        ::AppendMenuW(menu, MF_STRING, kAppearanceDark, strings::Get(IDS_SHELL_APPEARANCE_DARK));
        ::CheckMenuRadioItem(menu, kAppearanceLight, kAppearanceSystem,
                             follows_system                                      ? kAppearanceSystem
                             : theme::Preference() == app::AppearanceTheme::Dark ? kAppearanceDark
                                                                                 : kAppearanceLight,
                             MF_BYCOMMAND);
    }
    return menu;
}
struct MenuThemeScope::State {
    struct Item {
        HMENU menu;
        UINT index;
        UINT type;
        ULONG_PTR original;
        std::wstring text;
        bool submenu;
    };
    HWND owner{};
    HFONT font{};
    float scale{1};
    std::vector<std::unique_ptr<Item>> items;
    std::vector<HMENU> menus;
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR reference) {
        auto* self = reinterpret_cast<State*>(reference);
        if (message == WM_MEASUREITEM) {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (measure && measure->CtlType == ODT_MENU) {
                auto* item = reinterpret_cast<Item*>(measure->itemData);
                HDC dc = ::GetDC(hwnd);
                const auto old = ::SelectObject(dc, self->font);
                SIZE text{};
                ::GetTextExtentPoint32W(dc, item->text.c_str(), static_cast<int>(item->text.size()), &text);
                ::SelectObject(dc, old);
                ::ReleaseDC(hwnd, dc);
                measure->itemWidth = static_cast<UINT>(text.cx + theme::Scaled(theme::kMenuTextAllowance, self->scale));
                measure->itemHeight = theme::Scaled((item->type & MFT_SEPARATOR) ? theme::kMenuSeparatorHeight : theme::kMenuItemHeight, self->scale);
                return TRUE;
            }
        }
        if (message == WM_DRAWITEM) {
            const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (draw && draw->CtlType == ODT_MENU) {
                const auto* item = reinterpret_cast<Item*>(draw->itemData);
                const auto& p = theme::Current();
                theme::FillSolid(draw->hDC, draw->rcItem, p.panel);
                if (item->type & MFT_SEPARATOR) {
                    RECT line = draw->rcItem;
                    line.top = (line.top + line.bottom) / 2;
                    line.bottom = line.top + 1;
                    ::InflateRect(&line, -theme::Scaled(theme::kMenuSeparatorInset, self->scale), 0);
                    theme::FillSolid(draw->hDC, line, p.border);
                    return TRUE;
                }
                if (draw->itemState & ODS_SELECTED) {
                    RECT face = draw->rcItem;
                    ::InflateRect(&face, -theme::Scaled(theme::kMenuSelectionInsetX, self->scale), -theme::Scaled(theme::kMenuSelectionInsetY, self->scale));
                    theme::DrawMaterial(draw->hDC, face, theme::kMenuSelectionRadius, self->scale, p.panel, theme::kMenuSelectionDepth);
                }
                const auto ink = draw->itemState & ODS_DISABLED ? p.disabled : p.text;
                RECT label = draw->rcItem;
                label.left += theme::Scaled(theme::kMenuLabelInset, self->scale);
                label.right -= theme::Scaled(theme::kMenuLabelTrailing, self->scale);
                const auto split = item->text.find(L'\t');
                const auto title = item->text.substr(0, split);
                theme::DrawLabel(draw->hDC, label, title.c_str(), ink, self->font, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                if (split != std::wstring::npos) { theme::DrawLabel(draw->hDC, label, item->text.substr(split + 1).c_str(), p.muted_text, self->font, DT_RIGHT | DT_VCENTER | DT_SINGLELINE); }
                RECT icon = draw->rcItem;
                icon.left += theme::Scaled(theme::kMenuCheckInset, self->scale);
                icon.right = icon.left + theme::Scaled(theme::kMenuCheckSize, self->scale);
                icon.top = (icon.top + icon.bottom - theme::Scaled(theme::kMenuCheckSize, self->scale)) / 2;
                icon.bottom = icon.top + theme::Scaled(theme::kMenuCheckSize, self->scale);
                if (draw->itemState & ODS_CHECKED) { DrawIcon(draw->hDC, icon, Icon::Check, p.focus); }
                if (item->submenu) {
                    const LONG x = draw->rcItem.right - theme::Scaled(theme::kMenuArrowInset, self->scale), y = (draw->rcItem.top + draw->rcItem.bottom) / 2, d = theme::Scaled(theme::kMenuArrowSize, self->scale);
                    theme::DrawSolidLine(draw->hDC, {x - d, y - d}, {x, y}, ink, 1);
                    theme::DrawSolidLine(draw->hDC, {x, y}, {x - d, y + d}, ink, 1);
                    // Windows draws its own submenu arrow into the item after
                    // this message, in the system's colours: a second arrow
                    // beside this one. Clip the item so it cannot.
                    ::ExcludeClipRect(draw->hDC, draw->rcItem.left, draw->rcItem.top, draw->rcItem.right, draw->rcItem.bottom);
                }
                return TRUE;
            }
        }
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }
    void Add(HMENU menu) {
        menus.push_back(menu);
        MENUINFO background{sizeof(background)};
        background.fMask = MIM_BACKGROUND;
        background.hbrBack = ::CreateSolidBrush(theme::Current().panel);
        ::SetMenuInfo(menu, &background);
        brushes.push_back(background.hbrBack);
        const int count = ::GetMenuItemCount(menu);
        for (int i = 0; i < count; ++i) {
            wchar_t text[256]{};
            MENUITEMINFOW info{sizeof(info)};
            info.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STRING | MIIM_SUBMENU;
            info.dwTypeData = text;
            info.cch = 256;
            if (!::GetMenuItemInfoW(menu, i, TRUE, &info)) { continue; }
            auto item = std::make_unique<Item>(Item{menu, static_cast<UINT>(i), info.fType, info.dwItemData, text, info.hSubMenu != nullptr});
            if (info.hSubMenu) { Add(info.hSubMenu); }
            info.fMask = MIIM_FTYPE | MIIM_DATA;
            info.fType |= MFT_OWNERDRAW;
            info.dwItemData = reinterpret_cast<ULONG_PTR>(item.get());
            if (::SetMenuItemInfoW(menu, i, TRUE, &info)) { items.push_back(std::move(item)); }
        }
    }
    std::vector<HBRUSH> brushes;
    ~State() {
        if (owner) { ::RemoveWindowSubclass(owner, Proc, kMenuOwnerSubclass); }
        for (const auto& item : items) {
            MENUITEMINFOW info{sizeof(info)};
            info.fMask = MIIM_FTYPE | MIIM_DATA;
            info.fType = item->type;
            info.dwItemData = item->original;
            ::SetMenuItemInfoW(item->menu, item->index, TRUE, &info);
        }
        for (HMENU menu : menus) {
            MENUINFO info{sizeof(info)};
            info.fMask = MIM_BACKGROUND;
            info.hbrBack = nullptr;
            ::SetMenuInfo(menu, &info);
        }
        for (HBRUSH brush : brushes) { ::DeleteObject(brush); }
        if (font) { ::DeleteObject(font); }
    }
};
MenuThemeScope::MenuThemeScope(HWND owner, HMENU menu) : state_(std::make_unique<State>()) {
    state_->owner = owner;
    state_->scale = ::GetDpiForWindow(owner) / 96.0f;
    state_->font = theme::CreateUiFont(theme::kBodyFontSize, state_->scale, false);
    if (::SetWindowSubclass(owner, State::Proc, kMenuOwnerSubclass, reinterpret_cast<DWORD_PTR>(state_.get()))) { state_->Add(menu); }
}
MenuThemeScope::~MenuThemeScope() = default;

platform::SaveImageDialogText SaveImageDialogLabels() {
    return {strings::Get(IDS_SHELL_SAVE_IMAGE_TITLE), strings::Get(IDS_SHELL_FILE_TYPE_PNG), strings::Get(IDS_SHELL_FILE_TYPE_JPEG)};
}
std::optional<ImageSaveDestination> ChooseImageDestination(HWND owner, const app::AppSettings* settings,
                                                           bool allow_quick,
                                                           const std::function<void()>& before_dialog) {
    const std::wstring suggested = platform::TimestampedFileName(L"Easnap", L"png");
    // Quick save: straight into the chosen folder. If that folder is gone, or
    // the file cannot be written there, the dialog offers every other place.
    if (allow_quick && settings != nullptr && settings->quick_save_images && !settings->image_folder.empty()) {
        std::wstring quick = platform::UniqueFilePath(settings->image_folder, suggested);
        if (!quick.empty()) { return ImageSaveDestination{std::move(quick), true}; }
    }
    const std::wstring initial = settings != nullptr ? settings->ImageStartDirectory() : std::wstring();
    if (before_dialog) { before_dialog(); }
    auto chosen = platform::ShowSaveImageDialog(owner, suggested, initial, SaveImageDialogLabels());
    if (!chosen) { return std::nullopt; }
    return ImageSaveDestination{std::move(*chosen), false};
}
void RecordImageSaved(app::AppSettings* settings, const ImageSaveDestination& destination) {
    if (settings != nullptr && !destination.quick) {
        settings->last_image_directory = platform::DirectoryOf(destination.path);
    }
}
void RefreshCursor() {
    POINT point{};
    if (::GetCursorPos(&point) == FALSE) { return; }
    HWND under = ::WindowFromPoint(point);
    if (under == nullptr || ::GetWindowThreadProcessId(under, nullptr) != ::GetCurrentThreadId()) { return; }
    const LRESULT hit = ::SendMessageW(under, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
    ::SendMessageW(under, WM_SETCURSOR, reinterpret_cast<WPARAM>(under), MAKELPARAM(static_cast<WORD>(hit), WM_MOUSEMOVE));
}
PopupScope::PopupScope(HWND invoker) : invoker_(invoker) {
    ++popup_depth;
    hook_ = ::SetWindowsHookExW(WH_CALLWNDPROC, MenuHook, nullptr, ::GetCurrentThreadId());
}
PopupScope::~PopupScope() {
    if (hook_) { ::UnhookWindowsHookEx(hook_); }
    --popup_depth;
    for (const int key : {VK_ESCAPE, VK_RETURN, VK_SPACE}) { swallowed_keys[key] = (::GetAsyncKeyState(key) & 0x8000) != 0; }
    swallowed_left = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    swallowed_right = (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (::IsWindow(invoker_) && ::IsWindowVisible(invoker_)) { ::SetFocus(invoker_); }
}
bool PopupActive() noexcept { return popup_depth != 0; }
bool ConsumePopupInput(UINT message, WPARAM key) noexcept {
    const bool keyboard = message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR;
    const bool left = message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_LBUTTONUP;
    const bool right = message == WM_RBUTTONDOWN || message == WM_RBUTTONUP;
    if (PopupActive() && (keyboard || left || right)) { return true; }
    if (keyboard && key < swallowed_keys.size() && swallowed_keys[key]) {
        if (message == WM_KEYUP) { swallowed_keys[key] = false; }
        return true;
    }
    if (left && swallowed_left) {
        if (message == WM_LBUTTONUP) { swallowed_left = false; }
        return true;
    }
    if (right && swallowed_right) {
        if (message == WM_RBUTTONUP) { swallowed_right = false; }
        return true;
    }
    return false;
}
void ShowAppearanceMenu(HWND invoker) {
    const UINT command = theme::Preference() == app::AppearanceTheme::Light ? kAppearanceDark : kAppearanceLight;
    ::SendMessageW(::GetAncestor(invoker, GA_ROOT), WM_COMMAND, command, 0);
}
} // namespace easnap::ui
