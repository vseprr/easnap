# ADR-030 — Log file and crash reports

**Status:** Accepted
**Date:** 2026-10-13

## Context

`LogMessage` wrote to the debugger only. On a user's computer a failure left
nothing behind: no line saying what Easnap had been doing, and after a crash
only what Windows Error Reporting keeps by default. The portable readme
already said that diagnostics are stored under `%LOCALAPPDATA%\Easnap`; none
were.

Easnap makes no network connection, and that does not change. Whatever is
kept has to stay on the computer until the user decides to share it.

## Decision

`platform/diagnostics` owns both mechanisms. Nothing in it opens a network
connection.

### Log file

`%LOCALAPPDATA%\Easnap\logs\easnap.log` receives every `LogMessage` line with
a UTC time and the thread id. A file that has reached one megabyte is set
aside as `easnap.1.log` before the next line is written, the previous one
becomes `easnap.2.log`, and the oldest is dropped: three files, three
megabytes plus a line each at most, and the newest line always in
`easnap.log`. The file is opened for appending and shared, so it can be read
while Easnap runs.

`LogMessage` keeps its signature. `platform::SetLogSink` connects it to the
file, so the tests and the helper that compile `platform_utils.cpp` without
the diagnostics module are unaffected.

A log line never contains user content: no path, file name, window title or
text the user typed. The 147 call sites that existed were checked against that
rule when the file was introduced; new ones follow it. The rule is stated at
the declaration of `LogMessage`.

### Crash reports

`InstallCrashHandler` runs first thing in both executables. From then on an
unhandled exception, `std::terminate`, `abort`, a pure virtual call or an
invalid CRT parameter on any thread writes
`%LOCALAPPDATA%\Easnap\crashes\<name>-<UTC time>-<pid>.dmp` and ends the
process with the failure's code.

- The report is a minidump with thread information and unloaded modules. It
  has no heap. It does contain the call stack of every thread, and a stack can
  hold fragments of what the process was handling: a path, a piece of text.
- A dedicated thread writes it, so a stack overflow can be reported too. A
  failure the runtime detects (`terminate`, pure call, invalid parameter) is
  turned into an exception first, so the report shows where it happened.
- `dbghelp.dll` is loaded from the system directory when the handler is
  installed; nothing is resolved while the process is failing.
- At most three reports are kept. Older ones are deleted at the next start.
- A line with the failure's code goes to the log when the log is not locked by
  the failing thread.

The next start tells the user once: Easnap closed unexpectedly, a report was
saved on this computer, nothing has been sent, here is the folder. The report
is then marked as seen.

## Consequences

- Reading a report needs the symbol files of the build that wrote it. Release
  builds of both executables therefore produce them
  (`easnap_keep_release_symbols`), `package-release.ps1` puts them into a ZIP
  of their own and the Release workflow attaches that ZIP to the release. The
  executables record only the file name of their symbol file. See
  [distribution](../distribution.md).
- The test of this handler cannot run under AddressSanitizer: the sanitizer
  takes an access violation before the application's handler sees it.
- A debug build shows the debug runtime's own dialog before the handler runs.
- A recording that was being written when the process died cannot be played:
  the MPEG-4 writer stores its index when it finalizes. A finished recording
  is a different matter; see [ADR-031](ADR-031-unsaved-work-survives.md).
- Test executables that start the preview helper create the `crashes` folder
  in the profile of the person running them.

## Validation

`diagnostics_log_and_crash_reports` writes through the production sink into a
directory of its own: line format, UTF-8, truncation of an over-long line,
appending across restarts, rotation under four writing threads. A second
process installs the production handler and fails on purpose in eight ways
(access violation, C++ exception, `terminate` and a `noexcept` violation on a
worker thread, `abort`, pure call, invalid parameter, stack overflow). Each
must end with the expected code and leave one valid minidump under eight
megabytes; the retention limit and the "seen" mark are checked on the way.
The test passes in Release and Debug.

In the running application the log file, the notice after a report and its
single appearance were checked by hand.
