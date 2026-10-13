# ADR-032 — Interface text lives in string tables, in English and Turkish

**Status:** Accepted
**Date:** 2026-10-13

## Context

Until v1.0.2 every word the user reads was an English literal in the C++
sources: about 330 of them in ten files. A Turkish user got an English
application, and a wording change meant a search through the code.

## Decision

### One accessor, two tables

- `resources/string_ids.h` numbers every string. `resources/strings_en.rc` and
  `resources/strings_tr.rc` hold one `STRINGTABLE` each, tagged with their
  language. `resources/strings.rc` includes both and is the only file the
  resource compiler is given; it carries `#pragma code_page(65001)` itself,
  because `rc.exe` ignores that pragma in the first file it includes.
- `strings::Get(id)` returns the text in the active language and falls back to
  English when the Turkish table has no entry. `strings::Format(id, {...})`
  replaces `%1` to `%9`, so a translation can order its arguments freely.
  `strings::Get(id, language)` reads one table without fallback and exists for
  tests.
- The CMake `INTERFACE` library `easnap_strings` gives a target the accessor
  and both tables. The application and every test that creates production
  windows link it.

### Which language

`strings::SetPreference` takes `System`, `English` or `Turkish`. `System`
follows the Windows display language: Turkish there selects the Turkish table,
anything else English. The choice is the `Language` key of `settings.ini`, set
in Settings. It applies to text requested afterwards, without a restart;
a window that is already open keeps the text it was created with. Text shown
before the settings are read (a failure to start) follows Windows.

### What is not translated

- Log lines and the details About copies to the clipboard. They are read by
  whoever handles a bug report.
- UI Automation identifiers. Accessible names are translated; identifiers are
  what scripts and tests address and stay English.
- The window titles "Edit annotation text" and "Brush sizes", which tests use
  to find those windows and which no user sees.
- The product name.

### Boundaries

Low-level files do not depend on the string module. `platform_utils.cpp` and
`recording_completion.cpp` are compiled into many test executables; the system
Save dialogs take their captions from the caller (`SaveImageDialogText`,
`SaveRecordingDialogText`).

### Text has to fit

Turkish runs longer than English. Three places size themselves to their text
instead of to a constant: the mode bar (one width for all three buttons, from
the widest label), the Stop button of the recording island, and the button row
of a dialog. `text_fits_in_every_language` creates the production windows on a
private desktop in both languages and checks that no label is wider than the
control that shows it.

`strings_catalog` reads both tables through the accessor and checks that
every identifier has text in both languages, that the two use the same
placeholders, and that the tables arrive as UTF-8.

## Consequences

- New text is added to both tables in the same change. A string missing from
  the Turkish table shows in English and fails `strings_catalog`.
- A sentence is one string. Text is never assembled from translated fragments;
  word order differs between the two languages.
- Wording that names a destructive action must be unambiguous in each
  language. The Turkish buttons that delete say "Sil"; "Vazgeç" reads as
  "Cancel" to many Turkish users and is used only where giving up the current
  capture is meant.
- The Turkish text was written by the implementer and read in the running
  application. It has not been reviewed by a second reader.
- Further languages need a table, an entry in `strings::Language` and a rule
  for `System`. Right-to-left layout is not prepared.
