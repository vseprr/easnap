// Easnap - known folders, file names and directories.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <shlobj.h>

#include <optional>
#include <string>

namespace easnap::platform {

/// The path of a shell known folder, created if missing; nullopt on failure.
std::optional<std::wstring> KnownFolder(REFKNOWNFOLDERID folder_id);
/// A path in `directory` that no file uses yet: `file_name`, or the same name
/// with " (2)", " (3)" ... before its extension. Empty when the directory does
/// not exist.
[[nodiscard]] std::wstring UniqueFilePath(const std::wstring& directory, const std::wstring& file_name);
/// %LOCALAPPDATA%\Easnap, created on demand.
std::wstring AppDataDirectory();
/// `path` without its last component; empty when it has no separator.
std::wstring DirectoryOf(const std::wstring& path);
/// `prefix`_YYYY-MM-DD_HH-MM-SS.`extension` in local time.
std::wstring TimestampedFileName(const wchar_t* prefix, const wchar_t* extension);
/// Creates the directory unless it exists. False when it cannot be created or
/// a file has its name (GetLastError is then ERROR_DIRECTORY).
bool EnsureDirectory(const std::wstring& path);
/// Deletes a directory strictly below AppDataDirectory(); refuses reparse points.
/// Returns false on incomplete cleanup, leaving unremoved entries intact.
bool RemoveDirectoryTree(const std::wstring& path);

}  // namespace easnap::platform
