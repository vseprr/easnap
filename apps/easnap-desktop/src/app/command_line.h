// Easnap - what Easnap.exe was asked to do by its command line (ADR-040).
//
// Three requests exist: "--startup" from the sign-in entry Settings writes, a
// video to open ("--open <path>", which Explorer's "Open with" runs, or one
// bare argument naming an existing file), and nothing, which is a request to
// capture when Easnap already runs. Anything else is ignored.
#pragma once

#include <string>

namespace easnap::app {

/// Precedes the path of the video to open.
inline constexpr wchar_t kOpenArgument[] = L"--open";

struct CommandLineRequest final {
    /// "--startup" was given as an argument of its own: stay quiet.
    bool startup{false};
    /// The video to open, made absolute when it was relative to the current
    /// directory; empty when none was named.
    std::wstring open_path;
    /// At least one argument was ignored: an unknown option, "--open" without
    /// a path, a second path, or a bare argument that names no existing file.
    bool bad{false};
};

/// Reads `argv` as CommandLineToArgvW splits it; argv[0] is the program and is
/// skipped. An option counts only as a whole argument, in any ASCII case, so a
/// path that merely contains "--startup" is never the option. The argument
/// after "--open" is the path, as given; a bare argument is a path only when
/// it names an existing file. The first path named is the one opened. A
/// relative path is made absolute against the current directory, so that the
/// running Easnap can be handed it. Reads the current directory and file
/// attributes, nothing else.
[[nodiscard]] CommandLineRequest ParseCommandLine(int argc, const wchar_t* const* argv);

}  // namespace easnap::app
