// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// wakeup_windows.cpp — Phase 2 stub.
//
// Windows is intentionally not supported in Phase 2; the spec
// (docs/proposals/vst-host-phase2-wire.md, Section 8) defers it to
// Phase 3. This stub exists so the codebase still compiles when
// configured for Windows, but every entry point throws.

#include "ipc/wakeup.h"

#if defined(ZEUS_PLUGHOST_OS_WINDOWS)

#include <stdexcept>

namespace zeus::plughost {

Wakeup::Wakeup() {
    throw std::runtime_error(
        "zeus-plughost: Wakeup is not implemented on Windows in Phase 2; "
        "see docs/proposals/vst-host-phase2-wire.md");
}

Wakeup::Wakeup(const std::string& /*name*/, bool /*create*/) {
    throw std::runtime_error(
        "zeus-plughost: Wakeup is not implemented on Windows in Phase 2; "
        "see docs/proposals/vst-host-phase2-wire.md");
}

void Wakeup::OpenInternal(const std::string& /*name*/, bool /*create*/) {
    // Unreachable; ctor throws above. Defined to satisfy the linker.
}

Wakeup::~Wakeup() = default;

bool Wakeup::Wait(std::uint32_t /*timeoutMs*/) {
    return false;
}

void Wakeup::Signal() {
    // No-op.
}

}  // namespace zeus::plughost

#endif  // ZEUS_PLUGHOST_OS_WINDOWS