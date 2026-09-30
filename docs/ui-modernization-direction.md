# UI modernization direction

The user authorized this separate presentation effort on 2026-09-14, before
Phase 4 release hardening. The 2026-09-21 correction session is also accepted
and closed. Phase 3/3.3 acceptance stays closed; Scrollshot stays after Phase 4/v1.
Future window features must follow the
[window implementation guide](ui/window-implementation-guide.md).

Apply the supplied Lark reference proportions to real Win32 surfaces: a compact
Screenshot/Record strip, prominent recording Start above format/settings, a slim
pause/time/Stop bar, and a dominant preview above timeline and terminal actions.
Warm light and graphite dark palettes share dimensions and restrained orange
actions (the implemented coral accent). Raised/recessed treatment stays within small controls. Use readable text,
visible focus and system colors without decorative shadows in high contrast.

The existing coordinator owns settings and SessionState. Presentation-only theme
notifications must preserve image/crop/history, pending input, mode fade, recording
time, preview intent and export generations. The persisted preference uses the
existing INI; pending recording preferences are accepted only at successful Start.
Annotation and sampled colors are content, independent of UI colors.

Keep GDI damage-limited overlays, the single Direct2D editor, native controls and
MFPlay helper. Preserve real audio/cursor/GIF settings and existing publication,
recovery and private-storage contracts. No camera/OCR/Scrollshot/device-picker
placeholders, web UI, new capture engine or codec work is included.

All target surfaces are implemented. Release/Debug builds and affected UI checks
pass. The user subsequently reported manual validation and 21/21 tests passing
on 2026-09-14. Earlier agent-environment limitations remain historical evidence;
broader release coverage remains separate.
See the [validation report](ui/modernization-validation.md),
[design system](ui/design-system.md) and [project status](project-status.md).
Phase 4 remains unstarted and must validate the final UI.
