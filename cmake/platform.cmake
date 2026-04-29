# platform.cmake — cross-platform detection and per-OS compile defs.
#
# Sets:
#   ZEUS_PLUGHOST_OS_LINUX / ZEUS_PLUGHOST_OS_WINDOWS / ZEUS_PLUGHOST_OS_MACOS
#   ZEUS_PLUGHOST_64BIT (must be 1 — we don't support 32-bit hosts)

if(WIN32)
    set(ZEUS_PLUGHOST_OS "Windows")
    add_compile_definitions(ZEUS_PLUGHOST_OS_WINDOWS=1)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    set(ZEUS_PLUGHOST_OS "macOS")
    add_compile_definitions(ZEUS_PLUGHOST_OS_MACOS=1)
elseif(UNIX)
    set(ZEUS_PLUGHOST_OS "Linux")
    add_compile_definitions(ZEUS_PLUGHOST_OS_LINUX=1)
    add_compile_definitions(_GNU_SOURCE=1)
else()
    message(FATAL_ERROR "Unsupported platform: zeus-plughost requires Windows, macOS, or Linux.")
endif()

# Pointer-size sanity check — 32-bit hosts would not have enough address space
# for shared-memory rings + plugin sandboxes. Reject early.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_compile_definitions(ZEUS_PLUGHOST_64BIT=1)
    message(STATUS "zeus-plughost: 64-bit target detected.")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "zeus-plughost requires a 64-bit target. "
        "32-bit builds are not supported (CMAKE_SIZEOF_VOID_P=${CMAKE_SIZEOF_VOID_P}).")
else()
    message(FATAL_ERROR
        "zeus-plughost: unrecognised pointer size CMAKE_SIZEOF_VOID_P=${CMAKE_SIZEOF_VOID_P}")
endif()

message(STATUS "zeus-plughost: target OS = ${ZEUS_PLUGHOST_OS}")
