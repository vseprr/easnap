#pragma once

#include <windows.h>

#include "ui/ui_theme.h"

namespace easnap::ui {

/// Height of the shared custom caption, in DIP.
inline constexpr int kWindowChromeHeight = theme::kCaptionHeight;
/// Replaces the caption of `window` with the shared custom one (title, close,
/// and for a resizable window minimize and maximize); a resizable window keeps
/// the native side and bottom frame (ADR-024).
[[nodiscard]] bool StyleWindowChrome(HWND window, bool resizable);

} // namespace easnap::ui
