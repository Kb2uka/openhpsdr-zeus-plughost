cmake_minimum_required(VERSION 3.20)

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

# Dependency stubs (FetchContent for vst3sdk / clap / vestige; disabled in Phase 1).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/deps.cmake)

# ---------------------------------------------------------------------------
# zeus-plughost executable
# ---------------------------------------------------------------------------
set(ZEUS_PLUGHOST_SOURCES
    src/main.cpp
    src/ipc/shm_ring.cpp
    src/ipc/control_pipe.cpp
    src/audio/passthrough.cpp
)

# Per-platform wakeup primitive impl. Stubs on Win/Mac in Phase 1.
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
)

# Warnings-as-errors: GCC/Clang on Linux/Mac, MSVC on Windows.
if(MSVC)
    target_compile_options(zeus-plughost PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(zeus-plughost PRIVATE
        -Wall -Wextra -Werror
        -Wno-unused-parameter
    )
endif()

# Link platform threading library (UNIX) — Win threads are intrinsic to MSVCRT.
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(zeus-plughost PRIVATE Threads::Threads)
endif()

# Linux-specific: rt for shm_open, futex via syscall in libc.
if(UNIX AND NOT APPLE)
    target_link_libraries(zeus-plughost PRIVATE rt)
endif()

# Output binary name is identical across platforms; CMake adds .exe on Windows.
set_target_properties(zeus-plughost PROPERTIES OUTPUT_NAME "zeus-plughost")

message(STATUS "zeus-plughost: configured for ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
