// wakeup_linux.cpp — POSIX named-semaphore Wakeup impl (Phase 2).
//
// On Linux, glibc's sem_open lives in librt and stores named semaphores
// at /dev/shm/sem.<name>. The host creates the semaphore (O_CREAT, initial
// 0); the sidecar opens by name without O_CREAT. Both call sem_post /
// sem_timedwait. Only the host calls sem_unlink — same ownership model as
// the shm rings.

#include "ipc/wakeup.h"

#if defined(ZEUS_PLUGHOST_OS_LINUX)

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <semaphore.h>
#include <unistd.h>

namespace zeus::plughost {

namespace {

std::string ErrnoMessage(const char* what) {
    char buf[256];
    char* msg = strerror_r(errno, buf, sizeof(buf));
    return std::string(what) + ": " + (msg ? msg : "(unknown)");
}

}  // namespace

Wakeup::Wakeup() {
    // Phase 2 default-constructed Wakeup uses an anonymous local
    // semaphore by allocating one via sem_open with a unique name,
    // then unlinking immediately so it has process-local lifetime.
    // This keeps the realtime call path identical between named and
    // unnamed cases — the audio thread always uses sem_timedwait.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/zeus-plughost-anon-%ld-%p",
                  static_cast<long>(::getpid()), static_cast<void*>(this));
    OpenInternal(buf, true);
    if (sem_ != nullptr && !name_.empty()) {
        ::sem_unlink(name_.c_str());
        // Keep ownsName_=true so dtor closes; the unlink already happened.
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

    if (timeoutMs == 0xFFFFFFFFu) {
        while (::sem_wait(static_cast<sem_t*>(sem_)) != 0) {
            if (errno == EINTR) continue;
            return false;
        }
        return true;
    }

    timespec ts{};
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return false;
    }
    ts.tv_sec  += static_cast<time_t>(timeoutMs / 1000);
    long add_ns = static_cast<long>((timeoutMs % 1000) * 1'000'000L);
    ts.tv_nsec += add_ns;
    if (ts.tv_nsec >= 1'000'000'000L) {
        ts.tv_sec  += ts.tv_nsec / 1'000'000'000L;
        ts.tv_nsec %= 1'000'000'000L;
    }

    while (::sem_timedwait(static_cast<sem_t*>(sem_), &ts) != 0) {
        if (errno == EINTR) continue;
        return false;  // ETIMEDOUT or other — caller treats as timeout
    }
    return true;
}

void Wakeup::Signal() {
    if (sem_ == nullptr) return;
    ::sem_post(static_cast<sem_t*>(sem_));
}

}  // namespace zeus::plughost

#endif  // ZEUS_PLUGHOST_OS_LINUX
