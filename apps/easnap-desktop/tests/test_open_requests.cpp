// Easnap tests - how a video reaches Easnap from outside (ADR-040, entry points).
//
// --command-line (Fast): the production parser, fed through CommandLineToArgvW
//   as Easnap.exe feeds it. "--startup" counts only as an argument of its own;
//   a path that contains it opens the file. Quoted paths, "--open", a bare
//   path, relative paths, unknown and missing arguments.
// --validate (Fast): the hand-over validator alone (magic, size, terminator,
//   spelling) and the location check the receiver and the sender share.
// --installer <easnap.iss> (Fast): the installer's "Open with" extensions are
//   exactly the support table's, and nothing makes Easnap a default.
// --handover (Standard, private desktop): the production receiver behind a
//   window of this test, driven by WM_COPYDATA; then this executable started
//   again as a second process that hands a path over with the production
//   sender, against this test's own window class, never Easnap's.
// --send <class> <path>: the second process of --handover.

#include "app/command_line.h"
#include "app/video_handover.h"
#include "media/video_probe.h"
#include "media/video_support.h"
#include "platform/startup_entry.h"
#include "temp_folder.h"

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace easnap;
using Clock = std::chrono::steady_clock;

int passed{}, failed{};
#define CHECK(...)                                                         \
    do {                                                                   \
        if (__VA_ARGS__) {                                                 \
            ++passed;                                                      \
        } else {                                                           \
            ++failed;                                                      \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #__VA_ARGS__); \
        }                                                                  \
    } while (false)

constexpr int kSkipped = 77;

int Finish(const char* name) {
    std::printf("%s: %d passed, %d failed\n", name, passed, failed);
    return failed == 0 ? 0 : 1;
}

bool WriteFileBytes(const std::wstring& path, const char* bytes) {
    const platform::UniqueHandle file(
        ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file || file.get() == INVALID_HANDLE_VALUE) { return false; }
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(std::strlen(bytes));
    return ::WriteFile(file.get(), bytes, size, &written, nullptr) != FALSE && written == size;
}

// --- Command line ------------------------------------------------------------------

app::CommandLineRequest Parse(const std::wstring& command_line) {
    int count = 0;
    LPWSTR* arguments = ::CommandLineToArgvW(command_line.c_str(), &count);
    if (arguments == nullptr) {
        ++failed;
        std::fprintf(stderr, "FAIL: CommandLineToArgvW refused a command line\n");
        return {};
    }
    app::CommandLineRequest request = app::ParseCommandLine(count, arguments);
    ::LocalFree(arguments);
    return request;
}

bool Is(const app::CommandLineRequest& request, bool startup, const std::wstring& path, bool bad) {
    return request.startup == startup && request.open_path == path && request.bad == bad;
}

int CommandLineParsing() {
    test::TempFolder folder(L"EasnapCommandLine");
    if (!folder.ok()) {
        std::fprintf(stderr, "FAIL: no temporary folder\n");
        return 1;
    }
    // A folder named like the option, and files whose names contain it.
    const std::wstring option_folder = folder.File(L"--startup");
    CHECK(::CreateDirectoryW(option_folder.c_str(), nullptr) != FALSE);
    const std::wstring inside_option = option_folder + L"\\clip.mp4";
    const std::wstring clip = folder.File(L"clip.mp4");
    const std::wstring spaced = folder.File(L"my holiday clip.mp4");
    const std::wstring suffixed = folder.File(L"clip--startup.mp4");
    CHECK(WriteFileBytes(inside_option, "x") && WriteFileBytes(clip, "x") && WriteFileBytes(spaced, "x") &&
          WriteFileBytes(suffixed, "x"));
    const std::wstring program = L"\"C:\\Program Files\\Easnap\\Easnap.exe\"";
    const auto quoted = [](const std::wstring& path) { return L"\"" + path + L"\""; };

    // Nothing, and the sign-in entry exactly as Settings writes it.
    CHECK(Is(Parse(program), false, L"", false));
    CHECK(Is(Parse(platform::StartupCommand(L"C:\\Program Files\\Easnap\\Easnap.exe")), true, L"", false));
    CHECK(Is(Parse(program + L" --STARTUP"), true, L"", false));  // Options ignore ASCII case.

    // A path that contains "--startup" is a file to open, never a quiet start.
    CHECK(Is(Parse(program + L" " + quoted(inside_option)), false, inside_option, false));
    if (inside_option.find(L' ') == std::wstring::npos) {  // Without spaces no quotes are needed.
        CHECK(Is(Parse(program + L" " + inside_option), false, inside_option, false));
    }
    CHECK(Is(Parse(program + L" " + quoted(suffixed)), false, suffixed, false));
    CHECK(Is(Parse(program + L" --open " + quoted(inside_option)), false, inside_option, false));
    // What Explorer runs for "Open with": the quoted path, spaces and all.
    CHECK(Is(Parse(program + L" --open " + quoted(spaced)), false, spaced, false));
    CHECK(Is(Parse(program + L" " + quoted(spaced)), false, spaced, false));
    CHECK(Is(Parse(program + L" --startup --open " + quoted(clip)), true, clip, false));
    // "--open" names the file even when it is not there: the probe says why.
    CHECK(Is(Parse(program + L" --open \"C:\\No such folder\\gone.mp4\""), false, L"C:\\No such folder\\gone.mp4",
             false));
    // A UNC path and a URL are kept as given; the location check refuses them.
    CHECK(Is(Parse(program + L" --open \\\\server\\share\\clip.mp4"), false, L"\\\\server\\share\\clip.mp4", false));
    CHECK(Is(Parse(program + L" --open https://example.com/clip.mp4"), false, L"https://example.com/clip.mp4",
             false));

    // Ignored: a bare argument that names no file, or a folder; an unknown
    // option; "--open" without a path; every path after the first.
    CHECK(Is(Parse(program + L" \"C:\\No such folder\\gone.mp4\""), false, L"", true));
    CHECK(Is(Parse(program + L" " + quoted(folder.path())), false, L"", true));
    CHECK(Is(Parse(program + L" --verbose"), false, L"", true));
    CHECK(Is(Parse(program + L" --startupx"), false, L"", true));
    CHECK(Is(Parse(program + L" -startup"), false, L"", true));  // A bare word that names no file.
    CHECK(Is(Parse(program + L" --open"), false, L"", true));
    CHECK(Is(Parse(program + L" --open \"\""), false, L"", true));
    CHECK(Is(Parse(program + L" --open " + quoted(clip) + L" --open " + quoted(spaced)), false, clip, true));
    CHECK(Is(Parse(program + L" " + quoted(clip) + L" " + quoted(spaced)), false, clip, true));
    CHECK(Is(Parse(program + L" " + quoted(clip) + L" --startup --what"), true, clip, true));

    // Relative paths are made absolute against the current directory, so the
    // running Easnap, which has another one, can be handed them.
    wchar_t previous[MAX_PATH]{};
    CHECK(::GetCurrentDirectoryW(MAX_PATH, previous) != 0);
    CHECK(::SetCurrentDirectoryW(folder.path().c_str()) != FALSE);
    CHECK(Is(Parse(program + L" clip.mp4"), false, clip, false));
    CHECK(Is(Parse(program + L" --open \"my holiday clip.mp4\""), false, spaced, false));
    CHECK(Is(Parse(program + L" --open later.mp4"), false, folder.File(L"later.mp4"), false));
    CHECK(Is(Parse(program + L" --startup\\clip.mp4"), false, L"", true));  // An option-like word stays an option.
    CHECK(Is(Parse(program + L" .\\--startup\\clip.mp4"), false, inside_option, false));
    CHECK(::SetCurrentDirectoryW(previous) != FALSE);

    // No arguments at all.
    CHECK(Is(app::ParseCommandLine(0, nullptr), false, L"", false));
    const wchar_t* const only_program[] = {L"Easnap.exe"};
    CHECK(Is(app::ParseCommandLine(1, only_program), false, L"", false));
    const wchar_t* const with_null[] = {L"Easnap.exe", nullptr};
    CHECK(Is(app::ParseCommandLine(2, with_null), false, L"", true));

    // The temporary folder removes one level of files only.
    CHECK(::DeleteFileW(inside_option.c_str()) != FALSE);
    return Finish("command_line_parsing");
}

// --- Hand-over validation --------------------------------------------------------------

/// A payload exactly as given: `text` with or without its terminator.
std::optional<std::wstring> ParseText(const std::wstring& text, bool terminated = true,
                                      ULONG_PTR magic = app::kOpenVideoHandOverMagic) {
    std::wstring buffer = text;
    buffer.push_back(terminated ? L'\0' : L'X');
    buffer.push_back(L'\0');  // Past the payload: a reader that overruns finds an end.
    COPYDATASTRUCT data{};
    data.dwData = magic;
    data.cbData = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
    data.lpData = buffer.data();
    return app::ParseHandOver(data);
}

std::wstring MissingDrive() {
    const DWORD drives = ::GetLogicalDrives();
    for (int letter = 25; letter >= 3; --letter) {
        if ((drives & (1u << letter)) == 0) { return std::wstring(1, static_cast<wchar_t>(L'A' + letter)) + L":\\"; }
    }
    return {};
}

int HandOverValidation() {
    using media::VideoRefusal;
    // Well-formed: the spelling alone decides; the file system is not asked.
    CHECK(ParseText(L"C:\\Videos\\clip.mp4") == std::wstring(L"C:\\Videos\\clip.mp4"));
    CHECK(ParseText(L"\\\\?\\C:\\Videos\\clip.mp4") == std::wstring(L"\\\\?\\C:\\Videos\\clip.mp4"));
    // Wrong magic, no terminator, a NUL inside.
    CHECK(!ParseText(L"C:\\Videos\\clip.mp4", true, app::kOpenVideoHandOverMagic + 1));
    CHECK(!ParseText(L"C:\\Videos\\clip.mp4", true, 0));
    CHECK(!ParseText(L"C:\\Videos\\clip.mp4", false));
    CHECK(!ParseText(std::wstring(L"C:\\Videos\\clip.mp4\0.avi", 23)));
    // Relative, drive-relative, UNC, URLs and devices.
    CHECK(!ParseText(L"clip.mp4"));
    CHECK(!ParseText(L"Videos\\clip.mp4"));
    CHECK(!ParseText(L"C:clip.mp4"));
    CHECK(!ParseText(L"\\Videos\\clip.mp4"));
    CHECK(!ParseText(L"\\\\server\\share\\clip.mp4"));
    CHECK(!ParseText(L"\\\\?\\UNC\\server\\share\\clip.mp4"));
    CHECK(!ParseText(L"https://example.com/clip.mp4"));
    CHECK(!ParseText(L"file:///C:/Videos/clip.mp4"));
    CHECK(!ParseText(L"\\\\.\\pipe\\clip.mp4"));
    CHECK(!ParseText(L""));
    // The size: whole UTF-16 units, at most 32 KiB with the terminator.
    const std::wstring longest = L"C:\\" + std::wstring(app::kMaxHandOverBytes / sizeof(wchar_t) - 4, L'a');
    CHECK(longest.size() + 1 == app::kMaxHandOverBytes / sizeof(wchar_t));
    CHECK(ParseText(longest).has_value());
    CHECK(!ParseText(longest + L"a"));
    std::wstring odd = L"C:\\clip.mp4";
    odd.push_back(L'\0');
    COPYDATASTRUCT data{};
    data.dwData = app::kOpenVideoHandOverMagic;
    data.lpData = odd.data();
    data.cbData = static_cast<DWORD>(odd.size() * sizeof(wchar_t) - 1);
    CHECK(!app::ParseHandOver(data));
    data.cbData = static_cast<DWORD>(odd.size() * sizeof(wchar_t));
    CHECK(app::ParseHandOver(data) == std::wstring(L"C:\\clip.mp4"));
    data.lpData = nullptr;
    CHECK(!app::ParseHandOver(data));
    data.lpData = odd.data();
    data.cbData = 0;
    CHECK(!app::ParseHandOver(data));
    data.cbData = sizeof(wchar_t);  // The terminator alone.
    CHECK(!app::ParseHandOver(data));

    // The location check both sides share, on files of this test.
    test::TempFolder folder(L"EasnapHandOverValidation");
    CHECK(folder.ok());
    const std::wstring clip = folder.File(L"clip.mp4");
    CHECK(WriteFileBytes(clip, "not a video; the location check never reads it"));
    CHECK(media::CheckVideoLocation(clip) == VideoRefusal::None);
    CHECK(media::CheckVideoLocation(L"\\\\?\\" + clip) == VideoRefusal::None);
    CHECK(media::CheckVideoLocation(folder.File(L"gone.mp4")) == VideoRefusal::NotFound);
    CHECK(media::CheckVideoLocation(folder.path()) == VideoRefusal::UnsupportedExtension);  // A folder.
    CHECK(media::CheckVideoLocation(L"clip.mp4") == VideoRefusal::NotFound);
    CHECK(media::CheckVideoLocation(L"\\\\server\\share\\clip.mp4") == VideoRefusal::NetworkOrOffline);
    CHECK(media::CheckVideoLocation(L"https://example.com/clip.mp4") == VideoRefusal::NetworkOrOffline);
    const std::wstring missing_drive = MissingDrive();
    if (!missing_drive.empty()) { CHECK(media::CheckVideoLocation(missing_drive + L"clip.mp4") == VideoRefusal::NotFound); }
    return Finish("handover_validation");
}

// --- Installer --------------------------------------------------------------------------

/// One value of an Inno Setup entry: `Name: "quoted ""text"""` or `Name: word`.
std::optional<std::wstring> EntryField(const std::wstring& line, const wchar_t* name) {
    const std::wstring key = std::wstring(name) + L":";
    std::size_t at = 0;
    while ((at = line.find(key, at)) != std::wstring::npos) {
        if (at == 0 || line[at - 1] == L' ' || line[at - 1] == L';') { break; }
        at += key.size();
    }
    if (at == std::wstring::npos) { return std::nullopt; }
    std::size_t position = at + key.size();
    while (position < line.size() && line[position] == L' ') { ++position; }
    std::wstring value;
    if (position < line.size() && line[position] == L'"') {
        for (++position; position < line.size(); ++position) {
            if (line[position] != L'"') {
                value.push_back(line[position]);
            } else if (position + 1 < line.size() && line[position + 1] == L'"') {
                value.push_back(L'"');
                ++position;
            } else {
                return value;
            }
        }
        return std::nullopt;  // Not closed.
    }
    const std::size_t end = line.find(L';', position);
    value = line.substr(position, end == std::wstring::npos ? std::wstring::npos : end - position);
    while (!value.empty() && value.back() == L' ') { value.pop_back(); }
    return value;
}

bool HasFlag(const std::wstring& line, const wchar_t* flag) {
    const std::optional<std::wstring> flags = EntryField(line, L"Flags");
    if (!flags) { return false; }
    // Flags ignore case in Inno Setup; the ones named here are lower case.
    std::wstring padded = L" " + *flags + L" ";
    for (wchar_t& character : padded) {
        if (character >= L'A' && character <= L'Z') { character = static_cast<wchar_t>(character - L'A' + L'a'); }
    }
    return padded.find(L" " + std::wstring(flag) + L" ") != std::wstring::npos;
}

bool EqualsIgnoringCase(const std::wstring& left, const std::wstring& right) {
    return ::CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

int InstallerOpenWithList(const wchar_t* script_path) {
    const platform::UniqueHandle file(::CreateFileW(script_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                                    FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file || file.get() == INVALID_HANDLE_VALUE) {
        std::printf("SKIP installer_open_with_list: the installer script is not in this tree\n");
        return kSkipped;
    }
    std::string bytes(1 << 20, '\0');
    DWORD read = 0;
    CHECK(::ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE);
    CHECK(read < bytes.size());
    bytes.resize(read);
    CHECK(bytes.rfind("\xEF\xBB\xBF", 0) == 0);  // Inno Setup reads the Turkish text through the BOM.
    const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
                                             nullptr, 0);
    std::wstring text(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    CHECK(length > 0 && ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                              static_cast<int>(bytes.size()), text.data(), length) == length);

    const std::wstring classes = L"Software\\Classes\\";
    const std::wstring progid_key = classes + L"Easnap.Video";
    const std::wstring application_key = classes + L"Applications\\Easnap.exe";
    const std::wstring supported_types_key = application_key + L"\\SupportedTypes";
    const std::wstring open_command = std::wstring(L"\"{app}\\Easnap.exe\" ") + app::kOpenArgument + L" \"%1\"";
    std::vector<std::wstring> open_with_progids;
    std::vector<std::wstring> supported_types;
    int commands = 0;
    int owned_keys_removed = 0;
    bool registry_section = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) { end = text.size(); }
        std::wstring line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == L'\r') { line.pop_back(); }
        // Inno Setup ignores leading blanks and the case of section names.
        const std::size_t first = line.find_first_not_of(L" \t");
        line.erase(0, first == std::wstring::npos ? line.size() : first);
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) { line.pop_back(); }
        if (!line.empty() && line[0] == L'[') {
            registry_section = EqualsIgnoringCase(line, L"[Registry]");
            continue;
        }
        if (line.empty() || line[0] == L';') { continue; }
        // Nothing anywhere in the script may choose a default for the user.
        CHECK(line.find(L"UserChoice") == std::wstring::npos);
        if (!registry_section) { continue; }
        const std::optional<std::wstring> root = EntryField(line, L"Root");
        const std::optional<std::wstring> subkey = EntryField(line, L"Subkey");
        CHECK(root.has_value() && subkey.has_value());
        if (!root || !subkey) { continue; }
        CHECK(*root == L"HKCU");  // Per user only: nothing for other users or the machine.
        const std::wstring value_type = EntryField(line, L"ValueType").value_or(L"none");
        const std::optional<std::wstring> value_name = EntryField(line, L"ValueName");
        const std::wstring extension_prefix = classes + L".";
        if (subkey->size() > extension_prefix.size() &&
            EqualsIgnoringCase(subkey->substr(0, extension_prefix.size()), extension_prefix)) {
            const std::wstring rest = subkey->substr(classes.size());
            const std::size_t separator = rest.find(L'\\');
            const std::wstring extension = rest.substr(0, separator);
            // Other programs keep their entries under these keys: uninstalling
            // Easnap must never delete one of them whole.
            CHECK(!HasFlag(line, L"uninsdeletekey"));
            if (separator == std::wstring::npos) {
                // The extension's own key: created at most, never given a value.
                CHECK(value_type == L"none" && !value_name.has_value());
                CHECK(HasFlag(line, L"uninsdeletekeyifempty"));
            } else {
                CHECK(EqualsIgnoringCase(rest.substr(separator), L"\\OpenWithProgids"));
                CHECK(value_type == L"string" && value_name == std::wstring(L"Easnap.Video"));
                CHECK(HasFlag(line, L"uninsdeletevalue") && HasFlag(line, L"uninsdeletekeyifempty"));
                open_with_progids.push_back(extension);
            }
        } else if (EqualsIgnoringCase(*subkey, supported_types_key)) {
            CHECK(value_type == L"string" && value_name.has_value());
            if (value_name) { supported_types.push_back(*value_name); }
        } else if (EqualsIgnoringCase(*subkey, progid_key) || EqualsIgnoringCase(*subkey, application_key)) {
            if (HasFlag(line, L"uninsdeletekey")) { ++owned_keys_removed; }
        } else if (EqualsIgnoringCase(*subkey, progid_key + L"\\shell\\open\\command") ||
                   EqualsIgnoringCase(*subkey, application_key + L"\\shell\\open\\command")) {
            CHECK(EntryField(line, L"ValueData") == open_command);
            ++commands;
        }
    }
    CHECK(commands == 2);
    CHECK(owned_keys_removed == 2);

    std::vector<std::wstring> table;
    for (const std::wstring_view extension : media::SupportedVideoExtensions()) { table.emplace_back(extension); }
    std::sort(table.begin(), table.end());
    std::sort(open_with_progids.begin(), open_with_progids.end());
    std::sort(supported_types.begin(), supported_types.end());
    CHECK(!table.empty());
    CHECK(open_with_progids == table);
    CHECK(supported_types == table);
    if (open_with_progids != table || supported_types != table) {
        std::fprintf(stderr, "The support table has %zu extensions; OpenWithProgids lists %zu and SupportedTypes %zu\n",
                     table.size(), open_with_progids.size(), supported_types.size());
    }
    return Finish("installer_open_with_list");
}

// --- Hand-over between processes ---------------------------------------------------

constexpr UINT kMsgDrained = WM_APP + 40;
constexpr int kSentTaken = 40;
constexpr int kSentRefused = 41;
constexpr int kSentNoRecipient = 42;

app::OpenVideoRequests g_requests;
bool g_accepting = true;
std::vector<std::wstring> g_opened;
bool g_drained = false;

/// The host window's two cases, as main.cpp wires them, around a recorder
/// instead of the coordinator.
LRESULT CALLBACK ReceiverProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_COPYDATA:
            return g_requests.OnCopyData(window, reinterpret_cast<const COPYDATASTRUCT*>(lparam), g_accepting) ? TRUE
                                                                                                               : FALSE;
        case app::kMsgOpenVideoRequest:
            if (const std::optional<std::wstring> path = g_requests.Take()) { g_opened.push_back(*path); }
            return 0;
        case kMsgDrained:
            g_drained = true;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

void Pump() {
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
}

/// Every message posted before this call has been handled.
bool Drain(HWND window) {
    g_drained = false;
    if (::PostMessageW(window, kMsgDrained, 0, 0) == FALSE) { return false; }
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
    while (!g_drained && Clock::now() < deadline) {
        Pump();
        if (!g_drained) { ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT); }
    }
    return g_drained;
}

bool SendText(HWND window, const std::wstring& text, ULONG_PTR magic = app::kOpenVideoHandOverMagic,
              bool terminated = true) {
    std::wstring buffer = text;
    buffer.push_back(terminated ? L'\0' : L'X');
    buffer.push_back(L'\0');
    COPYDATASTRUCT data{};
    data.dwData = magic;
    data.cbData = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
    data.lpData = buffer.data();
    return ::SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)) != FALSE;
}

std::wstring OwnPath() {
    wchar_t path[MAX_PATH * 4]{};
    const DWORD size = ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return size > 0 && size < ARRAYSIZE(path) ? std::wstring(path, size) : std::wstring();
}

/// Starts this executable as the second process and waits for it, handling
/// this thread's messages meanwhile: its WM_COPYDATA arrives through them.
int RunSender(const std::wstring& window_class, const std::wstring& path) {
    const std::wstring executable = OwnPath();
    std::wstring command = L"\"" + executable + L"\" --send " + window_class + L" \"" + path + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                         &process) == FALSE) {
        std::fprintf(stderr, "FAIL: the second process did not start (error %lu)\n", ::GetLastError());
        return -1;
    }
    ::CloseHandle(process.hThread);
    const platform::UniqueHandle child(process.hProcess);
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(30);
    for (;;) {
        Pump();
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) { break; }
        HANDLE handle = child.get();
        const DWORD woke = ::MsgWaitForMultipleObjects(1, &handle, FALSE, static_cast<DWORD>(left), QS_ALLINPUT);
        if (woke == WAIT_OBJECT_0) {
            DWORD code = 0;
            ::GetExitCodeProcess(child.get(), &code);
            return static_cast<int>(code);
        }
        if (woke != WAIT_OBJECT_0 + 1) { break; }
    }
    std::fprintf(stderr, "FAIL: the second process did not end in time\n");
    ::TerminateProcess(child.get(), 1);
    return -1;
}

int CopyDataHandOver() {
    test::TempFolder folder(L"EasnapHandOver");
    CHECK(folder.ok());
    const std::wstring clip = folder.File(L"clip with spaces.mp4");
    const std::wstring other = folder.File(L"other.avi");
    CHECK(WriteFileBytes(clip, "x") && WriteFileBytes(other, "x"));
    const std::wstring missing = folder.File(L"gone.mp4");

    const std::wstring window_class = L"EasnapHandOverTest-" + std::to_wstring(::GetCurrentProcessId());
    WNDCLASSEXW window_type{};
    window_type.cbSize = sizeof(window_type);
    window_type.lpfnWndProc = ReceiverProc;
    window_type.hInstance = ::GetModuleHandleW(nullptr);
    window_type.lpszClassName = window_class.c_str();
    if (::RegisterClassExW(&window_type) == 0) {
        std::fprintf(stderr, "FAIL: the window class was not registered\n");
        return 1;
    }
    // Like Easnap's host window: hidden, top level, a tool window.
    HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW, window_class.c_str(), L"Easnap hand-over test", WS_POPUP, 0, 0,
                                    0, 0, nullptr, nullptr, window_type.hInstance, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "FAIL: the receiving window was not created\n");
        return 1;
    }

    // A valid request is kept, answered yes, and opened from the posted message.
    g_accepting = true;
    CHECK(SendText(window, clip));
    CHECK(g_opened.empty() && g_requests.waiting());  // Nothing is done inside WM_COPYDATA.
    CHECK(Drain(window));
    CHECK(g_opened == std::vector<std::wstring>{clip});
    CHECK(!g_requests.waiting());

    // One request at a time: a second one before the first is taken is refused.
    g_opened.clear();
    CHECK(SendText(window, clip));
    CHECK(!SendText(window, other));
    CHECK(Drain(window));
    CHECK(g_opened == std::vector<std::wstring>{clip});

    // Busy: refused, nothing kept, nothing posted.
    g_opened.clear();
    g_accepting = false;
    CHECK(!SendText(window, clip));
    CHECK(!g_requests.waiting());
    CHECK(Drain(window));
    CHECK(g_opened.empty());
    g_accepting = true;

    // Malformed payloads and spellings that are not a local path are dropped
    // without a trace.
    const std::wstring overlong = L"C:\\" + std::wstring(app::kMaxHandOverBytes / sizeof(wchar_t), L'a');
    CHECK(!SendText(window, L"clip.mp4"));
    CHECK(!SendText(window, L"\\\\server\\share\\clip.mp4"));
    CHECK(!SendText(window, L"\\\\?\\UNC\\server\\share\\clip.mp4"));
    CHECK(!SendText(window, L"https://example.com/clip.mp4"));
    CHECK(!SendText(window, overlong));
    CHECK(!SendText(window, clip, app::kOpenVideoHandOverMagic, false));
    CHECK(!SendText(window, clip, app::kOpenVideoHandOverMagic ^ 0xFF));
    CHECK(::SendMessageW(window, WM_COPYDATA, 0, 0) == FALSE);
    CHECK(!g_requests.waiting());
    CHECK(Drain(window));
    CHECK(g_opened.empty());

    // A well-formed local path is kept without touching the disk on this
    // thread: whether the file is there is the probe's answer, on its worker.
    CHECK(SendText(window, missing));
    CHECK(Drain(window));
    CHECK(g_opened == std::vector<std::wstring>{missing});
    g_opened.clear();
    CHECK(SendText(window, folder.path()));
    CHECK(Drain(window));
    CHECK(g_opened == std::vector<std::wstring>{folder.path()});
    g_opened.clear();

    // This process's own command line goes the same way.
    CHECK(g_requests.Queue(window, other));
    CHECK(!g_requests.Queue(window, clip));
    CHECK(Drain(window));
    CHECK(g_opened == std::vector<std::wstring>{other});

    // A second process hands a path over with the production sender.
    g_opened.clear();
    CHECK(RunSender(window_class, clip) == kSentTaken);
    CHECK(Drain(window));
    CHECK(g_opened == std::vector<std::wstring>{clip});
    g_opened.clear();
    g_accepting = false;
    CHECK(RunSender(window_class, clip) == kSentRefused);
    g_accepting = true;
    CHECK(RunSender(window_class, L"relative\\clip.mp4") == kSentRefused);  // Not a local absolute path.
    CHECK(RunSender(window_class + L"-nobody", clip) == kSentNoRecipient);
    CHECK(Drain(window));
    CHECK(g_opened.empty());

    ::DestroyWindow(window);
    ::UnregisterClassW(window_class.c_str(), window_type.hInstance);
    return Finish("copydata_handover");
}

int SendAsSecondProcess(const wchar_t* window_class, const wchar_t* path) {
    switch (app::HandOverOpenVideo(window_class, path, std::chrono::seconds(10))) {
        case app::HandOverResult::Taken:
            return kSentTaken;
        case app::HandOverResult::Refused:
            return kSentRefused;
        case app::HandOverResult::NoRecipient:
            return kSentNoRecipient;
    }
    return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    if (mode == L"--command-line") { return CommandLineParsing(); }
    if (mode == L"--validate") { return HandOverValidation(); }
    if (mode == L"--installer" && argc > 2) { return InstallerOpenWithList(argv[2]); }
    if (mode == L"--handover") { return CopyDataHandOver(); }
    if (mode == L"--send" && argc > 3) { return SendAsSecondProcess(argv[2], argv[3]); }
    std::fprintf(stderr, "usage: test_open_requests --command-line | --validate | --installer <easnap.iss> | "
                         "--handover\n");
    return 2;
}
