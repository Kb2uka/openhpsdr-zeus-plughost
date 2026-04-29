cmake_minimum_required(VERSION 3.25)

project(zeus-plughost
    VERSION 0.1.0
    DESCRIPTION "Out-of-process VST3/CLAP/VST2 host sidecar for OpenHPSDR Zeus"
    LANGUAGES CXX)

# C++17 across all platforms.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Honour the user's choice of build type; default to RelWithDebInfo for sanity.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

# Platform detection + sanity checks (32 vs 64-bit, etc).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform.cmake)

# Dependency wiring: pulls in the VST 3 SDK submodule with the SDK's own
# example / VSTGUI targets disabled. After this we have sdk_hosting / base
# / pluginterfaces targets available, plus ZEUS_PLUGHOST_VST3_MODULE_IMPL
# pointing at the per-platform Module loader source.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/deps.cmake)

# ---------------------------------------------------------------------------
# zeus-plughost executable
# ---------------------------------------------------------------------------
set(ZEUS_PLUGHOST_SOURCES
    src/main.cpp
    src/ipc/shm_ring.cpp
    src/ipc/control_pipe.cpp
    src/audio/passthrough.cpp
    src/vst3/plugin_host.cpp
    src/vst3/plugin_chain.cpp
    # Platform-specific Module loader from the VST 3 SDK. Defined by
    # cmake/deps.cmake based on the host triple; we compile it directly
    # into our binary so the SDK's example target tree stays disabled.
    ${ZEUS_PLUGHOST_VST3_MODULE_IMPL}
)

# Phase 3 GUI: native plugin editor windows. Linux only this wave —
# Windows (HWND) and macOS (NSView) come in a later wave once the
# operator moves to those platforms.
if(UNIX AND NOT APPLE)
    list(APPEND ZEUS_PLUGHOST_SOURCES
        src/vst3/host_run_loop.cpp
        src/vst3/host_plug_frame.cpp
        src/vst3/editor_window.cpp
        src/vst3/gui_thread.cpp
    )
endif()

# Per-platform wakeup primitive impl. Stubs on Win/Mac in Phase 2.
if(WIN32)
    list(APPEND ZEUS_PLUGHOST_SOURCES src/ipc/wakeup_windows.cpp)
elseif(APPLE)
    list(APPEND ZEUS_PLUGHOST_SOURCES src/ipc/wakeup_macos.cpp)
elseif(UNIX)
    list(APPEND ZEUS_PLUGHOST_SOURCES src/ipc/wakeup_linux.cpp)
else()
    message(FATAL_ERROR "Unsupported platform: zeus-plughost requires Windows, macOS, or Linux.")
endif()

add_executable(zeus-plughost ${ZEUS_PLUGHOST_SOURCES})

target_include_directories(zeus-plughost PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk
)

# Warnings-as-errors: GCC/Clang on Linux/Mac, MSVC on Windows. SDK headers
# may emit warnings — those are quarantined behind src/vst3/sdk_includes.h
# (pragma diagnostic push/ignored/pop) so we do not relax -Werror globally.
if(MSVC)
    target_compile_options(zeus-plughost PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(zeus-plughost PRIVATE
        -Wall -Wextra -Werror
        -Wno-unused-parameter
    )
endif()

# The SDK's module_linux.cpp / module_mac.mm / module_win32.cpp source
# files are not authored to our -Wall -Wextra -Werror discipline; relax
# warnings on those individual translation units only.
set_source_files_properties(${ZEUS_PLUGHOST_VST3_MODULE_IMPL} PROPERTIES
    COMPILE_FLAGS "-w")

# Apple's Objective-C++ Module loader needs ARC; matches the SDK examples.
if(APPLE)
    set_source_files_properties(${ZEUS_PLUGHOST_VST3_MODULE_IMPL} PROPERTIES
        COMPILE_FLAGS "-fobjc-arc -w")
endif()

# Link platform threading library (UNIX) — Win threads are intrinsic to MSVCRT.
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(zeus-plughost PRIVATE Threads::Threads)
endif()

# Linux-specific: rt for shm_open, dl for the Module loader's dlopen,
# X11 for the Phase 3 GUI editor windows.
if(UNIX AND NOT APPLE)
    target_link_libraries(zeus-plughost PRIVATE rt dl)
    find_package(X11 REQUIRED)
    target_link_libraries(zeus-plughost PRIVATE X11::X11)
endif()

if(APPLE)
    target_link_libraries(zeus-plughost PRIVATE "-framework CoreFoundation")
endif()

# VST 3 SDK static libraries. sdk_hosting pulls in sdk_common transitively.
target_link_libraries(zeus-plughost PRIVATE
    sdk_hosting
    base
    pluginterfaces
)

# Output binary name is identical across platforms; CMake adds .exe on Windows.
set_target_properties(zeus-plughost PROPERTIES OUTPUT_NAME "zeus-plughost")

message(STATUS "zeus-plughost: configured for ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
