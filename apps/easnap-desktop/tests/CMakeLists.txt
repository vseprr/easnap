# ---------------------------------------------------------------------------
# Easnap regression tests
#
# Most targets here are console executables testing pure, deterministic logic.
# Native UI and media integration targets additionally exercise real windows,
# automation, graphics, codecs, clipboard and available capture/audio devices.
# Label those requirements explicitly; capability skips are not device coverage.
# Tests should exercise production logic and guard a documented behavior.
#
# scripts/test.ps1 derives a tier from the labels (docs/testing.md):
#   unit                                 -> fast     (seconds, every run)
#   interactive-desktop / desktop-visual -> desktop  (needs the visible desktop)
#   soak                                 -> soak     (long repeats, on demand)
#   anything else                        -> integration
# Tests wrapped in easnap_isolated_desktop run on a private, non-input desktop:
# they show nothing on screen and cannot be disturbed by the person using the PC.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# test_session_state — state machine transition graph (ADR coverage)
#
# Links the production-owned transition rules directly. It deliberately avoids
# capture_provider.cpp and every graphics/UI translation unit.
# ---------------------------------------------------------------------------

# Runs a test on a private, non-input Windows desktop (see the source header).
add_executable(easnap_isolated_desktop isolated_desktop_runner.cpp)
target_link_libraries(easnap_isolated_desktop PRIVATE easnap_project_options user32)

add_executable(test_recording_finalizer
    test_recording_finalizer.cpp
    ../src/app/recording_finalizer.cpp)
target_include_directories(test_recording_finalizer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_recording_finalizer PRIVATE easnap_project_options user32)
add_test(NAME recording_finalizer_recovery COMMAND test_recording_finalizer)
set_tests_properties(recording_finalizer_recovery PROPERTIES
    LABELS "unit;recording;lifecycle" TIMEOUT 30)

add_executable(test_session_state
    test_session_state.cpp
    ../src/app/session_transitions.cpp
    ../src/app/recording_workflow.cpp
    ../src/app/app_settings.cpp
    ../src/platform/platform_utils.cpp)

target_include_directories(test_session_state PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")

target_link_libraries(test_session_state PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid)

add_test(NAME session_state_transitions
         COMMAND test_session_state)
set_tests_properties(session_state_transitions PROPERTIES
    LABELS "unit;state-machine"
    TIMEOUT 30)

add_executable(test_trim_workflow
    test_trim_workflow.cpp
    ../src/app/trim_workflow.cpp
    ../src/app/session_transitions.cpp
    ../src/media/preview_transport.cpp
    ../src/ui/trim_layout.cpp
    ../src/platform/platform_utils.cpp)

target_include_directories(test_trim_workflow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_trim_workflow PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    shlwapi comctl32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32 propsys)

add_test(NAME trim_workflow_orchestration
         COMMAND test_trim_workflow)
set_tests_properties(trim_workflow_orchestration PROPERTIES
    LABELS "unit;state-machine;trim"
    TIMEOUT 30)

add_executable(test_recording_completion
    test_recording_completion.cpp
    ../src/app/recording_completion.cpp
    ../src/app/session_transitions.cpp
    ../src/platform/platform_utils.cpp)

target_include_directories(test_recording_completion PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_recording_completion PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    shlwapi comctl32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32 propsys)

add_test(NAME recording_completion_workflow
         COMMAND test_recording_completion)
set_tests_properties(recording_completion_workflow PROPERTIES
    LABELS "unit;state-machine;recording;clipboard"
    TIMEOUT 30)

add_executable(test_frame_queue
    test_frame_queue.cpp)

target_include_directories(test_frame_queue PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_frame_queue PRIVATE easnap_project_options)

add_test(NAME frame_queue_semantics
         COMMAND test_frame_queue)
set_tests_properties(frame_queue_semantics PROPERTIES
    LABELS "unit;recording-foundation"
    TIMEOUT 30)

add_executable(test_audio_timeline
    test_audio_timeline.cpp
    ../src/recording/audio_timeline.cpp)

target_include_directories(test_audio_timeline PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_audio_timeline PRIVATE easnap_project_options)

add_test(NAME audio_active_timeline
         COMMAND test_audio_timeline)
set_tests_properties(audio_active_timeline PROPERTIES
    LABELS "unit;recording;audio"
    TIMEOUT 30)

add_executable(test_dual_audio_mixer
    test_dual_audio_mixer.cpp
    ../src/media/dual_audio_mixer.cpp)

target_include_directories(test_dual_audio_mixer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_dual_audio_mixer PRIVATE easnap_project_options)

add_test(NAME dual_audio_mixer
         COMMAND test_dual_audio_mixer)
set_tests_properties(dual_audio_mixer PROPERTIES
    LABELS "unit;recording;audio"
    TIMEOUT 30)

add_executable(test_audio_resampler
    test_audio_resampler.cpp
    ../src/media/audio_resampler.cpp)
target_include_directories(test_audio_resampler PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_audio_resampler PRIVATE easnap_project_options
    mf mfplat mfuuid wmcodecdspuuid ole32 uuid)
add_test(NAME audio_resampler_dsp
         COMMAND test_audio_resampler)
set_tests_properties(audio_resampler_dsp PROPERTIES
    LABELS "integration;media-foundation;recording;audio"
    TIMEOUT 30)

add_executable(test_recording_worker
    test_recording_worker.cpp
    ../src/capture/recording_capture_worker.cpp)

target_include_directories(test_recording_worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_recording_worker PRIVATE easnap_project_options user32)

add_test(NAME recording_worker_foundations
         COMMAND test_recording_worker)
set_tests_properties(recording_worker_foundations PROPERTIES
    LABELS "unit;recording-foundation"
    TIMEOUT 30)

add_executable(test_media_foundation_writer
    test_media_foundation_writer.cpp
    ../src/media/media_foundation_writer.cpp
    ../src/capture/recording_capture_worker.cpp
    ../src/platform/platform_utils.cpp)

target_include_directories(test_media_foundation_writer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_media_foundation_writer PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    mf mfplat mfuuid mfreadwrite
    shlwapi comctl32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32 propsys)

add_test(NAME media_foundation_h264_aac_mp4
         COMMAND test_media_foundation_writer)
set_tests_properties(media_foundation_h264_aac_mp4 PROPERTIES
    LABELS "integration;media-foundation;recording;audio"
    TIMEOUT 120)

add_executable(test_mp4_trim_exporter
    test_mp4_trim_exporter.cpp
    ../src/media/mp4_trim_exporter.cpp
    ../src/media/media_foundation_writer.cpp
    ../src/media/mf_d3d_device.cpp
    ../src/platform/platform_utils.cpp)

target_include_directories(test_mp4_trim_exporter PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_mp4_trim_exporter PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    mf mfplat mfuuid mfreadwrite
    shlwapi comctl32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32 propsys)

add_test(NAME mp4_trim_export_backend
         COMMAND test_mp4_trim_exporter)
set_tests_properties(mp4_trim_export_backend PROPERTIES
    LABELS "integration;media-foundation;trim;audio"
    TIMEOUT 180)

add_executable(test_media_preview
    test_media_preview.cpp
    ../src/capture/capture_provider.cpp
    ../src/app/trim_workflow.cpp
    ../src/app/session_transitions.cpp
    ../src/media/preview_process.cpp
    ../src/media/preview_video_endpoint.cpp
    ../src/media/preview_transport.cpp
    ../src/media/media_foundation_writer.cpp
    ../src/platform/platform_utils.cpp)
add_dependencies(test_media_preview EasnapPreviewHost)
target_include_directories(test_media_preview PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_media_preview PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs dwmapi
    mf mfplat mfuuid mfreadwrite mfplay
    shlwapi comctl32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32 propsys)
add_test(NAME mfplay_preview_transport COMMAND test_media_preview --transport)
set_tests_properties(mfplay_preview_transport PROPERTIES
    LABELS "integration;media-foundation;trim;preview" TIMEOUT 240)
# Seed 0 also covers reopen and delayed-completion recovery and runs every
# time. Seeds 1 and 2 execute the same code with other random delays, and the
# 30-second seek runs only add resource-growth sample depth: both are soak.
foreach(seed RANGE 0 2)
    add_test(NAME mfplay_preview_stress_${seed} COMMAND test_media_preview --stress-seed ${seed})
    set_tests_properties(mfplay_preview_stress_${seed} PROPERTIES
        LABELS "integration;media-foundation;trim;preview;stress" TIMEOUT 180 PROCESSORS 2)
    add_test(NAME mfplay_preview_continuous_seek_soak_${seed}
             COMMAND test_media_preview --continuous-seek ${seed} 30)
    set_tests_properties(mfplay_preview_continuous_seek_soak_${seed} PROPERTIES
        LABELS "integration;media-foundation;trim;preview;stress;soak" TIMEOUT 120 PROCESSORS 2)
endforeach()
set_property(TEST mfplay_preview_stress_1 mfplay_preview_stress_2 APPEND PROPERTY LABELS soak)
add_test(NAME mfplay_preview_continuous_seek COMMAND test_media_preview --continuous-seek 0 15)
set_tests_properties(mfplay_preview_continuous_seek PROPERTIES
    LABELS "integration;media-foundation;trim;preview;stress" TIMEOUT 90 PROCESSORS 2)
add_test(NAME mfplay_preview_startup COMMAND test_media_preview --startup)
set_tests_properties(mfplay_preview_startup PROPERTIES
    LABELS "integration;media-foundation;trim;preview" TIMEOUT 60)
add_test(NAME mfplay_preview_audio_transport
         COMMAND test_media_preview --focus-audio 0x13579bdf 8)
set_tests_properties(mfplay_preview_audio_transport PROPERTIES
    LABELS "integration;media-foundation;trim;preview" TIMEOUT 180)

add_executable(test_preview_backpressure
    test_preview_backpressure.cpp
    ../src/media/preview_process.cpp
    ../src/media/preview_video_endpoint.cpp
    ../src/media/preview_transport.cpp
    ../src/media/media_foundation_writer.cpp
    ../src/platform/platform_utils.cpp)
add_dependencies(test_preview_backpressure EasnapPreviewHost)
target_include_directories(test_preview_backpressure PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_preview_backpressure PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    mf mfplat mfuuid mfreadwrite mfplay
    shlwapi comctl32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32 propsys)
add_test(NAME mfplay_preview_backpressure
    COMMAND test_preview_backpressure "${CMAKE_BINARY_DIR}/preview-backpressure-test")
set_tests_properties(mfplay_preview_backpressure PROPERTIES
    LABELS "integration;media-foundation;trim;preview" TIMEOUT 60)

add_executable(test_recording_session_integration
    test_recording_session_integration.cpp
    ../src/app/recording_completion.cpp
    ../src/recording/recording_session.cpp
    ../src/recording/audio_timeline.cpp
    ../src/capture/capture_provider.cpp
    ../src/capture/recording_capture_worker.cpp
    ../src/media/gif_encoder.cpp
    ../src/media/gif_exporter.cpp
    ../src/media/preview_process.cpp
    ../src/media/preview_transport.cpp
    ../src/media/media_foundation_writer.cpp
    ../src/media/audio_resampler.cpp
    ../src/media/dual_audio_mixer.cpp
    ../src/media/dual_audio_provider.cpp
    ../src/media/wasapi_audio_provider.cpp
    ../src/platform/platform_utils.cpp)
add_dependencies(test_recording_session_integration EasnapPreviewHost)

target_include_directories(test_recording_session_integration PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_recording_session_integration PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    mf mfplat mfuuid mfreadwrite mfplay wmcodecdspuuid
    shlwapi comctl32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32 propsys)

add_test(NAME real_desktop_recording_session
         COMMAND test_recording_session_integration)
set_tests_properties(real_desktop_recording_session PROPERTIES
    LABELS "integration;real-desktop-capture;recording;gif"
    TIMEOUT 90)

add_test(NAME recording_pause_stop_stress
         COMMAND test_recording_session_integration --stress)
set_tests_properties(recording_pause_stop_stress PROPERTIES
    LABELS "integration;real-desktop-capture;recording;audio;stress"
    RUN_SERIAL TRUE
    TIMEOUT 120
    SKIP_RETURN_CODE 77)

# ---------------------------------------------------------------------------
# test_region_selector — RegionSelector interaction logic (ADR-002 coverage)
#
# Tests CanCommit, drag, nudge, hit-test, minimum size, bounds clamping and
# Reset semantics. RenderGdi is never called so no display or GDI context
# is needed at runtime.
# ---------------------------------------------------------------------------

add_executable(test_region_selector
    test_region_selector.cpp
    ../src/ui/region_selector.cpp
    ../src/ui/ui_theme.cpp
    ../src/ui/native_controls.cpp
    ../src/platform/platform_utils.cpp)

target_include_directories(test_region_selector PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")

target_link_libraries(test_region_selector PRIVATE easnap_project_options easnap_strings
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    mfplat mfuuid mfreadwrite
    shlwapi comctl32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32 propsys)

add_test(NAME region_selector_logic
         COMMAND test_region_selector)
set_tests_properties(region_selector_logic PROPERTIES
    LABELS "unit;selection"
    TIMEOUT 30)

# Incremental redaction must equal a rebuild from the capture, pixel for pixel.
add_executable(test_redaction_renderer
    test_redaction_renderer.cpp
    ../src/editor/redaction_renderer.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_redaction_renderer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_redaction_renderer PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    shlwapi comctl32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32 propsys)
add_test(NAME redaction_renderer_logic COMMAND test_redaction_renderer)
# Every edit is compared with a full rebuild. Optimized that takes seconds; an
# unoptimized Debug build on the hosted runner needs about 24 s.
set_tests_properties(redaction_renderer_logic PROPERTIES LABELS "unit;editor" TIMEOUT 120)

# The stitching engine has no window, thread or capture dependency: synthetic
# documents cut along scripted scroll trajectories must come back exactly.
add_executable(test_scrollshot_stitch
    test_scrollshot_stitch.cpp
    ../src/scrollshot/stitch_engine.cpp)
target_include_directories(test_scrollshot_stitch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_scrollshot_stitch PRIVATE easnap_project_options)
add_test(NAME scrollshot_stitch_logic COMMAND test_scrollshot_stitch)
set_tests_properties(scrollshot_stitch_logic PROPERTIES LABELS "unit;scrollshot" TIMEOUT 240)

# The capture session's worker, preview, notifications and stop barrier against
# a scripted provider: no desktop is read and no window is shown.
add_executable(test_scrollshot_session
    test_scrollshot_session.cpp
    ../src/scrollshot/scrollshot_session.cpp
    ../src/scrollshot/stitch_engine.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_scrollshot_session PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_scrollshot_session PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid)
add_test(NAME scrollshot_session_worker COMMAND test_scrollshot_session)
set_tests_properties(scrollshot_session_worker PROPERTIES LABELS "unit;scrollshot" TIMEOUT 60)

# Where the Scrollshot island and preview go around a frame, and the scrolled,
# zoomed view the editor maps a stitched image through. Rectangles only.
add_executable(test_scrollshot_layout
    test_scrollshot_layout.cpp
    ../src/ui/scrollshot_layout.cpp
    ../src/editor/image_viewport.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_scrollshot_layout PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_scrollshot_layout PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid)
add_test(NAME scrollshot_layout_logic COMMAND test_scrollshot_layout)
set_tests_properties(scrollshot_layout_logic PROPERTIES LABELS "unit;scrollshot;editor" TIMEOUT 30)

# Where the tools island, the recording panel and the mode island go around
# the frame: pure geometry over the production functions.
add_executable(test_island_layout
    test_island_layout.cpp
    ../src/ui/island_layout.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_island_layout PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_island_layout PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid)
add_test(NAME island_layout_logic COMMAND test_island_layout)
set_tests_properties(island_layout_logic PROPERTIES LABELS "unit;ui;layout" TIMEOUT 30)

# The log file through the production sink, and crash reports from a second
# process that installs the production handler and fails on purpose.
add_executable(test_diagnostics
    test_diagnostics.cpp
    ../src/platform/diagnostics.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_diagnostics PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_diagnostics PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid)
add_test(NAME diagnostics_log_and_crash_reports COMMAND test_diagnostics)
set_tests_properties(diagnostics_log_and_crash_reports PROPERTIES LABELS "unit;diagnostics" TIMEOUT 120)

# The "start with Windows" entry, written to a registry key the test owns.
add_executable(test_startup_entry
    test_startup_entry.cpp
    ../src/platform/startup_entry.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_startup_entry PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_startup_entry PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid)
add_test(NAME startup_entry_registry COMMAND test_startup_entry)
set_tests_properties(startup_entry_registry PROPERTIES LABELS "unit;settings" TIMEOUT 30)

# The import tables of the built Easnap.exe: Media Foundation must not be
# needed to start, or Windows N editions cannot run Easnap at all.
add_executable(test_executable_imports test_executable_imports.cpp)
target_link_libraries(test_executable_imports PRIVATE easnap_project_options)
add_dependencies(test_executable_imports easnap-desktop)
add_test(NAME executable_imports COMMAND test_executable_imports $<TARGET_FILE:easnap-desktop>)
set_tests_properties(executable_imports PROPERTIES LABELS "unit;compatibility" TIMEOUT 30)

# Both string tables, read from this executable's resources through the
# production accessor.
add_executable(test_strings test_strings.cpp)
target_link_libraries(test_strings PRIVATE easnap_project_options easnap_strings user32)
add_test(NAME strings_catalog COMMAND test_strings)
set_tests_properties(strings_catalog PROPERTIES LABELS "unit;localization" TIMEOUT 30)

add_executable(test_capture_overlay
    test_capture_overlay.cpp
    ../src/ui/overlay.cpp
    ../src/ui/island_layout.cpp
    ../src/ui/capture_magnifier.cpp
    ../src/ui/capture_workspace.cpp
    ../src/ui/region_selector.cpp
    ../src/ui/ui_theme.cpp
    ../src/ui/native_controls.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_capture_overlay PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_capture_overlay PRIVATE easnap_project_options easnap_strings
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid imm32)
add_test(NAME capture_magnifier_logic COMMAND test_capture_overlay)
set_tests_properties(capture_magnifier_logic PROPERTIES LABELS "unit;overlay" TIMEOUT 30)
# Native dispatch regression: synthetic snapshot, real production windows; no
# backend capture or OS clipboard writes. Requires an interactive desktop.
add_test(NAME capture_overlay_interaction
    COMMAND easnap_isolated_desktop $<TARGET_FILE:test_capture_overlay> --native)
set_tests_properties(capture_overlay_interaction PROPERTIES
    LABELS "integration;overlay;isolated-desktop" TIMEOUT 30)

add_executable(test_capture_workspace
    test_capture_workspace.cpp
    ../src/ui/capture_workspace.cpp
    ../src/ui/overlay.cpp
    ../src/ui/island_layout.cpp
    ../src/ui/capture_magnifier.cpp
    ../src/ui/region_selector.cpp
    ../src/ui/recording_controls.cpp
    ../src/ui/scrollshot_controls.cpp
    ../src/ui/scrollshot_layout.cpp
    ../src/ui/ui_theme.cpp
    ../src/ui/native_controls.cpp
    ../src/editor/screenshot_editor.cpp
    ../src/editor/image_viewport.cpp
    ../src/editor/redaction_renderer.cpp
    ../src/app/recording_workflow.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_capture_workspace PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_capture_workspace PRIVATE easnap_project_options easnap_strings
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid)
add_test(NAME capture_workspace_production
    COMMAND easnap_isolated_desktop $<TARGET_FILE:test_capture_workspace>)
set_tests_properties(capture_workspace_production PROPERTIES
    LABELS "integration;workspace;clipboard;editor;isolated-desktop" TIMEOUT 60)

add_test(NAME capture_frame_interaction
    COMMAND easnap_isolated_desktop $<TARGET_FILE:test_capture_workspace> --frame-performance)
set_tests_properties(capture_frame_interaction PROPERTIES
    LABELS "integration;workspace;overlay;performance;isolated-desktop" TIMEOUT 30)

# The mode island, the tools island and the recording panel as production
# windows: never over each other, and the mode island hidden while the frame
# is dragged.
add_test(NAME capture_island_placement
    COMMAND easnap_isolated_desktop $<TARGET_FILE:test_capture_workspace> --islands)
set_tests_properties(capture_island_placement PROPERTIES
    LABELS "integration;workspace;overlay;layout;isolated-desktop" TIMEOUT 30)

# Scrollshot in the production workspace: mode bar, locked frame and its open
# input plane, both islands, the preview, and the editor on a stitched image
# that scrolls and zooms. Message-driven; nothing is captured from a screen.
add_executable(test_scrollshot_workspace
    test_scrollshot_workspace.cpp
    ../src/ui/capture_workspace.cpp
    ../src/ui/overlay.cpp
    ../src/ui/island_layout.cpp
    ../src/ui/capture_magnifier.cpp
    ../src/ui/region_selector.cpp
    ../src/ui/scrollshot_controls.cpp
    ../src/ui/scrollshot_layout.cpp
    ../src/ui/ui_theme.cpp
    ../src/ui/native_controls.cpp
    ../src/editor/screenshot_editor.cpp
    ../src/editor/image_viewport.cpp
    ../src/editor/redaction_renderer.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_scrollshot_workspace PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_scrollshot_workspace PRIVATE easnap_project_options easnap_strings
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid imm32)
add_test(NAME scrollshot_workspace_production
    COMMAND easnap_isolated_desktop $<TARGET_FILE:test_scrollshot_workspace>)
set_tests_properties(scrollshot_workspace_production PROPERTIES
    LABELS "integration;scrollshot;workspace;editor;isolated-desktop" TIMEOUT 90)

# Every caption and label fits its control in English and in Turkish.
add_executable(test_text_fit
    test_text_fit.cpp
    ../src/ui/capture_workspace.cpp
    ../src/ui/overlay.cpp
    ../src/ui/island_layout.cpp
    ../src/ui/capture_magnifier.cpp
    ../src/ui/region_selector.cpp
    ../src/ui/recording_controls.cpp
    ../src/ui/scrollshot_controls.cpp
    ../src/ui/scrollshot_layout.cpp
    ../src/ui/ui_theme.cpp
    ../src/ui/native_controls.cpp
    ../src/editor/screenshot_editor.cpp
    ../src/editor/image_viewport.cpp
    ../src/editor/redaction_renderer.cpp
    ../src/app/recording_workflow.cpp
    ../src/ui/settings_dialog.cpp
    ../src/app/app_settings.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_text_fit PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_text_fit PRIVATE easnap_project_options easnap_strings
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid imm32)
add_test(NAME text_fits_in_every_language
    COMMAND easnap_isolated_desktop $<TARGET_FILE:test_text_fit>)
set_tests_properties(text_fits_in_every_language PROPERTIES
    LABELS "integration;ui;localization;isolated-desktop" TIMEOUT 60)

# Questions asked before work is lost: the dialog with named buttons, the
# tracking of nested dialog loops and the editor's discard confirmation.
add_executable(test_data_safety
    test_data_safety.cpp
    ../src/ui/capture_workspace.cpp
    ../src/ui/overlay.cpp
    ../src/ui/island_layout.cpp
    ../src/ui/capture_magnifier.cpp
    ../src/ui/region_selector.cpp
    ../src/ui/ui_theme.cpp
    ../src/ui/native_controls.cpp
    ../src/editor/screenshot_editor.cpp
    ../src/editor/image_viewport.cpp
    ../src/editor/redaction_renderer.cpp
    ../src/ui/settings_dialog.cpp
    ../src/app/app_settings.cpp
    ../src/app/recording_workflow.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_data_safety PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_data_safety PRIVATE easnap_project_options easnap_strings
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid imm32)
add_test(NAME data_safety_questions
    COMMAND easnap_isolated_desktop $<TARGET_FILE:test_data_safety>)
set_tests_properties(data_safety_questions PROPERTIES
    LABELS "integration;ui;editor;isolated-desktop" TIMEOUT 60)

# Scrollshot against the real desktop: a second process shows a known document,
# real wheel and keyboard input scrolls it through the open frame, and the
# production session and provider must return it pixel for pixel.
add_executable(test_scrollshot_desktop
    test_scrollshot_desktop.cpp
    ../src/capture/capture_provider.cpp
    ../src/scrollshot/scrollshot_session.cpp
    ../src/scrollshot/stitch_engine.cpp
    ../src/ui/capture_workspace.cpp
    ../src/ui/overlay.cpp
    ../src/ui/island_layout.cpp
    ../src/ui/capture_magnifier.cpp
    ../src/ui/region_selector.cpp
    ../src/ui/scrollshot_controls.cpp
    ../src/ui/scrollshot_layout.cpp
    ../src/ui/ui_theme.cpp
    ../src/ui/native_controls.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_scrollshot_desktop PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_scrollshot_desktop PRIVATE easnap_project_options easnap_strings
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid imm32)
add_test(NAME scrollshot_real_desktop COMMAND test_scrollshot_desktop)
set_tests_properties(scrollshot_real_desktop PROPERTIES
    LABELS "integration;scrollshot;real-desktop-capture" TIMEOUT 90 SKIP_RETURN_CODE 77)

add_executable(test_ui_modernization
    test_ui_modernization.cpp
    ../src/capture/capture_provider.cpp
    ../src/ui/capture_workspace.cpp
    ../src/ui/overlay.cpp
    ../src/ui/island_layout.cpp
    ../src/ui/capture_magnifier.cpp
    ../src/ui/region_selector.cpp
    ../src/app/app_settings.cpp
    ../src/app/recording_workflow.cpp
    ../src/app/trim_workflow.cpp
    ../src/app/session_transitions.cpp
    ../src/ui/ui_theme.cpp
    ../src/ui/native_controls.cpp
    ../src/ui/recording_controls.cpp
    ../src/ui/trim_window.cpp
    ../src/ui/trim_layout.cpp
    ../src/ui/timeline_accessibility.cpp
    ../src/editor/screenshot_editor.cpp
    ../src/editor/image_viewport.cpp
    ../src/editor/redaction_renderer.cpp
    ../src/media/preview_process.cpp
    ../src/media/preview_transport.cpp
    ../src/media/media_foundation_writer.cpp
    ../src/platform/platform_utils.cpp)
add_dependencies(test_ui_modernization EasnapPreviewHost)
target_link_libraries(test_ui_modernization PRIVATE windowsapp easnap_strings)
target_include_directories(test_ui_modernization PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_ui_modernization PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs mf mfplat mfuuid mfreadwrite mfplay
    shlwapi comctl32 shell32 ole32 oleaut32 uuid user32 gdi32 advapi32 propsys UIAutomationCore)
add_test(NAME ui_modernization_native COMMAND test_ui_modernization)
add_test(NAME trim_endpoint_presentation COMMAND test_ui_modernization --trim-endpoint)
set_tests_properties(trim_endpoint_presentation PROPERTIES
    LABELS "integration;ui;preview;desktop-visual" TIMEOUT 90)
# 24 real clicks per case keep every gate (frame, motion and freeze thresholds)
# reachable in about a third of the time; the 72-click run is the soak variant.
add_test(NAME trim_timeline_seek_stress
         COMMAND test_ui_modernization --seek-stress
                 "${CMAKE_CURRENT_BINARY_DIR}/seek-stress-$<CONFIG>" 24)
add_test(NAME trim_timeline_seek_soak
         COMMAND test_ui_modernization --seek-stress
                 "${CMAKE_CURRENT_BINARY_DIR}/seek-soak-$<CONFIG>" 72)
set_tests_properties(trim_timeline_seek_stress trim_timeline_seek_soak PROPERTIES
    LABELS "integration;ui;preview;desktop-visual;stress" TIMEOUT 300
    # The long interactive observer must not depend on WGC's asynchronous DLL
    # teardown. DXGI observes the same composed desktop; pixel gates are shared.
    ENVIRONMENT "EASNAP_TEST_CAPTURE=duplication")
set_property(TEST trim_timeline_seek_soak APPEND PROPERTY LABELS soak)
add_test(NAME ui_material_raster_parity COMMAND test_ui_modernization --material-rasters)
set_tests_properties(ui_material_raster_parity PROPERTIES LABELS "unit;ui;theme" TIMEOUT 30)
set_tests_properties(ui_modernization_native PROPERTIES
    LABELS "integration;ui;accessibility;theme;preview" TIMEOUT 90)

foreach(ui_target test_region_selector test_redaction_renderer test_capture_overlay test_capture_workspace
                  test_scrollshot_workspace test_scrollshot_desktop test_data_safety test_text_fit
                  test_ui_modernization)
    target_link_libraries(${ui_target} PRIVATE uxtheme gdiplus oleacc dwmapi)
endforeach()

# ---------------------------------------------------------------------------
# test_gif_encoder — Phase 3.1 stream, timing, palette, delta and publication
# ---------------------------------------------------------------------------

add_executable(test_gif_encoder
    test_gif_encoder.cpp
    ../src/media/gif_encoder.cpp
    ../src/media/gif_exporter.cpp
    ../src/media/media_foundation_writer.cpp
    ../src/platform/platform_utils.cpp)

target_include_directories(test_gif_encoder PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_gif_encoder PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    mf mfplat mfuuid mfreadwrite
    shlwapi comctl32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32 propsys)

add_test(NAME gif_backend
         COMMAND test_gif_encoder)
set_tests_properties(gif_backend PROPERTIES
    LABELS "integration;media-foundation;gif"
    TIMEOUT 120)

# ---------------------------------------------------------------------------
# test_wasapi_audio — real WASAPI audio capture validation
#
# Exercises the production WASAPI provider against real Windows audio
# endpoints. System-loopback test generates a real sine wave through a WASAPI
# render client as TEST INPUT while the production loopback capture acquires
# it. Microphone test probes real hardware and reports unavailability
# accurately rather than faking success.
# ---------------------------------------------------------------------------

add_executable(test_wasapi_audio
    test_wasapi_audio.cpp
    ../src/media/audio_resampler.cpp
    ../src/media/dual_audio_mixer.cpp
    ../src/media/dual_audio_provider.cpp
    ../src/media/wasapi_audio_provider.cpp
    ../src/capture/recording_capture_worker.cpp
    ../src/platform/platform_utils.cpp)

target_include_directories(test_wasapi_audio PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_wasapi_audio PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    mfplat mfuuid mfreadwrite wmcodecdspuuid
    shlwapi comctl32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32 propsys)

add_test(NAME wasapi_audio_capture
         COMMAND test_wasapi_audio)
# Exits with 77 where the machine has no audio endpoint at all: every capture
# test skipped itself, and that is not a pass.
set_tests_properties(wasapi_audio_capture PROPERTIES
    LABELS "integration;audio;wasapi"
    SKIP_RETURN_CODE 77
    TIMEOUT 60)

add_test(NAME annotation_editing_native COMMAND test_ui_modernization --editor-checks)
set_tests_properties(annotation_editing_native PROPERTIES LABELS "integration;editor;ui" TIMEOUT 60)

# These assertions use the physical desktop, pointer, focus or capture timing.
# Keep their full coverage, but never run them concurrently with another test.
# The three capture_* tests left this list: they are message-driven and run on
# a private desktop instead.
set(interactive_desktop_tests
    real_desktop_recording_session recording_pause_stop_stress scrollshot_real_desktop
    ui_modernization_native annotation_editing_native trim_endpoint_presentation
    trim_timeline_seek_stress trim_timeline_seek_soak)
set_tests_properties(${interactive_desktop_tests} PROPERTIES RUN_SERIAL TRUE)
set_property(TEST ${interactive_desktop_tests} APPEND PROPERTY LABELS interactive-desktop)
set_tests_properties(mfplay_preview_startup PROPERTIES RUN_SERIAL TRUE)
set_property(TEST mfplay_preview_startup APPEND PROPERTY LABELS desktop-visual)
set_tests_properties(mfplay_preview_audio_transport wasapi_audio_capture PROPERTIES RESOURCE_LOCK easnap_audio)

add_executable(test_storage_cleanup
    test_storage_cleanup.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_storage_cleanup PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_storage_cleanup PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi windowscodecs
    user32 gdi32 shlwapi advapi32 comctl32 shell32 ole32 uuid)
add_test(NAME storage_cleanup_containment COMMAND test_storage_cleanup)
set_tests_properties(storage_cleanup_containment PROPERTIES
    LABELS "unit;storage;security" TIMEOUT 30)

add_test(NAME incremental_test_selection
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${PROJECT_SOURCE_DIR}/scripts/test-test-selection.ps1")
set_tests_properties(incremental_test_selection PROPERTIES LABELS "unit;tooling" TIMEOUT 30)
include(runtime-inputs.cmake)
