// wakeup_macos.cpp — POSIX named-semaphore Wakeup impl for macOS.
//
// macOS supports POSIX named semaphores (sem_open / sem_post / sem_wait)
// out of the libc dylib. macOS does NOT implement sem_timedwait, so we
// emulate it via sem_trywait + a short sleep loop — acceptable for Phase 2
// where the timeout is 50 ms.
//
// Phase 2 ships this alongside the Linux impl so the cross-platform shape
// is real. Phase 4 may swap to dispatch_semaphore_t for tighter realtime.

#include "ipc/wakeup.h"

#if defined(ZEUS_PLUGHOST_OS_MACOS)

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#include <fcntl.h>
#include <semaphore.h>
#include <unistd.h>

namespace zeus::plughost {

namespace {

std::string ErrnoMessage(const char* what) {
    char buf[256];
    if (strerror_r(errno, buf, sizeof(buf)) != 0) {
        std::snprintf(buf, sizeof(buf), "errno=%d", errno);
    }
    return std::string(what) + ": " + buf;
}

}  // namespace

Wakeup::Wakeup() {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/zeus-plughost-anon-%ld-%p",
                  static_cast<long>(::getpid()), static_cast<void*>(this));
    OpenInternal(buf, true);
    if (sem_ != nullptr && !name_.empty()) {
        ::sem_unlink(name_.c_str());
    }
}

Wakeup::Wakeup(const std::string& name, bool create) {
    OpenInternal(name, create);
}

void Wakeup::OpenInternal(const std::string& name, bool create) {
    name_ = name;
    sem_t* s;
    if (create) {
        s = ::sem_open(name.c_str(), O_CREAT, 0600, 0);
        ownsName_ = true;
    } else {
        s = ::sem_open(name.c_str(), 0);
        ownsName_ = false;
    }
    if (s == SEM_FAILED) {
        throw std::runtime_error(ErrnoMessage("sem_open"));
    }
    sem_ = static_cast<void*>(s);
}

Wakeup::~Wakeup() {
    if (sem_ != nullptr) {
        ::sem_close(static_cast<sem_t*>(sem_));
        sem_ = nullptr;
    }
    if (ownsName_ && !name_.empty()) {
        ::sem_unlink(name_.c_str());
    }
}

bool Wakeup::Wait(std::uint32_t timeoutMs) {
    if (sem_ == nullptr) return false;

    auto* s = static_cast<sem_t*>(sem_);

    if (timeoutMs == 0xFFFFFFFFu) {
        while (::sem_wait(s) != 0) {
            if (errno == EINTR) continue;
            return false;
        }
        return true;
    }

    // macOS lacks sem_timedwait. Emulate via sem_trywait + sleep loop.
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
    while (true) {
        if (::sem_trywait(s) == 0) return true;
        if (errno != EAGAIN && errno != EINTR) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void Wakeup::Signal() {
    if (sem_ == nullptr) return;
    ::sem_post(static_cast<sem_t*>(sem_));
}

}  // namespace zeus::plughost

#endif  // ZEUS_PLUGHOST_OS_MACOS
