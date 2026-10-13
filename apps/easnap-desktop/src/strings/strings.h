// Easnap - localized user-visible text.
//
// Every string a person can read comes from the string tables in
// resources/strings_en.rc and resources/strings_tr.rc. Both tables are loaded
// once and never change afterwards, so text can be requested from any thread
// and the pointers stay valid for the lifetime of the process.
#pragma once

#include <windows.h>

#include <initializer_list>
#include <string>
#include <string_view>

#include "string_ids.h"

namespace easnap::strings {

enum class Language { English, Turkish };

/// What the user chose. `System` follows the Windows display language.
enum class LanguagePreference { System, English, Turkish };

/// Selects the language. Text requested afterwards uses it; windows that are
/// already open keep the text they were created with.
void SetPreference(LanguagePreference preference) noexcept;
[[nodiscard]] LanguagePreference Preference() noexcept;
[[nodiscard]] Language Current() noexcept;
/// The language `System` resolves to on this computer.
[[nodiscard]] Language SystemLanguage() noexcept;

/// Text for `id` in the active language. A string missing from the Turkish
/// table falls back to English; an unknown id yields an empty string.
[[nodiscard]] const wchar_t* Get(UINT id) noexcept;
/// Text for `id` in one specific language, without fallback.
[[nodiscard]] const wchar_t* Get(UINT id, Language language) noexcept;
/// Text for `id` with %1 ... %9 replaced by `arguments`.
[[nodiscard]] std::wstring Format(UINT id, std::initializer_list<std::wstring_view> arguments);

}  // namespace easnap::strings
