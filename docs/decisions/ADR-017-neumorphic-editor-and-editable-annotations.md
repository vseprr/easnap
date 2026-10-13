# ADR-017: Neumorphic controls and editable screenshot annotations

Date: 2026-09-16  
Status: Implemented.  
**Amended by:** [ADR-018](ADR-018-editor-interaction-corrections.md) (selection, text editing, compact controls); [ADR-019](ADR-019-stable-native-paint-and-custom-window-chrome.md) (custom message dialog and window chrome); [ADR-020](ADR-020-workspace-layering-and-outline-interaction.md) (hit targets follow outlines); [ADR-036](ADR-036-island-placement-around-the-frame.md) (island placement around the frame)

## Context

The separately authorized UI/editor overhaul extends the accepted Phase 3
application. It does not start Phase 4. The behavior samples specify interaction
and composition; the supplied neumorphic illustration specifies material and
lighting. The source report identifies 26 Illustrator effect styles, but their
coordinates, blur values and raw blend-mode codes are not verified Easnap DIPs
or Direct2D parameters. There is no source-derived Dark palette.

This decision extends ADR-016 and replaces the old toolbar, annotation input
and incremental mosaic details in ADR-003. Capture ownership, native recording,
preview isolation, publication and recovery contracts remain in force.

## Decisions

1. Shared controls use neutral, uniform panel backgrounds and locally shaded
   material. Cached antialiased masks with separable Gaussian filtering supply
   paired raised shadows and clipped recessed shading. The latter is an
   adaptation of the source's separate blurred CH/VOL edge geometry, not a claim
   that a single Illustrator Inner Shadow produced it. Dark uses the same
   upper-left lighting direction with deliberately chosen graphite values.
2. Native BUTTON, EDIT, COMBOBOX, TRACKBAR and progress controls retain their
   input and accessibility behavior. Shared rendering handles material and
   vector icons; a scoped owner-draw adapter styles native menus, and
   application messages use the shared custom modal dialog (`ShowMessage`,
   ADR-019). Windows file pickers remain
   system-owned. Focus is independent of checked and pointer states.
3. Hover/press/release retarget local timers immediately. Reduced client-area
   animation or high contrast disables transitions. Sliders and trim handles
   follow input directly. Theme buttons toggle immediately through the existing
   settings owner; they neither commit pending recording choices nor modify
   screenshot content. The tray retains explicit Light/Dark choices.
4. The slim main screenshot island contains tools and actions. All per-tool
   options live in one separate contextual island, driven by the selected
   annotation or the active tool's defaults. Pointer with no selection has no
   empty options panel. The group prefers below the frame, then above, then a
   work-area-clamped fallback. Blank space and grips drag the main island;
   interactive children do not. Manual placement survives option changes until
   frame geometry changes. Both islands hide at frame-resize press and restore
   on release or capture loss without requesting acquisition.
5. The mode selector has a dot grip and horizontal blank-space dragging clamped
   to its current work area. Its established retirement fade and mode lifecycle
   remain intact. Existing first-main-island-interaction acquisition still
   applies; merely showing or hiding islands does not acquire pixels.
6. An annotation document stores persistent objects and reversible snapshots of
   annotation state, never copies of the captured image. Selection is separate
   from creation defaults. A completed geometry or slider gesture is one undo
   entry; cancellation restores the preceding state. Lines/arrows expose two
   endpoints; rectangular/elliptical shapes, effect regions and spotlight
   regions expose four corners. Pen, text and tags move without resize handles.
   Watermark is a document effect, with no repeated-word transform handles.
7. Handles take precedence over object movement. Pointer selects arbitrary
   objects, and the selected object remains movable with its tool active.
   Choosing a creation tool again clears selection for overlapping new objects.
   Text double-click always starts native caret editing, including unselected
   text. Word selection, paragraph triple-click and clipboard input stay in the
   EDIT. Editor gestures do not enter overlay quick-copy; blank Pointer content
   retains quick-copy. Escape rolls back an active edit/transform before exiting.
8. Text is measured and rendered through DirectWrite. Point sizes convert once
   using the acquired image's DPI, independent of later UI scaling. Plain,
   rounded Fill and glyph Outline are exactly three visual modes; multiline is
   ordinary text input. Fill/Outline use white glyphs, or black for a white
   chosen color. Tag body/text are fixed content colors, with a colored dot and
   notch. Theme changes cannot recolor exported annotations.
9. Blur/pixelation rebuild derived pixels from the immutable capture. Brush and
   region are separate application methods; strength controls blur radius or
   block width. Moving/removing effects cannot leave old redacted pixels.
   Spotlight regions form one visibility union, with one shared outside opacity;
   overlapping regions never apply the dim layer twice. Angled tiled watermark
   text is clipped to image bounds. Preview and export use the same effect and
   annotation renderers; selection chrome is painted only in the editor.

## Consequences and validation boundary

The renderer adds Windows GDI+ and accessibility linkage, not a new UI framework.
Geometry masks have a bounded 64-entry thread-local cache. Vector helpers honor
GDI damage regions; rounded frame labels include their antialiased fringe in
damage bounds. Frame buffers and capture/media backends remain unchanged.

CPU blur uses three linear-time sliding box passes as a Gaussian approximation
over an expanded effect area. Very large images and long effect histories still
need hardware-specific profiling. Native HWND fixtures establish rendering,
document behavior and message routing, not perceived animation smoothness or
physical mixed-DPI operation.
