# ---------------------------------------------------------------------------
# easnap-desktop : the notification-area capture application (Win32, C++20)
# ---------------------------------------------------------------------------

configure_file(resources/easnap_version.h.in generated/easnap_version.h @ONLY)
configure_file(resources/portable-readme.txt.in generated/README.txt @ONLY)
target_include_directories(easnap_project_options INTERFACE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(EASNAP_SOURCES
    src/main.cpp
    src/app/session_transitions.h
    src/app/session_transitions.cpp
    src/app/recording_workflow.h
    src/app/recording_workflow.cpp
    src/app/recording_completion.h
    src/app/recording_completion.cpp
    src/app/recording_finalizer.h
    src/app/recording_finalizer.cpp
    src/app/trim_workflow.h
    src/app/trim_workflow.cpp
    src/app/app_settings.h
    src/app/appearance_theme.h
    src/app/app_settings.cpp
    src/app/session_state.h
    src/app/session_state.cpp
    src/ui/ui_theme.h
    src/ui/ui_theme.cpp
    src/ui/native_controls.cpp
    src/ui/native_controls.h
    src/ui/paint_buffer.h
    src/ui/overlay.h
    src/ui/overlay.cpp
    src/ui/capture_magnifier.h
    src/ui/capture_magnifier.cpp
    src/ui/capture_workspace.h
    src/ui/capture_workspace.cpp
    src/ui/recording_controls.h
    src/ui/recording_controls.cpp
    src/ui/trim_window.h
    src/ui/trim_window.cpp
    src/ui/timeline_accessibility.cpp
    src/ui/timeline_accessibility.h
    src/ui/trim_layout.h
    src/ui/trim_layout.cpp
    src/ui/region_selector.h
    src/ui/region_selector.cpp
    src/editor/screenshot_editor.h
    src/editor/screenshot_editor.cpp
    src/editor/redaction_renderer.h
    src/editor/redaction_renderer.cpp
    src/capture/capture_provider.h
    src/capture/capture_provider.cpp
    src/capture/recording_capture_worker.h
    src/capture/recording_capture_worker.cpp
    src/scrollshot/stitch_engine.h
    src/scrollshot/stitch_engine.cpp
    src/media/audio_format.h
    src/media/audio_delivery_barrier.h
    src/media/audio_resampler.h
    src/media/audio_resampler.cpp
    src/media/dual_audio_mixer.h
    src/media/dual_audio_mixer.cpp
    src/media/dual_audio_provider.cpp
    src/media/audio_queue.h
    src/media/frame_queue.h
    src/media/media_types.h
    src/media/media_interfaces.h
    src/media/media_foundation_writer.h
    src/media/media_foundation_writer.cpp
    src/media/gif_encoder.h
    src/media/gif_encoder.cpp
    src/media/gif_exporter.cpp
    src/media/mf_d3d_device.h
    src/media/mf_video_aperture.h
    src/media/mf_d3d_device.cpp
    src/media/media_preview.h
    src/media/preview_process.cpp
    src/media/preview_host_protocol.h
    src/media/preview_transport.h
    src/media/preview_transport.cpp
    src/media/mp4_trim_exporter.cpp
    src/media/temp_session.h
    src/media/wasapi_audio_provider.h
    src/media/wasapi_audio_provider.cpp
    src/recording/recording_session.h
    src/recording/recording_session.cpp
    src/recording/audio_timeline.h
    src/recording/audio_timeline.cpp
    src/platform/platform_utils.h
    src/platform/platform_utils.cpp)

set(EASNAP_RESOURCES
    resources/easnap.rc
    resources/resource.h)

add_executable(easnap-desktop WIN32 ${EASNAP_SOURCES} ${EASNAP_RESOURCES})

# A Media Foundation driver can block inside Shutdown. Keep the native player
# in an owned process so transport recovery and editor close remain bounded.
add_executable(EasnapPreviewHost WIN32
    resources/preview_host.rc
    src/media/preview_host_main.cpp
    src/media/preview_host_protocol.h
    src/media/media_preview.cpp
    src/media/mf_video_aperture.h
    src/ui/paint_buffer.h
    src/media/preview_video_endpoint.cpp
    src/media/preview_transport.cpp
    src/platform/platform_utils.cpp)
target_include_directories(EasnapPreviewHost PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources")
target_link_libraries(EasnapPreviewHost PRIVATE easnap_project_options
    d3d11 d2d1 dwrite dxgi dxguid windowscodecs
    mfplat mfuuid mfreadwrite mfplay shlwapi comctl32 shell32
    ole32 oleaut32 uuid user32 gdi32 advapi32 propsys)
# The retained still is a layered child; Windows requires an 8+-aware manifest.
# Embed the project's Windows 10 baseline in the helper that owns that HWND.
# The same manifest declares per-monitor v2 DPI awareness (see easnap.manifest).
target_sources(EasnapPreviewHost PRIVATE resources/preview_host.manifest)
add_dependencies(easnap-desktop EasnapPreviewHost)

set_target_properties(easnap-desktop PROPERTIES OUTPUT_NAME "Easnap")

target_include_directories(easnap-desktop PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources")

target_link_libraries(easnap-desktop PRIVATE easnap_project_options)

# Native graphics, media, shell, theme and accessibility dependencies.
# The supervised preview helper has its own narrower link set above.
target_link_libraries(easnap-desktop PRIVATE
    uxtheme gdiplus oleacc
    d3d11
    d2d1
    dwrite
    dxgi
    dxguid
    windowscodecs
    mfplat
    mfuuid
    wmcodecdspuuid
    mfreadwrite
    mfplay
    shlwapi
    comctl32
    shell32
    ole32
    oleaut32
    uuid
    user32
    gdi32
    advapi32
    propsys dwmapi uiautomationcore)

# Per-monitor v2 DPI awareness is declared in resources/easnap.manifest. Do not
# set VS_DPI_AWARE: it embeds per-monitor v1, which a process cannot raise
# afterwards, so the shipped binaries ran in a different mode than the tests.
# platform::EnableDpiAwareness remains for manifest-less test executables.
# Common controls v6 come from a linker manifest dependency in main.cpp.
target_sources(easnap-desktop PRIVATE resources/easnap.manifest)
if(MSVC)
    target_link_options(easnap-desktop PRIVATE
        "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
endif()

# Keep the source tree layout visible inside Visual Studio.
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${EASNAP_SOURCES} ${EASNAP_RESOURCES})

install(TARGETS easnap-desktop EasnapPreviewHost RUNTIME DESTINATION bin)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/README.txt" DESTINATION .)

if(EASNAP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
