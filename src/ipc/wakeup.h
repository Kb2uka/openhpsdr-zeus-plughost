// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// wakeup.h — cross-platform wakeup primitive for the data-plane consumer.
//
// The audio thread blocks on Wait() when the input ring is empty and the
// producer (Zeus) calls Signal() after publishing a block. This is the ONE
// syscall the audio thread is allowed to make per block; all other work
// must be lock-free / syscall-free.
//
// Per-platform impls:
//
//   - Linux : POSIX named semaphores (sem_open, sem_post, sem_timedwait).
//   - macOS : same POSIX surface; libc.dylib.
//   - Win32 : Phase 3 — named events.
//
// Phase 2 ships Linux + macOS via POSIX named semaphores so the host and
// sidecar can share the wakeup edge across processes by name. The default
// constructor builds an unnamed/local Wakeup retained for unit tests; the
// cross-process constructor takes a leading-slash name.

#pragma once

#include <cstdint>
#include <string>

namespace zeus::plughost {

class Wakeup {
public:
    // Default constructor — unnamed, process-local. Phase 2 uses this for
    // backward-compatible tests that don't need cross-process visibility.
    // On platforms where named semaphores are the ONLY supported impl
    // (Linux/macOS), this falls back to a per-instance unnamed name
    // generated from the address; callers that want cross-process visibility
    // MUST use the named constructor below.
    Wakeup();

    // Open or create a named POSIX semaphore. `name` must start with '/'.
    // If `create` is true, the semaphore is created with O_CREAT. On
    // Linux/macOS this maps to sem_open(name, O_CREAT, 0600, 0).
    Wakeup(const std::string& name, bool create);

    ~Wakeup();

    Wakeup(const Wakeup&)            = delete;
    Wakeup& operator=(const Wakeup&) = delete;

    // Block until Signal() is called or `timeoutMs` elapses.
    // Returns true on wake, false on timeout / error.
    bool Wait(std::uint32_t timeoutMs);

    // Wake any thread waiting in Wait(). Safe to call from a non-realtime
    // producer thread (Zeus side); the audio consumer side calls Wait().
    void Signal();

    // True if this Wakeup owns the named primitive (i.e. created it with
    // O_CREAT). Sidecar wakeups created via Wakeup(name, false) return
    // false; they MUST NOT sem_unlink.
    bool OwnsName() const { return ownsName_; }

    const std::string& Name() const { return name_; }

private:
    void OpenInternal(const std::string& name, bool create);

    std::string name_;
    void*       sem_ = nullptr;     // sem_t* on POSIX, HANDLE on Windows
    bool        ownsName_ = false;
};

}  // namespace zeus::plughost