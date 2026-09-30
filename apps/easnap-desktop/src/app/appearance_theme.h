#pragma once

namespace easnap::app {

// A preference, independent of Windows rendering and temporary high contrast.
enum class AppearanceTheme { Light, Dark };

[[nodiscard]] constexpr bool IsValidAppearanceTheme(AppearanceTheme theme) noexcept {
    return theme == AppearanceTheme::Light || theme == AppearanceTheme::Dark;
}

} // namespace easnap::app
