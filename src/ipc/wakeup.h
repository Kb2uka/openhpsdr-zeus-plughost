// wakeup.h — cross-platform wakeup primitive for the data-plane consumer.
//
// The audio thread blocks on Wait() when the input ring is empty and the
// producer (Zeus) calls Signal() after publishing a block. This is the ONE
// syscall the audio thread is allowed to make per block; all other work
// must be lock-free / syscall-free.
//
// Per-platform impls:
//
//   - Linux : futex (linux/futex.h, syscall(SYS_futex, ...))
//   - Win32 : CreateEventA + SetEvent / WaitForSingleObject
//   - macOS : os_unfair_lock + dispatch_semaphore_t (or mach semaphore)
//
// Phase 1 ships a working Linux impl; Win/Mac return ENOTIMPL but compile.

#pragma once

#include <cstdint>

namespace zeus::plughost {

class Wakeup {
public:
    Wakeup();
    ~Wakeup();

    Wakeup(const Wakeup&)            = delete;
    Wakeup& operator=(const Wakeup&) = delete;

    // Block until Signal() is called or `timeoutMs` elapses.
    // Returns true on wake, false on timeout / error.
    bool Wait(std::uint32_t timeoutMs);

    // Wake any thread waiting in Wait(). Safe to call from a non-realtime
    // producer thread (Zeus side); the audio consumer side calls Wait().
    void Signal();

private:
#if defined(ZEUS_PLUGHOST_OS_LINUX)
    // Plain int futex word. Atomicity is enforced via std::atomic_ref-style
    // syscall semantics (FUTEX_WAIT compares the word, FUTEX_WAKE is a
    // pure wake). Kept as a raw int so we can pass &state_ to the syscall.
    int state_ = 0;
#elif defined(ZEUS_PLUGHOST_OS_WINDOWS)
    void* event_ = nullptr;  // HANDLE; void* avoids dragging windows.h here
#elif defined(ZEUS_PLUGHOST_OS_MACOS)
    void* sem_ = nullptr;    // dispatch_semaphore_t opaque
#endif
};

}  // namespace zeus::plughost
