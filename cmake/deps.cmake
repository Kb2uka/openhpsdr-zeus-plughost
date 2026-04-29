# deps.cmake — third-party dependency stubs.
#
# Phase 1 (current):
#   We are NOT loading any plugins yet. The data-plane skeleton is a pure
#   pass-through smoke test. No vendor SDKs are required to build.
#
# Phase 2 (TODO):
#   Re-enable the FetchContent_Declare blocks below and check the resulting
#   trees into third_party/ as submodules.
#
#     - vst3sdk : https://github.com/steinbergmedia/vst3sdk         (MIT)
#     - clap    : https://github.com/free-audio/clap                (MIT)
#     - vestige : VST2 header (LGPL) — kept under third_party/vestige/
#     - tinycbor: https://github.com/intel/tinycbor                 (MIT)
#                 for control-plane CBOR framing.

include(FetchContent)

option(ZEUS_PLUGHOST_FETCH_DEPS
    "Fetch vst3sdk / CLAP / vestige via FetchContent (Phase 2 only)" OFF)

if(ZEUS_PLUGHOST_FETCH_DEPS)
    message(WARNING
        "ZEUS_PLUGHOST_FETCH_DEPS is ON, but Phase 1 has no plugin loading. "
        "Ignoring; flip this back ON in Phase 2.")
endif()

# ---- Phase 2 stubs (commented; do not enable in Phase 1) -------------------
#
# FetchContent_Declare(vst3sdk
#     GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
#     GIT_TAG        v3.7.x        # pin once Phase 2 begins
# )
#
# FetchContent_Declare(clap
#     GIT_REPOSITORY https://github.com/free-audio/clap.git
#     GIT_TAG        1.2.x         # pin once Phase 2 begins
# )
#
# # vestige is single-header — vendor it under third_party/vestige/aeffect.h.
# # Do not fetch via git; capture the LGPL header verbatim with origin notes.
#
# FetchContent_Declare(tinycbor
#     GIT_REPOSITORY https://github.com/intel/tinycbor.git
#     GIT_TAG        v0.6.x        # pin once Phase 2 begins
# )
#
# Once Phase 2 is live, gate the calls behind ZEUS_PLUGHOST_FETCH_DEPS:
#   if(ZEUS_PLUGHOST_FETCH_DEPS)
#       FetchContent_MakeAvailable(vst3sdk clap tinycbor)
#   endif()
