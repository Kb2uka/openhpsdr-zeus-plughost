# openhpsdr-zeus-plughost

Out-of-process VST3 / CLAP / VST2 host sidecar for the
[OpenHPSDR Zeus](https://github.com/brianbruff/openhpsdr-zeus) SDR client.
Zeus runs on .NET 8 and uses WDSP for radio DSP; this sidecar runs as a
separate process and lets operators splice general-purpose audio plugins
into the TX (and eventually RX) signal chain without ever loading
third-party native code into the Zeus process.

## Status

**Phase 1 skeleton — data-plane pass-through only, no plugin loading yet.**

This phase exists to validate the IPC plumbing in isolation:

- a SPSC lock-free shared-memory ring in each direction (planar float32),
- a control-channel stub for length-prefixed CBOR messages (Phase 2),
- a SIGKILL-tolerance gate (see `docs/PHASE1.md`).

No VST3 / CLAP / VST2 SDK is required to build Phase 1. The third-party
vendor trees land in `third_party/` as git submodules in Phase 2.

## Build (Linux)

```bash
cmake -S . -B build
cmake --build build -j
./build/zeus-plughost                          # prints usage, exits 1
./build/zeus-plughost --shm-name zeus-test \
                      --control-pipe /tmp/zeus-plughost.sock
```

CMake 3.20+ and a C++17 compiler are the only requirements. Warnings are
errors on all platforms (`-Wall -Wextra -Werror` on GCC/Clang, `/W4 /WX`
on MSVC).

Windows and macOS targets compile but the wakeup primitive is stubbed
(`ENOTIMPL`); only Linux is functionally complete in Phase 1.

## Dependency licenses (Phase 2 plan)

| Component        | Upstream                                           | License | GPLv2+ compat |
|------------------|----------------------------------------------------|---------|---------------|
| Steinberg vst3sdk | https://github.com/steinbergmedia/vst3sdk         | MIT     | yes           |
| CLAP             | https://github.com/free-audio/clap                 | MIT     | yes           |
| Vestige (VST2)   | vendored single-header                             | LGPL    | yes           |
| tinycbor (opt.)  | https://github.com/intel/tinycbor                  | MIT     | yes           |

This sidecar itself is licensed GPL v2-or-later to match the Zeus parent
project. License selection is provisional and subject to maintainer
review.

## Phase 1 details

See [`docs/PHASE1.md`](docs/PHASE1.md) for the smoke-test procedure, the
SIGKILL-tolerance gate, and the boundary between Phase 1 and Phase 2.

## Repository status

This repo is **local-only** until the Zeus maintainer (Brian Keating,
EI6LF) approves promotion to GitHub. There is no remote, no push, no CI.
