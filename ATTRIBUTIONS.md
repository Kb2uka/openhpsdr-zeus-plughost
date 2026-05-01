# Attributions

`openhpsdr-zeus-plughost` is licensed **GPL v2 or later** to match the
[OpenHPSDR Zeus](https://github.com/brianbruff/openhpsdr-zeus) parent
project. The full license text is at [LICENSE](LICENSE).

This file documents the third-party components this sidecar redistributes
or links against, and their license terms.

## First-party

- **openhpsdr-zeus-plughost** — Copyright (C) 2026 Brian Keating (EI6LF),
  Douglas J. Cerrato (KB2UKA), and contributors. Licensed
  GPL-2.0-or-later. SPDX-License-Identifier headers in every source file.

## Third-party — vendored

| Component | Version | Source | License | License file |
|-----------|---------|--------|---------|--------------|
| free-audio/clap | 1.2.7 (commit 29ffcc2) | https://github.com/free-audio/clap | MIT | [`third_party/clap/LICENSE`](third_party/clap/LICENSE) |
| Vestige (VST 2.4 header) | clean-room single-header | this repo, derived from the public VST 2.4 specification | GPL-2.0-or-later | [LICENSE](LICENSE) (covers it) |

The Vestige `aeffectx.h` was reverse-engineered from public Steinberg
VST 2.4 API documentation and the host/plugin contract that Linux audio
plugins compile against. No Steinberg code is reproduced — the struct
layout, opcode values, and function signatures are matters of binary
compatibility, not copyrightable expression.

## Third-party — submodule (build-time dependency)

| Component | Source | License | Notes |
|-----------|--------|---------|-------|
| Steinberg vst3sdk | https://github.com/steinbergmedia/vst3sdk | proprietary, [free for commercial use](https://www.steinberg.net/developers/) | Pulled in by `cmake/deps.cmake` as a git submodule. Not redistributed in source releases. Distributors building from source must accept the Steinberg VST 3 SDK License when they `git submodule update --init`. |

## Linux runtime

- **X11 (libX11)** — runtime link dependency for the editor host. MIT-style
  X11 license, GPLv2-compatible.
- **glibc / pthreads** — system runtime. LGPL.

## Sample plugins used during development testing

These are not redistributed; they were used as the canonical "does it
work?" probe during operator-side rack-testing. Mentioned here as
appreciation rather than as a license claim.

- ZAM Audio plugins (DPF) — https://github.com/zamaudio
- LSP Plug-ins (LV2 / VST / CLAP) — https://lsp-plug.in/
- DISTRHO Plugin Framework — https://github.com/DISTRHO/DPF

## License compatibility matrix

| This redistributes | Under | Compatible with GPL-2.0-or-later? |
|--------------------|-------|-----------------------------------|
| Sidecar source     | GPL-2.0-or-later | yes (it IS the license) |
| CLAP headers       | MIT | yes — MIT is GPL-2-compatible |
| Vestige header     | GPL-2.0-or-later | yes |
| vst3sdk (submodule, not vendored) | Steinberg dual license | yes for source distribution; binary distribution requires accepting Steinberg's terms |

If you spot an attribution gap, please open an issue at
https://github.com/Kb2uka/openhpsdr-zeus-plughost/issues.
