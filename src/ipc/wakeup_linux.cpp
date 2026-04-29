// wakeup_linux.cpp — futex-backed Wakeup impl.

#include "ipc/wakeup.h"

#if defined(ZEUS_PLUGHOST_OS_LINUX)

#include <atomic>
#include <cerrno>
#include <ctime>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

namespace zeus::plughost {

namespace {

int FutexWait(int* addr, int expected, std::uint32_t timeoutMs) {
    timespec ts{};
    timespec* tsPtr = nullptr;
    if (timeoutMs != 0xFFFFFFFFu) {
        ts.tv_sec  = static_cast<time_t>(timeoutMs / 1000);
        ts.tv_nsec = static_cast<long>((timeoutMs % 1000) * 1'000'000);
        tsPtr = &ts;
    }
    return static_cast<int>(::syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE,
                                      expected, tsPtr, nullptr, 0));
}

int FutexWake(int* addr, int howMany) {
    return static_cast<int>(::syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE,
                                      howMany, nullptr, nullptr, 0));
}

}  // namespace

Wakeup::Wakeup() : state_(0) {}
Wakeup::~Wakeup() = default;

bool Wakeup::Wait(std::uint32_t timeoutMs) {
    // Spin once on the cheap path: if state_ is already non-zero, consume
    // it without entering the kernel.
    auto* atomic_state = reinterpret_cast<std::atomic<int>*>(&state_);
    int observed = atomic_state->exchange(0, std::memory_order_acquire);
    if (observed != 0) {
        return true;
    }

    int rc = FutexWait(&state_, 0, timeoutMs);
    if (rc == 0) {
        atomic_state->store(0, std::memory_order_relaxed);
        return true;
    }
    // EAGAIN means state_ changed under us before we slept — that counts
    // as a wake. ETIMEDOUT / EINTR are real timeouts / spurious wakes.
    if (errno == EAGAIN) {
        return true;
    }
    return false;
}

void Wakeup::Signal() {
    auto* atomic_state = reinterpret_cast<std::atomic<int>*>(&state_);
    atomic_state->store(1, std::memory_order_release);
    FutexWake(&state_, 1);
}

}  // namespace zeus::plughost

#endif  // ZEUS_PLUGHOST_OS_LINUX
