// Easnap unit tests - localized string tables.
//
// The English and Turkish tables are read from this executable's resources
// through the production accessor. Every identifier must exist in both
// languages with the same placeholders, so a translation can never drop an
// argument or leave a control without text.

#include "strings/strings.h"

#include <cstdio>
#include <set>
#include <string>

using namespace easnap::strings;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL  %s:%d  %s\n", file, line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

/// The %1...%9 arguments a pattern uses, ignoring a literal %%.
std::set<wchar_t> Placeholders(const std::wstring& text) {
    std::set<wchar_t> found;
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != L'%') { continue; }
        if (text[i + 1] >= L'1' && text[i + 1] <= L'9') { found.insert(text[i + 1]); }
        ++i;
    }
    return found;
}

void TestBothTablesAreComplete() {
    int defined = 0;
    int incomplete = 0;
    int mismatched = 0;
    for (UINT id = IDS_FIRST; id <= IDS_LAST; ++id) {
        const std::wstring english = Get(id, Language::English);
        const std::wstring turkish = Get(id, Language::Turkish);
        if (english.empty() && turkish.empty()) { continue; }  // A gap between groups.
        ++defined;
        if (english.empty() || turkish.empty()) {
            ++incomplete;
            std::fprintf(stderr, "  string %u is missing in %s\n", id, english.empty() ? "English" : "Turkish");
        } else if (Placeholders(english) != Placeholders(turkish)) {
            ++mismatched;
            std::fprintf(stderr, "  string %u uses different placeholders in the two languages\n", id);
        }
    }
    CHECK(defined > 30);
    CHECK(incomplete == 0);
    CHECK(mismatched == 0);
    // The ends of the range are real strings, so the range is not stale.
    CHECK(*Get(IDS_FIRST, Language::English) != L'\0');
    CHECK(*Get(IDS_LAST, Language::English) != L'\0');
}

void TestLanguageSelection() {
    SetPreference(LanguagePreference::English);
    CHECK(Current() == Language::English);
    CHECK(std::wstring(Get(IDS_MODE_RECORD)) == L"Record");
    CHECK(std::wstring(LocaleName()) == L"en-us");

    SetPreference(LanguagePreference::Turkish);
    CHECK(Current() == Language::Turkish);
    CHECK(std::wstring(Get(IDS_MODE_RECORD)) == L"Kayıt");  // The table is read as UTF-8.
    CHECK(std::wstring(LocaleName()) == L"tr-tr");

    SetPreference(LanguagePreference::System);
    CHECK(Preference() == LanguagePreference::System);
    CHECK(Current() == SystemLanguage());

    // Text handed out earlier stays valid when the language changes.
    SetPreference(LanguagePreference::English);
    const wchar_t* english = Get(IDS_SCROLLSHOT_START);
    SetPreference(LanguagePreference::Turkish);
    const wchar_t* turkish = Get(IDS_SCROLLSHOT_START);
    CHECK(english != turkish && std::wstring(english) == L"Start Scrollshot");
    CHECK(std::wstring(turkish) != std::wstring(english));
    SetPreference(LanguagePreference::English);
}

void TestFormatAndUnknownIds() {
    SetPreference(LanguagePreference::English);
    CHECK(Format(IDS_SCROLLSHOT_SIZE, {L"1280", L"5430"}) == L"1280 × 5430");
    CHECK(Format(IDS_DOCUMENT_VIEW_LABEL, {L"1280", L"5430", L"75"}) == L"1280 × 5430   75%");
    // A missing argument leaves nothing behind; a surplus one is ignored.
    CHECK(Format(IDS_SCROLLSHOT_SIZE, {L"7"}) == L"7 × ");
    CHECK(Format(IDS_SCROLLSHOT_SIZE, {L"1", L"2", L"3"}) == L"1 × 2");
    SetPreference(LanguagePreference::Turkish);
    CHECK(Format(IDS_DOCUMENT_VIEW_LABEL, {L"1280", L"5430", L"75"}) == L"1280 × 5430   %75");
    SetPreference(LanguagePreference::English);

    CHECK(*Get(IDS_FIRST - 1) == L'\0');
    CHECK(*Get(IDS_LAST + 1) == L'\0');
    CHECK(*Get(0) == L'\0');
}

}  // namespace

int main() {
    TestBothTablesAreComplete();
    TestLanguageSelection();
    TestFormatAndUnknownIds();
    std::printf("strings: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
