# ADR-030 — Log file and crash reports

**Status:** Accepted
**Date:** 2026-10-13
**Amended:** 2026-11-07 — a report never replaces another
**Amended:** 2026-11-09 — the crash line names the step that failed and its
error, a failed dump is tried again, and the report thread allocates nothing
itself

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
- A line with the failure's code and thread says what became of the report
  (amended 2026-11-09): `crash report written`, followed by
  `, attempt N after 0x…` when an earlier attempt failed, or
  `crash report not written: <step> failed with 0x…`. The step is
  `writer load` (dbghelp or its `MiniDumpWriteDump` could not be loaded when
  the handler was installed; that error), `create` (creating the file failed;
  the Windows error) or `write` (every attempt failed; the error of the first
  one, then `attempts: N`). The line holds no path. It goes to the log; when
  the log is not open (the preview helper keeps none), the failing thread
  holds its lock or the write fails, the same line goes to the debugger
  through `OutputDebugStringA`, which, unlike the W function, converts
  nothing on the heap.
- When `MiniDumpWriteDump` fails, the file is emptied and the dump tried
  again after 100 ms: four attempts in all, the last one with
  `MiniDumpNormal` alone. A retry is started only while the request is less
  than five seconds old, so the last attempt begins by about 5.1 seconds and
  ends long before the failing thread stops waiting at 20 seconds. The file
  is deleted only when every attempt failed.
- Once a failure is reported, the report thread calls nothing of the C
  runtime and allocates nothing itself: the file name and the line are built
  in fixed buffers on its stack, the name's folder part and the log's state
  when the handler is installed. Before, its first `swprintf` could set up the
  runtime's data for the thread on the heap while the failing thread held a
  lock. The Windows functions that create the file and write the dump can
  still use the process heap; a failure that leaves it locked ends when the
  failing thread stops waiting, without a report or a line.
- After the log starts, one line says how the installation went: installed,
  installed without reports because the writer could not be loaded, or not
  installed because of the folder, the events or the thread, with the error.
  The preview helper sends that line to the debugger.
- Some failures never reach the handler, and Windows Error Reporting takes
  them without a line: the fail-fast family (`__fastfail`, a stack buffer
  overrun, 0xC0000409), heap corruption (0xC0000374), a second failure while
  the first is being reported, and termination from outside. An access
  violation, a C++ exception and a stack overflow end with the same code
  whether the handler ran or not, so for these three the exit code alone does
  not prove that it ran; the line does. The 0xE0E5xxxx codes come only from
  the handler.
- A dump cut off at the 20-second limit keeps its `.dmp` name. Writing under
  a temporary name and renaming on success was left out: it adds a step that
  can fail on the crash path, the counter would have to keep both names
  unique, and left-over temporary files would escape the retention limit.
- A report never replaces another (amended 2026-11-07). The name carries the
  time to the millisecond, the file is created only if it does not exist, and
  a name that exists gets a counter (`-2` to `-9`). Before, the name had
  one-second resolution and an existing file was overwritten. On GitHub's
  hosted runner `diagnostics_log_and_crash_reports` failed in 2 of 40 jobs
  with every later report missing; the likely cause is that Windows reused
  the process ID within the same second, so a report got the name of the
  previous one and could not replace it. With unique names it happened once
  more (2026-11-08, Debug job of the v1.2.0 release run): the children ended
  with the expected codes, so the handler ran, but 7 of 8 wrote no report.
  The reuse of names was therefore not the whole cause. The line said only
  "could not be written" and the file was deleted, so which step failed there
  is not known; since 2026-11-09 the line names the step and the error, and
  the test prints it for every child, so the next occurrence will say.

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
must end with the expected code, leave one valid minidump under eight
megabytes and log that the report was written; the retention limit and the
"seen" mark are checked on the way. A ninth child removes its crash folder
after installing the handler and then causes an access violation: it must end
with that code, leave no report and log that creating the file failed with
0x00000003 (path not found). The test prints every child's line. A folder
that cannot be created reports the `Folder` step with that error. The retry
is not exercised: `MiniDumpWriteDump` cannot be made to fail without a fake.
The test passes in Release and Debug.

In the running application the log file, the notice after a report and its
single appearance were checked by hand.
