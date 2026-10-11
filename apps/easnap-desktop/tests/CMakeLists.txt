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
    TIMEOUT 120)

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

target_link_libraries(test_region_selector PRIVATE easnap_project_options
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

add_executable(test_capture_overlay
    test_capture_overlay.cpp
    ../src/ui/overlay.cpp
    ../src/ui/capture_magnifier.cpp
    ../src/ui/capture_workspace.cpp
    ../src/ui/region_selector.cpp
    ../src/ui/ui_theme.cpp
    ../src/ui/native_controls.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_capture_overlay PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_capture_overlay PRIVATE easnap_project_options
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
    ../src/ui/capture_magnifier.cpp
    ../src/ui/region_selector.cpp
    ../src/ui/recording_controls.cpp
    ../src/ui/ui_theme.cpp
    ../src/ui/native_controls.cpp
    ../src/editor/screenshot_editor.cpp
    ../src/editor/redaction_renderer.cpp
    ../src/app/recording_workflow.cpp
    ../src/platform/platform_utils.cpp)
target_include_directories(test_capture_workspace PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_SOURCE_DIR}/../resources")
target_link_libraries(test_capture_workspace PRIVATE easnap_project_options
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

add_executable(test_ui_modernization
    test_ui_modernization.cpp
    ../src/capture/capture_provider.cpp
    ../src/ui/capture_workspace.cpp
    ../src/ui/overlay.cpp
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
    ../src/editor/redaction_renderer.cpp
    ../src/media/preview_process.cpp
    ../src/media/preview_transport.cpp
    ../src/media/media_foundation_writer.cpp
    ../src/platform/platform_utils.cpp)
add_dependencies(test_ui_modernization EasnapPreviewHost)
target_link_libraries(test_ui_modernization PRIVATE windowsapp)
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

foreach(ui_target test_region_selector test_redaction_renderer test_capture_overlay test_capture_workspace test_ui_modernization)
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
set_tests_properties(wasapi_audio_capture PROPERTIES
    LABELS "integration;audio;wasapi"
    TIMEOUT 60)

add_test(NAME annotation_editing_native COMMAND test_ui_modernization --editor-checks)
set_tests_properties(annotation_editing_native PROPERTIES LABELS "integration;editor;ui" TIMEOUT 60)

# These assertions use the physical desktop, pointer, focus or capture timing.
# Keep their full coverage, but never run them concurrently with another test.
# The three capture_* tests left this list: they are message-driven and run on
# a private desktop instead.
set(interactive_desktop_tests
    real_desktop_recording_session recording_pause_stop_stress
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
