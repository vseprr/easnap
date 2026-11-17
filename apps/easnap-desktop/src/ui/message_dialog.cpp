#include "ui/message_dialog.h"

#include <algorithm>
#include <string>
#include <vector>

#include "platform/platform_utils.h"
#include "platform/screen_geometry.h"
#include "strings/strings.h"
#include "ui/native_controls.h"
#include "ui/ui_theme.h"
#include "ui/window_chrome.h"

namespace easnap::ui {
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
    ~MessageDialog() {
        if (font) { ::DeleteObject(font); }
    }
};
INT_PTR CALLBACK MessageDialogProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* data = reinterpret_cast<MessageDialog*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<MessageDialog*>(lparam);
        ::SetWindowLongPtrW(hwnd, DWLP_USER, lparam);
        ::SetWindowTextW(hwnd, data->title);
        // Help can be open during a Scrollshot and the Exit question during a
        // recording: excluded as an opaque layered window, it leaves no hole.
        static_cast<void>(ExcludeFromCapture(hwnd));
        if (!StyleWindowChrome(hwnd, false)) {
            ::EndDialog(hwnd, -1);
            return TRUE;
        }
        const float scale = ::GetDpiForWindow(hwnd) / 96.0f;
        const auto dip = [&](int n) { return theme::Scaled(static_cast<float>(n), scale); };
        const auto monitor = platform::MonitorForRect([&] { RECT r{}; if(HWND owner=::GetWindow(hwnd,GW_OWNER)) { ::GetWindowRect(owner,&r); } else { ::GetCursorPos(reinterpret_cast<POINT*>(&r)); r.right=r.left+1; r.bottom=r.top+1; } return r; }());
        data->font = theme::CreateUiFont(14, scale, false);
        std::vector<DialogChoice> buttons = data->choices;
        size_t default_index = 0;
        if (buttons.empty()) {
            switch (data->flags & MB_TYPEMASK) {
                case MB_YESNO:
                    buttons = {{IDYES, strings::Get(IDS_DIALOG_YES), true}, {IDNO, strings::Get(IDS_DIALOG_NO)}};
                    data->cancel = IDNO;
                    break;
                case MB_OKCANCEL:
                    buttons = {{IDOK, strings::Get(IDS_DIALOG_OK)}, {IDCANCEL, strings::Get(IDS_DIALOG_CANCEL)}};
                    data->cancel = IDCANCEL;
                    break;
                default: buttons = {{IDOK, strings::Get(IDS_DIALOG_OK)}}; break;
            }
            default_index = std::min(static_cast<size_t>((data->flags & MB_DEFMASK) >> 8), buttons.size() - 1);
        } else {
            for (size_t i = 0; i < buttons.size(); ++i) {
                if (buttons[i].value == data->default_value) { default_index = i; }
            }
        }
        HDC dc = ::GetDC(hwnd);
        const auto old = ::SelectObject(dc, data->font);
        // A button is as wide as its label needs and at least the standard
        // width. The dialog grows for a long row; only a work area too narrow
        // for the labels themselves makes buttons give up width.
        const int gap = dip(8), work_width = static_cast<int>(platform::RectWidth(monitor.work_area));
        std::vector<int> natural, widths;
        for (const DialogChoice& choice : buttons) {
            SIZE size{};
            ::GetTextExtentPoint32W(dc, choice.label, ::lstrlenW(choice.label), &size);
            natural.push_back(static_cast<int>(size.cx) + dip(36));
            widths.push_back(std::max(dip(120), natural.back()));
        }
        const auto row_of = [&](const std::vector<int>& values) { int sum=gap*static_cast<int>(values.size()-1); for(int v:values) { sum+=v; } return sum; };
        if (row_of(widths) + dip(40) > work_width) { widths = natural; }
        if (row_of(widths) + dip(40) > work_width) {
            const int room = std::max(1, work_width - dip(40) - gap * static_cast<int>(widths.size() - 1)), sum = std::max(1, row_of(widths) - gap * static_cast<int>(widths.size() - 1));
            for (int& w : widths) { w = std::max(1, w * room / sum); }
        }
        const int row = row_of(widths);
        const int width = std::min(std::max(dip(480), row + dip(40)), work_width);
        RECT body{0, 0, std::max(1, width - dip(48)), 0};
        ::DrawTextW(dc, data->text, -1, &body, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        ::SelectObject(dc, old);
        ::ReleaseDC(hwnd, dc);
        const int body_height = std::max(dip(60), static_cast<int>(body.bottom));
        const int height = std::min(dip(kWindowChromeHeight + 104) + body_height, static_cast<int>(platform::RectHeight(monitor.work_area)));
        ::SetWindowPos(hwnd, HWND_TOP, monitor.work_area.left + (platform::RectWidth(monitor.work_area) - width) / 2,
                       monitor.work_area.top + (platform::RectHeight(monitor.work_area) - height) / 2, width, height, SWP_NOACTIVATE);
        const int text_height = std::max(1, height - dip(kWindowChromeHeight + 100));
        std::wstring body_text;
        for (const wchar_t* p = data->text; *p; ++p) {
            if (*p == L'\n' && (p == data->text || p[-1] != L'\r')) { body_text += L'\r'; }
            body_text += *p;
        }
        HWND text = ::CreateWindowExW(0, L"EDIT", body_text.c_str(), WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | (text_height < body_height ? WS_VSCROLL : 0),
                                      dip(24), dip(kWindowChromeHeight + 16), width - dip(48), text_height, hwnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (!text || !StyleLabel(text)) {
            ::EndDialog(hwnd, -1);
            return TRUE;
        }
        ::SendMessageW(text, WM_SETFONT, reinterpret_cast<WPARAM>(data->font), FALSE);
        HWND focus{};
        int x = width - dip(20) - row;
        for (size_t i = 0; i < buttons.size(); ++i) {
            HWND button = ::CreateWindowExW(0, L"BUTTON", buttons[i].label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == default_index ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                                            x, height - dip(60), widths[i], dip(40), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(buttons[i].value)), ::GetModuleHandleW(nullptr), nullptr);
            if (!button || !StyleButton(button, {Icon::None, buttons[i].destructive ? ButtonRole::Destructive : ButtonRole::Standard})) {
                ::EndDialog(hwnd, -1);
                return TRUE;
            }
            ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(data->font), FALSE);
            if (i == default_index) { focus = button; }
            x += widths[i] + gap;
        }
        ::SendMessageW(hwnd, DM_SETDEFID, buttons[default_index].value, 0);
        ::SetFocus(focus);
        return FALSE;
    }
    if (!data) { return FALSE; }
    switch (message) {
        case WM_CLOSE: ::EndDialog(hwnd, data->cancel); return TRUE;
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED) {
                ::EndDialog(hwnd, LOWORD(wparam) == IDCANCEL ? data->cancel : LOWORD(wparam));
                return TRUE;
            }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORDLG: return ControlBackground(wparam, theme::Current().text);
        case WM_ERASEBKGND: return TRUE;
        case WM_PAINT: PaintNativePanel(hwnd, ::GetDpiForWindow(hwnd) / 96.0f); return TRUE;
        case WM_PRINTCLIENT: {
            RECT r{};
            ::GetClientRect(hwnd, &r);
            theme::DrawPanel(reinterpret_cast<HDC>(wparam), r, ::GetDpiForWindow(hwnd) / 96.0f);
            return TRUE;
        }
        case theme::kChangedMessage: ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN); return TRUE;
    }
    return FALSE;
}
}  // namespace
namespace {
int RunMessageDialog(HWND owner, MessageDialog& dialog) {
    struct Template {
        DLGTEMPLATE dialog;
        WORD menu{}, window_class{}, title{};
    } definition{};
    definition.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    definition.dialog.dwExtendedStyle = WS_EX_CONTROLPARENT;
    definition.dialog.cx = 300;
    definition.dialog.cy = 140;
    const platform::ModalLoopScope modal;
    return static_cast<int>(::DialogBoxIndirectParamW(::GetModuleHandleW(nullptr), &definition.dialog, owner, MessageDialogProc, reinterpret_cast<LPARAM>(&dialog)));
}
}  // namespace
int ShowMessage(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags) {
    MessageDialog dialog{text, title, flags};
    return RunMessageDialog(owner, dialog);
}
int ShowChoice(HWND owner, const wchar_t* text, const wchar_t* title, const std::vector<DialogChoice>& choices,
               int default_value, int cancel_value) {
    if (choices.empty()) { return cancel_value; }
    MessageDialog dialog{text, title, MB_ICONWARNING};
    dialog.choices = choices;
    dialog.default_value = default_value;
    dialog.cancel = cancel_value;
    const int answer = RunMessageDialog(owner, dialog);
    return answer <= 0 ? cancel_value : answer;  // -1 and 0 report that no dialog ran.
}
bool ConfirmDestructive(HWND owner, const wchar_t* text, const wchar_t* title, const wchar_t* confirm, const wchar_t* keep) {
    return ShowChoice(owner, text, title, {{IDYES, confirm, true}, {IDNO, keep}}, IDNO, IDNO) == IDYES;
}

} // namespace easnap::ui
