# ADR-025 — Incremental test execution and desktop ownership

**Status:** Accepted  
**Date:** 2026-09-23  
**Amended by:** [ADR-026](ADR-026-test-tiers-and-desktop-isolation.md)

ADR-026 replaces the run modes described below with tiers, moves the repeated
stress seeds to a soak tier, hashes only `EASNAP_*` environment variables and
runs message-driven window tests on a private desktop. The caching rules in
this document still apply. Current commands are in [testing](../testing.md).

## Context

The user develops and uses other applications on the same Windows desktop.
Running every native media stress scenario after an unchanged build is slow;
tests using global input or desktop pixels can fail because another application
owns focus. Faster iteration must preserve assertions and full regression scope.

## Decision

- `build.ps1` always builds before selecting tests. Its default `Affected` mode
  fingerprints built test executables, helper executables from CMake's target
  graph, command/properties, configuration and environment. Shared headers,
  resources, tooling and unmapped production code conservatively invalidate all
  tests. This is dependency-aware result reuse, not source-class impact analysis.
- Only selected tests with an actual successful JUnit result are cached, for at
  most 24 hours. A selected result is invalidated before execution. Missing,
  failed, interrupted, skipped, expired or malformed results require execution.
  Cached passes and newly executed passes are reported separately.
- Physical input/focus/capture tests remain serial and default to explicitly
  pending in daily runs. They run with `-DesktopTests Run`. `Full` and CI always
  run the complete suite; `-WithTests` retains that full-run meaning. `None` is
  an explicit build-only choice. CMake 3.20 uses uncached execution; 3.21+ JUnit
  support enables the local cache.
- Independent tests use four CTest slots by default. The original three preview
  stress seeds are separate cases with unchanged actions and pacing; each uses
  two scheduling slots. Real audio checks share a resource lock. Finalizer
  success polling uses 1 ms; the lost-notification recovery test keeps 250 ms.
- Standalone preview pixel tests use a nonactivating topmost tool window, verify actual
  visibility and capture the real desktop. Display changes recreate capture and
  refit the window. They do not take the user's foreground focus or move input.
  Failure to obtain real visible pixels fails explicitly, without a fake pass.
- The production trim endpoint test additionally maximizes/restores the real
  trim window and samples natural end and replay in both themes. It belongs to
  the serial `interactive-desktop` group and is deferred with that group in
  daily runs; it is not covered by the standalone window's focus isolation.

## Consequences

Unchanged iterations are short; full validation retains all scenarios. The
local cache is not a release gate and does not replace manual display/device
acceptance. Real mouse drags and focus-sensitive checks still require a quiet
desktop. Display reconfiguration or exclusive fullscreen applications can
invalidate a live measurement; no guarantee of isolation from these is made.
Direct `test.ps1` or CTest commands operate on already-built binaries.
