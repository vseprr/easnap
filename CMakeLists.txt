cmake_minimum_required(VERSION 3.20)

project(Easnap
    VERSION 1.0.2
    DESCRIPTION "Easnap - Windows 10 screen capture and recording tool"
    HOMEPAGE_URL "https://github.com/vseprr/easnap"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global build configuration
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR
        "Easnap v1 targets Windows 10 (and newer) only. "
        "Configure this project with a Windows toolchain (MSVC 2019/2022 recommended).")
endif()

# Windows 10 baseline (0x0A00). Feature availability is probed at runtime instead
# of branching on OS versions, see docs/architecture.md.
set(EASNAP_WINDOWS_BASELINE 0x0A00 CACHE STRING "Minimum supported Windows version (_WIN32_WINNT)")

option(EASNAP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for project code" ON)
option(EASNAP_BUILD_TESTS "Build unit and native integration test targets" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Static CRT keeps the portable build free of redistributable dependencies.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# Shared compile options / definitions for all Easnap targets
# ---------------------------------------------------------------------------
add_library(easnap_project_options INTERFACE)
target_compile_features(easnap_project_options INTERFACE cxx_std_20)
target_compile_definitions(easnap_project_options INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    STRICT
    _WIN32_WINNT=${EASNAP_WINDOWS_BASELINE}
    WINVER=${EASNAP_WINDOWS_BASELINE}
    EASNAP_VERSION_STRING="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(easnap_project_options INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /MP /EHsc /guard:cf /sdl)
    if(EASNAP_WARNINGS_AS_ERRORS)
        target_compile_options(easnap_project_options INTERFACE /WX)
    endif()
    # /DEPENDENTLOADFLAG:0x800 resolves every static import from System32 only
    # (LOAD_LIBRARY_SEARCH_SYSTEM32), so a DLL placed beside a portable copy is
    # never loaded. /CETCOMPAT opts in to hardware shadow stacks where available.
    target_link_options(easnap_project_options INTERFACE
        /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /GUARD:CF /CETCOMPAT /DEPENDENTLOADFLAG:0x800)
else()
    # MinGW / clang-cl cross builds are supported for syntax verification.
    target_compile_options(easnap_project_options INTERFACE -Wall -Wextra)
    if(EASNAP_WARNINGS_AS_ERRORS)
        target_compile_options(easnap_project_options INTERFACE -Werror)
    endif()
    if(MINGW)
        # MSVC picks wWinMain automatically for /SUBSYSTEM:WINDOWS; the MinGW
        # runtime needs -municode to select the wide entry point instead of main.
        target_compile_options(easnap_project_options INTERFACE -municode)
        target_link_options(easnap_project_options INTERFACE -municode -mwindows)
    endif()
endif()

# enable_testing() must run in the top-level scope for CTest to pick the tests up.
if(EASNAP_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(apps/easnap-desktop)

message(STATUS "Easnap ${PROJECT_VERSION} configured (baseline ${EASNAP_WINDOWS_BASELINE})")
