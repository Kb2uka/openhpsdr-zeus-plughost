# openhpsdr-zeus-plughost

Out-of-process VST3 / VST 2.4 / CLAP host sidecar for the
[OpenHPSDR Zeus](https://github.com/brianbruff/openhpsdr-zeus) SDR client.

Zeus runs on .NET 10 and uses WDSP for radio DSP. This sidecar runs as a
separate process, lets operators splice general-purpose audio plugins into
the TX (and eventually RX) signal chain, and never loads third-party native
plugin code into the Zeus process — a crash in a buggy VST kills the
sidecar instead of the radio.

## Status

**Linux x86_64 — operator-tested end-to-end.** VST3, Linux VST 2.4 (Vestige),
and CLAP plugins all load + process audio + open native editor windows
through the X11 attach path. Stereo plugins are upmixed/downmixed against
Zeus's mono mic chain.

**Windows + macOS — sidecar compiles**, plugins load + process audio, but
`SlotShowEditor` returns `platform-not-supported`. The HWND + NSView GUI
hosts are the next work item. Operators on those platforms can drive
plugins headlessly via the Zeus slot UI sliders today.

Tracked under https://github.com/brianbruff/openhpsdr-zeus/issues/106.

## Build (Linux)

```bash
git clone --recurse-submodules https://github.com/Kb2uka/openhpsdr-zeus-plughost.git
cd openhpsdr-zeus-plughost
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

That produces `build/zeus-plughost`. Zeus's .NET host (`Zeus.PluginHost`
project, see `Native/SidecarLocator.cs`) finds it via, in order:

1. `ZEUS_PLUGHOST_BIN` env var (absolute path or PATH-resolvable name)
2. Sibling-checkout walk: `<...>/openhpsdr-zeus-plughost/build/zeus-plughost`
3. `zeus-plughost` on `PATH`

For development with both repos checked out side-by-side, the sibling
walk Just Works.

## Dependencies

CMake 3.25+ and a C++17 compiler. Linux additionally needs X11 (`libx11-dev`).

| Component           | Source                                              | How             | License       |
|---------------------|-----------------------------------------------------|-----------------|---------------|
| Steinberg vst3sdk   | https://github.com/steinbergmedia/vst3sdk          | git submodule   | proprietary, free for commercial use |
| free-audio/clap     | https://github.com/free-audio/clap (v1.2.7)        | vendored copy   | MIT           |
| Vestige VST2 header | clean-room single header                            | vendored        | GPL v2+       |

This sidecar is GPL v2+ to match Zeus.

## Distribution / packaging

This repo intentionally has **no release binaries** today. Two viable
deployment paths:

1. **Vendor as submodule** under `native/zeus-plughost/` in the main
   `openhpsdr-zeus` repo. The Zeus build system invokes CMake to compile
   it alongside `wdsp`. Installer bundles the binary per-arch.
2. **Per-arch release binaries** here, downloaded by the .NET host on
   first use (or by the installer at package time).

Maintainer (EI6LF) decision pending. See issue
https://github.com/brianbruff/openhpsdr-zeus/issues/106 for the latest.

## Architecture

- `src/main.cpp` — sidecar entry, control-pipe protocol, audio loop.
- `src/ipc/` — AF_UNIX control pipe + shared-memory audio rings.
- `src/vst3/` — Steinberg VST3 plugin host + native editor window
  infrastructure (X11 + IRunLoop + IPlugFrame).
- `src/vst2/` — Linux VST 2.4 plugin host + IPlugView wrapper for editors.
- `src/clap/` — CLAP plugin host + IPlugView wrapper for editors.

The `IPlugView` wrappers for VST2 + CLAP let one editor host
(`vst3/editor_window.cpp`) host all three formats, with an `IEditorIdlePump`
hook for the formats that need periodic main-thread time.

## Testing

The .NET-side test suite in `tests/Zeus.PluginHost.Tests` (in the main Zeus
repo) drives this binary. Set `ZEUS_PLUGHOST_BIN=/path/to/zeus-plughost`
before running `dotnet test`. Several tests are gated on a real plugin
binary being available — see `tests/Zeus.PluginHost.Tests/EditorWindowTests.cs`
for the env vars they look for.
