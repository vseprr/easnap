cmake_minimum_required(VERSION 3.20)

project(Easnap
    VERSION 1.1.0
    DESCRIPTION "Easnap - Windows 10 screen capture and recording tool"
    HOMEPAGE_URL "https://github.com/vseprr/easnap"
    LANGUAGES CXX)

# A prerelease label such as "beta.1" or "rc.1"; empty for a stable release.
# With the project version it forms the version users see, the package names
# and the release tag: v1.1.0-beta.1. scripts/package-release.ps1 and the
# Release workflow read this line.
set(EASNAP_VERSION_PRERELEASE "beta.1")
if(EASNAP_VERSION_PRERELEASE)
    set(EASNAP_VERSION_TEXT "${PROJECT_VERSION}-${EASNAP_VERSION_PRERELEASE}")
    set(EASNAP_VERSION_IS_PRERELEASE 1)
else()
    set(EASNAP_VERSION_TEXT "${PROJECT_VERSION}")
    set(EASNAP_VERSION_IS_PRERELEASE 0)
endif()

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

# Windows 10 baseline (0x0A00). Features are probed at runtime where possible
# (capture exclusion, Media Foundation, DPI entry points). The application
# refuses to start below build 19041; see ADR-033 and ADR-034.
set(EASNAP_WINDOWS_BASELINE 0x0A00 CACHE STRING "Minimum supported Windows version (_WIN32_WINNT)")

option(EASNAP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for project code" ON)
option(EASNAP_BUILD_TESTS "Build unit and native integration test targets" OFF)
# For test builds only. The binaries need the sanitizer runtime of the compiler
# beside them or on PATH and are never packaged.
option(EASNAP_SANITIZE_ADDRESS "Build with AddressSanitizer (MSVC; for running tests)" OFF)

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
    if(EASNAP_SANITIZE_ADDRESS)
        # The sanitizer runtime is a DLL outside System32 and rejects /RTC1,
        # incremental linking and shadow stacks.
        target_compile_options(easnap_project_options INTERFACE /fsanitize=address /Zi /bigobj)
        string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        target_link_options(easnap_project_options INTERFACE
            /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /INCREMENTAL:NO /DEBUG)
    else()
        # /DEPENDENTLOADFLAG:0x800 resolves every static import from System32 only
        # (LOAD_LIBRARY_SEARCH_SYSTEM32), so a DLL placed beside a portable copy is
        # never loaded. /CETCOMPAT opts in to hardware shadow stacks where available.
        target_link_options(easnap_project_options INTERFACE
            /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /GUARD:CF /CETCOMPAT /DEPENDENTLOADFLAG:0x800)
    endif()
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

# A crash report is a minidump; reading it needs the symbol file of exactly
# that build. The shipped executables therefore get one in Release as well.
# Optimization is unchanged, and only the file name of the symbol file is
# recorded in the executable, not the build directory. `output_name` is the
# name of the executable without its extension.
function(easnap_keep_release_symbols target output_name)
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:/Zi>")
        target_link_options(${target} PRIVATE
            "$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF;/INCREMENTAL:NO;/PDBALTPATH:${output_name}.pdb>")
    endif()
endfunction()

# enable_testing() must run in the top-level scope for CTest to pick the tests up.
if(EASNAP_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(apps/easnap-desktop)

message(STATUS "Easnap ${PROJECT_VERSION} configured (baseline ${EASNAP_WINDOWS_BASELINE})")
