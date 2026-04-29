// wakeup_macos.cpp — Phase 1 stub.
//
// Compiles on macOS so the rest of the host can link, but every call
// returns "not implemented". Phase 2 will wire this up to a
// dispatch_semaphore_t (signal/wait) or a mach semaphore for tighter
// realtime guarantees.

#include "ipc/wakeup.h"

#if defined(ZEUS_PLUGHOST_OS_MACOS)

namespace zeus::plughost {

Wakeup::Wakeup()  = default;
Wakeup::~Wakeup() = default;

bool Wakeup::Wait(std::uint32_t /*timeoutMs*/) {
    // ENOTIMPL — Phase 2 will use dispatch_semaphore_wait(sem_, ...).
    return false;
}

void Wakeup::Signal() {
    // ENOTIMPL — Phase 2 will dispatch_semaphore_signal(sem_).
}

}  // namespace zeus::plughost

#endif  // ZEUS_PLUGHOST_OS_MACOS
