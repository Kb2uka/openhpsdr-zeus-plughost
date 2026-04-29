# deps.cmake — third-party dependency wiring.
#
# Phase 2 (current):
#   We bring in the Steinberg VST 3 SDK as a git submodule under
#   third_party/vst3sdk/. The SDK exposes three static-library targets we
#   actually want to link (sdk_hosting, base, pluginterfaces); everything
#   else (examples, validator, VSTGUI, plugin-link helpers) is force-OFF
#   here BEFORE add_subdirectory() so the SDK does not produce surplus
#   artefacts in our build tree.
#
# Phase 3+ (TODO):
#   - clap : https://github.com/free-audio/clap          (MIT)
#   - vestige : VST2 header (LGPL) under third_party/vestige/
#
# Pinned version + commit SHA: see third_party/vst3sdk_PINNED_VERSION.md.

# ---- VST 3 SDK -------------------------------------------------------------

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT        OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR            OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK           OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_WAYLAND_SUPPORT       OFF CACHE BOOL "" FORCE)

set(_zeus_vst3sdk_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk")
if(NOT EXISTS "${_zeus_vst3sdk_dir}/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/vst3sdk is missing or unpopulated. Run:\n"
        "  git submodule update --init --recursive\n"
        "from the plughost repository root.")
endif()

# The SDK builds a few utility / wrapper subprojects unconditionally even
# with the example flags off; that is fine — the only ones we link below
# are the three canonical targets (sdk_hosting, base, pluginterfaces).
add_subdirectory(${_zeus_vst3sdk_dir} ${CMAKE_BINARY_DIR}/_vst3sdk EXCLUDE_FROM_ALL)

# Expose the platform-specific module loader source path. sdk_hosting
# contains the platform-agnostic Module facade (module.cpp) but does NOT
# include module_linux.cpp / module_mac.mm / module_win32.cpp — examples
# pick the right one per platform. We link our own selection in via
# zeus-plughost target sources.
if(WIN32)
    set(ZEUS_PLUGHOST_VST3_MODULE_IMPL
        "${_zeus_vst3sdk_dir}/public.sdk/source/vst/hosting/module_win32.cpp"
        CACHE INTERNAL "")
elseif(APPLE)
    set(ZEUS_PLUGHOST_VST3_MODULE_IMPL
        "${_zeus_vst3sdk_dir}/public.sdk/source/vst/hosting/module_mac.mm"
        CACHE INTERNAL "")
else()
    set(ZEUS_PLUGHOST_VST3_MODULE_IMPL
        "${_zeus_vst3sdk_dir}/public.sdk/source/vst/hosting/module_linux.cpp"
        CACHE INTERNAL "")
endif()
