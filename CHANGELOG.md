# Changelog

All notable changes to **zeus-plughost** are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [v0.1.0] — 2026-05-05

> **First standalone release.** zeus-plughost is the out-of-process audio-plugin
> host sidecar for [OpenHPSDR Zeus](https://github.com/brianbruff/openhpsdr-zeus).
> It lets ham operators splice general-purpose VST3, VST 2.4, and CLAP plugins —
> EQs, compressors, gates, multiband processors, reverbs — into the **TX mic
> chain** before Zeus's modulator. Plugins run in a separate process: a buggy
> plugin crashes the sidecar, *not* the radio.

### What this means for the operator

- **Drop-in plugins on the TX mic path.** Pick any VST3, VST 2.4, or CLAP plugin
  installed on your Linux box and put it in front of Zeus's CFC. Up to **8 plugins
  in series**, each individually bypassable, with a chain-wide master enable.
- **Native plugin editor windows.** When you click *Edit* on a slot, the plugin's
  own GUI opens as a real X11 window on the server's display. Knob drags update
  the plugin in real time without stuttering audio or freezing the editor.
- **Stereo plugins Just Work.** Zeus's mic chain is mono; stereo-only plugins
  (LSP Parametric EQ, ZAM stereo builds, MB Expander L/R, etc.) are auto-upmixed
  to L+R going in and averaged back to mono coming out, with no audio-thread
  allocation.
- **Audition mode (TX monitor).** Use Zeus's TX monitor to dial in a plugin's
  settings against your own voice off-air, before you key the radio.
- **A crashing plugin can't take the radio down.** The sidecar dies, Zeus restarts
  it, and your QSO continues on a clean chain.

### Platform support

| Platform              | Audio path | Plugin editors | Status                                                |
|-----------------------|:----------:|:--------------:|-------------------------------------------------------|
| **Linux x86_64**      |     ✅     |       ✅       | Operator-tested end-to-end. Pre-built tarball in [Releases](https://github.com/Kb2uka/openhpsdr-zeus-plughost/releases/tag/v0.1.0). |
| Windows x86_64        |     ⚠️     |       ❌       | Sidecar compiles and processes audio; `SlotShowEditor` returns `platform-not-supported` until the HWND attach path lands. |
| macOS arm64 / x86_64  |     ⚠️     |       ❌       | Sidecar compiles and processes audio; NSView attach path is the next platform wave. |

Operators on Windows or macOS can still drive plugin parameters headlessly via
the slot UI sliders in Zeus.

### Tested plugins (Linux x86_64)

End-to-end on real TX audio with native editors:

- **ZamGEQ31** — 31-band graphic EQ
- **ZamVerb** — convolution reverb
- **LSP Parametric EQ** — multi-band parametric (mono and stereo variants)
- **MB Expander** — multiband expander / gate

⚠️ **Known plugin issue:** *LSP Parametric Equalizer x16 mono* hangs in the
plugin's own teardown path on unload and is not safe to load in this release.
The framework-level fix (running sidecar unload on a detached thread) is queued
for the next cut. Other LSP variants are unaffected.

### Plugin format support

- **VST3** — via Steinberg vst3sdk v3.8.0 (MIT). Single-component and split-component
  plugins both supported, including `IConnectionPoint` wiring.
- **VST 2.4** — via Vestige headers (clean-room, no Steinberg VST2 SDK in the
  build). `effEditIdle` ticks the plugin's editor every GUI loop.
- **CLAP** — via the official CLAP headers v1.2.7 (MIT, vendored). Host extensions
  implemented: `clap.gui`, `clap.timer-support`, `clap.params`. Without
  `clap.timer-support`, DPF / pugl CLAP editors stay blank — that's now wired up.

### Under the hood (for the curious)

- **Out-of-process design.** zeus-plughost is a separate C++ binary launched by
  Zeus's `Zeus.PluginHost.PluginHostManager` and supervised across crashes.
- **Lock-free audio.** The audio thread runs a fixed 256-frame block at 48 kHz.
  Slot bypass, master enable, and active-plugin pointers are read with single
  atomic acquire-loads. No malloc, no syscalls, no locks beyond the wakeup
  semaphore. Two ping-pong scratch buffers chain up to 8 plugins without heap
  traffic.
- **Memory-ordered plugin swap.** Plugin loads on the control thread, audio thread
  reads via `std::atomic<ActivePlugin*>`, with a `seq_cst` fence on swap so
  weakly-ordered hosts (arm64) can't still be inside `Process()` on the old
  pointer when it's destroyed.
- **Native editor hosting.** Each editor is one X11 top-level window per slot,
  driven by a dedicated GUI thread. `IRunLoop` lets plugins register fd handlers
  and timers; `IPlugFrame` handles plugin-driven `resizeView`. `WM_DELETE_WINDOW`
  closes the editor cleanly. CLAP teardown is reordered into
  `IPlugView::removed()` so the plugin releases its X resources before our parent
  window is destroyed (this fixes the `BadWindow` / `GLXBadDrawable` Xlib errors
  that were taking the sidecar down on plugin close).
- **Lossy parameter forwarding.** Slider drags fire `audioMasterAutomate` (VST2),
  `performEdit` (VST3), or output events (CLAP) at high rates. We forward via
  `SIOCOUTQ`-probed non-blocking sends so the plugin's GUI thread never stalls
  on a full socket buffer (the previous blocking send was freezing DPF VST2
  editors after a few seconds of motion).
- **IPC plumbing.** POSIX `shm_open` + `mmap` SPSC ring for audio (160-byte
  control block, 8-block depth, planar float32). AF_UNIX `SOCK_STREAM` control
  pipe with length-prefixed framing. Per-platform wakeup primitive (futex on
  Linux, named POSIX semaphore on macOS).

### Build & distribution

- **CMake 3.25+, C++17**, warnings-as-errors (`-Wall -Wextra -Werror` on
  GCC/Clang, `/W4 /WX /permissive-` on MSVC). SDK headers are quarantined behind
  `pragma diagnostic push/pop` so warnings stay engaged on first-party code.
- **Submodule:** `third_party/vst3sdk` (Steinberg vst3sdk @ v3.8.0).
- **Vendored:** `third_party/clap` (v1.2.7) and `third_party/vestige` (one
  clean-room VST 2.4 header). Vendored — not submodules — so a plain
  `git clone` without `--recurse-submodules` is buildable.
- **CI release workflow.** Pushing a `v*` tag triggers a `ubuntu-22.04` build,
  strips the binary, packages it with `LICENSE`, `ATTRIBUTIONS.md`, and
  `INSTALL.txt`, and attaches a tarball + SHA-256 to a GitHub Release.

### Legal

- License: **GPL-2.0-or-later** (matches the upstream Zeus project). Full GPL v2
  text in [`LICENSE`](LICENSE).
- Per-file SPDX-License-Identifier headers on every first-party source file.
- Third-party license inventory + GPL-2.0 compatibility matrix in
  [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md).
- No Steinberg VST2 SDK in the build tree (license-incompatible) — VST 2.4
  support uses the clean-room Vestige header instead.

### Pre-1.0 caveat

The control-pipe wire format may still drift between Zeus and the sidecar before
1.0. **Pin a matched pair of versions if you upgrade either side.** The wire
spec is tracked in `docs/proposals/vst-host-phase2-wire.md` in the parent Zeus
repo.

### Commit map

| Phase                            | Commit    | What landed |
|----------------------------------|-----------|-------------|
| Bootstrap (Phase 1)              | `d91836b` | CMake scaffold, IPC skeleton, pass-through audio loop, license preamble |
| Test hook                        | `001641f` | `--idle` flag for the .NET supervisor's SIGKILL-recovery test           |
| Phase 2 entry                    | `fe40643` | Real `shm_open` + named semaphores + AF_UNIX control pipe + audio loop  |
| VST3 plugin host                 | `db2201f` | vst3sdk submodule, single-slot plugin loading, `IComponent` + `IAudioProcessor` wiring |
| 8-slot chain                     | `c615d5d` | Master enable, per-slot bypass, parameter introspection, ping-pong scratch buffers |
| Native editor windows (Phase 3 GUI) | `6063b75` | X11 + `IRunLoop` + `IPlugFrame` + `EditorWindow` + dedicated GUI thread |
| VST2 + CLAP                      | `21641b5` | Vestige + CLAP plugin hosts, editor wrappers, stereo upmix, lossy `ParamChanged` |
| Vendoring + clean-room headers   | `1f9a1d0` | CLAP vendored (MIT, ~200 KB), Vestige vendored (clean-room VST 2.4 header) |
| Legal + docs polish              | `e673b1d` | Full GPL v2 text, SPDX headers across 34 source files, `ATTRIBUTIONS.md`, README rewrite |
| CI release pipeline              | `4d20b90` | Tag-triggered Linux x86_64 build → stripped binary → tarball + SHA-256 → GitHub Release |

---

[v0.1.0]: https://github.com/Kb2uka/openhpsdr-zeus-plughost/releases/tag/v0.1.0
