#pragma once

#include <windows.h>

#include <vector>

namespace easnap::ui {

/// A message box in the application's own chrome; `flags` takes MB_OK,
/// MB_OKCANCEL or MB_YESNO and MB_DEFBUTTON*. Returns the ID of the answer.
int ShowMessage(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags);
/// One button of `ShowChoice`. The value is what the call returns for it.
struct DialogChoice {
    int value{0};
    const wchar_t* label{nullptr};
    bool destructive{false};
};
/// A question whose answers are named after what they do. `default_value`
/// has the focus; Esc and the close button answer `cancel_value`. Returns the
/// chosen value, or `cancel_value` when the dialog could not be shown.
int ShowChoice(HWND owner, const wchar_t* text, const wchar_t* title, const std::vector<DialogChoice>& choices,
               int default_value, int cancel_value);
/// "Really lose this?" - `confirm` is the destructive answer, `keep` is the
/// default and the answer to Esc. True only when `confirm` was chosen.
[[nodiscard]] bool ConfirmDestructive(HWND owner, const wchar_t* text, const wchar_t* title, const wchar_t* confirm,
                                      const wchar_t* keep);
/// ShowMessage with a warning icon and the product name as title.
inline void ShowErrorMessage(HWND owner, const wchar_t* text) { ShowMessage(owner, text, L"Easnap", MB_OK | MB_ICONWARNING); }

} // namespace easnap::ui
