## What changes and why

<!-- The behaviour that prompted this, and what is different afterwards. -->

## How it was checked

<!-- Say what you actually ran. Mark what you could not run as pending. -->

- [ ] `.\scripts\build.ps1 -Configuration Release` (builds, runs the affected tests)
- [ ] `.\scripts\test.ps1 -Tier Full` on display: <!-- needed for windows, capture or recording -->
- [ ] `python scripts/check_repository_hygiene.py`

Windows version, displays and scale used:

## Checklist

- [ ] No captures, recordings, build output or local settings are included.
- [ ] New user-visible text is in both `strings_en.rc` and `strings_tr.rc`.
- [ ] Documentation that describes the changed behaviour is updated
      (`docs/user-flows.md`, `docs/architecture.md`, an ADR for a durable decision).
- [ ] A window's layout, painting or lifetime changed: I followed
      `docs/ui/window-implementation-guide.md`.
