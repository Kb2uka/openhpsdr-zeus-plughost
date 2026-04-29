# vst3sdk pinned version

This file records which Steinberg VST 3 SDK release the
`third_party/vst3sdk/` submodule is checked out at, and why. Update both
the tag and the commit SHA when bumping the pin so the rationale survives.

## Pin

- Repository  : https://github.com/steinbergmedia/vst3sdk
- Tag         : `v3.8.0_build_66`
- Commit SHA  : `9fad9770f2ae8542ab1a548a68c1ad1ac690abe0`
- License     : MIT (since October 2025) — compatible with Zeus GPL-2.0-or-later
- Pinned on   : 2026-04-29

## Rationale

`v3.8.0_build_66` is the most recent tagged release at the time of the
Phase 2 vst3 host bring-up. It builds clean on Linux x86_64 with GCC 13 +
C++17 and links against the canonical `sdk_hosting` / `base` /
`pluginterfaces` library targets that the SDK exposes through
`smtg_create_*` functions in `cmake/modules/SMTG_VST3_SDK.cmake`.

We disable the SDK's example targets (`SMTG_ENABLE_VST3_PLUGIN_EXAMPLES`,
`SMTG_ENABLE_VST3_HOSTING_EXAMPLES`), VSTGUI support
(`SMTG_ENABLE_VSTGUI_SUPPORT`), and the validator / plugin-link helpers
(`SMTG_RUN_VST_VALIDATOR`, `SMTG_CREATE_PLUGIN_LINK`) before pulling the
SDK in via `add_subdirectory()` so the only artefacts produced are the
three static libraries we link.

## Submodules

The SDK pulls these nested submodules; `git submodule update --init
--recursive` after bumping the tag refreshes them:

- `base/`           — `3d2e82f8e6bff59c1d8b7a27491a29c2286b5206`
- `cmake/`          — `de6e54eeaaab35b7145f5c32c279b5e892146e04`
- `doc/`            — `6d4737c9e70750056e731d88d49aa06eefc8a1a4`
- `pluginterfaces/` — `31d6eeba6daaa3e2a8bfbe3e7a90ca0b7fbfbc1c`
- `public.sdk/`     — `a3911a4615dabbfdfd9d181ee26b05c70c289a95`
- `tutorials/`      — `33b73dfbb87f3fde3bce8c0a10cae934dc66ad34`
- `vstgui4/`        — `76823bdbe286e4bdb9f79ab8986af5ce7202336c`

Note: `vstgui4/` is fetched but not built; we set
`SMTG_ENABLE_VSTGUI_SUPPORT=OFF` to skip it.
