// Easnap - what Easnap.exe was asked to do by its command line (ADR-040).
#include "app/command_line.h"

#include <windows.h>

#include <string_view>
#include <utility>

#include "media/video_probe.h"
#include "platform/startup_entry.h"

namespace easnap::app {
namespace {

bool IsOption(const wchar_t* argument, const wchar_t* option) noexcept {
    return ::CompareStringOrdinal(argument, -1, option, -1, TRUE) == CSTR_EQUAL;
}

bool LooksLikeOption(std::wstring_view argument) noexcept {
    return argument.size() >= 2 && argument[0] == L'-' && argument[1] == L'-';
}

bool IsExistingFile(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) == 0;
}

/// A relative or drive-relative path made absolute; every other spelling
/// (absolute, UNC, a URL) stays as it is, and the probe answers for it.
std::wstring Absolute(std::wstring path) {
    if (path.empty() || media::CheckVideoPathSyntax(path) != media::VideoPathProblem::NotAbsolute) {
        return path;
    }
    const DWORD needed = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return path;
    }
    std::wstring full(needed, L'\0');
    const DWORD written = ::GetFullPathNameW(path.c_str(), needed, full.data(), nullptr);
    if (written == 0 || written >= needed) {
        return path;
    }
    full.resize(written);
    return full;
}

}  // namespace

CommandLineRequest ParseCommandLine(int argc, const wchar_t* const* argv) {
    CommandLineRequest request;
    if (argv == nullptr) {
        return request;
    }
    for (int index = 1; index < argc; ++index) {
        const wchar_t* const argument = argv[index];
        if (argument == nullptr) {
            request.bad = true;
            continue;
        }
        if (IsOption(argument, platform::kStartupArgument)) {
            request.startup = true;
            continue;
        }
        if (IsOption(argument, kOpenArgument)) {
            const wchar_t* const path = index + 1 < argc ? argv[index + 1] : nullptr;
            if (path == nullptr || path[0] == L'\0') {
                request.bad = true;
                continue;
            }
            ++index;
            if (request.open_path.empty()) {
                request.open_path = Absolute(path);
            } else {
                request.bad = true;
            }
            continue;
        }
        if (LooksLikeOption(argument)) {
            request.bad = true;
            continue;
        }
        std::wstring path = Absolute(argument);
        if (request.open_path.empty() && !path.empty() && IsExistingFile(path)) {
            request.open_path = std::move(path);
        } else {
            request.bad = true;
        }
    }
    return request;
}

}  // namespace easnap::app
