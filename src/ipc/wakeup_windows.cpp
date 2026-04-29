// wakeup_windows.cpp — Phase 1 stub.
//
// Compiles on Windows so the rest of the host can link, but every call
// returns "not implemented". Phase 2 will wire this up to CreateEventA +
// SetEvent / WaitForSingleObject.

#include "ipc/wakeup.h"

#if defined(ZEUS_PLUGHOST_OS_WINDOWS)

namespace zeus::plughost {

Wakeup::Wakeup()  = default;
Wakeup::~Wakeup() = default;

bool Wakeup::Wait(std::uint32_t /*timeoutMs*/) {
    // ENOTIMPL — Phase 2 will use WaitForSingleObject(event_, timeoutMs).
    return false;
}

void Wakeup::Signal() {
    // ENOTIMPL — Phase 2 will SetEvent(event_).
}

}  // namespace zeus::plughost

#endif  // ZEUS_PLUGHOST_OS_WINDOWS
