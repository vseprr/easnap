# Documentation

Start with [project status](project-status.md) for the current baseline, actual
validation, limitations and next slice. Read the [roadmap](../EASNAP_ROADMAP.md)
alongside it. Contributor instructions are in [CONTRIBUTING.md](../CONTRIBUTING.md).

| Document | Purpose |
|---|---|
| [README](../README.md) | Product overview, build, run and test instructions |
| [Roadmap](../EASNAP_ROADMAP.md) | Accepted phases and current bounded work |
| [Architecture](architecture.md) | Implemented modules, ownership and data flow |
| [Development](development.md) | Building, running, packaging and source layout |
| [Testing](testing.md) | Test tiers, running only what changed, display selection and isolation from the user |
| [User flows](user-flows.md) | Current interaction, completion and retry behavior |
| [Troubleshooting](troubleshooting.md) | What goes wrong most often and what to do, and how to report a problem |
| [Distribution](distribution.md) | Version text, packages, symbol files, the release steps and package-manager manifests |
| [Design system](ui/design-system.md) | Implemented colors, controls, accessibility and layout |
| [Window implementation guide](ui/window-implementation-guide.md) | Required shared-code, layout, painting and regression-validation path for new or changed windows |
| [Security](../SECURITY.md) | Reporting, private storage and publication model |
| Release notes | [v1.1.0-beta.1](releases/v1.1.0-beta.1.md), [v1.0.2](releases/v1.0.2.md), [v1.0.1](releases/v1.0.1.md), [v1.0.0](releases/v1.0.0.md): what each version changed and how it was checked |
| [Validation records of v1.0.1 and v1.0.2](releases/validation-history.md) | The individual runs behind those release notes, including the ones that failed |

## Decisions

ADRs preserve the reasons and contracts behind the implementation. Amended
decisions describe current behavior; historical observations remain evidence,
not unfinished roadmap instructions.

- Workspace: [GDI overlay](decisions/ADR-001-overlay-rendering-gdi-not-d3d.md),
  [state and completion](decisions/ADR-002-explicit-region-confirmation.md),
  [editor/export](decisions/ADR-003-editor-window-and-export-model.md),
  [frame performance](decisions/ADR-015-frame-interaction-rendering.md),
  [island placement around the frame](decisions/ADR-036-island-placement-around-the-frame.md).
- Appearance/accessibility: [shared theme and native controls](decisions/ADR-016-shared-appearance-and-native-controls.md),
  [neumorphic controls and editable annotations](decisions/ADR-017-neumorphic-editor-and-editable-annotations.md),
  [editor interaction corrections](decisions/ADR-018-editor-interaction-corrections.md),
  [stable native painting](decisions/ADR-019-stable-native-paint-and-custom-window-chrome.md),
  [workspace layering and outline interaction](decisions/ADR-020-workspace-layering-and-outline-interaction.md),
  [native text input](decisions/ADR-022-text-input-and-content-sized-trim.md),
  [layout and click continuations](decisions/ADR-023-trim-resize-and-text-click-continuations.md),
  [accepted native resize frame](decisions/ADR-024-native-trim-frame.md).
- Capture/recording: [output fallback](decisions/ADR-004-per-output-capture-fallback.md),
  [active cadence](decisions/ADR-005-recording-active-timeline-and-cadence.md),
  [audio clock/queues](decisions/ADR-006-audio-capture-clock-and-buffering.md),
  [A/V timing](decisions/ADR-007-av-mux-and-active-time-audio.md),
  [controls/finalization](decisions/ADR-008-recording-ui-and-safe-finalization.md),
  [Both audio](decisions/ADR-012-dual-source-audio-mixing.md),
  [startup, pacing and session leases](decisions/ADR-021-recording-startup-timeline-and-session-leases.md).
- Media/completion: [MP4 trim](decisions/ADR-009-mp4-trim-boundaries-and-publication.md),
  [preview ownership](decisions/ADR-010-trim-view-ownership-and-orchestration.md),
  [publication/clipboard lifetime](decisions/ADR-011-recording-completion-and-clipboard-lifetime.md),
  [GIF policy](decisions/ADR-013-gif-stream-timing-palette-and-publication.md),
  [shared configuration/completion](decisions/ADR-014-unified-recording-configuration-and-format-completion.md).
- Scrollshot: [stitching engine](decisions/ADR-027-scrollshot-stitching-engine.md),
  [capture workspace](decisions/ADR-028-scrollshot-capture-workspace.md),
  [editing an image larger than the screen](decisions/ADR-029-scrolled-and-zoomed-image-editing.md).
- Diagnostics and data safety: [log file and crash reports](decisions/ADR-030-log-file-and-crash-reports.md),
  [unsaved work survives exit, sign-out and crashes](decisions/ADR-031-unsaved-work-survives.md).
- Shell and compatibility: [interface text in string tables](decisions/ADR-032-interface-text-in-string-tables.md),
  [Settings, the capture shortcut and how Easnap starts](decisions/ADR-033-settings-shortcut-and-launch-behaviour.md),
  [Media Foundation on first use](decisions/ADR-034-media-foundation-on-first-use.md),
  [prerelease versions, one channel and symbol files](decisions/ADR-035-prerelease-versions-and-distribution.md).
- Development/testing: [incremental execution and desktop ownership](decisions/ADR-025-incremental-test-execution.md),
  [test tiers and desktop isolation](decisions/ADR-026-test-tiers-and-desktop-isolation.md).

Keep progress in project status, durable decisions in ADRs, and measured
evidence in the ADR it supports or in the validation record of its release.
A finished investigation leaves its conclusion in an ADR; the report itself is
not kept. Avoid duplicating phase histories across these files.


Raw diagnostic media and machine-specific tool settings are not part of the
public repository.
