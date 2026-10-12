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
#include <dwmapi.h>
#include <initguid.h>
#include <oleacc.h>
#include <gdiplus.h>

#include "strings/strings.h"

namespace easnap::ui {
namespace {
constexpr UINT_PTR kButtonSubclass = 0xEA51;
constexpr UINT_PTR kTooltipSubclass = 0xEA52;
struct ButtonData {
    ButtonStyle style;
    bool hot{false};
    float depth{1}, from{1}, target{1}, hover{0};
    ULONGLONG started{0};
    float duration{140};
};
void Advance(ButtonData& data) {
    if (!data.started) { return; }
    float t = std::clamp(static_cast<float>(::GetTickCount64()-data.started)/data.duration,0.0f,1.0f);
    t = t*t*(3-2*t);
    data.depth = data.from + (data.target-data.from)*t;
}
void Retarget(HWND hwnd, ButtonData& data) {
    Advance(data);
    const bool pressed=(::SendMessageW(hwnd,BM_GETSTATE,0,0)&BST_PUSHED)!=0;
    const bool checked=::SendMessageW(hwnd,BM_GETCHECK,0,0)==BST_CHECKED;
    const float target=!::IsWindowEnabled(hwnd) && !data.style.preserve_disabled_visual ? 0.0f : pressed ? -1.0f : checked ? -1.0f : data.hot ? 1.25f : 1.0f;
    if(target==data.target) { return; }
    data.from=data.depth; data.target=target; data.started=::GetTickCount64();
    data.duration=pressed ? 85.0f : checked ? 160.0f : 145.0f;
    if(theme::AnimationsEnabled() && ::IsWindowVisible(hwnd)) { ::SetTimer(hwnd,kButtonSubclass,15,nullptr); }
    else { data.depth=target; data.started=0; ::KillTimer(hwnd,kButtonSubclass); }
    ::InvalidateRect(hwnd,nullptr,FALSE);
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
    if (message == theme::kChangedMessage) { RefreshTooltip(window); return 0; }
    return ::DefSubclassProc(window, message, wparam, lparam);
}

void PaintButton(HWND hwnd, HDC dc, ButtonData& data) {
    Advance(data);
    const auto& p = theme::Current();
    const float scale = static_cast<float>(::GetDpiForWindow(hwnd)) / 96.0f;
    RECT client{};
    ::GetClientRect(hwnd, &client);
    theme::FillSolid(dc, client, p.panel);
    RECT face = client;
    ::InflateRect(&face, -theme::Scaled(6, scale), -theme::Scaled(6, scale));
    const bool enabled = ::IsWindowEnabled(hwnd) != FALSE || data.style.preserve_disabled_visual;
    const LRESULT state = ::SendMessageW(hwnd, BM_GETSTATE, 0, 0);
    const bool pressed = (state & BST_PUSHED) != 0;
    const bool checked = ::SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool focused = ::GetFocus() == hwnd;
    COLORREF fill = checked || pressed ? p.pressed : (data.hot ? p.hover : p.raised);
    COLORREF ink = enabled ? p.text : p.disabled;
    if (checked && enabled) { ink = p.focus; }
    if (enabled && data.style.role == ButtonRole::Primary) { ink = p.accent; }
    if (enabled && data.style.role == ButtonRole::Destructive) { ink = p.error; }
    if (enabled && data.style.role == ButtonRole::Success) { ink = p.success; }
    if (p.high_contrast && pressed) { fill = p.accent; ink = p.on_accent; }
    const float face_radius=data.style.circular ? std::min(face.right-face.left,face.bottom-face.top)/(2*scale) : theme::kControlRadius;
    theme::DrawMaterial(dc, face, face_radius, scale, fill, data.depth, data.style.circular);
    if (focused && !data.style.hide_focus_line && !(::SendMessageW(hwnd,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)) {
        // Keyboard focus is a neutral underline, independent of latched depth.
        const LONG inset=theme::Scaled(10,scale);
        theme::DrawSolidLine(dc,{face.left+inset,face.bottom+2},{face.right-inset,face.bottom+2},p.text,1);
    }
    const LONG size = theme::Scaled(data.style.circular ? 12.0f : 18.0f, scale);
    const LONG cy = (face.top + face.bottom) / 2;
    RECT icon{face.left + theme::Scaled(10, scale), cy - size / 2, 0, cy - size / 2 + size};
    if (data.style.icon_only || data.style.swatch) { icon.left = (face.left + face.right - size) / 2; }
    if (data.style.icon_only && data.style.dropdown) { icon.left -= theme::Scaled(5, scale); }
    icon.right = icon.left + size;
    if (data.style.sample != 0) {
        icon.left = (face.left + face.right - size) / 2;
        icon.right = icon.left + size;
        if (data.style.sample == 1) {
            theme::DrawSolidLine(dc, {icon.left, cy}, {icon.right, cy}, ink, theme::Scaled(data.style.sample_size, scale));
        } else {
            const LONG radius = theme::Scaled(2 + data.style.sample_size / 10.0f, scale);
            const LONG cx = (face.left + face.right) / 2;
            theme::FillRounded(dc, {cx-radius, cy-radius, cx+radius, cy+radius}, radius, ink);
        }
    } else if (data.style.swatch) {
        theme::FillRoundedBordered(dc, icon, theme::Scaled(3, scale), data.style.content_color, p.control_border, 1);
        if (checked) { DrawIcon(dc,icon,Icon::Check,GetRValue(data.style.content_color)+GetGValue(data.style.content_color)+GetBValue(data.style.content_color)>500 ? RGB(30,30,30):RGB(255,255,255),scale); }
    } else if (data.style.icon != Icon::None) {
        DrawIcon(dc, icon, data.style.icon, ink, scale);
    }
    if (data.style.dropdown) {
        const LONG chevron = theme::Scaled(8, scale);
        const LONG right = face.right - theme::Scaled(3, scale);
        DrawIcon(dc, {right-chevron, cy-chevron/2, right, cy+chevron/2}, Icon::ChevronDown, ink, scale);
    }
    if (!data.style.icon_only && !data.style.swatch) {
        wchar_t text[256]{};
        ::GetWindowTextW(hwnd, text, ARRAYSIZE(text));
        RECT label = face;
        ::InflateRect(&label, -theme::Scaled(6, scale), 0);
        UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
        if (data.style.icon != Icon::None) { label.left = icon.right + theme::Scaled(6, scale); format = DT_LEFT | DT_VCENTER | DT_SINGLELINE; }
        const auto type = ::GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK;
        if ((type == BS_AUTOCHECKBOX || type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON) && data.style.icon == Icon::None && !data.style.segmented) {
            RECT check{face.left + theme::Scaled(10, scale), cy - theme::Scaled(5, scale),
                face.left + theme::Scaled(20, scale), cy + theme::Scaled(5, scale)};
            if (checked && type==BS_AUTOCHECKBOX) { DrawIcon(dc, check, Icon::Check, ink, scale); }
            else if (checked) { theme::FillRounded(dc,check,theme::Scaled(5,scale),ink); }
            label.left = check.right + theme::Scaled(8, scale);
            format = DT_LEFT | DT_VCENTER | DT_SINGLELINE;
        }
        theme::DrawLabel(dc, label, text, ink, reinterpret_cast<HFONT>(::SendMessageW(hwnd, WM_GETFONT, 0, 0)), format);
    }
}

LRESULT CALLBACK ButtonProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR reference) {
    if(message==WM_PRINT) { return ::DefWindowProcW(hwnd,message,wparam,lparam); }
    auto* data = reinterpret_cast<ButtonData*>(reference);
    if(message==WM_SETTEXT && lparam) {
        const int length=::GetWindowTextLengthW(hwnd);
        std::wstring current(static_cast<size_t>(length)+1,L'\0'); ::GetWindowTextW(hwnd,current.data(),length+1);
        if(::lstrcmpW(current.c_str(),reinterpret_cast<const wchar_t*>(lparam))==0) { return TRUE; }
    }
    if (message == WM_NCDESTROY) {
        ::KillTimer(hwnd, kButtonSubclass);
        ::RemoveWindowSubclass(hwnd, ButtonProc, kButtonSubclass);
        delete data;
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }
    if (message == WM_TIMER && wparam == kButtonSubclass) {
        Advance(*data);
        if (::GetTickCount64()-data->started >= static_cast<ULONGLONG>(data->duration)) {
            data->depth=data->target; data->started=0; ::KillTimer(hwnd,kButtonSubclass);
        }
        ::InvalidateRect(hwnd,nullptr,FALSE); return 0;
    }
    if (message == WM_PAINT || message == WM_PRINTCLIENT || message == WM_ERASEBKGND) {
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? ::BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wparam);
        RECT r{}; ::GetClientRect(hwnd,&r);
        // USER32 can request erase after WINDOWPOSCHANGED's paint, with the
        // final newly exposed region. Publish the complete control to that DC.
        PaintBuffer buffer(dc,r);
        PaintButton(hwnd,buffer.dc(),*data); buffer.Present();
        if(message == WM_PAINT) { ::EndPaint(hwnd, &paint); }
        return message == WM_ERASEBKGND ? 1 : 0;
    }
    if (message == WM_MOUSEMOVE && !data->hot) {
        data->hot = true;
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
        Retarget(hwnd,*data);
        ::InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (message == WM_MOUSELEAVE) { data->hot = false; Retarget(hwnd,*data); ::InvalidateRect(hwnd, nullptr, FALSE); }
    if(message==WM_SHOWWINDOW) { NativeRedrawGuard::VisibilityChanged(hwnd,wparam!=0); }
    // Input dispatch must remain visible: automatic radio groups enumerate
    // visible siblings during activation. Their nested state messages own paint.
    const bool changes_pixels=message==BM_SETCHECK || message==BM_SETSTATE || message==BM_SETSTYLE ||
        message==WM_SETFOCUS || message==WM_KILLFOCUS || message==WM_ENABLE || message==WM_SETTEXT;
    const LRESULT result = [&] {
        NativeRedrawGuard redraw(hwnd,changes_pixels);
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }();
    if(!::IsWindow(hwnd)) { return result; }
    switch (message) {
        case BM_SETCHECK: case BM_SETSTATE: case WM_SETFOCUS: case WM_KILLFOCUS:
        case WM_ENABLE: case WM_SETTEXT: case WM_THEMECHANGED: case theme::kChangedMessage:
        case WM_KEYDOWN: case WM_KEYUP: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            Retarget(hwnd,*data); ::InvalidateRect(hwnd, nullptr, FALSE); break;
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
            if (::lstrcmpW(name, L"#32768") == 0) { ::SetWindowDisplayAffinity(call->hwnd, 0x11); }
        }
    }
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
}
}

namespace { constexpr wchar_t kRedrawGuard[] = L"Easnap.NativeRedrawGuard"; }
NativeRedrawGuard::NativeRedrawGuard(HWND window,bool enabled) {
    if(!enabled || !window || ::GetPropW(window,kRedrawGuard) || !(::GetWindowLongPtrW(window,GWL_STYLE)&WS_VISIBLE)) { return; }
    if(!::SetPropW(window,kRedrawGuard,this)) { return; }
    window_=window;
    ::SendMessageW(window_,WM_SETREDRAW,FALSE,0);
}
NativeRedrawGuard::~NativeRedrawGuard() {
    if(!window_ || !::IsWindow(window_) || ::GetPropW(window_,kRedrawGuard)!=this) { return; }
    ::RemovePropW(window_,kRedrawGuard);
    ::SendMessageW(window_,WM_SETREDRAW,TRUE,0);
    if(!visible_) { ::ShowWindow(window_,SW_HIDE); }
    ::InvalidateRect(window_,nullptr,FALSE);
}
void NativeRedrawGuard::VisibilityChanged(HWND window,bool visible) {
    if(auto* guard=static_cast<NativeRedrawGuard*>(::GetPropW(window,kRedrawGuard))) { guard->visible_=visible; }
}
void DrawIcon(HDC dc, const RECT& r, Icon icon, COLORREF color, float scale) {
    static const ULONG_PTR token=[] { ULONG_PTR result{}; Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&result,&input,nullptr); return result; }();
    if (!token) { return; }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.TranslateTransform(static_cast<float>(r.left),static_cast<float>(r.top));
    graphics.ScaleTransform((r.right-r.left)/24.0f,(r.bottom-r.top)/24.0f);
    Gdiplus::Pen pen(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color)), 1.8f);
    pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound); pen.SetLineJoin(Gdiplus::LineJoinRound);
    const auto line=[&](int x,int y,int xx,int yy) { graphics.DrawLine(&pen,x,y,xx,yy); };
    const auto box=[&](int x,int y,int xx,int yy) { graphics.DrawRectangle(&pen,x,y,xx-x,yy-y); };
    const auto circle=[&](int x,int y,int xx,int yy) { graphics.DrawEllipse(&pen,x,y,xx-x,yy-y); };
    (void)scale;
    switch (icon) {
        case Icon::Record: circle(4,4,20,20); circle(8,8,16,16); break;
        case Icon::Pause: box(5,3,9,21); box(15,3,19,21); break;
        case Icon::Play: line(6,3,21,12); line(21,12,6,21); line(6,21,6,3); break;
        case Icon::Stop: box(5,5,19,19); break;
        case Icon::SystemAudio: box(2,8,7,16); line(7,8,13,3); line(13,3,13,21); line(13,21,7,16); line(17,7,20,10); line(20,10,20,14); line(20,14,17,17); break;
        case Icon::Microphone: circle(8,2,16,16); line(5,11,5,15); line(5,15,12,19); line(12,19,19,15); line(19,15,19,11); line(12,19,12,23); line(8,23,16,23); break;
        case Icon::Cursor: line(4,2,4,20); line(4,20,9,15); line(9,15,13,23); line(13,23,16,21); line(16,21,12,14); line(12,14,21,14); line(21,14,4,2); break;
        case Icon::Copy: box(8,8,21,22); line(15,5,15,2); line(15,2,2,2); line(2,2,2,16); line(2,16,5,16); break;
        case Icon::Save: line(12,2,12,16); line(6,10,12,16); line(12,16,18,10); line(3,18,3,22); line(3,22,21,22); line(21,22,21,18); break;
        case Icon::Discard: line(5,5,19,19); line(5,19,19,5); break;
        case Icon::Clear: line(4,5,20,5); line(9,2,15,2); line(6,8,7,21); line(7,21,17,21); line(17,21,18,8); line(10,9,10,18); line(14,9,14,18); break;
        case Icon::Appearance: circle(3,3,21,21); line(12,3,12,21); line(15,6,15,18); line(18,8,18,16); break;
        case Icon::Sun: circle(7,7,17,17); line(12,0,12,4); line(12,20,12,24); line(0,12,4,12); line(20,12,24,12); line(3,3,5,5); line(19,19,21,21); line(3,21,5,19); line(19,5,21,3); break;
        case Icon::Moon: circle(3,3,21,21); line(12,3,8,9); line(8,9,11,16); line(11,16,21,17); break;
        case Icon::Pen: line(3,21,6,13); line(6,13,17,2); line(17,2,22,7); line(22,7,11,18); line(11,18,3,21); line(14,5,19,10); break;
        case Icon::Arrow: line(3,21,21,3); line(11,3,21,3); line(21,3,21,13); break;
        case Icon::Rectangle: box(2,5,22,19); break;
        case Icon::Ellipse: circle(2,4,22,20); break;
        case Icon::Mosaic: {
            box(2,2,22,22);
            Gdiplus::SolidBrush fill(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color)));
            for(int y=0;y<3;++y) for(int x=0;x<3;++x) if((x+y)%2==0) { graphics.FillRectangle(&fill,5+x*5,5+y*5,4,4); }
            break;
        }
        case Icon::Undo: line(8,3,3,8); line(3,8,8,13); graphics.DrawBezier(&pen,3,8,24,3,25,21,10,21); break;
        case Icon::Redo: line(16,3,21,8); line(21,8,16,13); graphics.DrawBezier(&pen,21,8,0,3,-1,21,14,21); break;
        case Icon::Check: line(3,12,9,18); line(9,18,21,5); break;
        case Icon::Line: line(4,20,20,4); break;
        case Icon::Droplet: graphics.DrawBezier(&pen,12,2,11,7,3,11,5,17); graphics.DrawBezier(&pen,5,17,8,25,20,22,20,15); graphics.DrawBezier(&pen,20,15,20,11,14,7,12,2); break;
        case Icon::Tag: line(3,3,13,3); line(13,3,22,12); line(22,12,12,22); line(12,22,3,13); line(3,13,3,3); circle(6,6,9,9); break;
        case Icon::Spotlight: box(2,2,22,22); circle(7,7,17,17); break;
        case Icon::Watermark: line(2,10,10,2); line(2,19,19,2); line(5,24,24,5); line(14,24,24,14); break;
        case Icon::Text:
        case Icon::TextFill:
        case Icon::TextOutline: {
            Gdiplus::GraphicsPath glyph;
            glyph.AddLine(5,5,19,5); glyph.AddLine(19,5,19,9); glyph.AddLine(19,9,17,9);
            glyph.AddLine(17,9,17,7); glyph.AddLine(17,7,14,7); glyph.AddLine(14,7,14,18);
            glyph.AddLine(14,18,17,18); glyph.AddLine(17,18,17,20); glyph.AddLine(17,20,7,20);
            glyph.AddLine(7,20,7,18); glyph.AddLine(7,18,10,18); glyph.AddLine(10,18,10,7);
            glyph.AddLine(10,7,7,7); glyph.AddLine(7,7,7,9); glyph.AddLine(7,9,5,9); glyph.CloseFigure();
            Gdiplus::Color ink(255,GetRValue(color),GetGValue(color),GetBValue(color));
            if(icon==Icon::TextFill) {
                Gdiplus::GraphicsPath square;
                square.AddArc(3,3,4,4,180,90); square.AddArc(17,3,4,4,270,90);
                square.AddArc(17,17,4,4,0,90); square.AddArc(3,17,4,4,90,90); square.CloseFigure();
                Gdiplus::SolidBrush background(ink); graphics.FillPath(&background,&square);
                const auto c=theme::Current().panel; ink=Gdiplus::Color(255,GetRValue(c),GetGValue(c),GetBValue(c));
            }
            if(icon==Icon::TextOutline) { pen.SetWidth(1.2f); graphics.DrawPath(&pen,&glyph); }
            else { Gdiplus::SolidBrush fill(ink); graphics.FillPath(&fill,&glyph); }
            break;
        }
        case Icon::Hand: {
            Gdiplus::GraphicsPath path;
            path.AddLine(9,12,9,4); path.AddBezier(9,4,9,1,13,1,13,4);
            path.AddLine(13,4,13,10); path.AddBezier(13,10,19,9,22,12,21,16);
            path.AddLine(21,16,19,22); path.AddLine(19,22,10,22); path.AddLine(10,22,3,15);
            path.AddBezier(3,15,1,11,5,10,9,14); graphics.DrawPath(&pen,&path); break;
        }
        case Icon::Region: box(5,5,19,19); box(2,2,7,7); box(17,2,22,7); box(2,17,7,22); box(17,17,22,22); break;
        case Icon::RoundedRectangle: {
            Gdiplus::GraphicsPath path; path.AddArc(3,3,7,7,180,90); path.AddArc(14,3,7,7,270,90);
            path.AddArc(14,14,7,7,0,90); path.AddArc(3,14,7,7,90,90); path.CloseFigure(); graphics.DrawPath(&pen,&path); break;
        }
        case Icon::ChevronDown: line(6,9,12,15); line(12,15,18,9); break;
        case Icon::Minimize: line(5,16,19,16); break;
        case Icon::Maximize: box(5,5,19,19); break;
        case Icon::Scissors: circle(3,15,9,21); circle(15,15,21,21); line(8,16,19,3); line(16,16,5,3); break;
        case Icon::Help: {
            circle(3,3,21,21);
            graphics.DrawBezier(&pen,9,10,9,5,15,5,15,10); graphics.DrawBezier(&pen,15,10,15,12,12,12,12,15);
            Gdiplus::SolidBrush dot(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color)));
            graphics.FillEllipse(&dot,11,17,2,2);
            break;
        }
        case Icon::None: break;
    }

}

bool StyleButton(HWND button, ButtonStyle style) {
    if (!button) { return false; }
    auto* data = new (std::nothrow) ButtonData{style};
    if (!data) { return false; }
    if (!::SetWindowSubclass(button, ButtonProc, kButtonSubclass, reinterpret_cast<DWORD_PTR>(data))) { delete data; return false; }
    return true;
}
void UpdateButtonStyle(HWND button, ButtonStyle style) {
    DWORD_PTR data{};
    if (::GetWindowSubclass(button, ButtonProc, kButtonSubclass, &data)) {
        reinterpret_cast<ButtonData*>(data)->style = style;
        ::InvalidateRect(button, nullptr, FALSE);
    }
}
namespace {
LRESULT CALLBACK BufferedTextProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR) {
    if(message==WM_PRINT) { return ::DefWindowProcW(hwnd,message,wparam,lparam); }
    if(message==WM_NCDESTROY) { ::RemoveWindowSubclass(hwnd,BufferedTextProc,id); return ::DefSubclassProc(hwnd,message,wparam,lparam); }
    if(message==WM_SHOWWINDOW) { NativeRedrawGuard::VisibilityChanged(hwnd,wparam!=0); }
    if(message==WM_PAINT || message==WM_PRINTCLIENT || message==WM_ERASEBKGND) {
        PAINTSTRUCT ps{}; HDC target=message==WM_PAINT ? ::BeginPaint(hwnd,&ps):reinterpret_cast<HDC>(wparam);
        RECT r{}; ::GetClientRect(hwnd,&r);
        const bool caret=::HideCaret(hwnd)!=FALSE;
        { PaintBuffer buffer(target,r); theme::FillSolid(buffer.dc(),r,theme::Current().panel);
          ::DefSubclassProc(hwnd,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(buffer.dc()),PRF_CLIENT); }
        if(caret) { ::ShowCaret(hwnd); }
        if(message==WM_PAINT) { ::EndPaint(hwnd,&ps); } return message==WM_ERASEBKGND ? 1 : 0;
    }
    const bool update=(message>=WM_KEYFIRST && message<=WM_KEYLAST) || (message>=WM_MOUSEFIRST && message<=WM_MOUSELAST) ||
        message==WM_SETTEXT || message==WM_SETFONT || message==WM_ENABLE || message==WM_SETFOCUS || message==WM_KILLFOCUS ||
        message==EM_REPLACESEL || message==EM_SETSEL || message==WM_CUT || message==WM_PASTE || message==WM_CLEAR ||
        message==WM_UNDO || message==EM_UNDO || message==WM_IME_COMPOSITION || message==WM_SIZE;
    NativeRedrawGuard guard(hwnd,update);
    if(message==theme::kChangedMessage) { ::InvalidateRect(hwnd,nullptr,FALSE); }
    return ::DefSubclassProc(hwnd,message,wparam,lparam);
}
LRESULT CALLBACK SliderProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR) {
    if(message==WM_NCDESTROY) { ::RemoveWindowSubclass(hwnd,SliderProc,id); }
    if(message==WM_ERASEBKGND) { return 1; }
    if(message==WM_PAINT || message==WM_PRINTCLIENT) {
        PAINTSTRUCT ps{}; HDC dc=message==WM_PAINT ? ::BeginPaint(hwnd,&ps) : reinterpret_cast<HDC>(wparam);
        RECT r{},thumb{},track{}; ::GetClientRect(hwnd,&r);
        HDC destination=dc, buffer=::CreateCompatibleDC(dc);
        HBITMAP bitmap=::CreateCompatibleBitmap(dc,r.right,r.bottom);
        HGDIOBJ old=nullptr;
        if(buffer && bitmap) { old=::SelectObject(buffer,bitmap); dc=buffer; }
        ::SendMessageW(hwnd,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
        ::SendMessageW(hwnd,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&track));
        const float scale=::GetDpiForWindow(hwnd)/96.0f;
        const auto& p=theme::Current(); theme::FillSolid(dc,r,p.panel);
        const LONG cy=(r.top+r.bottom)/2;
        track.top=cy-theme::Scaled(3,scale); track.bottom=cy+theme::Scaled(3,scale);
        theme::DrawMaterial(dc,track,3,scale,p.recessed,-1);
        const LONG x=(thumb.left+thumb.right)/2;
        RECT fill=track; fill.right=x; ::InflateRect(&fill,0,-theme::Scaled(1,scale));
        theme::FillRounded(dc,fill,theme::Scaled(2,scale),p.accent);
        const LONG radius=theme::Scaled(6,scale);
        theme::DrawMaterial(dc,{x-radius,cy-radius,x+radius,cy+radius},6,scale,p.raised,0.9f);
        theme::FillRounded(dc,{x-radius/2,cy-radius/2,x+radius/2,cy+radius/2},radius/2,p.accent);
        if(old) { ::BitBlt(destination,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY); ::SelectObject(buffer,old); }
        if(bitmap) { ::DeleteObject(bitmap); } if(buffer) { ::DeleteDC(buffer); }
        if(message==WM_PAINT) { ::EndPaint(hwnd,&ps); } return 0;
    }
    const bool input=message==WM_MOUSEMOVE || message==WM_LBUTTONDOWN || message==WM_LBUTTONUP || message==WM_KEYDOWN || message==TBM_SETPOS;
    const bool suspend=input && (::GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_VISIBLE)!=0;
    if(suspend) { ::DefSubclassProc(hwnd,WM_SETREDRAW,FALSE,0); }
    const LRESULT result=::DefSubclassProc(hwnd,message,wparam,lparam);
    if(suspend) { ::DefSubclassProc(hwnd,WM_SETREDRAW,TRUE,0); }
    if(message==WM_MOUSEMOVE || message==WM_LBUTTONDOWN || message==WM_LBUTTONUP || message==WM_KEYDOWN ||
        message==WM_SETFOCUS || message==WM_KILLFOCUS || message==TBM_SETPOS || message==theme::kChangedMessage) { ::InvalidateRect(hwnd,nullptr,FALSE); }
    return result;
}
LRESULT CALLBACK InputProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR) {
    if(message==WM_NCDESTROY) { ::RemoveWindowSubclass(hwnd,InputProc,id); }
    if(message==WM_CTLCOLOREDIT || message==WM_CTLCOLORLISTBOX) { return ControlBackground(wparam,theme::Current().text); }
    wchar_t class_name[32]{}; ::GetClassNameW(hwnd,class_name,32);
    const bool combo=::lstrcmpiW(class_name,L"COMBOBOX")==0;
    if(combo && message==WM_PRINT) { return ::DefWindowProcW(hwnd,message,wparam,lparam); }
    if(combo && (message==WM_NCPAINT || message==WM_ERASEBKGND)) { return 1; }
    if(combo && (message==WM_PAINT || message==WM_PRINTCLIENT)) {
        PAINTSTRUCT ps{}; HDC dc=message==WM_PAINT ? ::BeginPaint(hwnd,&ps):reinterpret_cast<HDC>(wparam);
        RECT r{}; ::GetClientRect(hwnd,&r); const float scale=::GetDpiForWindow(hwnd)/96.0f;
        PaintBuffer buffer(dc,r); dc=buffer.dc();
        theme::FillSolid(dc,r,theme::Current().panel); RECT face=r; ::InflateRect(&face,-1,-1);
        theme::DrawMaterial(dc,face,5,scale,theme::Current().panel,-1);
        COMBOBOXINFO info{sizeof(info)}; ::GetComboBoxInfo(hwnd,&info);
        wchar_t value[32]{}; ::GetWindowTextW(hwnd,value,32);
        RECT label=r; label.left+=theme::Scaled(7,scale); label.right=info.rcButton.left-theme::Scaled(3,scale);
        if(!info.hwndItem || info.hwndItem==hwnd) {
            theme::DrawLabel(dc,label,value,theme::Current().text,reinterpret_cast<HFONT>(::SendMessageW(hwnd,WM_GETFONT,0,0)),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        }
        const LONG x=(info.rcButton.left+info.rcButton.right)/2,y=(r.top+r.bottom)/2,d=theme::Scaled(3,scale);
        theme::DrawSolidLine(dc,{x-d,y-1},{x,y+d-1},theme::Current().text,theme::Scaled(1,scale));
        theme::DrawSolidLine(dc,{x,y+d-1},{x+d,y-1},theme::Current().text,theme::Scaled(1,scale));

        buffer.Present(); if(message==WM_PAINT) { ::EndPaint(hwnd,&ps); } return 0;
    }
    if(message==WM_NCCALCSIZE) {
        const auto result=::DefSubclassProc(hwnd,message,wparam,lparam);
        wchar_t name[32]{}; ::GetClassNameW(hwnd,name,32);
        if(::lstrcmpW(name,L"Edit")==0 && lparam) {
            auto* rect=wparam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]:reinterpret_cast<RECT*>(lparam);
            ::InflateRect(rect,-theme::Scaled(5,::GetDpiForWindow(hwnd)/96.0f),-theme::Scaled(3,::GetDpiForWindow(hwnd)/96.0f));
        }
        return result;
    }
    const LRESULT result=::DefSubclassProc(hwnd,message,wparam,lparam);
    if(message==WM_NCPAINT || message==WM_PAINT || message==WM_SETFOCUS || message==WM_KILLFOCUS || message==theme::kChangedMessage) {
        HDC dc=::GetWindowDC(hwnd); RECT r{},client{}; ::GetWindowRect(hwnd,&r); ::GetClientRect(hwnd,&client);
        ::MapWindowPoints(hwnd,nullptr,reinterpret_cast<POINT*>(&client),2); ::OffsetRect(&client,-r.left,-r.top); ::OffsetRect(&r,-r.left,-r.top);
        const auto saved=::SaveDC(dc); ::ExcludeClipRect(dc,client.left,client.top,client.right,client.bottom);
        RECT face=r; ::InflateRect(&face,-1,-1);
        theme::DrawMaterial(dc,face,4,::GetDpiForWindow(hwnd)/96.0f,theme::Current().panel,-1);

        ::RestoreDC(dc,saved); ::ReleaseDC(hwnd,dc);
    }
    return result;
}
LRESULT CALLBACK ProgressProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR) {
    if(message==WM_NCDESTROY) { ::RemoveWindowSubclass(hwnd,ProgressProc,id); }
    if(message==WM_PAINT || message==WM_PRINTCLIENT || message==WM_ERASEBKGND) {
        PAINTSTRUCT ps{}; HDC dc=message==WM_PAINT ? ::BeginPaint(hwnd,&ps):reinterpret_cast<HDC>(wparam);
        RECT r{}; ::GetClientRect(hwnd,&r); const float scale=::GetDpiForWindow(hwnd)/96.0f;
        PaintBuffer buffer(dc,r); dc=buffer.dc();
        theme::FillSolid(dc,r,theme::Current().panel); ::InflateRect(&r,-theme::Scaled(4,scale),-theme::Scaled(4,scale));
        const float radius=std::max(1.0f,(r.bottom-r.top)/(2*scale));
        theme::DrawMaterial(dc,r,radius,scale,theme::Current().recessed,-1);
        const auto low=::SendMessageW(hwnd,PBM_GETRANGE,TRUE,0),high=::SendMessageW(hwnd,PBM_GETRANGE,FALSE,0),value=::SendMessageW(hwnd,PBM_GETPOS,0,0);
        ::InflateRect(&r,-theme::Scaled(3,scale),-theme::Scaled(3,scale));
        if(high>low && value>low) {
            r.right=r.left+static_cast<LONG>((r.right-r.left)*std::clamp(static_cast<double>(value-low)/(high-low),0.0,1.0));
            theme::FillRounded(dc,r,std::max(1L,(r.bottom-r.top)/2),theme::Current().accent);
        }
        buffer.Present(); if(message==WM_PAINT) { ::EndPaint(hwnd,&ps); } return message==WM_ERASEBKGND ? 1 : 0;
    }
    const auto result=[&] { NativeRedrawGuard redraw(hwnd,message==PBM_SETPOS || message==PBM_SETRANGE32); return ::DefSubclassProc(hwnd,message,wparam,lparam); }();
    if(message==PBM_SETPOS || message==theme::kChangedMessage) { ::InvalidateRect(hwnd,nullptr,FALSE); }
    return result;
}
}
namespace {
LRESULT CALLBACK AccessibleNameProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR) {
    if(message==WM_NCDESTROY) {
        platform::ComPtr<IAccPropServices> properties;
        if(SUCCEEDED(::CoCreateInstance(CLSID_AccPropServices,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&properties)))) {
            const MSAAPROPID property=PROPID_ACC_NAME;
            properties->ClearHwndProps(hwnd,static_cast<DWORD>(OBJID_CLIENT),CHILDID_SELF,&property,1);
        }
        ::RemoveWindowSubclass(hwnd,AccessibleNameProc,id);
    }
    return ::DefSubclassProc(hwnd,message,wparam,lparam);
}
}
bool CaptureExclusionSupported() {
    // EXCLUDEFROMCAPTURE was introduced in Windows 10 build 19041. Older
    // systems interpret it as a black rectangle.
    static const bool supported=[] {
        using VersionFn=LONG(WINAPI*)(OSVERSIONINFOW*);
        const auto version=reinterpret_cast<VersionFn>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
        OSVERSIONINFOW info{}; info.dwOSVersionInfoSize=sizeof(info);
        return version && version(&info)==0 && (info.dwMajorVersion>10 || (info.dwMajorVersion==10 && info.dwBuildNumber>=19041));
    }();
    return supported;
}
bool CanCaptureVisibleWindows() {
    // Where exclusion is missing, or one visible window lacks it, the caller
    // retains its hide fallback.
    if(!CaptureExclusionSupported()) { return false; }
    bool excluded=true;
    const bool enumerated=::EnumWindows([](HWND hwnd,LPARAM param)->BOOL {
        DWORD process{}; ::GetWindowThreadProcessId(hwnd,&process);
        if(process==::GetCurrentProcessId() && ::IsWindowVisible(hwnd)) {
            DWORD affinity{};
            if(!::GetWindowDisplayAffinity(hwnd,&affinity) || affinity!=0x11) { *reinterpret_cast<bool*>(param)=false; return FALSE; }
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&excluded));
    return enumerated && excluded;
}
void RoundWindow(HWND window,float radius) {
    if(!window) { return; }
    RECT r{}; ::GetWindowRect(window,&r); const int d=theme::Scaled(radius*2,::GetDpiForWindow(window)/96.0f);
    HRGN region=::CreateRoundRectRgn(0,0,r.right-r.left+1,r.bottom-r.top+1,d,d);
    HRGN current=::CreateRectRgn(0,0,0,0);
    const bool same=current && ::GetWindowRgn(window,current)!=ERROR && ::EqualRgn(current,region);
    if(current) { ::DeleteObject(current); }
    if(region && (same || !::SetWindowRgn(window,region,TRUE))) { ::DeleteObject(region); }
}
void DrawSeparator(HDC dc,const RECT& r,float scale) {
    const LONG x=(r.left+r.right)/2;
    theme::DrawSolidLine(dc,{x,r.top},{x,r.bottom},theme::Current().high_contrast ? theme::Current().text :
        theme::Mix(theme::Current().panel,theme::Current().text,0.55f),theme::Scaled(1,scale));
}
bool DrawComboItem(const DRAWITEMSTRUCT& item) {
    if(item.CtlType!=ODT_COMBOBOX) { return false; }
    const float scale=::GetDpiForWindow(item.hwndItem)/96.0f; const auto& p=theme::Current();
    theme::FillSolid(item.hDC,item.rcItem,p.panel);
    if(item.itemID==static_cast<UINT>(-1)) { return true; }
    wchar_t text[64]{}; ::SendMessageW(item.hwndItem,CB_GETLBTEXT,item.itemID,reinterpret_cast<LPARAM>(text));
    RECT r=item.rcItem; ::InflateRect(&r,-theme::Scaled(5,scale),-theme::Scaled(3,scale));
    if(item.itemState&ODS_SELECTED) { theme::DrawMaterial(item.hDC,r,6,scale,p.pressed,-1); }
    r.left+=theme::Scaled(9,scale);
    theme::DrawLabel(item.hDC,r,text,item.itemState&ODS_SELECTED ? p.focus:p.text,
        reinterpret_cast<HFONT>(::SendMessageW(item.hwndItem,WM_GETFONT,0,0)),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    return true;
}
void SetAccessibleName(HWND control,const wchar_t* name) {
    platform::ComPtr<IAccPropServices> properties;
    if(control && SUCCEEDED(::CoCreateInstance(CLSID_AccPropServices,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&properties))) &&
       ::SetWindowSubclass(control,AccessibleNameProc,0xEA56,0)) {
        properties->SetHwndPropStr(control,static_cast<DWORD>(OBJID_CLIENT),CHILDID_SELF,PROPID_ACC_NAME,name);
    }
}

bool StyleProgress(HWND progress) {
    if(!progress) { return false; }
    ::SetWindowTheme(progress,L"",L"");
    ::SetWindowLongPtrW(progress,GWL_STYLE,::GetWindowLongPtrW(progress,GWL_STYLE)&~WS_BORDER);
    ::SetWindowLongPtrW(progress,GWL_EXSTYLE,::GetWindowLongPtrW(progress,GWL_EXSTYLE)&~(WS_EX_CLIENTEDGE|WS_EX_STATICEDGE));
    ::SetWindowPos(progress,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    return ::SetWindowSubclass(progress,ProgressProc,0xEA55,0)!=FALSE;
}
bool StyleSlider(HWND slider) { return slider && ::SetWindowSubclass(slider,SliderProc,0xEA53,0); }
bool StyleLabel(HWND label) { return label && ::SetWindowSubclass(label,BufferedTextProc,0xEA5A,0); }
bool StyleInput(HWND input) {
    if(!input) { return false; }
    ::SetWindowTheme(input,L"",L"");
    ::SetWindowLongPtrW(input,GWL_STYLE,::GetWindowLongPtrW(input,GWL_STYLE)&~WS_BORDER);
    ::SetWindowLongPtrW(input,GWL_EXSTYLE,::GetWindowLongPtrW(input,GWL_EXSTYLE)&~WS_EX_CLIENTEDGE);
    if(!::SetWindowSubclass(input,InputProc,0xEA54,0)) { return false; }
    wchar_t cls[32]{}; ::GetClassNameW(input,cls,32);
    if(::lstrcmpiW(cls,L"EDIT")==0) { if(!StyleLabel(input)) { return false; } }
    else {
        ::SetWindowLongPtrW(input,GWL_STYLE,::GetWindowLongPtrW(input,GWL_STYLE)|WS_CLIPCHILDREN);
        COMBOBOXINFO info{sizeof(info)};
        if(::GetComboBoxInfo(input,&info) && info.hwndItem && info.hwndItem!=input) {
            ::SetWindowTheme(info.hwndItem,L"",L""); if(!StyleLabel(info.hwndItem)) { return false; }
        }
    }
    ::SetWindowPos(input,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_FRAMECHANGED);
    return true;
}

void StyleTooltip(HWND tooltip) {
    if (!tooltip) { return; }
    // Native tooltip colors are ignored while visual styles are active. Keep
    // its native behavior/provider, with application-owned accessible colors.
    ::SetWindowTheme(tooltip, L"", L"");
    ::SetWindowSubclass(tooltip, TooltipProc, kTooltipSubclass, 0);
    ::SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0,
        theme::Scaled(360, static_cast<float>(::GetDpiForWindow(tooltip)) / 96.0f));
    ::SetWindowDisplayAffinity(tooltip, 0x11);
    RefreshTooltip(tooltip);
}
void PaintNativePanel(HWND window, float scale, bool grip) {
    PAINTSTRUCT paint{};
    HDC dc = ::BeginPaint(window, &paint);
    RECT client{};
    ::GetClientRect(window, &client);
    PaintBuffer buffer(dc,client);
    theme::DrawPanel(buffer.dc(), client, scale);
    if (grip) { theme::DrawGrip(buffer.dc(), {0, 0, theme::Scaled(28, scale), client.bottom}, scale); }
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
HMENU CreateAppearanceMenu() {
    HMENU menu = ::CreatePopupMenu();
    if (menu) {
        ::AppendMenuW(menu, MF_STRING, kAppearanceLight, L"Light");
        ::AppendMenuW(menu, MF_STRING, kAppearanceDark, L"Dark");
        ::CheckMenuRadioItem(menu, kAppearanceLight, kAppearanceDark,
            theme::Preference() == app::AppearanceTheme::Dark ? kAppearanceDark : kAppearanceLight, MF_BYCOMMAND);
    }
    return menu;
}
struct MenuThemeScope::State {
    struct Item { HMENU menu; UINT index; UINT type; ULONG_PTR original; std::wstring text; bool submenu; };
    HWND owner{}; HFONT font{}; float scale{1}; std::vector<std::unique_ptr<Item>> items; std::vector<HMENU> menus;
    static LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR reference) {
        auto* self=reinterpret_cast<State*>(reference);
        if(message==WM_MEASUREITEM) {
            auto* measure=reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if(measure && measure->CtlType==ODT_MENU) {
                auto* item=reinterpret_cast<Item*>(measure->itemData);
                HDC dc=::GetDC(hwnd); const auto old=::SelectObject(dc,self->font); SIZE text{};
                ::GetTextExtentPoint32W(dc,item->text.c_str(),static_cast<int>(item->text.size()),&text);
                ::SelectObject(dc,old); ::ReleaseDC(hwnd,dc);
                measure->itemWidth=static_cast<UINT>(text.cx+theme::Scaled(72,self->scale));
                measure->itemHeight=theme::Scaled((item->type&MFT_SEPARATOR) ? 10.0f:36.0f,self->scale); return TRUE;
            }
        }
        if(message==WM_DRAWITEM) {
            const auto* draw=reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if(draw && draw->CtlType==ODT_MENU) {
                const auto* item=reinterpret_cast<Item*>(draw->itemData); const auto& p=theme::Current();
                theme::FillSolid(draw->hDC,draw->rcItem,p.panel);
                if(item->type&MFT_SEPARATOR) { RECT line=draw->rcItem; line.top=(line.top+line.bottom)/2; line.bottom=line.top+1; ::InflateRect(&line,-theme::Scaled(12,self->scale),0); theme::FillSolid(draw->hDC,line,p.border); return TRUE; }
                if(draw->itemState&ODS_SELECTED) { RECT face=draw->rcItem; ::InflateRect(&face,-theme::Scaled(6,self->scale),-theme::Scaled(4,self->scale)); theme::DrawMaterial(draw->hDC,face,6,self->scale,p.panel,-0.8f); }
                const auto ink=draw->itemState&ODS_DISABLED ? p.disabled:p.text;
                RECT label=draw->rcItem; label.left+=theme::Scaled(34,self->scale); label.right-=theme::Scaled(24,self->scale);
                const auto split=item->text.find(L'\t'); const auto title=item->text.substr(0,split);
                theme::DrawLabel(draw->hDC,label,title.c_str(),ink,self->font,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                if(split!=std::wstring::npos) { theme::DrawLabel(draw->hDC,label,item->text.substr(split+1).c_str(),p.muted_text,self->font,DT_RIGHT|DT_VCENTER|DT_SINGLELINE); }
                RECT icon=draw->rcItem; icon.left+=theme::Scaled(9,self->scale); icon.right=icon.left+theme::Scaled(16,self->scale);
                icon.top=(icon.top+icon.bottom-theme::Scaled(16,self->scale))/2; icon.bottom=icon.top+theme::Scaled(16,self->scale);
                if(draw->itemState&ODS_CHECKED) { DrawIcon(draw->hDC,icon,Icon::Check,p.focus,self->scale); }
                if(item->submenu) { const LONG x=draw->rcItem.right-theme::Scaled(14,self->scale),y=(draw->rcItem.top+draw->rcItem.bottom)/2,d=theme::Scaled(3,self->scale); theme::DrawSolidLine(draw->hDC,{x-d,y-d},{x,y},ink,1); theme::DrawSolidLine(draw->hDC,{x,y},{x-d,y+d},ink,1); }
                return TRUE;
            }
        }
        return ::DefSubclassProc(hwnd,message,wparam,lparam);
    }
    void Add(HMENU menu) {
        menus.push_back(menu);
        MENUINFO background{sizeof(background)}; background.fMask=MIM_BACKGROUND; background.hbrBack=::CreateSolidBrush(theme::Current().panel);
        ::SetMenuInfo(menu,&background); brushes.push_back(background.hbrBack);
        const int count=::GetMenuItemCount(menu);
        for(int i=0;i<count;++i) {
            wchar_t text[256]{}; MENUITEMINFOW info{sizeof(info)};
            info.fMask=MIIM_FTYPE|MIIM_DATA|MIIM_STRING|MIIM_SUBMENU; info.dwTypeData=text; info.cch=256;
            if(!::GetMenuItemInfoW(menu,i,TRUE,&info)) { continue; }
            auto item=std::make_unique<Item>(Item{menu,static_cast<UINT>(i),info.fType,info.dwItemData,text,info.hSubMenu!=nullptr});
            if(info.hSubMenu) { Add(info.hSubMenu); }
            info.fMask=MIIM_FTYPE|MIIM_DATA; info.fType|=MFT_OWNERDRAW; info.dwItemData=reinterpret_cast<ULONG_PTR>(item.get());
            if(::SetMenuItemInfoW(menu,i,TRUE,&info)) { items.push_back(std::move(item)); }
        }
    }
    std::vector<HBRUSH> brushes;
    ~State() {
        if(owner) { ::RemoveWindowSubclass(owner,Proc,0xEA57); }
        for(const auto& item:items) { MENUITEMINFOW info{sizeof(info)}; info.fMask=MIIM_FTYPE|MIIM_DATA; info.fType=item->type; info.dwItemData=item->original; ::SetMenuItemInfoW(item->menu,item->index,TRUE,&info); }
        for(HMENU menu:menus) { MENUINFO info{sizeof(info)}; info.fMask=MIM_BACKGROUND; info.hbrBack=nullptr; ::SetMenuInfo(menu,&info); }
        for(HBRUSH brush:brushes) { ::DeleteObject(brush); } if(font) { ::DeleteObject(font); }
    }
};
MenuThemeScope::MenuThemeScope(HWND owner,HMENU menu) : state_(std::make_unique<State>()) {
    state_->owner=owner; state_->scale=::GetDpiForWindow(owner)/96.0f; state_->font=theme::CreateUiFont(13,state_->scale,false);
    if(::SetWindowSubclass(owner,State::Proc,0xEA57,reinterpret_cast<DWORD_PTR>(state_.get()))) { state_->Add(menu); }
}
MenuThemeScope::~MenuThemeScope()=default;

namespace {
constexpr UINT_PTR kChromeSubclass=0xEA59;
struct WindowChrome {
    HWND owner{},header{},close{},minimize{},maximize{};
    HFONT font{};
    UINT font_dpi{};
    bool resizable{};
    ~WindowChrome() { if(font) { ::DeleteObject(font); } }
    void PaintResize() const {
        // Finish only our own HWNDs before returning to the native resize
        // loop. Never synchronously paint the out-of-process video renderer.
        constexpr UINT flags=RDW_UPDATENOW|RDW_NOCHILDREN|RDW_NOERASE;
        ::RedrawWindow(owner,nullptr,nullptr,flags);
        ::EnumChildWindows(owner,[](HWND child,LPARAM)->BOOL {
            if(::GetWindowThreadProcessId(child,nullptr)==::GetCurrentThreadId() && ::IsWindowVisible(child)) {
                ::RedrawWindow(child,nullptr,nullptr,RDW_UPDATENOW|RDW_NOCHILDREN|RDW_NOERASE);
            }
            return TRUE;
        },0);
    }
    void Layout() {
        const float scale=::GetDpiForWindow(owner)/96.0f;
        RECT r{}; ::GetClientRect(owner,&r);
        const int height=theme::Scaled(kWindowChromeHeight,scale),button=theme::Scaled(34,scale),gap=theme::Scaled(6,scale);
        constexpr UINT flags=SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOREDRAW;
        ::SetWindowPos(header,nullptr,0,0,r.right,height,flags);
        HDWP batch=::BeginDeferWindowPos(3);
        LONG x=r.right-gap-button;
        for(HWND control:{close,maximize,minimize}) if(control) {
            if(batch) { batch=::DeferWindowPos(batch,control,nullptr,x,(height-button)/2,button,button,flags); }
            x-=button+gap;
        }
        if(!batch || !::EndDeferWindowPos(batch)) {
            x=r.right-gap-button;
            for(HWND control:{close,maximize,minimize}) if(control) {
                ::SetWindowPos(control,nullptr,x,(height-button)/2,button,button,flags); x-=button+gap;
            }
        }
        const UINT dpi=::GetDpiForWindow(owner);
        if(!font || font_dpi!=dpi) {
            HFONT next=theme::CreateUiFont(13,scale,true);
            if(next) { if(font) { ::DeleteObject(font); } font=next; font_dpi=dpi; }
        }
        // Resizable windows use DWM corners. Replacing their region during
        // every WM_SIZE invalidates the compositor's resize backing surface.
        if(!resizable) { RoundWindow(owner); }
        ::RedrawWindow(header,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_NOERASE);
    }
};
LRESULT CALLBACK ChromeHeaderProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* data=reinterpret_cast<WindowChrome*>(::GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        data=static_cast<WindowChrome*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        ::SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(data));
    }
    if(!data) { return ::DefWindowProcW(hwnd,message,wparam,lparam); }
    switch(message) {
        case WM_NCHITTEST: {
            if(data->resizable && !::IsZoomed(data->owner)) {
                RECT r{}; ::GetWindowRect(data->owner,&r); const int edge=theme::Scaled(6,::GetDpiForWindow(hwnd)/96.0f);
                if(GET_X_LPARAM(lparam)<r.left+edge || GET_X_LPARAM(lparam)>=r.right-edge || GET_Y_LPARAM(lparam)<r.top+edge) { return HTTRANSPARENT; }
            }
            return HTCLIENT;
        }
        case WM_PAINT: case WM_PRINTCLIENT: case WM_ERASEBKGND: {
            PAINTSTRUCT ps{}; HDC target=message==WM_PAINT ? ::BeginPaint(hwnd,&ps):reinterpret_cast<HDC>(wparam);
            RECT r{}; ::GetClientRect(hwnd,&r); PaintBuffer buffer(target,r);
            theme::FillSolid(buffer.dc(),r,theme::Current().panel);
            const float scale=::GetDpiForWindow(hwnd)/96.0f;
            wchar_t title[256]{}; ::GetWindowTextW(data->owner,title,256);
            r.left=theme::Scaled(20,scale); r.right-=theme::Scaled(data->resizable ? 130.0f:52.0f,scale);
            theme::DrawLabel(buffer.dc(),r,title,theme::Current().muted_text,data->font,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            buffer.Present(); if(message==WM_PAINT) { ::EndPaint(hwnd,&ps); } return message==WM_ERASEBKGND ? 1 : 0;
        }
        case WM_COMMAND:
            if(reinterpret_cast<HWND>(lparam)==data->close) { ::PostMessageW(data->owner,WM_CLOSE,0,0); }
            else if(reinterpret_cast<HWND>(lparam)==data->minimize) { ::SendMessageW(data->owner,WM_SYSCOMMAND,SC_MINIMIZE,0); }
            else if(reinterpret_cast<HWND>(lparam)==data->maximize) { ::SendMessageW(data->owner,WM_SYSCOMMAND,::IsZoomed(data->owner) ? SC_RESTORE:SC_MAXIMIZE,0); }
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONUP: {
            POINT p{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; ::ClientToScreen(hwnd,&p);
            ::SendMessageW(data->owner,message==WM_LBUTTONDOWN ? WM_NCLBUTTONDOWN:message==WM_LBUTTONDBLCLK ? WM_NCLBUTTONDBLCLK:WM_NCRBUTTONUP,HTCAPTION,MAKELPARAM(p.x,p.y)); return 0;
        }
        case theme::kChangedMessage: ::RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN); return 0;
    }
    return ::DefWindowProcW(hwnd,message,wparam,lparam);
}
LRESULT CALLBACK ChromeParentProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR reference) {
    auto* data=reinterpret_cast<WindowChrome*>(reference);
    if(message==WM_NCDESTROY) { ::RemoveWindowSubclass(hwnd,ChromeParentProc,id); delete data; return ::DefSubclassProc(hwnd,message,wparam,lparam); }
    if(message==WM_NCCALCSIZE) {
        if(data->resizable) {
            auto* rect=reinterpret_cast<RECT*>(lparam);
            const LONG top=rect->top;
            // Preserve DWM's left/right/bottom resize frame. Removing the
            // entire frame makes its old surface follow the moving origin
            // before the new child layout is presented on Windows 10.
            ::DefSubclassProc(hwnd,message,wparam,lparam);
            const UINT dpi=::GetDpiForWindow(hwnd);
            rect->top=top+(::IsZoomed(hwnd) ? ::GetSystemMetricsForDpi(SM_CYSIZEFRAME,dpi)+
                ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi):0);
        }
        // WVR_REDRAW mispositions children when nonclient insets exist.
        // Layout invalidates and paints our owned surfaces after positioning.
        return 0;
    }
    if(message==WM_NCPAINT && !data->resizable) { return 0; }
    if(message==WM_NCACTIVATE && !data->resizable) { return TRUE; }
    if(message==WM_NCHITTEST && data->resizable && !::IsZoomed(hwnd)) {
        RECT r{}; ::GetWindowRect(hwnd,&r); const int edge=theme::Scaled(6,::GetDpiForWindow(hwnd)/96.0f);
        const int x=GET_X_LPARAM(lparam),y=GET_Y_LPARAM(lparam);
        const bool left=x<r.left+edge,right=x>=r.right-edge,top=y<r.top+edge,bottom=y>=r.bottom-edge;
        if(top) { return left ? HTTOPLEFT:right ? HTTOPRIGHT:HTTOP; }
        if(bottom) { return left ? HTBOTTOMLEFT:right ? HTBOTTOMRIGHT:HTBOTTOM; }
        if(left || right) { return left ? HTLEFT:HTRIGHT; }
        return HTCLIENT;
    }
    const bool resizing=message==WM_SIZE && wparam!=SIZE_MINIMIZED;
    if(resizing) { data->Layout(); }
    const LRESULT result=::DefSubclassProc(hwnd,message,wparam,lparam);
    if(message==WM_WINDOWPOSCHANGED && data->resizable &&
       !(reinterpret_cast<const WINDOWPOS*>(lparam)->flags&SWP_NOSIZE)) { data->PaintResize(); }
    if(message==WM_SETTEXT) { ::InvalidateRect(data->header,nullptr,FALSE); }
    return result;
}
}
bool StyleWindowChrome(HWND window,bool resizable) {
    static const bool registered=[] {
        WNDCLASSEXW cls{sizeof(cls)}; cls.style=CS_DBLCLKS; cls.lpfnWndProc=ChromeHeaderProc;
        cls.hInstance=::GetModuleHandleW(nullptr); cls.hCursor=::LoadCursorW(nullptr,IDC_ARROW); cls.lpszClassName=L"EasnapWindowChrome";
        return ::RegisterClassExW(&cls)!=0 || ::GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
    }();
    if(!registered) { return false; }
    auto data=std::make_unique<WindowChrome>(); data->owner=window; data->resizable=resizable;
    data->header=::CreateWindowExW(WS_EX_CONTROLPARENT,L"EasnapWindowChrome",L"Window controls",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,0,1,1,window,nullptr,::GetModuleHandleW(nullptr),data.get());
    if(!data->header) { return false; }
    const auto button=[&](const wchar_t* title,Icon icon) {
        HWND control=::CreateWindowExW(0,L"BUTTON",title,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,1,1,data->header,nullptr,::GetModuleHandleW(nullptr),nullptr);
        if(control && !StyleButton(control,{icon,ButtonRole::Standard,true})) { ::DestroyWindow(control); return static_cast<HWND>(nullptr); }
        return control;
    };
    data->close=button(L"Close",Icon::Discard);
    if(resizable) { data->minimize=button(L"Minimize",Icon::Minimize); data->maximize=button(L"Maximize or restore",Icon::Maximize); }
    if(!data->close || (resizable && (!data->minimize || !data->maximize)) ||
        !::SetWindowSubclass(window,ChromeParentProc,kChromeSubclass,reinterpret_cast<DWORD_PTR>(data.get()))) {
        ::DestroyWindow(data->header); return false;
    }
    auto* installed=data.release();
    if(resizable) {
        // Unsupported on older Windows versions: a rectangular frame is the
        // safe fallback, without region replacement or a separate shadow HWND.
        const DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_ROUND;
        ::DwmSetWindowAttribute(window,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners));
    }
    ::SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    installed->Layout(); return true;
}

namespace {
struct MessageDialog {
    const wchar_t* text;
    const wchar_t* title;
    UINT flags;
    /// Named buttons; empty for the standard ones selected by `flags`.
    std::vector<DialogChoice> choices;
    int default_value{0};
    HFONT font{};
    int cancel{IDOK};
    ~MessageDialog() { if(font) { ::DeleteObject(font); } }
};
INT_PTR CALLBACK MessageDialogProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* data=reinterpret_cast<MessageDialog*>(::GetWindowLongPtrW(hwnd,DWLP_USER));
    if(message==WM_INITDIALOG) {
        data=reinterpret_cast<MessageDialog*>(lparam); ::SetWindowLongPtrW(hwnd,DWLP_USER,lparam);
        ::SetWindowTextW(hwnd,data->title); ::SetWindowDisplayAffinity(hwnd,0x11);
        if(!StyleWindowChrome(hwnd,false)) { ::EndDialog(hwnd,-1); return TRUE; }
        const float scale=::GetDpiForWindow(hwnd)/96.0f;
        const auto dip=[&](int n) { return theme::Scaled(static_cast<float>(n),scale); };
        const auto monitor=platform::MonitorForRect([&] { RECT r{}; if(HWND owner=::GetWindow(hwnd,GW_OWNER)) { ::GetWindowRect(owner,&r); } else { ::GetCursorPos(reinterpret_cast<POINT*>(&r)); r.right=r.left+1; r.bottom=r.top+1; } return r; }());
        data->font=theme::CreateUiFont(14,scale,false);
        std::vector<DialogChoice> buttons=data->choices;
        size_t default_index=0;
        if(buttons.empty()) {
            switch(data->flags&MB_TYPEMASK) {
                case MB_YESNO:
                    buttons={{IDYES,strings::Get(IDS_DIALOG_YES),true},{IDNO,strings::Get(IDS_DIALOG_NO)}};
                    data->cancel=IDNO; break;
                case MB_OKCANCEL:
                    buttons={{IDOK,strings::Get(IDS_DIALOG_OK)},{IDCANCEL,strings::Get(IDS_DIALOG_CANCEL)}};
                    data->cancel=IDCANCEL; break;
                default: buttons={{IDOK,strings::Get(IDS_DIALOG_OK)}}; break;
            }
            default_index=std::min(static_cast<size_t>((data->flags&MB_DEFMASK)>>8),buttons.size()-1);
        } else {
            for(size_t i=0;i<buttons.size();++i) { if(buttons[i].value==data->default_value) { default_index=i; } }
        }
        HDC dc=::GetDC(hwnd); const auto old=::SelectObject(dc,data->font);
        // A button is as wide as its label needs and at least the standard
        // width. The dialog grows for a long row; only a work area too narrow
        // for the labels themselves makes buttons give up width.
        const int gap=dip(8),work_width=static_cast<int>(platform::RectWidth(monitor.work_area));
        std::vector<int> natural,widths;
        for(const DialogChoice& choice:buttons) {
            SIZE size{}; ::GetTextExtentPoint32W(dc,choice.label,::lstrlenW(choice.label),&size);
            natural.push_back(static_cast<int>(size.cx)+dip(36)); widths.push_back(std::max(dip(120),natural.back()));
        }
        const auto row_of=[&](const std::vector<int>& values) { int sum=gap*static_cast<int>(values.size()-1); for(int v:values) { sum+=v; } return sum; };
        if(row_of(widths)+dip(40)>work_width) { widths=natural; }
        if(row_of(widths)+dip(40)>work_width) {
            const int room=std::max(1,work_width-dip(40)-gap*static_cast<int>(widths.size()-1)),sum=std::max(1,row_of(widths)-gap*static_cast<int>(widths.size()-1));
            for(int& w:widths) { w=std::max(1,w*room/sum); }
        }
        const int row=row_of(widths);
        const int width=std::min(std::max(dip(480),row+dip(40)),work_width);
        RECT body{0,0,std::max(1,width-dip(48)),0}; ::DrawTextW(dc,data->text,-1,&body,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        ::SelectObject(dc,old); ::ReleaseDC(hwnd,dc);
        const int body_height=std::max(dip(60),static_cast<int>(body.bottom));
        const int height=std::min(dip(kWindowChromeHeight+104)+body_height,static_cast<int>(platform::RectHeight(monitor.work_area)));
        ::SetWindowPos(hwnd,HWND_TOP,monitor.work_area.left+(platform::RectWidth(monitor.work_area)-width)/2,
            monitor.work_area.top+(platform::RectHeight(monitor.work_area)-height)/2,width,height,SWP_NOACTIVATE);
        const int text_height=std::max(1,height-dip(kWindowChromeHeight+100));
        std::wstring body_text;
        for(const wchar_t* p=data->text;*p;++p) { if(*p==L'\n' && (p==data->text || p[-1]!=L'\r')) { body_text+=L'\r'; } body_text+=*p; }
        HWND text=::CreateWindowExW(0,L"EDIT",body_text.c_str(),WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|(text_height<body_height ? WS_VSCROLL:0),
            dip(24),dip(kWindowChromeHeight+16),width-dip(48),text_height,hwnd,nullptr,::GetModuleHandleW(nullptr),nullptr);
        if(!text || !StyleLabel(text)) { ::EndDialog(hwnd,-1); return TRUE; }
        ::SendMessageW(text,WM_SETFONT,reinterpret_cast<WPARAM>(data->font),FALSE);
        HWND focus{};
        int x=width-dip(20)-row;
        for(size_t i=0;i<buttons.size();++i) {
            HWND button=::CreateWindowExW(0,L"BUTTON",buttons[i].label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|(i==default_index ? BS_DEFPUSHBUTTON:BS_PUSHBUTTON),
                x,height-dip(60),widths[i],dip(40),hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(buttons[i].value)),::GetModuleHandleW(nullptr),nullptr);
            if(!button || !StyleButton(button,{Icon::None,buttons[i].destructive ? ButtonRole::Destructive:ButtonRole::Standard})) { ::EndDialog(hwnd,-1); return TRUE; }
            ::SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(data->font),FALSE);
            if(i==default_index) { focus=button; } x+=widths[i]+gap;
        }
        ::SendMessageW(hwnd,DM_SETDEFID,buttons[default_index].value,0); ::SetFocus(focus); return FALSE;
    }
    if(!data) { return FALSE; }
    switch(message) {
        case WM_CLOSE: ::EndDialog(hwnd,data->cancel); return TRUE;
        case WM_COMMAND:
            if(HIWORD(wparam)==BN_CLICKED) { ::EndDialog(hwnd,LOWORD(wparam)==IDCANCEL ? data->cancel:LOWORD(wparam)); return TRUE; }
            break;
        case WM_CTLCOLORSTATIC: case WM_CTLCOLORDLG: return ControlBackground(wparam,theme::Current().text);
        case WM_ERASEBKGND: return TRUE;
        case WM_PAINT: PaintNativePanel(hwnd,::GetDpiForWindow(hwnd)/96.0f); return TRUE;
        case WM_PRINTCLIENT: { RECT r{}; ::GetClientRect(hwnd,&r); theme::DrawPanel(reinterpret_cast<HDC>(wparam),r,::GetDpiForWindow(hwnd)/96.0f); return TRUE; }
        case theme::kChangedMessage: ::RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN); return TRUE;
    }
    return FALSE;
}
}
namespace {
int RunMessageDialog(HWND owner,MessageDialog& dialog) {
    struct Template { DLGTEMPLATE dialog; WORD menu{},window_class{},title{}; } definition{};
    definition.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN;
    definition.dialog.dwExtendedStyle=WS_EX_CONTROLPARENT;
    definition.dialog.cx=300; definition.dialog.cy=140;
    const platform::ModalLoopScope modal;
    return static_cast<int>(::DialogBoxIndirectParamW(::GetModuleHandleW(nullptr),&definition.dialog,owner,MessageDialogProc,reinterpret_cast<LPARAM>(&dialog)));
}
}
int ShowMessage(HWND owner,const wchar_t* text,const wchar_t* title,UINT flags) {
    MessageDialog dialog{text,title,flags};
    return RunMessageDialog(owner,dialog);
}
int ShowChoice(HWND owner,const wchar_t* text,const wchar_t* title,const std::vector<DialogChoice>& choices,
               int default_value,int cancel_value) {
    if(choices.empty()) { return cancel_value; }
    MessageDialog dialog{text,title,MB_ICONWARNING};
    dialog.choices=choices; dialog.default_value=default_value; dialog.cancel=cancel_value;
    const int answer=RunMessageDialog(owner,dialog);
    return answer<=0 ? cancel_value:answer;  // -1 and 0 report that no dialog ran.
}
bool ConfirmDestructive(HWND owner,const wchar_t* text,const wchar_t* title,const wchar_t* confirm,const wchar_t* keep) {
    return ShowChoice(owner,text,title,{{IDYES,confirm,true},{IDNO,keep}},IDNO,IDNO)==IDYES;
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
    if (left && swallowed_left) { if (message == WM_LBUTTONUP) { swallowed_left = false; } return true; }
    if (right && swallowed_right) { if (message == WM_RBUTTONUP) { swallowed_right = false; } return true; }
    return false;
}
void ShowAppearanceMenu(HWND invoker) {
    const UINT command = theme::Preference() == app::AppearanceTheme::Light ? kAppearanceDark : kAppearanceLight;
    ::SendMessageW(::GetAncestor(invoker,GA_ROOT),WM_COMMAND,command,0);
}
} // namespace easnap::ui
