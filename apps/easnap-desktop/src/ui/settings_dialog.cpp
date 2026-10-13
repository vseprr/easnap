// Easnap - the Settings dialog.
#include "ui/settings_dialog.h"

#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <vector>

#include "platform/platform_utils.h"
#include "strings/strings.h"
#include "ui/native_controls.h"
#include "ui/ui_theme.h"

namespace easnap::ui {
namespace {

constexpr UINT_PTR kShortcutSubclass = 0xEA70;
constexpr int kDialogWidth = 600;   // DIP
constexpr int kMargin = 24;
constexpr int kLabelWidth = 204;
constexpr int kFieldLeft = kMargin + kLabelWidth + 8;
constexpr int kRowHeight = 40;
constexpr int kRowGap = 8;

struct Dialog {
    SettingsDialogModel model;
    const SettingsDialogCallbacks* callbacks{nullptr};
    HWND hwnd{nullptr};
    HFONT font{nullptr};
    float scale{1.0f};
    bool capturing{false};
    bool hint_is_warning{false};

    ~Dialog() {
        if (font != nullptr) { ::DeleteObject(font); }
    }

    [[nodiscard]] HWND Item(int id) const { return ::GetDlgItem(hwnd, id); }
    [[nodiscard]] int Dip(int value) const { return theme::Scaled(static_cast<float>(value), scale); }

    void SetHint(const wchar_t* text, bool warning) {
        hint_is_warning = warning;
        ::SetWindowTextW(Item(kSettingsShortcutHint), text);
        ::InvalidateRect(Item(kSettingsShortcutHint), nullptr, TRUE);
    }

    void ShowShortcut() {
        ::SetWindowTextW(Item(kSettingsShortcut),
                         ShortcutText(model.hotkey_modifiers, model.hotkey_virtual_key).c_str());
    }

    void BeginCapture() {
        capturing = true;
        ::SetWindowTextW(Item(kSettingsShortcut), strings::Get(IDS_SETTINGS_SHORTCUT_PRESS));
        SetHint(strings::Get(IDS_SETTINGS_SHORTCUT_HINT), false);
        ::SetFocus(Item(kSettingsShortcut));
    }

    void EndCapture() {
        capturing = false;
        ShowShortcut();
    }

    /// A key arrived while the shortcut button waits for the new combination.
    void OnCaptureKey(UINT key) {
        if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU || key == VK_LWIN || key == VK_RWIN) {
            return;  // Still holding modifiers.
        }
        UINT modifiers = 0;
        if (::GetKeyState(VK_CONTROL) < 0) { modifiers |= MOD_CONTROL; }
        if (::GetKeyState(VK_SHIFT) < 0) { modifiers |= MOD_SHIFT; }
        if (::GetKeyState(VK_MENU) < 0) { modifiers |= MOD_ALT; }
        if (::GetKeyState(VK_LWIN) < 0 || ::GetKeyState(VK_RWIN) < 0) { modifiers |= MOD_WIN; }
        if (key == VK_ESCAPE && modifiers == 0) {
            EndCapture();
            SetHint(L"", false);
            return;
        }
        // Shift alone would take a capital letter away from every application.
        if ((modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)) == 0) {
            SetHint(strings::Get(IDS_SETTINGS_SHORTCUT_HINT), true);
            return;
        }
        const bool same = modifiers == model.hotkey_modifiers && key == model.hotkey_virtual_key;
        if (!same && callbacks != nullptr && callbacks->shortcut_available &&
            !callbacks->shortcut_available(modifiers, key)) {
            EndCapture();
            SetHint(strings::Format(IDS_SETTINGS_SHORTCUT_TAKEN, {ShortcutText(modifiers, key)}).c_str(), true);
            return;
        }
        model.hotkey_modifiers = modifiers;
        model.hotkey_virtual_key = key;
        EndCapture();
        SetHint(L"", false);
    }

    void ShowFolders() {
        const auto show = [this](int path_id, int reset_id, const std::wstring& folder) {
            ::SetWindowTextW(Item(path_id), folder.empty() ? strings::Get(IDS_SETTINGS_FOLDER_NOT_SET) : folder.c_str());
            ::EnableWindow(Item(reset_id), folder.empty() ? FALSE : TRUE);
        };
        show(kSettingsImageFolder, kSettingsImageReset, model.image_folder);
        show(kSettingsVideoFolder, kSettingsVideoReset, model.video_folder);
        // Saving without the dialog needs a folder to save into.
        if (model.image_folder.empty()) { ::SendMessageW(Item(kSettingsQuickSave), BM_SETCHECK, BST_UNCHECKED, 0); }
        ::EnableWindow(Item(kSettingsQuickSave), model.image_folder.empty() ? FALSE : TRUE);
    }

    void ChangeFolder(std::wstring& folder) {
        if (callbacks == nullptr || !callbacks->choose_folder) { return; }
        const std::wstring chosen = callbacks->choose_folder(hwnd, folder);
        if (!chosen.empty()) {
            folder = chosen;
            ShowFolders();
        }
    }

    void ReadControls() {
        const LRESULT language = ::SendMessageW(Item(kSettingsLanguage), CB_GETCURSEL, 0, 0);
        model.language = language == 1 ? app::LanguageSetting::English
                         : language == 2 ? app::LanguageSetting::Turkish : app::LanguageSetting::System;
        const LRESULT appearance = ::SendMessageW(Item(kSettingsAppearance), CB_GETCURSEL, 0, 0);
        model.appearance_follows_system = appearance != 1 && appearance != 2;
        if (appearance == 1) { model.appearance_theme = app::AppearanceTheme::Light; }
        if (appearance == 2) { model.appearance_theme = app::AppearanceTheme::Dark; }
        const auto checked = [this](int id) { return ::SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; };
        model.start_with_windows = checked(kSettingsStartWithWindows);
        model.capture_cursor = checked(kSettingsCaptureCursor);
        model.quick_save_images = !model.image_folder.empty() && checked(kSettingsQuickSave);
    }

    [[nodiscard]] bool Create() {
        ::SetWindowTextW(hwnd, strings::Get(IDS_SETTINGS_TITLE));
        ::SetWindowDisplayAffinity(hwnd, 0x11);
        if (!StyleWindowChrome(hwnd, false)) { return false; }
        scale = static_cast<float>(::GetDpiForWindow(hwnd)) / 96.0f;
        font = theme::CreateUiFont(14, scale, false);
        POINT cursor{};
        ::GetCursorPos(&cursor);
        const platform::MonitorInfo monitor = platform::MonitorForPoint(cursor);
        const int width = std::min(Dip(kDialogWidth), static_cast<int>(platform::RectWidth(monitor.work_area)));
        const int field_width = width - Dip(kFieldLeft) - Dip(kMargin);

        bool ok = true;
        const auto make = [&](const wchar_t* class_name, const wchar_t* text, DWORD style, int id, int x, int y,
                              int w, int h) {
            HWND control = ::CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                             ::GetModuleHandleW(nullptr), nullptr);
            if (control == nullptr) { ok = false; return control; }
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            return control;
        };
        const auto label = [&](UINT text, int y) {
            HWND control = make(L"STATIC", strings::Get(text), SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, -1,
                                Dip(kMargin), y, Dip(kLabelWidth), Dip(kRowHeight));
            ok = ok && StyleLabel(control);
        };
        const auto button = [&](UINT text, DWORD style, int id, int x, int y, int w, ButtonStyle look = {}) {
            HWND control = make(L"BUTTON", strings::Get(text), WS_TABSTOP | style, id, x, y, w, Dip(kRowHeight));
            ok = ok && StyleButton(control, look);
            return control;
        };
        const auto combo = [&](int id, int y, std::initializer_list<const wchar_t*> items, int selected, UINT name) {
            HWND control = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                id, Dip(kFieldLeft), y, field_width, Dip(kRowHeight) * 5);
            for (const wchar_t* item : items) { ::SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item)); }
            ::SendMessageW(control, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
            ok = ok && StyleInput(control);
            SetAccessibleName(control, strings::Get(name));
        };

        int y = Dip(kWindowChromeHeight + 16);
        const int step = Dip(kRowHeight + kRowGap);

        label(IDS_SETTINGS_LANGUAGE, y);
        combo(kSettingsLanguage, y, {strings::Get(IDS_SETTINGS_SAME_AS_WINDOWS), L"English", L"Türkçe"},
              model.language == app::LanguageSetting::English ? 1 : model.language == app::LanguageSetting::Turkish ? 2 : 0,
              IDS_SETTINGS_LANGUAGE);
        y += step;

        label(IDS_SETTINGS_APPEARANCE, y);
        combo(kSettingsAppearance, y,
              {strings::Get(IDS_SETTINGS_SAME_AS_WINDOWS), strings::Get(IDS_SHELL_APPEARANCE_LIGHT),
               strings::Get(IDS_SHELL_APPEARANCE_DARK)},
              model.appearance_follows_system ? 0 : model.appearance_theme == app::AppearanceTheme::Dark ? 2 : 1,
              IDS_SETTINGS_APPEARANCE);
        y += step;

        label(IDS_SETTINGS_SHORTCUT, y);
        HWND shortcut = make(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, kSettingsShortcut, Dip(kFieldLeft), y, field_width,
                             Dip(kRowHeight));
        ok = ok && StyleButton(shortcut) &&
             ::SetWindowSubclass(shortcut, ShortcutProc, kShortcutSubclass, reinterpret_cast<DWORD_PTR>(this)) != FALSE;
        y += Dip(kRowHeight + 2);
        // Two lines: the instruction while a shortcut is awaited, or why one was refused.
        HWND hint = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kSettingsShortcutHint, Dip(kFieldLeft), y, field_width,
                         Dip(40));
        ok = ok && StyleLabel(hint);
        y += Dip(40 + kRowGap);

        const int full_width = width - 2 * Dip(kMargin);
        HWND startup = button(IDS_SETTINGS_START_WITH_WINDOWS, BS_AUTOCHECKBOX, kSettingsStartWithWindows, Dip(kMargin), y, full_width);
        ::SendMessageW(startup, BM_SETCHECK, model.start_with_windows ? BST_CHECKED : BST_UNCHECKED, 0);
        y += step;
        HWND cursor_box = button(IDS_SETTINGS_CAPTURE_CURSOR, BS_AUTOCHECKBOX, kSettingsCaptureCursor, Dip(kMargin), y, full_width);
        ::SendMessageW(cursor_box, BM_SETCHECK, model.capture_cursor ? BST_CHECKED : BST_UNCHECKED, 0);
        y += step;

        const auto folder_row = [&](UINT name, int path_id, int change_id, int reset_id) {
            label(name, y);
            HWND path = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_PATHELLIPSIS, path_id, Dip(kFieldLeft), y,
                             field_width, Dip(kRowHeight));
            ok = ok && StyleLabel(path);
            y += Dip(kRowHeight);
            HWND change = button(IDS_SETTINGS_FOLDER_CHANGE, BS_PUSHBUTTON, change_id, Dip(kFieldLeft), y, Dip(120));
            const int change_width = std::max(Dip(120), ButtonContentWidth(change));
            ::SetWindowPos(change, nullptr, 0, 0, change_width, Dip(kRowHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            HWND reset = button(IDS_SETTINGS_FOLDER_RESET, BS_PUSHBUTTON, reset_id, Dip(kFieldLeft) + change_width + Dip(8), y, Dip(120));
            ::SetWindowPos(reset, nullptr, 0, 0, std::max(Dip(120), ButtonContentWidth(reset)), Dip(kRowHeight),
                           SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            y += step;
        };
        folder_row(IDS_SETTINGS_IMAGE_FOLDER, kSettingsImageFolder, kSettingsImageChange, kSettingsImageReset);
        HWND quick = button(IDS_SETTINGS_QUICK_SAVE, BS_AUTOCHECKBOX, kSettingsQuickSave, Dip(kFieldLeft), y, field_width);
        ::SendMessageW(quick, BM_SETCHECK, model.quick_save_images ? BST_CHECKED : BST_UNCHECKED, 0);
        y += step;
        folder_row(IDS_SETTINGS_VIDEO_FOLDER, kSettingsVideoFolder, kSettingsVideoChange, kSettingsVideoReset);

        y += Dip(8);
        HWND cancel = button(IDS_DIALOG_CANCEL, BS_PUSHBUTTON, IDCANCEL, 0, y, Dip(120));
        HWND accept = button(IDS_DIALOG_OK, BS_DEFPUSHBUTTON, IDOK, 0, y, Dip(120), {Icon::None, ButtonRole::Primary});
        const int cancel_width = std::max(Dip(120), ButtonContentWidth(cancel));
        const int accept_width = std::max(Dip(120), ButtonContentWidth(accept));
        ::SetWindowPos(cancel, nullptr, width - Dip(kMargin) - cancel_width, y, cancel_width, Dip(kRowHeight),
                       SWP_NOZORDER | SWP_NOACTIVATE);
        ::SetWindowPos(accept, cancel, width - Dip(kMargin) - cancel_width - Dip(8) - accept_width, y, accept_width,
                       Dip(kRowHeight), SWP_NOACTIVATE);
        y += Dip(kRowHeight + 20);
        if (!ok) { return false; }

        ShowShortcut();
        ShowFolders();
        const int height = std::min(y, static_cast<int>(platform::RectHeight(monitor.work_area)));
        ::SetWindowPos(hwnd, HWND_TOP, monitor.work_area.left + (platform::RectWidth(monitor.work_area) - width) / 2,
                       monitor.work_area.top + (platform::RectHeight(monitor.work_area) - height) / 2, width, height,
                       SWP_NOACTIVATE);
        ::SendMessageW(hwnd, DM_SETDEFID, IDOK, 0);
        ::SetFocus(Item(kSettingsLanguage));
        return true;
    }

    static LRESULT CALLBACK ShortcutProc(HWND button, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                         DWORD_PTR reference) {
        auto* dialog = reinterpret_cast<Dialog*>(reference);
        if (message == WM_NCDESTROY) {
            ::RemoveWindowSubclass(button, ShortcutProc, id);
        } else if (dialog->capturing) {
            switch (message) {
                case WM_GETDLGCODE: return DLGC_WANTALLKEYS;  // Tab, Enter and Esc belong to the shortcut now.
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                    dialog->OnCaptureKey(static_cast<UINT>(wparam));
                    return 0;
                case WM_KEYUP:
                case WM_SYSKEYUP:
                case WM_CHAR:
                case WM_SYSCHAR:
                    return 0;
                case WM_KILLFOCUS:
                    dialog->EndCapture();
                    dialog->SetHint(L"", false);
                    break;
                default: break;
            }
        }
        return ::DefSubclassProc(button, message, wparam, lparam);
    }
};

INT_PTR CALLBACK SettingsProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog = reinterpret_cast<Dialog*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<Dialog*>(lparam);
        dialog->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, DWLP_USER, lparam);
        if (!dialog->Create()) { ::EndDialog(hwnd, -1); }
        return FALSE;  // The focus was set by Create.
    }
    if (dialog == nullptr) { return FALSE; }
    switch (message) {
        case WM_CLOSE:
            ::EndDialog(hwnd, IDCANCEL);
            return TRUE;
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int notification = HIWORD(wparam);
            if (notification != BN_CLICKED) { break; }
            switch (id) {
                case IDOK:
                    dialog->ReadControls();
                    ::EndDialog(hwnd, IDOK);
                    return TRUE;
                case IDCANCEL:
                    ::EndDialog(hwnd, IDCANCEL);
                    return TRUE;
                case kSettingsShortcut:
                    if (!dialog->capturing) { dialog->BeginCapture(); }
                    return TRUE;
                case kSettingsImageChange: dialog->ChangeFolder(dialog->model.image_folder); return TRUE;
                case kSettingsVideoChange: dialog->ChangeFolder(dialog->model.video_folder); return TRUE;
                case kSettingsImageReset: dialog->model.image_folder.clear(); dialog->ShowFolders(); return TRUE;
                case kSettingsVideoReset: dialog->model.video_folder.clear(); dialog->ShowFolders(); return TRUE;
                default: break;
            }
            break;
        }
        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (measure != nullptr && measure->CtlType == ODT_COMBOBOX) {
                measure->itemHeight = static_cast<UINT>(dialog->Dip(32));
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (draw != nullptr && DrawComboItem(*draw)) { return TRUE; }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            const HWND control = reinterpret_cast<HWND>(lparam);
            const int id = ::GetDlgCtrlID(control);
            COLORREF color = theme::Current().text;
            if (id == kSettingsShortcutHint) { color = dialog->hint_is_warning ? theme::Current().error : theme::Current().muted_text; }
            if (id == kSettingsImageFolder || id == kSettingsVideoFolder) { color = theme::Current().muted_text; }
            return ControlBackground(wparam, color);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORDLG:
            return ControlBackground(wparam, theme::Current().text);
        case WM_ERASEBKGND:
            return TRUE;
        case WM_PAINT:
            PaintNativePanel(hwnd, dialog->scale);
            return TRUE;
        case WM_PRINTCLIENT: {
            RECT client{};
            ::GetClientRect(hwnd, &client);
            theme::DrawPanel(reinterpret_cast<HDC>(wparam), client, dialog->scale);
            return TRUE;
        }
        case theme::kChangedMessage:
            ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

}  // namespace

std::wstring ShortcutText(UINT modifiers, UINT virtual_key) {
    app::AppSettings settings;
    settings.hotkey_modifiers = modifiers;
    settings.hotkey_virtual_key = virtual_key;
    return settings.HotkeyDisplayText();
}

std::wstring ChooseFolder(HWND owner, const std::wstring& current, const wchar_t* title) {
    platform::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    if (title != nullptr) { dialog->SetTitle(title); }
    if (!current.empty()) {
        platform::ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(current.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }
    HRESULT result = E_FAIL;
    {
        const platform::ModalLoopScope modal;
        result = dialog->Show(owner);
    }
    platform::ComPtr<IShellItem> item;
    if (FAILED(result) || FAILED(dialog->GetResult(&item))) {
        return {};
    }
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || path == nullptr) {
        return {};
    }
    std::wstring chosen(path);
    ::CoTaskMemFree(path);
    return chosen;
}

bool ShowSettingsDialog(HWND owner, SettingsDialogModel& model, const SettingsDialogCallbacks& callbacks) {
    struct Template {
        DLGTEMPLATE dialog;
        WORD menu{}, window_class{}, title{};
    } definition{};
    definition.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    definition.dialog.dwExtendedStyle = WS_EX_CONTROLPARENT;
    definition.dialog.cx = 300;
    definition.dialog.cy = 200;
    Dialog dialog;
    dialog.model = model;
    dialog.callbacks = &callbacks;
    INT_PTR result = -1;
    {
        const platform::ModalLoopScope modal;
        result = ::DialogBoxIndirectParamW(::GetModuleHandleW(nullptr), &definition.dialog, owner, SettingsProc,
                                           reinterpret_cast<LPARAM>(&dialog));
    }
    if (result != IDOK) {
        return false;
    }
    model = dialog.model;
    return true;
}

}  // namespace easnap::ui
