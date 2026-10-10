# ADR-026 — Test tiers and desktop isolation

**Status:** Accepted
**Date:** 2026-10-10
**Amends:** [ADR-025](ADR-025-incremental-test-execution.md)

## Context

The complete suite took about 790 s and ran on the developer's own desktop.
Roughly two thirds of that time repeated paths already covered: three random
seeds of the same preview stress, three 30-second seek runs and a 72-click
timeline run. Nine tests were serialised as "interactive" although three of
them only send window messages. Any terminal with a different environment
invalidated every cached pass, because the whole environment was hashed.
A test that polled for a state with a fixed 20 ms sleep failed under load.

The developer uses the same computer while tests run, with two displays.

## Decision

- **Tiers from labels.** `unit` is fast; `interactive-desktop` or
  `desktop-visual` is desktop; `soak` is soak; everything else is integration.
  `scripts/test.ps1 -Tier Fast|Standard|Full|Soak` selects them. Standard is
  the default and reports desktop tests as pending.
- **Soak instead of deletion.** No assertion was removed. Stress seeds 1 and 2,
  the three 30-second seek runs and the 72-click timeline run are registered
  with the `soak` label. One seed, one 15-second seek run and a 24-click run
  stay in the normal tiers. Every threshold in the shortened runs is unchanged.
- **Private desktop for message-driven window tests.** `easnap_isolated_desktop`
  starts a test on a desktop created with `CreateDesktop`. The three
  `capture_*` tests moved there, left the serial interactive group and run in
  parallel in the background. This was adopted only for tests that were run
  there and passed unchanged.
- **Visible-desktop tests are explicit.** They run last, serially, on a display
  the user chooses, after a warning and countdown, and are retried once. A pass
  on retry is reported as unstable and not cached.
- **Fingerprint only what matters.** The environment part of a fingerprint is
  the `EASNAP_*` variables. Shared headers, resources, tooling and unmapped
  sources still invalidate everything, as ADR-025 decided.
- **No second pointer.** Windows offers no private system pointer for a
  process. Synthetic touch or pen injection does not move the mouse pointer
  but exercises a different input path than the one under test, so it was not
  used. Isolation comes from not needing the desktop, not from a fake device.

## Consequences

A Standard run after an unchanged build executes nothing. On the development
machine a Full run took 91 s for 26 background tests and 274 s for 7
visible-desktop tests, against 790 s for the previous suite; a Soak run took
195 s and 390 s. The three private-desktop tests are no longer excluded from
hosted CI.

The desktop tier still shares the input queue and focus with the user; the
tooling reduces interference and reports it, it does not eliminate it: in the
first complete Soak run a real-pointer test failed because the mouse was moved
during it. Soak coverage runs only when asked for and should be run before a
release. Not every desktop test follows the chosen display yet.

Complete isolation of the desktop tier needs a second Windows session (a
virtual machine or another PC). A Linux host cannot run these tests.
