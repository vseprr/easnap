// Easnap - localized user-visible text implementation.
#include "strings/strings.h"

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

namespace easnap::strings {
namespace {

constexpr WORD kEnglish = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
constexpr WORD kTurkish = MAKELANGID(LANG_TURKISH, SUBLANG_DEFAULT);
constexpr size_t kCount = IDS_LAST - IDS_FIRST + 1;

struct Tables {
    std::array<std::vector<std::wstring>, 2> text;
};

/// A string resource is stored in blocks of sixteen length-prefixed strings.
void LoadTable(WORD language, std::vector<std::wstring>& out) {
    out.assign(kCount, std::wstring());
    HMODULE module = ::GetModuleHandleW(nullptr);
    for (UINT block = IDS_FIRST / 16; block <= IDS_LAST / 16; ++block) {
        HRSRC found = ::FindResourceExW(module, RT_STRING, MAKEINTRESOURCEW(block + 1), language);
        if (found == nullptr) { continue; }
        HGLOBAL loaded = ::LoadResource(module, found);
        const auto* cursor = loaded != nullptr ? static_cast<const wchar_t*>(::LockResource(loaded)) : nullptr;
        const DWORD size = ::SizeofResource(module, found) / sizeof(wchar_t);
        if (cursor == nullptr) { continue; }
        const wchar_t* const end = cursor + size;
        for (UINT index = 0; index < 16 && cursor < end; ++index) {
            const size_t length = static_cast<size_t>(*cursor++);
            if (cursor + length > end) { break; }
            const UINT id = block * 16 + index;
            if (id >= IDS_FIRST && id <= IDS_LAST && length != 0) {
                out[id - IDS_FIRST].assign(cursor, length);
            }
            cursor += length;
        }
    }
}

const Tables& Loaded() {
    static Tables tables;
    static std::once_flag once;
    std::call_once(once, [] {
        LoadTable(kEnglish, tables.text[0]);
        LoadTable(kTurkish, tables.text[1]);
    });
    return tables;
}

std::atomic<int> g_preference{static_cast<int>(LanguagePreference::System)};

}  // namespace

void SetPreference(LanguagePreference preference) noexcept {
    g_preference.store(static_cast<int>(preference), std::memory_order_release);
}

LanguagePreference Preference() noexcept {
    return static_cast<LanguagePreference>(g_preference.load(std::memory_order_acquire));
}

Language SystemLanguage() noexcept {
    return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_TURKISH ? Language::Turkish : Language::English;
}

Language Current() noexcept {
    switch (Preference()) {
        case LanguagePreference::English: return Language::English;
        case LanguagePreference::Turkish: return Language::Turkish;
        case LanguagePreference::System: break;
    }
    return SystemLanguage();
}

const wchar_t* Get(UINT id, Language language) noexcept {
    if (id < IDS_FIRST || id > IDS_LAST) { return L""; }
    return Loaded().text[language == Language::Turkish ? 1 : 0][id - IDS_FIRST].c_str();
}

const wchar_t* Get(UINT id) noexcept {
    const Language language = Current();
    const wchar_t* text = Get(id, language);
    return *text != L'\0' || language == Language::English ? text : Get(id, Language::English);
}

std::wstring Format(UINT id, std::initializer_list<std::wstring_view> arguments) {
    const std::wstring_view pattern = Get(id);
    std::wstring result;
    result.reserve(pattern.size() + 32);
    for (size_t i = 0; i < pattern.size(); ++i) {
        const wchar_t c = pattern[i];
        if (c == L'%' && i + 1 < pattern.size()) {
            const wchar_t next = pattern[i + 1];
            if (next >= L'1' && next <= L'9') {
                const size_t index = static_cast<size_t>(next - L'1');
                if (index < arguments.size()) { result.append(*(arguments.begin() + index)); }
                ++i;
                continue;
            }
            if (next == L'%') {
                result.push_back(L'%');
                ++i;
                continue;
            }
        }
        result.push_back(c);
    }
    return result;
}

}  // namespace easnap::strings
